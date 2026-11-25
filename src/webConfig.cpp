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
  if (origin.isEmpty())
    return true;
  if (origin.equalsIgnoreCase("null"))
    return false;

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

struct ChunkedWriter : public Print
{
  char buf[512];
  size_t pos = 0;

  size_t write(uint8_t c) override
  {
    char ch = static_cast<char>(c);
    write(&ch, 1);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) override
  {
    if (buffer && size > 0)
      write(reinterpret_cast<const char *>(buffer), size);
    return size;
  }
  void flush() override
  {
    if (pos > 0)
    {
      buf[pos] = '\0';
      webCfgServer.sendContent(buf);
      pos = 0;
    }
  }
  void write(const char *s, size_t len)
  {
    while (len > 0)
    {
      if (pos == sizeof(buf) - 1)
        flush();
      size_t space = (sizeof(buf) - 1) - pos;
      size_t n = (len < space) ? len : space;
      memcpy(buf + pos, s, n);
      pos += n;
      s += n;
      len -= n;
    }
  }
  void write(const char *s) { if (s) write(s, strlen(s)); }
  void write(const String &s) { write(s.c_str(), s.length()); }
  void writeF(const __FlashStringHelper *fs)
  {
    const char *p = reinterpret_cast<const char *>(fs);
    write(p, strlen_P(p));
  }
  void writePrintf(const char *fmt, ...)
  {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
    {
      if (n < (int)sizeof(tmp))
      {
        write(tmp, (size_t)n);
      }
      else
      {
        String dyn;
        if (dyn.reserve((size_t)n + 1))
        {
          char *bufPtr = const_cast<char *>(dyn.c_str());
          vsnprintf(bufPtr, (size_t)n + 1, fmt, ap2);
          write(bufPtr, (size_t)n);
        }
        else
        {
          write(tmp, sizeof(tmp) - 1);
        }
      }
    }
    va_end(ap2);
  }
  void writeHtmlEscaped(const char *s)
  {
    if (!s) return;
    for (; *s; ++s)
    {
      switch (*s)
      {
        case '&': write("&amp;", 5); break;
        case '"': write("&quot;", 6); break;
        case '<': write("&lt;", 4); break;
        case '>': write("&gt;", 4); break;
        case '\'': write("&#39;", 5); break;
        default: write(s, 1); break;
      }
    }
  }
  void writeJsonEscaped(const char *s)
  {
    if (!s) return;
    for (; *s; ++s)
    {
      unsigned char c = (unsigned char)*s;
      if (c == '"') write("\\\"", 2);
      else if (c == '\\') write("\\\\", 2);
      else if (c == '\b') write("\\b", 2);
      else if (c == '\f') write("\\f", 2);
      else if (c == '\n') write("\\n", 2);
      else if (c == '\r') write("\\r", 2);
      else if (c == '\t') write("\\t", 2);
      else if (c < 0x20)
      {
        char esc[7];
        snprintf(esc, sizeof(esc), "\\u%04x", c);
        write(esc, 6);
      }
      else
      {
        write(s, 1);
      }
    }
  }
  void finish()
  {
    flush();
    webCfgServer.sendContent("");
  }
};

static bool s_webRestartPending = false;
static uint32_t s_webRestartAt = 0;

static void scheduleWebRestart(uint32_t delayMs)
{
  s_webRestartPending = true;
  s_webRestartAt = millis() + delayMs;
}

