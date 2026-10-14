#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "webConfig.h"
#include "version.h"
#include "wManager.h"
#include "mining.h"
#include "monitor.h"
#include "drivers/devices/device.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "currency.h"
#include "timezone.h"
#include "version.h"
#include "utils.h"
#include "cities.h"
#include "drivers/storage/nvMemory.h"
#include <ArduinoJson.h>
#include "ota.h"

extern TSettings Settings;
extern double best_diff;
extern volatile uint32_t shares;
extern volatile uint32_t valids;
extern volatile uint32_t rejects;
extern uint64_t upTime;

// Basic HTTP auth for the LAN settings page. The password starts as the setup
// AP's (DEFAULT_WIFIPW, "MineYourCoins") and can be changed on the page
// (Settings.WebPassword). Meant as a gate against casual access from others
// on the same network, not strong security (plain HTTP, no rate limiting).
static const char *WEBCFG_USER = "admin";
#define WEBCFG_PASS_MAX 63

static WebServer webCfgServer(80);
static bool webCfgStarted = false;

static bool checkAuth()
{
  // Settings.WebPassword is only written by handleSave, on this same task
  if (!webCfgServer.authenticate(WEBCFG_USER, Settings.WebPassword.c_str()))
  {
    webCfgServer.requestAuthentication();
    return false;
  }
  return true;
}

static String htmlEscape(const String &in)
{
  String out = in;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

// The settings as the page should show them: what is in use, or what was
// saved and is still waiting for the stratum or monitor task to apply it.
TSettings currentSettings()
{
  TSettings s;
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    s = Settings;
  }
  readPoolSettings(s);
  s.Currency = currencySetting();
  return s;
}

static void handleRoot()
{
  if (!checkAuth())
    return;

  const TSettings Settings = currentSettings(); // shadows the global on purpose
  String page;
  page.reserve(4096);
  // Sent in parts (chunked), so the whole page with its city list never has
  // to sit in one String.
  bool started = false;
  auto flush = [&]() {
    if (!started)
    {
      webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
      webCfgServer.send(200, "text/html", page);
      started = true;
    }
    else
      webCfgServer.sendContent(page);
    page = "";
  };
  page += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width, initial-scale=1'>"
             "<title>NerdMiner Settings</title>"
             "<style>body{font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em}"
             "label{display:block;margin-top:1em;font-weight:bold}"
             "input[type=text],input[type=number],input[type=password],select{width:100%;padding:.4em;box-sizing:border-box}"
             "input[type=submit]{margin-top:1.5em;padding:.6em 1.2em}</style>"
             "<script>"
             "function filterTZ(){"
             "var q=document.getElementById('tz_search').value.toLowerCase();"
             "var sel=document.getElementById('tz_select');"
             "var opts=sel.options;"
             "for(var i=0;i<opts.length;i++){"
             "var m=opts[i].text.toLowerCase().indexOf(q)!==-1;"
             "opts[i].hidden=!m;"
             "opts[i].style.display=m?'':'none';"
             "}"
             "}"
             "window.addEventListener('DOMContentLoaded',function(){"
             "var sel=document.getElementById('tz_select');"
             "if(sel&&sel.selectedIndex>=0){"
             "sel.options[sel.selectedIndex].scrollIntoView({block:'nearest'});"
             "}"
             "});"
             "</script></head><body>"
             "<h2>NerdMiner Settings</h2>"
             "<form method='POST' action='/save'>");

  page += "<label>Pool URL</label><input type='text' name='pool' value='" + htmlEscape(Settings.PoolAddress) + "'>";
  page += "<label>Pool Port</label><input type='number' name='port' value='" + String(Settings.PoolPort) + "'>";
  page += "<label>Pool Password (optional)</label><input type='text' name='poolpass' value='" + htmlEscape(String(Settings.PoolPassword)) + "'>";
  page += "<label>BTC Address</label><input type='text' name='wallet' value='" + htmlEscape(String(Settings.BtcWallet)) + "'>";
  page += "<label>Backup pool URL (optional)</label><input type='text' name='bpool' value='" + htmlEscape(Settings.BackupPoolAddress) + "'>";
  page += "<label>Backup pool port</label><input type='number' name='bport' value='" + String(Settings.BackupPoolPort) + "'>";
  page += "<label>Backup pool password (optional)</label><input type='text' name='bpoolpass' value='" + htmlEscape(String(Settings.BackupPoolPassword)) + "'>";
  page += "<small>Mined with the same BTC address after " + String(POOL_FAILOVER_TRIES) +
          " failed attempts in a row on the pool above; back to that pool once it answers again (checked every " +
          String(POOL_FAILBACK_ms / 60000) + " minutes). Now mining on the " + (onBackupPool() ? "<b>backup</b>" : "main") + " pool.</small>";
  page += F("<label>Time Zone</label>"
            "<input type='text' id='tz_search' placeholder='Search time zone (e.g. London, New York)...' oninput='filterTZ()' autocomplete='off'>"
            "<select name='tz' id='tz_select' size='6' style='margin-top:4px;'>");

  bool tzMatched = false;
  for (int i = 0; i < kTimeZoneCount; i++)
  {
    bool sel = !tzMatched && (Settings.Timezone.equalsIgnoreCase(kTimeZones[i].id) || Settings.Timezone.equalsIgnoreCase(kTimeZones[i].posix));
    if (sel) tzMatched = true;
    page += "<option value='";
    page += kTimeZones[i].id;
    page += sel ? "' selected>" : "'>";
    page += kTimeZones[i].label;
    page += "</option>";
  }
  if (!tzMatched && Settings.Timezone.length() > 0)
  {
    page += "<option value='" + htmlEscape(Settings.Timezone) + "' selected>Custom: " + htmlEscape(Settings.Timezone) + "</option>";
  }
  page += "</select>";
  flush();
  page += "<label>BTC price currency</label><select name='currency'>";
  for (int i = 0; i < kCurrencyCount; i++)
  {
    page += "<option value='" + String(kCurrencies[i].code) + "'";
    if (Settings.Currency.equalsIgnoreCase(kCurrencies[i].code))
      page += " selected";
    page += ">" + String(kCurrencies[i].label) + "</option>";
  }
  page += "</select>";
  page += "<label><input type='checkbox' name='savestats' " + String(Settings.saveStats ? "checked" : "") + "> Save mining statistics to flash</label>";
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  page += "<label><input type='checkbox' name='invert' " + String(Settings.invertColors ? "checked" : "") + "> Invert display colors</label>";
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (USB on the left)</label>";
  page += "<label><input type='checkbox' name='autobrightness' " + String(Settings.autoBrightness ? "checked" : "") + "> Auto brightness (LDR light sensor)</label>";
