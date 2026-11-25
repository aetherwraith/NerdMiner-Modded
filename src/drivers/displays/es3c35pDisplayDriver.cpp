// "PULSE" theme for the LCDWiki ES3C35P (ESP32-S3, 3.5" 480x320 landscape).
//
// The panel is an ST77922 on quad SPI, which TFT_eSPI cannot drive. TFT_eSPI
// is used here only as a drawing engine: everything is drawn into a 480x320
// sprite in PSRAM (the frame buffer) and the changed rectangle is then sent
// to the panel by the small quad SPI driver below. tft.init() is never called.
//
// Dev aid: -D ES3C35P_UI_SCREENSHOT=1 renders every screen once at boot and
// dumps the frame buffer over serial (see SHOT_BEGIN/SHOT_END).
#include "displayDriver.h"
#include "display.h"

#ifdef ES3C35P_DISPLAY

#include <TFT_eSPI.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <driver/periph_ctrl.h>
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "pulseUi.h"
#include "pulse480.h"

extern nvMemory nvMem;
extern TSettings Settings;

static TFT_eSPI tft = TFT_eSPI();          // drawing engine only, never initialised
static TFT_eSprite fb = TFT_eSprite(&tft); // the frame buffer

#define SCR_W 480
#define SCR_H 320
#define G_MAX_KH 480.0f // full-scale of the ring: 16 KH/s per segment (this board runs ~370)

// ================================================================== panel ==

#define LCD_NATIVE_W 320 // the panel is portrait; landscape is done in panelFlush
#define LCD_NATIVE_H 480
#define LCD_SPI_HZ 40000000 // the controller's write clock limit is 62.5 MHz
#define LCD_TX_BYTES 30720  // 48 full native rows per transfer

// Init sequence of this panel, as {command, parameter count, parameters...}.
// Decoded from the board's factory firmware; LCDWiki publish the same table
// in their Arduino examples.
static const uint8_t LCD_INIT[] = {
    0xF1, 1, 0x00,
    0x60, 3, 0x00, 0x00, 0x00,
    0x65, 1, 0x80,
    0x79, 1, 0x06,
    0x7B, 3, 0x00, 0x08, 0x08,
    0x80, 11, 0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA,
    0x81, 4, 0x26, 0x52, 0x72, 0x27,
    0x84, 2, 0x92, 0x25,
    0x87, 6, 0x10, 0x10, 0x58, 0x00, 0x02, 0x3A,
    0x88, 15, 0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06,
    0x89, 3, 0x00, 0x00, 0x00,
    0x8A, 11, 0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19,
    0x8B, 9, 0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E,
    0x8C, 13, 0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09,
    0x8D, 1, 0x0C,
    0x8E, 6, 0x33, 0x01, 0x0C, 0x13, 0x01, 0x01,
    0xB3, 2, 0x00, 0x30,
    0xF1, 1, 0x00,
    0x71, 1, 0xD0,
    0x66, 2, 0x02, 0x3F,
    0xBE, 3, 0x26, 0x00, 0x9D,
    0x70, 12, 0x01, 0xA6, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x60, 0x11, 0x00, 0x00, 0x1A,
    0x90, 9, 0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x2D, 0x2D,
    0x91, 9, 0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x2D, 0x2D,
    0x92, 10, 0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x2D,
    0x93, 10, 0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x2D,
    0x94, 6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x95, 5, 0x96, 0x16, 0x00, 0x00, 0xFF,
    0x96, 12, 0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x9A, 0x2D, 0x00, 0x44,
    0x97, 12, 0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x9A, 0x2D, 0x00, 0x44,
    0xBA, 5, 0x55, 0x9A, 0x2D, 0x9A, 0x2D,
    0x9A, 7, 0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x9B, 7, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x9C, 13, 0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00,
    0x9D, 8, 0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0x9E, 7, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0xB4, 12, 0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10,
    0xB5, 12, 0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12,
    0xB6, 7, 0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF,
    0x86, 14, 0xC6, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5,
    0xB7, 16, 0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D,
    0xB8, 16, 0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D,
    0xB9, 2, 0x23, 0x23,
    0xBF, 6, 0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B,
    0xF2, 1, 0x00,
    0x73, 5, 0x04, 0xDA, 0x12, 0x54, 0x47,
    0x77, 5, 0x6B, 0x5B, 0xFD, 0xC3, 0xC5,
    0x7A, 2, 0x15, 0x27,
    0x7B, 2, 0x04, 0x57,
    0x7E, 2, 0x01, 0x0E,
    0xBF, 1, 0x36,
    0xE3, 2, 0x40, 0x40,
    0xF0, 1, 0x00,
    0xD0, 1, 0x00,
    0x2A, 4, 0x00, 0x00, 0x01, 0x3F,
    0x2B, 4, 0x00, 0x00, 0x01, 0xDF,
    0x21, 0,       // inversion on (this panel needs it for true colours)
    0x11, 0,       // sleep out, then wait 120 ms
    0x29, 0,       // display on
    0x2C, 0,
    0x3A, 1, 0x01, // RGB565
    0x36, 1, 0x00, // native portrait, RGB order
    0x35, 1, 0x01, // tearing effect line on, then wait 20 ms
};

