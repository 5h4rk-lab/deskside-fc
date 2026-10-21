// ST7789 driver.
//
// Init sequence, pins and window addressing adapted from the Waveshare demo
// Arduino/examples/LVGL_Arduino/Display_ST7789.cpp in
// ESP32-S3-Touch-LCD-2.8-V2-Demo.zip (identical in the V1 demo zip),
// downloaded 2026-10-20, via firmware/bringup/src/display_st7789.cpp (verified
// on the real board). Rewritten as a command table; the bytes are unchanged.
//
// Vendor-verified: every init command and parameter byte, MADCTL 0x00
// (portrait, RGB order), COLMOD 0x05 (16 bit), RAMCTRL 0xB0 00 E8, INVON,
// offsets 0/0, SPI mode 0.
// Chosen here:
//   - SPI clock: 40 MHz by default (the vendor uses 80 MHz), kDisplaySpiHz.
//   - Byte order: the vendor sends LVGL's little-endian RGB565 memory as is
//     (LV_COLOR_16_SWAP 0), which RAMCTRL 0xE8 (ENDIAN bit set) is meant to
//     accept. The ESP32 SPI master sends a DMA buffer in memory order, so the
//     pixels need no swapping. Verified on the real board by the bring-up.
//   - CS is driven by the SPI peripheral: low for each command (+ parameters)
//     or pixel block (the vendor toggles CS per byte; both are valid for the
//     ST7789).
//   - Transfers use the ESP-IDF spi_master driver in queued (interrupt, DMA)
//     mode: one flush = 2A + columns + 2B + rows + 2C + pixels queued
//     back to back; the post-transaction callback of the pixel block reports
//     completion. The D/C line is set from the transaction's `user` field in
//     the pre-transaction callback.
#include "display_st7789.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_attr.h>

#include "pins.h"

namespace {

constexpr spi_host_device_t kHost = SPI2_HOST;  // vendor: SPIClass LCDspi(FSPI) = SPI2
// LVGL partial buffers are 240 x 40 pixels (lvgl_port.cpp); one block per flush.
constexpr size_t kMaxTransferBytes = (size_t)kDisplayWidth * 40 * sizeof(uint16_t);

// `user` field of a transaction: bit 0 is the D/C level, bit 1 marks the last
// pixel block of a flush (post callback reports it).
constexpr uintptr_t kUserCommand = 0;
constexpr uintptr_t kUserData = 1;
constexpr uintptr_t kUserDataLast = 3;

constexpr int kSetWindowTransactions = 5;  // 2A, 4 bytes, 2B, 4 bytes, 2C
constexpr int kMaxInFlight = kSetWindowTransactions + 1;

spi_device_handle_t s_dev = nullptr;
spi_transaction_t s_trans[kMaxInFlight];
int s_inFlight = 0;  // queued and not yet collected with spi_device_get_trans_result
DisplayFlushDone s_done = nullptr;
void* s_doneArg = nullptr;
volatile uint32_t s_errors = 0;

struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[14];
  uint16_t delayMs;
};

// Vendor order, including the early DISPON before SLPOUT (kept as is).
const InitCmd kInitSeq[] = {
    {0x29, 0, {}, 120},  // DISPON (vendor sends it early too)
    {0x11, 0, {}, 120},  // SLPOUT
    {0x36, 1, {0x00}, 0},  // MADCTL 0x00 portrait RGB
    {0x3A, 1, {0x05}, 0},  // COLMOD 16 bit
    {0xB0, 2, {0x00, 0xE8}, 0},  // RAMCTRL 00 E8 (little-endian RGB565)
    {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 0},  // PORCTRL
    {0xB7, 1, {0x75}, 0},  // GCTRL VGH=14.97V VGL=-7.67V
    {0xBB, 1, {0x1A}, 0},  // VCOMS
    {0xC0, 1, {0x2C}, 0},  // LCMCTRL
    {0xC2, 2, {0x01, 0xFF}, 0},  // VDVVRHEN
    {0xC3, 1, {0x13}, 0},  // VRHS
    {0xC4, 1, {0x20}, 0},  // VDVS
    {0xC6, 1, {0x0F}, 0},  // FRCTRL2
    {0xD0, 2, {0xA4, 0xA1}, 0},  // PWCTRL1
    {0xD6, 1, {0xA1}, 0},  // D6 (vendor)
    {0xE0, 14, {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30}, 0},  // PVGAMCTRL
    {0xE1, 14, {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E}, 0},  // NVGAMCTRL
    {0x21, 0, {}, 0},  // INVON (vendor default; the panel needs it for correct colours)
    {0x29, 0, {}, 0},  // DISPON
    {0x2C, 0, {}, 0},  // RAMWR, as the vendor ends its init
};

