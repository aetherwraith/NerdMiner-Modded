// "PULSE" theme for the LilyGO T-Display 1.14" (ST7789, 240x135 landscape).
//
// The CYD's Pulse look (esp23_2432s028r.cpp) laid out again for a screen with
// 42% of the pixels: no bitmaps, everything drawn from primitives through one
// small scratch sprite. The classic driver kept a 240x135 16-bit sprite
// (~64 KB) for the whole life of the miner, which left too little heap for the
// HTTPS fetches (price, block height, global stats) to connect.
//
//   MINE    segmented hashrate ring, four stat rows
//   MARKET  BTC price, block height, fee / difficulty / network, halving
//   CLOCK   big desk clock, date, key numbers, hashrate history bars
//   POOL    workers, pool rate, pool best, uptime, pool, IP address
//
// Buttons (NerdMinerV2.ino.cpp): right button (GPIO35) click = next screen,
// double click = next accent colour, hold 5 s = reset the configuration;
// left button (GPIO0) click = screen off/on, double click = flip 180 degrees.
//
// The original look is kept in tDisplayV1Driver.cpp (build the
// TTGO-T-Display-Classic env, or define TDISPLAY_THEME_CLASSIC).
//
// Also used by the M5StickC Plus2 (env M5Stick-C-Plus2-Pulse): same 240x135
// ST7789. There the right-side button B is "next screen" (double click = next
// accent), the front button A is screen off/on (double click = flip), the
// header shows the battery level and the LED / buzzer / low-battery power-off
// come from m5stickPlus2Hw.cpp.
//
// Dev aid: -D TDISPLAY_UI_SCREENSHOT=1 renders every screen once at boot into
// an offscreen 8-bit sprite, keeps them run-length encoded and serves them at
// http://<ip>/shot?i=<n> (tools/tdisplay_shots.py turns them into PNGs).
// The shots hold ~70 KB of heap for good, enough to make WiFi sluggish and
// drop the pool after a few minutes: flash a normal build afterwards.
#include "displayDriver.h"
#include "display.h"

#ifdef TDISPLAY_PULSE

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include "media/myFonts.h"
#include "version.h"
#include "monitor.h"
#include "OpenFontRender.h"
#include "rotation.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "currency.h"
#include "pulseUi.h"
#ifdef M5STICK_C_PLUS2
#include "m5stickPlus2Hw.h"
#endif

extern nvMemory nvMem;
extern monitor_data mMonitor;
extern pool_data pData;
extern DisplayDriver *currentDisplayDriver;
extern TSettings Settings;
extern unsigned long mPoolUpdate;
extern String readCustomAPName(); // wManager.cpp

OpenFontRender render;
TFT_eSPI tft = TFT_eSPI(); // pins in lib/TFT_eSPI/User_Setups/Setup25_TTGO_T_Display.h (Plus2: Setup304)

// ---------------------------------------------------------------- palette --

#define ACCENT_COUNT PULSE_ACCENT_COUNT

static int accentIdx = 0;
static uint16_t cAcc = PULSE_ACCENTS[0].main;
static uint16_t cAlt = PULSE_ACCENTS[0].alt;

#define mix565 pulseMix565

// --------------------------------------------------------------- geometry --

#define SCR_W 240
#define SCR_H 135
#define HDR_H 18 // header band; hairline sits on this row

#define SCR_MINE 0
#define SCR_MARKET 1
#define SCR_CLOCK 2
#define SCR_POOL 3

// Hashrate ring
#define G_CX 54
#define G_CY 77
#define G_R 52
#define G_RI 45
#define G_SEGS 30
#ifdef M5STICK_C_PLUS2
#define G_MAX_KH 1200.0f // full scale: 40 KH/s per segment (the Plus2 runs ~990, like the CYD)
#else
#define G_MAX_KH 600.0f // full scale: 20 KH/s per segment (the T-Display runs ~400)
#endif

// --------------------------------------------------------------- UI state --

static int uiScreen = -1;   // screen whose chrome is currently on the panel
static bool uiLive = false; // false while the boot / setup screens own the panel
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

static bool isShareFlashing()
{
  return pulseIsShareFlashing(shareFlashUntil);
}

// hashrate history for the CLOCK screen
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
  W_T0, // 4 mine rows
  W_T1,
  W_T2,
  W_T3,
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
  W_P0, // 6 pool tiles
  W_P1,
  W_P2,
  W_P3,
  W_P4,
  W_P5,
  W_COUNT
};
static uint32_t wkey[W_COUNT];

static bool need(int id, uint32_t k) { return pulseNeed(wkey, id, k); }
static void done(int id, uint32_t k) { pulseDone(wkey, id, k); }
static void resetWidgets() { pulseResetWidgets(wkey, W_COUNT); }

// ---------------------------------------------------------- output target --
// Everything draws either to the panel or (dev builds) to an offscreen shot.

static TFT_eSprite wgt = TFT_eSprite(&tft); // scratch sprite reused by every widget

#ifdef TDISPLAY_UI_SCREENSHOT
static TFT_eSprite shot = TFT_eSprite(&tft);
static bool shotOn = false;
static TFT_eSPI &out() { return shotOn ? (TFT_eSPI &)shot : tft; }
#else
static TFT_eSPI &out() { return tft; }
#endif

