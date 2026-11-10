#include "../devices/device.h"

#ifdef HAS_RGB_LED

#include <Arduino.h>
#include "rgbLed.h"
#include "display.h"
#include "monitor.h"
#include "../storage/storage.h"

extern TSettings Settings;
extern monitor_data mMonitor;

// Common anode: each colour pin is active low. Driven by PWM so any colour
// and brightness works. LEDC channels 2-4 (timers 1 and 2); the backlight
// has channel 0 on timer 0, and ledcSetup on channel 1 would retime it.
static const uint8_t kPins[3] = {LED_PIN, LED_PIN_G, LED_PIN_B};
static const uint8_t kFirstChannel = 2;

static uint32_t s_shown = 0xFFFFFFFF; // duties on the pins now, packed; none yet
static uint32_t s_lastBlink = 0;
static bool s_blinkOn = false;

// Rough gamma (square) so the brightness setting and mixed colours look
// even; any lit component stays at least 1/255.
static uint32_t level(uint32_t component, uint32_t brightness)
{
  const uint32_t v = component * brightness / 255;
  return (v * v + 254) / 255;
}

static uint32_t lerpRgb(uint32_t a, uint32_t b, uint32_t t255)
{
  const int32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
  const int32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
  const uint32_t r = (uint32_t)(ar + ((br - ar) * (int32_t)t255) / 255);
  const uint32_t g = (uint32_t)(ag + ((bg - ag) * (int32_t)t255) / 255);
  const uint32_t bl = (uint32_t)(ab + ((bb - ab) * (int32_t)t255) / 255);
  return (r << 16) | (g << 8) | bl;
}

static void show(uint32_t rgb, uint32_t b)
{
  const uint32_t r = level((rgb >> 16) & 0xFF, b);
  const uint32_t g = level((rgb >> 8) & 0xFF, b);
  const uint32_t bl = level(rgb & 0xFF, b);
  const uint32_t packed = (r << 16) | (g << 8) | bl;
  if (packed == s_shown)
    return; // only touch the peripheral when something changes
  s_shown = packed;
  ledcWrite(kFirstChannel + 0, r ? (255 - r) : 256); // 256 = 100% high = completely off
  ledcWrite(kFirstChannel + 1, g ? (255 - g) : 256);
  ledcWrite(kFirstChannel + 2, bl ? (255 - bl) : 256);
}

void rgbLedInit(void)
{
  // Off is written after attaching: ledcAttachPin takes the duty it reads back
  // from a channel that has not run yet (0 = pin low = all three lit), so a
  // write before it is lost.
  for (int i = 0; i < 3; i++)
  {
    ledcSetup(kFirstChannel + i, 5000, 8);
    ledcAttachPin(kPins[i], kFirstChannel + i);
    ledcWrite(kFirstChannel + i, 256);
  }
  s_shown = 0xFFFFFFFF; // first rgbLedTick writes whatever it wants, off included
}

void rgbLedTick(void)
{
  // Priority 1: Physical device locator ("Identify Miner") — bright Cyan/White
  // strobe at full brightness regardless of LED mode, brightness, or screen state.
  if (isIdentifying())
  {
    const uint32_t rgb = ((millis() / 125) & 1) ? 0xFFFFFF : 0x00FFFF;
    show(rgb, 255);
    return;
  }

  // Priority 2: Share celebration — smooth Gold -> Emerald -> Cyan -> Magenta
  // cycle every 120 ms per transition at full brightness.
  if (isCelebrating())
  {
    static const uint32_t kCeleb[4] = {0xFFD700, 0x00FF88, 0x00E5FF, 0xFF2AD4};
    const uint32_t now = millis();
    const uint32_t idx = (now / 120) & 3;
    const uint32_t frac = ((now % 120) * 255) / 120;
    show(lerpRgb(kCeleb[idx], kCeleb[(idx + 1) & 3], frac), 255);
    return;
  }

  uint32_t rgb = 0;
  switch (Settings.ledMode)
  {
  case LED_MODE_OFF:
    break;

  case LED_MODE_STEADY:
    rgb = Settings.ledColor;
    break;

  default: // LED_MODE_STATUS
    switch (mMonitor.NerdStatus)
    {
    case NM_waitingConfig:
      rgb = 0xFF0000; // red, steady: starting up or setup portal
      break;
    case NM_Connecting:
      rgb = 0x0000FF; // blue: connecting
      break;
    case NM_hashing:
      if (millis() - s_lastBlink >= 500)
      {
        s_lastBlink = millis();
        s_blinkOn = !s_blinkOn;
      }
      rgb = s_blinkOn ? Settings.ledColor : 0;
      break;
    }
    break;
  }

  if (Settings.ledScreenOff && ledcRead(0) == 0)
    rgb = 0; // screen switched or timed out off (backlight is LEDC channel 0)
  show(rgb, constrain(Settings.ledBrightness, 1, 255));
}

#endif // HAS_RGB_LED
