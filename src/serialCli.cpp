#include "serialCli.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <initializer_list>
#include <vector>

#include "drivers/storage/storage.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/displays/display.h"
#include "wManager.h"
#include "webConfig.h"
#include "mining.h"
#include "monitor.h"
#include "timezone.h"
#include "version.h"
#include "currency.h"
#include "timeconst.h"
#include "utils.h"
#include "ota.h"

extern TSettings Settings;
extern monitor_data mMonitor;
extern double best_diff;
extern volatile uint32_t shares;
extern volatile uint32_t valids;
extern volatile uint32_t rejects;
extern uint64_t upTime;

static TaskHandle_t s_cliTaskHandle = nullptr;
static volatile bool s_cliLoopMode = false;
static char s_lineBuf[1024];
static size_t s_lineLen = 0;
static bool s_prevCharWasCr = false;

// Tokenize a command line into arguments, respecting single and double quotes.
static std::vector<String> tokenize(const String &line)
{
  std::vector<String> tokens;
  String current;
  bool inQuotes = false;
  char quoteChar = 0;

  for (size_t i = 0; i < line.length(); i++) {
    char c = line[i];
    if (inQuotes) {
      if (c == quoteChar) {
        inQuotes = false;
      } else {
        current += c;
      }
    } else {
      if (c == '"' || c == '\'') {
        inQuotes = true;
        quoteChar = c;
      } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (current.length() > 0) {
          tokens.push_back(current);
          current = "";
        }
      } else {
        current += c;
      }
    }
  }
  if (current.length() > 0) {
    tokens.push_back(current);
  }
  return tokens;
}

static void printHelp(void)
{
  Serial.println(F("\n================ NerdMiner USB Serial CLI ================"));
  Serial.println(F("Status & Inspection:"));
  Serial.println(F("  status                      - Display miner status, hashrate, ping, IP, heap"));
  Serial.println(F("  probe                       - Print compact JSON probe for automated tooling"));
  Serial.println(F("  identify [seconds]          - Flash screen & LED to physically locate miner"));
  Serial.println(F("  config [get]                - Display current configuration in JSON format"));
  Serial.println(F("  timezones [query]           - List supported timezones (optional search filter)"));
  Serial.println();
  Serial.println(F("Configuration Commands:"));
  Serial.println(F("  set wifi <ssid> <pass>      - Configure WiFi network credentials and reboot"));
  Serial.println(F("  set pool <host> [port]      - Set stratum mining pool host & port (applied live)"));
  Serial.println(F("  set port <port>             - Set stratum mining pool port (applied live)"));
  Serial.println(F("  set wallet <address>        - Set BTC payout address / worker (applied live)"));
  Serial.println(F("  set pass <password>         - Set stratum pool password (default 'x')"));
  Serial.println(F("  set bpool <host> [port]     - Set backup pool host & port (applied live)"));
  Serial.println(F("  set bport <port>            - Set backup pool port (applied live)"));
  Serial.println(F("  set bpass <password>        - Set backup pool password (applied live)"));
  Serial.println(F("  set hostname <name>         - Set device network/DHCP hostname (applied live)"));
  Serial.println(F("  set tz <timezone>           - Set timezone (e.g. Europe/London or POSIX string)"));
  Serial.println(F("  set currency <currency>     - Set fiat price currency (e.g. usd, eur, gbp)"));
  Serial.println(F("  set savestats <0|1>         - Enable (1) or disable (0) saving stats to flash"));
  Serial.println(F("  set brightness <0-255>      - Set screen backlight brightness (0-255)"));
  Serial.println(F("  set autobrightness <0|1>    - Enable (1) or disable (0) LDR auto-brightness"));
  Serial.println(F("  set timeout <minutes>       - Backlight idle timeout (0 = never off)"));
  Serial.println(F("  set nightdim <0|1>          - Enable (1) or disable (0) sunset/sunrise night dim"));
  Serial.println(F("  set nightbright <1-255>     - Set night dim target brightness (1-255)"));
  Serial.println(F("  set nightfade <1-240>       - Set night dim fade duration in minutes (1-240)"));
  Serial.println(F("  set city <name>             - Set location by city name for sunset/sunrise"));
  Serial.println(F("  set lat <deg> / lon <deg>   - Set latitude / longitude in decimal degrees"));
  Serial.println(F("  set ledmode <off|status|on> - Set rear RGB LED mode (0=off, 1=status, 2=steady)"));
  Serial.println(F("  set ledcolor <#rrggbb|name> - Set rear RGB LED color (e.g. #ff0000 or red)"));
  Serial.println(F("  set ledbright <1-255>       - Set rear RGB LED brightness (1-255)"));
  Serial.println(F("  set ledscreen <0|1>         - Turn LED off when screen is off (0|1)"));
  Serial.println(F("  set mqtt <0|1>              - Enable (1) or disable (0) MQTT publishing"));
  Serial.println(F("  set mqtthost <host>         - Set MQTT broker address (IP or hostname)"));
  Serial.println(F("  set mqttport <port>         - Set MQTT broker port (default 1883)"));
  Serial.println(F("  set mqttuser <user>         - Set MQTT broker username"));
  Serial.println(F("  set mqttpass <pass>         - Set MQTT broker password"));
  Serial.println(F("  set mqtttopic <topic>       - Set MQTT topic prefix"));
  Serial.println(F("  set webpass <password>      - Set Web UI admin password"));
  Serial.println(F("  set invert <0|1>            - Invert display colors (reboot required)"));
  Serial.println(F("  set flip <0|1>              - Flip display 180 deg (reboot required)"));
  Serial.println(F("  config json <json_payload>  - Apply entire configuration from JSON payload"));
  Serial.println();
  Serial.println(F("  ota status                  - Show OTA partitions and update status"));
  Serial.println(F("  ota check                   - Check GitHub Releases for new version"));
  Serial.println(F("  ota update [url|version]    - Download and flash OTA update in background"));
  Serial.println(F("  save                        - Persist current settings to SPIFFS (/config.json)"));
  Serial.println(F("  reset                       - Factory reset (erase config & WiFi credentials)"));
  Serial.println(F("  restart | reboot            - Restart the device"));
  Serial.println(F("  help                        - Show this command reference"));
  Serial.println(F("Note: 'config set <key> <val>' is supported as an alias for 'set <key> <val>'."));
  Serial.println(F("=========================================================="));
}

