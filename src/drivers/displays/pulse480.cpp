// Shared 480x320 "PULSE" UI module for ES3C35P and ESP32_4IN_ST7796.
//
//   MINE    segmented hashrate ring, stat tiles, pool strip
//   MARKET  BTC price, block height, fees, difficulty, halving progress
//   CLOCK   big desk clock, key numbers, hashrate history bars
#include "pulse480.h"

#if defined(ES3C35P_DISPLAY) || defined(ESP32_4IN_ST7796_DISPLAY)

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include "media/myFonts.h"
#include "version.h"
#include "monitor.h"
#include "OpenFontRender.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "currency.h"
#include "pulseUi.h"

extern nvMemory nvMem;
extern monitor_data mMonitor;
extern pool_data pData;
extern DisplayDriver *currentDisplayDriver;
extern TSettings Settings;
extern unsigned long mPoolUpdate;
extern String readCustomAPName(); // wManager.cpp

#define SCR_W 480
#define SCR_H 320

static Pulse480Backend s_backend = {};
static OpenFontRender render;
static TFT_eSprite *s_wgt = nullptr; // scratch sprite reused by every widget

// ---------------------------------------------------------------- palette --

#define ACCENT_COUNT PULSE_ACCENT_COUNT

static int accentIdx = 0;
static uint16_t cAcc = PULSE_ACCENTS[0].main;
static uint16_t cAlt = PULSE_ACCENTS[0].alt;

#define mix565 pulseMix565

// --------------------------------------------------------------- geometry --

#define HDR_H 34 // header band; hairline sits on the row below

#define SCR_MINE 0
#define SCR_MARKET 1
#define SCR_CLOCK 2

// Hashrate ring
#define G_CX 140
#define G_CY 148
#define G_R 104
#define G_RI 90
#define G_SEGS 30

// Bottom strip (pool cells / hashrate history)
#define STRIP_Y 264
#define STRIP_H 48

// Finger-sized corner zones at the top of the screen (the header itself is
// only HDR_H tall, too thin a target on a touch panel).
#define TOUCH_CORNER_W 160
#define TOUCH_CORNER_H 90

// --------------------------------------------------------------- UI state --

static int uiScreen = -1;    // screen whose chrome is currently on the panel
static bool uiLive = false;  // false while the boot / setup screens own the panel
static float gaugeTarget = 0;
static float gaugeShown = 0;
static uint8_t segLvl[G_SEGS];
static unsigned long shareFlashUntil = 0;
static uint32_t lastShares = 0;
static bool s_showingIdentify = false;
static int s_lastIdentifySec = -1;
static uint32_t s_identifyStaticKey = 0;
static bool s_wasCelebrating = false;
static bool s_lastRingFlash = false;

enum StaticPage
{
  SP_NONE = 0,
  SP_LOADING,
  SP_SETUP
};
static int staticPage = SP_NONE;

static volatile int pendingTouch = PULSE480_TA_NONE;
static volatile bool pendingFlip = false;

static bool isShareFlashing()
{
  return pulseIsShareFlashing(shareFlashUntil);
}

// history of hashrate for the CLOCK screen
#define HIST_N 48
#define HIST_EVERY_MS 10000
static float hist[HIST_N];
static uint8_t histCount = 0;
static uint32_t histEpoch = 0;
static unsigned long histLastMs = 0;

// Every widget remembers the "key" it last drew so it only repaints on change.
enum WidgetId
{
  W_HDR,
  W_HERO,
  W_TOTAL,
  W_T0, // 4 tiles
  W_T1,
  W_T2,
  W_T3,
  W_P0, // 4 strip cells
  W_P1,
  W_P2,
  W_P3,
  W_PRICE,
  W_BLOCK,
  W_M0, // 3 market tiles
  W_M1,
  W_M2,
  W_HALV,
  W_HH,
  W_COLON,
  W_MM,
  W_DATE,
  W_C0, // 3 clock tiles
  W_C1,
  W_C2,
  W_CHART,
  W_COUNT
};
static uint32_t wkey[W_COUNT];

static bool need(int id, uint32_t k) { return pulseNeed(wkey, id, k); }
static void done(int id, uint32_t k) { pulseDone(wkey, id, k); }
static void resetWidgets() { pulseResetWidgets(wkey, W_COUNT); }

// ---------------------------------------------------------- output target --
// Widgets are drawn into a scratch sprite and either pushed to fb + flushed
// (ES3C35P) or pushed directly to the panel (ESP32_4IN_ST7796).

