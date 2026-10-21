// Battery maths, free of LVGL and of the clock (every call takes the time):
// LiPo voltage to percent, a smoothing model that turns raw ADC millivolts
// into a steady PowerStatus, the low-battery alert state machine and the
// texts and levels the UI shows. Shared with the ESP32 firmware.
#pragma once

#include <stdint.h>

#include <deque>
#include <optional>
#include <string>

#include "power_service.h"

namespace battery {

// --- BatteryModel thresholds -------------------------------------------------
// A median-of-3 sample outside this range means no battery (an open ADC input
// reads near zero) or a bad reading.
constexpr int kAbsentBelowMv = 2500;
constexpr int kAbsentAboveMv = 4500;
// Exponential smoothing per one-second sample; a longer gap weighs the new
// sample as that many samples would: alpha = 1 - (1 - kAlpha)^seconds.
constexpr double kAlpha = 0.2;
// A gap longer than this (sleep, or the clock jumping at NTP sync) restarts
// the model from the next sample, as does time going backwards.
constexpr int64_t kGapResetSeconds = 300;
// Trend: the average smoothed voltage of the last 20 s minus that of the 20 s
// kTrendWindowSeconds earlier, in mV; needs kTrendMinSpanSeconds of history.
constexpr int64_t kTrendWindowSeconds = 60;
constexpr int64_t kTrendMinSpanSeconds = 30;
// Charging starts when the trend rises by at least kRisingTrendMv while the
// smoothed voltage is at least kRisingMinMv (the step a charger causes), or
// when the pack sits at kFullPackMv or more and is not falling (a full pack on
// the charger holds about 4.2 V). A trend at or below kFallingTrendMv counts
// as falling.
constexpr int kRisingTrendMv = 15;
constexpr int kRisingMinMv = 3900;
constexpr int kFullPackMv = 4180;
constexpr int kFallingTrendMv = -8;
// The rise must last this long (noise makes short-lived ones).
constexpr int64_t kRisingConfirmSeconds = 15;
// Once charging it stays so for at least kChargingHoldSeconds after the
// condition last held, and afterwards while the voltage keeps rising over
// kStallWindowSeconds (or the pack is full). It ends at once when the voltage
// falls kChargingDropMv below its peak since charging began (unplugging
// removes the charge current's rise across the cell's resistance).
constexpr int64_t kChargingHoldSeconds = 20;
constexpr int kChargingDropMv = 30;
constexpr int64_t kStallWindowSeconds = 600;

// --- Display levels ---------------------------------------------------------
constexpr int kLowPercent = 20;       // amber icon and the percent text
constexpr int kCriticalPercent = 10;  // warn-coloured icon
constexpr int kFullPercent = 99;      // charging at or above: "Fully charged"

// --- Low-battery banner -----------------------------------------------------
constexpr int kAlertLowPercent = 15;
constexpr int kAlertCriticalPercent = 5;
constexpr int kAlertRearmPercent = 25;  // above this (or charging) both alerts re-arm
constexpr uint32_t kAlertLowMs = 8000;
constexpr uint32_t kAlertCriticalMs = 10000;

constexpr const char* kAlertLowText = "Battery low. Plug in the charger.";
constexpr const char* kAlertCriticalText = "Battery very low. Plug in the charger now.";

}  // namespace battery

// Approximate state of charge of a single LiPo cell at rest, from an
// 12-point piecewise-linear discharge curve of the published kind
// (4.20 V = 100 %, 3.95 V = 50 %, 3.60 V = 10 %, 3.30 V = 0 %), rounded to
// the nearest percent and clamped outside the table. Under load or on the
// charger the reading is offset by the cell's resistance: good enough for an
// icon, not a fuel gauge.
int lipo_percent(int millivolts);

// Turns raw millivolt samples (about one a second) into a stable status:
// median-of-3 against single spikes, exponential smoothing, a percent that
// only moves one way per direction (never up while discharging, never down
// while charging; it follows the voltage again when the direction changes),
// and charging inferred from the voltage (see the battery:: constants) unless
// the firmware knows better (set_external_charging).
class BatteryModel {
 public:
  void add_sample(int millivolts, int64_t epochSeconds);
  PowerStatus status() const { return status_; }

  // A real charger-status signal: true/false overrides the inference,
  // nullopt goes back to inferring from the voltage.
  void set_external_charging(std::optional<bool> charging);

 private:
  struct Point {
    int64_t t;
    double mv;
  };
  void reset_tracking();
  std::optional<double> trend(int64_t now) const;
  std::optional<double> long_trend(int64_t now) const;
  bool infer_charging(int64_t now);
  int median() const;

  PowerStatus status_;
  std::optional<bool> external_;
  int raw_[3] = {0, 0, 0};
  int rawCount_ = 0;
  bool hasLast_ = false;
  int64_t lastT_ = 0;
  bool smoothing_ = false;  // smoothed_ holds a value
  double smoothed_ = 0;
  std::deque<Point> history_;  // smoothed values of the last trend window
  std::deque<Point> coarse_;   // one smoothed value a minute, kStallWindowSeconds back
  bool inferred_ = false;      // charging, as inferred
  int64_t condAt_ = 0;         // last time the charging condition held
  bool hasRisingSince_ = false;
  int64_t risingSince_ = 0;    // the rising trend has held since then
  double peak_ = 0;            // highest smoothed voltage since charging began
  bool hasPercent_ = false;
  int percent_ = 0;
  bool percentCharging_ = false;  // direction the percent was last held in
};

// The low-battery banner, as a pure state machine fed with the status about
// once a second. Low at kAlertLowPercent, very low at kAlertCriticalPercent,
// never while charging; each at most once per discharge cycle (both re-arm
// when charging starts or the percent rises above kAlertRearmPercent). Shown
// at boot too when already low. While `suppressed` (the goal celebration) a
// due alert waits and comes when the suppression ends, if still due. An alert
// never interrupts the previous one: it waits until that one's time is over.
struct Alert {
  enum class Level { Low, Critical };
  Level level;
  const char* text;
  uint32_t durationMs;
};

class BatteryAlert {
 public:
  std::optional<Alert> update(const PowerStatus& status, bool suppressed, int64_t nowSeconds);

 private:
  bool lowDone_ = false;
  bool criticalDone_ = false;
  int64_t busyUntil_ = 0;
  bool busy_ = false;
};

// How the battery icon looks.
enum class BatteryLevel { Normal, Low, Critical, Charging };
BatteryLevel battery_level(const PowerStatus& s);
// Filled segments of the 4-segment icon: ceil(percent / 25), at least 1.
int battery_segments(int percent);
// The percent text beside the icon ("18%"), only at kLowPercent or below.
bool battery_shows_percent(const PowerStatus& s);

// Settings: "78% · On battery", "55% · Charging", "100% · Fully charged",
// "Low: 18% · On battery", "No battery detected".
std::string battery_status_text(const PowerStatus& s);
// "3.92 V" (two decimals, rounded); empty without a battery.
std::string battery_voltage_text(const PowerStatus& s);