static void handleRoot()
{
  if (!checkAuth())
    return;

  const TSettings Settings = currentSettings(); // shadows the global on purpose
  webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webCfgServer.send(200, "text/html", "");
  ChunkedWriter w;
  w.writeF(F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
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
             "<h2>NerdMiner Settings</h2>"));
  if (Settings.WebPassword == DEFAULT_WEBPASSWORD)
    w.writeF(F("<p style='background:#fff3cd;color:#664d03;padding:.6em;border-radius:4px'>"
               "This page still uses the default password, the same on every NerdMiner: "
               "set your own at the bottom of the page.</p>"));
  w.writeF(F("<form method='POST' action='/save'>"));

  w.write("<label>Pool URL</label><input type='text' name='pool' value='");
  w.writeHtmlEscaped(Settings.PoolAddress.c_str());
  w.write("'><label>Pool Port</label><input type='number' name='port' value='");
  w.writePrintf("%d", Settings.PoolPort);
  w.write("'><label>Pool Password (optional)</label><input type='text' name='poolpass' value='");
  w.writeHtmlEscaped(Settings.PoolPassword);
  w.write("'><label>BTC Address</label><input type='text' name='wallet' value='");
  w.writeHtmlEscaped(Settings.BtcWallet);
  w.write("'><label>Hostname</label><input type='text' name='hostname' maxlength='31' value='");
  w.writeHtmlEscaped(Settings.Hostname);
  w.write("'><label>Backup pool URL (optional)</label><input type='text' name='bpool' value='");
  w.writeHtmlEscaped(Settings.BackupPoolAddress.c_str());
  w.write("'><label>Backup pool port</label><input type='number' name='bport' value='");
  w.writePrintf("%d", Settings.BackupPoolPort);
  w.write("'><label>Backup pool password (optional)</label><input type='text' name='bpoolpass' value='");
  w.writeHtmlEscaped(Settings.BackupPoolPassword);
  w.write("'>");
  w.writePrintf("<small>Mined with the same BTC address after %d failed attempts in a row on the pool above; "
                "back to that pool once it answers again (checked every %lu minutes). Now mining on the %s pool.</small>",
                POOL_FAILOVER_TRIES, (unsigned long)(POOL_FAILBACK_ms / 60000),
                onBackupPool() ? "<b>backup</b>" : "main");
  w.writeF(F("<label>Time Zone</label>"
             "<input type='text' id='tz_search' placeholder='Search time zone (e.g. London, New York)...' oninput='filterTZ()' autocomplete='off'>"
             "<select name='tz' id='tz_select' size='6' style='margin-top:4px;'>"));

  bool tzMatched = false;
  for (int i = 0; i < kTimeZoneCount; i++)
  {
    bool sel = !tzMatched && (Settings.Timezone.equalsIgnoreCase(kTimeZones[i].id) || Settings.Timezone.equalsIgnoreCase(kTimeZones[i].posix));
    if (sel) tzMatched = true;
    w.write("<option value='");
    w.write(kTimeZones[i].id);
    w.write(sel ? "' selected>" : "'>");
    w.write(kTimeZones[i].label);
    w.write("</option>");
  }
  if (!tzMatched && Settings.Timezone.length() > 0)
  {
    w.write("<option value='");
    w.writeHtmlEscaped(Settings.Timezone.c_str());
    w.write("' selected>Custom: ");
    w.writeHtmlEscaped(Settings.Timezone.c_str());
    w.write("</option>");
  }
  w.write("</select>");
  w.write("<label>BTC price currency</label><select name='currency'>");
  for (int i = 0; i < kCurrencyCount; i++)
  {
    w.write("<option value='");
    w.write(kCurrencies[i].code);
    w.write("'");
    if (Settings.Currency.equalsIgnoreCase(kCurrencies[i].code))
      w.write(" selected");
    w.write(">");
    w.write(kCurrencies[i].label);
    w.write("</option>");
  }
  w.write("</select>");
  w.writePrintf("<label><input type='checkbox' name='savestats' %s> Save mining statistics to flash</label>",
                Settings.saveStats ? "checked" : "");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  w.writePrintf("<label><input type='checkbox' name='invert' %s> Invert display colors</label>",
                Settings.invertColors ? "checked" : "");
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  w.writePrintf("<label><input type='checkbox' name='flip' %s> Flip display (USB on the left)</label>",
                Settings.flipDisplay ? "checked" : "");
  w.writePrintf("<label><input type='checkbox' name='autobrightness' %s> Auto brightness (LDR light sensor)</label>",
                Settings.autoBrightness ? "checked" : "");
#else
  w.writePrintf("<label><input type='checkbox' name='flip' %s> Flip display (rotate 180 degrees)</label>",
                Settings.flipDisplay ? "checked" : "");
#endif
  w.writePrintf("<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='%d'>",
                Settings.Brightness);
#endif
#ifdef TDISPLAY_PULSE
#ifdef M5STICK_C_PLUS2
  w.writePrintf("<label><input type='checkbox' name='flip' %s> Flip display (rotate 180 degrees)</label>",
                Settings.flipDisplay ? "checked" : "");
#else
  w.writePrintf("<label><input type='checkbox' name='flip' %s> Flip display (USB on the left)</label>",
                Settings.flipDisplay ? "checked" : "");
#endif
  w.writePrintf("<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='%d'>",
                Settings.Brightness);
#endif
#if defined(M5STICK_C_PLUS2) && PLUS2_BACKLIGHT_PWM && !defined(TDISPLAY_PULSE)
  w.writePrintf("<label>Screen brightness (0-255)</label><input type='number' name='brightness' min='0' max='255' value='%d'>",
                Settings.Brightness);