static bool wBegin(int w, int h, uint16_t bg)
{
  if (!s_wgt)
    return false;
  s_wgt->setColorDepth(16);
  s_wgt->createSprite(w, h);
  if (!s_wgt->created() && s_backend.sprite8BitFallback)
  {
    s_wgt->setColorDepth(8);
    s_wgt->createSprite(w, h);
  }
  if (!s_wgt->created())
  {
    Serial.printf("[ui] sprite %dx%d alloc failed (free %u, max block %u)\n", w, h,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  s_wgt->fillSprite(bg);
  return true;
}

static void wEnd(int x, int y)
{
  if (s_backend.fb)
  {
    s_wgt->pushToSprite(s_backend.fb, x, y);
    if (s_backend.flushRect)
      s_backend.flushRect(x, y, s_wgt->width(), s_wgt->height());
  }
  else
  {
    s_wgt->pushSprite(x, y);
  }
  s_wgt->deleteSprite();
}

// ------------------------------------------------------------ draw helpers --

// fillScreen() is not virtual in TFT_eSPI, so it would ignore sprite targets
static void clearScreen(TFT_eSPI &d) { d.fillRect(0, 0, SCR_W, SCR_H, C_BG); }

static void text(TFT_eSPI &d, const char *s, int x, int y, unsigned size, uint16_t fg, uint16_t bg,
                 Align a = Align::TopLeft)
{
  render.setDrawer(d);
  render.setFontSize(size);
  render.setAlignment(a);
  render.drawString(s, x, y, fg, bg);
}

static int textW(const char *s, unsigned size)
{
  render.setFontSize(size);
  return render.getTextWidth("%s", s);
}

// value + smaller unit, centred on cx
static void textPair(TFT_eSPI &d, int cx, int y, const char *val, const char *unit, unsigned size,
                     uint16_t cv, uint16_t cu, uint16_t bg)
{
  unsigned usz = size > 12 ? size - 3 : size;
  int wv = textW(val, size);
  int wu = textW(unit, usz);
  int x0 = cx - (wv + 5 + wu) / 2;
  text(d, val, x0, y, size, cv, bg);
  text(d, unit, x0 + wv + 5, y + (size - usz), usz, cu, bg);
}

#define panel pulsePanel
static void gradBar(TFT_eSPI &d, int x, int y, int w, int h)
{
  pulseGradBar(d, x, y, w, h, cAlt, cAcc);
}

#define commas pulseCommas
static String fmtPrice(const String &s) { return pulseFmtPrice(s, Settings.Currency); }
static String btcPairLabel() { return pulseBtcPairLabel(Settings.Currency); }
#define fmtUptime pulseFmtUptime
#define fmtDate pulseFmtDate
#define wifiBars pulseWifiBars

static float lastKh = 0;

// The header follows what is actually happening: hashing whenever the hash
// counter moves, even while the pool connection is being made again.
static int uiStatus() { return pulseUiStatus(mMonitor.NerdStatus, lastKh); }

static void noteHashrate(float kh)
{
  pulseNoteHashrate(kh, lastKh, hist, HIST_N, histCount, histLastMs, HIST_EVERY_MS, histEpoch);
}

// -------------------------------------------------------------- gauge ring --

// Draws the segments whose level changed; flushes them unless the caller
// repaints the whole screen anyway.
static void gaugeDraw(TFT_eSPI &d, float v, bool flush)
{
  const bool flash = isShareFlashing();
  float pos = v * G_SEGS;
  int x0 = SCR_W, y0 = SCR_H, x1 = 0, y1 = 0;
  for (int i = 0; i < G_SEGS; i++)
  {
    float f = pos - i;
    uint8_t lvl = f >= 1.0f ? 4 : (f <= 0.05f ? 0 : 1 + (int)(f * 3.0f)); // 0, 1..3 (partial), 4
    if (lvl == segLvl[i])
      continue;
    segLvl[i] = lvl;
    uint16_t full = flash ? C_GOLD : mix565(cAlt, cAcc, (uint8_t)(i * 255 / (G_SEGS - 1)));
    uint16_t col = lvl == 0 ? C_TRACK : (lvl == 4 ? full : mix565(C_TRACK, full, lvl * 64));
    int a0 = 45 + i * 9; // 270 degree sweep, gap at 6 o'clock
    d.drawArc(G_CX, G_CY, G_R, G_RI, a0, a0 + 7, col, C_BG, true);

    if (flush && s_backend.flushRect)
    {
      // bounding box of the segment (drawArc angles: 0 at 6 o'clock, clockwise)
      for (int k = 0; k < 4; k++)
      {
        float a = (a0 + (k & 1 ? 7 : 0)) * DEG_TO_RAD;
        int rr = (k & 2) ? G_R : G_RI;
        int sx = G_CX - (int)(rr * sinf(a)), sy = G_CY + (int)(rr * cosf(a));
        x0 = min(x0, sx);
        x1 = max(x1, sx);
        y0 = min(y0, sy);
        y1 = max(y1, sy);
      }
    }
  }
  if (flush && s_backend.flushRect && x1 >= x0)
    s_backend.flushRect(x0 - 3, y0 - 3, x1 - x0 + 7, y1 - y0 + 7);
}

// ------------------------------------------------------------------ header --

struct HdrV
{
  String time;
  String ip;
  int bars;
  int status;
};

static void drawHeader(const HdrV &h)
{
  const uint32_t k = PulseKey().add(h.time).add(h.ip).add(h.bars).add(h.status).add(accentIdx).value();
  if (!need(W_HDR, k))
    return;
  const int X0 = 32, W = SCR_W - X0;
  if (!wBegin(W, HDR_H, C_BG))
    return;

  TFT_eSprite &wgt = *s_wgt;

  // wordmark
  int wn = textW("NERD", 19);
  text(wgt, "NERD", 2, 6, 19, C_TEXT, C_BG);
  text(wgt, "MINER", 2 + wn + 1, 6, 19, cAcc, C_BG);
  int wm = 2 + wn + 1 + textW("MINER", 19);

  // state chip
  const char *label = "SETUP";
  uint16_t col = C_DIM;
  if (h.status == NM_hashing)
  {
    label = "HASHING";
    col = C_GOOD;
  }
  else if (h.status == NM_Connecting)
  {
    label = "LINKING";
    col = C_WARN;
  }
  int cw = textW(label, 12) + 18;
  int cx = wm + 12;
  pulseDrawStatusBadge(wgt, render, label, cx, 7, cw, 20, 9, 12, col, C_BG);

  // time (right)
  text(wgt, h.time.c_str(), W - 10, 6, 19, C_TEXT, C_BG, Align::TopRight);

  // wifi bars
  for (int i = 0; i < 4; i++)
  {
    int bh = 5 + i * 4;
    wgt.fillRect(356 + i * 7, 26 - bh, 4, bh, i < h.bars ? cAcc : C_TRACK);
  }

  // local IP, right-aligned before the wifi bars; shrinks to fit beside the state chip
  if (h.ip.length())
  {
    const int right = 344, room = right - (cx + cw) - 10;
    unsigned sz = 16;
    while (sz > 10 && textW(h.ip.c_str(), sz) > room)
      sz--;
    text(wgt, h.ip.c_str(), right, 17 - (int)sz / 2 - 1, sz, C_DIM, C_BG, Align::TopRight);
  }

  wEnd(X0, 0);
  done(W_HDR, k);
}

// Breathing status dot, left of the wordmark. Called every animation frame.
static void drawPulse(unsigned long frame)
{
  static int lastLvl = -1;
  int ph = frame % 20;
  int lvl = ph < 10 ? ph : 20 - ph; // 0..10..0
  int st = uiStatus();
  uint16_t base = st == NM_hashing ? cAcc : (st == NM_Connecting ? C_WARN : C_DIM);
  int key = lvl + 11 * st + 40 * accentIdx;
  if (key == lastLvl)
    return;
  lastLvl = key;
  s_backend.canvas->fillSmoothCircle(17, 17, 5, mix565(C_TRACK, base, 70 + lvl * 18), C_BG);
  if (s_backend.flushRect)
    s_backend.flushRect(8, 8, 20, 20);
}

// -------------------------------------------------------------- pool strip --

struct StripV
{
  String l[4];
  String v[4];
};

static void chromeStrip(TFT_eSPI &d)
{
  panel(d, 10, STRIP_Y, 460, STRIP_H, 10, C_PANEL, C_EDGE, C_BG);
}

static void drawStrip(const StripV &s)
{
  for (int i = 0; i < 4; i++)
  {
    const uint32_t k = PulseKey().add(s.l[i]).add(s.v[i]).add(accentIdx).value();
    if (!need(W_P0 + i, k))
      continue;
    if (!wBegin(104, 40, C_PANEL))
      continue;
    TFT_eSprite &wgt = *s_wgt;
    text(wgt, s.l[i].c_str(), 8, 3, 12, C_DIM, C_PANEL);
    text(wgt, s.v[i].c_str(), 8, 18, 19, C_TEXT, C_PANEL);
    wEnd(20 + 112 * i, STRIP_Y + 4);
    done(W_P0 + i, k);
  }
}

static void chromeStripCells(TFT_eSPI &d)
{
  chromeStrip(d);
  for (int i = 1; i < 4; i++)
    d.drawFastVLine(16 + 112 * i, STRIP_Y + 10, STRIP_H - 20, C_EDGE);
}

// generic stat tile (label over value, coloured stripe at the left)
static void drawTile(int id, int x, int y, int w, int h, const char *label, const char *value,
                     uint16_t stripe, uint16_t valCol, uint16_t edge)
{
  const uint32_t k = PulseKey().add(label).add(value).add(stripe).add(valCol).add(edge).value();
  if (!need(id, k))
    return;
  if (!wBegin(w, h, C_BG))
    return;
  TFT_eSprite &wgt = *s_wgt;
  panel(wgt, 0, 0, w, h, 10, C_PANEL, edge, C_BG);
  int top = (h - 38) / 2;
  wgt.fillSmoothRoundRect(10, 10, 4, h - 20, 2, stripe, C_PANEL);
  text(wgt, label, 24, top, 12, C_DIM, C_PANEL);
  text(wgt, value, 24, top + 16, strlen(value) > 9 ? 16 : 20, valCol, C_PANEL);
  wEnd(x, y);
  done(id, k);
}

// ============================================================ MINE screen ==

struct MineV
{
  HdrV hdr;
  String hash, shares, best, tmpl, blocks, total;
  float kh;
  bool flash;
  StripV strip;
};

static const int TILE_Y[4] = {42, 96, 150, 204};

static void chromeMine(TFT_eSPI &d)
{
  memset(segLvl, 0xFF, sizeof(segLvl));
  gaugeDraw(d, gaugeShown, false);
  chromeStripCells(d);
}

static void paintMine(const MineV &m)
{
  drawHeader(m.hdr);

  // hero readout inside the ring
  uint32_t k = PulseKey().add(m.hash).add(accentIdx).value();
  if (need(W_HERO, k))
  {
    if (wBegin(140, 100, C_BG))
    {
      TFT_eSprite &wgt = *s_wgt;
      text(wgt, "HASHRATE", 70, 5, 12, C_DIM, C_BG, Align::TopCenter);
      unsigned sz = m.hash.length() <= 5 ? 48 : (m.hash.length() == 6 ? 40 : 34);
      text(wgt, m.hash.c_str(), 70, 20, sz, C_TEXT, C_BG, Align::TopCenter);
      text(wgt, "KH/s", 70, 75, 17, cAcc, C_BG, Align::TopCenter);
      wEnd(G_CX - 70, G_CY - 50);
      done(W_HERO, k);
    }
  }

  // lifetime hashes, in the gap at the bottom of the ring
  k = PulseKey().add(m.total).add(accentIdx).value();
  if (need(W_TOTAL, k))
  {
    if (wBegin(160, 22, C_BG))
    {
      TFT_eSprite &wgt = *s_wgt;
      textPair(wgt, 80, 2, commas(m.total).c_str(), "MH TOTAL", 15, C_TEXT, C_DIM, C_BG);
      wEnd(G_CX - 80, 232);
      done(W_TOTAL, k);
    }
  }

  const int tx = 262, tw = 208, th = 48;
  drawTile(W_T0, tx, TILE_Y[0], tw, th, "SHARES", m.shares.c_str(), cAcc, C_TEXT, m.flash ? cAcc : C_EDGE);
  drawTile(W_T1, tx, TILE_Y[1], tw, th, "BEST DIFF", m.best.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_T2, tx, TILE_Y[2], tw, th, "TEMPLATES", m.tmpl.c_str(), C_DIM, C_TEXT, C_EDGE);
  bool found = m.blocks.toInt() > 0;
  drawTile(W_T3, tx, TILE_Y[3], tw, th, "BLOCKS FOUND", m.blocks.c_str(), found ? C_GOLD : C_GOOD,
           found ? C_GOLD : C_TEXT, found ? C_GOLD : C_EDGE);

  drawStrip(m.strip);
}

// ========================================================== MARKET screen ==

struct MarketV
{
  HdrV hdr;
  String price, block, fee, diff, net;
  int pct;
  String remaining;
  StripV strip;
};

static void chromeMarket(TFT_eSPI &d)
{
  panel(d, 10, 42, 460, 92, 12, C_PANEL, C_EDGE, C_BG);
  text(d, btcPairLabel().c_str(), 26, 51, 12, C_DIM, C_PANEL);
  text(d, "LATEST BLOCK", 454, 51, 12, C_DIM, C_PANEL, Align::TopRight);
  panel(d, 10, 206, 460, 50, 10, C_PANEL, C_EDGE, C_BG);
  chromeStripCells(d);
}

static void paintMarket(const MarketV &m)
{
  drawHeader(m.hdr);

  uint32_t k = PulseKey().add(m.price).add(accentIdx).value();
  if (need(W_PRICE, k) && wBegin(264, 62, C_PANEL))
  {
    TFT_eSprite &wgt = *s_wgt;
    const unsigned len = m.price.length();
    text(wgt, m.price.c_str(), 2, 0, len > 8 ? 50 * 8 / len : 50, C_TEXT, C_PANEL);
    wEnd(24, 68);
    done(W_PRICE, k);
  }

  k = PulseKey().add(m.block).add(accentIdx).value();
  if (need(W_BLOCK, k) && wBegin(164, 62, C_PANEL))
  {
    TFT_eSprite &wgt = *s_wgt;
    text(wgt, m.block.c_str(), 160, 22, 30, cAcc, C_PANEL, Align::TopRight);
    wEnd(292, 68);
    done(W_BLOCK, k);
  }

  const int y = 142, w = 148, h = 56;
  drawTile(W_M0, 10, y, w, h, "FEE / HALF HOUR", m.fee.c_str(), cAcc, C_TEXT, C_EDGE);
  drawTile(W_M1, 166, y, w, h, "DIFFICULTY", m.diff.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_M2, 322, y, w, h, "NETWORK RATE", m.net.c_str(), C_DIM, C_TEXT, C_EDGE);

  const uint32_t kb = PulseKey().add(m.pct).add(m.remaining).add(accentIdx).value();
  if (need(W_HALV, kb) && wBegin(436, 40, C_PANEL))
  {
    TFT_eSprite &wgt = *s_wgt;
    text(wgt, "NEXT HALVING", 4, 2, 12, C_DIM, C_PANEL);
    char pc[8];
    snprintf(pc, sizeof(pc), "%d%%", m.pct);
    text(wgt, pc, 4 + textW("NEXT HALVING", 12) + 10, 0, 15, cAcc, C_PANEL);
    String rem = commas(m.remaining) + " blocks left";
    text(wgt, rem.c_str(), 432, 1, 13, C_TEXT, C_PANEL, Align::TopRight);
    wgt.fillSmoothRoundRect(4, 24, 428, 10, 5, C_TRACK, C_PANEL);
    int fw = (428 * m.pct) / 100;
    if (fw < 10 && m.pct > 0)
      fw = 10;
    if (fw > 0)
      gradBar(wgt, 4, 24, fw, 10);
    wEnd(22, 211);
    done(W_HALV, kb);
  }

  drawStrip(m.strip);
}

// ============================================================ CLOCK screen ==

struct ClockV
{
  HdrV hdr;
  String hh, mm;
  bool colon;
  String date, price, block, hash;
};

static void chromeClock(TFT_eSPI &d)
{
  chromeStrip(d);
}

static void drawChart()
{
  const uint32_t k = PulseKey().add(histEpoch).add(histCount).add(accentIdx).value();
  if (!need(W_CHART, k))
    return;
  const int W = 432, H = 38;
  if (!wBegin(W, H, C_PANEL))
    return;
  TFT_eSprite &wgt = *s_wgt;
  float mx = 1;
  for (int i = 0; i < histCount; i++)
    if (hist[i] > mx)
      mx = hist[i];
  mx *= 1.08f;
  const int step = 9;
  for (int i = 0; i < HIST_N; i++)
  {
    int slot = i - (HIST_N - histCount); // right-align so the newest bar is on the right
    int x = i * step;
    if (slot < 0)
    {
      wgt.fillRect(x, H - 2, 7, 2, C_TRACK); // not sampled yet
      continue;
    }
    int bh = (int)(H * hist[slot] / mx);
    if (bh < 2)
      bh = 2;
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (HIST_N - 1)));
    wgt.fillRect(x, H - bh, 7, bh, c);
  }
  wEnd(24, STRIP_Y + 5);
  done(W_CHART, k);
}

