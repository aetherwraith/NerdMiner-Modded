#include "nvMemory.h"

#ifdef NVMEM_SPIFFS

#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoJson.h>

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
        StaticJsonDocument<768> json;
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

static String getJsonString(const JsonDocument& doc, const char* k1, const char* k2 = nullptr, const char* k3 = nullptr)
{
    if (k1 && doc.containsKey(k1)) return doc[k1].as<String>();
    if (k2 && doc.containsKey(k2)) return doc[k2].as<String>();
    if (k3 && doc.containsKey(k3)) return doc[k3].as<String>();
    return "";
}

static int getJsonInt(const JsonDocument& doc, int defaultVal, const char* k1, const char* k2 = nullptr, const char* k3 = nullptr, const char* k4 = nullptr)
{
    if (k1 && doc.containsKey(k1)) return doc[k1].as<int>();
    if (k2 && doc.containsKey(k2)) return doc[k2].as<int>();
    if (k3 && doc.containsKey(k3)) return doc[k3].as<int>();
    if (k4 && doc.containsKey(k4)) return doc[k4].as<int>();
    return defaultVal;
}

static bool getJsonBool(const JsonDocument& doc, bool defaultVal, const char* k1, const char* k2 = nullptr, const char* k3 = nullptr)
{
    if (k1 && doc.containsKey(k1)) return doc[k1].as<bool>();
    if (k2 && doc.containsKey(k2)) return doc[k2].as<bool>();
    if (k3 && doc.containsKey(k3)) return doc[k3].as<bool>();
    return defaultVal;
}

bool nvMemory::parseJsonConfig(const JsonDocument& json, TSettings* Settings)
{
    if (!Settings) return false;

    // WiFi credentials (supported from SDCard or SPIFFS)
    String ssid = getJsonString(json, JSON_KEY_SSID, "ssid");
    if (ssid.length() > 0) Settings->WifiSSID = ssid;

    String pw = getJsonString(json, JSON_KEY_PASW, "password");
    if (pw.length() > 0) Settings->WifiPW = pw;

    // Pool connection
    String pool = getJsonString(json, JSON_SPIFFS_KEY_POOLURL, JSON_KEY_POOLURL);
    if (pool.length() > 0) Settings->PoolAddress = pool;

    Settings->PoolPort = getJsonInt(json, Settings->PoolPort, JSON_SPIFFS_KEY_POOLPORT, JSON_KEY_POOLPORT);

    // Credentials
    String pass = getJsonString(json, JSON_SPIFFS_KEY_POOLPASS, JSON_KEY_POOLPASS);
    if (pass.length() > 0) {
        strncpy(Settings->PoolPassword, pass.c_str(), sizeof(Settings->PoolPassword) - 1);
        Settings->PoolPassword[sizeof(Settings->PoolPassword) - 1] = '\0';
    }

    String wallet = getJsonString(json, JSON_SPIFFS_KEY_WALLETID, JSON_KEY_WALLETID);
    if (wallet.length() > 0) {
        strncpy(Settings->BtcWallet, wallet.c_str(), sizeof(Settings->BtcWallet) - 1);
        Settings->BtcWallet[sizeof(Settings->BtcWallet) - 1] = '\0';
    }

    // Timezone
    String tz = getJsonString(json, JSON_SPIFFS_KEY_TIMEZONE, JSON_KEY_TIMEZONE);
    if (tz.length() > 0) Settings->Timezone = tz;

    // Currency
    String cur = getJsonString(json, JSON_SPIFFS_KEY_CURRENCY, JSON_KEY_CURRENCY);
    if (cur.length() > 0) Settings->Currency = cur;

    // Booleans
    Settings->saveStats = getJsonBool(json, Settings->saveStats, JSON_SPIFFS_KEY_STATS2NV, JSON_KEY_STATS2NV);
    Settings->invertColors = getJsonBool(json, Settings->invertColors, JSON_SPIFFS_KEY_INVCOLOR, JSON_KEY_INVCOLOR);
    Settings->flipDisplay = getJsonBool(json, Settings->flipDisplay, JSON_SPIFFS_KEY_FLIPDISPLAY, JSON_KEY_FLIPDISPLAY);
    Settings->autoBrightness = getJsonBool(json, Settings->autoBrightness, JSON_SPIFFS_KEY_AUTOBRIGHTNESS, JSON_KEY_AUTOBRIGHTNESS, "auto_brightness");

    // Display & timeouts
    Settings->Brightness = getJsonInt(json, Settings->Brightness, JSON_SPIFFS_KEY_BRIGHTNESS, JSON_KEY_BRIGHTNESS);
    Settings->screenOffTimeout = getJsonInt(json, Settings->screenOffTimeout, JSON_SPIFFS_KEY_SCREENOFFTIMEOUT, JSON_KEY_SCREENOFFTIMEOUT, "screen_off_timeout", "screenOff");

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
                StaticJsonDocument<768> json;
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