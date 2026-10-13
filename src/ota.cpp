#include "ota.h"
#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#include "version.h"
#include "mining.h"
#include "wManager.h"
#include "drivers/storage/storage.h"

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

static OtaStatus s_otaStatus;
static TaskHandle_t s_otaTaskHandle = NULL;
static String s_targetOtaUrl = "";
static bool s_otaRebootScheduled = false;
static unsigned long s_otaRebootTime = 0;

String getBoardModel(void) {
  return String(PIOENV_NAME);
}

bool hasDualOtaSupport(void) {
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  return (update_partition != NULL);
}

String getRunningPartitionName(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  return running ? String(running->label) : "unknown";
}

String getNextPartitionName(void) {
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  return next ? String(next->label) : "none";
}

OtaStatus getOtaStatus(void) {
  s_otaStatus.runningPartition = getRunningPartitionName();
  s_otaStatus.nextPartition = getNextPartitionName();
  s_otaStatus.hasDualOta = hasDualOtaSupport();
  s_otaStatus.boardModel = getBoardModel();
  s_otaStatus.currentVersion = String(CURRENT_VERSION);
  return s_otaStatus;
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
  String url = s_targetOtaUrl;
  Serial.printf("[OTA] Starting background download from: %s\n", url.c_str());

  s_otaStatus.state = OTA_STATE_DOWNLOADING;
  s_otaStatus.bytesWritten = 0;
  s_otaStatus.totalBytes = 0;
  s_otaStatus.progressPercent = 0;
  s_otaStatus.errorMessage = "";

  if (!hasDualOtaSupport()) {
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = "Dual-OTA partitions not configured on this device. USB flash required once.";
    Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
    s_otaTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }

  // Pause mining tasks so SHA hardware and flash cache are completely free
  g_otaUpdating = true;
  vTaskDelay(pdMS_TO_TICKS(200));

  WiFiClientSecure client;
  client.setInsecure(); // Skip full root CA bundle to preserve heap

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setUserAgent("NerdMiner-Modded-OTA/" CURRENT_VERSION);

  if (!http.begin(client, url)) {
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = "Failed to initialize HTTPS connection";
    Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
    g_otaUpdating = false;
    s_otaTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_FOUND && httpCode != HTTP_CODE_MOVED_PERMANENTLY) {
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = "HTTP request failed with code " + String(httpCode);
    Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int contentLength = http.getSize();
  s_otaStatus.totalBytes = (contentLength > 0) ? (size_t)contentLength : 0;
  Serial.printf("[OTA] Connected. Content length: %d bytes\n", contentLength);

  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = "Failed to obtain HTTP stream";
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = NULL;
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
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = validateErr;
    Serial.println("[OTA] Validation failed: " + validateErr);
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }

  s_otaStatus.state = OTA_STATE_FLASHING;
  size_t updateSize = (contentLength > 0) ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(updateSize, U_FLASH)) {
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = "Update.begin failed: " + String(Update.errorString());
    Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
    http.end();
    g_otaUpdating = false;
    s_otaTaskHandle = NULL;
    vTaskDelete(NULL);
    return;
  }

  // Write the validated first chunk
  Update.write(buffer, firstChunkSize);
  s_otaStatus.bytesWritten = firstChunkSize;

  // Stream remaining bytes
  while (http.connected() && (s_otaStatus.bytesWritten < s_otaStatus.totalBytes || s_otaStatus.totalBytes == 0)) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t toRead = (avail > sizeof(buffer)) ? sizeof(buffer) : avail;
      size_t bytesRead = stream->readBytes(buffer, toRead);
      if (bytesRead > 0) {
        size_t written = Update.write(buffer, bytesRead);
        if (written != bytesRead) {
          s_otaStatus.state = OTA_STATE_ERROR;
          s_otaStatus.errorMessage = "Write error: " + String(Update.errorString());
          Update.abort();
          break;
        }
        s_otaStatus.bytesWritten += written;
        if (s_otaStatus.totalBytes > 0) {
          s_otaStatus.progressPercent = (int)((s_otaStatus.bytesWritten * 100) / s_otaStatus.totalBytes);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  http.end();

  if (s_otaStatus.state != OTA_STATE_ERROR) {
    if (Update.end(true)) {
      s_otaStatus.state = OTA_STATE_SUCCESS;
      s_otaStatus.progressPercent = 100;
      Serial.printf("[OTA] Update Success! Wrote %u bytes. Rebooting...\n", s_otaStatus.bytesWritten);
      s_otaRebootScheduled = true;
      s_otaRebootTime = millis() + 2000;
    } else {
      s_otaStatus.state = OTA_STATE_ERROR;
      s_otaStatus.errorMessage = "Update.end failed: " + String(Update.errorString());
      Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
      g_otaUpdating = false;
    }
  } else {
    g_otaUpdating = false;
  }

  s_otaTaskHandle = NULL;
  vTaskDelete(NULL);
}

bool startUrlOta(const String &url) {
  if (s_otaStatus.state == OTA_STATE_DOWNLOADING || s_otaStatus.state == OTA_STATE_FLASHING) {
    return false;
  }
  s_targetOtaUrl = url;
  BaseType_t res = xTaskCreatePinnedToCore(otaWorkerTask, "OtaWorker", 8192, NULL, 3, &s_otaTaskHandle, 0);
  return (res == pdPASS);
}

static bool fetchGitHubRelease(const char* repo, DynamicJsonDocument &doc) {
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

  int httpCode = http.GET();
  Serial.printf("[OTA] HTTP GET code: %d\n", httpCode);
  if (httpCode != HTTP_CODE_OK) {
    if (httpCode < 0) {
      Serial.printf("[OTA] HTTP error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
    return false;
  }

  // Filter keys to minimize JSON memory usage - exclude large body markdown
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

bool checkGitHubUpdate(void) {
  s_otaStatus.state = OTA_STATE_CHECKING;
  s_otaStatus.errorMessage = "";

  // Pause mining tasks while TLS handshake and JSON parsing occur
  bool wasMining = !g_otaUpdating;
  g_otaUpdating = true;
  vTaskDelay(pdMS_TO_TICKS(150));

  DynamicJsonDocument doc(6144);
  bool ok = fetchGitHubRelease(GITHUB_PRIMARY_REPO, doc);
  if (!ok) {
    doc.clear();
    ok = fetchGitHubRelease(GITHUB_FALLBACK_REPO, doc);
  }

  if (!ok || !doc.containsKey("tag_name")) {
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = "Failed to query GitHub releases API";
    if (wasMining && s_otaStatus.state != OTA_STATE_DOWNLOADING && s_otaStatus.state != OTA_STATE_FLASHING) {
      g_otaUpdating = false;
    }
    return false;
  }

  String latestTag = doc["tag_name"].as<String>();
  String currentVer = String(CURRENT_VERSION);
  s_otaStatus.latestVersion = latestTag;
  s_otaStatus.releaseUrl = doc.containsKey("html_url") ? doc["html_url"].as<String>() : "";
  s_otaStatus.releaseNotes = "";

  // Compare versions
  String tagClean = latestTag;
  if (tagClean.startsWith("v") || tagClean.startsWith("V")) tagClean = tagClean.substring(1);
  String curClean = currentVer;
  if (curClean.startsWith("v") || curClean.startsWith("V")) curClean = curClean.substring(1);

  s_otaStatus.hasUpdate = (!tagClean.equalsIgnoreCase(curClean));

  // Find matching binary asset for this board: {PIOENV_NAME}_firmware.bin
  String targetAsset = getBoardModel() + "_firmware.bin";
  s_otaStatus.downloadUrl = "";

  if (doc.containsKey("assets") && doc["assets"].is<JsonArray>()) {
    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
      String name = asset["name"].as<String>();
      if (name.equalsIgnoreCase(targetAsset)) {
        s_otaStatus.downloadUrl = asset["browser_download_url"].as<String>();
        break;
      }
    }
  }

  s_otaStatus.state = OTA_STATE_IDLE;
  if (wasMining && s_otaStatus.state != OTA_STATE_DOWNLOADING && s_otaStatus.state != OTA_STATE_FLASHING) {
    g_otaUpdating = false;
  }
  return true;
}

bool startGitHubOta(const String &targetVersion) {
  if (s_otaStatus.downloadUrl.length() == 0) {
    if (!checkGitHubUpdate()) return false;
  }
  if (s_otaStatus.downloadUrl.length() == 0) {
    s_otaStatus.state = OTA_STATE_ERROR;
    s_otaStatus.errorMessage = "No matching firmware asset found for board " + getBoardModel();
    return false;
  }
  return startUrlOta(s_otaStatus.downloadUrl);
}

void otaProcess(void) {
  if (s_otaRebootScheduled && millis() >= s_otaRebootTime) {
    Serial.println("[OTA] Rebooting into newly updated firmware...");
    ESP.restart();
  }
}

// ----------------- Web UI & API Handlers -----------------

static void handleOtaPage(WebServer &server) {
  OtaStatus st = getOtaStatus();

  String html;
  html.reserve(4096);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>NerdMiner Firmware Update</title>"
            "<style>"
            "body{font-family:sans-serif;background:#121212;color:#e0e0e0;margin:0;padding:1.5em;line-height:1.5}"
            ".container{max-width:540px;margin:0 auto}"
            ".card{background:#1e1e1e;border-radius:8px;padding:1.2em;margin-bottom:1.2em;box-shadow:0 4px 6px rgba(0,0,0,0.3)}"
            "h2,h3{color:#f59e0b;margin-top:0}"
            ".badge{display:inline-block;padding:0.2em 0.6em;border-radius:4px;font-size:0.85em;font-weight:bold}"
            ".badge-green{background:#065f46;color:#34d399}"
            ".badge-yellow{background:#78350f;color:#fde68a}"
            ".badge-red{background:#7f1d1d;color:#fca5a5}"
            ".badge-blue{background:#1e3a8a;color:#93c5fd}"
            "button,.btn{background:#f59e0b;color:#000;font-weight:bold;border:none;padding:0.6em 1.2em;border-radius:4px;cursor:pointer;width:100%;margin-top:0.8em;font-size:1em;box-sizing:border-box;text-align:center;text-decoration:none;display:inline-block}"
            "button:hover,.btn:hover{background:#d97706}"
            "button:disabled{background:#4b5563;color:#9ca3af;cursor:not-allowed}"
            "input[type=file]{display:block;width:100%;padding:0.6em;background:#2d2d2d;border:1px dashed #4b5563;border-radius:4px;color:#9ca3af;margin-top:0.6em;box-sizing:border-box}"
            ".progress-container{background:#2d2d2d;border-radius:4px;overflow:hidden;margin-top:0.8em;height:20px;display:none}"
            ".progress-bar{background:#10b981;height:100%;width:0%;transition:width 0.2s}"
            ".info-row{display:flex;justify-content:space-between;padding:0.3em 0;border-bottom:1px solid #2d2d2d}"
            ".info-row:last-child{border-bottom:none}"
            ".warning{background:#451a03;border-left:4px solid #f59e0b;padding:0.8em;border-radius:4px;margin-bottom:1em;font-size:0.9em}"
            "</style></head><body><div class='container'>"
            "<h2>⚡ Firmware Update (OTA)</h2>");

  if (!st.hasDualOta) {
    html += F("<div class='warning'>⚠️ <b>Dual-OTA not enabled:</b> This device is running a single partition. Flash once via USB to enable dual-partition OTA.</div>");
  }

  html += "<div class='card'>"
          "<h3>Device Info</h3>"
          "<div class='info-row'><span>Model</span><b>" + st.boardModel + "</b></div>"
          "<div class='info-row'><span>Installed Version</span><b>" + st.currentVersion + "</b></div>"
          "<div class='info-row'><span>Active Partition</span><span class='badge badge-blue'>" + st.runningPartition + "</span></div>"
          "<div class='info-row'><span>Target Partition</span><span class='badge " + (st.hasDualOta ? "badge-green" : "badge-red") + "'>" + st.nextPartition + "</span></div>"
          "</div>";

  html += "<div class='card'>"
          "<h3>🌐 Online Update (GitHub)</h3>"
          "<p style='font-size:0.9em;color:#9ca3af'>Download and install latest releases directly from GitHub.</p>"
          "<button id='btnCheck' onclick='checkUpdates()'>Check for Updates</button>"
          "<div id='releaseInfo' style='display:none;margin-top:0.8em;padding:0.6em;background:#262626;border-radius:4px'>"
          "<div><b>Latest:</b> <span id='relTag'></span></div>"
          "<div id='relNotes' style='font-size:0.85em;color:#9ca3af;margin:0.4em 0'></div>"
          "<button id='btnInstallOnline' onclick='installOnline()' style='background:#10b981;color:#fff'>Install Now</button>"
          "</div>"
          "</div>";

  html += "<div class='card'>"
          "<h3>📁 Manual Firmware Upload</h3>"
          "<p style='font-size:0.9em;color:#9ca3af'>Select a <code>" + st.boardModel + "_firmware.bin</code> file:</p>"
          "<form id='uploadForm' method='POST' action='/update' enctype='multipart/form-data' onsubmit='return uploadFile(event)'>"
          "<input type='file' id='fileInput' name='update' accept='.bin' required>"
          "<button type='submit' id='btnUpload'>Upload & Flash Firmware</button>"
          "</form>"
          "</div>";

  html += "<div class='card' id='statusCard' style='display:none'>"
          "<h3>Update Progress</h3>"
          "<div id='statusText' style='margin-bottom:0.4em;font-weight:bold'>Idle</div>"
          "<div class='progress-container' id='progContainer' style='display:block'>"
          "<div class='progress-bar' id='progBar'></div>"
          "</div>"
          "</div>";

  html += "<div style='text-align:center;margin-top:1.5em'>"
          "<a href='/' class='btn' style='background:#2d2d2d;color:#e0e0e0;width:auto;padding:0.5em 1.5em'>&larr; Back to Settings</a>"
          "</div>";

  html += F("<script>"
            "var timer=null;"
            "function checkUpdates(){"
            "var btn=document.getElementById('btnCheck');"
            "btn.disabled=true;btn.innerText='Checking...';"
            "fetch('/api/system/update/check',{method:'POST'}).then(function(r){return r.json()}).then(function(d){"
            "btn.disabled=false;btn.innerText='Check for Updates';"
            "if(d.latestVersion){"
            "document.getElementById('releaseInfo').style.display='block';"
            "document.getElementById('relTag').innerText=d.latestVersion + (d.hasUpdate?' (New!)':' (Up to date)');"
            "document.getElementById('relNotes').innerText=d.releaseNotes?d.releaseNotes.substring(0,180)+'...':'';"
            "document.getElementById('btnInstallOnline').style.display=d.downloadUrl?'block':'none';"
            "}else{alert(d.error||'No update found')}"
            "}).catch(function(e){btn.disabled=false;btn.innerText='Check for Updates';alert('Check failed: '+e)});"
            "}"
            "function installOnline(){"
            "if(!confirm('Install latest firmware and reboot?'))return;"
            "fetch('/api/system/update',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({version:'latest'})})"
            ".then(function(r){return r.json()}).then(function(d){"
            "if(d.success){startPolling();}"
            "else{alert(d.message||'Failed')}"
            "});"
            "}"
            "function uploadFile(e){"
            "e.preventDefault();"
            "var f=document.getElementById('fileInput').files[0];"
            "if(!f){alert('Please select a file');return false;}"
            "if(!confirm('Upload '+f.name+' ('+Math.round(f.size/1024)+' KB) and flash device?'))return false;"
            "document.getElementById('statusCard').style.display='block';"
            "document.getElementById('statusText').innerText='Uploading firmware...';"
            "var xhr=new XMLHttpRequest();"
            "xhr.open('POST','/update',true);"
            "xhr.upload.onprogress=function(pe){"
            "if(pe.lengthComputable){"
            "var pct=Math.round((pe.loaded/pe.total)*100);"
            "document.getElementById('progBar').style.width=pct+'%';"
            "document.getElementById('statusText').innerText='Uploading: '+pct+'% ('+Math.round(pe.loaded/1024)+' KB / '+Math.round(pe.total/1024)+' KB)';"
            "}"
            "};"
            "xhr.onload=function(){"
            "if(xhr.status==200){"
            "document.getElementById('progBar').style.width='100%';"
            "document.getElementById('statusText').innerText='✅ Flash complete! Device rebooting...';"
            "countdown(15);"
            "}else{document.getElementById('statusText').innerText='❌ Error: '+xhr.responseText;}"
            "};"
            "var fd=new FormData();fd.append('update',f);"
            "xhr.send(fd);"
            "return false;"
            "}"
            "function startPolling(){"
            "document.getElementById('statusCard').style.display='block';"
            "timer=setInterval(function(){"
            "fetch('/api/system/update').then(function(r){return r.json()}).then(function(d){"
            "var pct=d.progress||0;"
            "document.getElementById('progBar').style.width=pct+'%';"
            "if(d.state==2){document.getElementById('statusText').innerText='📥 Downloading update: '+pct+'%';}"
            "else if(d.state==3){document.getElementById('statusText').innerText='⚡ Flashing firmware: '+pct+'%';}"
            "else if(d.state==4){clearInterval(timer);document.getElementById('statusText').innerText='✅ Success! Device is rebooting...';countdown(15);}"
            "else if(d.state==5){clearInterval(timer);document.getElementById('statusText').innerText='❌ Error: '+(d.error||'Update failed');}"
            "});"
            "},1000);"
            "}"
            "function countdown(n){"
            "var c=n;"
            "var iv=setInterval(function(){"
            "document.getElementById('statusText').innerText='✅ Complete! Redirecting in '+c+'s...';"
            "c--;if(c<=0){clearInterval(iv);window.location.href='/';}"
            "},1000);"
            "}"
            "</script></body></html>");

  server.send(200, "text/html", html);
}

static void handleApiGetUpdate(WebServer &server) {
  OtaStatus st = getOtaStatus();
  StaticJsonDocument<512> doc;
  doc["state"] = (int)st.state;
  doc["progress"] = st.progressPercent;
  doc["bytesWritten"] = st.bytesWritten;
  doc["totalBytes"] = st.totalBytes;
  doc["runningPartition"] = st.runningPartition;
  doc["nextPartition"] = st.nextPartition;
  doc["hasDualOta"] = st.hasDualOta;
  doc["boardModel"] = st.boardModel;
  doc["currentVersion"] = st.currentVersion;
  doc["latestVersion"] = st.latestVersion;
  doc["hasUpdate"] = st.hasUpdate;
  doc["releaseUrl"] = st.releaseUrl;
  doc["downloadUrl"] = st.downloadUrl;
  doc["error"] = st.errorMessage;

  String json;
  serializeJson(doc, json);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

static void handleApiPostUpdate(WebServer &server) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  String targetUrl = "";
  if (server.hasArg("plain")) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain")) == DeserializationError::Ok) {
      if (doc.containsKey("url")) targetUrl = doc["url"].as<String>();
    }
  }
  if (targetUrl.length() == 0 && server.hasArg("url")) {
    targetUrl = server.arg("url");
  }

  bool ok = false;
  if (targetUrl.length() > 0) {
    ok = startUrlOta(targetUrl);
  } else {
    ok = startGitHubOta("latest");
  }

  StaticJsonDocument<256> resp;
  resp["success"] = ok;
  resp["message"] = ok ? "OTA update initiated" : "Failed to start OTA update";
  if (!ok && s_otaStatus.errorMessage.length() > 0) {
    resp["error"] = s_otaStatus.errorMessage;
  }

  String json;
  serializeJson(resp, json);
  server.send(ok ? 200 : 400, "application/json", json);
}