// Pre-transaction callback (interrupt context): D/C level from `user`.
void dcPreCb(spi_transaction_t* t) { gpio_set_level((gpio_num_t)pins::kLcdDc, (uintptr_t)t->user & 1); }

// Post-transaction callback (interrupt context): only the last pixel block of a
// flush reports completion.
void postCb(spi_transaction_t* t) {
  if ((uintptr_t)t->user == kUserDataLast && s_done) s_done(s_doneArg);
}

// Collects the finished transactions of the previous flush (the last one has
// already reported through postCb; the driver hands the result over a moment
// later). Returns false if one did not finish in time.
bool collectInFlight() {
  while (s_inFlight > 0) {
    spi_transaction_t* done = nullptr;
    if (spi_device_get_trans_result(s_dev, &done, pdMS_TO_TICKS(200)) != ESP_OK) {
      s_errors = s_errors + 1;
      Serial.printf("[display] ERROR: SPI transaction did not finish (%d in flight)\n", s_inFlight);
      return false;
    }
    --s_inFlight;
  }
  return true;
}

// Fills a transaction slot with a 1 byte command or up to 4 bytes of inline data.
void prepInline(spi_transaction_t& t, uintptr_t user, const uint8_t* data, size_t len) {
  memset(&t, 0, sizeof(t));
  t.flags = SPI_TRANS_USE_TXDATA;
  t.length = len * 8;
  t.user = (void*)user;
  for (size_t i = 0; i < len; ++i) t.tx_data[i] = data[i];
}

bool queueTrans(int index) {
  if (spi_device_queue_trans(s_dev, &s_trans[index], pdMS_TO_TICKS(100)) != ESP_OK) {
    s_errors = s_errors + 1;
    Serial.printf("[display] ERROR: spi_device_queue_trans(%d) failed\n", index);
    return false;
  }
  ++s_inFlight;
  return true;
}

// Prepares transactions 0..4 for the window (inclusive corners).
void prepWindow(int x0, int y0, int x1, int y1) {
  const uint8_t cmdCol = 0x2A, cmdRow = 0x2B, cmdRam = 0x2C;
  // Column 0x2A uses x, row 0x2B uses y (vendor HORIZONTAL branch, offsets 0).
  const uint8_t col[4] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
  const uint8_t row[4] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
  prepInline(s_trans[0], kUserCommand, &cmdCol, 1);
  prepInline(s_trans[1], kUserData, col, 4);
  prepInline(s_trans[2], kUserCommand, &cmdRow, 1);
  prepInline(s_trans[3], kUserData, row, 4);
  prepInline(s_trans[4], kUserCommand, &cmdRam, 1);
}

// Blocking command (+ parameters), used by the init sequence only. Parameters
// longer than 4 bytes are copied to a RAM buffer (the table lives in flash,
// which DMA cannot read).
uint8_t s_initParams[16];

bool sendCommandBlocking(uint8_t cmd, const uint8_t* data, size_t len) {
  if (len > sizeof(s_initParams)) return false;
  if (!collectInFlight()) return false;
  prepInline(s_trans[0], kUserCommand, &cmd, 1);
  if (!queueTrans(0)) return false;
  if (len > 0) {
    if (len <= 4) {
      prepInline(s_trans[1], kUserData, data, len);
    } else {
      memcpy(s_initParams, data, len);
      memset(&s_trans[1], 0, sizeof(s_trans[1]));
      s_trans[1].length = len * 8;
      s_trans[1].tx_buffer = s_initParams;
      s_trans[1].user = (void*)kUserData;
    }
    if (!queueTrans(1)) return false;
  }
  return collectInFlight();
}

}  // namespace

