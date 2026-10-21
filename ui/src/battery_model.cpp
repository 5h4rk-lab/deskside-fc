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
constexpr int64_t kCoarseStepSeconds = 60;

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
  coarse_.clear();
  inferred_ = false;
  peak_ = 0;
  hasRisingSince_ = false;
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

// The smoothed voltage now minus about kStallWindowSeconds ago.
std::optional<double> BatteryModel::long_trend(int64_t now) const {
  std::optional<double> then;
  for (const Point& p : coarse_) {
    if (p.t <= now - kStallWindowSeconds) then = p.mv;
  }
  if (!then) return std::nullopt;
  return smoothed_ - *then;
}

bool BatteryModel::infer_charging(int64_t now) {
  const std::optional<double> tr = trend(now);
  const bool falling = tr && *tr <= kFallingTrendMv;
  const bool risingNow = tr && *tr >= kRisingTrendMv && smoothed_ >= kRisingMinMv;
  if (!risingNow) {
    hasRisingSince_ = false;
  } else if (!hasRisingSince_) {
    hasRisingSince_ = true;
    risingSince_ = now;
  }
  // Noise makes short-lived rises; a charger's rise lasts.
  const bool rising = risingNow && now - risingSince_ >= kRisingConfirmSeconds;
  const bool full = smoothed_ >= kFullPackMv && !falling;
  if (inferred_ && smoothed_ <= peak_ - kChargingDropMv) {
    // Unplugged: forget the climb that led here, or it would start again.
    inferred_ = false;
    peak_ = 0;
    history_.clear();
    return false;
  }
  if (rising || full) {
    if (!inferred_) peak_ = smoothed_;
    inferred_ = true;
    condAt_ = now;
  } else if (inferred_ && now - condAt_ >= kChargingHoldSeconds) {
    const std::optional<double> lt = long_trend(now);
    if (lt && *lt <= 0) inferred_ = false;  // stalled: it was not a charger
  }
  if (inferred_) peak_ = std::max(peak_, smoothed_);
  return inferred_;
}

void BatteryModel::add_sample(int millivolts, int64_t now) {
  const int64_t dt = hasLast_ ? now - lastT_ : 1;
  if (hasLast_ && (dt <= 0 || dt > kGapResetSeconds)) {
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
  if (coarse_.empty() || now - coarse_.back().t >= kCoarseStepSeconds) {
    coarse_.push_back(Point{now, smoothed_});
  }
  while (coarse_.size() > 2 && coarse_[1].t <= now - kStallWindowSeconds) coarse_.pop_front();

  const bool inferred = infer_charging(now);
  const bool charging = external_.value_or(inferred);

  const int rounded = static_cast<int>(lround(smoothed_));
  const int raw = lipo_percent(rounded);
  if (!hasPercent_ || charging != percentCharging_) {
    percent_ = raw;  // first reading or a new direction: follow the voltage
  } else if (charging) {
    percent_ = std::max(percent_, raw);
  } else {
    percent_ = std::min(percent_, raw);
  }
  hasPercent_ = true;
  percentCharging_ = charging;

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
  if (s.percent <= kLowPercent) return "Low: " + pct + dot + "On battery";
  return pct + dot + "On battery";
}

std::string battery_voltage_text(const PowerStatus& s) {
  if (!s.present) return "";
  const int centi = (std::max(0, s.millivolts) + 5) / 10;
  const int frac = centi % 100;
  return std::to_string(centi / 100) + "." + (frac < 10 ? "0" : "") + std::to_string(frac) + " V";
}
