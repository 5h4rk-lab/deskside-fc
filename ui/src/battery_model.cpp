#include "battery_model.h"

#include <math.h>

#include <algorithm>

namespace {

using namespace battery;

struct CurvePoint {
  int mv;
  int percent;
};

// Approximate open-circuit discharge curve of a LiPo cell (the usual
// published shape: steep at both ends, a long plateau around 3.7-4.0 V).
constexpr CurvePoint kCurve[] = {
    {3300, 0},  {3600, 10}, {3790, 20}, {3870, 30}, {3910, 40},  {3950, 50},
    {3980, 60}, {4020, 70}, {4080, 80}, {4110, 90}, {4150, 95}, {4200, 100},
};
constexpr int kCurveSize = static_cast<int>(sizeof(kCurve) / sizeof(kCurve[0]));

constexpr int64_t kRecentSeconds = 20;  // the trend compares 20 s averages

int round_div(int num, int den) { return (num * 2 + den) / (den * 2); }  // num, den > 0

}  // namespace

int lipo_percent(int mv) {
  if (mv <= kCurve[0].mv) return kCurve[0].percent;
  if (mv >= kCurve[kCurveSize - 1].mv) return kCurve[kCurveSize - 1].percent;
  for (int i = 1; i < kCurveSize; ++i) {
    const CurvePoint& hi = kCurve[i];
    if (mv > hi.mv) continue;
    const CurvePoint& lo = kCurve[i - 1];
    return lo.percent + round_div((mv - lo.mv) * (hi.percent - lo.percent), hi.mv - lo.mv);
  }
  return 100;
}

// ---------------------------------------------------------------- BatteryModel

