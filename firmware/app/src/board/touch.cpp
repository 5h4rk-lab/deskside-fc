// Touch: CST328 (V1) and CST3530 (V2) on Wire1.
//
// Register protocol, reset timings and the detection order adapted from the
// Waveshare demo Arduino/examples/LVGL_Arduino/Touch_CST328.cpp and
// Touch_CST3530.cpp in ESP32-S3-Touch-LCD-2.8-V2-Demo.zip (downloaded
// 2026-10-20), via firmware/bringup/src/touch.cpp (verified on a V2 board).
// Vendor-verified: addresses 0x1A / 0x58, reset sequences, 0xCACA signature
// at 0xD1FE (24-byte block read from 0xD1F4), CST328 16-bit registers and
// coordinate unpacking, CST3530 32-bit registers, point layout and
// end-of-read write.
// CST3530 coordinates need no swap or mirror for this panel (the vendor
// passes them to LVGL unchanged, so do we; confirmed on the real board).
// No public datasheet exists for either chip.
#include "touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "display_st7789.h"
#include "pins.h"

namespace {

constexpr uint8_t kCst328Addr = 0x1A;
constexpr uint8_t kCst3530Addr = 0x58;
constexpr uint8_t kMaxPoints = 5;

// CST328 registers (vendor Touch_CST328.h).
constexpr uint16_t kCst328DebugInfoMode = 0xD101;
constexpr uint16_t kCst328NormalMode = 0xD109;
constexpr uint16_t kCst328InfoBlock = 0xD1F4;  // 24 bytes: D1F4..D20B
constexpr uint16_t kCst328BootTime = 0xD1FC;
constexpr uint16_t kCst328Xy = 0xD000;
constexpr uint16_t kCst328Count = 0xD005;
constexpr uint16_t kCst328Signature = 0xCACA;

// CST3530 registers (vendor Touch_CST3530.h).
constexpr uint32_t kCst3530Data = 0xD0070000;
constexpr uint32_t kCst3530NextPoints = 0xD0070900;
constexpr uint32_t kCst3530EndRead = 0xD00002AB;

// Empty polls (without INT activity) tolerated while pressed before the
// release is reported.
constexpr int kReleaseDebouncePolls = 1;

TouchController s_controller = TouchController::None;
volatile uint32_t s_interrupts = 0;
uint32_t s_i2cErrors = 0;

// Poll state.
bool s_pressed = false;
int s_missingPolls = 0;
uint32_t s_lastInterrupts = 0;
TouchPoint s_last = {0, 0};

void IRAM_ATTR onTouchInt() { s_interrupts = s_interrupts + 1; }

// ---- I2C helpers ----------------------------------------------------------

bool probe(TwoWire& bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission(true) == 0;
}

bool readBytes(uint8_t addr, uint8_t* out, size_t len) {
  if (Wire1.requestFrom(addr, len, true) != len) {
    ++s_i2cErrors;
    return false;
  }
  for (size_t i = 0; i < len; ++i) out[i] = uint8_t(Wire1.read());
  return true;
}

// CST328: 16-bit register address, STOP, then read (vendor Touch_I2C_Read).
bool cst328Read(uint16_t reg, uint8_t* out, size_t len) {
  Wire1.beginTransmission(kCst328Addr);
  Wire1.write(uint8_t(reg >> 8));
  Wire1.write(uint8_t(reg));
  if (Wire1.endTransmission(true) != 0) {
    ++s_i2cErrors;
    return false;
  }
  return readBytes(kCst328Addr, out, len);
}

bool cst328Write(uint16_t reg, const uint8_t* data, size_t len) {
  Wire1.beginTransmission(kCst328Addr);
  Wire1.write(uint8_t(reg >> 8));
  Wire1.write(uint8_t(reg));
  for (size_t i = 0; i < len; ++i) Wire1.write(data[i]);
  if (Wire1.endTransmission(true) != 0) {
    ++s_i2cErrors;
    return false;
  }
  return true;
}

// CST3530: 32-bit register address, repeated START, then read (vendor Touch2_I2C_Read).
bool cst3530Read(uint32_t reg, uint8_t* out, size_t len) {
  Wire1.beginTransmission(kCst3530Addr);
  Wire1.write(uint8_t(reg >> 24));
  Wire1.write(uint8_t(reg >> 16));
  Wire1.write(uint8_t(reg >> 8));
  Wire1.write(uint8_t(reg));
  if (Wire1.endTransmission(false) != 0) {
    ++s_i2cErrors;
    return false;
  }
  return readBytes(kCst3530Addr, out, len);
}

bool cst3530EndRead() {
  Wire1.beginTransmission(kCst3530Addr);
  Wire1.write(uint8_t(kCst3530EndRead >> 24));
  Wire1.write(uint8_t(kCst3530EndRead >> 16));
  Wire1.write(uint8_t(kCst3530EndRead >> 8));
  Wire1.write(uint8_t(kCst3530EndRead));
  if (Wire1.endTransmission(true) != 0) {
    ++s_i2cErrors;
    return false;
  }
  return true;
}

// ---- detection --------------------------------------------------------------

// Vendor CST328_Touch_Reset(): high 50 ms, low 5 ms, high 50 ms.
void cst328Reset() {
  digitalWrite(pins::kTouchRst, HIGH);
  delay(50);
  digitalWrite(pins::kTouchRst, LOW);
  delay(5);
  digitalWrite(pins::kTouchRst, HIGH);
  delay(50);
}

// Vendor CST3530_Touch_Reset(): low 100 ms, then high and wait 500 ms.
void cst3530Reset() {
  digitalWrite(pins::kTouchRst, LOW);
  delay(100);
  digitalWrite(pins::kTouchRst, HIGH);
  delay(500);
}

// Vendor CST328_Read_cfg(): enter debug-info mode, read the info block,
// expect 0xCACA in bytes 10..11 (register 0xD1FE), return to normal mode.
bool detectCst328() {
  cst328Reset();
  uint8_t buf[24] = {0};
  if (!cst328Write(kCst328DebugInfoMode, nullptr, 0)) return false;
  if (!cst328Read(kCst328BootTime, buf, 4)) return false;
  if (!cst328Read(kCst328InfoBlock, buf, sizeof(buf))) return false;
  const uint16_t signature = uint16_t(buf[11] << 8 | buf[10]);
  cst328Write(kCst328NormalMode, nullptr, 0);
  return signature == kCst328Signature;
}

bool detectCst3530() {
  cst3530Reset();
  return probe(Wire1, kCst3530Addr);
}

// ---- reading ---------------------------------------------------------------

// Vendor Touch_Read_Data() for CST328.
int readCst328(TouchPoint* first) {
  uint8_t buf[41] = {0};
  const uint8_t clear = 0;
  if (!cst328Read(kCst328Count, buf, 1)) return 0;
  const uint8_t count = buf[0] & 0x0F;
  if (count == 0 || count > kMaxPoints) {
    cst328Write(kCst328Count, &clear, 1);
    return 0;
  }
  if (!cst328Read(kCst328Xy, &buf[1], 27)) {
    cst328Write(kCst328Count, &clear, 1);
    return 0;
  }
  cst328Write(kCst328Count, &clear, 1);
  // Point 0: x high byte buf[2], y high byte buf[3], low nibbles buf[4].
  first->x = uint16_t((buf[2] << 4) | ((buf[4] & 0xF0) >> 4));
  first->y = uint16_t((buf[3] << 4) | (buf[4] & 0x0F));
  return count;
}

// Vendor Touch2_Read_Data() for CST3530.
int readCst3530(TouchPoint* first) {
  uint8_t buf[9] = {0};
  if (!cst3530Read(kCst3530Data, buf, sizeof(buf))) return 0;
  const uint8_t count = buf[3] & 0x0F;
  if (count == 0 || (buf[8] & 0xF0) == 0 || count > kMaxPoints) {
    cst3530EndRead();
    return 0;
  }
  // The vendor reads the other points from 0xD0070900 before ending the
  // read; we only report the first point but keep the same bus traffic.
  if (count > 1) {
    uint8_t rest[(kMaxPoints - 1) * 5];
    if (!cst3530Read(kCst3530NextPoints, rest, size_t(count - 1) * 5)) {
      cst3530EndRead();
      return 0;
    }
  }
  cst3530EndRead();
  first->x = uint16_t(((buf[7] & 0x0F) << 8) | buf[4]);
  first->y = uint16_t(((buf[7] & 0xF0) << 4) | buf[5]);
  return count;
}

}  // namespace

