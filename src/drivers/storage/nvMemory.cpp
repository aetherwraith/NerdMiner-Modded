#include "nvMemory.h"

#ifdef NVMEM_SPIFFS

#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <initializer_list>

#include "../devices/device.h"
#include "storage.h"

nvMemory::nvMemory() : Initialized_(false){};

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

        // Create a JSON document
        StaticJsonDocument<1024> json;
        json[JSON_SPIFFS_KEY_POOLURL] = Settings->PoolAddress;
        json[JSON_SPIFFS_KEY_POOLPORT] = Settings->PoolPort;
        json[JSON_SPIFFS_KEY_POOLPASS] = Settings->PoolPassword;
        json[JSON_SPIFFS_KEY_WALLETID] = Settings->BtcWallet;
        json[JSON_SPIFFS_KEY_TIMEZONE] = Settings->Timezone;
        json[JSON_SPIFFS_KEY_STATS2NV] = Settings->saveStats;
        json[JSON_SPIFFS_KEY_INVCOLOR] = Settings->invertColors;
        json[JSON_SPIFFS_KEY_BRIGHTNESS] = Settings->Brightness;
        json[JSON_SPIFFS_KEY_FLIPDISPLAY] = Settings->flipDisplay;
        json[JSON_SPIFFS_KEY_CURRENCY] = Settings->Currency;
        json[JSON_SPIFFS_KEY_AUTOBRIGHTNESS] = Settings->autoBrightness;
        json[JSON_SPIFFS_KEY_SCREENOFFTIMEOUT] = Settings->screenOffTimeout;
        json[JSON_SPIFFS_KEY_SCREENTIMEOUT] = Settings->ScreenTimeout;
        json[JSON_SPIFFS_KEY_NIGHTDIM] = Settings->nightDim;
        json[JSON_SPIFFS_KEY_NIGHTBRIGHTNESS] = Settings->NightBrightness;
        json[JSON_SPIFFS_KEY_NIGHTFADE] = Settings->NightFade;
        json[JSON_SPIFFS_KEY_LATITUDE] = Settings->Latitude;
        json[JSON_SPIFFS_KEY_LONGITUDE] = Settings->Longitude;
#ifdef NO_DISPLAY
        json[JSON_SPIFFS_KEY_STATUSLED] = Settings->statusLed;
#endif

        // Open config file
        File configFile = SPIFFS.open(JSON_CONFIG_FILE, "w");
        if (!configFile)
        {
            // Error, file did not open
            Serial.println("SPIFS: Failed to open config file for writing");
            return false;
        }

        // Serialize JSON data to write to file
        serializeJsonPretty(json, Serial);
        Serial.print('\n');
        if (serializeJson(json, configFile) == 0)
        {
            // Error writing file
            Serial.println(F("SPIFS: Failed to write to file"));
            return false;
        }
        // Close file
        configFile.close();
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

    // Timezone
    String tz = getJsonString(json, {JSON_SPIFFS_KEY_TIMEZONE, JSON_KEY_TIMEZONE, "timezone", "tz"});
    if (tz.length() > 0) Settings->Timezone = tz;

    // Currency
    String cur = getJsonString(json, {JSON_SPIFFS_KEY_CURRENCY, JSON_KEY_CURRENCY, "currency"});
    if (cur.length() > 0) Settings->Currency = cur;

    // Booleans
    Settings->saveStats = getJsonBool(json, Settings->saveStats, {JSON_SPIFFS_KEY_STATS2NV, JSON_KEY_STATS2NV, "savestats", "saveStats"});
    Settings->invertColors = getJsonBool(json, Settings->invertColors, {JSON_SPIFFS_KEY_INVCOLOR, JSON_KEY_INVCOLOR, "invert", "invertColors"});
    Settings->flipDisplay = getJsonBool(json, Settings->flipDisplay, {JSON_SPIFFS_KEY_FLIPDISPLAY, JSON_KEY_FLIPDISPLAY, "flip", "flipDisplay"});
    Settings->autoBrightness = getJsonBool(json, Settings->autoBrightness, {JSON_SPIFFS_KEY_AUTOBRIGHTNESS, JSON_KEY_AUTOBRIGHTNESS, "autobrightness", "autoBrightness", "auto_brightness"});

    // Display & timeouts
    Settings->Brightness = getJsonInt(json, Settings->Brightness, {JSON_SPIFFS_KEY_BRIGHTNESS, JSON_KEY_BRIGHTNESS, "brightness"});
    Settings->screenOffTimeout = getJsonInt(json, Settings->screenOffTimeout, {JSON_SPIFFS_KEY_SCREENOFFTIMEOUT, JSON_KEY_SCREENOFFTIMEOUT, "screen_off_timeout", "screenOff"});
    Settings->ScreenTimeout = getJsonInt(json, Settings->ScreenTimeout, {JSON_SPIFFS_KEY_SCREENTIMEOUT, "screenTimeout", "ScreenTimeout", "screenoff"});
    Settings->nightDim = getJsonBool(json, Settings->nightDim, {JSON_SPIFFS_KEY_NIGHTDIM, "nightDim", "nightdim"});
    Settings->NightBrightness = getJsonInt(json, Settings->NightBrightness, {JSON_SPIFFS_KEY_NIGHTBRIGHTNESS, "nightBrightness", "NightBrightness", "nightbright"});
    Settings->NightFade = getJsonInt(json, Settings->NightFade, {JSON_SPIFFS_KEY_NIGHTFADE, "nightFade", "NightFade", "nightfade"});

    Settings->Latitude = getJsonFloat(json, Settings->Latitude, {JSON_SPIFFS_KEY_LATITUDE, "latitude", "lat"});
    Settings->Longitude = getJsonFloat(json, Settings->Longitude, {JSON_SPIFFS_KEY_LONGITUDE, "longitude", "lon"});

#ifdef NO_DISPLAY
    Settings->statusLed = getJsonBool(json, Settings->statusLed, {JSON_SPIFFS_KEY_STATUSLED, "statusLed", "led"});
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
        if (SPIFFS.exists(JSON_CONFIG_FILE))
        {
            // The file exists, reading and loading
            File configFile = SPIFFS.open(JSON_CONFIG_FILE, "r");
            if (configFile)
            {
                Serial.println("SPIFS: Loading config file");
                StaticJsonDocument<1024> json;
                DeserializationError error = deserializeJson(json, configFile);
                configFile.close();
                serializeJsonPretty(json, Serial);
                Serial.print('\n');
                if (!error)
                {
                    return parseJsonConfig(json, Settings);
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