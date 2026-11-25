#include "ota.h"
#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mutex>

#include "version.h"
#include "mining.h"
#include "utils.h"
#include "wManager.h"
#include "drivers/storage/storage.h"
#include "webOtaPage.h"
#include "webConfig.h"

#ifndef PIOENV_NAME
#if defined(ESP32_2432S028R)
#define PIOENV_NAME "ESP32-2432S028R"
#elif defined(ESP32_2432S028_2USB)
#define PIOENV_NAME "ESP32_2432S028_2USB"
#elif defined(NERDMINERV2)
#define PIOENV_NAME "NerdminerV2"
#elif defined(LILYGO_T3_V1)
#define PIOENV_NAME "LilygoT3V1"
#elif defined(ES3C35P)
#define PIOENV_NAME "ES3C35P"
#elif defined(ESP32_4IN_ST7796)
#define PIOENV_NAME "ESP32-4in-ST7796"
#else
#define PIOENV_NAME "ESP32"
#endif
#endif

static const char* GITHUB_PRIMARY_REPO = "aetherwraith/NerdMiner-Modded";
static const char* GITHUB_FALLBACK_REPO = "HenrysCat/NerdMiner-Modded";

static std::mutex s_otaMutex;
static OtaStatus s_otaStatus;
static TaskHandle_t s_otaTaskHandle = nullptr;
static TaskHandle_t s_checkTaskHandle = nullptr;
static String s_targetOtaUrl = "";
static volatile bool s_otaRebootScheduled = false;
static unsigned long s_otaRebootTime = 0;

String getBoardModel(void) {
  return String(PIOENV_NAME);
}

bool hasDualOtaSupport(void) {
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  return (update_partition != NULL);
}

static String getRunningPartitionName(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running ? String(running->label) : "unknown";
}

static String getNextPartitionName(void) {
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  return next ? String(next->label) : "none";
}

OtaStatus getOtaStatus(void) {
  std::lock_guard<std::mutex> lock(s_otaMutex);
  s_otaStatus.runningPartition = getRunningPartitionName();
  s_otaStatus.nextPartition = getNextPartitionName();
  s_otaStatus.hasDualOta = hasDualOtaSupport();
  s_otaStatus.boardModel = getBoardModel();
  s_otaStatus.currentVersion = String(CURRENT_VERSION);
  return s_otaStatus;
}

static void setOtaError(const String &err) {
  std::lock_guard<std::mutex> lock(s_otaMutex);
  s_otaStatus.state = OTA_STATE_ERROR;
  s_otaStatus.errorMessage = err;
}

static bool validateFirmwareHeader(const uint8_t *data, size_t len, String &errOut) {
  if (len < 16) {
    errOut = "Header too short to validate";
    return false;
  }
  if (data[0] != 0xE9) {
    errOut = "Invalid magic byte (expected 0xE9, got 0x" + String(data[0], HEX) + ")";
    return false;
  }

  uint8_t chipId = data[12];
#if defined(CONFIG_IDF_TARGET_ESP32)
  if (chipId != 0x00 && chipId != 0xFF) {
    errOut = "Binary chip mismatch: binary is not for ESP32 classic (chip ID: " + String(chipId) + ")";
    return false;
  }
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  if (chipId != 0x09) {
    errOut = "Binary chip mismatch: binary is not for ESP32-S3 (chip ID: " + String(chipId) + ")";
    return false;
  }
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  if (chipId != 0x05) {
    errOut = "Binary chip mismatch: binary is not for ESP32-C3 (chip ID: " + String(chipId) + ")";
    return false;
  }
#endif
  return true;
}