static bool wBegin(int w, int h, uint16_t bg)
{
  wgt.setColorDepth(16);
  wgt.createSprite(w, h);
  if (!wgt.created())
  {
    Serial.printf("[ui] sprite %dx%d alloc failed (free %u, max block %u)\n", w, h,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  wgt.fillSprite(bg);
  return true;
}

static void wEnd(int x, int y)
{
#ifdef TDISPLAY_UI_SCREENSHOT
  if (shotOn)
  {
    wgt.pushToSprite(&shot, x, y);
    wgt.deleteSprite();
    return;
  }
#endif
  wgt.pushSprite(x, y);
  wgt.deleteSprite();
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

// largest size <= max (and >= min) at which s fits in room pixels
static unsigned fitSize(const char *s, unsigned max, unsigned min, int room)
{
  unsigned sz = max;
  while (sz > min && textW(s, sz) > room)
    sz--;
  return sz;
}

#define panel pulsePanel
static void gradBar(TFT_eSPI &d, int x, int y, int w, int h)
{
  pulseGradBar(d, x, y, w, h, cAlt, cAcc);
}

#define commas pulseCommas
static String fmtPrice(const String &s) { return pulseFmtPrice(s, Settings.Currency); }
static String btcPairLabel() { return pulseBtcPairLabel(Settings.Currency); }

// The block height is only real once it has been fetched: before that the
// monitor hands out a hard-coded default.
static String fmtBlock(const String &s)
{
  return s.toInt() > 0 && s != "793261" ? commas(s) : String("--");
}

#define fmtUptime pulseFmtUptime
#define fmtDate pulseFmtDate
#define wifiBars pulseWifiBars

static float lastKh = 0;

// The header follows what is actually happening: hashing whenever the hash
// counter moves, even while the pool connection is being made again.
static int uiStatus()
{
  if (mMonitor.NerdStatus == NM_waitingConfig)
    return NM_waitingConfig;
  return (WiFi.status() == WL_CONNECTED && lastKh > 0) ? NM_hashing : NM_Connecting;
}

static void noteHashrate(float kh)
{
  lastKh = kh;
  unsigned long now = millis();
  if (histCount != 0 && now - histLastMs < HIST_EVERY_MS)
    return;
  histLastMs = now;
  if (histCount < HIST_N)
    hist[histCount++] = kh;
  else
  {
    memmove(hist, hist + 1, (HIST_N - 1) * sizeof(float));
    hist[HIST_N - 1] = kh;
  }
  histEpoch++;
}

// -------------------------------------------------------------- gauge ring --

static void gaugeDraw(TFT_eSPI &d, float v)
{
  float pos = v * G_SEGS;
  for (int i = 0; i < G_SEGS; i++)
  {
    float f = pos - i;
    uint8_t lvl = f >= 1.0f ? 4 : (f <= 0.05f ? 0 : 1 + (int)(f * 3.0f)); // 0, 1..3 (partial), 4
    if (lvl == segLvl[i])
      continue;
    segLvl[i] = lvl;
    uint16_t full = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (G_SEGS - 1)));
    uint16_t col = lvl == 0 ? C_TRACK : (lvl == 4 ? full : mix565(C_TRACK, full, lvl * 64));
    int a0 = 45 + i * 9; // 270 degree sweep, gap at 6 o'clock
    d.drawArc(G_CX, G_CY, G_R, G_RI, a0, a0 + 7, col, C_BG, true);
  }
}

// ------------------------------------------------------------------ header --

struct HdrV
{
  String time;
  int bars;
  int status;
  int batt = -1; // battery %, -1 = none (only the M5StickC Plus2 has one)
};

static void drawHeader(const HdrV &h)
{
  const uint32_t k = PulseKey().add(h.time).add(h.bars).add(h.status).add(h.batt).add(accentIdx).value();
  if (!need(W_HDR, k))
    return;
  const int X0 = 13, W = SCR_W - X0;
  if (!wBegin(W, HDR_H, C_BG))
    return;

  // wordmark
  int wn = textW("NERD", 13);
  text(wgt, "NERD", 0, 2, 13, C_TEXT, C_BG);
  text(wgt, "MINER", wn + 1, 2, 13, cAcc, C_BG);
  int wm = wn + 1 + textW("MINER", 13);

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
  int cw = textW(label, 9) + 10;
  int cx = wm + 6;
  wgt.fillSmoothRoundRect(cx, 3, cw, 12, 6, mix565(C_BG, col, 46), C_BG);
  text(wgt, label, cx + cw / 2, 4, 9, col, mix565(C_BG, col, 46), Align::TopCenter);

  // time (right), wifi bars left of it
  text(wgt, h.time.c_str(), W - 3, 2, 13, C_TEXT, C_BG, Align::TopRight);
  int bx = W - 3 - textW(h.time.c_str(), 13) - 17;
  for (int i = 0; i < 4; i++)
  {
    int bh = 3 + i * 2;
    wgt.fillRect(bx + i * 3, 13 - bh, 2, bh, i < h.bars ? cAcc : C_TRACK);
  }

  // battery level left of the wifi bars, in the gap after the state chip
  // ("100%" is 23px, the gap ~32px)
  if (h.batt >= 0)
  {
    char b[8];
    snprintf(b, sizeof(b), "%d%%", h.batt);
    uint16_t bc = h.batt > 50 ? C_GOOD : (h.batt > 20 ? C_WARN : C_BAD);
    text(wgt, b, bx - 4, 4, 9, bc, C_BG, Align::TopRight);
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
  out().fillSmoothCircle(6, 9, 3, mix565(C_TRACK, base, 70 + lvl * 18), C_BG);
}

// Stat tile: label over value, coloured stripe at the left. The value shrinks
// to fit the tile.
static void drawTile(int id, int x, int y, int w, int h, const char *label, const char *value,
                     unsigned vmax, uint16_t stripe, uint16_t valCol, uint16_t edge)
{
  const uint32_t k = PulseKey().add(label).add(value).add(stripe).add(valCol).add(edge).value();
  if (!need(id, k))
    return;
  if (!wBegin(w, h, C_BG))
    return;
  panel(wgt, 0, 0, w, h, 6, C_PANEL, edge, C_BG);
  wgt.fillSmoothRoundRect(5, 5, 2, h - 10, 1, stripe, C_PANEL);
  unsigned vs = fitSize(value, vmax, 9, w - 18);
  int top = (h - 10 - (int)vs) / 2; // label 8 + 2 gap + value
  text(wgt, label, 11, top, 9, C_DIM, C_PANEL);
  text(wgt, value, 11, top + 10, vs, valCol, C_PANEL);
  wEnd(x, y);
  done(id, k);
}

// Stat row: label left, value right, on one line.
static void drawRow(int id, int x, int y, int w, int h, const char *label, const char *value,
                    uint16_t stripe, uint16_t valCol, uint16_t edge)
{
  const uint32_t k = PulseKey().add(label).add(value).add(stripe).add(valCol).add(edge).value();
  if (!need(id, k))
    return;
  if (!wBegin(w, h, C_BG))
    return;
  panel(wgt, 0, 0, w, h, 6, C_PANEL, edge, C_BG);
  wgt.fillSmoothRoundRect(5, 5, 2, h - 10, 1, stripe, C_PANEL);
  text(wgt, label, 11, (h - 9) / 2, 9, C_DIM, C_PANEL);
  int room = w - 11 - textW(label, 9) - 12;
  unsigned vs = fitSize(value, 15, 9, room);
  text(wgt, value, w - 6, (h - (int)vs) / 2 - 1, vs, valCol, C_PANEL, Align::TopRight);
  wEnd(x, y);
  done(id, k);
}

// ============================================================ MINE screen ==

struct MineV
{
  HdrV hdr;
  String hash, shares, best, tmpl, blocks, total;
  bool flash;
};

static void chromeMine(TFT_eSPI &d)
{
  memset(segLvl, 0xFF, sizeof(segLvl));
  gaugeDraw(d, gaugeShown);
}

static void paintMine(const MineV &m)
{
  drawHeader(m.hdr);

  // hero readout inside the ring
  uint32_t k = PulseKey().add(m.hash).add(accentIdx).value();
  if (need(W_HERO, k) && wBegin(78, 54, C_BG))
  {
    text(wgt, "HASHRATE", 39, 1, 9, C_DIM, C_BG, Align::TopCenter);
    unsigned sz = fitSize(m.hash.c_str(), 26, 16, 76);
    text(wgt, m.hash.c_str(), 39, 12 + (26 - sz) / 2, sz, C_TEXT, C_BG, Align::TopCenter);
    text(wgt, "KH/s", 39, 41, 11, cAcc, C_BG, Align::TopCenter);
    wEnd(G_CX - 39, G_CY - 28);
    done(W_HERO, k);
  }

  // lifetime hashes, in the gap at the bottom of the ring
  k = PulseKey().add(m.total).add(accentIdx).value();
  if (need(W_TOTAL, k) && wBegin(80, 13, C_BG))
  {
    String t = commas(m.total) + " MH";
    text(wgt, t.c_str(), 40, 1, fitSize(t.c_str(), 10, 8, 78), C_DIM, C_BG, Align::TopCenter);
    wEnd(G_CX - 40, 121);
    done(W_TOTAL, k);
  }

  const int tx = 112, tw = 125, th = 25;
  drawRow(W_T0, tx, 21, tw, th, "SHARES", m.shares.c_str(), cAcc, C_TEXT, m.flash ? cAcc : C_EDGE);
  drawRow(W_T1, tx, 49, tw, th, "BEST DIFF", m.best.c_str(), cAlt, C_TEXT, C_EDGE);
  drawRow(W_T2, tx, 77, tw, th, "TEMPLATES", m.tmpl.c_str(), C_DIM, C_TEXT, C_EDGE);
  bool found = m.blocks.toInt() > 0;
  drawRow(W_T3, tx, 105, tw, th, "BLOCKS", m.blocks.c_str(), found ? C_GOLD : C_GOOD,
          found ? C_GOLD : C_TEXT, found ? C_GOLD : C_EDGE);
}

// ========================================================== MARKET screen ==

struct MarketV
{
  HdrV hdr;
  String price, block, fee, diff, net;
  int pct;
  String remaining;
};

static void chromeMarket(TFT_eSPI &d)
{
  panel(d, 3, 22, 234, 46, 8, C_PANEL, C_EDGE, C_BG);
  text(d, btcPairLabel().c_str(), 11, 26, 9, C_DIM, C_PANEL);
  text(d, "LATEST BLOCK", 229, 26, 9, C_DIM, C_PANEL, Align::TopRight);
  panel(d, 3, 105, 234, 28, 7, C_PANEL, C_EDGE, C_BG);
}

static void paintMarket(const MarketV &m)
{
  drawHeader(m.hdr);

  uint32_t k = PulseKey().add(m.price).add(accentIdx).value();
  // Starts below the label and is tall enough for the comma tails; the
  // glyph box has ascender space on top, so the text is drawn 3px above it.
  if (need(W_PRICE, k) && wBegin(132, 29, C_PANEL))
  {
    text(wgt, m.price.c_str(), 1, -3, fitSize(m.price.c_str(), 26, 14, 130), C_TEXT, C_PANEL);
    wEnd(10, 37);
    done(W_PRICE, k);
  }

  k = PulseKey().add(m.block).add(accentIdx).value();
  if (need(W_BLOCK, k) && wBegin(86, 26, C_PANEL))
  {
    text(wgt, m.block.c_str(), 85, 3, fitSize(m.block.c_str(), 18, 12, 84), cAcc, C_PANEL, Align::TopRight);
    wEnd(144, 38);
    done(W_BLOCK, k);
  }

  const int y = 72, w = 76, h = 30;
  drawTile(W_M0, 3, y, w, h, "FEE 1/2 H", m.fee.c_str(), 13, cAcc, C_TEXT, C_EDGE);
  drawTile(W_M1, 82, y, w, h, "DIFFICULTY", m.diff.c_str(), 13, cAlt, C_TEXT, C_EDGE);
  drawTile(W_M2, 161, y, w, h, "NETWORK", m.net.c_str(), 13, C_DIM, C_TEXT, C_EDGE);

  const uint32_t kb = PulseKey().add(m.pct).add(m.remaining).add(accentIdx).value();
  if (need(W_HALV, kb) && wBegin(222, 22, C_PANEL))
  {
    text(wgt, "NEXT HALVING", 0, 0, 9, C_DIM, C_PANEL);
    if (m.pct >= 0)
    {
      char pc[8];
      snprintf(pc, sizeof(pc), "%d%%", m.pct);
      text(wgt, pc, textW("NEXT HALVING", 9) + 6, 0, 10, cAcc, C_PANEL);
    }
    String rem = m.remaining.length() ? commas(m.remaining) + " blocks left" : String("--");
    text(wgt, rem.c_str(), 221, 0, 9, C_TEXT, C_PANEL, Align::TopRight);
    wgt.fillSmoothRoundRect(0, 14, 222, 7, 3, C_TRACK, C_PANEL);
    int fw = m.pct > 0 ? (222 * m.pct) / 100 : 0;
    if (fw < 7 && m.pct > 0)
      fw = 7;
    if (fw > 0)
      gradBar(wgt, 0, 14, fw, 7);
    wEnd(9, 109);
    done(W_HALV, kb);
  }
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
  (void)d;
}

static void drawChart()
{
  const uint32_t k = PulseKey().add(histEpoch).add(histCount).add(accentIdx).value();
  if (!need(W_CHART, k))
    return;
  const int W = HIST_N * 4 - 1, H = 12;
  if (!wBegin(W, H, C_BG))
    return;
  float mx = 1;
  for (int i = 0; i < histCount; i++)
    if (hist[i] > mx)
      mx = hist[i];
  mx *= 1.08f;
  for (int i = 0; i < HIST_N; i++)
  {
    int slot = i - (HIST_N - histCount); // right-align so the newest bar is on the right
    int x = i * 4;
    if (slot < 0)
    {
      wgt.fillRect(x, H - 2, 3, 2, C_TRACK); // not sampled yet
      continue;
    }
    int bh = (int)(H * hist[slot] / mx);
    if (bh < 2)
      bh = 2;
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (HIST_N - 1)));
    wgt.fillRect(x, H - bh, 3, bh, c);
  }
  wEnd((SCR_W - W) / 2, 122);
  done(W_CHART, k);
}

