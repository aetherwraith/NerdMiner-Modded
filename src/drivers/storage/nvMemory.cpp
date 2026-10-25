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

// The config as JSON on the serial log, without the passwords in it.
// Modifies json in-place (call after reading from or writing to json).
static void printConfigMasked(JsonDocument &json)
{
    for (const char *key : {JSON_SPIFFS_KEY_POOLPASS, JSON_SPIFFS_KEY_BACKUPPOOLPASS, JSON_SPIFFS_KEY_WEBPASSWORD,
                            JSON_SPIFFS_KEY_MQTTPASS, JSON_KEY_PASW, JSON_KEY_POOLPASS})
    {
        const char *v = json[key].as<const char *>();
        if (v && *v)
            json[key] = "***";
    }
    serializeJsonPretty(json, Serial);
    Serial.print('\n');
}

nvMemory::~nvMemory()
{
    if (Initialized_)
        SPIFFS.end();
};

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
        json[JSON_SPIFFS_KEY_POOLURL] = Settings->PoolAddress;
        json[JSON_SPIFFS_KEY_POOLPORT] = Settings->PoolPort;
        json[JSON_SPIFFS_KEY_POOLPASS] = Settings->PoolPassword;
        json[JSON_SPIFFS_KEY_WALLETID] = Settings->BtcWallet;
        json[JSON_SPIFFS_KEY_BACKUPPOOLURL] = Settings->BackupPoolAddress;
        json[JSON_SPIFFS_KEY_BACKUPPOOLPORT] = Settings->BackupPoolPort;
        json[JSON_SPIFFS_KEY_BACKUPPOOLPASS] = Settings->BackupPoolPassword;
        json[JSON_SPIFFS_KEY_WEBPASSWORD] = Settings->WebPassword;
        json[JSON_SPIFFS_KEY_MQTTENABLED] = Settings->MqttEnabled;
        json[JSON_SPIFFS_KEY_MQTTHOST] = Settings->MqttHost;
        json[JSON_SPIFFS_KEY_MQTTPORT] = Settings->MqttPort;
        json[JSON_SPIFFS_KEY_MQTTUSER] = Settings->MqttUser;
        json[JSON_SPIFFS_KEY_MQTTPASS] = Settings->MqttPass;
        json[JSON_SPIFFS_KEY_MQTTTOPIC] = Settings->MqttTopic;
        json[JSON_SPIFFS_KEY_TIMEZONE] = Settings->Timezone;
        json[JSON_SPIFFS_KEY_STATS2NV] = Settings->saveStats;
        json[JSON_SPIFFS_KEY_INVCOLOR] = Settings->invertColors;
        json[JSON_SPIFFS_KEY_BRIGHTNESS] = Settings->Brightness;
        json[JSON_SPIFFS_KEY_FLIPDISPLAY] = Settings->flipDisplay;
        json[JSON_SPIFFS_KEY_CURRENCY] = Settings->Currency;
        json[JSON_SPIFFS_KEY_AUTOBRIGHTNESS] = Settings->autoBrightness;
        json[JSON_SPIFFS_KEY_SCREENTIMEOUT] = Settings->ScreenTimeout;
        json[JSON_SPIFFS_KEY_NIGHTDIM] = Settings->nightDim;
        json[JSON_SPIFFS_KEY_NIGHTBRIGHTNESS] = Settings->NightBrightness;
        json[JSON_SPIFFS_KEY_NIGHTFADE] = Settings->NightFade;
        json[JSON_SPIFFS_KEY_LATITUDE] = Settings->Latitude;
        json[JSON_SPIFFS_KEY_LONGITUDE] = Settings->Longitude;
        json[JSON_SPIFFS_KEY_HOSTNAME] = Settings->Hostname;
#ifdef NO_DISPLAY
        json[JSON_SPIFFS_KEY_STATUSLED] = Settings->statusLed;