static void paintClock(const ClockV &c)
{
  drawHeader(c.hdr);

  // Glyph boxes start well above the digits (ascender space), so draw the
  // text above the widget: the digits then fill it.
  uint32_t k = PulseKey().add(c.hh).add(accentIdx).value();
  if (need(W_HH, k) && wBegin(168, 94, C_BG))
  {
    TFT_eSprite &wgt = *s_wgt;
    text(wgt, c.hh.c_str(), 166, -42, 124, C_TEXT, C_BG, Align::TopRight);
    wEnd(52, 40);
    done(W_HH, k);
  }
  k = PulseKey().add(c.mm).add(accentIdx).value();
  if (need(W_MM, k) && wBegin(168, 94, C_BG))
  {
    TFT_eSprite &wgt = *s_wgt;
    text(wgt, c.mm.c_str(), 2, -42, 124, cAcc, C_BG, Align::TopLeft);
    wEnd(260, 40);
    done(W_MM, k);
  }
  k = PulseKey().add(c.colon).add(accentIdx).value();
  if (need(W_COLON, k) && wBegin(40, 94, C_BG))
  {
    TFT_eSprite &wgt = *s_wgt;
    uint16_t dot = c.colon ? C_DIM : C_BG;
    wgt.fillSmoothCircle(20, 30, 7, dot, C_BG);
    wgt.fillSmoothCircle(20, 63, 7, dot, C_BG);
    wEnd(220, 40);
    done(W_COLON, k);
  }

  k = PulseKey().add(c.date).value();
  if (need(W_DATE, k) && wBegin(332, 24, C_BG))
  {
    TFT_eSprite &wgt = *s_wgt;
    text(wgt, c.date.c_str(), 166, 2, 16, C_DIM, C_BG, Align::TopCenter);
    wEnd(74, 142);
    done(W_DATE, k);
  }

  const int y = 180, w = 148, h = 64;
  drawTile(W_C0, 10, y, w, h, btcPairLabel().c_str(), c.price.c_str(), cAcc, C_TEXT, C_EDGE);
  drawTile(W_C1, 166, y, w, h, "BLOCK", c.block.c_str(), cAlt, C_TEXT, C_EDGE);
  drawTile(W_C2, 322, y, w, h, "HASHRATE KH/s", c.hash.c_str(), C_GOOD, C_TEXT, C_EDGE);

  drawChart();
}