static void paintClock(const ClockV &c)
{
  drawHeader(c.hdr);

  // Glyph boxes start well above the digits (ascender space): draw the text
  // above the widget so the digits (44px at size 62) fill local rows 1..45.
  const int DY = -20, SZ = 62, WY = 21, WH = 48;
  uint32_t k = PulseKey().add(c.hh).add(accentIdx).value();
  if (need(W_HH, k) && wBegin(86, WH, C_BG))
  {
    text(wgt, c.hh.c_str(), 85, DY, SZ, C_TEXT, C_BG, Align::TopRight);
    wEnd(26, WY);
    done(W_HH, k);
  }
  k = PulseKey().add(c.mm).add(accentIdx).value();
  if (need(W_MM, k) && wBegin(86, WH, C_BG))
  {
    text(wgt, c.mm.c_str(), 1, DY, SZ, cAcc, C_BG, Align::TopLeft);
    wEnd(128, WY);
    done(W_MM, k);
  }
  k = PulseKey().add(c.colon).add(accentIdx).value();
  if (need(W_COLON, k) && wBegin(16, WH, C_BG))
  {
    uint16_t dot = c.colon ? C_DIM : C_BG;
    wgt.fillSmoothCircle(8, 15, 3, dot, C_BG);
    wgt.fillSmoothCircle(8, 33, 3, dot, C_BG);
    wEnd(112, WY);
    done(W_COLON, k);
  }

  k = PulseKey().add(c.date).value();
  if (need(W_DATE, k) && wBegin(200, 13, C_BG))
  {
    text(wgt, c.date.c_str(), 100, 1, 10, C_DIM, C_BG, Align::TopCenter);
    wEnd(20, 72);
    done(W_DATE, k);
  }

  const int y = 88, w = 76, h = 30;
  drawTile(W_C0, 3, y, w, h, btcPairLabel().c_str(), c.price.c_str(), 13, cAcc, C_TEXT, C_EDGE);
  drawTile(W_C1, 82, y, w, h, "BLOCK", c.block.c_str(), 13, cAlt, C_TEXT, C_EDGE);
  drawTile(W_C2, 161, y, w, h, "KH/s", c.hash.c_str(), 13, C_GOOD, C_TEXT, C_EDGE);

  drawChart();
}

