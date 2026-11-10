#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_timer.h>
#include "webConfig.h"
#include "version.h"
#include "wManager.h"
#include "mining.h"
#include "monitor.h"
#include "mqttPublisher.h"
#include "drivers/devices/device.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include "currency.h"
#include "timezone.h"
#include "utils.h"
#include "cities.h"
#include "drivers/storage/nvMemory.h"
#include "ota.h"
#include "webMonitorPage.h"
#ifdef M5STICK_C_PLUS2
#include "m5stickPlus2Hw.h"
#endif

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
static bool s_corsAdded = false;

static void ensureCorsHeaders()
{
  if (s_corsAdded)
    return;
  s_corsAdded = true;
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");
  webCfgServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, PATCH, PUT, OPTIONS");
  webCfgServer.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

static String currentWebPassword()
{
  std::lock_guard<std::mutex> lock(g_settingsMutex);
  return Settings.WebPassword;
}

static bool checkAuth()
{
  const String pw = currentWebPassword();
  if (!webCfgServer.authenticate(WEBCFG_USER, pw.c_str()))
  {
    webCfgServer.requestAuthentication();
    return false;
  }
  return true;
}

static bool parseIpv4Octets(const String &host, int out[4])
{
  int start = 0;
  for (int i = 0; i < 4; i++)
  {
    int dot = (i < 3) ? host.indexOf('.', start) : (int)host.length();
    if (dot <= start || dot - start > 3)
      return false;
    int val = 0;
    for (int k = start; k < dot; k++)
    {
      char c = host[k];
      if (c < '0' || c > '9')
        return false;
      val = val * 10 + (c - '0');
    }
    if (val < 0 || val > 255)
      return false;
    out[i] = val;
    start = dot + 1;
  }
  return start == (int)host.length() + 1;
}

static bool isLanOriginAllowed()
{
  String origin = webCfgServer.header("Origin");
  origin.trim();
  if (origin.isEmpty() || origin.equalsIgnoreCase("null"))
    return true;

  const String hostHdr = webCfgServer.hostHeader();
  if (hostHdr.length() > 0)
  {
    if (origin.equalsIgnoreCase("http://" + hostHdr) || origin.equalsIgnoreCase("https://" + hostHdr))
      return true;
  }

  String rest;
  if (origin.startsWith("http://") || origin.startsWith("HTTP://"))
    rest = origin.substring(7);
  else if (origin.startsWith("https://") || origin.startsWith("HTTPS://"))
    rest = origin.substring(8);
  else
    return false;

  int slash = rest.indexOf('/');
  if (slash >= 0)
    rest = rest.substring(0, slash);
  int colon = rest.lastIndexOf(':');
  if (colon > 0 && rest.indexOf(':') == colon)
    rest = rest.substring(0, colon);
  rest.toLowerCase();
  if (rest.isEmpty())
    return false;

  if (rest == "localhost" || rest.endsWith(".localhost") ||
      rest.endsWith(".local") || rest.endsWith(".lan") ||
      rest.endsWith(".home") || rest.endsWith(".internal") ||
      rest.endsWith(".localdomain"))
  {
    return true;
  }

  int oct[4] = {0, 0, 0, 0};
  if (parseIpv4Octets(rest, oct))
  {
    if (oct[0] == 127 || oct[0] == 10)
      return true;
    if (oct[0] == 192 && oct[1] == 168)
      return true;
    if (oct[0] == 172 && oct[1] >= 16 && oct[1] <= 31)
      return true;
    if (oct[0] == 169 && oct[1] == 254)
      return true;
    return false;
  }

  // Single-label LAN hostnames (e.g. http://nm1 .. http://nm9 on local router DNS) have no TLD dot
  if (rest.indexOf('.') < 0)
    return true;

  return false;
}

bool isWebApiMutationPermitted()
{
  if (!isLanOriginAllowed())
    return false;
  const String pw = currentWebPassword();
  if (pw.length() > 0 && pw != DEFAULT_WEBPASSWORD)
    return webCfgServer.authenticate(WEBCFG_USER, pw.c_str());
  return true;
}

bool checkWebApiMutationAllowed()
{
  ensureCorsHeaders();
  if (!isLanOriginAllowed())
  {
    webCfgServer.send(403, "application/json", "{\"success\":false,\"error\":\"Forbidden: external cross-origin request refused\"}");
    s_corsAdded = false;
    return false;
  }
  const String pw = currentWebPassword();
  if (pw.length() > 0 && pw != DEFAULT_WEBPASSWORD)
  {
    if (!webCfgServer.authenticate(WEBCFG_USER, pw.c_str()))
    {
      webCfgServer.requestAuthentication();
      s_corsAdded = false;
      return false;
    }
  }
  return true;
}

// A form posted from another site still carries the Basic auth the browser
// has cached for this page, so /save only takes requests whose Origin (or,
// without one, Referer) is this device. Browsers send Origin with every POST;
// a request with neither header (curl, scripts) is let through.
static bool sameOrigin()
{
  const String self = "http://" + webCfgServer.hostHeader();
  const String origin = webCfgServer.header("Origin");
  if (origin.length())
    return origin.equalsIgnoreCase(self);
  const String referer = webCfgServer.header("Referer");
  if (referer.length())
    return referer.equalsIgnoreCase(self) || referer.startsWith(self + "/");
  return true;
}

static String htmlEscape(const String &in)
{
  String out = in;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("'", "&#39;"); // the page puts every value between single quotes
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
             "<h2>NerdMiner Settings</h2>");
  if (Settings.WebPassword == DEFAULT_WEBPASSWORD)
    page += F("<p style='background:#fff3cd;color:#664d03;padding:.6em;border-radius:4px'>"
              "This page still uses the default password, the same on every NerdMiner: "
              "set your own at the bottom of the page.</p>");
  page += F("<form method='POST' action='/save'>");

  page += "<label>Pool URL</label><input type='text' name='pool' value='" + htmlEscape(Settings.PoolAddress) + "'>";
  page += "<label>Pool Port</label><input type='number' name='port' value='" + String(Settings.PoolPort) + "'>";
  page += "<label>Pool Password (optional)</label><input type='text' name='poolpass' value='" + htmlEscape(String(Settings.PoolPassword)) + "'>";
  page += "<label>BTC Address</label><input type='text' name='wallet' value='" + htmlEscape(String(Settings.BtcWallet)) + "'>";
  page += "<label>Hostname</label><input type='text' name='hostname' maxlength='31' value='" + htmlEscape(String(Settings.Hostname)) + "'>";
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
#ifdef TDISPLAY_PULSE
#ifdef M5STICK_C_PLUS2
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (rotate 180 degrees)</label>";
#else
  page += "<label><input type='checkbox' name='flip' " + String(Settings.flipDisplay ? "checked" : "") + "> Flip display (USB on the left)</label>";
#endif
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM && !defined(TDISPLAY_PULSE)
  page += "<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='" + String(Settings.Brightness) + "'>";
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  page += "<label>Screen off after (minutes without a touch or button press, 0 = never)</label><input type='number' name='screenoff' min='0' max='1440' value='" + String(Settings.ScreenTimeout) + "'>";
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
  flush();
#ifndef NO_MQTT
  page += F("<h3>MQTT (Home Assistant)</h3>");
  page += "<label><input type='checkbox' name='mqtt' " + String(Settings.MqttEnabled ? "checked" : "") +
          "> Publish the miner's state to an MQTT broker</label>";
  page += "<label>Broker address (IP or name)</label><input type='text' name='mqhost' maxlength='64' value='" + htmlEscape(Settings.MqttHost) + "'>";
  page += "<label>Broker port</label><input type='number' name='mqport' min='1' max='65535' value='" + String(Settings.MqttPort) + "'>";
  page += "<label>User (optional)</label><input type='text' name='mquser' maxlength='64' autocomplete='off' value='" + htmlEscape(Settings.MqttUser) + "'>";
  // The saved password is never sent back to the browser: empty keeps it.
  page += String("<label>Password (optional)</label><input type='password' name='mqpass' maxlength='64' autocomplete='new-password' placeholder='") +
          (Settings.MqttPass.length() ? "saved, leave empty to keep it" : "none") + "'>";
  if (Settings.MqttPass.length())
    page += F("<label><input type='checkbox' name='mqpassclear'> Remove the saved password</label>");
  page += "<label>Topic (empty: nerdminer/ and the end of the MAC address)</label><input type='text' name='mqtopic' maxlength='64' value='" + htmlEscape(Settings.MqttTopic) + "'>";
  page += String("<p><small>") + (mqttConnected() ? "Connected to the broker." : Settings.MqttEnabled ? "Not connected to the broker." : "Off.") +
          " Home Assistant finds the sensors by itself (MQTT discovery). Plain MQTT only (usually port 1883), no TLS.</small></p>";
#endif
  page += F("<label>New password for this page (leave empty to keep it)</label>"
            "<input type='password' name='webpass' maxlength='63' autocomplete='new-password'>"
            "<label>New password again</label>"
            "<input type='password' name='webpass2' maxlength='63' autocomplete='new-password'>");
  page += F("<input type='submit' value='Save'>");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  page += F("<p><small>Settings apply straight away. Changing invert or flip restarts the device.</small></p>");
#elif defined(TDISPLAY_PULSE)
  page += F("<p><small>Settings apply straight away. Changing flip restarts the device.</small></p>");
#else
  page += F("<p><small>Settings apply straight away, no restart.</small></p>");
#endif
  page += F("</form><p><small><a href='/'>Live monitor</a> (no login needed) &middot; status as JSON: <a href='/api/status'>/api/status</a></small></p>");
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
  if (!sameOrigin())
  {
    webCfgServer.send(403, "text/plain", "Refused: the form was not sent from this miner's own settings page.");
    return;
  }

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
                            " Nothing was saved.</p><p><a href='/settings'>Back to settings</a></p></body></html>");
      return;
    }
  }

  const TSettings before = currentSettings();
  TSettings next = before;

  const bool cityHasCoords = webCfgServer.arg("city").indexOf(',') > 0;
  DynamicJsonDocument doc(1536);
  for (int i = 0; i < webCfgServer.args(); i++)
  {
    const String k = webCfgServer.argName(i);
    if (k == "plain" || k == "webpass" || k == "webpass2" || k == "mqpass" || k == "mqpassclear")
      continue;
    if (cityHasCoords && (k == "lat" || k == "lon"))
      continue;
    String v = webCfgServer.arg(i);
    if (k == "hostname")
    {
      v.trim();
      if (v.isEmpty())
        continue;
    }
    if (k == "city" && v.isEmpty())
      continue;
    if (k == "mquser" || k == "mqtopic" || k == "mqhost")
      v = v.substring(0, 64);
    doc[k.c_str()] = v.c_str();
  }
  nvMemory::parseJsonConfig(doc, &next);

  // HTML checkboxes omit the form parameter when unchecked
  next.saveStats = webCfgServer.hasArg("savestats");
