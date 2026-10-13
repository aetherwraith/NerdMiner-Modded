#include "serialCli.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <vector>

#include "drivers/storage/storage.h"
#include "drivers/storage/nvMemory.h"
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

static TaskHandle_t s_cliTaskHandle = NULL;
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
  Serial.println(F("  status                      - Display miner status, hashrate, IP, uptime, heap"));
  Serial.println(F("  probe                       - Print compact JSON probe for automated tooling"));
  Serial.println(F("  config [get]                - Display current configuration in JSON format"));
  Serial.println(F("  timezones [query]           - List supported timezones (optional search filter)"));
  Serial.println();
  Serial.println(F("Configuration Commands:"));
  Serial.println(F("  set wifi <ssid> <pass>      - Configure WiFi network credentials and reboot"));
  Serial.println(F("  set pool <host> [port]      - Set stratum mining pool host & port (applied live)"));
  Serial.println(F("  set port <port>             - Set stratum mining pool port (applied live)"));
  Serial.println(F("  set wallet <address>        - Set BTC payout address / worker (applied live)"));
  Serial.println(F("  set pass <password>         - Set stratum pool password (default 'x')"));
  Serial.println(F("  set tz <timezone>           - Set timezone (e.g. Europe/London or POSIX string)"));
  Serial.println(F("  set currency <currency>     - Set fiat price currency (e.g. usd, eur, gbp)"));
  Serial.println(F("  set brightness <0-255>      - Set screen backlight brightness (0-255)"));
  Serial.println(F("  set autobrightness <0|1>    - Enable (1) or disable (0) LDR auto-brightness"));
  Serial.println(F("  set timeout <minutes>       - Backlight idle timeout (0 = never off)"));
  Serial.println(F("  set nightdim <0|1>          - Enable (1) or disable (0) sunset/sunrise night dim"));
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

