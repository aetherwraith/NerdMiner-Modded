#ifndef PULSE_UI_H
#define PULSE_UI_H

#include <Arduino.h>
#include <stdint.h>
#include "displayDriver.h"
#include <string.h>

#ifndef RGB565
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#endif

// Shared Pulse dark glassy palette
static constexpr uint16_t C_BG = RGB565(5, 7, 13);      // near-black ink
static constexpr uint16_t C_PANEL = RGB565(13, 18, 30); // raised surface
static constexpr uint16_t C_EDGE = RGB565(30, 41, 62);  // hairlines
static constexpr uint16_t C_TRACK = RGB565(22, 30, 46); // unlit gauge / bar tracks
static constexpr uint16_t C_TEXT = RGB565(232, 238, 248);
static constexpr uint16_t C_DIM = RGB565(112, 128, 156);
static constexpr uint16_t C_GOOD = RGB565(61, 255, 176);
static constexpr uint16_t C_WARN = RGB565(255, 176, 40);
static constexpr uint16_t C_BAD = RGB565(255, 92, 112);
static constexpr uint16_t C_GOLD = RGB565(255, 206, 64);

struct PulseAccent
{
  uint16_t main; // hot end of gradients, wordmark, dots
  uint16_t alt;  // cool end of gradients
};

extern const PulseAccent PULSE_ACCENTS[];
extern const size_t PULSE_ACCENT_COUNT;

// Blends RGB565 color a -> b by t (0..255)
uint16_t pulseMix565(uint16_t a, uint16_t b, uint8_t t);

// FNV-1a 32-bit hashing for zero-heap widget cache keys (0 is reserved for unset)
uint32_t pulseFnv1aStr(const char *s, uint32_t seed = 2166136261u);
uint32_t pulseFnv1aU32(uint32_t v, uint32_t seed = 2166136261u);
inline uint32_t pulseFnv1aInt(int32_t v, uint32_t seed = 2166136261u)
{
  return pulseFnv1aU32((uint32_t)v, seed);
}

struct PulseKey
{
  uint32_t h;
  explicit PulseKey(uint32_t seed = 2166136261u) : h(seed) {}
  PulseKey &add(const char *s)
  {
    h = pulseFnv1aStr(s, h);
    return *this;
  }
  PulseKey &add(const String &s)
  {
    h = pulseFnv1aStr(s.c_str(), h);
    return *this;
  }
  PulseKey &add(int v)
  {
    h = pulseFnv1aInt((int32_t)v, h);
    return *this;
  }
  PulseKey &add(uint32_t v)
  {
    h = pulseFnv1aU32(v, h);
    return *this;
  }
  PulseKey &add(uint16_t v)
  {
    h = pulseFnv1aU32((uint32_t)v, h);
    return *this;
  }
  PulseKey &add(bool v)
  {
    h = pulseFnv1aU32(v ? 1u : 0u, h);
    return *this;
  }
  uint32_t value() const { return h ? h : 1u; }
};

inline bool pulseNeed(const uint32_t *wkey, int id, uint32_t k) { return wkey[id] != k; }
inline void pulseDone(uint32_t *wkey, int id, uint32_t k) { wkey[id] = k; }
inline void pulseResetWidgets(uint32_t *wkey, size_t count) { memset(wkey, 0, count * sizeof(uint32_t)); }

// Formatting helpers
String pulseCommas(const String &s);
String pulseFmtPrice(const String &s, const String &currency);
String pulseBtcPairLabel(const String &currency);
String pulseFmtUptime(const String &s);
String pulseFmtDate(const String &s);

// Status / WiFi / wrap-safe share flash helpers
int pulseWifiBars();
int pulseUiStatus(int nerdStatus, float lastKh);
inline bool pulseIsShareFlashing(unsigned long shareFlashUntil)
{
  return shareFlashUntil != 0 && (int32_t)(millis() - shareFlashUntil) < 0;
}

#if ((defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)) && !defined(CYD_THEME_CLASSIC)) || \
    defined(ES3C35P_DISPLAY) || defined(TDISPLAY_PULSE) || defined(ESP32_4IN_ST7796_DISPLAY)
class TFT_eSPI;
class OpenFontRender;
class nvMemory;

void pulsePanel(TFT_eSPI &d, int x, int y, int w, int h, int r, uint16_t fill, uint16_t edge, uint16_t behind);
void pulseGradBar(TFT_eSPI &d, int x, int y, int w, int h, uint16_t cAlt, uint16_t cAcc);
void pulseDrawStatusBadge(TFT_eSPI &d, OpenFontRender &render, const char *label, int cx, int y,
                          int cw, int h, int r, unsigned fontSize, uint16_t col, uint16_t bg);
void pulseLoadUiPrefs(const char *ns, int &accentIdx, uint16_t &cAcc, uint16_t &cAlt,
                      int &savedAccent, int &savedPage, int &restorePage);
void pulseSaveUiPrefs(const char *ns, int accentIdx, int page, int &savedAccent, int &savedPage);
void pulsePersistFlip(nvMemory &nvMem, bool flipped);
void pulseNoteHashrate(float kh, float &lastKh, float *hist, int histN, uint8_t &histCount,
                       unsigned long &histLastMs, unsigned long histEveryMs, uint32_t &histEpoch);
#endif

#endif // PULSE_UI_H