#ifndef NO_MQTT
  next.MqttEnabled = webCfgServer.hasArg("mqtt");
  if (webCfgServer.hasArg("mqhost"))
    next.MqttHost = mqttCleanHost(webCfgServer.arg("mqhost").substring(0, 64), next.MqttPort);
  if (webCfgServer.hasArg("mqpassclear"))
    next.MqttPass = "";
  else if (webCfgServer.arg("mqpass").length() > 0)
    next.MqttPass = webCfgServer.arg("mqpass").substring(0, 64);
#endif
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  next.invertColors = webCfgServer.hasArg("invert");
  next.flipDisplay = webCfgServer.hasArg("flip");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  next.autoBrightness = webCfgServer.hasArg("autobrightness");
#endif
#endif
#ifdef TDISPLAY_PULSE
  next.flipDisplay = webCfgServer.hasArg("flip");
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  next.nightDim = webCfgServer.hasArg("nightdim");
#endif
#ifdef NO_DISPLAY
  next.statusLed = webCfgServer.hasArg("led");
#endif
#ifdef HAS_RGB_LED
  next.ledScreenOff = webCfgServer.hasArg("ledscreen");
#endif
  if (!webPass.isEmpty())
    next.WebPassword = webPass;

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
                      "<meta http-equiv='refresh' content='3;url=/settings'></head>"
                      "<body style='font-family:sans-serif'>" + msg +
                      "<p><a href='/settings'>Back to settings</a></p></body></html>");
    return;
  }

  webCfgServer.send(200, "text/html",
                     "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                     "<meta name='viewport' content='width=device-width, initial-scale=1'></head>"
                     "<body style='font-family:sans-serif'>"
                     "<p>Saved. Restarting device...</p>"
                     "<p><a href='/settings'>Back to settings</a></p>"
                     "<script>function p(){fetch('/settings',{cache:'no-store'})"
                     ".then(function(r){if(r.ok||r.status==401)location.replace('/settings');else setTimeout(p,1500)})"
                     ".catch(function(){setTimeout(p,1500)})}setTimeout(p,4000);</script>"
                     "</body></html>");

  saveStat(); // stats mined since the last periodic save
  delay(1000);
  ESP.restart();
}

