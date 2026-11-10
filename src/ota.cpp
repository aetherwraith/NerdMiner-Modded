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

extern bool checkWebApiMutationAllowed();
extern bool isWebApiMutationPermitted();

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
static String s_targetOtaUrl = "";
static volatile bool s_otaRebootScheduled = false;
static unsigned long s_otaRebootTime = 0;
static bool s_uploadRejected = false;

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

  WiFiClientSecure client;
  client.setInsecure(); // Skip full root CA bundle to preserve heap

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setUserAgent("NerdMiner-Modded-OTA/" CURRENT_VERSION);

  if (!http.begin(client, url)) {
    String msg = "Failed to initialize HTTPS connection";
    setOtaError(msg);
    Serial.println("[OTA] Error: " + msg);
    g_otaUpdating = false;
    s_otaTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  int httpCode = 0;
  {
    HW_SHA_REQUEST();
    std::lock_guard<std::mutex> shaLock(g_hwShaMutex);
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
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    s_otaStatus.bytesWritten = writtenTotal;
    if (totalBytes > 0) {
      s_otaStatus.progressPercent = (int)((writtenTotal * 100ULL) / totalBytes);
    }
  }

  bool writeFailed = false;
  // Stream remaining bytes while g_otaUpdating stays true
  while (http.connected() && (writtenTotal < totalBytes || totalBytes == 0)) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t toRead = (avail > sizeof(buffer)) ? sizeof(buffer) : avail;
      size_t bytesRead = stream->readBytes(buffer, toRead);
      if (bytesRead > 0) {
        size_t written = Update.write(buffer, bytesRead);
        if (written != bytesRead) {
          setOtaError("Write error: " + String(Update.errorString()));
          Update.abort();
          writeFailed = true;
          break;
        }
        writtenTotal += written;
        std::lock_guard<std::mutex> lock(s_otaMutex);
        s_otaStatus.bytesWritten = writtenTotal;
        if (totalBytes > 0) {
          s_otaStatus.progressPercent = (int)((writtenTotal * 100ULL) / totalBytes);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  http.end();

  if (!writeFailed) {
    if (Update.end(true)) {
      {
        std::lock_guard<std::mutex> lock(s_otaMutex);
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
    if (s_otaStatus.state == OTA_STATE_DOWNLOADING || s_otaStatus.state == OTA_STATE_FLASHING) {
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

  int httpCode = 0;
  {
    HW_SHA_REQUEST();
    std::lock_guard<std::mutex> shaLock(g_hwShaMutex);
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

  String payload = http.getString();
  http.end();
  if (payload.isEmpty()) {
    return false;
  }

  // Filter keys to minimize JSON memory usage and use const char* reader ONLY
  StaticJsonDocument<256> filter;
  filter["tag_name"] = true;
  filter["name"] = true;
  filter["html_url"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["size"] = true;

  DeserializationError err = deserializeJson(doc, payload.c_str(), DeserializationOption::Filter(filter));
  Serial.printf("[OTA] JSON parse result: %s\n", err.c_str());
  return (err == DeserializationError::Ok);
}

bool checkGitHubUpdate(void) {
  {
    std::lock_guard<std::mutex> lock(s_otaMutex);
    if (s_otaStatus.state == OTA_STATE_DOWNLOADING || s_otaStatus.state == OTA_STATE_FLASHING) {
      return false;
    }
    s_otaStatus.state = OTA_STATE_CHECKING;
    s_otaStatus.errorMessage = "";
  }

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
    bool resumeMining = false;
    {
      std::lock_guard<std::mutex> lock(s_otaMutex);
      s_otaStatus.state = OTA_STATE_ERROR;
      s_otaStatus.errorMessage = "Failed to query GitHub releases API";
      resumeMining = wasMining && s_otaStatus.state != OTA_STATE_DOWNLOADING && s_otaStatus.state != OTA_STATE_FLASHING;
    }
    if (resumeMining) g_otaUpdating = false;
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
  if (s_otaRebootScheduled && millis() >= s_otaRebootTime) {
    s_otaRebootScheduled = false;
    Serial.println("[OTA] Saving stats and rebooting into newly updated firmware...");
    saveStat();
    delay(100);
    ESP.restart();
  }
}

// ----------------- Web UI & API Handlers -----------------

static void sendOtaOptions(WebServer &server) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
  server.send(204);
}

static void handleOtaPage(WebServer &server) {
  if (!checkWebApiMutationAllowed()) return;
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", (PGM_P)OTA_HTML_GZ, OTA_HTML_GZ_LEN);
}

static void handleApiGetUpdate(WebServer &server) {
  OtaStatus st = getOtaStatus();
  String json;
  json.reserve(480);
  json += "{\"state\":" + String((int)st.state);
  json += ",\"progress\":" + String(st.progressPercent);
  json += ",\"bytesWritten\":" + String((unsigned long)st.bytesWritten);
  json += ",\"totalBytes\":" + String((unsigned long)st.totalBytes);
  json += ",\"runningPartition\":\"" + jsonEscape(st.runningPartition) + "\"";
  json += ",\"nextPartition\":\"" + jsonEscape(st.nextPartition) + "\"";
  json += ",\"hasDualOta\":" + String(st.hasDualOta ? "true" : "false");
  json += ",\"boardModel\":\"" + jsonEscape(st.boardModel) + "\"";
  json += ",\"currentVersion\":\"" + jsonEscape(st.currentVersion) + "\"";
  json += ",\"latestVersion\":\"" + jsonEscape(st.latestVersion) + "\"";
  json += ",\"hasUpdate\":" + String(st.hasUpdate ? "true" : "false");
  json += ",\"releaseUrl\":\"" + jsonEscape(st.releaseUrl) + "\"";
  json += ",\"downloadUrl\":\"" + jsonEscape(st.downloadUrl) + "\"";
  json += ",\"error\":\"" + jsonEscape(st.errorMessage) + "\"}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void handleApiPostUpdate(WebServer &server) {
  if (server.method() == HTTP_OPTIONS) {
    sendOtaOptions(server);
    return;
  }
  if (!checkWebApiMutationAllowed()) return;

  String targetUrl = "";
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body.c_str()) == DeserializationError::Ok) {
      if (doc.containsKey("url")) targetUrl = doc["url"].as<String>();
    }
  }
  if (targetUrl.length() == 0 && server.hasArg("url")) {
    targetUrl = server.arg("url");
  }

  bool ok = (targetUrl.length() > 0) ? startUrlOta(targetUrl) : startGitHubOta("latest");
  OtaStatus st = getOtaStatus();

  String json;
  json.reserve(192);
  json += "{\"success\":" + String(ok ? "true" : "false");
  json += ",\"message\":\"" + String(ok ? "OTA update initiated" : "Failed to start OTA update") + "\"";
  if (!ok && st.errorMessage.length() > 0) {
    json += ",\"error\":\"" + jsonEscape(st.errorMessage) + "\"";
  }
  json += "}";

  // checkWebApiMutationAllowed() already added CORS headers
  server.send(ok ? 200 : 400, "application/json", json);
}

static void handleApiCheckUpdate(WebServer &server) {
  if (server.method() == HTTP_OPTIONS) {
    sendOtaOptions(server);
    return;
  }

  bool ok = checkGitHubUpdate();
  OtaStatus st = getOtaStatus();

  String json;
  json.reserve(384);
  json += "{\"success\":" + String(ok ? "true" : "false");
  json += ",\"currentVersion\":\"" + jsonEscape(st.currentVersion) + "\"";
  json += ",\"latestVersion\":\"" + jsonEscape(st.latestVersion) + "\"";
  json += ",\"hasUpdate\":" + String(st.hasUpdate ? "true" : "false");
  json += ",\"downloadUrl\":\"" + jsonEscape(st.downloadUrl) + "\"";
  json += ",\"releaseUrl\":\"" + jsonEscape(st.releaseUrl) + "\"";
  json += ",\"releaseNotes\":\"" + jsonEscape(st.releaseNotes) + "\"";
  if (!ok) {
    json += ",\"error\":\"" + jsonEscape(st.errorMessage) + "\"";
  }
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(ok ? 200 : 500, "application/json", json);
}

void setup_ota(WebServer &server) {
  server.on("/update", HTTP_OPTIONS, [&server]() {
    sendOtaOptions(server);
  });

  server.on("/update", HTTP_GET, [&server]() {
    handleOtaPage(server);
  });

  server.on("/update", HTTP_POST, [&server]() {
    if (!checkWebApiMutationAllowed()) {
      return;
    }
    server.sendHeader("Connection", "close");
    OtaStatus st = getOtaStatus();
    if (Update.hasError() || st.state == OTA_STATE_ERROR) {
      String errMsg = st.errorMessage.length() > 0 ? st.errorMessage : String(Update.errorString());
      server.send(500, "text/plain", "Update Failed: " + errMsg);
    } else {
      server.send(200, "text/html", F("<html><head><meta http-equiv='refresh' content='15;url=/'></head>"
                                      "<body style='font-family:sans-serif;background:#121212;color:#e0e0e0;padding:2em;text-align:center'>"
                                      "<h2>&#x2705; Update Successful!</h2>"
                                      "<p>Firmware written successfully. Device is restarting...</p>"
                                      "<p>Redirecting in 15 seconds...</p>"
                                      "</body></html>"));
      saveStat();
      delay(1000);
      ESP.restart();
    }
  }, [&server]() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      if (!isWebApiMutationPermitted()) {
        s_uploadRejected = true;
        Serial.println(F("[OTA] Rejected unauthorized or cross-origin /update upload"));
        return;
      }
      s_uploadRejected = false;
      Serial.printf("[OTA] Starting upload: %s\n", upload.filename.c_str());
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
        Update.abort();
        return;
      }

      // Pause mining and yield HW SHA engine immediately before flash operations
      g_otaUpdating = true;
      pauseHwMinerForTls();

      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        String msg = "Update.begin failed: " + String(Update.errorString());
        setOtaError(msg);
        Serial.println("[OTA] Error: " + msg);
        g_otaUpdating = false;
      }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      if (s_uploadRejected) return;
      if (Update.isRunning()) {
        size_t writtenSoFar = 0;
        {
          std::lock_guard<std::mutex> lock(s_otaMutex);
          writtenSoFar = s_otaStatus.bytesWritten;
        }
        if (writtenSoFar == 0 && upload.currentSize >= 16) {
          String valErr;
          if (!validateFirmwareHeader(upload.buf, upload.currentSize, valErr)) {
            setOtaError(valErr);
            Serial.println("[OTA] Validation failed: " + valErr);
            Update.abort();
            g_otaUpdating = false;
            return;
          }
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          String msg = "Write failed: " + String(Update.errorString());
          setOtaError(msg);
          Serial.println("[OTA] Error: " + msg);
          Update.abort();
          g_otaUpdating = false;
        } else {
          std::lock_guard<std::mutex> lock(s_otaMutex);
          s_otaStatus.bytesWritten += upload.currentSize;
        }
      }
    }
    else if (upload.status == UPLOAD_FILE_END) {
      if (s_uploadRejected) return;
      if (Update.isRunning()) {
        if (Update.end(true)) {
          {
            std::lock_guard<std::mutex> lock(s_otaMutex);
            s_otaStatus.state = OTA_STATE_SUCCESS;
            s_otaStatus.progressPercent = 100;
          }
          Serial.printf("[OTA] Manual upload success! Total: %u bytes.\n", upload.totalSize);
        } else {
          String msg = "Update.end failed: " + String(Update.errorString());
          setOtaError(msg);
          Serial.println("[OTA] Error: " + msg);
        }
      }
      g_otaUpdating = false;
    }
    else if (upload.status == UPLOAD_FILE_ABORTED) {
      if (s_uploadRejected) return;
      Update.abort();
      setOtaError("Upload aborted by client");
      Serial.println(F("[OTA] Upload aborted"));
      g_otaUpdating = false;
    }
  });

  server.on("/api/system/update", HTTP_OPTIONS, [&server]() {
    sendOtaOptions(server);
  });

  server.on("/api/system/update", HTTP_GET, [&server]() {
    handleApiGetUpdate(server);
  });

  server.on("/api/system/update", HTTP_POST, [&server]() {
    handleApiPostUpdate(server);
  });

  server.on("/api/system/update/check", HTTP_OPTIONS, [&server]() {
    sendOtaOptions(server);
  });

  server.on("/api/system/update/check", HTTP_POST, [&server]() {
    handleApiCheckUpdate(server);
  });

  server.on("/api/system/update/check", HTTP_GET, [&server]() {
    handleApiCheckUpdate(server);
  });
}