// ============================================================= POOL screen ==

struct PoolV
{
  HdrV hdr;
  String l[6];
  String v[6];
};

static void chromePool(TFT_eSPI &d)
{
  (void)d;
}

static void paintPool(const PoolV &p)
{
  drawHeader(p.hdr);
  static const uint16_t *stripes[6] = {&cAcc, &cAlt, &C_GOOD, &C_DIM, &cAcc, &cAlt};
  for (int i = 0; i < 6; i++)
  {
    int col = i % 2, row = i / 2;
    drawTile(W_P0 + i, 3 + col * 118, 22 + row * 38, 116, 34, p.l[i].c_str(), p.v[i].c_str(), 14,
             *stripes[i], C_TEXT, C_EDGE);
  }
}

// ------------------------------------------------------ boot / setup pages --

static void paintLoading(TFT_eSPI &d)
{
  clearScreen(d);
  // full-circle segmented ring, gradient around it
  const int cx = 120, cy = 42, r = 34, ri = 28, n = 24;
  for (int i = 0; i < n; i++)
  {
    uint16_t c = mix565(cAlt, cAcc, (uint8_t)(i * 255 / (n - 1)));
    int a0 = i * 15;
    d.drawArc(cx, cy, r, ri, a0, a0 + 11, c, C_BG, true);
  }
  text(d, "#", cx, cy - 18, 36, C_TEXT, C_BG, Align::TopCenter);
  int wn = textW("NERD", 22), wm = textW("MINER", 22);
  int x0 = cx - (wn + wm + 2) / 2;
  text(d, "NERD", x0, 80, 22, C_TEXT, C_BG);
  text(d, "MINER", x0 + wn + 2, 80, 22, cAcc, C_BG);
  text(d, CURRENT_VERSION, cx, 105, 10, C_DIM, C_BG, Align::TopCenter);
  text(d, "STARTING UP", cx, 118, 9, C_DIM, C_BG, Align::TopCenter);
}