bool applyLiveSettings(const TSettings &before, const TSettings &next, bool &poolChangedOut)
{
  saveSettingsToFlash(const_cast<TSettings*>(&next));

  const bool wifiChanged = (next.WifiSSID != before.WifiSSID || next.WifiPW != before.WifiPW) && next.WifiSSID.length() > 0;
  if (wifiChanged)
  {
    {
      std::lock_guard<std::mutex> lock(g_settingsMutex);
      Settings.WifiSSID = next.WifiSSID;
      Settings.WifiPW = next.WifiPW;
    }
    WiFi.begin(next.WifiSSID.c_str(), next.WifiPW.c_str());
  }

  const bool restart = next.invertColors != before.invertColors || next.flipDisplay != before.flipDisplay || wifiChanged;
  if (!restart)
  {
    const bool poolChanged = next.PoolAddress != before.PoolAddress || next.PoolPort != before.PoolPort ||
                             strcmp(next.BtcWallet, before.BtcWallet) != 0 ||
                             strcmp(next.PoolPassword, before.PoolPassword) != 0;
    const bool backupChanged = next.BackupPoolAddress != before.BackupPoolAddress ||
                               next.BackupPoolPort != before.BackupPoolPort ||
                               strcmp(next.BackupPoolPassword, before.BackupPoolPassword) != 0;
    const bool hostnameChanged = strcmp(next.Hostname, before.Hostname) != 0;
    const bool mqttChanged = next.MqttEnabled != before.MqttEnabled || next.MqttHost != before.MqttHost ||
                             next.MqttPort != before.MqttPort || next.MqttUser != before.MqttUser ||
                             next.MqttPass != before.MqttPass || next.MqttTopic != before.MqttTopic;
    const bool tzChanged = next.Timezone != before.Timezone;
    const bool timeoutChanged = next.ScreenTimeout != before.ScreenTimeout;

    poolChangedOut = poolChanged || backupChanged;

    {
      std::lock_guard<std::mutex> lock(g_settingsMutex);
      if (hostnameChanged)
        strlcpy(Settings.Hostname, next.Hostname, sizeof(Settings.Hostname));
      Settings.WebPassword = next.WebPassword;
      if (mqttChanged)
      {
        Settings.MqttEnabled = next.MqttEnabled;
        Settings.MqttHost = next.MqttHost;
        Settings.MqttPort = next.MqttPort;
        Settings.MqttUser = next.MqttUser;
        Settings.MqttPass = next.MqttPass;
        Settings.MqttTopic = next.MqttTopic;
      }
      Settings.Timezone = next.Timezone;
      Settings.saveStats = next.saveStats;
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
      Settings.autoBrightness = next.autoBrightness;
#endif
#ifdef HAS_BACKLIGHT_CONTROL
      Settings.Brightness = next.Brightness;
      Settings.nightDim = next.nightDim;
      Settings.NightBrightness = next.NightBrightness;
      Settings.NightFade = next.NightFade;
      Settings.Latitude = next.Latitude;
      Settings.Longitude = next.Longitude;
      Settings.ScreenTimeout = next.ScreenTimeout;
#endif
#ifdef NO_DISPLAY
      Settings.statusLed = next.statusLed;
#endif
#ifdef HAS_RGB_LED
      Settings.ledMode = next.ledMode;
      Settings.ledColor = next.ledColor;
      Settings.ledBrightness = next.ledBrightness;
      Settings.ledScreenOff = next.ledScreenOff;
#endif
    }

    if (poolChanged || backupChanged)
      requestPoolChange(next);
    if (hostnameChanged)
      WiFi.setHostname(next.Hostname);
    if (mqttChanged)
      mqttSettingsChanged();
    if (!next.Currency.equalsIgnoreCase(before.Currency))
      requestCurrencyChange(currencyFor(next.Currency).code);
    if (tzChanged)
    {
      String posix = getPosixTz(next.Timezone);
      setenv("TZ", posix.c_str(), 1);
      tzset();
    }
#ifdef HAS_BACKLIGHT_CONTROL
    if (timeoutChanged)
      wakeScreen();
#endif
    return false; // no restart needed
  }
  return true; // restart needed
}

static void sendCorsJson(int code, const String &json)
{
  ensureCorsHeaders();
  webCfgServer.send(code, "application/json", json);
  s_corsAdded = false;
}

static bool handleCorsOptions()
{
  if (webCfgServer.method() != HTTP_OPTIONS)
    return false;
  ensureCorsHeaders();
  webCfgServer.send(204);
  s_corsAdded = false;
  return true;
}

