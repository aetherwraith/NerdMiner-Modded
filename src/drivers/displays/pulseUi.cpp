#include "pulseUi.h"

#if ((defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)) && !defined(CYD_THEME_CLASSIC)) || \
    defined(ES3C35P_DISPLAY) || defined(TDISPLAY_PULSE) || defined(ESP32_4IN_ST7796_DISPLAY)

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include "OpenFontRender.h"
#include "currency.h"
#include "monitor.h"
#include "../storage/storage.h"
#include "../storage/nvMemory.h"

extern TSettings Settings;

const PulseAccent PULSE_ACCENTS[] = {
    {RGB565(34, 225, 255), RGB565(140, 92, 255)}, // ice / violet
    {RGB565(61, 255, 176), RGB565(38, 148, 255)}, // mint / azure
    {RGB565(255, 176, 40), RGB565(255, 72, 112)}, // amber / rose
    {RGB565(255, 84, 190), RGB565(122, 92, 255)}, // magenta / indigo
    {RGB565(190, 255, 60), RGB565(30, 208, 160)}, // lime / teal
};
const size_t PULSE_ACCENT_COUNT = sizeof(PULSE_ACCENTS) / sizeof(PULSE_ACCENTS[0]);

uint16_t pulseMix565(uint16_t a, uint16_t b, uint8_t t)
{
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + ((br - ar) * t) / 255;
  int g = ag + ((bg - ag) * t) / 255;
  int bl = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

uint32_t pulseFnv1aStr(const char *s, uint32_t seed)
{
  uint32_t h = seed;
  if (s)
  {
    while (*s)
    {
      h ^= (uint8_t)*s++;
      h *= 16777619u;
    }
  }
  // Hash separator so adjacent fields ("ab","c" vs "a","bc") never collide
  h ^= 0x1Fu;
  h *= 16777619u;
  return h;
}

uint32_t pulseFnv1aU32(uint32_t v, uint32_t seed)
{
  uint32_t h = seed;
  for (int i = 0; i < 4; i++)
  {
    h ^= (v & 0xFFu);
    h *= 16777619u;
    v >>= 8;
  }
  h ^= 0x1Eu;
  h *= 16777619u;
  return h;
}

String pulseCommas(const String &s)
{
  int n = s.length();
  for (int i = 0; i < n; i++)
    if (!isDigit(s[i]))
      return s;
  String o;
  for (int i = 0; i < n; i++)
  {
    o += s[i];
    int rem = n - 1 - i;
    if (rem > 0 && rem % 3 == 0)
      o += ',';
  }
  return o;
}

String pulseFmtPrice(const String &s, const String &currency)
{
  int i = 0;
  while (i < (int)s.length() && !isdigit((unsigned char)s[i]))
    i++;
  String digits = s.substring(i);
  if (digits.toInt() <= 0)
    return "--";
  return String(currencyFor(currency).symbol) + pulseCommas(digits);
}

String pulseBtcPairLabel(const String &currency)
{
  String c = currencyFor(currency).code;
  c.toUpperCase();
  return "BTC / " + c;
}

String pulseFmtUptime(const String &s)
{
  int d = 0, h = 0, m = 0, sec = 0;
  sscanf(s.c_str(), "%d %d:%d:%d", &d, &h, &m, &sec);
  char b[16];
  if (d > 0)
    snprintf(b, sizeof(b), "%dd %02dh", d, h);
  else
    snprintf(b, sizeof(b), "%02d:%02d:%02d", h, m, sec);
  return String(b);
}

String pulseFmtDate(const String &s)
{
  static const char *mon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  static const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int d = 0, m = 0, y = 0;
  if (sscanf(s.c_str(), "%d/%d/%d", &d, &m, &y) != 3 || m < 1 || m > 12)
    return "";
  int yy = y - (m < 3 ? 1 : 0);
  int dow = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
  char b[32];
  snprintf(b, sizeof(b), "%s  %d %s %d", wd[dow], d, mon[m - 1], y);
  return String(b);
}

int pulseWifiBars()
{
  if (WiFi.status() != WL_CONNECTED)
    return 0;
  int r = WiFi.RSSI();
  return r > -55 ? 4 : r > -65 ? 3
                   : r > -75   ? 2
                   : r > -85   ? 1
                               : 0;
}

int pulseUiStatus(int nerdStatus, float lastKh)
{
  if (nerdStatus == NM_waitingConfig)
    return NM_waitingConfig;
  return (WiFi.status() == WL_CONNECTED && lastKh > 0) ? NM_hashing : NM_Connecting;
}

void pulsePanel(TFT_eSPI &d, int x, int y, int w, int h, int r, uint16_t fill, uint16_t edge, uint16_t behind)
{
  d.fillSmoothRoundRect(x, y, w, h, r, edge, behind);
  d.fillSmoothRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, fill, edge);
}

