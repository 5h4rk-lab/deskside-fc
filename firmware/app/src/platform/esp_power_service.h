// The board's PowerService: the battery voltage on ADC GPIO8 (the pack
// divided by 3, schematic R5 200k / R11 100k), averaged, calibrated and
// smoothed by the shared BatteryModel; plus the power key (GPIO6) and the
// power hold (GPIO7) as raw pins.
//
// Threads: sample() and read_factory_mv() run in the Arduino loop task (the
// only user of the ADC); status() may be called from any task (the LVGL task
// calls it once a second) and answers a copy kept under a spinlock. The model
// itself is touched only by the loop task.
//
// What the board cannot tell (schematic, see the hardware notes "Measured on
// the board"): there is no VBUS sense pin and the charger's STAT pin only
// drives the red LED, so charging is inferred from the voltage (BatteryModel).
// USB data presence (usb_serial_jtag_is_connected) is logged for information
// only: a wall charger gives power without data, a PC gives both.
#pragma once

#include <stdint.h>

#include <atomic>
#include <optional>

#include <freertos/FreeRTOS.h>

#include "app_diag.h"
#include "battery_model.h"
#include "power_service.h"
#include "prefs.h"

class EspPowerService : public PowerService {
 public:
  static constexpr int kSamples = 16;          // ADC readings averaged per second
  static constexpr uint32_t kSampleMs = 1000;  // how often the loop task samples

  // Sets up the ADC pin and the key input, reads the calibration (batt.cal)
  // and takes the first reading, so the status is ready before the UI starts.
  void begin(const Prefs& prefs);
  // Loop task, every kSampleMs: kSamples readings averaged, into the model.
  void sample();
  // Any task.
  PowerStatus status() override;

  // Loop task: a fresh average of `samples` readings by the factory formula
  // (no calibration), for :cal-battery.
  int read_factory_mv(int samples = 64);
  // Any task. nullopt: the factory formula.
  void set_calibration(std::optional<int> ppm);
  std::optional<int> calibration() const;

  // The [power] line: ADC, factory and calibrated mV, the model's status,
  // key, hold pin, USB data presence. Any task.
  void print_status(BatteryChoice choice) const;

#if APP_DIAG
  // Diagnostics: the model gets `mv` instead of the ADC (0: the ADC again).
  // The model restarts at each change so the new value shows at once.
  void diag_inject_mv(int mv);
#endif

 private:
  int read_adc_mv(int samples);

  BatteryModel model_;  // loop task only
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  PowerStatus shared_;     // under mux_
  int lastAdcMv_ = 0;      // under mux_: last average at the pin
  int lastFactoryMv_ = 0;  // under mux_
  int lastMv_ = 0;         // under mux_: calibrated, what the model got
  std::atomic<int> calPpm_{0};  // 0: none
#if APP_DIAG
  std::atomic<int> injectMv_{0};
  std::atomic<bool> resetModel_{false};
#endif
};

// GPIO6, active low (10k pull-up on the board): true while the power key is down.
bool power_key_down();
// Drives the power hold (GPIO7) low: on battery the board loses power as soon
// as the power key is up. On USB the board stays powered (VCC is fed from VBUS
// through D3 whatever GPIO7 does). The single place that does this.
void power_hold_cut();