#endif
#ifdef HAS_BACKLIGHT_CONTROL
  w.writePrintf("<label>Screen off after (minutes without a touch or button press, 0 = never)</label><input type='number' name='screenoff' min='0' max='1440' value='%d'>",
                Settings.ScreenTimeout);
  w.writePrintf("<label><input type='checkbox' name='nightdim' %s> Dim the screen at night</label>",
                Settings.nightDim ? "checked" : "");
  w.write("<small>Fades from the brightness above to the night brightness around sunset, and back at sunrise. ");
  w.write(nightDimStatus());
  w.write("</small>");
  w.writePrintf("<label>Night brightness (1-255)</label><input type='number' name='nightbright' min='1' max='255' value='%d'>",
                Settings.NightBrightness);
  w.writePrintf("<label>Fade length (minutes)</label><input type='number' name='nightfade' min='1' max='240' value='%d'>",
                Settings.NightFade);
  w.writeF(F("<small>The fade is centred on sunset and sunrise, half before and half after: 60 minutes starts dimming "
             "30 minutes before sunset and reaches the night brightness 30 minutes after it.</small>"));
  w.writeF(F("<label>Nearest city</label><select name='city'><option value=''>Custom (latitude and longitude below)</option>"));
  for (int i = 0; i < kCityCount; i++)
  {
    w.writePrintf("<option value='%.2f,%.2f'%s>%s</option>",
                  kCities[i].latitude, kCities[i].longitude,
                  (fabsf(Settings.Latitude - kCities[i].latitude) < 0.005f && fabsf(Settings.Longitude - kCities[i].longitude) < 0.005f) ? " selected" : "",
                  kCities[i].name);
  }
  w.writeF(F("</select>"));
  w.writePrintf("<label>Latitude (decimal degrees, south negative)</label><input type='number' name='lat' min='-90' max='90' step='any' value='%.4f'>",
                Settings.Latitude);
  w.writePrintf("<label>Longitude (decimal degrees, west negative)</label><input type='number' name='lon' min='-180' max='180' step='any' value='%.4f'>",
                Settings.Longitude);
#endif
#ifdef NO_DISPLAY
  w.writePrintf("<label><input type='checkbox' name='led' %s> Status LED (blinks while mining)</label>",
                Settings.statusLed ? "checked" : "");
#endif
#ifdef HAS_RGB_LED
  {
    static const char *const kLedModes[] = {"Off", "Status (red starting up, blue connecting, blinks the colour below while mining)", "Always on in the colour below"};
    w.writeF(F("<label>LED on the back</label><select name='ledmode'>"));
    for (int i = LED_MODE_OFF; i <= LED_MODE_STEADY; i++)
    {
      w.writePrintf("<option value='%d'%s>%s</option>", i, Settings.ledMode == i ? " selected" : "", kLedModes[i]);
    }
    w.writeF(F("</select>"));
    w.writePrintf("<label>LED colour</label><input type='color' name='ledcolor' value='#%06lx'>",
                  (unsigned long)(Settings.ledColor & 0xFFFFFF));
    w.writePrintf("<label>LED brightness (1-255)</label><input type='number' name='ledbright' min='1' max='255' value='%d'>",
                  Settings.ledBrightness);
    w.writePrintf("<label><input type='checkbox' name='ledscreen' %s> LED off while the screen is off</label>",
                  Settings.ledScreenOff ? "checked" : "");
  }
#endif
#ifndef NO_MQTT
  w.writeF(F("<h3>MQTT (Home Assistant)</h3>"));
  w.writePrintf("<label><input type='checkbox' name='mqtt' %s> Publish the miner's state to an MQTT broker</label>",
                Settings.MqttEnabled ? "checked" : "");
  w.write("<label>Broker address (IP or name)</label><input type='text' name='mqhost' maxlength='64' value='");
  w.writeHtmlEscaped(Settings.MqttHost.c_str());
  w.writePrintf("'><label>Broker port</label><input type='number' name='mqport' min='1' max='65535' value='%d'>",
                Settings.MqttPort);
  w.write("<label>User (optional)</label><input type='text' name='mquser' maxlength='64' autocomplete='off' value='");
  w.writeHtmlEscaped(Settings.MqttUser.c_str());
  w.writePrintf("'><label>Password (optional)</label><input type='password' name='mqpass' maxlength='64' autocomplete='new-password' placeholder='%s'>",
                Settings.MqttPass.length() ? "saved, leave empty to keep it" : "none");
  if (Settings.MqttPass.length())
    w.writeF(F("<label><input type='checkbox' name='mqpassclear'> Remove the saved password</label>"));
  w.write("<label>Topic (empty: nerdminer/ and the end of the MAC address)</label><input type='text' name='mqtopic' maxlength='64' value='");
  w.writeHtmlEscaped(Settings.MqttTopic.c_str());
  w.writePrintf("'><p><small>%s Home Assistant finds the sensors by itself (MQTT discovery). Plain MQTT only (usually port 1883), no TLS.</small></p>",
                mqttConnected() ? "Connected to the broker." : Settings.MqttEnabled ? "Not connected to the broker." : "Off.");