static void otaWorkerTask(void *param) {
  String url;
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    url = s_targetOtaUrl;
    s_otaStatus.state = OTA_STATE_DOWNLOADING;
    s_otaStatus.bytesWritten = 0;
    s_otaStatus.totalBytes = 0;
    s_otaStatus.progressPercent = 0;
    s_otaStatus.errorMessage = "";
  }
  Serial.printf("[OTA] Starting background download from: %s\n", url.c_str());

  if (!hasDualOtaSupport()) {
    String msg = "Dual-OTA partitions not configured on this device. USB flash required once.";
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    s_otaTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  // Pause mining tasks and serialize HW SHA across HTTPS TLS handshakes (including GitHub 302 redirects)
  g_otaUpdating = true;
  pauseHwMinerForTls();

  const bool isHttps = !url.startsWith("http://") && !url.startsWith("HTTP://");
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // Skip full root CA bundle to preserve heap

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setUserAgent("NerdMiner-Modded-OTA/" CURRENT_VERSION);

  const bool began = isHttps ? http.begin(secureClient, url) : http.begin(plainClient, url);
  if (!began) {
    String msg = String("Failed to initialize ") + (isHttps ? "HTTPS" : "HTTP") + " connection";
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    g_otaUpdating = false;
    s_otaTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  int httpCode = 0;
  if (isHttps) {
    HW_SHA_REQUEST();
    TlsPauseGuard tlsGuard;
    httpCode = http.GET();
  } else {
    httpCode = http.GET();
  }
  if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_FOUND && httpCode != HTTP_CODE_MOVED_PERMANENTLY) {
    String msg = "HTTP request failed with code " + String(httpCode);
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  int contentLength = http.getSize();
  size_t totalBytes = (contentLength > 0) ? (size_t)contentLength : 0;
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    s_otaStatus.totalBytes = totalBytes;
  }
  Serial.printf("[OTA] Connected. Content length: %d bytes\n", contentLength);

  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    setOtaError("Failed to obtain HTTP stream");
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  // Read first chunk to validate image header before Update.begin
  uint8_t buffer[1024];
  size_t firstChunkSize = 0;
  unsigned long timeout = millis() + 10000;
  while (firstChunkSize < 64 && millis() < timeout) {
    if (stream->available()) {
      size_t readLen = stream->readBytes(buffer + firstChunkSize, 64 - firstChunkSize);
      firstChunkSize += readLen;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  String validateErr;
  if (!validateFirmwareHeader(buffer, firstChunkSize, validateErr)) {
    setOtaError(validateErr);
    Serial.println("[OTA] Validation failed: " + validateErr);
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    s_otaStatus.state = OTA_STATE_FLASHING;
  }
  size_t updateSize = (contentLength > 0) ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(updateSize, U_FLASH)) {
    String msg = "Update.begin failed: " + String(Update.errorString());
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  // Write the validated first chunk
  Update.write(buffer, firstChunkSize);
  size_t writtenTotal = firstChunkSize;
  int lastReportedPercent = (totalBytes > 0) ? (int)((writtenTotal * 100ULL) / totalBytes) : 0;
  size_t lastReportedBytes = writtenTotal;
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    s_otaStatus.bytesWritten = writtenTotal;
    if (totalBytes > 0) {
      s_otaStatus.progressPercent = lastReportedPercent;
    }
  }

  bool writeFailed = false;
  unsigned long lastDataMs = millis();
  // Stream remaining bytes while g_otaUpdating stays true
  while ((http.connected() || stream->available() > 0) && (writtenTotal < totalBytes || totalBytes == 0)) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t toRead = (avail > sizeof(buffer)) ? sizeof(buffer) : avail;
      size_t bytesRead = stream->readBytes(buffer, toRead);
      if (bytesRead > 0) {
        lastDataMs = millis();
        size_t written = Update.write(buffer, bytesRead);
        if (written != bytesRead) {
          setOtaError("Write error: " + String(Update.errorString()));
          Update.abort();
          writeFailed = true;
          break;
        }
        writtenTotal += written;
        int newPct = (totalBytes > 0) ? (int)((writtenTotal * 100ULL) / totalBytes) : 0;
        if ((totalBytes > 0 && newPct != lastReportedPercent) ||
            (writtenTotal - lastReportedBytes >= 16384)) {
          std::lock_guard<std::mutex> lock(s_otaMutex);
          s_otaStatus.bytesWritten = writtenTotal;
          if (totalBytes > 0) {
            s_otaStatus.progressPercent = newPct;
          }
          lastReportedPercent = newPct;
          lastReportedBytes = writtenTotal;
        }
      }
    } else {
      if ((unsigned long)(millis() - lastDataMs) > 15000UL) {
        setOtaError("Download stalled (socket timeout)");
        Update.abort();
        writeFailed = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  // Drain any trailing buffered bytes before closing the socket cleanly
  if (!writeFailed) {
    size_t drainLimit = 4096;
    while (stream->available() > 0 && drainLimit > 0) {
      size_t toRead = stream->available();
      if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
      if (toRead > drainLimit) toRead = drainLimit;
      size_t n = stream->readBytes(buffer, toRead);
      if (n == 0) break;
      drainLimit -= n;
    }
  }

  http.end();

  if (!writeFailed && (writtenTotal == 0 || (totalBytes > 0 && writtenTotal < totalBytes))) {
    String msg = (writtenTotal == 0)
        ? "Empty firmware download (0 bytes)"
        : "Truncated download (" + String((unsigned)writtenTotal) + "/" + String((unsigned)totalBytes) + " bytes)";
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    Update.abort();
    writeFailed = true;
  }

  if (!writeFailed) {
    if (Update.end(true)) {
      {
        std::lock_guard<std::mutex> lock(s_otaMutex);
        s_otaStatus.bytesWritten = writtenTotal;
        s_otaStatus.state = OTA_STATE_SUCCESS;
        s_otaStatus.progressPercent = 100;
      }
      Serial.printf("[OTA] Update Success! Wrote %u bytes. Rebooting...\n", (unsigned)writtenTotal);
      s_otaRebootTime = millis() + 2000;
      s_otaRebootScheduled = true;
    } else {
      String msg = "Update.end failed: " + String(Update.errorString());
      setOtaError(msg);
      Serial.println("[OTA] Error: " + msg);
      g_otaUpdating = false;
    }
  } else {
    g_otaUpdating = false;
  }

  s_otaTaskHandle = nullptr;
  vTaskDelete(NULL);
}

bool startUrlOta(const String &url) {
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    if (s_otaStatus.state != OTA_STATE_IDLE &&
        s_otaStatus.state != OTA_STATE_ERROR &&
        s_otaStatus.state != OTA_STATE_SUCCESS) {
      return false;
    }
    s_targetOtaUrl = url;
  }
  BaseType_t res = xTaskCreatePinnedToCore(otaWorkerTask, "OtaWorker", 8192, NULL, 3, &s_otaTaskHandle, 0);
  return (res == pdPASS);
}

static bool fetchGitHubRelease(const char* repo, DynamicJsonDocument &doc) {
  g_otaUpdating = true;
  pauseHwMinerForTls();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(12000);
  http.setUserAgent("NerdMiner-Modded-OTA/" CURRENT_VERSION);

  String url = "https://api.github.com/repos/" + String(repo) + "/releases/latest";
  Serial.printf("[OTA] Querying %s ... (free heap: %u)\n", url.c_str(), ESP.getFreeHeap());
  if (!http.begin(client, url)) {
    Serial.println(F("[OTA] http.begin failed"));
    return false;
  }

  http.useHTTP10(true);
  int httpCode = 0;
  {
    HW_SHA_REQUEST();
    TlsPauseGuard tlsGuard;
    httpCode = http.GET();
  }
  Serial.printf("[OTA] HTTP GET code: %d\n", httpCode);
  if (httpCode != HTTP_CODE_OK) {
    if (httpCode < 0) {
      Serial.printf("[OTA] HTTP error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
    return false;
  }

  // Filter keys to minimize JSON memory usage and stream directly from socket
  StaticJsonDocument<256> filter;
  filter["tag_name"] = true;
  filter["name"] = true;
  filter["html_url"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;

  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  Serial.printf("[OTA] JSON parse result: %s\n", err.c_str());
  return (err == DeserializationError::Ok);
}

static bool runGitHubUpdateCheck(void) {
  bool wasMining = !g_otaUpdating;
  g_otaUpdating = true;
  pauseHwMinerForTls();

  DynamicJsonDocument doc(6144);
  bool ok = fetchGitHubRelease(GITHUB_PRIMARY_REPO, doc);
  if (!ok) {
    doc.clear();
    ok = fetchGitHubRelease(GITHUB_FALLBACK_REPO, doc);
  }

  if (!ok || !doc.containsKey("tag_name")) {
    {
      std::lock_guard<std::mutex> lock(s_otaMutex);
      s_otaStatus.state = OTA_STATE_ERROR;
      s_otaStatus.errorMessage = "Failed to query GitHub releases API";
    }
    if (wasMining) g_otaUpdating = false;
    return false;
  }

  String latestTag = doc["tag_name"].as<String>();
  String currentVer = String(CURRENT_VERSION);
  String releaseUrl = doc.containsKey("html_url") ? doc["html_url"].as<String>() : "";

  String tagClean = latestTag;
  if (tagClean.startsWith("v") || tagClean.startsWith("V")) tagClean = tagClean.substring(1);
  String curClean = currentVer;
  if (curClean.startsWith("v") || curClean.startsWith("V")) curClean = curClean.substring(1);
  bool hasUpdate = (!tagClean.equalsIgnoreCase(curClean));

  // Find matching binary asset for this board: {PIOENV_NAME}_firmware.bin
  String targetAsset = getBoardModel() + "_firmware.bin";
  String downloadUrl = "";

  if (doc.containsKey("assets") && doc["assets"].is<JsonArray>()) {
    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
      String name = asset["name"].as<String>();
      if (name.equalsIgnoreCase(targetAsset)) {
        downloadUrl = asset["browser_download_url"].as<String>();
        break;
      }
    }
  }

  bool resumeMining = false;
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    s_otaStatus.latestVersion = latestTag;
    s_otaStatus.releaseUrl = releaseUrl;
    s_otaStatus.releaseNotes = "";
    s_otaStatus.hasUpdate = hasUpdate;
    s_otaStatus.downloadUrl = downloadUrl;
    if (s_otaStatus.state == OTA_STATE_CHECKING) {
      s_otaStatus.state = OTA_STATE_IDLE;
    }
    resumeMining = wasMining && s_otaStatus.state != OTA_STATE_DOWNLOADING && s_otaStatus.state != OTA_STATE_FLASHING;
  }
  if (resumeMining) {
    g_otaUpdating = false;
  }
  return true;
}

bool checkGitHubUpdate(void) {
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    if (s_otaStatus.state == OTA_STATE_CHECKING ||
        s_otaStatus.state == OTA_STATE_DOWNLOADING ||
        s_otaStatus.state == OTA_STATE_FLASHING) {
      return false;
    }
    s_otaStatus.state = OTA_STATE_CHECKING;
    s_otaStatus.errorMessage = "";
  }
  return runGitHubUpdateCheck();
}

static void checkUpdateWorkerTask(void *param) {
  (void)param;
  runGitHubUpdateCheck();
  s_checkTaskHandle = nullptr;
  vTaskDelete(NULL);
}

bool startGitHubCheck(void) {
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    if (s_otaStatus.state == OTA_STATE_CHECKING) {
      return true;
    }
    if (s_otaStatus.state == OTA_STATE_DOWNLOADING ||
        s_otaStatus.state == OTA_STATE_FLASHING ||
        s_checkTaskHandle != nullptr) {
      return false;
    }
    s_otaStatus.state = OTA_STATE_CHECKING;
    s_otaStatus.errorMessage = "";
  }
  BaseType_t res = xTaskCreatePinnedToCore(checkUpdateWorkerTask, "OtaCheck", 8192, NULL, 2, &s_checkTaskHandle, 0);
  if (res != pdPASS) {
    setOtaError("Failed to start update check task");
    return false;
  }
  return true;
}