#else
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (rotate 180 degrees)</label>";
#endif
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  page += "<label>Screen off after (minutes without a touch, 0 = never)</label><input type='number' name='screenoff' min='0' max='1440' value='" + String(Settings.ScreenTimeout) + "'>";
  page += "<label><input type='checkbox' name='nightdim' " + String(Settings.nightDim ? "checked" : "") + "> Dim the screen at night</label>";
  page += "<small>Fades from the brightness above to the night brightness around sunset, and back at sunrise. " + nightDimStatus() + "</small>";
  page += "<label>Night brightness (1-255)</label><input type='number' name='nightbright' min='1' max='255' value='" + String(Settings.NightBrightness) + "'>";
  page += "<label>Fade length (minutes)</label><input type='number' name='nightfade' min='1' max='240' value='" + String(Settings.NightFade) + "'>";
  page += F("<small>The fade is centred on sunset and sunrise, half before and half after: 60 minutes starts dimming "
            "30 minutes before sunset and reaches the night brightness 30 minutes after it.</small>");
  // Location for sunrise and sunset: a city fills in the two fields below
  // (also without JavaScript: /save takes the city's coordinates when one is
  // selected); typing in them goes back to "Custom".
  flush();
  page += F("<label>Nearest city</label><select name='city'><option value=''>Custom (latitude and longitude below)</option>");
  for (int i = 0; i < kCityCount; i++)
  {
    page += "<option value='" + String(kCities[i].latitude, 2) + "," + String(kCities[i].longitude, 2) + "'";
    if (fabsf(Settings.Latitude - kCities[i].latitude) < 0.005f && fabsf(Settings.Longitude - kCities[i].longitude) < 0.005f)
      page += " selected";
    page += ">" + String(kCities[i].name) + "</option>";
  }
  page += F("</select>");
  flush();
  page += "<label>Latitude (decimal degrees, south negative)</label><input type='number' name='lat' min='-90' max='90' step='any' value='" + String(Settings.Latitude, 4) + "'>";
  page += "<label>Longitude (decimal degrees, west negative)</label><input type='number' name='lon' min='-180' max='180' step='any' value='" + String(Settings.Longitude, 4) + "'>";
#endif
#ifdef NO_DISPLAY
  page += "<label><input type='checkbox' name='led' " + String(Settings.statusLed ? "checked" : "") + "> Status LED (blinks while mining)</label>";