static spi_device_handle_t lcdSpi = NULL;
static uint8_t *lcdTx = NULL; // DMA-capable transfer buffer
static bool lcdFlipped = false;

// Register write: opcode 0x02, the command in a 24-bit address, single line.
static void lcdCmd(uint8_t cmd, const uint8_t *data, size_t len)
{
  spi_transaction_t t = {};
  t.cmd = 0x02;
  t.addr = (uint32_t)cmd << 8;
  if (len)
  {
    memcpy(lcdTx, data, len);
    t.tx_buffer = lcdTx;
    t.length = len * 8;
  }
  spi_device_polling_transmit(lcdSpi, &t);
}

static bool panelInit()
{
  spi_bus_config_t bus = {};
  bus.mosi_io_num = LCD_PIN_D0;
  bus.miso_io_num = LCD_PIN_D1;
  bus.quadwp_io_num = LCD_PIN_D2;
  bus.quadhd_io_num = LCD_PIN_D3;
  bus.sclk_io_num = LCD_PIN_SCK;
  bus.max_transfer_sz = LCD_TX_BYTES;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
    return false;

  spi_device_interface_config_t dev = {};
  dev.command_bits = 8;
  dev.address_bits = 24;
  dev.mode = 0;
  dev.clock_speed_hz = LCD_SPI_HZ;
  dev.spics_io_num = LCD_PIN_CS;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  dev.queue_size = 1;
  if (spi_bus_add_device(SPI2_HOST, &dev, &lcdSpi) != ESP_OK)
    return false;

  lcdTx = (uint8_t *)heap_caps_malloc(LCD_TX_BYTES, MALLOC_CAP_DMA);
  if (!lcdTx)
    return false;

  for (size_t i = 0; i < sizeof(LCD_INIT);)
  {
    uint8_t cmd = LCD_INIT[i], len = LCD_INIT[i + 1];
    lcdCmd(cmd, &LCD_INIT[i + 2], len);
    if (cmd == 0x11)
      delay(120);
    else if (cmd == 0x35)
      delay(20);
    i += 2 + len;
  }
  // The table leaves the panel in its normal state (inversion on). The
  // "Invert display colors" setting turns it off, i.e. shows the negative.
  if (Settings.invertColors)
    lcdCmd(0x20, NULL, 0);
  return true;
}