// ------------------------------------------------------ boot / setup pages --

static void paintLoading(TFT_eSPI &d)
{
  clearScreen(d);
  // full-circle segmented ring, gradient around it
  const int cx = 240, cy = 116, r = 68, ri = 56, n = 24;
  for (int i = 0; i < n; i++)
  {
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (n - 1)));
    int a0 = i * 15;
    d.drawArc(cx, cy, r, ri, a0, a0 + 11, c, C_BG, true);
  }
  text(d, "#", cx, cy - 37, 74, C_TEXT, C_BG, Align::TopCenter);
  int wn = textW("NERD", 40), wm = textW("MINER", 40);
  int x0 = cx - (wn + wm + 3) / 2;
  text(d, "NERD", x0, 200, 40, C_TEXT, C_BG);
  text(d, "MINER", x0 + wn + 3, 200, 40, cAcc, C_BG);
  text(d, CURRENT_VERSION, cx, 258, 16, C_DIM, C_BG, Align::TopCenter);
  text(d, "STARTING UP", cx, 288, 12, C_DIM, C_BG, Align::TopCenter);
}

static void paintSetup(TFT_eSPI &d)
{
  clearScreen(d);
  text(d, "SETUP", 20, 10, 32, C_TEXT, C_BG);
  text(d, "MODE", 20 + textW("SETUP", 32) + 9, 10, 32, cAcc, C_BG);
  text(d, "Join the miner's WiFi to configure it", 20, 54, 15, C_DIM, C_BG);

  String ap = readCustomAPName();
  if (ap.length() == 0)
    ap = DEFAULT_SSID;
  const char *lab[3] = {"JOIN THIS WIFI NETWORK", "PASSWORD", "THEN OPEN IN A BROWSER"};
  const char *val[3] = {ap.c_str(), DEFAULT_WIFIPW, "192.168.4.1"};
  for (int i = 0; i < 3; i++)
  {
    int y = 86 + i * 66;
    panel(d, 10, y, 460, 58, 12, C_PANEL, C_EDGE, C_BG);
    d.fillSmoothCircle(44, y + 29, 17, mix565(C_PANEL, cAcc, 60), C_PANEL);
    char n[2] = {(char)('1' + i), 0};
    text(d, n, 44, y + 16, 22, cAcc, mix565(C_PANEL, cAcc, 60), Align::TopCenter);
    text(d, lab[i], 78, y + 8, 12, C_DIM, C_PANEL);
    text(d, val[i], 78, y + 24, 22, C_TEXT, C_PANEL);
  }
  text(d, "Save the form and the miner restarts by itself", 240, 292, 13, C_DIM, C_BG, Align::TopCenter);
}