#endif
#ifdef HAS_RGB_LED
  {
    static const char *const kLedModes[] = {"Off", "Status (red starting up, blue connecting, blinks the colour below while mining)", "Always on in the colour below"};
    page += F("<label>LED on the back</label><select name='ledmode'>");
    for (int i = LED_MODE_OFF; i <= LED_MODE_STEADY; i++)
    {
      page += "<option value='" + String(i) + "'";
      if (Settings.ledMode == i)
        page += " selected";
      page += ">" + String(kLedModes[i]) + "</option>";
    }
    page += F("</select>");
    char color[8];
    snprintf(color, sizeof(color), "#%06lx", (unsigned long)(Settings.ledColor & 0xFFFFFF));
    page += "<label>LED colour</label><input type='color' name='ledcolor' value='" + String(color) + "'>";
    page += "<label>LED brightness (1-255)</label><input type='number' name='ledbright' min='1' max='255' value='" + String(Settings.ledBrightness) + "'>";
    page += "<label><input type='checkbox' name='ledscreen' " + String(Settings.ledScreenOff ? "checked" : "") + "> LED off while the screen is off</label>";
  }
#endif
  page += F("<label>New password for this page (leave empty to keep it)</label>"
            "<input type='password' name='webpass' maxlength='63' autocomplete='new-password'>"
            "<label>New password again</label>"
            "<input type='password' name='webpass2' maxlength='63' autocomplete='new-password'>");
  page += F("<input type='submit' value='Save'>");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  page += F("<p><small>Settings apply straight away. Changing invert or flip restarts the device.</small></p>");
#else
  page += F("<p><small>Settings apply straight away, no restart.</small></p>");
#endif
  page += F("</form><p><small>Miner status as JSON, no login needed: <a href='/api/status'>/api/status</a></small></p>");
  page += F("<hr style='margin-top:1.5em;border:0;border-top:1px solid #ddd'><div style='text-align:center;margin:1em 0'><a href='/update' style='color:#f59e0b;font-weight:bold;text-decoration:none'>⚡ Firmware Updates (OTA) &rarr;</a></div>");
#ifdef HAS_BACKLIGHT_CONTROL
  page += F("<script>var f=document.forms[0],c=f.city,a=f.lat,o=f.lon;"
            "c.onchange=function(){if(c.value){var p=c.value.split(',');a.value=p[0];o.value=p[1]}};"
            "a.oninput=o.oninput=function(){c.value=''};</script>");
#endif
  page += F("</body></html>");
  flush();
  webCfgServer.sendContent(""); // ends the chunked response
}

bool applyLiveSettings(const TSettings &before, const TSettings &next, bool &poolChangedOut);