bool display_begin(DisplayFlushDone done, void* arg) {
  s_done = done;
  s_doneArg = arg;

  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << pins::kLcdDc) | (1ULL << pins::kLcdRst);
  io.mode = GPIO_MODE_OUTPUT;
  if (gpio_config(&io) != ESP_OK) {
    Serial.println("[display] ERROR: gpio_config for DC/RST failed");
    return false;
  }
  gpio_set_level((gpio_num_t)pins::kLcdDc, 1);
  gpio_set_level((gpio_num_t)pins::kLcdRst, 1);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = pins::kLcdMosi;
  bus.miso_io_num = pins::kLcdMiso;
  bus.sclk_io_num = pins::kLcdSclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = (int)kMaxTransferBytes;
  esp_err_t err = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("[display] ERROR: spi_bus_initialize failed (%d)\n", (int)err);
    return false;
  }

  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = (int)kDisplaySpiHz;
  dev.mode = 0;  // SPI mode 0
  dev.spics_io_num = pins::kLcdCs;
  dev.queue_size = kMaxInFlight;
  dev.flags = SPI_DEVICE_NO_DUMMY;  // write only; needed above 26 MHz on the GPIO matrix
  dev.pre_cb = dcPreCb;
  dev.post_cb = postCb;
  err = spi_bus_add_device(kHost, &dev, &s_dev);
  if (err != ESP_OK) {
    Serial.printf("[display] ERROR: spi_bus_add_device failed (%d)\n", (int)err);
    spi_bus_free(kHost);
    return false;
  }

  // Hardware reset: vendor LCD_Reset() is 50 ms low, 50 ms high, then 120 ms.
  gpio_set_level((gpio_num_t)pins::kLcdRst, 1);
  delay(5);
  gpio_set_level((gpio_num_t)pins::kLcdRst, 0);
  delay(50);
  gpio_set_level((gpio_num_t)pins::kLcdRst, 1);
  delay(50);
  delay(120);

  for (const InitCmd& c : kInitSeq) {
    if (!sendCommandBlocking(c.cmd, c.data, c.len)) {
      Serial.printf("[display] ERROR: init command 0x%02X failed\n", c.cmd);
      return false;
    }
    if (c.delayMs) delay(c.delayMs);
  }
  return true;
}

size_t display_max_transfer_bytes() { return kMaxTransferBytes; }

bool display_set_window(int x0, int y0, int x1, int y1) {
  if (!s_dev || !collectInFlight()) return false;
  prepWindow(x0, y0, x1, y1);
  for (int i = 0; i < kSetWindowTransactions; ++i) {
    if (!queueTrans(i)) return false;
  }
  return true;
}

bool display_flush_async(int x0, int y0, int x1, int y1, const uint16_t* pixels) {
  if (!s_dev || !pixels) return false;
  const size_t bytes = (size_t)(x1 - x0 + 1) * (size_t)(y1 - y0 + 1) * sizeof(uint16_t);
  if (x1 < x0 || y1 < y0 || x0 < 0 || y0 < 0 || x1 >= kDisplayWidth || y1 >= kDisplayHeight ||
      bytes > kMaxTransferBytes) {
    s_errors = s_errors + 1;
    Serial.printf("[display] ERROR: bad flush area %d,%d..%d,%d (%u bytes)\n", x0, y0, x1, y1, (unsigned)bytes);
    return false;
  }
  if (!display_set_window(x0, y0, x1, y1)) return false;

  spi_transaction_t& t = s_trans[kSetWindowTransactions];
  memset(&t, 0, sizeof(t));
  t.length = bytes * 8;
  t.tx_buffer = pixels;
  t.user = (void*)kUserDataLast;
  return queueTrans(kSetWindowTransactions);
}

uint32_t display_error_count() { return s_errors; }