static void getTimezoneFields(const String &tz, String &labelOut, String &posixOut, String &localTimeOut, bool &dstOut)
{
  const TimeZoneInfo* info = findTimeZone(tz);
  posixOut = getPosixTz(tz);
  labelOut = info ? String(info->label) : tz;

  unsigned long h = 0, min = 0, sec = 0;
  getTime(&h, &min, &sec);
  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu:%02lu", h, min, sec);
  localTimeOut = String(timeBuf);

  struct tm timeinfo;
  dstOut = getLocalTime(&timeinfo, 10) && (timeinfo.tm_isdst > 0);
}

static String findCityName(float lat, float lon)
{
  for (int i = 0; i < kCityCount; i++)
  {
    if (fabsf(lat - kCities[i].latitude) < 0.005f && fabsf(lon - kCities[i].longitude) < 0.005f)
      return String(kCities[i].name);
  }
  return "";
}

#ifdef HAS_RGB_LED
static const char* ledModeName(uint8_t mode)
{
  switch (mode)
  {
    case LED_MODE_OFF: return "off";
    case LED_MODE_STEADY: return "steady";
    default: return "status";
  }
}

static String ledColorHex(uint32_t color)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06lx", (unsigned long)(color & 0xFFFFFF));
  return String(buf);
}
#endif

static void handleGetConfig()
{
  const TSettings s = currentSettings();
  String tzLabel, tzPosix, localTime;
  bool dstActive = false;
  getTimezoneFields(s.Timezone, tzLabel, tzPosix, localTime, dstActive);
  String cityName = findCityName(s.Latitude, s.Longitude);

  DynamicJsonDocument doc(2560);
  nvMemory::fillConfigJson(doc, s, false, true);
  doc["timezoneLabel"] = tzLabel;
  doc["timezonePosix"] = tzPosix;
  doc["localTime"] = localTime;
  doc["localDate"] = getDate();
  doc["dstActive"] = dstActive;
  doc["city"] = cityName;
#ifdef HAS_BACKLIGHT_CONTROL
  doc["nightDimStatus"] = nightDimStatus();
#endif
#ifdef HAS_RGB_LED
  doc["ledModeName"] = ledModeName((uint8_t)s.ledMode);
#endif
  doc["mqttConnected"] = mqttConnected();
  // Bitaxe / AxeOS / NMMiner / Wotcha aliases for seamless integration:
  doc["stratumURL"] = s.PoolAddress;
  doc["stratumPort"] = s.PoolPort;
  doc["stratumUser"] = s.BtcWallet;
  doc["fallbackStratumURL"] = s.BackupPoolAddress;
  doc["fallbackStratumPort"] = s.BackupPoolPort;
  doc["fallbackStratumPassword"] = s.BackupPoolPassword[0] != '\0' ? "********" : "";
  doc["PrimaryPool"] = s.PoolAddress;
  doc["PrimaryAddress"] = s.BtcWallet;
  doc["version"] = CURRENT_VERSION;

  String json;
  json.reserve(1400);
  serializeJson(doc, json);
  sendCorsJson(200, json);
}

static void handleTimezoneEndpoint()
{
  if (handleCorsOptions()) return;

  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    if (!checkWebApiMutationAllowed()) return;
    String newTz = "";
    if (webCfgServer.hasArg("plain")) {
      String body = webCfgServer.arg("plain");
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, body.c_str())) {
        if (doc.containsKey("timezone")) newTz = doc["timezone"].as<String>();
        else if (doc.containsKey("tz")) newTz = doc["tz"].as<String>();
        else if (doc.containsKey("posix")) newTz = doc["posix"].as<String>();
      }
    }
    if (newTz.isEmpty() && webCfgServer.hasArg("timezone")) newTz = webCfgServer.arg("timezone");
    if (newTz.isEmpty() && webCfgServer.hasArg("tz")) newTz = webCfgServer.arg("tz");

    if (newTz.length() > 0) {
      const TSettings before = currentSettings();
      TSettings next = before;
      next.Timezone = newTz;
      bool poolChanged = false;
      applyLiveSettings(before, next, poolChanged);
    }
  } else if (m != HTTP_GET) {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return;
  }

  const TSettings s = currentSettings();
  String label, posix, localTime;
  bool dstActive = false;
  getTimezoneFields(s.Timezone, label, posix, localTime, dstActive);

  String json;
  json.reserve(320);
  json += "{\"timezone\":\"" + jsonEscape(s.Timezone) + "\"";
  json += ",\"label\":\"" + jsonEscape(label) + "\"";
  json += ",\"posix\":\"" + jsonEscape(posix) + "\"";
  json += ",\"localTime\":\"" + jsonEscape(localTime) + "\"";
  json += ",\"localDate\":\"" + jsonEscape(getDate()) + "\"";
  json += ",\"dstActive\":" + String(dstActive ? "true" : "false");
  json += "}";

  sendCorsJson(200, json);
}

static void handleTimezonesList()
{
  if (handleCorsOptions()) return;
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");

  webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webCfgServer.send(200, "application/json", "[");
  for (int i = 0; i < kTimeZoneCount; i++) {
    String entry;
    entry.reserve(160);
    if (i > 0) entry += ",";
    entry += "{\"id\":\"";
    entry += jsonEscape(String(kTimeZones[i].id));
    entry += "\",\"label\":\"";
    entry += jsonEscape(String(kTimeZones[i].label));
    entry += "\",\"posix\":\"";
    entry += jsonEscape(String(kTimeZones[i].posix));
    entry += "\"}";
    webCfgServer.sendContent(entry);
  }
  webCfgServer.sendContent("]");
  webCfgServer.sendContent("");
}

static void handleCitiesList()
{
  if (handleCorsOptions()) return;
  webCfgServer.sendHeader("Access-Control-Allow-Origin", "*");

  webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webCfgServer.send(200, "application/json", "[");
  for (int i = 0; i < kCityCount; i++) {
    String entry;
    entry.reserve(96);
    if (i > 0) entry += ",";
    entry += "{\"name\":\"";
    entry += jsonEscape(String(kCities[i].name));
    entry += "\",\"latitude\":";
    entry += String(kCities[i].latitude, 2);
    entry += ",\"longitude\":";
    entry += String(kCities[i].longitude, 2);
    entry += "}";
    webCfgServer.sendContent(entry);
  }
  webCfgServer.sendContent("]");
  webCfgServer.sendContent("");
}

