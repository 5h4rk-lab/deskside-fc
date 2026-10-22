// Battery voltage from the ADC, and its calibration against a multimeter.
// Pure (shared with the firmware, tested in the simulator).
//
// The board divides the pack voltage by 3 (200k over 100k, schematic) into
// ADC GPIO8; the vendor demo multiplies the ADC millivolts by 3 and divides by
// 0.990476. That is the factory formula here. A calibration is one scale
// factor in parts per million, stored in the prefs under batt.cal, set by the
// serial command `:cal-battery <mV>` (the multimeter's reading of the pack,
// 3000 to 4400 mV) and removed by `:cal-battery clear`.
#pragma once

#include <optional>
#include <string>

#include "prefs.h"

namespace battery_cal {

constexpr const char* kPrefKey = "batt.cal";
constexpr int kMinMv = 3000;
constexpr int kMaxMv = 4400;
constexpr int kPpmOne = 1000000;
// A calibration may move the reading by at most 20 percent; anything more is
// a typo or a wrong measurement.
constexpr int kMinPpm = 800000;
constexpr int kMaxPpm = 1200000;

}  // namespace battery_cal

// The factory formula: pack millivolts from ADC millivolts, rounded.
int battery_mv_from_adc(int adcMv);

// `mv` scaled by `ppm` (parts per million of 1), rounded.
int apply_battery_cal(int mv, int ppm);

// The scale that turns `measuredMv` (the factory reading) into `trueMv`, or
// nullopt when it is outside kMinPpm..kMaxPpm or measuredMv is not positive.
std::optional<int> battery_cal_ppm(int trueMv, int measuredMv);

// The argument of `:cal-battery`: "" shows the state, "clear" removes the
// calibration, 3000..4400 (digits only, at most 4 of them) sets it.
struct CalCommand {
  enum class Kind { Invalid, Show, Clear, Set };
  Kind kind = Kind::Invalid;
  int mv = 0;
};
CalCommand parse_cal_command(const std::string& arg);

// The stored calibration, or nullopt when there is none or it is malformed or
// outside kMinPpm..kMaxPpm (then the factory formula applies).
std::optional<int> load_battery_cal(const Prefs& prefs);
void save_battery_cal(Prefs& prefs, int ppm);
void clear_battery_cal(Prefs& prefs);