#endif
  w.writeF(F("<label>New password for this page (leave empty to keep it)</label>"
             "<input type='password' name='webpass' maxlength='63' autocomplete='new-password'>"
             "<label>New password again</label>"
             "<input type='password' name='webpass2' maxlength='63' autocomplete='new-password'>"));
  w.writeF(F("<input type='submit' value='Save'>"));
#if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB) || defined(ES3C35P) || defined(ESP32_4IN_ST7796)
  w.writeF(F("<p><small>Settings apply straight away. Changing invert or flip restarts the device.</small></p>"));
#elif defined(TDISPLAY_PULSE)
  w.writeF(F("<p><small>Settings apply straight away. Changing flip restarts the device.</small></p>"));
#else
  w.writeF(F("<p><small>Settings apply straight away, no restart.</small></p>"));
#endif
  w.writeF(F("</form><p><small><a href='/'>Live monitor</a> (no login needed) &middot; status as JSON: <a href='/api/status'>/api/status</a></small></p>"));
  w.writeF(F("<hr style='margin-top:1.5em;border:0;border-top:1px solid #ddd'><div style='text-align:center;margin:1em 0'><a href='/update' style='color:#f59e0b;font-weight:bold;text-decoration:none'>⚡ Firmware Updates (OTA) &rarr;</a></div>"));
#ifdef HAS_BACKLIGHT_CONTROL
  w.writeF(F("<script>var f=document.forms[0],c=f.city,a=f.lat,o=f.lon;"
             "c.onchange=function(){if(c.value){var p=c.value.split(',');a.value=p[0];o.value=p[1]}};"
             "a.oninput=o.oninput=function(){c.value=''};</script>"));
#endif
  w.writeF(F("</body></html>"));
  w.finish();
}

bool applyLiveSettings(const TSettings &before, const TSettings &next, bool &poolChangedOut);

static bool applySettingsFromRequest(bool isHtmlForm, const String &webPass,
                                       bool &poolChangedOut, bool &restartOut, bool &passChangedOut)
{
  const TSettings before = currentSettings();
  TSettings next = before;

  DynamicJsonDocument doc(1536);
  DeserializationError err = DeserializationError::EmptyInput;
  if (!isHtmlForm && webCfgServer.hasArg("plain"))
  {
    String body = webCfgServer.arg("plain");
    err = deserializeJson(doc, body.c_str());
  }

  if (err)
  {
    const bool cityHasCoords = isHtmlForm && webCfgServer.arg("city").indexOf(',') > 0;
    for (int i = 0; i < webCfgServer.args(); i++)
    {
      const String k = webCfgServer.argName(i);
      if (k == "plain")
        continue;
      if (isHtmlForm)
      {
        if (k == "webpass" || k == "webpass2" || k == "mqpass" || k == "mqpassclear")
          continue;
        if (cityHasCoords && (k == "lat" || k == "lon"))
          continue;
      }
      String v = webCfgServer.arg(i);
      if (isHtmlForm)
      {
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
      }
      doc[k.c_str()] = v.c_str();
    }
  }

  nvMemory::parseJsonConfig(doc, &next);

  if (isHtmlForm)
  {
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
  }

  passChangedOut = next.WebPassword != before.WebPassword;
  restartOut = applyLiveSettings(before, next, poolChangedOut);
  return true;
}

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

  bool poolChanged = false, restart = false, passChanged = false;
  applySettingsFromRequest(true, webPass, poolChanged, restart, passChanged);

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
  scheduleWebRestart(1000);
}

