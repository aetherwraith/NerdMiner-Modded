#include "nvMemory.h"

#ifdef NVMEM_SPIFFS

#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <initializer_list>

#include "../devices/device.h"
#include "storage.h"
#include "../../currency.h"
#include "../../cities.h"

nvMemory::nvMemory() : Initialized_(false){};

// Written first, then renamed over JSON_CONFIG_FILE once complete: opening the
// config itself with "w" emptied it at once, so a power cut during a save left
// no settings at all (the miner came back in its setup portal, then mined to
// the default address).
#define JSON_CONFIG_TMP JSON_CONFIG_FILE ".tmp"

static String readFileToString(File &f)
{
    String out;
    out.reserve(640);
    char buf[128];
    size_t n = 0;
    while ((n = f.readBytes(buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        out += buf;
    }
    return out;
}

// The config as JSON on the serial log, without the passwords in it.
// Modifies json in-place (call after reading from or writing to json).
static void printConfigMasked(JsonDocument &json)
{
    for (const char *key : {JSON_SPIFFS_KEY_POOLPASS, JSON_SPIFFS_KEY_BACKUPPOOLPASS, JSON_SPIFFS_KEY_WEBPASSWORD,
                            JSON_SPIFFS_KEY_MQTTPASS, JSON_KEY_PASW, JSON_KEY_POOLPASS,
                            "wifiPass", "mqttPass", "webPass"})
    {
        const char *v = json[key].as<const char *>();
        if (v && *v)
            json[key] = "********";
    }
    String out;
    serializeJsonPretty(json, out);
    Serial.println(out);
}

nvMemory::~nvMemory()
{
    if (Initialized_)
        SPIFFS.end();
};

void nvMemory::fillConfigJson(JsonDocument &doc, const TSettings &s, bool spiffsKeys, bool maskSecrets)
{
    if (spiffsKeys)
    {
        doc[JSON_SPIFFS_KEY_POOLURL] = s.PoolAddress;
        doc[JSON_SPIFFS_KEY_POOLPORT] = s.PoolPort;
        doc[JSON_SPIFFS_KEY_POOLPASS] = (maskSecrets && s.PoolPassword[0] != '\0') ? "********" : s.PoolPassword;
        doc[JSON_SPIFFS_KEY_WALLETID] = s.BtcWallet;
        doc[JSON_SPIFFS_KEY_BACKUPPOOLURL] = s.BackupPoolAddress;
        doc[JSON_SPIFFS_KEY_BACKUPPOOLPORT] = s.BackupPoolPort;
        doc[JSON_SPIFFS_KEY_BACKUPPOOLPASS] = (maskSecrets && s.BackupPoolPassword[0] != '\0') ? "********" : s.BackupPoolPassword;
        doc[JSON_SPIFFS_KEY_WEBPASSWORD] = (maskSecrets && s.WebPassword.length() > 0) ? "********" : s.WebPassword.c_str();
        doc[JSON_SPIFFS_KEY_MQTTENABLED] = s.MqttEnabled;
        doc[JSON_SPIFFS_KEY_MQTTHOST] = s.MqttHost;
        doc[JSON_SPIFFS_KEY_MQTTPORT] = s.MqttPort;
        doc[JSON_SPIFFS_KEY_MQTTUSER] = s.MqttUser;
        doc[JSON_SPIFFS_KEY_MQTTPASS] = (maskSecrets && s.MqttPass.length() > 0) ? "********" : s.MqttPass.c_str();
        doc[JSON_SPIFFS_KEY_MQTTTOPIC] = s.MqttTopic;
        doc[JSON_SPIFFS_KEY_TIMEZONE] = s.Timezone;
        doc[JSON_SPIFFS_KEY_STATS2NV] = s.saveStats;
        doc[JSON_SPIFFS_KEY_INVCOLOR] = s.invertColors;
        doc[JSON_SPIFFS_KEY_BRIGHTNESS] = s.Brightness;
        doc[JSON_SPIFFS_KEY_FLIPDISPLAY] = s.flipDisplay;
        doc[JSON_SPIFFS_KEY_CURRENCY] = s.Currency;
        doc[JSON_SPIFFS_KEY_AUTOBRIGHTNESS] = s.autoBrightness;
        doc[JSON_SPIFFS_KEY_SCREENTIMEOUT] = s.ScreenTimeout;
        doc[JSON_SPIFFS_KEY_NIGHTDIM] = s.nightDim;
        doc[JSON_SPIFFS_KEY_NIGHTBRIGHTNESS] = s.NightBrightness;
        doc[JSON_SPIFFS_KEY_NIGHTFADE] = s.NightFade;
        doc[JSON_SPIFFS_KEY_LATITUDE] = s.Latitude;
        doc[JSON_SPIFFS_KEY_LONGITUDE] = s.Longitude;
        doc[JSON_SPIFFS_KEY_HOSTNAME] = s.Hostname;
#ifdef NO_DISPLAY
        doc[JSON_SPIFFS_KEY_STATUSLED] = s.statusLed;
#endif
#ifdef HAS_RGB_LED
        doc[JSON_SPIFFS_KEY_LEDMODE] = s.ledMode;
        doc[JSON_SPIFFS_KEY_LEDCOLOR] = s.ledColor;
        doc[JSON_SPIFFS_KEY_LEDBRIGHTNESS] = s.ledBrightness;
        doc[JSON_SPIFFS_KEY_LEDSCREENOFF] = s.ledScreenOff;
#endif
        if (s.WifiSSID.length() > 0 && s.WifiSSID != "NerdMinerAP" && s.WifiSSID != "HanSoloAP")
        {
            doc[JSON_KEY_SSID] = s.WifiSSID;
            doc[JSON_KEY_PASW] = (maskSecrets && s.WifiPW.length() > 0) ? "********" : s.WifiPW.c_str();
        }
        return;
    }

    doc["hostname"] = s.Hostname;
    doc["wifiSSID"] = s.WifiSSID;
    doc["wifiPass"] = maskSecrets ? (s.WifiPW.length() > 0 ? "********" : "") : s.WifiPW.c_str();
    doc["pool"] = s.PoolAddress;
    doc["port"] = s.PoolPort;
    doc["wallet"] = s.BtcWallet;
    doc["poolPassword"] = maskSecrets ? (s.PoolPassword[0] != '\0' ? "********" : "") : s.PoolPassword;
    doc["backupPool"] = s.BackupPoolAddress;
    doc["backupPort"] = s.BackupPoolPort;
    doc["backupPoolPassword"] = maskSecrets ? (s.BackupPoolPassword[0] != '\0' ? "********" : "") : s.BackupPoolPassword;
    doc["currency"] = s.Currency;
    doc["timezone"] = s.Timezone;
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
    doc["saveStats"] = s.saveStats;
    doc["ledMode"] = s.ledMode;
    doc["ledColor"] = s.ledColor;
    char hexBuf[8];
    snprintf(hexBuf, sizeof(hexBuf), "#%06lx", (unsigned long)(s.ledColor & 0xFFFFFF));
    doc["ledColorHex"] = hexBuf;
    doc["ledBrightness"] = s.ledBrightness;
    doc["ledScreenOff"] = s.ledScreenOff;
    doc["statusLed"] = s.statusLed;
    doc["mqttEnabled"] = s.MqttEnabled;
    doc["mqttHost"] = s.MqttHost;
    doc["mqttPort"] = s.MqttPort;
    doc["mqttUser"] = s.MqttUser;
    doc["mqttPass"] = maskSecrets ? (s.MqttPass.length() > 0 ? "********" : "") : s.MqttPass.c_str();
    doc["mqttTopic"] = s.MqttTopic;
}

/// @brief Save settings to config file on SPIFFS
/// @param TSettings* Settings to be saved.
/// @return true on success
bool nvMemory::saveConfig(TSettings* Settings)
{
    if (init())
    {
        // Save Config in JSON format
        Serial.println(F("SPIFS: Saving configuration."));

        // Create a JSON document (on the heap: it has outgrown a small stack)
        DynamicJsonDocument json(3072); // room for the MQTT fields too
        fillConfigJson(json, *Settings, true, false);

        // Into the temporary file first
        File configFile = SPIFFS.open(JSON_CONFIG_TMP, "w");
        if (!configFile)
        {
            // Error, file did not open
            Serial.println("SPIFS: Failed to open config file for writing");
            return false;
        }
        String raw;
        raw.reserve(768);
        serializeJson(json, raw);
        const size_t expected = raw.length();
        const size_t written = configFile.write((const uint8_t *)raw.c_str(), expected);
        configFile.close();
        printConfigMasked(json);
        if (written == 0 || written != expected)
        {
            // Error writing file (full?): the saved config stays as it was
            Serial.printf("SPIFS: Failed to write to file (%u of %u bytes)\n", (unsigned)written, (unsigned)expected);
            SPIFFS.remove(JSON_CONFIG_TMP);
            return false;
        }
        // ...then in place of the old one. SPIFFS can't rename over an
        // existing file; a power cut between the two steps leaves only the
        // complete temporary file, which loadConfig() picks up.
        if (SPIFFS.exists(JSON_CONFIG_FILE) && !SPIFFS.remove(JSON_CONFIG_FILE))
        {
            Serial.println("SPIFS: Failed to replace the config file");
            return false;
        }
        if (!SPIFFS.rename(JSON_CONFIG_TMP, JSON_CONFIG_FILE))
        {
            Serial.println("SPIFS: Failed to rename the new config file");
            return false;
        }
        return true;
    };
    return false;
}

static bool hasAnyKey(const JsonDocument& doc, std::initializer_list<const char*> keys, const char** matchedOut = nullptr)
{
    for (const char* k : keys) {
        if (k && doc.containsKey(k)) {
            if (matchedOut) *matchedOut = k;
            return true;
        }
    }
    return false;
}

static String getJsonString(const JsonDocument& doc, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        if (k && doc.containsKey(k)) return doc[k].as<String>();
    }
    return "";
}

static int getJsonInt(const JsonDocument& doc, int defaultVal, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        if (k && doc.containsKey(k)) {
            JsonVariantConst v = doc[k];
            if (v.is<const char*>()) return (int)String(v.as<const char*>()).toInt();
            return v.as<int>();
        }
    }
    return defaultVal;
}