#endif
#ifdef HAS_RGB_LED
        json[JSON_SPIFFS_KEY_LEDMODE] = Settings->ledMode;
        json[JSON_SPIFFS_KEY_LEDCOLOR] = Settings->ledColor;
        json[JSON_SPIFFS_KEY_LEDBRIGHTNESS] = Settings->ledBrightness;
        json[JSON_SPIFFS_KEY_LEDSCREENOFF] = Settings->ledScreenOff;
#endif
        if (Settings->WifiSSID.length() > 0 && Settings->WifiSSID != "NerdMinerAP" && Settings->WifiSSID != "HanSoloAP")
        {
            json[JSON_KEY_SSID] = Settings->WifiSSID;
            json[JSON_KEY_PASW] = Settings->WifiPW;
        }

        // Into the temporary file first
        File configFile = SPIFFS.open(JSON_CONFIG_TMP, "w");
        if (!configFile)
        {
            // Error, file did not open
            Serial.println("SPIFS: Failed to open config file for writing");
            return false;
        }
        const size_t expected = measureJson(json);
        const size_t written = serializeJson(json, configFile);
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
        if (k && doc.containsKey(k)) return doc[k].as<int>();
    }
    return defaultVal;
}

static float getJsonFloat(const JsonDocument& doc, float defaultVal, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        if (k && doc.containsKey(k)) return doc[k].as<float>();
    }
    return defaultVal;
}

static bool getJsonBool(const JsonDocument& doc, bool defaultVal, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        if (k && doc.containsKey(k)) return doc[k].as<bool>();
    }
    return defaultVal;
}