// Send a rectangle of the (landscape) frame buffer to the (portrait) panel.
// Not flipped: the panel's long edge runs right-to-left across the frame
// buffer; flipped is the same turned by 180 degrees.
static void panelFlush(int x, int y, int w, int h)
{
  if (!lcdSpi || !fb.created())
    return;
  if (x < 0)
  {
    w += x;
    x = 0;
  }
  if (y < 0)
  {
    h += y;
    y = 0;
  }
  if (x + w > SCR_W)
    w = SCR_W - x;
  if (y + h > SCR_H)
    h = SCR_H - y;
  if (w <= 0 || h <= 0)
    return;

  int px0, px1, py0, py1; // native rectangle, end exclusive
  if (!lcdFlipped)
  {
    px0 = y;
    px1 = y + h;
    py0 = LCD_NATIVE_H - (x + w);
    py1 = LCD_NATIVE_H - x;
  }
  else
  {
    px0 = LCD_NATIVE_W - (y + h);
    px1 = LCD_NATIVE_W - y;
    py0 = x;
    py1 = x + w;
  }
  // The controller drives its gates in pairs: windows must be 4-pixel aligned.
  px0 &= ~3;
  py0 &= ~3;
  px1 = (px1 + 3) & ~3;
  py1 = (py1 + 3) & ~3;
  const int nw = px1 - px0, nh = py1 - py0;

  spi_device_acquire_bus(lcdSpi, portMAX_DELAY);
  const uint8_t ca[4] = {(uint8_t)(px0 >> 8), (uint8_t)px0, (uint8_t)((px1 - 1) >> 8), (uint8_t)(px1 - 1)};
  const uint8_t ra[4] = {(uint8_t)(py0 >> 8), (uint8_t)py0, (uint8_t)((py1 - 1) >> 8), (uint8_t)(py1 - 1)};
  lcdCmd(0x2A, ca, 4);
  lcdCmd(0x2B, ra, 4);

  const uint16_t *src = (const uint16_t *)fb.getPointer(); // already in wire byte order
  uint16_t *dst = (uint16_t *)lcdTx;
  const int rowsPerTx = (LCD_TX_BYTES / 2) / nw;
  for (int r = 0; r < nh; r += rowsPerTx)
  {
    const int n = (nh - r < rowsPerTx) ? nh - r : rowsPerTx;
    // Walk the frame buffer along its rows (it lives in PSRAM) and scatter
    // into the transfer buffer.
    for (int i = 0; i < nw; i++)
    {
      const int px = px0 + i;
      if (!lcdFlipped)
      {
        const uint16_t *s = src + px * SCR_W + (SCR_W - 1 - (py0 + r));
        for (int j = 0; j < n; j++)
          dst[j * nw + i] = s[-j];
      }
      else
      {
        const uint16_t *s = src + (SCR_H - 1 - px) * SCR_W + (py0 + r);
        for (int j = 0; j < n; j++)
          dst[j * nw + i] = s[j];
      }
    }

    // Pixels: opcode 0x32 and RAMWR in the address, data on four lines. CS
    // stays low across the transfers of one window.
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_MODE_QIO;
    if (r == 0)
    {
      t.base.cmd = 0x32;
      t.base.addr = 0x002C00;
    }
    else
    {
      t.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    }
    if (r + n < nh)
      t.base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
    t.base.tx_buffer = lcdTx;
    t.base.length = (size_t)n * nw * 16;
    spi_device_polling_transmit(lcdSpi, &t.base);
  }
  spi_device_release_bus(lcdSpi);
}

static void panelFlushAll() { panelFlush(0, 0, SCR_W, SCR_H); }

static void flipHardware()
{
  lcdFlipped = !lcdFlipped;
  pulsePersistFlip(nvMem, lcdFlipped);
  panelFlushAll();
}

// ================================================================== touch ==
// The touch half of the ST77922: 16-bit register addresses, MSB first.

#define TP_REG_STATUS 0x0001
#define TP_REG_MAX_X 0x0005
#define TP_REG_MAX_TOUCHES 0x0009
#define TP_REG_REPORT 0x0010 // 4 bytes of report header, then 7 bytes per point
#define TP_POINT_BYTES 7
#define TP_MAX_POINTS 10

static bool touchReady = false;
static uint8_t touchPoints = TP_MAX_POINTS;

static bool touchRead(uint16_t reg, uint8_t *buf, size_t n)
{
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, n) != n)
    return false;
  for (size_t i = 0; i < n; i++)
    buf[i] = Wire.read();
  return true;
}

static void touchInit()
{
  pinMode(TOUCH_PIN_RST, OUTPUT);
  digitalWrite(TOUCH_PIN_RST, HIGH);
  delay(5);
  digitalWrite(TOUCH_PIN_RST, LOW);
  delay(5);
  digitalWrite(TOUCH_PIN_RST, HIGH);
  delay(40);
  Wire.begin(TOUCH_PIN_SDA, TOUCH_PIN_SCL, 400000);

  uint8_t status = 0xFF, res[4] = {0};
  for (int i = 0; i < 20; i++) // low nibble 1 = controller still starting
  {
    if (touchRead(TP_REG_STATUS, &status, 1) && (status & 0x0F) != 1)
    {
      touchReady = true;
      break;
    }
    delay(10);
  }
  if (touchReady)
  {
    uint8_t n = 0;
    touchRead(TP_REG_MAX_X, res, 4);
    if (touchRead(TP_REG_MAX_TOUCHES, &n, 1) && n >= 1 && n <= TP_MAX_POINTS)
      touchPoints = n;
  }
  Serial.printf("[touch] %s, status 0x%02X, range %u x %u, %u points\n", touchReady ? "ready" : "not responding",
                status, ((res[0] & 0x3F) << 8) | res[1], ((res[2] & 0x3F) << 8) | res[3], touchPoints);
}

static int touchRawX = 0, touchRawY = 0; // last point as the controller reported it

