// Simulator batteries: a scripted one (MockPower: --battery, the b key) and
// one fed with raw millivolts through the real BatteryModel (--battery-mv).
#pragma once

#include <stdint.h>

#include <string>

#include "battery_model.h"
#include "power_service.h"

class MockPower : public PowerService {
 public:
  MockPower();
  PowerStatus status() override { return status_; }
  void set(const PowerStatus& s) { status_ = s; }

  // The b key: the next of 78 % discharging, 40 %, 18 % (low), 8 %
  // (critical), 55 % charging, 100 % charging (full), no battery; then round again.
  const PowerStatus& next_demo();

  // --battery: "PERCENT" (on battery), "PERCENT,charging" or "none". The
  // voltage is a plausible one for the percent. False for anything else.
  static bool parse(const std::string& arg, PowerStatus& out);
  // One line for the terminal: "battery 18% on battery (3.77 V)", "no battery".
  static std::string describe(const PowerStatus& s);
  // A resting voltage for `percent` on the LiPo curve (the lowest voltage
  // that reads as that percent), a little higher while charging.
  static int millivolts_for(int percent, bool charging);

 private:
  PowerStatus status_;
  int demo_ = 0;
};

// --battery-mv: a pretend ADC. The main loop feeds sample() once a second;
// status() answers from the model, as the firmware's service would.
class ModelPower : public PowerService {
 public:
  explicit ModelPower(int millivolts) : mv_(millivolts) {}
  PowerStatus status() override { return model_.status(); }
  void sample(int64_t now) { model_.add_sample(mv_, now); }
  void set_millivolts(int mv) { mv_ = mv; }
  int millivolts() const { return mv_; }

 private:
  BatteryModel model_;
  int mv_;
};