static bool applyRequestConfig(bool &poolChangedOut, bool &restartOut)
{
  const TSettings before = currentSettings();
  TSettings next = before;

  DynamicJsonDocument doc(1536);
  DeserializationError err = DeserializationError::EmptyInput;
  if (webCfgServer.hasArg("plain")) {
    String body = webCfgServer.arg("plain");
    err = deserializeJson(doc, body.c_str());
  }

  if (err) {
    for (int i = 0; i < webCfgServer.args(); i++) {
      String name = webCfgServer.argName(i);
      if (name != "plain") {
        doc[name.c_str()] = webCfgServer.arg(i).c_str();
      }
    }
  }

  nvMemory::parseJsonConfig(doc, &next);
  restartOut = applyLiveSettings(before, next, poolChangedOut);
  return true;
}

static void handlePostConfig()
{
  if (!checkWebApiMutationAllowed()) return;

  bool poolChanged = false;
  bool restart = false;
  applyRequestConfig(poolChanged, restart);

  if (restart) {
    sendCorsJson(200, "{\"success\":true,\"message\":\"Settings saved. Restarting device for changes...\",\"rebootRequired\":true}");
    saveStat();
    delay(1000);
    ESP.restart();
  } else {
    String resp = "{\"success\":true,\"message\":\"Settings saved and applied live.\",\"rebootRequired\":false";
    if (poolChanged) resp += ",\"poolChanged\":true";
    resp += "}";
    sendCorsJson(200, resp);
  }
}

static void handleConfigRoute()
{
  if (handleCorsOptions()) return;
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_GET) {
    handleGetConfig();
    return;
  }
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    handlePostConfig();
    return;
  }
  sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
}

static void handleLedEndpoint()
{
  if (handleCorsOptions()) return;
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    if (!checkWebApiMutationAllowed()) return;
    bool poolChanged = false, restart = false;
    applyRequestConfig(poolChanged, restart);
  } else if (m != HTTP_GET) {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return;
  }

  const TSettings s = currentSettings();
  String json;
  json.reserve(256);
  json += "{";
#ifdef HAS_RGB_LED
  json += "\"ledMode\":" + String(s.ledMode);
  json += ",\"ledModeName\":\"" + String(ledModeName((uint8_t)s.ledMode)) + "\"";
  json += ",\"ledColor\":" + String(s.ledColor);
  json += ",\"ledColorHex\":\"" + ledColorHex(s.ledColor) + "\"";
  json += ",\"ledBrightness\":" + String(s.ledBrightness);
  json += ",\"ledScreenOff\":" + String(s.ledScreenOff ? "true" : "false");
#elif defined(NO_DISPLAY)
  json += "\"statusLed\":" + String(s.statusLed ? "true" : "false");
#else
  json += "\"supported\":false";
#endif
  json += "}";
  sendCorsJson(200, json);
}

static void handleDisplayEndpoint()
{
  if (handleCorsOptions()) return;
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    if (!checkWebApiMutationAllowed()) return;

    String screenAction = "";
    bool hasConfigKeys = false;
    if (webCfgServer.hasArg("plain")) {
      String body = webCfgServer.arg("plain");
      StaticJsonDocument<512> doc;
      if (!deserializeJson(doc, body.c_str())) {
        if (doc.containsKey("screen")) {
          screenAction = doc["screen"].as<String>();
          screenAction.toLowerCase();
        }
        for (JsonPair kv : doc.as<JsonObject>()) {
          if (strcmp(kv.key().c_str(), "screen") != 0) {
            hasConfigKeys = true;
            break;
          }
        }
      }
    }
    if (screenAction.isEmpty() && webCfgServer.hasArg("screen")) {
      screenAction = webCfgServer.arg("screen");
      screenAction.toLowerCase();
    }
    for (int i = 0; i < webCfgServer.args(); i++) {
      String n = webCfgServer.argName(i);
      if (n != "plain" && n != "screen") {
        hasConfigKeys = true;
        break;
      }
    }

    if (screenAction.length() > 0) {
#ifdef HAS_BACKLIGHT_CONTROL
      if (screenAction == "toggle") {
        if (!wakeScreen()) sleepScreen();
      } else if (screenAction == "on" || screenAction == "wake") {
        wakeScreen();
      } else if (screenAction == "off" || screenAction == "sleep") {
        sleepScreen();
      }
#else
      if (screenAction == "toggle") alternateScreenState();
#endif
    }

    if (hasConfigKeys) {
      bool poolChanged = false, restart = false;
      applyRequestConfig(poolChanged, restart);
      if (restart) {
        sendCorsJson(200, "{\"success\":true,\"message\":\"Display settings saved. Restarting device...\",\"rebootRequired\":true}");
        saveStat();
        delay(1000);
        ESP.restart();
        return;
      }
    }
  } else if (m != HTTP_GET) {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return;
  }

  const TSettings s = currentSettings();
  String json;
  json.reserve(420);
  json += "{\"brightness\":" + String(s.Brightness);
  json += ",\"autoBrightness\":" + String(s.autoBrightness ? "true" : "false");
  json += ",\"screenTimeout\":" + String(s.ScreenTimeout);
  json += ",\"nightDim\":" + String(s.nightDim ? "true" : "false");
  json += ",\"nightBrightness\":" + String(s.NightBrightness);
  json += ",\"nightFade\":" + String(s.NightFade);
  json += ",\"city\":\"" + jsonEscape(findCityName(s.Latitude, s.Longitude)) + "\"";
  json += ",\"latitude\":" + String(s.Latitude, 4);
  json += ",\"longitude\":" + String(s.Longitude, 4);
#ifdef HAS_BACKLIGHT_CONTROL
  json += ",\"nightDimStatus\":\"" + jsonEscape(nightDimStatus()) + "\"";
