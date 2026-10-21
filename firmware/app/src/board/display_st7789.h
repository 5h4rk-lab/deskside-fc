// ST7789 panel driver (240 x 320, RGB565, portrait) over SPI with DMA.
//
// No LVGL in here: the LVGL port calls display_flush_async() and is told by a
// callback (run in interrupt context) when the pixels have left the SPI bus.
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 320;

// SPI clock. 40 MHz is verified on the real board (the vendor uses 80 MHz).
#ifndef APP_DISPLAY_SPI_HZ
#define APP_DISPLAY_SPI_HZ 40000000
#endif
constexpr uint32_t kDisplaySpiHz = APP_DISPLAY_SPI_HZ;

// Called from the SPI interrupt when a display_flush_async() transfer is done.
using DisplayFlushDone = void (*)(void* arg);

// Starts the SPI bus, resets the panel and sends the vendor init sequence.
// Returns false (after printing the reason) on any failure; the display must
// then not be used. Takes about 0.4 s. The backlight is not touched.
bool display_begin(DisplayFlushDone done, void* arg);

// Largest pixel block display_flush_async() accepts, in bytes.
size_t display_max_transfer_bytes();

// Sets the RAM window (inclusive corners) and queues the memory-write command.
// Part of display_flush_async(); public for tests. Returns false if the SPI
// queue refused a transaction (printed). Not for use while a flush is running.
bool display_set_window(int x0, int y0, int x1, int y1);

// Sends one block of RGB565 pixels (little-endian memory order, as LVGL
// renders it; no byte swapping needed) to the window x0..x1 / y0..y1. `pixels`
// must be DMA-capable internal RAM and stay untouched until `done` runs.
// Returns false (printed) if the transfer could not be queued; then `done`
// will NOT run and the caller must treat the block as finished.
bool display_flush_async(int x0, int y0, int x1, int y1, const uint16_t* pixels);

// Transfers that failed to queue or complete, since boot.
uint32_t display_error_count();