void pulseGradBar(TFT_eSPI &d, int x, int y, int w, int h, uint16_t cAlt, uint16_t cAcc)
{
  for (int i = 0; i < w; i++)
  {
    int inset = 0;
    int e = i < w - 1 - i ? i : w - 1 - i;
    if (h >= 6)
      inset = e < 1 ? 2 : (e < 3 ? 1 : 0);
    uint16_t c = pulseMix565(cAlt, cAcc, (uint8_t)(w > 1 ? i * 255 / (w - 1) : 255));
    d.drawFastVLine(x + i, y + inset, h - 2 * inset, c);
  }
}

void pulseDrawStatusBadge(TFT_eSPI &d, OpenFontRender &render, const char *label, int cx, int y,
                          int cw, int h, int r, unsigned fontSize, uint16_t col, uint16_t bg)
{
  uint16_t pillBg = pulseMix565(bg, col, 46);
  d.fillSmoothRoundRect(cx, y, cw, h, r, pillBg, bg);
  render.setDrawer(d);
  render.setFontSize(fontSize);
  render.setAlignment(Align::TopCenter);
  render.drawString(label, cx + cw / 2, y + (h - (int)fontSize) / 2 - 1, col, pillBg);
}

void pulseLoadUiPrefs(const char *ns, int &accentIdx, uint16_t &cAcc, uint16_t &cAlt,
                      int &savedAccent, int &savedPage, int &restorePage)
{
  Preferences p;
  if (!p.begin(ns, true))
    return;
  accentIdx = p.getUChar("accent", 0) % PULSE_ACCENT_COUNT;
  int page = p.getUChar("page", 0);
  p.end();
  cAcc = PULSE_ACCENTS[accentIdx].main;
  cAlt = PULSE_ACCENTS[accentIdx].alt;
  savedAccent = accentIdx;
  savedPage = restorePage = page;
}

void pulseSaveUiPrefs(const char *ns, int accentIdx, int page, int &savedAccent, int &savedPage)
{
  if (page == savedPage && accentIdx == savedAccent)
    return;
  Preferences p;
  if (p.begin(ns, false))
  {
    p.putUChar("accent", accentIdx);
    p.putUChar("page", page);
    p.end();
  }
  savedAccent = accentIdx;
  savedPage = page;
}

void pulsePersistFlip(nvMemory &nvMem, bool flipped)
{
  Settings.flipDisplay = flipped;
  TSettings stored;
  if (nvMem.loadConfig(&stored))
  {
    stored.flipDisplay = flipped;
    nvMem.saveConfig(&stored);
  }
}

void pulseNoteHashrate(float kh, float &lastKh, float *hist, int histN, uint8_t &histCount,
                       unsigned long &histLastMs, unsigned long histEveryMs, uint32_t &histEpoch)
{
  lastKh = kh;
  unsigned long now = millis();
  if (histCount != 0 && now - histLastMs < histEveryMs)
    return;
  histLastMs = now;
  if (histCount < histN)
    hist[histCount++] = kh;
  else
  {
    memmove(hist, hist + 1, (histN - 1) * sizeof(float));
    hist[histN - 1] = kh;
  }
  histEpoch++;
}

#endif