static void paintSetup(TFT_eSPI &d)
{
  clearScreen(d);
  text(d, "SETUP", 6, 3, 16, C_TEXT, C_BG);
  text(d, "MODE", 6 + textW("SETUP", 16) + 5, 3, 16, cAcc, C_BG);
  text(d, "Join the miner's WiFi", 234, 7, 9, C_DIM, C_BG, Align::TopRight);

  String ap = readCustomAPName();
  if (ap.length() == 0)
    ap = DEFAULT_SSID;
  const char *lab[3] = {"WIFI NETWORK", "PASSWORD", "THEN OPEN IN A BROWSER"};
  const char *val[3] = {ap.c_str(), DEFAULT_WIFIPW, "192.168.4.1"};
  for (int i = 0; i < 3; i++)
  {
    int y = 24 + i * 37;
    panel(d, 3, y, 234, 34, 7, C_PANEL, C_EDGE, C_BG);
    uint16_t disc = mix565(C_PANEL, cAcc, 60);
    d.fillSmoothCircle(19, y + 17, 9, disc, C_PANEL);
    char n[2] = {(char)('1' + i), 0};
    text(d, n, 19, y + 10, 12, cAcc, disc, Align::TopCenter);
    text(d, lab[i], 36, y + 4, 9, C_DIM, C_PANEL);
    text(d, val[i], 36, y + 15, fitSize(val[i], 14, 9, 194), C_TEXT, C_PANEL);
  }
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
  case SCR_CLOCK:
    chromeClock(d);
    break;
  default:
    chromePool(d);
    break;
  }
}

