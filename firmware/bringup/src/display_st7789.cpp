// ST7789 bring-up driver.
//
// Init sequence, pins, backlight PWM and window addressing adapted from the
// Waveshare demo Arduino/examples/LVGL_Arduino/Display_ST7789.cpp in
// ESP32-S3-Touch-LCD-2.8-V2-Demo.zip (identical in the V1 demo zip),
// downloaded 2026-10-20. Rewritten as a command table; the bytes are unchanged.
//
// Vendor-verified: every init command and parameter byte, MADCTL 0x00
// (portrait, RGB order), COLMOD 0x05 (16 bit), RAMCTRL 0xB0 00 E8, INVON,
// offsets 0/0, SPI mode 0, backlight 20 kHz / 10 bit on GPIO5.
// Guessed / chosen here:
//   - SPI clock: 40 MHz by default (the vendor uses 80 MHz). Override with
//     -DBRINGUP_SPI_HZ=80000000.
//   - Byte order: the vendor sends LVGL's little-endian RGB565 memory as is
//     (LV_COLOR_16_SWAP 0), which RAMCTRL 0xE8 (ENDIAN bit set) is meant to
//     accept. We do the same by default; -DBRINGUP_SWAP_BYTES=1 sends the
//     high byte first instead (classic ST7789 order) in case colours are off.
//   - CS is held low for a whole command + parameters (the vendor toggles CS
//     per byte; both are valid for the ST7789).
#include "display_st7789.h"

#include <Arduino.h>
#include <SPI.h>

#include "board_pins.h"
#include "bringup_log.h"

#ifndef BRINGUP_SPI_HZ
#define BRINGUP_SPI_HZ 40000000
#endif