bool startGitHubOta(const String &targetVersion) {
  (void)targetVersion;
  String dlUrl;
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    dlUrl = s_otaStatus.downloadUrl;
  }
  if (dlUrl.length() == 0) {
    if (!checkGitHubUpdate()) return false;
    std::lock_guard<std::mutex> lock(s_otaMutex);
    dlUrl = s_otaStatus.downloadUrl;
  }
  if (dlUrl.length() == 0) {
    setOtaError("No matching firmware asset found for board " + getBoardModel());
    return false;
  }
  return startUrlOta(dlUrl);
}

void otaProcess(void) {
  if (s_otaRebootScheduled && (int32_t)(millis() - s_otaRebootTime) >= 0) {
    s_otaRebootScheduled = false;
    Serial.println("[OTA] Saving stats and rebooting into newly updated firmware...");
    saveStat();
    delay(100);
    ESP.restart();
  }
}

// ----------------- Web UI & API Handlers -----------------

static esp_err_t handleOtaOptions(httpd_req_t *req) {
  return handleCorsOptions(req);
}

static esp_err_t handleOtaPage(httpd_req_t *req) {
  if (!checkWebApiMutationAllowed(req)) return ESP_OK;
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, (const char *)OTA_HTML_GZ, OTA_HTML_GZ_LEN);
}