int BatteryModel::median() const {
  if (rawCount_ == 1) return raw_[0];
  if (rawCount_ == 2) return (raw_[0] + raw_[1]) / 2;
  const int a = raw_[0];
  const int b = raw_[1];
  const int c = raw_[2];
  return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

void BatteryModel::reset_tracking() {
  smoothing_ = false;
  history_.clear();
  hasRisingSince_ = false;
  armed_ = true;
  inferred_ = false;
  confirmed_ = false;
  hasPercent_ = false;
}

// Average of the last kRecentSeconds minus the average of the oldest
// kRecentSeconds of the window: a rise of the whole window, robust to noise.
std::optional<double> BatteryModel::trend(int64_t now) const {
  if (history_.empty() || now - history_.front().t < kTrendMinSpanSeconds) return std::nullopt;
  const int64_t oldEnd = history_.front().t + kRecentSeconds;
  double oldSum = 0;
  double newSum = 0;
  int oldN = 0;
  int newN = 0;
  for (const Point& p : history_) {
    if (p.t < oldEnd) {
      oldSum += p.mv;
      ++oldN;
    }
    if (p.t > now - kRecentSeconds) {
      newSum += p.mv;
      ++newN;
    }
  }
  if (oldN == 0 || newN == 0) return std::nullopt;
  return newSum / newN - oldSum / oldN;
}

// Mean of the smoothed voltage over the last kRecentSeconds: steadier than
// the latest value, for judging new highs.
double BatteryModel::recent_mean(int64_t now) const {
  double sum = 0;
  int n = 0;
  for (const Point& p : history_) {
    if (p.t > now - kRecentSeconds) {
      sum += p.mv;
      ++n;
    }
  }
  return n > 0 ? sum / n : smoothed_;
}

bool BatteryModel::infer_charging(int64_t now) {
  const std::optional<double> tr = trend(now);
  const bool risingNow = tr && *tr >= kRisingTrendMv && smoothed_ >= kRisingMinMv;
  if (!risingNow) {
    hasRisingSince_ = false;
    armed_ = true;  // the last rise is over: the next one may start charging
  } else if (!hasRisingSince_) {
    hasRisingSince_ = true;
    risingSince_ = now;
  }
  // Noise makes short-lived rises; a charger's rise lasts.
  const bool rising = risingNow && now - risingSince_ >= kRisingConfirmSeconds;
  const double level = recent_mean(now);

  if (inferred_) {
    highest_ = std::max(highest_, level);
    if (level >= peakRef_ + kPeakStepMv) {  // still climbing
      peakRef_ = level;
      peakAt_ = now;
    }
    if (level >= startMv_ + kConfirmRiseMv) confirmed_ = true;
    const bool unplugged = smoothed_ <= highest_ - kChargingDropMv;
    const bool stalled = now - peakAt_ >= kStallSeconds;
    if (unplugged || stalled) {
      inferred_ = false;
      confirmed_ = false;
      armed_ = !risingNow;  // a new start needs a fresh rise
    }
  } else if (rising && armed_) {
    // Measured from the top of the recent readings, so the tail of the rise
    // that started it (or a noise peak) is not taken for further climbing.
    double top = smoothed_;
    for (const Point& p : history_) {
      if (p.t > now - kRecentSeconds) top = std::max(top, p.mv);
    }
    inferred_ = true;
    confirmed_ = false;
    armed_ = false;
    startMv_ = top;
    highest_ = top;
    peakRef_ = top;
    peakAt_ = now;
  }
  return inferred_;
}

void BatteryModel::add_sample(int millivolts, int64_t now) {
  const int64_t dt = hasLast_ ? now - lastT_ : 1;
  if (hasLast_ && dt <= 0 && dt >= -kDuplicateBackSeconds) {
    return;  // the same second again, or jitter: keep everything as it is
  }
  if (hasLast_ && (dt < 0 || dt > kGapResetSeconds)) {
    // Asleep, or the clock jumped: start again from this reading.
    rawCount_ = 0;
    reset_tracking();
  }
  hasLast_ = true;
  lastT_ = now;

  if (rawCount_ < 3) {
    raw_[rawCount_++] = millivolts;
  } else {
    raw_[0] = raw_[1];
    raw_[1] = raw_[2];
    raw_[2] = millivolts;
  }
  const int mv = median();
  if (mv < kAbsentBelowMv || mv > kAbsentAboveMv) {
    reset_tracking();
    status_ = PowerStatus{};
    return;
  }

  if (!smoothing_) {
    smoothed_ = mv;
    smoothing_ = true;
  } else {
    const double steps = static_cast<double>(std::max<int64_t>(dt, 1));
    const double alpha = 1.0 - pow(1.0 - kAlpha, steps);
    smoothed_ += alpha * (mv - smoothed_);
  }

  history_.push_back(Point{now, smoothed_});
  while (!history_.empty() && history_.front().t < now - kTrendWindowSeconds - kRecentSeconds) {
    history_.pop_front();
  }

  const bool inferred = infer_charging(now);
  const bool charging = external_.value_or(inferred);
  // The percent follows a charge up only once it is certain: a real charger
  // signal, or an inferred charge that has kept rising.
  const bool percentCharging = external_ ? *external_ : inferred && confirmed_;

  const int rounded = static_cast<int>(lround(smoothed_));
  const bool full = !charging && rounded >= kFullPackMv;
  const int raw = full ? 100 : lipo_percent(rounded);
  if (!hasPercent_) {
    percent_ = raw;
  } else if (percentCharging) {
    // Never down while charging; up only as far as the steadier 20 s average
    // allows, so a noise peak just before an unplug is not kept.
    const double steady = std::min(smoothed_, recent_mean(now));
    const int up = full ? 100 : lipo_percent(static_cast<int>(lround(steady)));
    percent_ = std::max(percent_, up);
  } else {
    percent_ = std::min(percent_, raw);  // never up while discharging
  }
  hasPercent_ = true;

  status_.present = true;
  status_.percent = percent_;
  status_.charging = charging;
  status_.millivolts = rounded;
}

void BatteryModel::set_external_charging(std::optional<bool> charging) {
  external_ = charging;
}

// ---------------------------------------------------------------- BatteryAlert

std::optional<Alert> BatteryAlert::update(const PowerStatus& s, bool suppressed, int64_t now) {
  if (!s.present) return std::nullopt;
  if (s.charging || s.percent > kAlertRearmPercent) {
    lowDone_ = false;
    criticalDone_ = false;
    return std::nullopt;
  }
  const bool critical = s.percent <= kAlertCriticalPercent && !criticalDone_;
  const bool low = !critical && s.percent <= kAlertLowPercent && !lowDone_;
  if (!critical && !low) return std::nullopt;
  if (suppressed || (busy_ && now < busyUntil_)) return std::nullopt;

  const Alert a = critical ? Alert{Alert::Level::Critical, kAlertCriticalText, kAlertCriticalMs}
                           : Alert{Alert::Level::Low, kAlertLowText, kAlertLowMs};
  lowDone_ = true;  // the very low one covers the low one
  if (critical) criticalDone_ = true;
  busy_ = true;
  busyUntil_ = now + static_cast<int64_t>((a.durationMs + 999) / 1000);
  return a;
}

// ---------------------------------------------------------------- levels and texts

BatteryLevel battery_level(const PowerStatus& s) {
  if (s.charging) return BatteryLevel::Charging;
  if (s.percent <= kCriticalPercent) return BatteryLevel::Critical;
  if (s.percent <= kLowPercent) return BatteryLevel::Low;
  return BatteryLevel::Normal;
}

int battery_segments(int percent) {
  const int n = (percent + 24) / 25;
  return std::min(4, std::max(1, n));
}

bool battery_shows_percent(const PowerStatus& s) { return s.present && s.percent <= kLowPercent; }

std::string battery_status_text(const PowerStatus& s) {
  if (!s.present) return "No battery detected";
  const std::string pct = std::to_string(s.percent) + "%";
  const char* dot = " \xC2\xB7 ";
  if (s.charging) return pct + dot + (s.percent >= kFullPercent ? "Fully charged" : "Charging");
  if (s.millivolts >= kFullPackMv) return pct + dot + "Full";
  if (s.percent <= kLowPercent) return "Low: " + pct + dot + "On battery";
  return pct + dot + "On battery";
}

std::string battery_voltage_text(const PowerStatus& s) {
  if (!s.present) return "";
  const int centi = (std::max(0, s.millivolts) + 5) / 10;
  const int frac = centi % 100;
  return std::to_string(centi / 100) + "." + (frac < 10 ? "0" : "") + std::to_string(frac) + " V";
}