// First touch point in screen (landscape) coordinates.
static bool touchGetXY(int &x, int &y)
{
  // The whole report is read every time: the controller holds a frame until
  // it has been read out, so reading only the first point leaves it stuck on
  // the first touch.
  uint8_t d[4 + TP_MAX_POINTS * TP_POINT_BYTES];
  if (!touchReady || !touchRead(TP_REG_REPORT, d, 4 + touchPoints * TP_POINT_BYTES))
    return false;
  const uint8_t *p = d + 4;
  if (!(p[0] & 0x80))
    return false;
  int px = ((p[0] & 0x3F) << 8) | p[1];
  int py = ((p[2] & 0x3F) << 8) | p[3];
  touchRawX = px;
  touchRawY = py;
  if (!lcdFlipped)
  {
    x = LCD_NATIVE_H - 1 - py;
    y = px;
  }
  else
  {
    x = py;
    y = LCD_NATIVE_W - 1 - px;
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
        Serial.printf("[touch] x=%d y=%d (raw %d,%d)\n", t_x, t_y, touchRawX, touchRawY);
      }
    }
    else
    {
      if (wasDown)
        Serial.println("[touch] released");
      downCount = 0;
      wasDown = false;
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ----------------------------------------------------- dev: screenshot dump --

#ifdef ES3C35P_UI_SCREENSHOT
// Raw frame buffer: SCR_W x SCR_H pixels, 2 bytes each, high byte first.
static void dumpShot(const char *name)
{
  panelFlushAll();
  Serial.printf("\nSHOT_BEGIN %s %d %d\n", name, SCR_W, SCR_H);
  const uint8_t *p = (const uint8_t *)fb.getPointer();
  size_t left = (size_t)SCR_W * SCR_H * 2;
  while (left)
  {
    size_t n = Serial.write(p, left > 512 ? 512 : left);
    p += n;
    left -= n;
    Serial.flush(); // the USB serial drops data when its buffer overruns
    if (!n)
      delay(2);
  }
  Serial.printf("\nSHOT_END\n");
  Serial.flush();
}

static void dumpScreens(void)
{
  // Wait for the capture script to say it is listening.
  Serial.println("SHOT_READY (send any byte)");
  for (int i = 0; i < 600 && !Serial.available(); i++)
    delay(50);
  while (Serial.available())
    Serial.read();

  pulse480RunScreenshotDump(dumpShot);
}
#endif

// --------------------------------------------------------- driver plumbing --

void es3c35p_Init(void)
{
  // Speaker amplifier off, backlight off until the first frame is on the panel
  pinMode(AMP_PIN_SHUTDOWN, OUTPUT);
  digitalWrite(AMP_PIN_SHUTDOWN, HIGH);
  ledcSetup(0, 5000, 8); // LEDC channel 0, 5kHz, 8 bit (0-255 duty)
  ledcAttachPin(LCD_PIN_BL, 0);
  ledcWrite(0, 0);

  if (nvMem.loadConfig(&Settings)) // also the invert setting, read by panelInit
    lcdFlipped = Settings.flipDisplay;

  // TFT_eSPI's text path touches its SPI port's registers even when the
  // target is a sprite. That port is SPI3 here (USE_HSPI_PORT), which nothing
  // else uses: keep it clocked so those writes are harmless.
  periph_module_enable(PERIPH_SPI3_MODULE);

  fb.setColorDepth(16);
  fb.createSprite(SCR_W, SCR_H);
  if (!fb.created())
    Serial.printf("[ui] frame buffer alloc failed (PSRAM %u bytes free)\n", ESP.getFreePsram());
  else
    fb.fillSprite(C_BG);

  if (!panelInit())
    Serial.println("[ui] panel init failed");
  panelFlushAll();
  ledcWrite(0, Settings.Brightness);

  touchInit();
  xTaskCreate(touchTask, "touch", 3072, NULL, 2, NULL);

  Pulse480Backend backend = {};
  backend.tft = &tft;
  backend.canvas = &fb;
  backend.fb = &fb;
  backend.sprite8BitFallback = false;
  backend.gaugeMaxKh = G_MAX_KH;
  backend.redrawOnFlip = false;
  backend.flushRect = panelFlush;
  backend.flushAll = panelFlushAll;
  backend.flipHardware = flipHardware;
  if (!pulse480Init(backend))
    return;

#ifdef ES3C35P_UI_SCREENSHOT
  dumpScreens();
#endif
}

CyclicScreenFunction es3c35pCyclicScreens[] = {pulse480MinerScreen, pulse480MarketScreen, pulse480ClockScreen};

DisplayDriver es3c35pDriver = {
    es3c35p_Init,
    pulse480AlternateScreenState,
    pulse480AlternateRotation,
    pulse480LoadingScreen,
    pulse480SetupScreen,
    es3c35pCyclicScreens,
    pulse480AnimateCurrentScreen,
    pulse480DoLedStuff,
    SCREENS_ARRAY_SIZE(es3c35pCyclicScreens),
    0,
    SCR_W,
    SCR_H};
#endif