static void handleSave()
{
  if (!checkAuth())
    return;

  // The form goes into a copy. Most of it is then applied live, each part by
  // the task that reads it; only invert/flip still need a restart (they are
  // set when the display starts).
  const TSettings before = currentSettings();
  TSettings next = before;

  // New page password: checked first, so a typo saves nothing at all.
  const String webPass = webCfgServer.arg("webpass");
  if (!webPass.isEmpty() || !webCfgServer.arg("webpass2").isEmpty())
  {
    const char *problem = webPass != webCfgServer.arg("webpass2") ? "The two new passwords are not the same."
                          : webPass.length() > WEBCFG_PASS_MAX   ? "The new password is longer than 63 characters."
                                                                  : nullptr;
    if (problem)
    {
      webCfgServer.send(400, "text/html",
                        String("<!DOCTYPE html><html><head><meta charset='utf-8'>"
                               "<meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                               "<body style='font-family:sans-serif'><p>") + problem +
                            " Nothing was saved.</p><p><a href='/'>Back to settings</a></p></body></html>");
      return;
    }
    next.WebPassword = webPass;
  }

  if (webCfgServer.hasArg("pool"))
    next.PoolAddress = webCfgServer.arg("pool");
  if (webCfgServer.hasArg("port"))
    next.PoolPort = webCfgServer.arg("port").toInt();
  if (webCfgServer.hasArg("poolpass"))
    strncpy(next.PoolPassword, webCfgServer.arg("poolpass").c_str(), sizeof(next.PoolPassword) - 1);
  if (webCfgServer.hasArg("wallet"))
    strncpy(next.BtcWallet, webCfgServer.arg("wallet").c_str(), sizeof(next.BtcWallet) - 1);
  if (webCfgServer.hasArg("bpool"))
  {
    next.BackupPoolAddress = webCfgServer.arg("bpool");
    next.BackupPoolAddress.trim();
  }
  if (webCfgServer.hasArg("bport"))
    next.BackupPoolPort = constrain(webCfgServer.arg("bport").toInt(), 0, 65535);
  if (webCfgServer.hasArg("bpoolpass"))
    strlcpy(next.BackupPoolPassword, webCfgServer.arg("bpoolpass").c_str(), sizeof(next.BackupPoolPassword));
  if (webCfgServer.hasArg("tz"))
    next.Timezone = webCfgServer.arg("tz");
  if (webCfgServer.hasArg("currency"))
    next.Currency = currencyFor(webCfgServer.arg("currency")).code;
  next.saveStats = webCfgServer.hasArg("savestats");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  next.invertColors = webCfgServer.hasArg("invert");
  next.flipDisplay = webCfgServer.hasArg("flip");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  next.autoBrightness = webCfgServer.hasArg("autobrightness");
#endif
  if (webCfgServer.hasArg("brightness"))
    next.Brightness = constrain(webCfgServer.arg("brightness").toInt(), 0, 255);
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  if (webCfgServer.hasArg("screenoff"))
    next.ScreenTimeout = constrain(webCfgServer.arg("screenoff").toInt(), 0, 1440);
  next.nightDim = webCfgServer.hasArg("nightdim");
  if (webCfgServer.hasArg("nightbright"))
    next.NightBrightness = constrain(webCfgServer.arg("nightbright").toInt(), 1, 255);
  if (webCfgServer.hasArg("nightfade"))
    next.NightFade = constrain(webCfgServer.arg("nightfade").toInt(), 1, 240);
  // A selected city is "lat,lon"; Custom is empty and the two fields count.
  const String city = webCfgServer.arg("city");
  const int comma = city.indexOf(',');
  const String lat = comma > 0 ? city.substring(0, comma) : webCfgServer.arg("lat");
  const String lon = comma > 0 ? city.substring(comma + 1) : webCfgServer.arg("lon");
  if (comma > 0 || webCfgServer.hasArg("lat"))
    next.Latitude = constrain(lat.toFloat(), -90.0f, 90.0f);
  if (comma > 0 || webCfgServer.hasArg("lon"))
    next.Longitude = constrain(lon.toFloat(), -180.0f, 180.0f);
#endif
#ifdef NO_DISPLAY
  next.statusLed = webCfgServer.hasArg("led");
#endif
#ifdef HAS_RGB_LED
  if (webCfgServer.hasArg("ledmode"))
    next.ledMode = constrain(webCfgServer.arg("ledmode").toInt(), LED_MODE_OFF, LED_MODE_STEADY);
  const String ledColor = webCfgServer.arg("ledcolor"); // "#rrggbb"
  if (ledColor.length() == 7 && ledColor[0] == '#')
    next.ledColor = strtoul(ledColor.c_str() + 1, NULL, 16) & 0xFFFFFF;
  if (webCfgServer.hasArg("ledbright"))
    next.ledBrightness = constrain(webCfgServer.arg("ledbright").toInt(), 1, 255);
  next.ledScreenOff = webCfgServer.hasArg("ledscreen");
#endif

  const bool passChanged = next.WebPassword != before.WebPassword;
  bool poolChanged = false;
  bool restart = applyLiveSettings(before, next, poolChanged);

  if (!restart)
  {
    String msg = poolChanged ? F("<p>Saved. Reconnecting to the pool with the new settings...</p>")
                             : F("<p>Saved and applied.</p>");
    if (passChanged)
      msg += F("<p>Password changed: log in again with the new one (user admin).</p>");
    webCfgServer.send(200, "text/html",
                      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                      "<meta http-equiv='refresh' content='3;url=/'></head>"
                      "<body style='font-family:sans-serif'>" + msg +
                      "<p><a href='/'>Back to settings</a></p></body></html>");
    return;
  }

  // The page polls until the device is back up, then returns to the settings
  // page so the browser isn't left sitting on /save.
  webCfgServer.send(200, "text/html",
                     "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                     "<meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                     "<body style='font-family:sans-serif'>"
                     "<p>Saved. Restarting device...</p>"
                     "<p><a href='/'>Back to settings</a></p>"
                     "<script>function p(){fetch('/',{cache:'no-store'})"
                     ".then(function(r){if(r.ok||r.status==401)location.replace('/');else setTimeout(p,1500)})"
                     ".catch(function(){setTimeout(p,1500)})}setTimeout(p,4000);</script>"
                     "</body></html>");

  delay(1000);
  ESP.restart();
}

bool applyLiveSettings(const TSettings &before, const TSettings &next, bool &poolChangedOut)
{
  saveSettingsToFlash(const_cast<TSettings*>(&next));

  const bool restart = next.invertColors != before.invertColors || next.flipDisplay != before.flipDisplay;
  if (!restart)
  {
    const bool poolChanged = next.PoolAddress != before.PoolAddress || next.PoolPort != before.PoolPort ||
                             strcmp(next.BtcWallet, before.BtcWallet) != 0 ||
                             strcmp(next.PoolPassword, before.PoolPassword) != 0;
    const bool backupChanged = next.BackupPoolAddress != before.BackupPoolAddress ||
                               next.BackupPoolPort != before.BackupPoolPort ||
                               strcmp(next.BackupPoolPassword, before.BackupPoolPassword) != 0;
    poolChangedOut = poolChanged || backupChanged;
    if (poolChanged || backupChanged)
      requestPoolChange(next); // stratum task reconnects if the pool in use changed
    Settings.WebPassword = next.WebPassword;
    if (!next.Currency.equalsIgnoreCase(before.Currency))
      requestCurrencyChange(currencyFor(next.Currency).code); // monitor task
    Settings.Timezone = next.Timezone; // monitor task moves the clock
    Settings.saveStats = next.saveStats;
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
    Settings.autoBrightness = next.autoBrightness;
#endif
#ifdef HAS_BACKLIGHT_CONTROL
    // backlightTick() on the monitor task sets the backlight from these
    // within a second.
    Settings.Brightness = next.Brightness;
    Settings.nightDim = next.nightDim;
    Settings.NightBrightness = next.NightBrightness;
    Settings.NightFade = next.NightFade;
    Settings.Latitude = next.Latitude;
    Settings.Longitude = next.Longitude;
    if (next.ScreenTimeout != before.ScreenTimeout)
    {
      Settings.ScreenTimeout = next.ScreenTimeout;
      wakeScreen(); // back on if it had timed out; the new timeout counts from now
    }
#endif
#ifdef NO_DISPLAY
    Settings.statusLed = next.statusLed;
#endif
#ifdef HAS_RGB_LED
    // rgbLedTick() on the monitor task picks these up on its next pass.
    Settings.ledMode = next.ledMode;
    Settings.ledColor = next.ledColor;
    Settings.ledBrightness = next.ledBrightness;
    Settings.ledScreenOff = next.ledScreenOff;
#endif
    return false; // no restart needed
  }
  return true; // restart needed
}

static void handleGetConfig()
{
  const TSettings s = currentSettings();
  const TimeZoneInfo* tzInfo = findTimeZone(s.Timezone);
  String tzPosix = getPosixTz(s.Timezone);
  String tzLabel = tzInfo ? String(tzInfo->label) : s.Timezone;

  unsigned long h = 0, min = 0, sec = 0;
  getTime(&h, &min, &sec);
  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu:%02lu", h, min, sec);

  struct tm timeinfo;
  bool dstActive = false;
  if (getLocalTime(&timeinfo, 10)) {
    dstActive = timeinfo.tm_isdst > 0;
  }

  String json;
  json.reserve(1024);
  json += "{";
  json += "\"pool\":\"" + htmlEscape(s.PoolAddress) + "\"";
  json += ",\"port\":" + String(s.PoolPort);
  json += ",\"wallet\":\"" + htmlEscape(String(s.BtcWallet)) + "\"";
  json += ",\"hostname\":\"" + htmlEscape(String(s.Hostname)) + "\"";
  json += ",\"poolPassword\":\"" + htmlEscape(String(s.PoolPassword)) + "\"";
  json += ",\"backupPool\":\"" + htmlEscape(s.BackupPoolAddress) + "\"";
  json += ",\"backupPort\":" + String(s.BackupPoolPort);
  json += ",\"currency\":\"" + htmlEscape(s.Currency) + "\"";
  json += ",\"timezone\":\"" + htmlEscape(s.Timezone) + "\"";
  json += ",\"timezoneLabel\":\"" + htmlEscape(tzLabel) + "\"";
  json += ",\"timezonePosix\":\"" + htmlEscape(tzPosix) + "\"";
  json += ",\"localTime\":\"" + String(timeBuf) + "\"";
  json += ",\"localDate\":\"" + getDate() + "\"";
  json += ",\"dstActive\":" + String(dstActive ? "true" : "false");
  json += ",\"saveStats\":" + String(s.saveStats ? "true" : "false");
  json += ",\"brightness\":" + String(s.Brightness);
  json += ",\"autoBrightness\":" + String(s.autoBrightness ? "true" : "false");
  json += ",\"screenTimeout\":" + String(s.ScreenTimeout);
  json += ",\"nightDim\":" + String(s.nightDim ? "true" : "false");
  json += ",\"nightBrightness\":" + String(s.NightBrightness);
  json += ",\"nightFade\":" + String(s.NightFade);
  json += ",\"latitude\":" + String(s.Latitude, 4);
  json += ",\"longitude\":" + String(s.Longitude, 4);
  json += ",\"invertColors\":" + String(s.invertColors ? "true" : "false");
  json += ",\"flipDisplay\":" + String(s.flipDisplay ? "true" : "false");
#ifdef NO_DISPLAY
  json += ",\"statusLed\":" + String(s.statusLed ? "true" : "false");
#endif
#ifdef HAS_RGB_LED
  json += ",\"ledMode\":" + String(s.ledMode);
  json += ",\"ledColor\":" + String(s.ledColor);
  json += ",\"ledBrightness\":" + String(s.ledBrightness);
  json += ",\"ledScreenOff\":" + String(s.ledScreenOff ? "true" : "false");
#endif
  // Bitaxe / AxeOS / NMMiner / Wotcha aliases for seamless integration:
  json += ",\"stratumURL\":\"" + htmlEscape(s.PoolAddress) + "\"";
  json += ",\"stratumPort\":" + String(s.PoolPort);
  json += ",\"stratumUser\":\"" + htmlEscape(String(s.BtcWallet)) + "\"";
  json += ",\"PrimaryPool\":\"" + htmlEscape(s.PoolAddress) + "\"";
  json += ",\"PrimaryAddress\":\"" + htmlEscape(String(s.BtcWallet)) + "\"";
  json += ",\"version\":\"" + String(CURRENT_VERSION) + "\"";
  json += "}";

  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, PATCH, OPTIONS");
  webCfgServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
  webCfgServer.send(200, "application/json", json);
}

static void handleTimezoneEndpoint()
{
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_OPTIONS) {
    webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
    webCfgServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, PATCH, OPTIONS");
    webCfgServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    webCfgServer.send(204);
    return;
  }

  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    String newTz = "";
    if (webCfgServer.hasArg("plain")) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, webCfgServer.arg("plain"))) {
        if (doc.containsKey("timezone")) newTz = doc["timezone"].as<String>();
        else if (doc.containsKey("tz")) newTz = doc["tz"].as<String>();
        else if (doc.containsKey("posix")) newTz = doc["posix"].as<String>();
      }
    }
    if (newTz.isEmpty() && webCfgServer.hasArg("timezone")) newTz = webCfgServer.arg("timezone");
    if (newTz.isEmpty() && webCfgServer.hasArg("tz")) newTz = webCfgServer.arg("tz");

    if (newTz.length() > 0) {
      TSettings next = currentSettings();
      next.Timezone = newTz;
      bool poolChanged = false;
      applyLiveSettings(currentSettings(), next, poolChanged);
    }
  }

  const TSettings s = currentSettings();
  const TimeZoneInfo* info = findTimeZone(s.Timezone);
  String posix = getPosixTz(s.Timezone);
  String label = info ? String(info->label) : s.Timezone;

  unsigned long h = 0, min = 0, sec = 0;
  getTime(&h, &min, &sec);
  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu:%02lu", h, min, sec);

  struct tm timeinfo;
  bool dstActive = false;
  if (getLocalTime(&timeinfo, 10)) {
    dstActive = timeinfo.tm_isdst > 0;
  }

  String json;
  json.reserve(320);
  json += "{\"timezone\":\"" + htmlEscape(s.Timezone) + "\"";
  json += ",\"label\":\"" + htmlEscape(label) + "\"";
  json += ",\"posix\":\"" + htmlEscape(posix) + "\"";
  json += ",\"localTime\":\"" + String(timeBuf) + "\"";
  json += ",\"localDate\":\"" + getDate() + "\"";
  json += ",\"dstActive\":" + String(dstActive ? "true" : "false");
  json += "}";

  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, PATCH, OPTIONS");
  webCfgServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
  webCfgServer.send(200, "application/json", json);
}