// ---------------------------------------------------------------- chrome --

static void paintChrome(TFT_eSPI &d, int scr)
{
  clearScreen(d);
  d.drawFastHLine(0, HDR_H, SCR_W, C_EDGE);
  switch (scr)
  {
  case SCR_MINE:
    chromeMine(d);
    break;
  case SCR_MARKET:
    chromeMarket(d);
    break;
  default:
    chromeClock(d);
    break;
  }
}

static void ensureChrome(int scr)
{
  if (uiScreen == scr)
    return;
  uiScreen = scr;
  resetWidgets();
  paintChrome(*s_backend.canvas, scr);
  if (s_backend.flushAll)
    s_backend.flushAll();
}

static void repaintCached(int scr);

static void setAccent(int idx)
{
  accentIdx = idx % ACCENT_COUNT;
  cAcc = PULSE_ACCENTS[accentIdx].main;
  cAlt = PULSE_ACCENTS[accentIdx].alt;
  if (uiLive)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    uiScreen = -1;
    ensureChrome(s);
    repaintCached(s);
  }
}

// Last view painted per screen, so a page change can repaint instantly
// instead of waiting for the next (possibly network-blocked) data tick.
static MineV lastMine;
static MarketV lastMarket;
static ClockV lastClock;
static bool haveMine = false, haveMarket = false, haveClock = false;

static void repaintCached(int scr)
{
  if (scr == SCR_MINE && haveMine)
    paintMine(lastMine);
  else if (scr == SCR_MARKET && haveMarket)
    paintMarket(lastMarket);
  else if (scr == SCR_CLOCK && haveClock)
    paintClock(lastClock);
}

// ------------------------------------------------------- saved UI choices --
// Accent colour and last-viewed page live in their own NVS namespace (the
// pool/wallet/brightness/invert/flip settings are in the main config file).

#define UI_PREFS_NS "pulse_ui"

static int savedAccent = 0;
static int savedPage = 0;
static int restorePage = 0; // applied on the first data tick (the monitor resets the page to 0 at start)

static void loadUiPrefs()
{
  pulseLoadUiPrefs(UI_PREFS_NS, accentIdx, cAcc, cAlt, savedAccent, savedPage, restorePage);
}