#endif
  json += ",\"screenLit\":" + String(screenLit() ? "true" : "false");
  json += ",\"invertColors\":" + String(s.invertColors ? "true" : "false");
  json += ",\"flipDisplay\":" + String(s.flipDisplay ? "true" : "false");
  json += "}";
  sendCorsJson(200, json);
}

static void handlePoolEndpoint()
{
  if (handleCorsOptions()) return;
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    if (!checkWebApiMutationAllowed()) return;
    bool poolChanged = false, restart = false;
    applyRequestConfig(poolChanged, restart);
  } else if (m != HTTP_GET) {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return;
  }

  const TSettings s = currentSettings();
  String json;
  json.reserve(512);
  json += "{\"pool\":\"" + jsonEscape(s.PoolAddress) + "\"";
  json += ",\"port\":" + String(s.PoolPort);
  json += ",\"wallet\":\"" + jsonEscape(String(s.BtcWallet)) + "\"";
  json += ",\"poolPassword\":\"" + String(s.PoolPassword[0] != '\0' ? "********" : "") + "\"";
  json += ",\"backupPool\":\"" + jsonEscape(s.BackupPoolAddress) + "\"";
  json += ",\"backupPort\":" + String(s.BackupPoolPort);
  json += ",\"backupPoolPassword\":\"" + String(s.BackupPoolPassword[0] != '\0' ? "********" : "") + "\"";
  json += ",\"activePool\":\"" + jsonEscape(activePoolAddress()) + "\"";
  json += ",\"activePort\":" + String(activePoolPort());
  json += ",\"onBackupPool\":" + String(onBackupPool() ? "true" : "false");
  json += "}";
  sendCorsJson(200, json);
}

static void handleMqttEndpoint()
{
  if (handleCorsOptions()) return;
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT) {
    if (!checkWebApiMutationAllowed()) return;
    bool poolChanged = false, restart = false;
    applyRequestConfig(poolChanged, restart);
  } else if (m != HTTP_GET) {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return;
  }

  const TSettings s = currentSettings();
  String json;
  json.reserve(320);
  json += "{\"mqttEnabled\":" + String(s.MqttEnabled ? "true" : "false");
  json += ",\"mqttHost\":\"" + jsonEscape(s.MqttHost) + "\"";
  json += ",\"mqttPort\":" + String(s.MqttPort);
  json += ",\"mqttUser\":\"" + jsonEscape(s.MqttUser) + "\"";
  json += ",\"mqttTopic\":\"" + jsonEscape(s.MqttTopic) + "\"";
  json += ",\"mqttConnected\":" + String(mqttConnected() ? "true" : "false");
  json += "}";
  sendCorsJson(200, json);
}

static void handleIdentify()
{
  if (handleCorsOptions()) return;
  if (webCfgServer.method() != HTTP_POST) {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return;
  }
  if (!checkWebApiMutationAllowed()) return;

  uint32_t durationMs = 10000;
  if (webCfgServer.hasArg("plain")) {
    String body = webCfgServer.arg("plain");
    StaticJsonDocument<192> doc;
    if (!deserializeJson(doc, body.c_str()) && doc.containsKey("durationMs")) {
      long d = doc["durationMs"].as<long>();
      durationMs = (d <= 0) ? 0 : (uint32_t)constrain(d, 500L, 120000L);
    }
  } else if (webCfgServer.hasArg("durationMs")) {
    long d = webCfgServer.arg("durationMs").toInt();
    durationMs = (d <= 0) ? 0 : (uint32_t)constrain(d, 500L, 120000L);
  }

  triggerIdentify(durationMs);

  String host;
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    host = String(Settings.Hostname);
  }

  String json;
  json.reserve(160);
  json += "{\"success\":true";
  json += ",\"identifying\":" + String(isIdentifying() ? "true" : "false");
  json += ",\"durationMs\":" + String((unsigned long)durationMs);
  json += ",\"hostname\":\"" + jsonEscape(host) + "\"}";
  sendCorsJson(200, json);
}

static void handleProbe()
{
  if (handleCorsOptions()) return;
  char best_diff_str[16] = {0};
  char sess_diff_str[16] = {0};
  suffix_string(getBestDiff(), best_diff_str, sizeof(best_diff_str), 0);
  suffix_string(getSessionBestDiff(), sess_diff_str, sizeof(sess_diff_str), 0);

  double khs = getHashrateKhs();
  uint32_t hr_hs = (uint32_t)(khs * 1000.0);
  String host;
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    host = String(Settings.Hostname);
  }

  String json;
  json.reserve(192);
  json += "{\"hr\":" + String(hr_hs);
  json += ",\"ut\":" + String((unsigned long)upTime);
  json += ",\"ebd\":\"" + String(best_diff_str) + "\"";
  json += ",\"sbd\":\"" + String(sess_diff_str) + "\"";
  json += ",\"temp\":" + String(temperatureRead(), 1);
  json += ",\"nerdminer\":true";
  json += ",\"host\":\"" + jsonEscape(host) + "\"";
  json += ",\"ver\":\"" + String(CURRENT_VERSION) + "\"";
  json += "}";

  sendCorsJson(200, json);
}

static void handleStatus()
{
  if (handleCorsOptions()) return;
  char best_diff_str[16] = {0};
  char sess_diff_str[16] = {0};
  suffix_string(getBestDiff(), best_diff_str, sizeof(best_diff_str), 0);
  suffix_string(getSessionBestDiff(), sess_diff_str, sizeof(sess_diff_str), 0);

  double khs = getHashrateKhs();
  float temp = temperatureRead();
  const MinerStatus m = getMinerStatus();
  const TSettings s = currentSettings();

  String json;
  json.reserve(1024);
  json += "{\"nerdminer\":true";
  json += ",\"hostname\":\"" + jsonEscape(String(s.Hostname)) + "\"";
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
  json += ",\"bestSessionDiff\":\"" + String(sess_diff_str) + "\"";
  json += ",\"uptime\":" + String((unsigned long)upTime);
  json += ",\"poolDiff\":" + String(m.poolDifficulty);
  if (m.pingMs >= 0)
    json += ",\"pingMs\":" + String((long)m.pingMs);
  json += ",\"stratumURL\":\"" + jsonEscape(activePoolAddress()) + "\"";
  json += ",\"stratumPort\":" + String(activePoolPort());
  json += ",\"stratumUser\":\"" + jsonEscape(String(s.BtcWallet)) + "\"";
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

  sendCorsJson(200, json);
}