bool applyLiveSettings(const TSettings &before, const TSettings &next, bool &poolChangedOut)
{
  saveSettingsToFlash(const_cast<TSettings*>(&next));

  const bool wifiChanged = (next.WifiSSID != before.WifiSSID || next.WifiPW != before.WifiPW) && next.WifiSSID.length() > 0;
  if (wifiChanged)
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    Settings.WifiSSID = next.WifiSSID;
    Settings.WifiPW = next.WifiPW;
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

void sendCorsJson(int code, const String &json)
{
  ensureCorsHeaders();
  webCfgServer.send(code, "application/json", json);
  s_corsAdded = false;
}

static void beginChunkedCorsJson(int code = 200)
{
  ensureCorsHeaders();
  webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webCfgServer.send(code, "application/json", "");
  s_corsAdded = false;
}

bool handleCorsOptions(void)
{
  if (webCfgServer.method() != HTTP_OPTIONS)
    return false;
  ensureCorsHeaders();
  webCfgServer.send(204);
  s_corsAdded = false;
  return true;
}

template <typename MutateFn>
static bool dispatchRestEndpoint(MutateFn onMutate)
{
  if (handleCorsOptions())
    return false;
  HTTPMethod m = webCfgServer.method();
  if (m == HTTP_POST || m == HTTP_PATCH || m == HTTP_PUT)
  {
    if (!checkWebApiMutationAllowed())
      return false;
    return onMutate();
  }
  if (m != HTTP_GET)
  {
    sendCorsJson(405, "{\"error\":\"Method Not Allowed\"}");
    return false;
  }
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

  beginChunkedCorsJson(200);
  ChunkedWriter w;
  serializeJson(doc, w);
  w.finish();
}

static void handleTimezoneEndpoint()
{
  bool ok = dispatchRestEndpoint([]() {
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
    return true;
  });
  if (!ok) return;

  const TSettings s = currentSettings();
  String label, posix, localTime;
  bool dstActive = false;
  getTimezoneFields(s.Timezone, label, posix, localTime, dstActive);

  beginChunkedCorsJson(200);
  ChunkedWriter w;
  w.write("{\"timezone\":\"");
  w.writeJsonEscaped(s.Timezone.c_str());
  w.write("\",\"label\":\"");
  w.writeJsonEscaped(label.c_str());
  w.write("\",\"posix\":\"");
  w.writeJsonEscaped(posix.c_str());
  w.write("\",\"localTime\":\"");
  w.writeJsonEscaped(localTime.c_str());
  w.write("\",\"localDate\":\"");
  w.writeJsonEscaped(getDate().c_str());
  w.writePrintf("\",\"dstActive\":%s}", dstActive ? "true" : "false");
  w.finish();
}

static void handleTimezonesList()
{
  if (handleCorsOptions()) return;
  ensureCorsHeaders();
  webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webCfgServer.send(200, "application/json", "");
  s_corsAdded = false;

  ChunkedWriter w;
  w.write("[", 1);
  for (int i = 0; i < kTimeZoneCount; i++) {
    if (i > 0) w.write(",", 1);
    w.write("{\"id\":\"");
    w.writeJsonEscaped(kTimeZones[i].id);
    w.write("\",\"label\":\"");
    w.writeJsonEscaped(kTimeZones[i].label);
    w.write("\",\"posix\":\"");
    w.writeJsonEscaped(kTimeZones[i].posix);
    w.write("\"}");
  }
  w.write("]", 1);
  w.finish();
}

static void handleCitiesList()
{
  if (handleCorsOptions()) return;
  ensureCorsHeaders();
  webCfgServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webCfgServer.send(200, "application/json", "");
  s_corsAdded = false;

  ChunkedWriter w;
  w.write("[", 1);
  for (int i = 0; i < kCityCount; i++) {
    if (i > 0) w.write(",", 1);
    w.write("{\"name\":\"");
    w.writeJsonEscaped(kCities[i].name);
    w.writePrintf("\",\"latitude\":%.2f,\"longitude\":%.2f}", kCities[i].latitude, kCities[i].longitude);
  }
  w.write("]", 1);
  w.finish();
}

static bool applyRequestConfig(bool &poolChangedOut, bool &restartOut)
{
  bool passChanged = false;
  return applySettingsFromRequest(false, "", poolChangedOut, restartOut, passChanged);
}

static void handleConfigRoute()
{
  bool ok = dispatchRestEndpoint([]() {
    bool poolChanged = false, restart = false;
    applyRequestConfig(poolChanged, restart);
    if (restart) {
      sendCorsJson(200, "{\"success\":true,\"message\":\"Settings saved. Restarting device for changes...\",\"rebootRequired\":true}");
      saveStat();
      scheduleWebRestart(1000);
    } else {
      String resp = "{\"success\":true,\"message\":\"Settings saved and applied live.\",\"rebootRequired\":false";
      if (poolChanged) resp += ",\"poolChanged\":true";
      resp += "}";
      sendCorsJson(200, resp);
    }
    return false;
  });
  if (ok) {
    handleGetConfig();
  }
}

static void handleLedEndpoint()
{
  bool ok = dispatchRestEndpoint([]() {
    bool poolChanged = false, restart = false;
    applyRequestConfig(poolChanged, restart);
    return true;
  });
  if (!ok) return;

  const TSettings s = currentSettings();
  char buf[256];
#ifdef HAS_RGB_LED
  snprintf(buf, sizeof(buf),
           "{\"ledMode\":%d,\"ledModeName\":\"%s\",\"ledColor\":%lu,"
           "\"ledColorHex\":\"%s\",\"ledBrightness\":%d,\"ledScreenOff\":%s}",
           s.ledMode, ledModeName((uint8_t)s.ledMode), (unsigned long)s.ledColor,
           ledColorHex(s.ledColor).c_str(), s.ledBrightness,
           s.ledScreenOff ? "true" : "false");
#elif defined(NO_DISPLAY)
  snprintf(buf, sizeof(buf), "{\"statusLed\":%s}", s.statusLed ? "true" : "false");
#else
  snprintf(buf, sizeof(buf), "{\"supported\":false}");
#endif
  sendCorsJson(200, buf);
}

static void handleDisplayEndpoint()
{
  bool ok = dispatchRestEndpoint([]() {
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
        scheduleWebRestart(1000);
        return false;
      }
    }
    return true;
  });
  if (!ok) return;

  const TSettings s = currentSettings();
  beginChunkedCorsJson(200);
  ChunkedWriter w;
  w.writePrintf("{\"brightness\":%d,\"autoBrightness\":%s,\"screenTimeout\":%d,"
                "\"nightDim\":%s,\"nightBrightness\":%d,\"nightFade\":%d,\"city\":\"",
                s.Brightness, s.autoBrightness ? "true" : "false", s.ScreenTimeout,
                s.nightDim ? "true" : "false", s.NightBrightness, s.NightFade);
  w.writeJsonEscaped(findCityName(s.Latitude, s.Longitude).c_str());
  w.writePrintf("\",\"latitude\":%.4f,\"longitude\":%.4f", s.Latitude, s.Longitude);