namespace display {
namespace {

SPIClass s_spi(FSPI);  // vendor: SPIClass LCDspi(FSPI)
const SPISettings kSpiSettings(BRINGUP_SPI_HZ, MSBFIRST, SPI_MODE0);

// Line buffer in internal RAM (static => DRAM). 16 rows = 7.5 KB.
constexpr int kBufRows = 16;
uint16_t s_lineBuf[kWidth * kBufRows];

// Backlight: vendor Frequency 20000, Resolution 10.
constexpr uint32_t kBacklightHz = 20000;
constexpr uint8_t kBacklightBits = 10;
constexpr uint32_t kBacklightFull = 1u << kBacklightBits;  // vendor writes 1024 for 100 %

struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[14];
  uint16_t delayMs;
  const char *name;
};

// Vendor order, including the early DISPON before SLPOUT (kept as is).
const InitCmd kInitSeq[] = {
    {0x29, 0, {}, 120, "DISPON (vendor sends it early too)"},
    {0x11, 0, {}, 120, "SLPOUT"},
    {0x36, 1, {0x00}, 0, "MADCTL 0x00 portrait RGB"},
    {0x3A, 1, {0x05}, 0, "COLMOD 16 bit"},
    {0xB0, 2, {0x00, 0xE8}, 0, "RAMCTRL 00 E8 (little-endian RGB565)"},
    {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 0, "PORCTRL"},
    {0xB7, 1, {0x75}, 0, "GCTRL VGH=14.97V VGL=-7.67V"},
    {0xBB, 1, {0x1A}, 0, "VCOMS"},
    {0xC0, 1, {0x2C}, 0, "LCMCTRL"},
    {0xC2, 2, {0x01, 0xFF}, 0, "VDVVRHEN"},
    {0xC3, 1, {0x13}, 0, "VRHS"},
    {0xC4, 1, {0x20}, 0, "VDVS"},
    {0xC6, 1, {0x0F}, 0, "FRCTRL2"},
    {0xD0, 2, {0xA4, 0xA1}, 0, "PWCTRL1"},
    {0xD6, 1, {0xA1}, 0, "D6 (vendor)"},
    {0xE0, 14, {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30}, 0,
     "PVGAMCTRL"},
    {0xE1, 14, {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E}, 0,
     "NVGAMCTRL"},
};

inline uint16_t toWire(uint16_t c) {
#if defined(BRINGUP_SWAP_BYTES) && BRINGUP_SWAP_BYTES
  return __builtin_bswap16(c);  // high byte first on the wire
#else
  return c;  // little-endian memory as is (vendor behaviour)
#endif
}

void writeCommand(uint8_t cmd, const uint8_t *data, size_t len) {
  s_spi.beginTransaction(kSpiSettings);
  digitalWrite(pins::kLcdCs, LOW);
  digitalWrite(pins::kLcdDc, LOW);
  s_spi.write(cmd);
  if (len > 0) {
    digitalWrite(pins::kLcdDc, HIGH);
    s_spi.writeBytes(data, len);
  }
  digitalWrite(pins::kLcdCs, HIGH);
  s_spi.endTransaction();
}

void writeCommand(uint8_t cmd) { writeCommand(cmd, nullptr, 0); }

// Column 0x2A uses x, row 0x2B uses y (vendor HORIZONTAL branch, offsets 0).
void setWindow(int x0, int y0, int x1, int y1) {
  const uint8_t col[4] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
  const uint8_t row[4] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
  writeCommand(0x2A, col, sizeof(col));
  writeCommand(0x2B, row, sizeof(row));
  writeCommand(0x2C);
}

void pushPixels(const uint16_t *px, size_t count) {
  s_spi.beginTransaction(kSpiSettings);
  digitalWrite(pins::kLcdCs, LOW);
  digitalWrite(pins::kLcdDc, HIGH);
  s_spi.writeBytes(reinterpret_cast<const uint8_t *>(px), count * sizeof(uint16_t));
  digitalWrite(pins::kLcdCs, HIGH);
  s_spi.endTransaction();
}

uint16_t gray565(uint8_t level) {
  return uint16_t(((level >> 3) << 11) | ((level >> 2) << 5) | (level >> 3));
}

// Bottom panel: frame, diagonal from its top-left to bottom-right corner.
bool onBottomDiagonal(int x, int y) {
  const int h = kHeight - kMiddleBandEnd;  // 106 rows
  const int dy = y - kMiddleBandEnd;
  const int xa = dy * (kWidth - 1) / (h - 1);
  const int xb = (dy + 1) * (kWidth - 1) / (h - 1);
  const int xEnd = xb - 1 > xa ? xb - 1 : xa;  // keep the line continuous
  return x >= xa && x <= xEnd;
}

using PixelFn = uint16_t (*)(int x, int y);

// Draw a clipped rectangle whose pixels come from fn, through the line buffer.
void drawRegion(int x, int y, int w, int h, PixelFn fn) {
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w - 1 >= kWidth ? kWidth - 1 : x + w - 1;
  int y1 = y + h - 1 >= kHeight ? kHeight - 1 : y + h - 1;
  if (x0 > x1 || y0 > y1) return;
  const int cw = x1 - x0 + 1;
  const int rowsPerChunk = (kWidth * kBufRows) / cw;
  setWindow(x0, y0, x1, y1);
  for (int row = y0; row <= y1; row += rowsPerChunk) {
    const int rows = (y1 - row + 1) < rowsPerChunk ? (y1 - row + 1) : rowsPerChunk;
    size_t i = 0;
    for (int yy = row; yy < row + rows; ++yy) {
      for (int xx = x0; xx <= x1; ++xx) s_lineBuf[i++] = toWire(fn(xx, yy));
    }
    pushPixels(s_lineBuf, i);
  }
}

uint16_t invertedTestCardPixel(int x, int y) { return uint16_t(~testCardPixel(x, y)); }

}  // namespace

uint32_t spiHz() { return BRINGUP_SPI_HZ; }

bool backlightBegin() {
  if (!ledcAttach(pins::kLcdBacklight, kBacklightHz, kBacklightBits)) {
    logf("[display] backlight: ledcAttach(GPIO%d, %lu Hz, %u bit) FAILED", pins::kLcdBacklight,
         (unsigned long)kBacklightHz, kBacklightBits);
    return false;
  }
  logf("[display] backlight: GPIO%d LEDC %lu Hz %u bit, ramp 20%% -> 100%% over 1 s",
       pins::kLcdBacklight, (unsigned long)kBacklightHz, kBacklightBits);
  logf("[display] expect: backlight glow rising (screen content may still be garbage)");
  const uint32_t start = kBacklightFull / 5;
  for (int step = 0; step <= 50; ++step) {  // 50 steps x 20 ms = 1 s
    ledcWrite(pins::kLcdBacklight, start + (kBacklightFull - start) * step / 50);
    delay(20);
  }
  logf("[display] backlight: 100%%");
  return true;
}