static void handleSystemRestart()
{
  if (handleCorsOptions()) return;
  if (webCfgServer.method() != HTTP_POST) {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return;
  }
  if (!checkWebApiMutationAllowed()) return;

  sendCorsJson(200, "{\"success\":true,\"message\":\"Restarting...\"}");
  saveStat();
  delay(500);
  ESP.restart();
}

// Miner state as JSON for Home Assistant, scripts and dashboards. No login:
// it is read-only and holds nothing the screen doesn't show, and no wallet
// address or passwords.
extern uint32_t templates;
extern uint64_t upTime;
extern volatile uint32_t shares, valids;
extern double best_diff;
extern monitor_data mMonitor;

// The miner's state as /api/status serves it; also the MQTT state message
// (mqttPublisher.cpp). Cheap: cached values only.
void fillMinerStatusJson(JsonDocument &doc)
{
  const MinerStatus st = getMinerStatus();
  const uint64_t totalHashes = getTotalHashes();

  doc["version"] = CURRENT_VERSION;
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    doc["hostname"] = Settings.Hostname;
  }
  doc["uptime_s"] = (uint32_t)(esp_timer_get_time() / 1000000ULL); // 64-bit clock: millis() wraps after 49.7 days
  doc["state"] = mMonitor.NerdStatus == NM_hashing ? "mining" : mMonitor.NerdStatus == NM_Connecting ? "connecting" : "starting";
  doc["hashrate_khs"] = round(st.hashrateKHs * 10.0) / 10.0;
  doc["hashrate_avg_khs"] = round(st.avgHashrateKHs * 10.0) / 10.0; // since boot
  doc["total_mhashes"] = (uint32_t)(totalHashes / 1000000ULL); // also kept across restarts when stats are saved
  doc["session_hashes"] = st.sessionHashes;
  doc["mining_time_s"] = (uint32_t)upTime;
  doc["templates"] = templates;
  doc["best_diff"] = st.bestDiff;
  doc["session_best_difficulty"] = st.sessionBestDiff;

  const double expectedBest = (double)totalHashes / 4294967296.0;
  doc["luck_pct"] = expectedBest > 0.0001 ? round((st.bestDiff / expectedBest) * 1000.0) / 10.0 : 0.0;
  const double expectedSess = (double)st.sessionHashes / 4294967296.0;
  doc["session_luck_pct"] = expectedSess > 0.0001 ? round((st.sessionBestDiff / expectedSess) * 1000.0) / 10.0 : 0.0;

  doc["shares_32bit"] = shares;                    // accepted by the pool
  doc["shares_32bit_rejected"] = shares32Rejected; // refused by the pool
  doc["blocks_found"] = valids;
  doc["shares_accepted"] = st.accepted; // since boot
  doc["shares_rejected"] = st.rejected;
  doc["identifying"] = isIdentifying();

  JsonObject pool = doc.createNestedObject("pool");
  pool["address"] = activePoolAddress(); // locks g_settingsMutex internally
  pool["port"] = activePoolPort();
  pool["backup"] = onBackupPool();
  pool["subscribed"] = st.subscribed;
  if (st.authorized >= 0)
    pool["authorized"] = st.authorized == 1; // missing: no answer yet
  pool["difficulty"] = st.poolDifficulty;
  if (st.pingMs >= 0)
    pool["ping_ms"] = st.pingMs;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();
  doc["free_heap"] = ESP.getFreeHeap();

  NotableShare ns[NOTABLE_SHARES_MAX];
  const size_t nShares = getNotableShares(ns, NOTABLE_SHARES_MAX);
  JsonArray arr = doc.createNestedArray("notable_shares");
  for (size_t i = 0; i < nShares; i++) {
    JsonObject o = arr.createNestedObject();
    o["difficulty"] = ns[i].difficulty;
    o["epoch"] = ns[i].epoch;
    o["uptime_s"] = ns[i].uptimeS;
    o["is_32bit"] = ns[i].is32Bit;
    o["is_block"] = ns[i].isBlock;
  }

  // Cached values only: nothing here may start an HTTPS fetch or an NTP
  // request (those run on the monitor task and pause the miner).
  JsonObject clock = doc.createNestedObject("clock");
  int hh, mi, ss, dd, mo, yy;
  if (getLocalClock(hh, mi, ss, dd, mo, yy)) {
    char buf[16];
    clock["synced"] = true;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mi, ss);
    clock["time"] = buf;
    snprintf(buf, sizeof(buf), "%02d/%02d/%04d", dd, mo, yy);
    clock["date"] = buf;
  } else {
    clock["synced"] = false;
  }
  JsonObject btc = doc.createNestedObject("btc");
  btc["price"] = getBTCpriceValue();                         // 0 = not fetched yet
  btc["prefix"] = currencyFor(currencySetting()).prefix;
  btc["block_height"] = getBlockHeightValue();               // 0 = not fetched yet
#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
  const int batMv = plus2BatteryCachedMillivolts();
  if (batMv > 0) {
    JsonObject bat = doc.createNestedObject("battery");
    bat["mv"] = batMv;
    bat["percent"] = plus2BatteryPercent(batMv);
  }
#endif
}

static void handleApiStatus()
{
  if (handleCorsOptions()) return;
  noteWebMonitorPoll(); // the live monitor (or a script) is watching
  DynamicJsonDocument doc(2560);
  fillMinerStatusJson(doc);
  String out;
  out.reserve(1024);
  serializeJson(doc, out);
  webCfgServer.sendHeader("Cache-Control", "no-store");
  sendCorsJson(200, out);
}