#ifdef HAS_BACKLIGHT_CONTROL
  w.write(",\"nightDimStatus\":\"");
  w.writeJsonEscaped(nightDimStatus().c_str());
  w.write("\"", 1);
#endif
  w.writePrintf(",\"screenLit\":%s,\"invertColors\":%s,\"flipDisplay\":%s}",
                screenLit() ? "true" : "false",
                s.invertColors ? "true" : "false",
                s.flipDisplay ? "true" : "false");
  w.finish();
}

static void handlePoolEndpoint()
{
  bool ok = dispatchRestEndpoint([]() {
    bool poolChanged = false, restart = false;
    applyRequestConfig(poolChanged, restart);
    return true;
  });
  if (!ok) return;

  const TSettings s = currentSettings();
  beginChunkedCorsJson(200);
  ChunkedWriter w;
  w.write("{\"pool\":\"");
  w.writeJsonEscaped(s.PoolAddress.c_str());
  w.writePrintf("\",\"port\":%d,\"wallet\":\"", s.PoolPort);
  w.writeJsonEscaped(s.BtcWallet);
  w.writePrintf("\",\"poolPassword\":\"%s\",\"backupPool\":\"",
                s.PoolPassword[0] != '\0' ? "********" : "");
  w.writeJsonEscaped(s.BackupPoolAddress.c_str());
  w.writePrintf("\",\"backupPort\":%d,\"backupPoolPassword\":\"%s\",\"activePool\":\"",
                s.BackupPoolPort, s.BackupPoolPassword[0] != '\0' ? "********" : "");
  w.writeJsonEscaped(activePoolAddress().c_str());
  w.writePrintf("\",\"activePort\":%d,\"onBackupPool\":%s}",
                activePoolPort(), onBackupPool() ? "true" : "false");
  w.finish();
}