static esp_err_t handleApiGetUpdate(httpd_req_t *req) {
  OtaStatus st = getOtaStatus();
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"state\":%d,\"progress\":%d,\"bytesWritten\":%lu,\"totalBytes\":%lu,"
           "\"runningPartition\":\"%s\",\"nextPartition\":\"%s\",\"hasDualOta\":%s,"
           "\"boardModel\":\"%s\",\"currentVersion\":\"%s\",\"latestVersion\":\"%s\","
           "\"hasUpdate\":%s,\"releaseUrl\":\"%s\",\"downloadUrl\":\"%s\",\"error\":\"%s\"}",
           (int)st.state, st.progressPercent,
           (unsigned long)st.bytesWritten, (unsigned long)st.totalBytes,
           jsonEscape(st.runningPartition).c_str(),
           jsonEscape(st.nextPartition).c_str(),
           st.hasDualOta ? "true" : "false",
           jsonEscape(st.boardModel).c_str(),
           jsonEscape(st.currentVersion).c_str(),
           jsonEscape(st.latestVersion).c_str(),
           st.hasUpdate ? "true" : "false",
           jsonEscape(st.releaseUrl).c_str(),
           jsonEscape(st.downloadUrl).c_str(),
           jsonEscape(st.errorMessage).c_str());

  return sendCorsJson(req, 200, buf, true);
}

