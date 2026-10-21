// Minimal ST7789 driver for the bring-up test (240 x 320, RGB565, portrait).
#pragma once

#include <stdint.h>

namespace display {

constexpr int kWidth = 240;
constexpr int kHeight = 320;

// RGB565 colours (logical values; byte order on the wire is handled inside).
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;
constexpr uint16_t kYellow = 0xFFE0;
constexpr uint16_t kCyan = 0x07FF;
constexpr uint16_t kMagenta = 0xF81F;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kBlack = 0x0000;

// Test card geometry: three horizontal bands.
constexpr int kTopBandEnd = 107;     // rows 0..106: eight 30 px colour columns
constexpr int kMiddleBandEnd = 214;  // rows 107..213: grayscale ramp
// rows 214..319: bottom panel with frame, diagonal and the 40 x 40 corner square
constexpr int kCornerSquareX = 0;
constexpr int kCornerSquareY = kMiddleBandEnd;
constexpr int kCornerSquareSize = 40;

// Backlight on (ramp 20 % -> 100 % over 1 s). Returns false if LEDC failed.
bool backlightBegin();

// SPI setup, hardware reset and the vendor init sequence.
// Returns false if the SPI bus could not be started.
bool begin();

uint32_t spiHz();

void fill(uint16_t color);
void drawTestCard();

// Pixel of the test card at (x, y); used to redraw any region.
uint16_t testCardPixel(int x, int y);

// Redraw the test card inside a rectangle (clipped to the screen).
void restoreTestCardRect(int x, int y, int w, int h);

// Draw a size x size marker centred on (cx, cy): the inverse of the test
// card pixels underneath, so it is visible on every band.
void drawTouchMarker(int cx, int cy, int size);

bool inCornerSquare(int x, int y, int slack);

}  // namespace display