// --------------------------------------------------------------------------
// Live monitor (webMonitorPage.h) and its hashrate history: one sample every
// 30 s, the last hour, kept here in RAM (240 bytes).
// --------------------------------------------------------------------------
#define HISTORY_LEN 120
#define HISTORY_PERIOD_ms 30000
static uint16_t s_history[HISTORY_LEN]; // KH/s x 10
static uint8_t s_historyCount = 0, s_historyHead = 0;
static uint32_t s_historyLast = 0;

static void historyTick()
{
  if (millis() - s_historyLast < HISTORY_PERIOD_ms)
    return;
  s_historyLast = millis();
  uint32_t v = (uint32_t)(getMinerStatus().hashrateKHs * 10.0f + 0.5f);
  if (v > 65535) v = 65535;
  s_history[s_historyHead] = (uint16_t)v;
  s_historyHead = (s_historyHead + 1) % HISTORY_LEN;
  if (s_historyCount < HISTORY_LEN) s_historyCount++;
}

static void handleHistory()
{
  if (handleCorsOptions()) return;
  String out;
  out.reserve(16 + s_historyCount * 7);
  out = F("{\"interval_s\":");
  out += HISTORY_PERIOD_ms / 1000;
  out += F(",\"khs\":[");
  for (uint8_t i = 0; i < s_historyCount; i++) {
    const uint16_t v = s_history[(s_historyHead + HISTORY_LEN - s_historyCount + i) % HISTORY_LEN];
    if (i) out += ',';
    out += v / 10;
    out += '.';
    out += v % 10;
  }
  out += F("]}");
  webCfgServer.sendHeader("Cache-Control", "no-store");
  sendCorsJson(200, out);
}

static void handleMonitor()
{
  noteWebMonitorOpened(); // fetch the BTC price / block height if the screens never did
  webCfgServer.sendHeader("Content-Encoding", "gzip");
  webCfgServer.sendHeader("Cache-Control", "no-cache");
  webCfgServer.send_P(200, "text/html; charset=utf-8", (PGM_P)MONITOR_HTML_GZ, MONITOR_HTML_GZ_LEN);
}

void setup_webConfig(void)
{
  if (webCfgStarted)
    return;

  static const char *kHeaders[] = {"Origin", "Referer"}; // read by sameOrigin() & checkWebApiMutationAllowed()
  webCfgServer.collectHeaders(kHeaders, 2);
  webCfgServer.on("/", HTTP_GET, handleMonitor);            // live monitor + fleet UI (gzipped)
  webCfgServer.on("/settings", HTTP_GET, handleRoot);        // settings, login
  webCfgServer.on("/save", HTTP_POST, handleSave);
  webCfgServer.on("/status", handleStatus);
  webCfgServer.on("/api/status", handleApiStatus);
  webCfgServer.on("/api/history", handleHistory);
  webCfgServer.on("/probe", handleProbe);
  webCfgServer.on("/api/identify", handleIdentify);
  webCfgServer.on("/api/system/identify", handleIdentify);
  webCfgServer.on("/api/system/info", handleStatus);
  webCfgServer.on("/api/system/restart", handleSystemRestart);
  webCfgServer.on("/api/restart", handleSystemRestart);
  webCfgServer.on("/api/system/config", handleConfigRoute);
  webCfgServer.on("/api/config", handleConfigRoute);
  webCfgServer.on("/api/system", handleConfigRoute);
  webCfgServer.on("/api/setting/mining", handleConfigRoute);
  webCfgServer.on("/api/system/timezone", handleTimezoneEndpoint);
  webCfgServer.on("/api/timezone", handleTimezoneEndpoint);
  webCfgServer.on("/api/system/timezones", handleTimezonesList);
  webCfgServer.on("/api/timezones", handleTimezonesList);
  webCfgServer.on("/api/system/cities", handleCitiesList);
  webCfgServer.on("/api/cities", handleCitiesList);
  webCfgServer.on("/api/system/led", handleLedEndpoint);
  webCfgServer.on("/api/led", handleLedEndpoint);
  webCfgServer.on("/api/system/display", handleDisplayEndpoint);
  webCfgServer.on("/api/display", handleDisplayEndpoint);
  webCfgServer.on("/api/system/pool", handlePoolEndpoint);
  webCfgServer.on("/api/pool", handlePoolEndpoint);
  webCfgServer.on("/api/system/mqtt", handleMqttEndpoint);
  webCfgServer.on("/api/mqtt", handleMqttEndpoint);
  setup_ota(webCfgServer);
#ifdef TDISPLAY_UI_SCREENSHOT
  // Dev builds: the screens rendered at boot (tDisplayPulseDriver.cpp),
  // run-length encoded; tools/tdisplay_shots.py turns them into PNGs.
  webCfgServer.on("/shot", HTTP_GET, []()
  {
    extern bool tDisplayShot(int i, const char **name, const uint8_t **data, size_t *len);
    const char *name;
    const uint8_t *data;
    size_t len;
    if (!tDisplayShot(webCfgServer.arg("i").toInt(), &name, &data, &len))
    {
      webCfgServer.send(404, "text/plain", "no such shot");
      return;
    }
    webCfgServer.sendHeader("X-Shot-Name", name);
    webCfgServer.send_P(200, "application/octet-stream", (PGM_P)data, len);
  });
#endif
  // Browsers ask for it on every visit; without this it fell to onNotFound,
  // which redirected to "/" and sent the whole page again.
  webCfgServer.on("/favicon.ico", HTTP_GET, []() { webCfgServer.send(204); });
  webCfgServer.onNotFound([]()
  {
    if (handleCorsOptions())
      return;
    webCfgServer.sendHeader("Location", "/");
    webCfgServer.send(302, "text/plain", "");
  });
  webCfgServer.begin();
  webCfgStarted = true;

  Serial.print("[WEBCFG] Monitor at http://");
  Serial.print(WiFi.localIP());
  Serial.println("/  (settings: /settings)");
}

void webConfigProcess(void)
{
  historyTick(); // also before the server is up, so the graph has the full hour
  if (webCfgStarted) {
    s_corsAdded = false;
    webCfgServer.handleClient();
    otaProcess();
  }
}