static void handleTimezonesList()
{
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  webCfgServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  if (webCfgServer.method() == HTTP_OPTIONS) {
    webCfgServer.send(204);
    return;
  }

  webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webCfgServer.send(200, "application/json", "[");
  for (int i = 0; i < kTimeZoneCount; i++) {
    String entry;
    entry.reserve(160);
    if (i > 0) entry += ",";
    entry += "{\"id\":\"";
    entry += kTimeZones[i].id;
    entry += "\",\"label\":\"";
    entry += kTimeZones[i].label;
    entry += "\",\"posix\":\"";
    entry += kTimeZones[i].posix;
    entry += "\"}";
    webCfgServer.sendContent(entry);
  }
  webCfgServer.sendContent("]");
  webCfgServer.sendContent("");
}

static void handlePostConfig()
{
  const TSettings before = currentSettings();
  TSettings next = before;

  StaticJsonDocument<1024> doc;
  DeserializationError err = DeserializationError::EmptyInput;
  if (webCfgServer.hasArg("plain")) {
    err = deserializeJson(doc, webCfgServer.arg("plain"));
  }

  if (err) {
    for (int i = 0; i < webCfgServer.args(); i++) {
      String name = webCfgServer.argName(i);
      if (name != "plain") {
        doc[name] = webCfgServer.arg(i);
      }
    }
  }

  nvMemory::parseJsonConfig(doc, &next);

  bool poolChanged = false;
  bool restart = applyLiveSettings(before, next, poolChanged);

  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, PATCH, OPTIONS");
  webCfgServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

  if (restart) {
    webCfgServer.send(200, "application/json", "{\"success\":true,\"message\":\"Settings saved. Restarting device for display changes...\",\"rebootRequired\":true}");
    delay(1000);
    ESP.restart();
  } else {
    String resp = "{\"success\":true,\"message\":\"Settings saved and applied live.\",\"rebootRequired\":false";
    if (poolChanged) resp += ",\"poolChanged\":true";
    resp += "}";
    webCfgServer.send(200, "application/json", resp);
  }
}