static void cmdShowStatus(void)
{
  const TSettings cfg = currentSettings();
  const MinerStatus m = getMinerStatus();
  char best_diff_str[16] = {0};
  char sess_diff_str[16] = {0};
  suffix_string(m.bestDiff, best_diff_str, sizeof(best_diff_str), 0);
  suffix_string(m.sessionBestDiff, sess_diff_str, sizeof(sess_diff_str), 0);
  double khs = getHashrateKhs();

  const char* statusStr = "Unknown";
  if (mMonitor.NerdStatus == NM_hashing) statusStr = "Mining";
  else if (mMonitor.NerdStatus == NM_Connecting) statusStr = "Connecting";
  else if (mMonitor.NerdStatus == NM_waitingConfig) statusStr = "Waiting Configuration / Setup AP";

  unsigned long secs = (unsigned long)upTime;
  unsigned long days = secs / 86400;
  unsigned long hours = (secs % 86400) / 3600;
  unsigned long mins = (secs % 3600) / 60;
  unsigned long s = secs % 60;

  Serial.println(F("\n================ NerdMiner Status ================"));
  Serial.printf("  Firmware:      %s (Chip: %s rev %d)\n", CURRENT_VERSION, ESP.getChipModel(), ESP.getChipRevision());
  Serial.printf("  Hostname:      %s%s\n", cfg.Hostname, isIdentifying() ? " [IDENTIFYING]" : "");
  Serial.printf("  State:         %s\n", statusStr);
  Serial.printf("  WiFi Status:   %s\n", WiFi.isConnected() ? "Connected" : "Disconnected / AP Mode");
  if (WiFi.isConnected()) {
    Serial.printf("  SSID / IP:     %s / %s (RSSI: %d dBm)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("  AP SSID:       %s (IP: %s)\n", cfg.WifiSSID.c_str(), WiFi.softAPIP().toString().c_str());
  }
  Serial.printf("  MAC Address:   %s\n", WiFi.macAddress().c_str());
  Serial.printf("  Hashrate:      %.2f KH/s (avg: %.2f KH/s)\n", khs, m.avgHashrateKHs);
  Serial.printf("  Shares:        %u 32-bit, %u accepted, %u rejected, %u blocks\n",
                shares, m.accepted, m.rejected, valids);
  Serial.printf("  Best Diff:     %s all-time / %s session (Pool Diff: %.5g)\n",
                best_diff_str, sess_diff_str, m.poolDifficulty);
  Serial.printf("  Active Pool:   %s:%d (%s, User: %s)\n",
                activePoolAddress().c_str(), activePoolPort(),
                onBackupPool() ? "BACKUP" : "PRIMARY", cfg.BtcWallet);
  if (m.pingMs >= 0) {
    Serial.printf("  Pool Ping:     %ld ms\n", (long)m.pingMs);
  } else {
    Serial.println(F("  Pool Ping:     N/A (awaiting share ACK)"));
  }
  if (!cfg.BackupPoolAddress.isEmpty()) {
    Serial.printf("  Backup Pool:   %s:%d\n", cfg.BackupPoolAddress.c_str(), cfg.BackupPoolPort);
  }
  Serial.printf("  Uptime:        %lud %02luh %02lum %02lus\n", days, hours, mins, s);
  Serial.printf("  Free Heap:     %u bytes (min: %u bytes)\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  Serial.printf("  Internal Temp: %.1f °C\n", temperatureRead());
  Serial.printf("  Timezone:      %s\n", cfg.Timezone.c_str());
  Serial.printf("  Screen:        Brightness %d, AutoBrightness %s, Timeout %d min\n",
                cfg.Brightness, cfg.autoBrightness ? "ON" : "OFF", cfg.ScreenTimeout);
  Serial.println(F("=================================================="));
}

static void printProbe(void)
{
  const TSettings cfg = currentSettings();
  const MinerStatus m = getMinerStatus();
  char best_diff_str[16] = {0};
  char sess_diff_str[16] = {0};
  suffix_string(m.bestDiff, best_diff_str, sizeof(best_diff_str), 0);
  suffix_string(m.sessionBestDiff, sess_diff_str, sizeof(sess_diff_str), 0);
  double khs = getHashrateKhs();
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "0.0.0.0";
  const char* statusStr = (mMonitor.NerdStatus == NM_hashing) ? "Mining" :
                          (mMonitor.NerdStatus == NM_Connecting) ? "Connecting" : "WaitConfig";

  Serial.printf("{\"version\":\"%s\",\"chip\":\"%s\",\"host\":\"%s\",\"ip\":\"%s\",\"hashrate\":%.2f,\"status\":\"%s\",\"heap\":%u,\"bestDiff\":\"%s\",\"sessionBestDiff\":\"%s\",\"pingMs\":%ld,\"temp\":%.1f}\n",
                CURRENT_VERSION, ESP.getChipModel(), jsonEscape(String(cfg.Hostname)).c_str(), ip.c_str(), khs, statusStr, ESP.getFreeHeap(), best_diff_str, sess_diff_str, (long)m.pingMs, temperatureRead());
}

static void printConfig(void)
{
  const TSettings s = currentSettings();
  const TimeZoneInfo* tzInfo = findTimeZone(s.Timezone);

  DynamicJsonDocument doc(2048);
  nvMemory::fillConfigJson(doc, s, false, true);
  doc["timezoneLabel"] = tzInfo ? String(tzInfo->label) : s.Timezone;
  doc["timezonePosix"] = getPosixTz(s.Timezone);
  doc["version"] = CURRENT_VERSION;

  String out;
  out.reserve(1024);
  serializeJsonPretty(doc, out);
  Serial.println(out);
}

static void handleConfigJson(const String &jsonStr)
{
  DynamicJsonDocument doc(1536);
  DeserializationError err = deserializeJson(doc, jsonStr.c_str());
  if (err) {
    Serial.printf("[ERROR] Failed to parse JSON: %s\n", err.c_str());
    return;
  }

  const TSettings before = currentSettings();
  TSettings next = before;
  nvMemory::parseJsonConfig(doc, &next);

  bool poolChanged = false;
  bool restart = applyLiveSettings(before, next, poolChanged);

  if (restart) {
    Serial.println(F("{\"success\":true,\"message\":\"Settings applied. Restarting device for changes...\",\"rebootRequired\":true}"));
    saveStat();
    delay(1000);
    ESP.restart();
  } else {
    Serial.println(F("{\"success\":true,\"message\":\"Settings applied live successfully.\",\"rebootRequired\":false}"));
  }
}

static void handleTimezones(const std::vector<String> &args)
{
  String q = (args.size() >= 2) ? args[1] : "";
  q.toLowerCase();
  Serial.println(F("\nAvailable Timezones:"));
  int count = 0;
  for (int i = 0; i < kTimeZoneCount; i++) {
    String id = kTimeZones[i].id;
    String lbl = kTimeZones[i].label;
    String idLower = id;
    idLower.toLowerCase();
    String lblLower = lbl;
    lblLower.toLowerCase();
    if (q.length() == 0 || idLower.indexOf(q) >= 0 || lblLower.indexOf(q) >= 0) {
      Serial.printf("  %-30s - %s\n", kTimeZones[i].id, kTimeZones[i].label);
      count++;
    }
  }
  Serial.printf("Total matching: %d\n", count);
}

static bool isSupportedSetKey(const String &key)
{
  static const char *const kValidKeys[] = {
    "ssid", "wifissid", "wifipass", "wifipw", "wifipassword",
    "pool", "poolurl", "stratum", "port", "poolport",
    "wallet", "btcwallet", "user", "btc",
    "pass", "poolpass", "poolpassword", "password",
    "bpool", "backuppool", "bport", "backupport",
    "bpass", "bpoolpass", "backuppass", "backuppoolpassword",
    "hostname", "host", "tz", "timezone", "currency", "cur",
    "savestats", "stats", "brightness", "bright",
    "autobrightness", "autobright", "timeout", "screenoff", "screentimeout",
    "nightdim", "nightbright", "nightbrightness", "nightfade",
    "city", "lat", "latitude", "lon", "longitude",
    "led", "statusled", "ledmode", "mode", "ledcolor", "ledcolorhex", "color",
    "ledbright", "ledbrightness", "ledscreen", "ledscreenoff",
    "mqtt", "mqttenabled", "mqtthost", "mqhost", "mqttport", "mqport",
    "mqttuser", "mquser", "mqttpass", "mqpass", "mqtttopic", "mqtopic",
    "webpass", "webpassword", "invert", "invertcolors", "flip", "flipdisplay"
  };
  for (const char *k : kValidKeys) {
    if (key == k) return true;
  }
  return false;
}

static void cmdSetKey(const std::vector<String> &args)
{
  if (args.size() < 3) {
    Serial.println(F("[ERROR] Usage: set <key> <value>. Type 'help' for examples."));
    return;
  }

  String key = args[1];
  key.toLowerCase();

  if (key == "wifi") {
    if (args.size() < 4) {
      Serial.println(F("[ERROR] Usage: set wifi <ssid> <password>"));
      return;
    }
    const TSettings before = currentSettings();
    TSettings next = before;
    StaticJsonDocument<384> doc;
    doc["ssid"] = args[2].c_str();
    doc["wifiPass"] = args[3].c_str();
    nvMemory::parseJsonConfig(doc, &next);
    bool poolChanged = false;
    applyLiveSettings(before, next, poolChanged);
    Serial.printf("[OK] WiFi configured for '%s'. Restarting in 1s to associate...\n", next.WifiSSID.c_str());
    saveStat();
    delay(1000);
    ESP.restart();
    return;
  }

  if (!isSupportedSetKey(key)) {
    Serial.printf("[ERROR] Unknown setting key '%s'. Type 'help' for available keys.\n", key.c_str());
    return;
  }

  if ((key == "port" || key == "poolport") && args[2].toInt() <= 0) {
    Serial.println(F("[ERROR] Invalid port number"));
    return;
  }

  const TSettings before = currentSettings();
  TSettings next = before;
  StaticJsonDocument<384> doc;
  doc[key.c_str()] = args[2].c_str();

  if ((key == "pool" || key == "poolurl" || key == "stratum") && args.size() >= 4 && args[3].toInt() > 0) {
    doc["port"] = args[3].toInt();
  } else if ((key == "bpool" || key == "backuppool") && args.size() >= 4 && args[3].toInt() > 0) {
    doc["backupPort"] = args[3].toInt();
  }

  nvMemory::parseJsonConfig(doc, &next);
  bool poolChanged = false;
  bool restart = applyLiveSettings(before, next, poolChanged);

  if (restart) {
    Serial.printf("[OK] Setting '%s' saved. Restarting in 1s to apply...\n", key.c_str());
    saveStat();
    delay(1000);
    ESP.restart();
    return;
  }

  Serial.printf("[OK] Setting '%s' updated to '%s' (applied live)\n", key.c_str(), args[2].c_str());
}

static void executeCommand(const String &rawLine)
{
  String line = rawLine;
  line.trim();
  if (line.length() == 0) return;

  std::vector<String> args = tokenize(line);
  if (args.empty()) return;

  // Support "config set <key> <val...>" alias -> normalize to "set <key> <val...>"
  if (args.size() >= 3 && args[0].equalsIgnoreCase("config") && args[1].equalsIgnoreCase("set")) {
    args.erase(args.begin()); // remove "config"
  }

  String cmd = args[0];
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    printHelp();
  }
  else if (cmd == "status") {
    cmdShowStatus();
  }
  else if (cmd == "probe") {
    printProbe();
  }
  else if (cmd == "identify" || cmd == "locate") {
    uint32_t durationMs = 10000;
    if (args.size() >= 2) {
      long v = args[1].toInt();
      if (args[1].equalsIgnoreCase("off") || args[1] == "0") durationMs = 0;
      else if (v > 0 && v <= 120) durationMs = (uint32_t)v * 1000U;
      else if (v > 120) durationMs = (uint32_t)constrain(v, 500L, 120000L);
    }
    triggerIdentify(durationMs);
    if (durationMs > 0) {
      Serial.printf("[OK] Identify locator active for %lu ms.\n", (unsigned long)durationMs);
    } else {
      Serial.println(F("[OK] Identify locator stopped."));
    }
  }
  else if (cmd == "config") {
    if (args.size() >= 2 && args[1].equalsIgnoreCase("json")) {
      int idx = line.indexOf("json");
      String jsonStr = line.substring(idx + 4);
      jsonStr.trim();
      handleConfigJson(jsonStr);
    } else if (args.size() >= 2 && args[1].equalsIgnoreCase("reset")) {
      Serial.println(F("[RESET] Erasing configuration and restarting..."));
      delay(1000);
      reset_configuration();
    } else {
      printConfig();
    }
  }
  else if (cmd == "timezones" || cmd == "tzlist") {
    handleTimezones(args);
  }
  else if (cmd == "save") {
    TSettings snap = currentSettings();
    saveSettingsToFlash(&snap);
    Serial.println(F("[OK] Settings saved to SPIFFS (/config.json)."));
  }
  else if (cmd == "reset") {
    Serial.println(F("[RESET] Erasing configuration and restarting..."));
    delay(1000);
    reset_configuration();
  }
  else if (cmd == "restart" || cmd == "reboot") {
    Serial.println(F("[REBOOT] Restarting device..."));
    saveStat();
    delay(500);
    ESP.restart();
  }
  else if (cmd == "ota") {
    String subcmd = (args.size() >= 2) ? args[1] : "status";
    subcmd.toLowerCase();

    if (subcmd == "status") {
      OtaStatus st = getOtaStatus();
      Serial.println(F("\n================ OTA Firmware Status ================"));
      Serial.printf("  Board Model:       %s\n", st.boardModel.c_str());
      Serial.printf("  Installed Version: %s\n", st.currentVersion.c_str());
      Serial.printf("  Active Partition:  %s\n", st.runningPartition.c_str());
      Serial.printf("  Target Partition:  %s\n", st.nextPartition.c_str());
      Serial.printf("  Dual-OTA Support:  %s\n", st.hasDualOta ? "Yes (Enabled)" : "No (USB Flash Required)");
      const char *stateStr = (st.state == OTA_STATE_IDLE) ? "Idle" :
                             (st.state == OTA_STATE_CHECKING) ? "Checking" :
                             (st.state == OTA_STATE_DOWNLOADING) ? "Downloading" :
                             (st.state == OTA_STATE_FLASHING) ? "Flashing" :
                             (st.state == OTA_STATE_SUCCESS) ? "Success" : "Error";
      Serial.printf("  Update State:      %s (%d%%)\n", stateStr, st.progressPercent);
      if (st.errorMessage.length() > 0) {
        Serial.printf("  Error:             %s\n", st.errorMessage.c_str());
      }
      Serial.println(F("====================================================="));
    }
    else if (subcmd == "check") {
      Serial.println(F("[OTA] Checking GitHub Releases for updates..."));
      if (checkGitHubUpdate()) {
        OtaStatus st = getOtaStatus();
        Serial.printf("[OTA] Latest version: %s\n", st.latestVersion.c_str());
        Serial.printf("[OTA] Has update:     %s\n", st.hasUpdate ? "YES" : "No (Already up to date)");
        if (st.downloadUrl.length() > 0) {
          Serial.printf("[OTA] Download URL:   %s\n", st.downloadUrl.c_str());
        }
      } else {
        Serial.println(F("[OTA] Update check failed or no releases found."));
      }
    }
    else if (subcmd == "update") {
      String target = (args.size() >= 3) ? args[2] : "latest";
      if (target.startsWith("http://") || target.startsWith("https://")) {
        Serial.printf("[OTA] Starting OTA from URL: %s\n", target.c_str());
        if (startUrlOta(target)) {
          Serial.println(F("[OTA] Background download started. Type 'ota status' to monitor progress."));
        } else {
          Serial.println(F("[ERROR] Failed to start URL OTA update."));
        }
      } else {
        Serial.println(F("[OTA] Starting OTA from GitHub Releases (latest)..."));
        if (startGitHubOta(target)) {
          Serial.println(F("[OTA] Background download started. Type 'ota status' to monitor progress."));
        } else {
          Serial.println(F("[ERROR] Failed to start GitHub OTA update."));
        }
      }
    }
    else {
      Serial.println(F("[ERROR] Usage: ota status | ota check | ota update [url|version]"));
    }
  }
  else if (cmd == "set") {
    cmdSetKey(args);
  }
  else {
    Serial.printf("[ERROR] Unknown command '%s'. Type 'help' for command list.\n", cmd.c_str());
  }
}

void serialCliProcess(void)
{
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;

    if (c == '\r' || c == '\n') {
      if (c == '\n' && s_prevCharWasCr) {
        s_prevCharWasCr = false;
        continue;
      }
      s_prevCharWasCr = (c == '\r');

      if (s_lineLen > 0) {
        s_lineBuf[s_lineLen] = '\0';
        Serial.println();
        String cmdLine = String(s_lineBuf);
        s_lineLen = 0;
        executeCommand(cmdLine);
      } else {
        Serial.println();
      }
      Serial.print(F("NerdMiner> "));
    }
    else if (c == '\b' || c == 0x7F) { // Backspace or DEL
      s_prevCharWasCr = false;
      if (s_lineLen > 0) {
        s_lineLen--;
        Serial.print(F("\b \b"));
      }
    }
    else if (c >= 32 && c <= 126) { // Printable characters
      s_prevCharWasCr = false;
      if (s_lineLen < sizeof(s_lineBuf) - 1) {
        s_lineBuf[s_lineLen++] = (char)c;
        Serial.write((char)c); // echo back to terminal
      }
    }
  }
}

static void serialCliTask(void *param)
{
  while (!s_cliLoopMode) {
    serialCliProcess();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  s_cliTaskHandle = nullptr;
  vTaskDelete(NULL);
}

void setup_serialCli(void)
{
  Serial.println(F("\n[CLI] NerdMiner USB Serial CLI active (115200 baud). Type 'help' for commands."));
  Serial.print(F("NerdMiner> "));
  if (!s_cliLoopMode && s_cliTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(serialCliTask, "SerialCLI", 4096, NULL, 2, &s_cliTaskHandle, 0);
  }
}

void serialCliEnterLoopMode(void)
{
  s_cliLoopMode = true;
}