static void saveUiPrefs()
{
  pulseSaveUiPrefs(UI_PREFS_NS, accentIdx, currentDisplayDriver->current_cyclic_screen, savedAccent, savedPage);
}

// -------------------------------------------------------------- live data --

static void refreshPool()
{
  // Screen off: nobody sees the pool stats, and each fetch is an HTTPS request
  // that pauses the HW miner. The last values stay; the next draw with the
  // screen on fetches again if they are older than UPDATE_POOL_min.
  if (!screenLit())
    return;
  if (Settings.PoolAddress != "tn.vkbit.com")
  {
    pData = getPoolData(); // throttled internally (UPDATE_POOL_min)
  }
  else
  {
    pData.bestDifficulty = "TESTNET";
    pData.workersHash = "TESTNET";
    pData.workersCount = 1;
    mPoolUpdate = millis();
  }
}

static HdrV liveHeader(const String &time)
{
  HdrV h;
  h.time = time;
  h.ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
  h.bars = wifiBars();
  h.status = uiStatus();
  return h;
}

static void logShare(const String &shares, const String &khashes, const String &rate)
{
  // Parsed by the bench scripts (see project notes) -- keep the format.
  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                shares.c_str(), khashes.c_str(), rate.c_str());
}

static void paintIdentifyScreen()
{
  const String host = Settings.Hostname[0] ? String(Settings.Hostname) : String("nerdminer");
  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("No WiFi");
  const String wallet = Settings.BtcWallet;
  const int dot = wallet.indexOf('.');
  const String worker = (dot >= 0 && dot + 1 < (int)wallet.length()) ? wallet.substring(dot) : String();

  const uint32_t staticKey = PulseKey().add(host).add(ip).add(worker).add(accentIdx).value();
  if (!s_showingIdentify || s_identifyStaticKey != staticKey)
  {
    s_showingIdentify = true;
    s_identifyStaticKey = staticKey;
    s_lastIdentifySec = -1;
    uiScreen = -1;

    TFT_eSPI &d = *s_backend.canvas;
    clearScreen(d);
    panel(d, 8, 8, SCR_W - 16, SCR_H - 16, 16, C_BG, cAcc, C_BG);
    d.drawRoundRect(11, 11, SCR_W - 22, SCR_H - 22, 13, cAcc);

    const int bw = textW("LOCATE MINER", 16) + 32;
    const uint16_t badgeBg = mix565(C_BG, cAcc, 56);
    d.fillSmoothRoundRect((SCR_W - bw) / 2, 24, bw, 28, 14, badgeBg, C_BG);
    text(d, "LOCATE MINER", SCR_W / 2, 30, 16, cAcc, badgeBg, Align::TopCenter);

    panel(d, 24, 64, SCR_W - 48, 96, 12, C_PANEL, C_EDGE, C_BG);
    unsigned hsz = 56;
    while (hsz > 22 && textW(host.c_str(), hsz) > SCR_W - 72)
      hsz -= 2;
    const int hy = 64 + (96 - (int)hsz) / 2 - 6;
    text(d, host.c_str(), SCR_W / 2, hy, hsz, cAcc, C_PANEL, Align::TopCenter);

    if (worker.length() > 0)
    {
      panel(d, 24, 174, 252, 66, 10, C_PANEL, C_EDGE, C_BG);
      text(d, "IP ADDRESS", 40, 183, 12, C_DIM, C_PANEL);
      unsigned ipsz = 20;
      while (ipsz > 12 && textW(ip.c_str(), ipsz) > 222)
        ipsz--;
      text(d, ip.c_str(), 40, 203, ipsz, C_TEXT, C_PANEL);

      panel(d, 288, 174, 168, 66, 10, C_PANEL, C_EDGE, C_BG);
      text(d, "WORKER", 304, 183, 12, C_DIM, C_PANEL);
      unsigned wsz = 20;
      while (wsz > 12 && textW(worker.c_str(), wsz) > 138)
        wsz--;
      text(d, worker.c_str(), 304, 203, wsz, C_GOOD, C_PANEL);
    }
    else
    {
      panel(d, 24, 174, SCR_W - 48, 66, 10, C_PANEL, C_EDGE, C_BG);
      text(d, "IP ADDRESS", SCR_W / 2, 183, 12, C_DIM, C_PANEL, Align::TopCenter);
      text(d, ip.c_str(), SCR_W / 2, 202, 22, C_TEXT, C_PANEL, Align::TopCenter);
    }
    if (s_backend.flushAll)
      s_backend.flushAll();
  }

  const int secs = (int)((identifyRemainingMs() + 999U) / 1000U);
  if (secs != s_lastIdentifySec)
  {
    s_lastIdentifySec = secs;
    char cd[32];
    snprintf(cd, sizeof(cd), "RETURNING IN %ds", secs);
    if (wBegin(300, 34, C_BG))
    {
      text(*s_wgt, cd, 150, 6, 17, C_TEXT, C_BG, Align::TopCenter);
      wEnd((SCR_W - 300) / 2, 256);
    }
  }
}