static float getJsonFloat(const JsonDocument& doc, float defaultVal, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        if (k && doc.containsKey(k)) {
            JsonVariantConst v = doc[k];
            if (v.is<const char*>()) return String(v.as<const char*>()).toFloat();
            return v.as<float>();
        }
    }
    return defaultVal;
}

static bool getJsonBool(const JsonDocument& doc, bool defaultVal, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        if (k && doc.containsKey(k)) {
            JsonVariantConst v = doc[k];
            if (v.is<bool>()) return v.as<bool>();
            if (v.is<int>() || v.is<long>() || v.is<unsigned int>()) return v.as<long>() != 0;
            if (v.is<const char*>()) {
                String s = v.as<String>();
                s.trim();
                if (s == "1" || s.equalsIgnoreCase("true") || s.equalsIgnoreCase("on") || s.equalsIgnoreCase("yes"))
                    return true;
                if (s == "0" || s.equalsIgnoreCase("false") || s.equalsIgnoreCase("off") || s.equalsIgnoreCase("no") || s.isEmpty())
                    return false;
            }
            return v.as<bool>();
        }
    }
    return defaultVal;
}

static bool isMaskedSecret(const String &s)
{
    return s == "********" || s == "***";
}

bool nvMemory::parseJsonConfig(const JsonDocument& json, TSettings* Settings)
{
    if (!Settings) return false;

    // WiFi credentials (supported from SDCard, SPIFFS, CLI, or API)
    const bool hasWifiSsidKey = hasAnyKey(json, {JSON_KEY_SSID, "ssid", "wifiSSID", "wifissid", "wifi_ssid"});
    String ssid = getJsonString(json, {JSON_KEY_SSID, "ssid", "wifiSSID", "wifissid", "wifi_ssid"});
    ssid.trim();
    if (ssid.length() > 0) Settings->WifiSSID = ssid;

    String pw = getJsonString(json, {JSON_KEY_PASW, "wifiPW", "wifipw", "wifiPass", "wifipass", "wifiPassword", "wifi_pass"});
    if (pw.isEmpty() && hasWifiSsidKey && json.containsKey("password")) {
        pw = json["password"].as<String>();
    }
    if (pw.length() > 0 && !isMaskedSecret(pw)) Settings->WifiPW = pw;

    // Pool connection (supports "stratum+tcp://host:port" or "host" + "port")
    String pool = getJsonString(json, {JSON_SPIFFS_KEY_POOLURL, JSON_KEY_POOLURL, "pool", "poolUrl", "poolurl", "stratum", "stratumURL", "PrimaryPool"});
    pool.trim();
    if (pool.length() > 0) {
        if (pool.startsWith("stratum+tcp://")) pool = pool.substring(14);
        else if (pool.startsWith("stratum://")) pool = pool.substring(10);
        else if (pool.startsWith("tcp://")) pool = pool.substring(6);
        int colonIdx = pool.lastIndexOf(':');
        if (colonIdx > 0) {
            int parsedPort = pool.substring(colonIdx + 1).toInt();
            if (parsedPort > 0) Settings->PoolPort = parsedPort;
            pool = pool.substring(0, colonIdx);
        }
        Settings->PoolAddress = pool;
    }

    Settings->PoolPort = constrain(getJsonInt(json, Settings->PoolPort, {JSON_SPIFFS_KEY_POOLPORT, JSON_KEY_POOLPORT, "port", "poolPort", "poolport", "stratumPort"}), 0, 65535);

    // Pool credentials
    const char* passKey = nullptr;
    if (hasAnyKey(json, {JSON_SPIFFS_KEY_POOLPASS, JSON_KEY_POOLPASS, "poolPass", "poolpass", "pass", "stratumPassword", "PrimaryPassword"}, &passKey) ||
        (!hasWifiSsidKey && hasAnyKey(json, {"password"}, &passKey))) {
        String pass = json[passKey].as<String>();
        if (!isMaskedSecret(pass)) {
            strncpy(Settings->PoolPassword, pass.c_str(), sizeof(Settings->PoolPassword) - 1);
            Settings->PoolPassword[sizeof(Settings->PoolPassword) - 1] = '\0';
        }
    }

    String wallet = getJsonString(json, {JSON_SPIFFS_KEY_WALLETID, JSON_KEY_WALLETID, "wallet", "btcWallet", "btcwallet", "user", "btc", "stratumUser", "PrimaryAddress"});
    wallet.trim();
    if (wallet.length() > 0) {
        strncpy(Settings->BtcWallet, wallet.c_str(), sizeof(Settings->BtcWallet) - 1);
        Settings->BtcWallet[sizeof(Settings->BtcWallet) - 1] = '\0';
    }

    // Backup Pool
    const char* bpoolKey = nullptr;
    if (hasAnyKey(json, {JSON_SPIFFS_KEY_BACKUPPOOLURL, "backupPool", "backuppool", "bpool", "fallbackStratumURL", "SecondaryPool"}, &bpoolKey)) {
        String bpool = json[bpoolKey].as<String>();
        bpool.trim();
        if (bpool.startsWith("stratum+tcp://")) bpool = bpool.substring(14);
        else if (bpool.startsWith("stratum://")) bpool = bpool.substring(10);
        else if (bpool.startsWith("tcp://")) bpool = bpool.substring(6);
        int colonIdx = bpool.lastIndexOf(':');
        if (colonIdx > 0) {
            int parsedPort = bpool.substring(colonIdx + 1).toInt();
            if (parsedPort > 0) Settings->BackupPoolPort = parsedPort;
            bpool = bpool.substring(0, colonIdx);
        }
        Settings->BackupPoolAddress = bpool;
    }
    Settings->BackupPoolPort = constrain(getJsonInt(json, Settings->BackupPoolPort, {JSON_SPIFFS_KEY_BACKUPPOOLPORT, "backupPort", "backupport", "bport", "fallbackStratumPort", "SecondaryPort"}), 0, 65535);

    const char* bpassKey = nullptr;
    if (hasAnyKey(json, {JSON_SPIFFS_KEY_BACKUPPOOLPASS, "backupPoolPassword", "backuppoolpassword", "backupPass", "backuppass", "bpass", "bpoolpass", "fallbackStratumPassword", "SecondaryPassword"}, &bpassKey)) {
        String bpass = json[bpassKey].as<String>();
        if (!isMaskedSecret(bpass)) {
            strlcpy(Settings->BackupPoolPassword, bpass.c_str(), sizeof(Settings->BackupPoolPassword));
        }
    }

    // Web UI password
    String webPass = getJsonString(json, {JSON_SPIFFS_KEY_WEBPASSWORD, "webPass", "webpass", "WebPassword"});
    if (webPass.length() > 0 && !isMaskedSecret(webPass)) {
        Settings->WebPassword = webPass;
    } else if (Settings->WebPassword.isEmpty()) {
        Settings->WebPassword = DEFAULT_WEBPASSWORD;
    }

    // MQTT settings
    Settings->MqttEnabled = getJsonBool(json, Settings->MqttEnabled, {JSON_SPIFFS_KEY_MQTTENABLED, "mqttEnabled", "mqttenabled", "mqtt"});
    Settings->MqttPort = constrain(getJsonInt(json, Settings->MqttPort, {JSON_SPIFFS_KEY_MQTTPORT, "mqttPort", "mqttport", "mqport"}), 1, 65535);
    const char* mqHostKey = nullptr;
    if (hasAnyKey(json, {JSON_SPIFFS_KEY_MQTTHOST, "mqttHost", "mqtthost", "mqhost"}, &mqHostKey)) {
        String h = json[mqHostKey].as<String>();
        h.trim();
        if (h.startsWith("mqtt://")) h = h.substring(7);
        else if (h.startsWith("tcp://")) h = h.substring(6);
        int slash = h.indexOf('/');
        if (slash >= 0) h = h.substring(0, slash);
        int colon = h.lastIndexOf(':');
        if (colon > 0 && h.indexOf(':') == colon) {
            int p = h.substring(colon + 1).toInt();
            if (p >= 1 && p <= 65535) Settings->MqttPort = p;
            h = h.substring(0, colon);
        }
        Settings->MqttHost = h;
    }
    const char* mqUserKey = nullptr;
    if (hasAnyKey(json, {JSON_SPIFFS_KEY_MQTTUSER, "mqttUser", "mqttuser", "mquser"}, &mqUserKey)) {
        Settings->MqttUser = json[mqUserKey].as<String>();
    }
    const char* mqPassKey = nullptr;
    if (hasAnyKey(json, {JSON_SPIFFS_KEY_MQTTPASS, "mqttPass", "mqttpass", "mqpass", "mqttPassword"}, &mqPassKey)) {
        String mp = json[mqPassKey].as<String>();
        if (!isMaskedSecret(mp)) {
            Settings->MqttPass = mp;
        }
    }
    const char* mqTopicKey = nullptr;
    if (hasAnyKey(json, {JSON_SPIFFS_KEY_MQTTTOPIC, "mqttTopic", "mqtttopic", "mqtopic"}, &mqTopicKey)) {
        Settings->MqttTopic = json[mqTopicKey].as<String>();
        Settings->MqttTopic.trim();
    }

    // Hostname (explicit or fallback to worker suffix from wallet if not yet set)
    String host = getJsonString(json, {JSON_SPIFFS_KEY_HOSTNAME, "hostname", "Hostname", "hostName", "host"});
    host.trim();
    if (host.length() > 0) {
        strncpy(Settings->Hostname, host.c_str(), sizeof(Settings->Hostname) - 1);
        Settings->Hostname[sizeof(Settings->Hostname) - 1] = '\0';
    } else if (Settings->Hostname[0] == '\0') {
        const char* dot = strrchr(Settings->BtcWallet, '.');
        if (dot && strlen(dot + 1) > 0 && strlen(dot + 1) < sizeof(Settings->Hostname)) {
            strncpy(Settings->Hostname, dot + 1, sizeof(Settings->Hostname) - 1);
            Settings->Hostname[sizeof(Settings->Hostname) - 1] = '\0';
        }
    }

    // Timezone
    String tz = getJsonString(json, {JSON_SPIFFS_KEY_TIMEZONE, JSON_KEY_TIMEZONE, "timezone", "tz", "posix"});
    if (tz.length() > 0) Settings->Timezone = tz;

    // Currency
    String cur = getJsonString(json, {JSON_SPIFFS_KEY_CURRENCY, JSON_KEY_CURRENCY, "currency", "cur"});
    if (cur.length() > 0) Settings->Currency = currencyFor(cur).code;

    // Booleans
    Settings->saveStats = getJsonBool(json, Settings->saveStats, {JSON_SPIFFS_KEY_STATS2NV, JSON_KEY_STATS2NV, "savestats", "saveStats", "stats"});
    Settings->invertColors = getJsonBool(json, Settings->invertColors, {JSON_SPIFFS_KEY_INVCOLOR, JSON_KEY_INVCOLOR, "invert", "invertColors", "invertcolors"});
    Settings->flipDisplay = getJsonBool(json, Settings->flipDisplay, {JSON_SPIFFS_KEY_FLIPDISPLAY, JSON_KEY_FLIPDISPLAY, "flip", "flipDisplay", "flipdisplay"});
    Settings->autoBrightness = getJsonBool(json, Settings->autoBrightness, {JSON_SPIFFS_KEY_AUTOBRIGHTNESS, JSON_KEY_AUTOBRIGHTNESS, "autobrightness", "autoBrightness", "autobright", "auto_brightness"});

    // Display & timeouts
    Settings->Brightness = constrain(getJsonInt(json, Settings->Brightness, {JSON_SPIFFS_KEY_BRIGHTNESS, JSON_KEY_BRIGHTNESS, "brightness", "bright"}), 0, 255);
    int legacySec = getJsonInt(json, -1, {"screenOffTimeout", "screen_off_timeout", "screenOff"});
    if (legacySec >= 0) {
        Settings->ScreenTimeout = constrain((legacySec + 59) / 60, 0, 1440);
    }
    Settings->ScreenTimeout = constrain(getJsonInt(json, Settings->ScreenTimeout, {JSON_SPIFFS_KEY_SCREENTIMEOUT, "screenTimeout", "ScreenTimeout", "timeout", "screenoff"}), 0, 1440);
    Settings->nightDim = getJsonBool(json, Settings->nightDim, {JSON_SPIFFS_KEY_NIGHTDIM, "nightDim", "nightdim"});
    Settings->NightBrightness = constrain(getJsonInt(json, Settings->NightBrightness, {JSON_SPIFFS_KEY_NIGHTBRIGHTNESS, "nightBrightness", "NightBrightness", "nightbright", "nightbrightness"}), 1, 255);
    Settings->NightFade = constrain(getJsonInt(json, Settings->NightFade, {JSON_SPIFFS_KEY_NIGHTFADE, "nightFade", "NightFade", "nightfade"}), 1, 240);

    // City (accepts "lat,lon" or a city name from kCities)
    String city = getJsonString(json, {"city", "City"});
    city.trim();
    if (city.length() > 0) {
        int comma = city.indexOf(',');
        if (comma > 0) {
            Settings->Latitude = constrain(city.substring(0, comma).toFloat(), -90.0f, 90.0f);
            Settings->Longitude = constrain(city.substring(comma + 1).toFloat(), -180.0f, 180.0f);
        } else {
            for (int i = 0; i < kCityCount; i++) {
                if (city.equalsIgnoreCase(kCities[i].name)) {
                    Settings->Latitude = kCities[i].latitude;
                    Settings->Longitude = kCities[i].longitude;
                    break;
                }
            }
        }
    }
    Settings->Latitude = constrain(getJsonFloat(json, Settings->Latitude, {JSON_SPIFFS_KEY_LATITUDE, "latitude", "lat"}), -90.0f, 90.0f);
    Settings->Longitude = constrain(getJsonFloat(json, Settings->Longitude, {JSON_SPIFFS_KEY_LONGITUDE, "longitude", "lon"}), -180.0f, 180.0f);

    // Status LED & RGB LED (parsed unconditionally since fields exist on TSettings across all targets)
    Settings->statusLed = getJsonBool(json, Settings->statusLed, {JSON_SPIFFS_KEY_STATUSLED, "statusLed", "statusled", "led"});

    for (const char* k : {JSON_SPIFFS_KEY_LEDMODE, "ledmode", "ledModeName", "mode"}) {
        if (json.containsKey(k)) {
            if (json[k].is<const char*>()) {
                String m = json[k].as<String>();
                m.trim();
                m.toLowerCase();
                if (m == "off") Settings->ledMode = LED_MODE_OFF;
                else if (m == "status" || m == "blink") Settings->ledMode = LED_MODE_STATUS;
                else if (m == "steady" || m == "on" || m == "solid") Settings->ledMode = LED_MODE_STEADY;
                else Settings->ledMode = constrain((int)m.toInt(), LED_MODE_OFF, LED_MODE_STEADY);
            } else {
                Settings->ledMode = constrain(json[k].as<int>(), LED_MODE_OFF, LED_MODE_STEADY);
            }
            break;
        }
    }
    for (const char* k : {JSON_SPIFFS_KEY_LEDCOLOR, "ledColorHex", "ledcolorhex", "ledcolor", "color"}) {
        if (json.containsKey(k)) {
            if (json[k].is<const char*>()) {
                String c = json[k].as<String>();
                c.trim();
                c.toLowerCase();
                if (c.startsWith("#") && c.length() == 7) {
                    Settings->ledColor = strtoul(c.c_str() + 1, NULL, 16) & 0xFFFFFF;
                } else if (c.startsWith("0x")) {
                    Settings->ledColor = strtoul(c.c_str() + 2, NULL, 16) & 0xFFFFFF;
                } else if (c == "red") Settings->ledColor = 0xFF0000;
                else if (c == "green") Settings->ledColor = 0x00FF00;
                else if (c == "blue") Settings->ledColor = 0x0000FF;
                else if (c == "yellow") Settings->ledColor = 0xFFFF00;
                else if (c == "cyan") Settings->ledColor = 0x00FFFF;
                else if (c == "magenta" || c == "pink") Settings->ledColor = 0xFF00FF;
                else if (c == "purple") Settings->ledColor = 0x8000FF;
                else if (c == "orange") Settings->ledColor = 0xFF8000;
                else if (c == "white") Settings->ledColor = 0xFFFFFF;
                else if (c == "off" || c == "black") Settings->ledColor = 0x000000;
                else if (c.length() == 6) {
                    Settings->ledColor = strtoul(c.c_str(), NULL, 16) & 0xFFFFFF;
                } else {
                    Settings->ledColor = (uint32_t)c.toInt() & 0xFFFFFF;
                }
            } else {
                Settings->ledColor = json[k].as<uint32_t>() & 0xFFFFFF;
            }
            break;
        }
    }
    Settings->ledBrightness = constrain(getJsonInt(json, Settings->ledBrightness, {JSON_SPIFFS_KEY_LEDBRIGHTNESS, "ledbright", "ledbrightness", "led_brightness"}), 1, 255);
    Settings->ledScreenOff = getJsonBool(json, Settings->ledScreenOff, {JSON_SPIFFS_KEY_LEDSCREENOFF, "ledscreen", "ledscreenoff", "led_screen_off"});

    return true;
}

