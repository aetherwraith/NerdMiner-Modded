#define ESP_DRD_USE_SPIFFS true

// Include Libraries
//#include ".h"

#include <WiFi.h>

#include <WiFiManager.h>

#include "wManager.h"
#include "webConfig.h"
#include "monitor.h"
#include "drivers/displays/display.h"
#include "drivers/storage/SDCard.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "mining.h"
#include "timeconst.h"

#include <ArduinoJson.h>
#include <esp_flash.h>


// Flag for saving data
bool shouldSaveConfig = false;

// Variables to hold data from custom textboxes
TSettings Settings;
std::mutex g_settingsMutex; // see wManager.h

// Define WiFiManager Object
WiFiManager wm;
extern monitor_data mMonitor;

nvMemory nvMem;

extern SDCard SDCrd;

String readWebflasherConfig(TSettings* settings) {
    Serial.println(F("DEBUG: Attempting to read flasher config from flash at 0x3F0000..."));
    
    const size_t DATA_SIZE = 1024;
    uint8_t buffer[DATA_SIZE];
    memset(buffer, 0, DATA_SIZE);
    
    esp_err_t result = esp_flash_read(NULL, buffer, 0x3F0000, DATA_SIZE);
    if (result != ESP_OK) {
        Serial.printf("DEBUG: Flash read error: %s\n", esp_err_to_name(result));
        return "";
    }
    
    buffer[DATA_SIZE - 1] = '\0';
    String data = String((char*)buffer);
    
    if (data.startsWith("WEBFLASHER_CONFIG:")) {
        Serial.println(F("DEBUG: Found WEBFLASHER_CONFIG marker"));
        String jsonPart = data.substring(18);
        
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, jsonPart);
        
        if (error == DeserializationError::Ok) {
            Serial.println(F("DEBUG: Webflasher JSON parsed successfully"));
            
            if (settings) {
                nvMemory::parseJsonConfig(doc, settings);
            }
            
            if (doc.containsKey("apname")) {
                String customAP = doc["apname"].as<String>();
                customAP.trim();
                
                if (customAP.length() > 0 && customAP.length() < 32) {
                    Serial.printf("✅ Custom AP name from webflasher: %s\n", customAP.c_str());
                    return customAP;
                }
            }
        } else {
            Serial.printf("DEBUG: JSON parse error: %s\n", error.c_str());
        }
    } else {
        Serial.println(F("DEBUG: WEBFLASHER_CONFIG marker not found - no custom flasher config"));
    }
    
    return "";
}

String readCustomAPName() {
    return readWebflasherConfig(nullptr);
}

void saveConfigCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;    
    //wm.setConfigPortalBlocking(false);
}

/* void saveParamsCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
    nvMem.saveConfig(&Settings);
} */

void configModeCallback(WiFiManager* myWiFiManager)
// Called when config mode launched
{
    Serial.println("Entered Configuration Mode");
    drawSetupScreen();
    Serial.print("Config SSID: ");
    Serial.println(myWiFiManager->getConfigPortalSSID());

    Serial.print("Config IP Address: ");
    Serial.println(WiFi.softAPIP());
}

void reset_configuration()
{
    Serial.println("Erasing Config, restarting");
    nvMem.deleteConfig();
    resetStat();
    wm.resetSettings();
    ESP.restart();
}

void saveSettingsToFlash(TSettings *settings)
{
    nvMem.saveConfig(settings);
}