void pulse480MinerScreen(unsigned long mElapsed)
{
  if (restorePage > 0 && restorePage < currentDisplayDriver->num_cyclic_screens)
  {
    // First tick after boot: go straight to the page that was showing before.
    int p = restorePage;
    restorePage = 0;
    currentDisplayDriver->current_cyclic_screen = p;
    currentDisplayDriver->cyclic_screens[p](mElapsed);
    return;
  }
  restorePage = 0;
  if (isIdentifying())
  {
    uiLive = true;
    paintIdentifyScreen();
    return;
  }
  s_showingIdentify = false;
  mining_data data = getMiningData(mElapsed);
  refreshPool();
  uiLive = true;
  ensureChrome(SCR_MINE);

  float kh = data.currentHashRate.toFloat();
  noteHashrate(kh);
  gaugeTarget = constrain(kh / s_backend.gaugeMaxKh, 0.0f, 1.0f);

  uint32_t sh = data.completedShares.toInt();
  if (sh > lastShares && lastShares != 0)
    shareFlashUntil = millis() + 1500;
  lastShares = sh;

  MineV m;
  m.hdr = liveHeader(data.currentTime);
  m.hash = data.currentHashRate;
  m.shares = commas(data.completedShares);
  m.best = data.bestDiff;
  m.tmpl = commas(data.templates);
  m.blocks = data.valids;
  m.total = data.totalMHashes;
  m.kh = kh;
  m.flash = isShareFlashing();
  m.strip.l[0] = "WORKERS";
  m.strip.v[0] = String(pData.workersCount);
  m.strip.l[1] = "POOL RATE";
  m.strip.v[1] = pData.workersHash;
  m.strip.l[2] = "POOL BEST";
  m.strip.v[2] = pData.bestDifficulty;
  m.strip.l[3] = "UPTIME";
  m.strip.v[3] = fmtUptime(data.timeMining);
  lastMine = m;
  haveMine = true;
  paintMine(m);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

void pulse480MarketScreen(unsigned long mElapsed)
{
  if (isIdentifying())
  {
    uiLive = true;
    paintIdentifyScreen();
    return;
  }
  s_showingIdentify = false;
  coin_data data = getCoinData(mElapsed);
  refreshPool();
  uiLive = true;
  ensureChrome(SCR_MARKET);
  noteHashrate(data.currentHashRate.toFloat());

  MarketV m;
  m.hdr = liveHeader(data.currentTime);
  m.price = fmtPrice(data.btcPrice);
  m.block = commas(data.blockHeight);
  m.fee = data.halfHourFee.startsWith("0 ") ? String("--") : data.halfHourFee;
  m.diff = data.netwrokDifficulty.length() ? data.netwrokDifficulty : String("--");
  m.net = data.globalHashRate.length() ? data.globalHashRate + " EH/s" : String("--");
  m.pct = (int)data.progressPercent;
  m.remaining = String(data.remainingBlocks.toInt());
  m.strip.l[0] = "WORKERS";
  m.strip.v[0] = String(pData.workersCount);
  m.strip.l[1] = "POOL RATE";
  m.strip.v[1] = pData.workersHash;
  m.strip.l[2] = "POOL BEST";
  m.strip.v[2] = pData.bestDifficulty;
  m.strip.l[3] = "SHARES";
  m.strip.v[3] = commas(data.completedShares);
  lastMarket = m;
  haveMarket = true;
  paintMarket(m);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

void pulse480ClockScreen(unsigned long mElapsed)
{
  if (isIdentifying())
  {
    uiLive = true;
    paintIdentifyScreen();
    return;
  }
  s_showingIdentify = false;
  clock_data data = getClockData(mElapsed);
  uiLive = true;
  ensureChrome(SCR_CLOCK);
  noteHashrate(data.currentHashRate.toFloat());

  ClockV c;
  c.hdr = liveHeader(data.currentTime);
  int colon = data.currentTime.indexOf(':');
  c.hh = colon > 0 ? data.currentTime.substring(0, colon) : String("--");
  c.mm = colon > 0 ? data.currentTime.substring(colon + 1) : String("--");
  c.colon = (millis() / 1000) & 1;
  c.date = fmtDate(data.currentDate);
  c.price = fmtPrice(data.btcPrice);
  c.block = commas(data.blockHeight);
  c.hash = data.currentHashRate;
  lastClock = c;
  haveClock = true;
  paintClock(c);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

// --------------------------------------------------------- driver plumbing --

bool pulse480Init(const Pulse480Backend &backend)
{
  s_backend = backend;
  if (!s_wgt && s_backend.tft)
    s_wgt = new TFT_eSprite(s_backend.tft);

  loadUiPrefs();

  if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold)))
  {
    Serial.println("Initialise error");
    return false;
  }

  pData.bestDifficulty = "0";
  pData.workersHash = "0";
  pData.workersCount = 0;

  resetWidgets();
  memset(segLvl, 0xFF, sizeof(segLvl));
  return true;
}

void pulse480AlternateScreenState(void)
{
  Serial.println("Switching display state");
  int screen_state_duty = ledcRead(0);
  // Switching the duty cycle for the ledc channel, where the backlight pin is attached.
  if (screen_state_duty > 0)
  {
    ledcWrite(0, 0);
  }
  else
  {
    ledcWrite(0, backlightLevel()); // day, night or in between
  }
}

static void applyFlip()
{
  if (s_backend.flipHardware)
    s_backend.flipHardware();
  if (s_backend.redrawOnFlip)
  {
    if (uiLive)
    {
      int s = currentDisplayDriver->current_cyclic_screen;
      uiScreen = -1;
      ensureChrome(s);
      repaintCached(s);
    }
    else if (staticPage == SP_LOADING)
      paintLoading(*s_backend.canvas);
    else if (staticPage == SP_SETUP)
      paintSetup(*s_backend.canvas);
  }
}

void pulse480AlternateRotation(void)
{
  if (uiLive)
    pendingFlip = true;
  else
    applyFlip();
}

void pulse480LoadingScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  staticPage = SP_LOADING;
  paintLoading(*s_backend.canvas);
  if (s_backend.flushAll)
    s_backend.flushAll();
}

void pulse480SetupScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  staticPage = SP_SETUP;
  paintSetup(*s_backend.canvas);
  if (s_backend.flushAll)
    s_backend.flushAll();
}

void pulse480AnimateCurrentScreen(unsigned long frame)
{
  if (isIdentifying() || !uiLive || uiScreen < 0)
    return;
  drawPulse(frame);
  if (uiScreen == SCR_MINE)
  {
    const bool flash = isShareFlashing();
    if (flash != s_lastRingFlash)
    {
      s_lastRingFlash = flash;
      memset(segLvl, 0xFF, sizeof(segLvl));
      gaugeDraw(*s_backend.canvas, gaugeShown, true);
    }
    else if (fabsf(gaugeTarget - gaugeShown) > 0.002f)
    {
      gaugeShown += (gaugeTarget - gaugeShown) * 0.3f; // ease toward the measured rate
      gaugeDraw(*s_backend.canvas, gaugeShown, true);
    }
  }
}