static void printStatus(void)
{
  char best_diff_str[16] = {0};
  suffix_string(best_diff, best_diff_str, sizeof(best_diff_str), 0);
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
  Serial.printf("  State:         %s\n", statusStr);
  Serial.printf("  WiFi Status:   %s\n", WiFi.isConnected() ? "Connected" : "Disconnected / AP Mode");
  if (WiFi.isConnected()) {
    Serial.printf("  SSID / IP:     %s / %s (RSSI: %d dBm)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("  AP SSID:       %s (IP: %s)\n", Settings.WifiSSID.c_str(), WiFi.softAPIP().toString().c_str());
  }
  Serial.printf("  MAC Address:   %s\n", WiFi.macAddress().c_str());
  Serial.printf("  Hashrate:      %.2f KH/s\n", khs);
  Serial.printf("  Shares:        %u total, %u accepted, %u rejected (%.2f%% rejects)\n",
                shares, valids, rejects, shares > 0 ? ((float)rejects / (float)shares * 100.0f) : 0.0f);
  Serial.printf("  Best Diff:     %s\n", best_diff_str);
  Serial.printf("  Stratum Pool:  %s:%d (User: %s)\n", Settings.PoolAddress.c_str(), Settings.PoolPort, Settings.BtcWallet);
  Serial.printf("  Uptime:        %lud %02luh %02lum %02lus\n", days, hours, mins, s);
  Serial.printf("  Free Heap:     %u bytes (min: %u bytes)\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  Serial.printf("  Internal Temp: %.1f °C\n", temperatureRead());
  Serial.printf("  Timezone:      %s\n", Settings.Timezone.c_str());
  Serial.printf("  Screen:        Brightness %d, AutoBrightness %s, Timeout %d min\n",
                Settings.Brightness, Settings.autoBrightness ? "ON" : "OFF", Settings.ScreenTimeout);
  Serial.println(F("=================================================="));
}

static void printProbe(void)
{
  char best_diff_str[16] = {0};
  suffix_string(best_diff, best_diff_str, sizeof(best_diff_str), 0);
  double khs = getHashrateKhs();
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "0.0.0.0";
  const char* statusStr = (mMonitor.NerdStatus == NM_hashing) ? "Mining" :
                          (mMonitor.NerdStatus == NM_Connecting) ? "Connecting" : "WaitConfig";

  Serial.printf("{\"version\":\"%s\",\"chip\":\"%s\",\"ip\":\"%s\",\"hashrate\":%.2f,\"status\":\"%s\",\"heap\":%u,\"bestDiff\":\"%s\",\"temp\":%.1f}\n",
                CURRENT_VERSION, ESP.getChipModel(), ip.c_str(), khs, statusStr, ESP.getFreeHeap(), best_diff_str, temperatureRead());
}

static void printConfig(void)
{
  const TSettings s = currentSettings();
  const TimeZoneInfo* tzInfo = findTimeZone(s.Timezone);
  String tzPosix = getPosixTz(s.Timezone);
  String tzLabel = tzInfo ? String(tzInfo->label) : s.Timezone;

  DynamicJsonDocument doc(1024);
  doc["pool"] = s.PoolAddress;
  doc["port"] = s.PoolPort;
  doc["wallet"] = s.BtcWallet;
  doc["poolPassword"] = s.PoolPassword;
  doc["currency"] = s.Currency;
  doc["timezone"] = s.Timezone;
  doc["timezoneLabel"] = tzLabel;
  doc["timezonePosix"] = tzPosix;
  doc["saveStats"] = s.saveStats;
  doc["brightness"] = s.Brightness;
  doc["autoBrightness"] = s.autoBrightness;
  doc["screenTimeout"] = s.ScreenTimeout;
  doc["nightDim"] = s.nightDim;
  doc["nightBrightness"] = s.NightBrightness;
  doc["nightFade"] = s.NightFade;
  doc["latitude"] = s.Latitude;
  doc["longitude"] = s.Longitude;
  doc["invertColors"] = s.invertColors;
  doc["flipDisplay"] = s.flipDisplay;
#ifdef NO_DISPLAY
  doc["statusLed"] = s.statusLed;
#endif
  doc["version"] = CURRENT_VERSION;

  serializeJsonPretty(doc, Serial);
  Serial.println();
}

static void handleConfigJson(const String &jsonStr)
{
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.printf("[ERROR] Failed to parse JSON: %s\n", err.c_str());
    return;
  }

  const TSettings before = currentSettings();
  TSettings next = before;
  nvMemory::parseJsonConfig(doc, &next);

  if (doc.containsKey("currency")) {
    next.Currency = currencyFor(doc["currency"].as<String>()).code;
  }

  next.Brightness = constrain(next.Brightness, 0, 255);
  next.NightBrightness = constrain(next.NightBrightness, 1, 255);
  next.NightFade = constrain(next.NightFade, 1, 240);
  next.ScreenTimeout = constrain(next.ScreenTimeout, 0, 1440);
  next.Latitude = constrain(next.Latitude, -90.0f, 90.0f);
  next.Longitude = constrain(next.Longitude, -180.0f, 180.0f);

  bool wifiChanged = (next.WifiSSID != before.WifiSSID || next.WifiPW != before.WifiPW);
  if (wifiChanged && next.WifiSSID.length() > 0) {
    WiFi.begin(next.WifiSSID.c_str(), next.WifiPW.c_str());
  }

  bool poolChanged = false;
  bool restart = applyLiveSettings(before, next, poolChanged);

  if (wifiChanged) restart = true;

  if (restart) {
    Serial.println(F("{\"success\":true,\"message\":\"Settings applied. Restarting device for changes...\",\"rebootRequired\":true}"));
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
    printStatus();
  }
  else if (cmd == "probe") {
    printProbe();
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
    saveSettingsToFlash(&Settings);
    Serial.println(F("[OK] Settings saved to SPIFFS (/config.json)."));
  }
  else if (cmd == "reset") {
    Serial.println(F("[RESET] Erasing configuration and restarting..."));
    delay(1000);
    reset_configuration();
  }
  else if (cmd == "restart" || cmd == "reboot") {
    Serial.println(F("[REBOOT] Restarting device..."));
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
      String ssid = args[2];
      String pass = args[3];
      Settings.WifiSSID = ssid;
      Settings.WifiPW = pass;
      saveSettingsToFlash(&Settings);
      WiFi.begin(ssid.c_str(), pass.c_str());
      Serial.printf("[OK] WiFi configured for '%s'. Restarting in 1s to associate...\n", ssid.c_str());
      delay(1000);
      ESP.restart();
    }
    else if (key == "pool" || key == "stratum") {
      String host = args[2];
      int port = (args.size() >= 4) ? args[3].toInt() : Settings.PoolPort;
      if (port <= 0) port = 3333;

      const TSettings before = currentSettings();
      TSettings next = before;
      next.PoolAddress = host;
      next.PoolPort = port;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Stratum pool set to %s:%d (applied live)\n", host.c_str(), port);
    }
    else if (key == "port") {
      int port = args[2].toInt();
      if (port <= 0) {
        Serial.println(F("[ERROR] Invalid port number"));
        return;
      }
      const TSettings before = currentSettings();
      TSettings next = before;
      next.PoolPort = port;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Stratum port set to %d (applied live)\n", port);
    }
    else if (key == "wallet" || key == "user" || key == "btc") {
      const TSettings before = currentSettings();
      TSettings next = before;
      strncpy(next.BtcWallet, args[2].c_str(), sizeof(next.BtcWallet) - 1);
      next.BtcWallet[sizeof(next.BtcWallet) - 1] = '\0';
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Wallet payout address set to %s (applied live)\n", next.BtcWallet);
    }
    else if (key == "pass" || key == "poolpass" || key == "password") {
      const TSettings before = currentSettings();
      TSettings next = before;
      strncpy(next.PoolPassword, args[2].c_str(), sizeof(next.PoolPassword) - 1);
      next.PoolPassword[sizeof(next.PoolPassword) - 1] = '\0';
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Pool password set to '%s' (applied live)\n", next.PoolPassword);
    }
    else if (key == "tz" || key == "timezone") {
      String tz = args[2];
      const TSettings before = currentSettings();
      TSettings next = before;
      next.Timezone = tz;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      String posix = getPosixTz(tz);
      setenv("TZ", posix.c_str(), 1);
      tzset();
      const TimeZoneInfo* info = findTimeZone(tz);
      Serial.printf("[OK] Timezone set to '%s' (%s, POSIX: %s)\n", tz.c_str(), info ? info->label : tz.c_str(), posix.c_str());
    }
    else if (key == "currency" || key == "cur") {
      String cur = currencyFor(args[2]).code;
      const TSettings before = currentSettings();
      TSettings next = before;
      next.Currency = cur;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Currency set to '%s' (applied live)\n", cur.c_str());
    }
    else if (key == "brightness" || key == "bright") {
      int b = constrain(args[2].toInt(), 0, 255);
      const TSettings before = currentSettings();
      TSettings next = before;
      next.Brightness = b;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Brightness set to %d\n", b);
    }
    else if (key == "autobrightness" || key == "autobright") {
      bool ab = (args[2] == "1" || args[2].equalsIgnoreCase("true") || args[2].equalsIgnoreCase("on"));
      const TSettings before = currentSettings();
      TSettings next = before;
      next.autoBrightness = ab;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Auto-brightness %s\n", ab ? "enabled" : "disabled");
    }
    else if (key == "timeout" || key == "screenoff") {
      int t = constrain(args[2].toInt(), 0, 1440);
      const TSettings before = currentSettings();
      TSettings next = before;
      next.ScreenTimeout = t;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Screen timeout set to %d min\n", t);
    }
    else if (key == "nightdim") {
      bool nd = (args[2] == "1" || args[2].equalsIgnoreCase("true") || args[2].equalsIgnoreCase("on"));
      const TSettings before = currentSettings();
      TSettings next = before;
      next.nightDim = nd;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
      Serial.printf("[OK] Night dimming %s\n", nd ? "enabled" : "disabled");
    }
    else if (key == "invert") {
      bool inv = (args[2] == "1" || args[2].equalsIgnoreCase("true") || args[2].equalsIgnoreCase("on"));
      const TSettings before = currentSettings();
      TSettings next = before;
      next.invertColors = inv;
      saveSettingsToFlash(&next);
      Serial.println(F("[OK] Invert display saved. Restarting in 1s to apply..."));
      delay(1000);
      ESP.restart();
    }
    else if (key == "flip") {
      bool fl = (args[2] == "1" || args[2].equalsIgnoreCase("true") || args[2].equalsIgnoreCase("on"));
      const TSettings before = currentSettings();
      TSettings next = before;
      next.flipDisplay = fl;
      saveSettingsToFlash(&next);
      Serial.println(F("[OK] Flip display saved. Restarting in 1s to apply..."));
      delay(1000);
      ESP.restart();
    }
    else {
      Serial.printf("[ERROR] Unknown setting key '%s'. Type 'help' for available keys.\n", key.c_str());
    }
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
  Serial.println(F("\n[CLI] NerdMiner USB Serial CLI active (115200 baud). Type 'help' for commands."));
  Serial.print(F("NerdMiner> "));
  while (true) {
    serialCliProcess();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void setup_serialCli(void)
{
  if (s_cliTaskHandle == NULL) {
    xTaskCreatePinnedToCore(serialCliTask, "SerialCLI", 10240, NULL, 2, &s_cliTaskHandle, 0);
  }
}