void init_WifiManager()
{
    // Check for custom AP name or webflasher config from flash offset 0x3F0000
    String customAPName = readWebflasherConfig(&Settings);
    const char* apName = customAPName.length() > 0 ? customAPName.c_str() : DEFAULT_SSID;

    // Change to true when testing to force configuration every time we run
    bool forceConfig = false;

#if defined(PIN_BUTTON_2)
    // Check if button2 is pressed to enter configMode with actual configuration
    if (!digitalRead(PIN_BUTTON_2)) {
        Serial.println(F("Button pressed to force start config mode"));
        forceConfig = true;
        wm.setBreakAfterConfig(true); //Set to detect config edition and save
    }
#endif
    // Explicitly set WiFi mode
    WiFi.mode(WIFI_STA);

    if (!nvMem.loadConfig(&Settings))
    {
        // No config file on internal flash.
        if (SDCrd.loadConfigFile(&Settings))
        {
            // Config file on SD card.
            SDCrd.SD2nvMemory(&nvMem, &Settings); // reboot on success.          
        }
        else if (Settings.WifiSSID.length() > 0 && Settings.WifiSSID != "NerdMinerAP" && Settings.WifiSSID != "HanSoloAP")
        {
            // Pre-configured WiFi credentials detected (from config.local.h, build flags, or webflasher)
            Serial.printf("First boot detected with pre-configured WiFi: %s. Saving to SPIFFS...\n", Settings.WifiSSID.c_str());
            nvMem.saveConfig(&Settings);
            WiFi.begin(Settings.WifiSSID.c_str(), Settings.WifiPW.c_str());
            forceConfig = false;
        }
        else
        {
            // No config file on SD card or flash. Starting wifi config server.
            forceConfig = true;
        }
    }

    if (Settings.Hostname[0] != '\0') {
        WiFi.setHostname(Settings.Hostname);
    }
    
    // Free the memory from SDCard class 
    SDCrd.terminate();
    
    // Set config save notify callback
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setSaveParamsCallback(saveConfigCallback);

    // Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wm.setAPCallback(configModeCallback);    

    //Advanced settings
    wm.setConfigPortalBlocking(false);
    wm.setConnectTimeout(40); // how long to try to connect for before continuing
    wm.setConfigPortalTimeout(180); // auto close configportal after n seconds

    // Custom elements
    WiFiManagerParameter pool_text_box("Poolurl", "Pool url", Settings.PoolAddress.c_str(), 80);

    char convertedValue[6];
    sprintf(convertedValue, "%d", Settings.PoolPort);
    WiFiManagerParameter port_text_box_num("Poolport", "Pool port", convertedValue, 7);

    WiFiManagerParameter addr_text_box("btcAddress", "Your BTC address", Settings.BtcWallet, 80);
    WiFiManagerParameter time_text_box("TimeZone", "TimeZone (e.g. Europe/London or UTC offset)", Settings.Timezone.c_str(), 40);
    WiFiManagerParameter features_html("<hr><br><label style=\"font-weight: bold;margin-bottom: 25px;display: inline-block;\">Features</label>");

    char checkboxParams[24] = "type=\"checkbox\"";
    if (Settings.saveStats)
    {
        strcat(checkboxParams, " checked");
    }
    WiFiManagerParameter save_stats_to_nvs("SaveStatsToNVS", "Save mining statistics to flash memory.", "T", 2, checkboxParams, WFM_LABEL_AFTER);
    WiFiManagerParameter password_text_box("Poolpassword", "Pool password (Optional)", Settings.PoolPassword, 80);

    // Add all defined parameters
    wm.addParameter(&pool_text_box);
    wm.addParameter(&port_text_box_num);
    wm.addParameter(&password_text_box);
    wm.addParameter(&addr_text_box);
    wm.addParameter(&time_text_box);
    wm.addParameter(&features_html);
    wm.addParameter(&save_stats_to_nvs);
    #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
    const bool invertAtBoot = Settings.invertColors;
    char checkboxParams2[24] = "type=\"checkbox\"";
    if (Settings.invertColors)
    {
        strcat(checkboxParams2, " checked");
    }
    WiFiManagerParameter invertColors("inverColors", "Invert Display Colors (if the colors looks weird)", "T", 2, checkboxParams2, WFM_LABEL_AFTER);
    wm.addParameter(&invertColors);
    const bool flipAtBoot = Settings.flipDisplay;
    char checkboxParams3[24] = "type=\"checkbox\"";
    if (Settings.flipDisplay)
    {
        strcat(checkboxParams3, " checked");
    }
    WiFiManagerParameter flipDisplay("flipDisplay", "Flip display (USB on the left instead of the right)", "T", 2, checkboxParams3, WFM_LABEL_AFTER);
    wm.addParameter(&flipDisplay);

    char brightnessConvValue[8]; // up to "255" + NUL; was [2], overflowed on sprintf
    sprintf(brightnessConvValue, "%d", Settings.Brightness);
    WiFiManagerParameter brightness_text_box_num("Brightness", "Screen backlight Duty Cycle (0-255)", brightnessConvValue, 3);
    wm.addParameter(&brightness_text_box_num);
    #endif
    #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
    char checkboxParams4[24] = "type=\"checkbox\"";
    if (Settings.autoBrightness)
    {
        strcat(checkboxParams4, " checked");
    }
    WiFiManagerParameter autoBrightnessParam("autoBrightness", "Auto Brightness (LDR Sensor)", "T", 2, checkboxParams4, WFM_LABEL_AFTER);
    wm.addParameter(&autoBrightnessParam);
    #endif
    #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
    char screenOffConvValue[8];
    sprintf(screenOffConvValue, "%d", Settings.screenOffTimeout);
    WiFiManagerParameter screenOff_text_box_num("screenOff", "Auto Screen Off (seconds, 0=disabled)", screenOffConvValue, 6);
    wm.addParameter(&screenOff_text_box_num);
    #endif

    auto extractPortalParams = [&](bool applyPortalCheckboxes) {
        Settings.PoolAddress = pool_text_box.getValue();
        Settings.PoolPort = atoi(port_text_box_num.getValue());
        strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword) - 1);
        Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
        strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet) - 1);
        Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';
        Settings.Timezone = time_text_box.getValue();
        if (applyPortalCheckboxes) {
            Settings.saveStats = (strncmp(save_stats_to_nvs.getValue(), "T", 1) == 0);
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
            Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
            Settings.flipDisplay = (strncmp(flipDisplay.getValue(), "T", 1) == 0);
            Settings.Brightness = atoi(brightness_text_box_num.getValue());
            Settings.screenOffTimeout = atoi(screenOff_text_box_num.getValue());
            #endif
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
            Settings.autoBrightness = (strncmp(autoBrightnessParam.getValue(), "T", 1) == 0);
            #endif
        }
    };

    Serial.println("AllDone: ");
    if (forceConfig)    
    {
        wm.setConfigPortalBlocking(true);
        drawSetupScreen();
        mMonitor.NerdStatus = NM_Connecting;
        wm.startConfigPortal(apName, DEFAULT_WIFIPW);

        if (shouldSaveConfig)
        {
            Serial.println("failed to connect and hit timeout");
            extractPortalParams(true);
            nvMem.saveConfig(&Settings);
            delay(3*SECOND_MS);
            ESP.restart();            
        }
    }
    else
    {
        mMonitor.NerdStatus = NM_Connecting;
        wm.setCaptivePortalEnable(true); 
        wm.setConfigPortalBlocking(true);
        wm.setEnableConfigPortal(true);
        if (!wm.autoConnect(apName, DEFAULT_WIFIPW))
        {
            Serial.println("Failed to connect to configured WIFI, and hit timeout");
            if (shouldSaveConfig) {
                extractPortalParams(true);
                nvMem.saveConfig(&Settings);
                vTaskDelay(2000 / portTICK_PERIOD_MS);      
            }        
            ESP.restart();                            
        } 
    }
    
    // Connected to WiFi network
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("");
        Serial.println("WiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        if (WiFi.SSID().length() > 0) {
            Settings.WifiSSID = WiFi.SSID();
            Settings.WifiPW = WiFi.psk();
        }

        // Only apply checkbox values when the portal form was submitted: an
        // unused checkbox still holds its "T", which would turn the saved
        // setting on in memory.
        extractPortalParams(shouldSaveConfig);

        Serial.print("PoolString: ");
        Serial.println(Settings.PoolAddress);
        Serial.print("portNumber: ");
        Serial.println(Settings.PoolPort);
        Serial.print("poolPassword: ");
        Serial.println(Settings.PoolPassword);
        Serial.print("btcString: ");
        Serial.println(Settings.BtcWallet);
        Serial.print("TimeZone: ");
        Serial.println(Settings.Timezone);
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
        Serial.print("Invert Colors: ");
        Serial.println(Settings.invertColors);
        Serial.print("Brightness: ");
        Serial.println(Settings.Brightness);
        #endif

        // Always-on LAN settings page -- lets the user change pool/wallet/etc.
        // later without re-entering the device's own setup AP.
        setup_webConfig();
    }
    // Save the custom parameters to FS
    if (shouldSaveConfig)
    {
        nvMem.saveConfig(&Settings);
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
        if (Settings.invertColors != invertAtBoot) ESP.restart();
        if (Settings.Brightness != 250) ESP.restart();
        if (Settings.flipDisplay != flipAtBoot) ESP.restart(); // rotation is applied at display init
        #endif
    }
}

//----------------- MAIN PROCESS WIFI MANAGER --------------
int oldStatus = 0;

void wifiManagerProcess() {

    wm.process(); // avoid delays() in loop when non-blocking and other long running code

    int newStatus = WiFi.status();
    if (newStatus != oldStatus) {
        if (newStatus == WL_CONNECTED) {
            Serial.println("CONNECTED - Current ip: " + WiFi.localIP().toString());
        } else {
            Serial.print("[Error] - current status: ");
            Serial.println(newStatus);
        }
        oldStatus = newStatus;
    }
}