static void handleMqttEndpoint()
{
  bool ok = dispatchRestEndpoint([]() {
    bool poolChanged = false, restart = false;
    applyRequestConfig(poolChanged, restart);
    return true;
  });
  if (!ok) return;

  const TSettings s = currentSettings();
  beginChunkedCorsJson(200);
  ChunkedWriter w;
  w.writePrintf("{\"mqttEnabled\":%s,\"mqttHost\":\"", s.MqttEnabled ? "true" : "false");
  w.writeJsonEscaped(s.MqttHost.c_str());
  w.writePrintf("\",\"mqttPort\":%d,\"mqttUser\":\"", s.MqttPort);
  w.writeJsonEscaped(s.MqttUser.c_str());
  w.write("\",\"mqttTopic\":\"");
  w.writeJsonEscaped(s.MqttTopic.c_str());
  w.writePrintf("\",\"mqttConnected\":%s}", mqttConnected() ? "true" : "false");
  w.finish();
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

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"success\":true,\"identifying\":%s,\"durationMs\":%lu,\"hostname\":\"%s\"}",
           isIdentifying() ? "true" : "false",
           (unsigned long)durationMs,
           jsonEscape(host).c_str());
  sendCorsJson(200, buf);
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
  const MinerStatus st = getMinerStatus();
  String host;
  {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    host = String(Settings.Hostname);
  }

  char buf[384];
  snprintf(buf, sizeof(buf),
           "{\"hr\":%lu,\"ut\":%lu,\"ebd\":\"%s\",\"sbd\":\"%s\",\"temp\":%.1f,"
           "\"acc\":%u,\"rej\":%u,\"rc\":%u,\"nerdminer\":true,\"host\":\"%s\",\"ver\":\"%s\"}",
           (unsigned long)hr_hs, (unsigned long)getUpTime(),
           best_diff_str, sess_diff_str, temperatureRead(),
           st.accepted, st.rejected, getStratumReconnects(),
           jsonEscape(host).c_str(), CURRENT_VERSION);
  sendCorsJson(200, buf);
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
  const uint32_t discMs = getPoolDisconnectedSinceMs();

  beginChunkedCorsJson(200);
  ChunkedWriter w;
  w.write("{\"nerdminer\":true,\"hostname\":\"");
  w.writeJsonEscaped(s.Hostname);
  w.writePrintf("\",\"version\":\"%s\",\"deviceModel\":\"NerdMiner v2\","
                "\"khash\":%.2f,\"hashRate\":%.2f,\"validShares\":%u,\"invalidShares\":%u,"
                "\"sharesAccepted\":%u,\"sharesRejected\":%u,\"validBlocks\":%u,\"blockFound\":%s,"
                "\"bestDiff\":\"%s\",\"bestSessionDiff\":\"%s\",\"uptime\":%lu,\"poolDiff\":%.10g",
                CURRENT_VERSION, khs, khs, shares, rejects,
                m.accepted, m.rejected, valids, valids > 0 ? "true" : "false",
                best_diff_str, sess_diff_str, (unsigned long)getUpTime(), m.poolDifficulty);
  if (m.pingMs >= 0)
    w.writePrintf(",\"pingMs\":%ld", (long)m.pingMs);
  w.write(",\"stratumURL\":\"");
  w.writeJsonEscaped(activePoolAddress().c_str());
  w.writePrintf("\",\"stratumPort\":%d,\"stratumUser\":\"", activePoolPort());
  w.writeJsonEscaped(s.BtcWallet);
  w.writePrintf("\",\"onBackupPool\":%s,\"temp\":%.1f,\"reset_reason\":\"",
                onBackupPool() ? "true" : "false", temp);
  w.writeJsonEscaped(getResetReasonStr());
  w.writePrintf("\",\"network_difficulty\":%.2f,\"pool_disconnected_s\":%lu,"
                "\"stratum_reconnects\":%u,\"thermal_throttled\":%s,\"frequency\":%u,"
                "\"macAddr\":\"%s\",\"freeHeap\":%u,"
                "\"gitRepo\":\"aetherwraith/NerdMiner-Modded\","
                "\"changelogUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\","
                "\"releaseNotesUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\","
                "\"firmwareStatus\":{\"familyKey\":\"nerdminer\",\"currentVersion\":\"%s\","
                "\"latestVersion\":\"%s\",\"hasUpdate\":false,"
                "\"changelogUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\","
                "\"releaseNotesUrl\":\"https://github.com/aetherwraith/NerdMiner-Modded/releases\","
                "\"releaseName\":\"NerdMiner Modded %s\",\"isNativeHardwareStatus\":true}}",
                getNetworkDifficulty(),
                discMs ? (unsigned long)((millis() - discMs) / 1000UL) : 0UL,
                getStratumReconnects(), getThermalThrottled() ? "true" : "false",
                getCpuFrequencyMhz(), WiFi.macAddress().c_str(), ESP.getFreeHeap(),
                CURRENT_VERSION, CURRENT_VERSION, CURRENT_VERSION);
  w.finish();
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
  scheduleWebRestart(500);
}

