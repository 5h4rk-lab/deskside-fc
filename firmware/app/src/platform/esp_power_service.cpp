#include "platform/esp_power_service.h"

#include <Arduino.h>
#include <HWCDC.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include "battery_cal.h"
#include "board/pins.h"

namespace {

int64_t monotonic_s() { return esp_timer_get_time() / 1000000; }

}  // namespace

bool power_key_down() { return gpio_get_level(static_cast<gpio_num_t>(pins::kPowerKey)) == 0; }

void power_hold_cut() { gpio_set_level(static_cast<gpio_num_t>(pins::kPowerHold), 0); }

void EspPowerService::begin(const Prefs& prefs) {
  // The key has a 10k pull-up to 3V3 on the board; the internal one is harmless.
  gpio_config_t key = {};
  key.pin_bit_mask = 1ULL << pins::kPowerKey;
  key.mode = GPIO_MODE_INPUT;
  key.pull_up_en = GPIO_PULLUP_ENABLE;
  key.pull_down_en = GPIO_PULLDOWN_DISABLE;
  key.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&key);
  pinMode(pins::kBatteryAdc, INPUT);
  const std::optional<int> cal = load_battery_cal(prefs);
  calPpm_ = cal ? *cal : 0;
  sample();
  Serial.printf("[power] battery ADC GPIO%d (pack / 3), %s; power key GPIO%d %s; hold GPIO%d %s\n",
                pins::kBatteryAdc,
                cal ? "calibrated (batt.cal)" : "factory formula (no calibration: :cal-battery <mV>)",
                pins::kPowerKey, power_key_down() ? "DOWN" : "up", pins::kPowerHold,
                gpio_get_level(static_cast<gpio_num_t>(pins::kPowerHold)) ? "high" : "LOW");
}

int EspPowerService::read_adc_mv(int samples) {
  uint32_t sum = 0;
  for (int i = 0; i < samples; ++i) sum += analogReadMilliVolts(pins::kBatteryAdc);
  return static_cast<int>((sum + samples / 2) / samples);
}

int EspPowerService::read_factory_mv(int samples) { return battery_mv_from_adc(read_adc_mv(samples)); }

void EspPowerService::sample() {
  const int adc = read_adc_mv(kSamples);
  const int factory = battery_mv_from_adc(adc);
  const int ppm = calPpm_.load();
  int mv = ppm > 0 ? apply_battery_cal(factory, ppm) : factory;
#if APP_DIAG
  if (resetModel_.exchange(false)) model_ = BatteryModel();
  if (injectMv_.load() > 0) mv = injectMv_.load();
#endif
  model_.add_sample(mv, monotonic_s());
  const PowerStatus s = model_.status();
  portENTER_CRITICAL(&mux_);
  shared_ = s;
  lastAdcMv_ = adc;
  lastFactoryMv_ = factory;
  lastMv_ = mv;
  portEXIT_CRITICAL(&mux_);
}

PowerStatus EspPowerService::status() {
  portENTER_CRITICAL(&mux_);
  const PowerStatus s = shared_;
  portEXIT_CRITICAL(&mux_);
  return s;
}

void EspPowerService::set_calibration(std::optional<int> ppm) { calPpm_ = ppm ? *ppm : 0; }

std::optional<int> EspPowerService::calibration() const {
  const int ppm = calPpm_.load();
  return ppm > 0 ? std::optional<int>(ppm) : std::nullopt;
}

void EspPowerService::print_status(BatteryChoice choice) const {
  portENTER_CRITICAL(&mux_);
  const PowerStatus s = shared_;
  const int adc = lastAdcMv_;
  const int factory = lastFactoryMv_;
  const int mv = lastMv_;
  portEXIT_CRITICAL(&mux_);
  static const char* const kChoice[] = {"Auto", "Installed", "None"};
  const int ppm = calPpm_.load();
  char cal[24];
  if (ppm > 0) {
    snprintf(cal, sizeof(cal), "x%d.%06d", ppm / 1000000, ppm % 1000000);
  } else {
    snprintf(cal, sizeof(cal), "none");
  }
  Serial.printf(
      "[power] adc %d mV -> factory %d mV, cal %s -> %d mV%s | model: %s %d%% %s, %d mV | setting %s | key %s, hold "
      "%s, usb data %s\n",
      adc, factory, cal, mv,
#if APP_DIAG
      injectMv_.load() > 0 ? " (INJECTED)" : "",
#else
      "",
#endif
      s.present ? "battery" : "no battery", s.percent, s.charging ? "charging (inferred)" : "not charging",
      s.millivolts, kChoice[static_cast<int>(choice) % 3], power_key_down() ? "DOWN" : "up",
      gpio_get_level(static_cast<gpio_num_t>(pins::kPowerHold)) ? "high" : "LOW",
      HWCDC::isPlugged() ? "connected" : "not seen");
}

#if APP_DIAG
void EspPowerService::diag_inject_mv(int mv) {
  injectMv_ = mv > 0 ? mv : 0;
  resetModel_ = true;
}
#endif