const char* touch_controller_name(TouchController c) {
  switch (c) {
    case TouchController::CST328: return "CST328";
    case TouchController::CST3530: return "CST3530";
    default: return "none";
  }
}

TouchController touch_begin() {
  pinMode(pins::kTouchInt, INPUT);
  pinMode(pins::kTouchRst, OUTPUT);
  if (!Wire1.begin(pins::kTouchSda, pins::kTouchScl, pins::kTouchI2cHz)) {
    Serial.printf("[touch] ERROR: Wire1.begin(SDA=%d, SCL=%d) failed\n", pins::kTouchSda, pins::kTouchScl);
    return s_controller = TouchController::None;
  }

  if (detectCst328()) {
    s_controller = TouchController::CST328;
  } else {
    s_controller = detectCst3530() ? TouchController::CST3530 : TouchController::None;
  }

  if (s_controller != TouchController::None) {
    attachInterrupt(digitalPinToInterrupt(pins::kTouchInt), onTouchInt, RISING);
  }
  return s_controller;
}

TouchController touch_controller() { return s_controller; }

bool touch_poll(TouchPoint& out) {
  out = s_last;
  if (s_controller == TouchController::None) return false;

  const uint32_t interrupts = s_interrupts;
  const bool intSeen = interrupts != s_lastInterrupts;
  s_lastInterrupts = interrupts;

  TouchPoint p = {0, 0};
  const int count = s_controller == TouchController::CST328 ? readCst328(&p) : readCst3530(&p);
  // A point outside the panel is a bad read, not a touch.
  const bool valid = count > 0 && p.x < kDisplayWidth && p.y < kDisplayHeight;

  if (valid) {
    s_pressed = true;
    s_missingPolls = 0;
    s_last = p;
    out = p;
    return true;
  }
  if (!s_pressed) return false;

  // Finger was down, this read is empty. INT activity means the controller is
  // still talking (a transient empty frame): keep the press. Otherwise allow
  // one empty poll, then release.
  if (intSeen) {
    s_missingPolls = 0;
    return true;
  }
  if (++s_missingPolls <= kReleaseDebouncePolls) return true;
  s_pressed = false;
  s_missingPolls = 0;
  return false;
}

uint32_t touch_interrupt_count() { return s_interrupts; }
uint32_t touch_i2c_error_count() { return s_i2cErrors; }