static void ensureChrome(int scr)
{
  if (uiScreen == scr)
    return;
  uiScreen = scr;
  resetWidgets();
  paintChrome(out(), scr);
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
static PoolV lastPool;
static bool haveMine = false, haveMarket = false, haveClock = false, havePool = false;

static void repaintCached(int scr)
{
  if (scr == SCR_MINE && haveMine)
    paintMine(lastMine);
  else if (scr == SCR_MARKET && haveMarket)
    paintMarket(lastMarket);
  else if (scr == SCR_CLOCK && haveClock)
    paintClock(lastClock);
  else if (scr == SCR_POOL && havePool)
    paintPool(lastPool);
}

// ------------------------------------------------------- saved UI choices --
// Accent colour and last-viewed page live in their own NVS namespace (the
// pool/wallet/brightness/flip settings are in the main config file).

#define UI_PREFS_NS "tdisp_ui"

static int savedAccent = 0;
static int savedPage = 0;
static int restorePage = 0; // applied on the first data tick (the monitor resets the page to 0 at start)

static void loadUiPrefs()
{
  Preferences p;
  if (!p.begin(UI_PREFS_NS, true))
    return; // namespace not created yet: defaults
  accentIdx = p.getUChar("accent", 0) % ACCENT_COUNT;
  int page = p.getUChar("page", 0);
  p.end();
  cAcc = PULSE_ACCENTS[accentIdx].main;
  cAlt = PULSE_ACCENTS[accentIdx].alt;
  savedAccent = accentIdx;
  savedPage = restorePage = page;
}

static void saveUiPrefs()
{
  int page = currentDisplayDriver->current_cyclic_screen;
  if (page == savedPage && accentIdx == savedAccent)
    return;
  Preferences p;
  if (p.begin(UI_PREFS_NS, false))
  {
    p.putUChar("accent", accentIdx);
    p.putUChar("page", page);
    p.end();
  }
  savedAccent = accentIdx;
  savedPage = page;
}

// Right button double click (NerdMinerV2.ino.cpp): next accent colour. Only
// records the request; the monitor task repaints in DoLedStuff.
static volatile bool accentRequested = false;
void tDisplayPulseNextAccent(void)
{
  if (wakeScreen())
    return; // dark: the press only wakes the screen
  accentRequested = true;
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
  h.bars = wifiBars();
  h.status = uiStatus();
#if defined(M5STICK_C_PLUS2) && defined(PIN_BATTERY_ADC)
  const int mv = plus2BatteryCachedMillivolts();
  if (mv > 0)
    h.batt = plus2BatteryPercent(mv);
#endif
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
  TFT_eSPI &d = out();
  const String host = Settings.Hostname[0] ? String(Settings.Hostname) : String("nerdminer");
  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("No WiFi");

  const uint32_t staticKey = PulseKey().add(host).add(ip).add(accentIdx).value();
  if (!s_showingIdentify || s_identifyStaticKey != staticKey)
  {
    s_showingIdentify = true;
    s_identifyStaticKey = staticKey;
    s_lastIdentifySec = -1;
    uiScreen = -1;

    clearScreen(d);
    panel(d, 4, 4, SCR_W - 8, SCR_H - 8, 8, C_BG, cAcc, C_BG);

    const int bw = textW("LOCATE MINER", 9) + 16;
    const uint16_t badgeBg = mix565(C_BG, cAcc, 56);
    d.fillSmoothRoundRect((SCR_W - bw) / 2, 10, bw, 16, 8, badgeBg, C_BG);
    text(d, "LOCATE MINER", SCR_W / 2, 13, 9, cAcc, badgeBg, Align::TopCenter);

    panel(d, 10, 30, SCR_W - 20, 44, 6, C_PANEL, C_EDGE, C_BG);
    unsigned hsz = 28;
    while (hsz > 14 && textW(host.c_str(), hsz) > SCR_W - 32)
      hsz -= 2;
    const int hy = 30 + (44 - (int)hsz) / 2 - 2;
    text(d, host.c_str(), SCR_W / 2, hy, hsz, cAcc, C_PANEL, Align::TopCenter);

    panel(d, 10, 78, SCR_W - 20, 30, 6, C_PANEL, C_EDGE, C_BG);
    text(d, ip.c_str(), SCR_W / 2, 85, 13, C_TEXT, C_PANEL, Align::TopCenter);
  }

  const int secs = (int)((identifyRemainingMs() + 999U) / 1000U);
  if (secs != s_lastIdentifySec)
  {
    s_lastIdentifySec = secs;
    char cd[32];
    snprintf(cd, sizeof(cd), "RETURNING IN %ds", secs);
    if (wBegin(160, 16, C_BG))
    {
      text(wgt, cd, 80, 2, 10, C_TEXT, C_BG, Align::TopCenter);
      wEnd((SCR_W - 160) / 2, 112);
    }
  }
}

void tDisplayPulse_MinerScreen(unsigned long mElapsed)
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
  uiLive = true;
  ensureChrome(SCR_MINE);

  float kh = data.currentHashRate.toFloat();
  noteHashrate(kh);
  gaugeTarget = constrain(kh / G_MAX_KH, 0.0f, 1.0f);

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
  m.flash = isShareFlashing();
  lastMine = m;
  haveMine = true;
  paintMine(m);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

void tDisplayPulse_MarketScreen(unsigned long mElapsed)
{
  if (isIdentifying())
  {
    uiLive = true;
    paintIdentifyScreen();
    return;
  }
  s_showingIdentify = false;
  coin_data data = getCoinData(mElapsed);
  uiLive = true;
  ensureChrome(SCR_MARKET);
  noteHashrate(data.currentHashRate.toFloat());

  MarketV m;
  m.hdr = liveHeader(data.currentTime);
  m.price = fmtPrice(data.btcPrice);
  m.block = fmtBlock(data.blockHeight);
  bool known = m.block != "--";
  m.fee = data.halfHourFee.startsWith("0 ") || data.halfHourFee.length() == 0 ? String("--") : data.halfHourFee;
  m.diff = data.netwrokDifficulty.length() ? data.netwrokDifficulty : String("--");
  m.net = data.globalHashRate.length() ? data.globalHashRate + " EH/s" : String("--");
  m.pct = known ? (int)data.progressPercent : -1;
  m.remaining = known ? String(data.remainingBlocks.toInt()) : String();
  lastMarket = m;
  haveMarket = true;
  paintMarket(m);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

void tDisplayPulse_ClockScreen(unsigned long mElapsed)
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
  c.block = fmtBlock(data.blockHeight);
  c.hash = data.currentHashRate;
  lastClock = c;
  haveClock = true;
  paintClock(c);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

void tDisplayPulse_PoolScreen(unsigned long mElapsed)
{
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
  ensureChrome(SCR_POOL);
  noteHashrate(data.currentHashRate.toFloat());

  PoolV p;
  p.hdr = liveHeader(data.currentTime);
  p.l[0] = "WORKERS";
  p.v[0] = String(pData.workersCount);
  p.l[1] = "POOL RATE";
  p.v[1] = pData.workersHash;
  p.l[2] = "POOL BEST";
  p.v[2] = pData.bestDifficulty;
  p.l[3] = "UPTIME";
  p.v[3] = fmtUptime(data.timeMining);
  p.l[4] = "POOL";
  p.v[4] = Settings.PoolAddress;
  p.l[5] = "SETTINGS PAGE";
  p.v[5] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("--");
  lastPool = p;
  havePool = true;
  paintPool(p);

  logShare(data.completedShares, data.totalKHashes, data.currentHashRate);
}

// --------------------------------------------------------- driver plumbing --

#define ROT_NORMAL ROTATION_90   // USB on the right
#define ROT_FLIPPED ROTATION_270 // USB on the left

void tDisplayPulse_Init(void)
{
  tft.init();
  if (nvMem.loadConfig(&Settings))
  {
    // brightness / flip only; the T-Display needs its default colour inversion
  }
  tft.setRotation(Settings.flipDisplay ? ROT_FLIPPED : ROT_NORMAL);
  tft.setSwapBytes(true);
  tft.fillScreen(C_BG);

  // Backlight: LEDC channel 0, 5kHz, 8 bit (0-255 duty). The duty has to be
  // written after attaching (Arduino-ESP32 2.0.x re-applies the channel's
  // read-back duty on attach).
#ifdef M5STICK_C_PLUS2
  ledcSetup(0, PLUS2_BACKLIGHT_FREQ, 8); // 256 Hz as in M5GFX: at 5 kHz the Plus2 backlight stays dark
#else
  ledcSetup(0, 5000, 8);
#endif
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, Settings.Brightness);

  loadUiPrefs();

  if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold)))
  {
    Serial.println("Initialise error");
    return;
  }

  pData.bestDifficulty = "0";
  pData.workersHash = "0";
  pData.workersCount = 0;

  resetWidgets();
  memset(segLvl, 0xFF, sizeof(segLvl));

