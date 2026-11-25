#ifndef PULSE480_H
#define PULSE480_H

#include "displayDriver.h"
#include "display.h"

#if defined(ES3C35P_DISPLAY) || defined(ESP32_4IN_ST7796_DISPLAY)

class TFT_eSPI;
class TFT_eSprite;

enum Pulse480TouchAction
{
  PULSE480_TA_NONE = 0,
  PULSE480_TA_NEXT,
  PULSE480_TA_PREV,
  PULSE480_TA_BACKLIGHT,
  PULSE480_TA_ACCENT
};

struct Pulse480Backend
{
  TFT_eSPI *tft;          // TFT_eSPI instance (sprite parent & font drawer)
  TFT_eSPI *canvas;       // Full-screen target (&fb on ES3C35P, &tft on 4" ST7796)
  TFT_eSprite *fb;        // Non-null when widgets push to fb and flushRect; null -> pushSprite
  bool sprite8BitFallback;
  float gaugeMaxKh;
  bool redrawOnFlip;
  void (*flushRect)(int x, int y, int w, int h);
  void (*flushAll)(void);
  void (*flipHardware)(void);
};

// Lifecycle & screen handlers
bool pulse480Init(const Pulse480Backend &backend);
void pulse480AlternateScreenState(void);
void pulse480AlternateRotation(void);
void pulse480LoadingScreen(void);
void pulse480SetupScreen(void);
void pulse480MinerScreen(unsigned long mElapsed);
void pulse480MarketScreen(unsigned long mElapsed);
void pulse480ClockScreen(unsigned long mElapsed);
void pulse480AnimateCurrentScreen(unsigned long frame);
void pulse480DoLedStuff(unsigned long frame);

// Touch & screenshot hooks
void pulse480PostTouchAction(int action);
int pulse480ClassifyTouch(int x, int y, bool backlightOff);

#ifdef ES3C35P_UI_SCREENSHOT
void pulse480RunScreenshotDump(void (*dumpShot)(const char *name));
#endif

#endif // ES3C35P_DISPLAY || ESP32_4IN_ST7796_DISPLAY
#endif // PULSE480_H