static void handleConfigRoute()
{
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_OPTIONS) {
    webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
    webCfgServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, PATCH, OPTIONS");
    webCfgServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    webCfgServer.send(204);
    return;
  }
  if (m == HTTP_GET) {
    handleGetConfig();
    return;
  }
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    handlePostConfig();
    return;
  }
  webCfgServer.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
}

static void handleProbe()
{
  char best_diff_str[16] = {0};
  suffix_string(best_diff, best_diff_str, sizeof(best_diff_str), 0);

  double khs = getHashrateKhs();
  uint32_t hr_hs = (uint32_t)(khs * 1000.0);

  String json;
  json.reserve(128);
  json += "{\"hr\":" + String(hr_hs);
  json += ",\"ut\":" + String((unsigned long)upTime);
  json += ",\"ebd\":\"" + String(best_diff_str) + "\"";
  json += ",\"sbd\":\"" + String(best_diff_str) + "\"";
  json += ",\"temp\":" + String(temperatureRead(), 1);
  json += "}";

  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.send(200, "application/json", json);
}

static void handleStatus()
{
  char best_diff_str[16] = {0};
  suffix_string(best_diff, best_diff_str, sizeof(best_diff_str), 0);

  double khs = getHashrateKhs();
  float temp = temperatureRead();
  const MinerStatus m = getMinerStatus();

  String json;
  json.reserve(1024);
  json += "{\"nerdminer\":true";
  json += ",\"hostname\":\"" + htmlEscape(String(Settings.Hostname)) + "\"";
  json += ",\"version\":\"" + String(CURRENT_VERSION) + "\"";
  json += ",\"deviceModel\":\"NerdMiner v2\"";
  json += ",\"khash\":" + String(khs, 2);
  json += ",\"hashRate\":" + String(khs, 2);
  json += ",\"validShares\":" + String(shares);
  json += ",\"invalidShares\":" + String(rejects);
  json += ",\"sharesAccepted\":" + String(m.accepted);
  json += ",\"sharesRejected\":" + String(m.rejected);
  json += ",\"validBlocks\":" + String(valids);
  json += ",\"blockFound\":" + String(valids > 0 ? "true" : "false");
  json += ",\"bestDiff\":\"" + String(best_diff_str) + "\"";
  json += ",\"bestSessionDiff\":\"" + String(best_diff_str) + "\"";
  json += ",\"uptime\":" + String((unsigned long)upTime);
  json += ",\"poolDiff\":" + String(m.poolDifficulty);
  json += ",\"stratumURL\":\"" + htmlEscape(activePoolAddress()) + "\"";
  json += ",\"stratumPort\":" + String(activePoolPort());
  json += ",\"stratumUser\":\"" + htmlEscape(String(Settings.BtcWallet)) + "\"";
  json += ",\"onBackupPool\":" + String(onBackupPool() ? "true" : "false");
  json += ",\"temp\":" + String(temp, 1);
  json += ",\"power\":1.5";
  json += ",\"frequency\":240";
  json += ",\"macAddr\":\"" + WiFi.macAddress() + "\"";
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());
  json += ",\"gitRepo\":\"aetherwraith/NerdMiner-Modded\"";
  json += ",\"changelogUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\"";
  json += ",\"releaseNotesUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\"";
  json += ",\"firmwareStatus\":{\"familyKey\":\"nerdminer\"";
  json += ",\"currentVersion\":\"" + String(CURRENT_VERSION) + "\"";
  json += ",\"latestVersion\":\"" + String(CURRENT_VERSION) + "\"";
  json += ",\"hasUpdate\":false";
  json += ",\"changelogUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\"";
  json += ",\"releaseNotesUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\"";
  json += ",\"releaseName\":\"NerdMiner Modded " + String(CURRENT_VERSION) + "\"";
  json += ",\"releaseDate\":\"2026-10-06\"";
  json += ",\"isNativeHardwareStatus\":true}";
  json += "}";

  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.send(200, "application/json", json);
}