#ifdef TDISPLAY_UI_SCREENSHOT
  extern void tDisplayDumpScreens(void);
  tDisplayDumpScreens();
#endif
}

void tDisplayPulse_AlternateScreenState(void)
{
  Serial.println("Switching display state");
  if (ledcRead(0) > 0)
    ledcWrite(0, 0);
  else
    ledcWrite(0, backlightLevel()); // day, night or in between
}

// Save the orientation into the main config file, same field the LAN page
// uses. Load a fresh copy first so only this one value changes.
static void persistFlip(bool flipped)
{
  Settings.flipDisplay = flipped;
  TSettings stored;
  if (nvMem.loadConfig(&stored))
  {
    stored.flipDisplay = flipped;
    nvMem.saveConfig(&stored);
  }
}

// Left/front button double click: only records the request, the monitor task
// flips in DoLedStuff. Buttons are read in loop() on the other core; drawing
// from there while the monitor task is mid-frame breaks TFT_eSPI's SPI bus
// lock (assert in xQueueGenericSend, then a reboot).
static volatile bool rotateRequested = false;
void tDisplayPulse_AlternateRotation(void)
{
  rotateRequested = true;
}

static void applyRotation(void)
{
  const bool flipped = tft.getRotation() != ROT_FLIPPED;
  tft.setRotation(flipped ? ROT_FLIPPED : ROT_NORMAL);
  persistFlip(flipped);
  if (uiLive)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    uiScreen = -1;
    ensureChrome(s);
    repaintCached(s);
  }
}

void tDisplayPulse_LoadingScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  paintLoading(out());
}

void tDisplayPulse_SetupScreen(void)
{
  uiLive = false;
  uiScreen = -1;
  paintSetup(out());
}

#ifdef M5STICK_C_PLUS2
// Shown by plus2BatteryGuardTick() (m5stickPlus2Hw.cpp) before it cuts the power
static void showLowBattery()
{
  uiLive = false;
  uiScreen = -1;
  TFT_eSPI &d = out();
  clearScreen(d);
  text(d, "LOW BATTERY", SCR_W / 2, 38, 22, C_BAD, C_BG, Align::TopCenter);
  text(d, "Powering off, please charge", SCR_W / 2, 76, 11, C_DIM, C_BG, Align::TopCenter);
}
#endif

void tDisplayPulse_AnimateCurrentScreen(unsigned long frame)
{
#ifdef M5STICK_C_PLUS2
#ifdef PIN_BATTERY_ADC
  plus2BatteryMillivolts(); // keeps the reading fresh (every 5 s) for the header and the web monitor
#endif
  plus2BatteryGuardTick(showLowBattery);
#endif
  if (isIdentifying() || !uiLive || uiScreen < 0)
    return;
  drawPulse(frame);
  if (uiScreen == SCR_MINE && (fabsf(gaugeTarget - gaugeShown) > 0.002f || isShareFlashing() != s_lastRingFlash))
  {
    s_lastRingFlash = isShareFlashing();
    gaugeShown += (gaugeTarget - gaugeShown) * 0.3f; // ease toward the measured rate
    gaugeDraw(out(), gaugeShown);
  }
}

