#include "mock_power.h"

#include <algorithm>

namespace {

constexpr int kChargingRiseMv = 40;  // the charge current lifts the reading a little
constexpr int kMaxMv = 4200;

struct DemoState {
  int percent;  // -1: no battery
  bool charging;
};

const DemoState kDemo[] = {{78, false}, {40, false}, {18, false}, {8, false},
                           {55, true},  {100, true}, {-1, false}};
constexpr int kDemoCount = static_cast<int>(sizeof(kDemo) / sizeof(kDemo[0]));

PowerStatus make(int percent, bool charging) {
  return PowerStatus{true, percent, charging, MockPower::millivolts_for(percent, charging)};
}

bool all_digits(const std::string& s) {
  if (s.empty() || s.size() > 3) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

MockPower::MockPower() : status_(make(kDemo[0].percent, kDemo[0].charging)) {}

const PowerStatus& MockPower::next_demo() {
  demo_ = (demo_ + 1) % kDemoCount;
  const DemoState& d = kDemo[demo_];
  status_ = d.percent < 0 ? PowerStatus{} : make(d.percent, d.charging);
  return status_;
}

bool MockPower::parse(const std::string& arg, PowerStatus& out) {
  if (arg == "none") {
    out = PowerStatus{};
    return true;
  }
  const size_t comma = arg.find(',');
  const std::string number = arg.substr(0, comma);
  bool charging = false;
  if (comma != std::string::npos) {
    if (arg.substr(comma + 1) != "charging") return false;
    charging = true;
  }
  if (!all_digits(number)) return false;
  const int percent = std::stoi(number);
  if (percent > 100) return false;
  out = make(percent, charging);
  return true;
}

std::string MockPower::describe(const PowerStatus& s) {
  if (!s.present) return "no battery";
  return "battery " + std::to_string(s.percent) + "% " + (s.charging ? "charging" : "on battery") + " (" +
         battery_voltage_text(s) + ")";
}

int MockPower::millivolts_for(int percent, bool charging) {
  int mv = 3000;
  while (mv < kMaxMv && lipo_percent(mv) < percent) ++mv;
  return charging ? std::min(kMaxMv, mv + kChargingRiseMv) : mv;
}
