// "PULSE" theme for the 4.0" 320x480 ST7796 "CYD" (classic ESP32, 480x320
// landscape).
//
// This board has no PSRAM, so there is no frame buffer: as on the 2.8" CYD
// (esp23_2432s028r.cpp) the chrome is drawn straight to the panel and every
// widget goes through a small scratch sprite.
#include "displayDriver.h"
#include "display.h"

#ifdef ESP32_4IN_ST7796_DISPLAY

#include <TFT_eSPI.h>
#include <SPI.h>
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "pulseUi.h"
#include "pulse480.h"

extern nvMemory nvMem;
extern TSettings Settings;

static TFT_eSPI tft = TFT_eSPI(); // pins defined in platformio.ini

#define SCR_W 480
#define SCR_H 320
#define G_MAX_KH 1200.0f // full-scale of the ring: 40 KH/s per segment (this board runs ~950)

// Landscape with the USB socket on the left, or (flipped) on the right
#define ROT_NORMAL 3
#define ROT_FLIPPED 1

// ================================================================== touch ==
// XPT2046 on the panel's SPI bus. Reads go through the SPI object TFT_eSPI
// uses: its transaction lock keeps them apart from the panel writes made on
// the monitor task.

#define TOUCH_SPI_HZ 2500000
#define TOUCH_Z_MIN 300 // a pressure reading below this is not a touch

// Raw readings at the edges of the panel, measured on hardware with the USB
// socket on the left: the controller's Y channel runs left to right along the
// long side, its X channel top to bottom along the short side.
#define TOUCH_LONG_MIN 350
#define TOUCH_LONG_MAX 3870
#define TOUCH_SHORT_MIN 260
#define TOUCH_SHORT_MAX 3740

static int touchRawL = 0, touchRawS = 0; // last point as the controller reported it

static int touchSample(SPIClass &spi, uint8_t cmd)
{
  spi.transfer(cmd);
  return spi.transfer16(0) >> 3;
}

// Touch point in screen (landscape) coordinates.
static bool touchGetXY(int &x, int &y)
{
  if (digitalRead(TOUCH_PIN_IRQ) == HIGH)
    return false;

  SPIClass &spi = TFT_eSPI::getSPIinstance();
  spi.beginTransaction(SPISettings(TOUCH_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_PIN_CS, LOW);
  const int z = 4095 + touchSample(spi, 0xB0) - touchSample(spi, 0xC0);
  int l = 0, s = 0;
  if (z >= TOUCH_Z_MIN)
  {
    touchSample(spi, 0xD0); // the first conversion after the pressure ones has not settled
    for (int i = 0; i < 4; i++)
    {
      s += touchSample(spi, 0xD0);
      l += touchSample(spi, 0x90);
    }
  }
  digitalWrite(TOUCH_PIN_CS, HIGH);
  spi.endTransaction();
  if (z < TOUCH_Z_MIN)
    return false;

  touchRawL = l / 4;
  touchRawS = s / 4;
  x = constrain(map(touchRawL, TOUCH_LONG_MIN, TOUCH_LONG_MAX, 0, SCR_W - 1), 0, SCR_W - 1);
  y = constrain(map(touchRawS, TOUCH_SHORT_MIN, TOUCH_SHORT_MAX, 0, SCR_H - 1), 0, SCR_H - 1);
  if (tft.getRotation() == ROT_FLIPPED)
  {
    x = SCR_W - 1 - x;
    y = SCR_H - 1 - y;
  }
  return true;
}

static void touchTask(void *)
{
  bool wasDown = false;
  int downCount = 0;
  for (;;)
  {
    int t_x, t_y;
    bool down = touchGetXY(t_x, t_y);
    if (down)
    {
      if (++downCount == 2 && !wasDown) // two consecutive samples: real press, act once per touch
      {
        wasDown = true;
        pulse480PostTouchAction(pulse480ClassifyTouch(t_x, t_y, ledcRead(0) == 0));
        Serial.printf("[touch] x=%d y=%d (raw %d,%d)\n", t_x, t_y, touchRawL, touchRawS);
      }
    }
    else
    {
      downCount = 0;
      wasDown = false;
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

static void flipHardware()
{
  tft.setRotation(tft.getRotation() == ROT_FLIPPED ? ROT_NORMAL : ROT_FLIPPED);
  pulsePersistFlip(nvMem, tft.getRotation() == ROT_FLIPPED);
}

// --------------------------------------------------------- driver plumbing --

void esp32_4in_Init(void)
{
  // Backlight off until the first frame is on the panel
  ledcSetup(0, 5000, 8); // LEDC channel 0, 5kHz, 8 bit (0-255 duty)
  ledcAttachPin(LCD_PIN_BL, 0);
  ledcWrite(0, 0);

  // The touch controller is on the panel's bus: deselect it before the panel
  // is set up
  pinMode(TOUCH_PIN_CS, OUTPUT);
  digitalWrite(TOUCH_PIN_CS, HIGH);
  pinMode(TOUCH_PIN_IRQ, INPUT);

  nvMem.loadConfig(&Settings);

  tft.init();
  tft.invertDisplay(Settings.invertColors);
  tft.setRotation(Settings.flipDisplay ? ROT_FLIPPED : ROT_NORMAL);
  tft.fillScreen(C_BG);
  ledcWrite(0, Settings.Brightness);

  xTaskCreatePinnedToCore(touchTask, "touch", 3072, NULL, 1, NULL, 0);

  Pulse480Backend backend = {};
  backend.tft = &tft;
  backend.canvas = &tft;
  backend.fb = nullptr;
  backend.sprite8BitFallback = true;
  backend.gaugeMaxKh = G_MAX_KH;
  backend.redrawOnFlip = true;
  backend.flushRect = nullptr;
  backend.flushAll = nullptr;
  backend.flipHardware = flipHardware;
  pulse480Init(backend);
}

CyclicScreenFunction esp32_4inCyclicScreens[] = {pulse480MinerScreen, pulse480MarketScreen, pulse480ClockScreen};

DisplayDriver esp32_4inDriver = {
    esp32_4in_Init,
    pulse480AlternateScreenState,
    pulse480AlternateRotation,
    pulse480LoadingScreen,
    pulse480SetupScreen,
    esp32_4inCyclicScreens,
    pulse480AnimateCurrentScreen,
    pulse480DoLedStuff,
    SCREENS_ARRAY_SIZE(esp32_4inCyclicScreens),
    0,
    SCR_W,
    SCR_H};
#endif
