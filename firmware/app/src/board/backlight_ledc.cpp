// LEDC backlight. Frequency, resolution and pin from the Waveshare demo
// Arduino/examples/LVGL_Arduino/LCD_Backlight (vendor: Frequency 20000,
// Resolution 10, GPIO5, the vendor writes 1024 for 100 %).
//
// The LED brightness is linear in the PWM duty but the eye is not: duty
// = (percent/100)^2.2 gives visually even steps from 10 % to 100 % (a plain
// linear mapping looks almost fully bright above 40 %).
#include "backlight_ledc.h"

#include <Arduino.h>
#include <math.h>

#include "pins.h"

namespace {
constexpr uint32_t kBacklightHz = 20000;
constexpr uint8_t kBacklightBits = 10;
constexpr uint32_t kBacklightFull = 1u << kBacklightBits;  // 1024 = 100 %
constexpr float kGamma = 2.2f;
}  // namespace

uint16_t backlight_duty_for_percent(uint8_t percent) {
  if (percent == 0) return 0;
  if (percent >= 100) return kBacklightFull;
  const float duty = powf(percent / 100.0f, kGamma) * kBacklightFull;
  const uint16_t d = (uint16_t)(duty + 0.5f);
  return d < 1 ? 1 : d;
}

LedcBacklight::LedcBacklight() {
  ok_ = ledcAttach(pins::kLcdBacklight, kBacklightHz, kBacklightBits);
  if (!ok_) {
    Serial.printf("[backlight] ERROR: ledcAttach(GPIO%d, %lu Hz, %u bit) failed\n", pins::kLcdBacklight,
                  (unsigned long)kBacklightHz, kBacklightBits);
    return;
  }
  ledcWrite(pins::kLcdBacklight, 0);
}

void LedcBacklight::set_percent(uint8_t percent) {
  if (!ok_) return;
  ledcWrite(pins::kLcdBacklight, backlight_duty_for_percent(percent));
}