static void handleRestart()
{
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.send(200, "application/json", "{\"success\":true,\"message\":\"Restarting...\"}");
  delay(500);
  ESP.restart();
}

// Miner state as JSON for Home Assistant, scripts and dashboards. No login:
// it is read-only and holds nothing the screen doesn't show, and no wallet
// address or passwords.
extern uint32_t templates, hashes, Mhashes;
extern monitor_data mMonitor;

static void handleApiStatus()
{
  const MinerStatus m = getMinerStatus();
  DynamicJsonDocument doc(1024);
  doc["version"] = CURRENT_VERSION;
  doc["uptime_s"] = (uint32_t)(millis() / 1000);
  doc["state"] = mMonitor.NerdStatus == NM_hashing ? "mining" : mMonitor.NerdStatus == NM_Connecting ? "connecting" : "starting";
  doc["hashrate_khs"] = round(m.hashrateKHs * 10.0) / 10.0;
  doc["hashrate_avg_khs"] = round(m.avgHashrateKHs * 10.0) / 10.0; // since boot
  doc["total_mhashes"] = Mhashes + hashes / 1000000; // also kept across restarts when stats are saved
  doc["mining_time_s"] = (uint32_t)upTime;
  doc["templates"] = templates;
  doc["best_diff"] = best_diff;
  doc["shares_32bit"] = shares;
  doc["blocks_found"] = valids;
  doc["shares_accepted"] = m.accepted; // since boot
  doc["shares_rejected"] = m.rejected;
  JsonObject pool = doc.createNestedObject("pool");
  pool["address"] = activePoolAddress();
  pool["port"] = activePoolPort();
  pool["backup"] = onBackupPool();
  pool["subscribed"] = m.subscribed;
  pool["difficulty"] = m.poolDifficulty;
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();
  doc["free_heap"] = ESP.getFreeHeap();

  String out;
  serializeJson(doc, out);
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Cache-Control", "no-store");
  webCfgServer.send(200, "application/json", out);
}