bool begin() {
  pinMode(pins::kLcdCs, OUTPUT);
  pinMode(pins::kLcdDc, OUTPUT);
  pinMode(pins::kLcdRst, OUTPUT);
  digitalWrite(pins::kLcdCs, HIGH);
  digitalWrite(pins::kLcdDc, HIGH);

  if (!s_spi.begin(pins::kLcdSclk, pins::kLcdMiso, pins::kLcdMosi, -1)) {
    logf("[display] SPI begin FAILED (FSPI SCLK=%d MOSI=%d)", pins::kLcdSclk, pins::kLcdMosi);
    return false;
  }
  logf("[display] SPI FSPI MOSI=%d SCLK=%d CS=%d DC=%d mode 0 at %lu Hz (vendor: 80000000)",
       pins::kLcdMosi, pins::kLcdSclk, pins::kLcdCs, pins::kLcdDc, (unsigned long)BRINGUP_SPI_HZ);

  // Hardware reset: vendor LCD_Reset() is 50 ms low, 50 ms high, then 120 ms.
  digitalWrite(pins::kLcdRst, HIGH);
  delay(5);
  digitalWrite(pins::kLcdRst, LOW);
  delay(50);
  digitalWrite(pins::kLcdRst, HIGH);
  delay(50);
  delay(120);
  logf("[display] reset pulse on GPIO%d done (50 ms low)", pins::kLcdRst);

  for (const InitCmd &c : kInitSeq) {
    writeCommand(c.cmd, c.data, c.len);
    if (c.delayMs) delay(c.delayMs);
  }
  logf("[display] init: %u vendor commands sent (0x29 0x11 0x36 0x3A 0xB0 ... gamma E0/E1)",
       (unsigned)(sizeof(kInitSeq) / sizeof(kInitSeq[0])));

#if defined(BRINGUP_NO_INVERT) && BRINGUP_NO_INVERT
  logf("[display] INVON (0x21) SKIPPED (BRINGUP_NO_INVERT)");
#else
  writeCommand(0x21);
  logf("[display] INVON (0x21) sent (vendor default)");
#endif
  writeCommand(0x29);  // DISPON
  writeCommand(0x2C);  // RAMWR, as the vendor ends its init
#if defined(BRINGUP_SWAP_BYTES) && BRINGUP_SWAP_BYTES
  logf("[display] pixel byte order: high byte first (BRINGUP_SWAP_BYTES)");
#else
  logf("[display] pixel byte order: low byte first (vendor default)");
#endif
  logf("[display] DISPON sent, init complete");
  return true;
}

void fill(uint16_t color) {
  const uint16_t w = toWire(color);
  for (int i = 0; i < kWidth * kBufRows; ++i) s_lineBuf[i] = w;
  setWindow(0, 0, kWidth - 1, kHeight - 1);
  for (int row = 0; row < kHeight; row += kBufRows) pushPixels(s_lineBuf, kWidth * kBufRows);
}

uint16_t testCardPixel(int x, int y) {
  if (y < kTopBandEnd) {
    static const uint16_t kBars[8] = {kRed, kGreen, kBlue, kYellow, kCyan, kMagenta, kWhite, kBlack};
    return kBars[x / 30];
  }
  if (y < kMiddleBandEnd) {
    return gray565(uint8_t(x * 255 / (kWidth - 1)));
  }
  if (x == 0 || x == kWidth - 1 || y == kMiddleBandEnd || y == kHeight - 1) return kWhite;
  if (x < kCornerSquareX + kCornerSquareSize && y < kCornerSquareY + kCornerSquareSize) return kWhite;
  if (onBottomDiagonal(x, y)) return kWhite;
  return kBlack;
}

void drawTestCard() { drawRegion(0, 0, kWidth, kHeight, testCardPixel); }

void restoreTestCardRect(int x, int y, int w, int h) { drawRegion(x, y, w, h, testCardPixel); }

void drawTouchMarker(int cx, int cy, int size) {
  drawRegion(cx - size / 2, cy - size / 2, size, size, invertedTestCardPixel);
}

bool inCornerSquare(int x, int y, int slack) {
  return x >= kCornerSquareX - slack && x < kCornerSquareX + kCornerSquareSize + slack &&
         y >= kCornerSquareY - slack && y < kCornerSquareY + kCornerSquareSize + slack;
}

}  // namespace display