// Miner state as JSON for Home Assistant, scripts and dashboards. No login:
// it is read-only and holds nothing the screen doesn't show, and no wallet
// address or passwords.
extern uint32_t templates;
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
  doc["mining_time_s"] = (uint32_t)getUpTime();
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
  doc["stratum_reconnects"] = getStratumReconnects();
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
  doc["temp"] = round(temperatureRead() * 10.0) / 10.0;
  doc["reset_reason"] = getResetReasonStr();
  doc["network_difficulty"] = getNetworkDifficulty();
  const uint32_t discMs = getPoolDisconnectedSinceMs();
  doc["pool_disconnected_s"] = discMs ? (uint32_t)((millis() - discMs) / 1000UL) : 0U;
  doc["thermal_throttled"] = getThermalThrottled();

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
  webCfgServer.sendHeader("Cache-Control", "no-store");
  beginChunkedCorsJson(200);
  ChunkedWriter w;
  serializeJson(doc, w);
  w.finish();
}

// --------------------------------------------------------------------------
// Live monitor (webMonitorPage.h) and its hashrate history: one sample every
// 30 s, the last hour, kept here in RAM (240 bytes).
// --------------------------------------------------------------------------
#define HISTORY_LEN 120
#define HISTORY_PERIOD_ms 30000
static uint16_t s_history[HISTORY_LEN];  // KH/s x 10
static int16_t s_histPing[HISTORY_LEN];  // pool ping ms (-1 if unavailable)
static uint16_t s_histRej[HISTORY_LEN];  // rejected shares delta in interval
static uint32_t s_histLastRej = 0;
static uint8_t s_historyCount = 0, s_historyHead = 0;
static uint32_t s_historyLast = 0;

static void historyTick()
{
  if (millis() - s_historyLast < HISTORY_PERIOD_ms)
    return;
  s_historyLast = millis();
  const MinerStatus st = getMinerStatus();
  uint32_t v = (uint32_t)(st.hashrateKHs * 10.0f + 0.5f);
  if (v > 65535) v = 65535;
  s_history[s_historyHead] = (uint16_t)v;
  s_histPing[s_historyHead] = (st.pingMs >= -1 && st.pingMs <= 32767) ? (int16_t)st.pingMs : (int16_t)32767;
  uint32_t rDelta = st.rejected >= s_histLastRej ? (st.rejected - s_histLastRej) : st.rejected;
  s_histLastRej = st.rejected;
  s_histRej[s_historyHead] = rDelta > 65535 ? 65535 : (uint16_t)rDelta;
  s_historyHead = (s_historyHead + 1) % HISTORY_LEN;
  if (s_historyCount < HISTORY_LEN) s_historyCount++;
}

static void handleHistory()
{
  if (handleCorsOptions()) return;
  webCfgServer.sendHeader("Cache-Control", "no-store");
  beginChunkedCorsJson();
  ChunkedWriter w;
  w.writePrintf("{\"interval_s\":%d,\"khs\":[", HISTORY_PERIOD_ms / 1000);
  for (uint8_t i = 0; i < s_historyCount; i++) {
    const uint16_t v = s_history[(s_historyHead + HISTORY_LEN - s_historyCount + i) % HISTORY_LEN];
    if (i) w.write(",", 1);
    w.writePrintf("%u.%u", v / 10U, v % 10U);
  }
  w.write("],\"ping_ms\":[");
  for (uint8_t i = 0; i < s_historyCount; i++) {
    const int16_t p = s_histPing[(s_historyHead + HISTORY_LEN - s_historyCount + i) % HISTORY_LEN];
    if (i) w.write(",", 1);
    w.writePrintf("%d", (int)p);
  }
  w.write("],\"rej\":[");
  for (uint8_t i = 0; i < s_historyCount; i++) {
    const uint16_t r = s_histRej[(s_historyHead + HISTORY_LEN - s_historyCount + i) % HISTORY_LEN];
    if (i) w.write(",", 1);
    w.writePrintf("%u", (unsigned)r);
  }
  w.write("]}");
  w.finish();
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
  if (s_webRestartPending && (int32_t)(millis() - s_webRestartAt) >= 0) {
    s_webRestartPending = false;
    ESP.restart();
  }
}