static void handleApiCheckUpdate(WebServer &server) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  bool ok = checkGitHubUpdate();
  OtaStatus st = getOtaStatus();

  StaticJsonDocument<512> doc;
  doc["success"] = ok;
  doc["currentVersion"] = st.currentVersion;
  doc["latestVersion"] = st.latestVersion;
  doc["hasUpdate"] = st.hasUpdate;
  doc["downloadUrl"] = st.downloadUrl;
  doc["releaseUrl"] = st.releaseUrl;
  doc["releaseNotes"] = st.releaseNotes;
  if (!ok) doc["error"] = st.errorMessage;

  String json;
  serializeJson(doc, json);
  server.send(ok ? 200 : 500, "application/json", json);
}

void setup_ota(WebServer &server) {
  server.on("/update", HTTP_GET, [&server]() {
    handleOtaPage(server);
  });

  server.on("/update", HTTP_POST, [&server]() {
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      server.send(500, "text/plain", "Update Failed: " + String(Update.errorString()));
    } else {
      server.send(200, "text/html", F("<html><head><meta http-equiv='refresh' content='15;url=/'></head>"
                                      "<body style='font-family:sans-serif;background:#121212;color:#e0e0e0;padding:2em;text-align:center'>"
                                      "<h2>✅ Update Successful!</h2>"
                                      "<p>Firmware written successfully. Device is restarting...</p>"
                                      "<p>Redirecting in 15 seconds...</p>"
                                      "</body></html>"));
      delay(1000);
      ESP.restart();
    }
  }, [&server]() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA] Starting upload: %s\n", upload.filename.c_str());
      s_otaStatus.state = OTA_STATE_FLASHING;
      s_otaStatus.bytesWritten = 0;
      s_otaStatus.totalBytes = 0;
      s_otaStatus.progressPercent = 0;
      s_otaStatus.errorMessage = "";

      const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
      if (!update_partition) {
        s_otaStatus.state = OTA_STATE_ERROR;
        s_otaStatus.errorMessage = "No OTA partition found; device must be flashed via USB once.";
        Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
        Update.abort();
        return;
      }

      // Pause mining so SHA engine and flash cache are free
      g_otaUpdating = true;

      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        s_otaStatus.state = OTA_STATE_ERROR;
        s_otaStatus.errorMessage = "Update.begin failed: " + String(Update.errorString());
        Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
        g_otaUpdating = false;
      }
    }
    else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.isRunning()) {
        if (s_otaStatus.bytesWritten == 0 && upload.currentSize >= 16) {
          String valErr;
          if (!validateFirmwareHeader(upload.buf, upload.currentSize, valErr)) {
            s_otaStatus.state = OTA_STATE_ERROR;
            s_otaStatus.errorMessage = valErr;
            Serial.println("[OTA] Validation failed: " + valErr);
            Update.abort();
            g_otaUpdating = false;
            return;
          }
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          s_otaStatus.state = OTA_STATE_ERROR;
          s_otaStatus.errorMessage = "Write failed: " + String(Update.errorString());
          Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
          Update.abort();
          g_otaUpdating = false;
        } else {
          s_otaStatus.bytesWritten += upload.currentSize;
        }
      }
    }
    else if (upload.status == UPLOAD_FILE_END) {
      if (Update.isRunning()) {
        if (Update.end(true)) {
          s_otaStatus.state = OTA_STATE_SUCCESS;
          s_otaStatus.progressPercent = 100;
          Serial.printf("[OTA] Manual upload success! Total: %u bytes.\n", upload.totalSize);
        } else {
          s_otaStatus.state = OTA_STATE_ERROR;
          s_otaStatus.errorMessage = "Update.end failed: " + String(Update.errorString());
          Serial.println("[OTA] Error: " + s_otaStatus.errorMessage);
        }
      }
      g_otaUpdating = false;
    }
    else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      s_otaStatus.state = OTA_STATE_ERROR;
      s_otaStatus.errorMessage = "Upload aborted by client";
      Serial.println(F("[OTA] Upload aborted"));
      g_otaUpdating = false;
    }
  });

  server.on("/api/system/update", HTTP_GET, [&server]() {
    handleApiGetUpdate(server);
  });

  server.on("/api/system/update", HTTP_POST, [&server]() {
    handleApiPostUpdate(server);
  });

  server.on("/api/system/update/check", HTTP_POST, [&server]() {
    handleApiCheckUpdate(server);
  });

  server.on("/api/system/update/check", HTTP_GET, [&server]() {
    handleApiCheckUpdate(server);
  });
}
