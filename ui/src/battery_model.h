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
// Timestamps: a sample in the same second as the last one, or up to
// kDuplicateBackSeconds before it (1 Hz jitter, SNTP slewing, a host sampling
// faster than 1 Hz), is ignored and the model keeps its state. A jump further
// back, or a gap longer than kGapResetSeconds (sleep, the first NTP sync),
// restarts the model from that sample.
constexpr int64_t kDuplicateBackSeconds = 5;
constexpr int64_t kGapResetSeconds = 300;
// Trend: the average smoothed voltage of the last 20 s minus that of the 20 s
// kTrendWindowSeconds earlier, in mV; needs kTrendMinSpanSeconds of history.
constexpr int64_t kTrendWindowSeconds = 60;
constexpr int64_t kTrendMinSpanSeconds = 30;
// Charging is inferred only from evidence of a rise: the trend at least
// kRisingTrendMv for kRisingConfirmSeconds in a row (noise makes short-lived
// rises; 40 s gave no false start in 1000 simulated hours of +-25 mV noise,
// and a charger is still recognised within about a minute) with the smoothed
// voltage at least kRisingMinMv (low enough to
// recognise a deeply discharged pack on the charger). After charging ends a
// new start needs a fresh rise (the trend must first drop below the mark).
constexpr int kRisingTrendMv = 15;
constexpr int kRisingMinMv = 3500;
constexpr int64_t kRisingConfirmSeconds = 40;
// Once inferred, charging lasts while the voltage (its 20 s average, steadier
// than single readings) keeps making new highs: a
// constant-current charge climbs a few mV a minute, so it must exceed its
// last high by kPeakStepMv within every kStallSeconds (counted from the
// start, then from each new high). Without that it has stalled: a load easing
// off rather than a charger, or the charger has finished. It ends at once on a
// fall of kChargingDropMv below the highest voltage since it began (unplugging
// removes the charge current's rise across the cell's resistance).
constexpr int kPeakStepMv = 3;
constexpr int64_t kStallSeconds = 150;
constexpr int kChargingDropMv = 30;
// The percent only follows the voltage up once the inferred charge has risen
// kConfirmRiseMv above where it started, so a false start that stalls never
// lifts it.
constexpr int kConfirmRiseMv = 6;
// At or above this a pack that is not charging reads 100 % and "Full". Voltage
// alone cannot tell a full pack on USB from one just unplugged, so a flat
// voltage up here is never taken for charging.
constexpr int kFullPackMv = 4180;

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

// What the UI shows for the measured status `s` under the user's choice:
// Auto: `s` itself. None: no battery (PowerStatus{}). Installed: `s` when it
// has a battery; when the model sees none (a reading below 2.5 V or above
// 4.5 V) an empty, not charging battery (0 %), so a dead or unplugged pack
// warns instead of vanishing.
PowerStatus apply_battery_choice(const PowerStatus& s, BatteryChoice choice);

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
// and charging inferred from the voltage (see the battery:: constants).
//
// Firmware: the voltage inference is a fallback. Whenever the firmware has
// real information (USB VBUS present, the charger's CHRG/STDBY pin), it must
// pass it through set_external_charging(); that overrides the inference.
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
  double recent_mean(int64_t now) const;
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
  bool hasRisingSince_ = false;
  int64_t risingSince_ = 0;    // the rising trend has held since then
  bool armed_ = true;          // a rise may start charging (false until the last one is over)
  bool inferred_ = false;      // charging, as inferred
  double startMv_ = 0;         // smoothed voltage when it began
  double highest_ = 0;         // highest smoothed voltage since it began
  double peakRef_ = 0;         // the last high that counted (kPeakStepMv steps)
  int64_t peakAt_ = 0;         // when it was reached
  bool confirmed_ = false;     // risen kConfirmRiseMv since it began
  bool hasPercent_ = false;
  int percent_ = 0;
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

// Settings: "78% · On battery", "55% · Charging", "100% · Fully charged"
// (charging at kFullPercent or more), "100% · Full" (not charging at
// kFullPackMv or more), "Low: 18% · On battery", "No battery detected".
std::string battery_status_text(const PowerStatus& s);
// "3.92 V" (two decimals, rounded); empty without a battery.
std::string battery_voltage_text(const PowerStatus& s);