bool nvMemory::parseJsonConfig(const JsonDocument& json, TSettings* Settings)
{
    if (!Settings) return false;

    // WiFi credentials (supported from SDCard, SPIFFS, or API)
    String ssid = getJsonString(json, {JSON_KEY_SSID, "ssid", "wifiSSID"});
    if (ssid.length() > 0) Settings->WifiSSID = ssid;

    String pw = getJsonString(json, {JSON_KEY_PASW, "password", "wifiPW"});
    if (pw.length() > 0) Settings->WifiPW = pw;

    // Pool connection (supports "stratum+tcp://host:port" or "host" + "port")
    String pool = getJsonString(json, {JSON_SPIFFS_KEY_POOLURL, JSON_KEY_POOLURL, "pool", "stratumURL", "PrimaryPool"});
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

    Settings->PoolPort = getJsonInt(json, Settings->PoolPort, {JSON_SPIFFS_KEY_POOLPORT, JSON_KEY_POOLPORT, "port", "stratumPort"});

    // Credentials
    String pass = getJsonString(json, {JSON_SPIFFS_KEY_POOLPASS, JSON_KEY_POOLPASS, "poolpass", "poolPassword", "stratumPassword", "PrimaryPassword"});
    if (pass.length() > 0) {
        strncpy(Settings->PoolPassword, pass.c_str(), sizeof(Settings->PoolPassword) - 1);
        Settings->PoolPassword[sizeof(Settings->PoolPassword) - 1] = '\0';
    }

    String wallet = getJsonString(json, {JSON_SPIFFS_KEY_WALLETID, JSON_KEY_WALLETID, "wallet", "btcWallet", "stratumUser", "PrimaryAddress"});
    if (wallet.length() > 0) {
        strncpy(Settings->BtcWallet, wallet.c_str(), sizeof(Settings->BtcWallet) - 1);
        Settings->BtcWallet[sizeof(Settings->BtcWallet) - 1] = '\0';
    }

    // Backup Pool
    for (const char* k : {JSON_SPIFFS_KEY_BACKUPPOOLURL, "backupPool", "bpool", "fallbackStratumURL", "SecondaryPool"}) {
        if (json.containsKey(k)) {
            String bpool = json[k].as<String>();
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
            break;
        }
    }
    Settings->BackupPoolPort = constrain(getJsonInt(json, Settings->BackupPoolPort, {JSON_SPIFFS_KEY_BACKUPPOOLPORT, "backupPort", "bport", "fallbackStratumPort", "SecondaryPort"}), 0, 65535);
    for (const char* k : {JSON_SPIFFS_KEY_BACKUPPOOLPASS, "backupPoolPassword", "bpoolpass", "fallbackStratumPassword", "SecondaryPassword"}) {
        if (json.containsKey(k)) {
            String bpass = json[k].as<String>();
            strlcpy(Settings->BackupPoolPassword, bpass.c_str(), sizeof(Settings->BackupPoolPassword));
            break;
        }
    }

    // Web UI password
    String webPass = getJsonString(json, {JSON_SPIFFS_KEY_WEBPASSWORD, "webpass", "webPassword"});
    if (webPass.length() > 0) {
        Settings->WebPassword = webPass;
    } else if (Settings->WebPassword.isEmpty()) {
        Settings->WebPassword = DEFAULT_WEBPASSWORD;
    }

    // MQTT settings
    Settings->MqttEnabled = getJsonBool(json, Settings->MqttEnabled, {JSON_SPIFFS_KEY_MQTTENABLED, "mqttEnabled", "mqtt"});
    if (json.containsKey(JSON_SPIFFS_KEY_MQTTHOST) || json.containsKey("mqttHost")) {
        Settings->MqttHost = getJsonString(json, {JSON_SPIFFS_KEY_MQTTHOST, "mqttHost"});
        Settings->MqttHost.trim();
    }
    Settings->MqttPort = constrain(getJsonInt(json, Settings->MqttPort, {JSON_SPIFFS_KEY_MQTTPORT, "mqttPort"}), 1, 65535);
    if (json.containsKey(JSON_SPIFFS_KEY_MQTTUSER) || json.containsKey("mqttUser")) {
        Settings->MqttUser = getJsonString(json, {JSON_SPIFFS_KEY_MQTTUSER, "mqttUser"});
    }
    if (json.containsKey(JSON_SPIFFS_KEY_MQTTPASS) || json.containsKey("mqttPass") || json.containsKey("mqttPassword")) {
        Settings->MqttPass = getJsonString(json, {JSON_SPIFFS_KEY_MQTTPASS, "mqttPass", "mqttPassword"});
    }
    if (json.containsKey(JSON_SPIFFS_KEY_MQTTTOPIC) || json.containsKey("mqttTopic")) {
        Settings->MqttTopic = getJsonString(json, {JSON_SPIFFS_KEY_MQTTTOPIC, "mqttTopic"});
        Settings->MqttTopic.trim();
    }

    // Hostname (explicit or fallback to worker suffix from wallet if not yet set)
    String host = getJsonString(json, {JSON_SPIFFS_KEY_HOSTNAME, "hostname", "Hostname", "hostName"});
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
    String tz = getJsonString(json, {JSON_SPIFFS_KEY_TIMEZONE, JSON_KEY_TIMEZONE, "timezone", "tz"});
    if (tz.length() > 0) Settings->Timezone = tz;

    // Currency
    String cur = getJsonString(json, {JSON_SPIFFS_KEY_CURRENCY, JSON_KEY_CURRENCY, "currency"});
    if (cur.length() > 0) Settings->Currency = currencyFor(cur).code;

    // Booleans
    Settings->saveStats = getJsonBool(json, Settings->saveStats, {JSON_SPIFFS_KEY_STATS2NV, JSON_KEY_STATS2NV, "savestats", "saveStats"});
    Settings->invertColors = getJsonBool(json, Settings->invertColors, {JSON_SPIFFS_KEY_INVCOLOR, JSON_KEY_INVCOLOR, "invert", "invertColors"});
    Settings->flipDisplay = getJsonBool(json, Settings->flipDisplay, {JSON_SPIFFS_KEY_FLIPDISPLAY, JSON_KEY_FLIPDISPLAY, "flip", "flipDisplay"});
    Settings->autoBrightness = getJsonBool(json, Settings->autoBrightness, {JSON_SPIFFS_KEY_AUTOBRIGHTNESS, JSON_KEY_AUTOBRIGHTNESS, "autobrightness", "autoBrightness", "auto_brightness"});

    // Display & timeouts
    Settings->Brightness = constrain(getJsonInt(json, Settings->Brightness, {JSON_SPIFFS_KEY_BRIGHTNESS, JSON_KEY_BRIGHTNESS, "brightness"}), 0, 255);
    int legacySec = getJsonInt(json, -1, {"screenOffTimeout", "screen_off_timeout", "screenOff"});
    if (legacySec >= 0) {
        Settings->ScreenTimeout = constrain((legacySec + 59) / 60, 0, 1440);
    }
    Settings->ScreenTimeout = constrain(getJsonInt(json, Settings->ScreenTimeout, {JSON_SPIFFS_KEY_SCREENTIMEOUT, "screenTimeout", "ScreenTimeout", "screenoff"}), 0, 1440);
    Settings->nightDim = getJsonBool(json, Settings->nightDim, {JSON_SPIFFS_KEY_NIGHTDIM, "nightDim", "nightdim"});
    Settings->NightBrightness = constrain(getJsonInt(json, Settings->NightBrightness, {JSON_SPIFFS_KEY_NIGHTBRIGHTNESS, "nightBrightness", "NightBrightness", "nightbright"}), 1, 255);
    Settings->NightFade = constrain(getJsonInt(json, Settings->NightFade, {JSON_SPIFFS_KEY_NIGHTFADE, "nightFade", "NightFade", "nightfade"}), 1, 240);

    // City (accepts "lat,lon" or a city name from kCities)
    String city = getJsonString(json, {"city", "City"});
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

#ifdef NO_DISPLAY
    Settings->statusLed = getJsonBool(json, Settings->statusLed, {JSON_SPIFFS_KEY_STATUSLED, "statusLed", "led"});
#endif
#ifdef HAS_RGB_LED
    for (const char* k : {JSON_SPIFFS_KEY_LEDMODE, "ledmode", "mode"}) {
        if (json.containsKey(k)) {
            if (json[k].is<const char*>()) {
                String m = json[k].as<String>();
                m.toLowerCase();
                if (m == "off") Settings->ledMode = LED_MODE_OFF;
                else if (m == "status" || m == "blink") Settings->ledMode = LED_MODE_STATUS;
                else if (m == "steady" || m == "on" || m == "solid") Settings->ledMode = LED_MODE_STEADY;
                else Settings->ledMode = constrain(m.toInt(), LED_MODE_OFF, LED_MODE_STEADY);
            } else {
                Settings->ledMode = constrain(json[k].as<int>(), LED_MODE_OFF, LED_MODE_STEADY);
            }
            break;
        }
    }
    for (const char* k : {JSON_SPIFFS_KEY_LEDCOLOR, "ledColorHex", "ledcolor", "color"}) {
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
    Settings->ledBrightness = constrain(getJsonInt(json, Settings->ledBrightness, {JSON_SPIFFS_KEY_LEDBRIGHTNESS, "ledbright", "led_brightness"}), 1, 255);
    Settings->ledScreenOff = getJsonBool(json, Settings->ledScreenOff, {JSON_SPIFFS_KEY_LEDSCREENOFF, "ledscreen", "led_screen_off"});
#endif

    return true;
}

/// @brief Load settings from config file located in SPIFFS.
/// @param TSettings* Struct to update with new settings.
/// @return true on success
bool nvMemory::loadConfig(TSettings* Settings)
{
    // Uncomment if we need to format filesystem
    // SPIFFS.format();

    // Load existing configuration file
    // Read configuration from FS json

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
            // The file exists, reading and loading
            File configFile = SPIFFS.open(JSON_CONFIG_FILE, "r");
            if (configFile)
            {
                Serial.println("SPIFS: Loading config file");
                DynamicJsonDocument json(3072); // room for the MQTT fields too
                DeserializationError error = deserializeJson(json, configFile);
                configFile.close();
                if (!error)
                {
                    bool ok = parseJsonConfig(json, Settings);
                    printConfigMasked(json);
                    return ok;
                }
                else
                {
                    // Error loading JSON data
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
bool nvMemory::init() { return false; }


#endif //NVMEM_TYPE