/// @brief Load settings from config file located in SPIFFS.
/// @param TSettings* Struct to update with new settings.
/// @return true on success
bool nvMemory::loadConfig(TSettings* Settings)
{
    if (init())
    {
        if (!SPIFFS.exists(JSON_CONFIG_FILE) && SPIFFS.exists(JSON_CONFIG_TMP))
        {
            // Power cut between the two steps of saveConfig(): the temporary
            // file is complete (it is only renamed after a full write)
            Serial.println("SPIFS: Restoring the config saved just before a power cut");
            SPIFFS.rename(JSON_CONFIG_TMP, JSON_CONFIG_FILE);
        }
        if (SPIFFS.exists(JSON_CONFIG_FILE))
        {
            // The file exists, reading and loading via const char* reader
            File configFile = SPIFFS.open(JSON_CONFIG_FILE, "r");
            if (configFile)
            {
                Serial.println("SPIFS: Loading config file");
                String raw = readFileToString(configFile);
                configFile.close();

                DynamicJsonDocument json(3072); // room for the MQTT fields too
                DeserializationError error = deserializeJson(json, raw.c_str());
                if (!error)
                {
                    bool ok = parseJsonConfig(json, Settings);
                    printConfigMasked(json);
                    return ok;
                }
                else
                {
                    Serial.println("SPIFS: Error parsing config file!");
                }
            }
            else
            {
                Serial.println("SPIFS: Error opening config file!");
            }
        }
        else
        {
            Serial.println("SPIFS: No config file available!");
        }
    }
    return false;
}