static esp_err_t handleApiPostUpdate(httpd_req_t *req) {
  if (!checkWebApiMutationAllowed(req)) return ESP_OK;

  String targetUrl = "";
  String body = reqReadBody(req, 1024);
  if (body.length() > 0) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body.c_str()) == DeserializationError::Ok) {
      if (doc.containsKey("url")) targetUrl = doc["url"].as<String>();
    }
  }
  if (targetUrl.length() == 0) {
    targetUrl = reqQueryParam(req, "url");
  }

  bool ok = (targetUrl.length() > 0) ? startUrlOta(targetUrl) : startGitHubOta("latest");
  OtaStatus st = getOtaStatus();

  char buf[384];
  if (!ok && st.errorMessage.length() > 0) {
    snprintf(buf, sizeof(buf),
             "{\"success\":false,\"message\":\"Failed to start OTA update\",\"error\":\"%s\"}",
             jsonEscape(st.errorMessage).c_str());
  } else {
    snprintf(buf, sizeof(buf),
             "{\"success\":%s,\"message\":\"%s\"}",
             ok ? "true" : "false",
             ok ? "OTA update initiated" : "Failed to start OTA update");
  }

  return sendCorsJson(req, ok ? 200 : 400, buf);
}

static esp_err_t handleApiCheckUpdate(httpd_req_t *req) {
  if (!checkWebApiMutationAllowed(req)) return ESP_OK;

  bool started = startGitHubCheck();
  OtaStatus st = getOtaStatus();

  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"success\":%s,\"state\":%d,\"currentVersion\":\"%s\",\"latestVersion\":\"%s\","
           "\"hasUpdate\":%s,\"downloadUrl\":\"%s\",\"releaseUrl\":\"%s\","
           "\"releaseNotes\":\"%s\",\"error\":\"%s\"}",
           started ? "true" : "false",
           (int)st.state,
           jsonEscape(st.currentVersion).c_str(),
           jsonEscape(st.latestVersion).c_str(),
           st.hasUpdate ? "true" : "false",
           jsonEscape(st.downloadUrl).c_str(),
           jsonEscape(st.releaseUrl).c_str(),
           jsonEscape(st.releaseNotes).c_str(),
           jsonEscape(st.errorMessage).c_str());

  return sendCorsJson(req, started ? 202 : (st.state == OTA_STATE_ERROR ? 500 : 409), buf, true);
}