void pulse480PostTouchAction(int action)
{
  pendingTouch = action;
}

int pulse480ClassifyTouch(int x, int y, bool backlightOff)
{
  if (backlightOff)
    return PULSE480_TA_BACKLIGHT; // dark: any touch turns the backlight back on
  if (y < TOUCH_CORNER_H && x > SCR_W - TOUCH_CORNER_W)
    return PULSE480_TA_BACKLIGHT; // top-right: backlight on/off
  if (y < TOUCH_CORNER_H && x < TOUCH_CORNER_W)
    return PULSE480_TA_ACCENT; // wordmark: next accent colour
  return x > SCR_W / 2 ? PULSE480_TA_NEXT : PULSE480_TA_PREV;
}

void pulse480DoLedStuff(unsigned long frame)
{
  (void)frame;
  int action = pendingTouch;
  pendingTouch = PULSE480_TA_NONE;
  if (action != PULSE480_TA_NONE)
    noteScreenActivity(); // screen timeout
  DisplayDriver *drv = currentDisplayDriver;
  switch (action)
  {
  case PULSE480_TA_BACKLIGHT:
    pulse480AlternateScreenState();
    break;
  case PULSE480_TA_ACCENT:
    setAccent(accentIdx + 1);
    break;
  case PULSE480_TA_NEXT:
    drv->current_cyclic_screen = (drv->current_cyclic_screen + 1) % drv->num_cyclic_screens;
    break;
  case PULSE480_TA_PREV:
    drv->current_cyclic_screen = drv->current_cyclic_screen - 1;
    if (drv->current_cyclic_screen < 0)
      drv->current_cyclic_screen = drv->num_cyclic_screens - 1;
    break;
  }

  if (pendingFlip)
  {
    pendingFlip = false;
    applyFlip();
  }

  const bool celebrating = isCelebrating();
  if (celebrating && !s_wasCelebrating)
  {
    shareFlashUntil = millis() + 2000;
  }
  s_wasCelebrating = celebrating;

  if (isIdentifying())
  {
    if (uiLive)
      paintIdentifyScreen();
    return;
  }
  if (s_showingIdentify)
  {
    s_showingIdentify = false;
    int s = currentDisplayDriver->current_cyclic_screen;
    ensureChrome(s);
    repaintCached(s);
  }

  // Screen changed by touch or the boot button: draw the new page right away
  // from the last values it showed; the next data tick refreshes them.
  if (uiLive && currentDisplayDriver->current_cyclic_screen != uiScreen)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    ensureChrome(s);
    repaintCached(s);
  }
  if (uiLive)
    saveUiPrefs();
}

#ifdef ES3C35P_UI_SCREENSHOT
void pulse480RunScreenshotDump(void (*dumpShot)(const char *name))
{
  const int bootAccent = accentIdx;
  HdrV h;
  h.time = "21:47";
  h.ip = "192.168.100.100";
  h.bars = 3;
  h.status = NM_hashing;

  for (int i = 0; i < HIST_N; i++)
    hist[i] = 310 + 30 * sinf(i * 0.45f) + (i * 7 % 23);
  histCount = HIST_N;
  histEpoch = 1;

  for (int acc = 0; acc < 2; acc++)
  {
    // accent 0 shows every screen; accent 2 (amber/rose) shows the mining screen again
    setAccent(acc == 0 ? 0 : 2);

    // loading + setup
    paintLoading(*s_backend.canvas);
    dumpShot(acc == 0 ? "loading" : "loading_alt");
    if (acc == 0)
    {
      paintSetup(*s_backend.canvas);
      dumpShot("setup");
    }

    // mine
    uiLive = true;
    gaugeShown = gaugeTarget = 0.84f;
    uiScreen = -1;
    ensureChrome(SCR_MINE);
    MineV m;
    m.hdr = h;
    m.hash = "372.4";
    m.shares = "1,284";
    m.best = "8.73M";
    m.tmpl = "412";
    m.blocks = "0";
    m.total = "58213";
    m.kh = 372.4f;
    m.flash = acc == 1;
    m.strip.l[0] = "WORKERS";
    m.strip.v[0] = "3";
    m.strip.l[1] = "POOL RATE";
    m.strip.v[1] = "1.92M";
    m.strip.l[2] = "POOL BEST";
    m.strip.v[2] = "412.6M";
    m.strip.l[3] = "UPTIME";
    m.strip.v[3] = "3d 07h";
    paintMine(m);
    dumpShot(acc == 0 ? "mine" : "mine_alt");
    if (acc == 1)
      continue;

    // market
    uiScreen = -1;
    ensureChrome(SCR_MARKET);
    MarketV k;
    k.hdr = h;
    k.price = "$97,412";
    k.block = "912,345";
    k.fee = "12 sat/vB";
    k.diff = "146.72T";
    k.net = "912 EH/s";
    k.pct = 47;
    k.remaining = "111655";
    k.strip = m.strip;
    k.strip.l[3] = "SHARES";
    k.strip.v[3] = "1,284";
    paintMarket(k);
    dumpShot("market");

    // clock
    uiScreen = -1;
    ensureChrome(SCR_CLOCK);
    ClockV c;
    c.hdr = h;
    c.hh = "21";
    c.mm = "47";
    c.colon = true;
    c.date = fmtDate("30/09/2026");
    c.price = "$97,412";
    c.block = "912,345";
    c.hash = "372.4";
    paintClock(c);
    dumpShot("clock");
  }

  uiLive = false;
  uiScreen = -1;
  setAccent(bootAccent);
  resetWidgets();
  histCount = 0;
}
#endif

#endif // ES3C35P_DISPLAY || ESP32_4IN_ST7796_DISPLAY