/// @brief Delete config file from SPIFFS
/// @return true on successs
bool nvMemory::deleteConfig()
{
    Serial.println("SPIFS: Erasing config file..");
    SPIFFS.remove(JSON_CONFIG_TMP);
    return SPIFFS.remove(JSON_CONFIG_FILE); //Borramos fichero
}

/// @brief Prepare and mount SPIFFS
/// @return true on success
bool nvMemory::init()
{
    if (!Initialized_)
    {
        Serial.println("SPIFS: Mounting File System...");
        // May need to make it begin(true) first time you are using SPIFFS
        Initialized_ = SPIFFS.begin(false) || SPIFFS.begin(true);
        Initialized_ ? Serial.println("SPIFS: Mounted") : Serial.println("SPIFS: Mounting failed.");
    }
    else
    {
        Serial.println("SPIFS: Already Mounted");
    }
    return Initialized_;
};

#else

nvMemory::nvMemory() {}
nvMemory::~nvMemory() {}
bool nvMemory::saveConfig(TSettings* Settings) { return false; }
bool nvMemory::loadConfig(TSettings* Settings) { return false; }
bool nvMemory::deleteConfig() { return false; }
bool nvMemory::parseJsonConfig(const JsonDocument& json, TSettings* Settings) { return false; }
void nvMemory::fillConfigJson(JsonDocument& doc, const TSettings& s, bool spiffsKeys, bool maskSecrets) {}
bool nvMemory::init() { return false; }

#endif //NVMEM_TYPE