void tDisplayPulse_DoLedStuff(unsigned long frame)
{
#ifdef M5STICK_C_PLUS2
  plus2SignalTick(); // LED / buzzer for shares and blocks
#endif
  if (accentRequested)
  {
    accentRequested = false;
    noteScreenActivity();
    setAccent(accentIdx + 1);
  }
  if (rotateRequested)
  {
    rotateRequested = false;
    applyRotation();
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

  // Screen changed by a button: draw the new page right away from the last
  // values it showed; the next data tick refreshes them.
  if (uiLive && currentDisplayDriver->current_cyclic_screen != uiScreen)
  {
    int s = currentDisplayDriver->current_cyclic_screen;
    ensureChrome(s);
    repaintCached(s);
  }
  if (uiLive)
    saveUiPrefs();
}

// ----------------------------------------------------- dev: screenshots --

#ifdef TDISPLAY_UI_SCREENSHOT
// Each screen is kept run-length encoded ((count, RGB332 value) pairs): a
// Pulse screen is mostly flat colour, so all of them fit in a few KB.
#define SHOT_MAX 8
static const char *shotName[SHOT_MAX];
static uint8_t *shotData[SHOT_MAX];
static size_t shotLen[SHOT_MAX];
static int shotCount = 0;

static void keepShot(const char *name)
{
  if (shotCount >= SHOT_MAX)
    return;
  const uint8_t *px = (const uint8_t *)shot.getPointer();
  const int n = SCR_W * SCR_H;
  size_t len = 0;
  for (int i = 0; i < n;)
  {
    int r = 1;
    while (i + r < n && r < 255 && px[i + r] == px[i])
      r++;
    len += 2;
    i += r;
  }
  uint8_t *buf = (uint8_t *)malloc(len);
  if (!buf)
  {
    Serial.printf("[ui] shot %s: no memory for %u bytes\n", name, (unsigned)len);
    return;
  }
  size_t o = 0;
  for (int i = 0; i < n;)
  {
    int r = 1;
    while (i + r < n && r < 255 && px[i + r] == px[i])
      r++;
    buf[o++] = r;
    buf[o++] = px[i];
    i += r;
  }
  shotName[shotCount] = name;
  shotData[shotCount] = buf;
  shotLen[shotCount] = len;
  shotCount++;
  Serial.printf("[ui] shot %s kept (%u bytes)\n", name, (unsigned)len);
}

// For the web server (webConfig.cpp): shot i, or false past the last one.
bool tDisplayShot(int i, const char **name, const uint8_t **data, size_t *len)
{
  if (i < 0 || i >= shotCount)
    return false;
  *name = shotName[i];
  *data = shotData[i];
  *len = shotLen[i];
  return true;
}

void tDisplayDumpScreens(void)
{
  shot.setColorDepth(8);
  shot.createSprite(SCR_W, SCR_H);
  if (!shot.created())
  {
    Serial.println("[ui] shot sprite alloc failed");
    return;
  }
  shotOn = true;

  HdrV h;
  h.time = "21:47";
  h.bars = 3;
  h.status = NM_hashing;

  for (int i = 0; i < HIST_N; i++)
    hist[i] = 400 + 25 * sinf(i * 0.45f) + (i * 7 % 13);
  histCount = HIST_N;
  histEpoch = 1;

  paintLoading(shot);
  keepShot("loading");
  paintSetup(shot);
  keepShot("setup");

  uiLive = true;
  gaugeShown = gaugeTarget = 412.5f / G_MAX_KH;
  uiScreen = -1;
  ensureChrome(SCR_MINE);
  MineV m;
  m.hdr = h;
  m.hash = "412.5";
  m.shares = "1,284";
  m.best = "8.73M";
  m.tmpl = "1,758";
  m.blocks = "0";
  m.total = "58213";
  m.flash = false;
  paintMine(m);
  drawPulse(5);
  keepShot("mine");

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
  paintMarket(k);
  drawPulse(5);
  keepShot("market");

  uiScreen = -1;
  ensureChrome(SCR_CLOCK);
  ClockV c;
  c.hdr = h;
  c.hh = "21";
  c.mm = "47";
  c.colon = true;
  c.date = fmtDate("21/10/2026");
  c.price = "$97,412";
  c.block = "912,345";
  c.hash = "412.5";
  paintClock(c);
  drawPulse(5);
  keepShot("clock");

  uiScreen = -1;
  ensureChrome(SCR_POOL);
  PoolV p;
  p.hdr = h;
  p.l[0] = "WORKERS";
  p.v[0] = "3";
  p.l[1] = "POOL RATE";
  p.v[1] = "1.92M";
  p.l[2] = "POOL BEST";
  p.v[2] = "412.6M";
  p.l[3] = "UPTIME";
  p.v[3] = "3d 07h";
  p.l[4] = "POOL";
  p.v[4] = "btc.heliospool.com";
  p.l[5] = "SETTINGS PAGE";
  p.v[5] = "192.168.0.222";
  paintPool(p);
  drawPulse(5);
  keepShot("pool");

  setAccent(2); // amber / rose: the mining screen again
  uiScreen = -1;
  ensureChrome(SCR_MINE);
  paintMine(m);
  drawPulse(5);
  keepShot("mine_alt");
  setAccent(0);

  shotOn = false;
  shot.deleteSprite();
  uiLive = false;
  uiScreen = -1;
  resetWidgets();
}
#endif

CyclicScreenFunction tDisplayPulseCyclicScreens[] = {tDisplayPulse_MinerScreen, tDisplayPulse_MarketScreen,
                                                     tDisplayPulse_ClockScreen, tDisplayPulse_PoolScreen};

DisplayDriver tDisplayPulseDriver = {
    tDisplayPulse_Init,
    tDisplayPulse_AlternateScreenState,
    tDisplayPulse_AlternateRotation,
    tDisplayPulse_LoadingScreen,
    tDisplayPulse_SetupScreen,
    tDisplayPulseCyclicScreens,
    tDisplayPulse_AnimateCurrentScreen,
    tDisplayPulse_DoLedStuff,
    SCREENS_ARRAY_SIZE(tDisplayPulseCyclicScreens),
    0,
    SCR_W,
    SCR_H};
#endif