void setup_webConfig(void)
{
  if (webCfgStarted)
    return;

  webCfgServer.on("/", HTTP_GET, handleRoot);
  webCfgServer.on("/save", HTTP_POST, handleSave);
  webCfgServer.on("/status", HTTP_GET, handleStatus);
  webCfgServer.on("/api/status", HTTP_GET, handleApiStatus);
  webCfgServer.on("/probe", HTTP_GET, handleProbe);
  webCfgServer.on("/api/system/info", HTTP_GET, handleStatus);
  webCfgServer.on("/api/system/restart", HTTP_POST, handleRestart);
  webCfgServer.on("/api/system/config", handleConfigRoute);
  webCfgServer.on("/api/config", handleConfigRoute);
  webCfgServer.on("/api/system", handleConfigRoute);
  webCfgServer.on("/api/setting/mining", handleConfigRoute);
  webCfgServer.on("/api/system/timezone", handleTimezoneEndpoint);
  webCfgServer.on("/api/timezone", handleTimezoneEndpoint);
  webCfgServer.on("/api/system/timezones", handleTimezonesList);
  webCfgServer.on("/api/timezones", handleTimezonesList);
  setup_ota(webCfgServer);
  webCfgServer.onNotFound([]()
  {
    webCfgServer.sendHeader("Location", "/");
    webCfgServer.send(302, "text/plain", "");
  });
  webCfgServer.begin();
  webCfgStarted = true;

  Serial.print("[WEBCFG] Settings page available at http://");
  Serial.println(WiFi.localIP());
}

void webConfigProcess(void)
{
  if (webCfgStarted) {
    webCfgServer.handleClient();
    otaProcess();
  }
}
