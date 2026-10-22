#include "battery_cal.h"

using namespace battery_cal;

namespace {

// a / b rounded to the nearest integer, for a >= 0 and b > 0.
long long round_div(long long a, long long b) { return (a * 2 + b) / (b * 2); }

}  // namespace

int battery_mv_from_adc(int adcMv) {
  if (adcMv <= 0) return 0;
  // adc * 3 / 0.990476 = adc * 3000000 / 990476
  return static_cast<int>(round_div(static_cast<long long>(adcMv) * 3000000LL, 990476LL));
}

int apply_battery_cal(int mv, int ppm) {
  if (mv <= 0) return 0;
  return static_cast<int>(round_div(static_cast<long long>(mv) * ppm, kPpmOne));
}

std::optional<int> battery_cal_ppm(int trueMv, int measuredMv) {
  if (measuredMv <= 0 || trueMv <= 0) return std::nullopt;
  const long long ppm = round_div(static_cast<long long>(trueMv) * kPpmOne, measuredMv);
  if (ppm < kMinPpm || ppm > kMaxPpm) return std::nullopt;
  return static_cast<int>(ppm);
}

CalCommand parse_cal_command(const std::string& arg) {
  CalCommand c;
  if (arg.empty()) {
    c.kind = CalCommand::Kind::Show;
    return c;
  }
  if (arg == "clear") {
    c.kind = CalCommand::Kind::Clear;
    return c;
  }
  if (arg.size() != 4 || arg[0] == '0') return c;
  int v = 0;
  for (char ch : arg) {
    if (ch < '0' || ch > '9') return c;
    v = v * 10 + (ch - '0');
  }
  if (v < kMinMv || v > kMaxMv) return c;
  c.kind = CalCommand::Kind::Set;
  c.mv = v;
  return c;
}

std::optional<int> load_battery_cal(const Prefs& prefs) {
  std::string raw;
  if (!prefs.get(kPrefKey, raw)) return std::nullopt;
  if (raw.empty() || raw.size() > 7) return std::nullopt;
  int v = 0;
  for (char ch : raw) {
    if (ch < '0' || ch > '9') return std::nullopt;
    v = v * 10 + (ch - '0');
  }
  if (v < kMinPpm || v > kMaxPpm) return std::nullopt;
  return v;
}

void save_battery_cal(Prefs& prefs, int ppm) { set_pref_int(prefs, kPrefKey, ppm); }

void clear_battery_cal(Prefs& prefs) { prefs.remove(kPrefKey); }