static esp_err_t sendUploadError500(httpd_req_t *req, const String &errMsg) {
  httpd_resp_set_status(req, "500 Internal Server Error");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Connection", "close");
  String body = "Update Failed: " + errMsg;
  return httpd_resp_send(req, body.c_str(), body.length());
}

static const uint8_t *findBytes(const uint8_t *haystack, size_t haystackLen,
                                const uint8_t *needle, size_t needleLen) {
  if (needleLen == 0 || haystackLen < needleLen) return nullptr;
  for (size_t i = 0; i <= haystackLen - needleLen; ++i) {
    if (haystack[i] == needle[0] && memcmp(haystack + i, needle, needleLen) == 0) {
      return haystack + i;
    }
  }
  return nullptr;
}

static esp_err_t handleUpdateUpload(httpd_req_t *req) {
  if (!checkWebApiMutationAllowed(req)) {
    return ESP_OK;
  }

  if (req->content_len <= 0) {
    String msg = "Empty or invalid upload content length";
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    return sendUploadError500(req, msg);
  }

  String contentType = reqHeader(req, "Content-Type");
  String contentTypeLower = contentType;
  contentTypeLower.toLowerCase();
  bool isMultipart = (contentTypeLower.indexOf("multipart/form-data") >= 0);
  String boundary = "";
  if (isMultipart) {
    int bIdx = contentTypeLower.indexOf("boundary=");
    if (bIdx >= 0) {
      String raw = contentType.substring(bIdx + 9);
      raw.trim();
      if (raw.startsWith("\"")) {
        int endQuote = raw.indexOf('"', 1);
        boundary = (endQuote > 1) ? raw.substring(1, endQuote) : raw.substring(1);
      } else {
        int endPos = raw.indexOf(';');
        if (endPos < 0) endPos = raw.length();
        boundary = raw.substring(0, endPos);
        boundary.trim();
      }
    }
    if (boundary.length() == 0 || boundary.length() > 120) {
      String msg = "Invalid or missing multipart boundary";
      setOtaError(msg);
      Serial.println("[OTA] Error: " + msg);
      return sendUploadError500(req, msg);
    }
  }

  Serial.printf("[OTA] Starting upload (%d bytes, %s)\n",
                (int)req->content_len, isMultipart ? "multipart" : "raw");
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    s_otaStatus.state = OTA_STATE_FLASHING;
    s_otaStatus.bytesWritten = 0;
    s_otaStatus.totalBytes = 0;
    s_otaStatus.progressPercent = 0;
    s_otaStatus.errorMessage = "";
  }

  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  if (!update_partition) {
    String msg = "No OTA partition found; device must be flashed via USB once.";
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    return sendUploadError500(req, msg);
  }

  // Pause mining and yield HW SHA engine immediately before flash operations
  g_otaUpdating = true;
  pauseHwMinerForTls();

  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    String msg = "Update.begin failed: " + String(Update.errorString());
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    g_otaUpdating = false;
    return sendUploadError500(req, msg);
  }

  uint8_t headerBuf[16];
  size_t headerLen = 0;
  size_t bytesWritten = 0;
  size_t lastReportedBytes = 0;
  bool failed = false;

  auto writeFirmwareBytes = [&](const uint8_t *data, size_t len) -> bool {
    if (failed || len == 0) return !failed;
    const uint8_t *writePtr = data;
    size_t writeLen = len;
    if (headerLen < 16) {
      size_t take = 16 - headerLen;
      if (take > writeLen) take = writeLen;
      memcpy(headerBuf + headerLen, writePtr, take);
      headerLen += take;
      writePtr += take;
      writeLen -= take;
      if (headerLen == 16) {
        String valErr;
        if (!validateFirmwareHeader(headerBuf, 16, valErr)) {
          setOtaError(valErr);
          Serial.println("[OTA] Validation failed: " + valErr);
          Update.abort();
          failed = true;
          return false;
        }
        if (Update.write(headerBuf, 16) != 16) {
          String msg = "Write failed: " + String(Update.errorString());
          setOtaError(msg);
          Serial.println("[OTA] Error: " + msg);
          Update.abort();
          failed = true;
          return false;
        }
        bytesWritten += 16;
      }
    }
    if (writeLen > 0 && headerLen == 16) {
      if (Update.write(const_cast<uint8_t *>(writePtr), writeLen) != writeLen) {
        String msg = "Write failed: " + String(Update.errorString());
        setOtaError(msg);
        Serial.println("[OTA] Error: " + msg);
        Update.abort();
        failed = true;
        return false;
      }
      bytesWritten += writeLen;
    }
    if (headerLen == 16 && (bytesWritten - lastReportedBytes) >= 16384) {
      lastReportedBytes = bytesWritten;
      std::lock_guard<std::mutex> lock(s_otaMutex);
      s_otaStatus.bytesWritten = bytesWritten;
    }
    return true;
  };

  uint8_t recvBuf[2048];
  size_t remaining = (size_t)req->content_len;

  // Multipart state
  bool inPartHeaders = isMultipart;
  bool boundaryFound = false;
  String partHdrBuf;
  const String boundaryMarker = isMultipart ? ("\r\n--" + boundary) : String("");
  const size_t tailWindowSize = isMultipart ? (boundary.length() + 6) : 0;
  // tailWindowSize <= 126, so 2048 + 128 fits both carried tail and one recvBuf chunk
  uint8_t workBuf[2048 + 128];
  size_t tailLen = 0;

  auto feedMultipartBody = [&](const uint8_t *data, size_t len) -> bool {
    if (boundaryFound || len == 0 || failed) return !failed;
    size_t offset = 0;
    while (offset < len && !boundaryFound && !failed) {
      size_t space = sizeof(workBuf) - tailLen;
      size_t take = len - offset;
      if (take > space) take = space;
      memcpy(workBuf + tailLen, data + offset, take);
      size_t totalLen = tailLen + take;
      offset += take;

      const uint8_t *match = findBytes(
          workBuf, totalLen,
          reinterpret_cast<const uint8_t *>(boundaryMarker.c_str()),
          boundaryMarker.length());
      if (match != nullptr) {
        size_t payloadLen = (size_t)(match - workBuf);
        if (payloadLen > 0) {
          writeFirmwareBytes(workBuf, payloadLen);
        }
        boundaryFound = true;
        tailLen = 0;
        break;
      }

      if (totalLen > tailWindowSize) {
        size_t flushLen = totalLen - tailWindowSize;
        if (!writeFirmwareBytes(workBuf, flushLen)) {
          return false;
        }
        memmove(workBuf, workBuf + flushLen, tailWindowSize);
        tailLen = tailWindowSize;
      } else {
        tailLen = totalLen;
      }
    }
    return !failed;
  };

  while (remaining > 0) {
    int toRead = (remaining > sizeof(recvBuf)) ? (int)sizeof(recvBuf) : (int)remaining;
    int ret = httpd_req_recv(req, (char *)recvBuf, toRead);
    if (ret <= 0) {
      String msg = "Upload stream interrupted";
      setOtaError(msg);
      Serial.println("[OTA] Error: " + msg);
      Update.abort();
      failed = true;
      break;
    }
    remaining -= (size_t)ret;

    if (failed || boundaryFound) {
      continue;
    }

    if (!isMultipart) {
      if (!writeFirmwareBytes(recvBuf, (size_t)ret)) {
        break;
      }
      continue;
    }

    if (inPartHeaders) {
      partHdrBuf.concat((const char *)recvBuf, (unsigned int)ret);
      int hdrEnd = partHdrBuf.indexOf("\r\n\r\n");
      if (hdrEnd >= 0) {
        inPartHeaders = false;
        size_t bodyStart = (size_t)hdrEnd + 4;
        if (bodyStart < partHdrBuf.length()) {
          feedMultipartBody(
              reinterpret_cast<const uint8_t *>(partHdrBuf.c_str()) + bodyStart,
              partHdrBuf.length() - bodyStart);
        }
        partHdrBuf = "";
      } else if (partHdrBuf.length() > 4096) {
        String msg = "Multipart headers too large";
        setOtaError(msg);
        Serial.println("[OTA] Error: " + msg);
        Update.abort();
        failed = true;
        break;
      }
    } else {
      feedMultipartBody(recvBuf, (size_t)ret);
    }
  }

  if (!failed && isMultipart && !boundaryFound) {
    String msg = "Truncated multipart upload (missing closing boundary)";
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    Update.abort();
    failed = true;
  }

  if (!failed) {
    if (headerLen < 16) {
      setOtaError("Firmware image too small (< 16 bytes)");
      Update.abort();
      failed = true;
    } else if (Update.end(true)) {
      {
        std::lock_guard<std::mutex> lock(s_otaMutex);
        s_otaStatus.bytesWritten = bytesWritten;
        s_otaStatus.totalBytes = bytesWritten;
        s_otaStatus.state = OTA_STATE_SUCCESS;
        s_otaStatus.progressPercent = 100;
      }
      Serial.printf("[OTA] Manual upload success! Total: %u bytes.\n", (unsigned)bytesWritten);
    } else {
      String msg = "Update.end failed: " + String(Update.errorString());
      setOtaError(msg);
      Serial.println("[OTA] Error: " + msg);
      failed = true;
    }
  }

  g_otaUpdating = false;

  if (failed || Update.hasError()) {
    OtaStatus st = getOtaStatus();
    String errMsg = st.errorMessage.length() > 0 ? st.errorMessage : String(Update.errorString());
    return sendUploadError500(req, errMsg);
  }

  static const char SUCCESS_HTML[] =
      "<html><head><meta http-equiv='refresh' content='15;url=/'></head>"
      "<body style='font-family:sans-serif;background:#121212;color:#e0e0e0;padding:2em;text-align:center'>"
      "<h2>&#x2705; Update Successful!</h2>"
      "<p>Firmware written successfully. Device is restarting...</p>"
      "<p>Redirecting in 15 seconds...</p>"
      "</body></html>";
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "text/html");
  esp_err_t res = httpd_resp_send(req, SUCCESS_HTML, sizeof(SUCCESS_HTML) - 1);
  s_otaRebootTime = millis() + 1000;
  s_otaRebootScheduled = true;
  return res;
}

void setup_ota(httpd_handle_t server) {
  static const httpd_uri_t routes[] = {
      {"/update", HTTP_OPTIONS, handleOtaOptions, NULL},
      {"/update", HTTP_GET, handleOtaPage, NULL},
      {"/update", HTTP_POST, handleUpdateUpload, NULL},
      {"/api/system/update", HTTP_OPTIONS, handleOtaOptions, NULL},
      {"/api/system/update", HTTP_GET, handleApiGetUpdate, NULL},
      {"/api/system/update", HTTP_POST, handleApiPostUpdate, NULL},
      {"/api/system/update/check", HTTP_OPTIONS, handleOtaOptions, NULL},
      {"/api/system/update/check", HTTP_POST, handleApiCheckUpdate, NULL},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    httpd_register_uri_handler(server, &routes[i]);
  }
}
