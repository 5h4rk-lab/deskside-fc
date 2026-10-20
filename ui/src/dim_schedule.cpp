#include "dim_schedule.h"

namespace {

int normalise_hour(int hour) {
  const int h = hour % 24;
  return h < 0 ? h + 24 : h;
}

}  // namespace

DimSchedule::DimSchedule(int startHour, int endHour, uint8_t dimPercent, uint8_t dayPercent)
    : start_(normalise_hour(startHour)),
      end_(normalise_hour(endHour)),
      dim_(dimPercent),
      day_(dayPercent) {}

uint8_t DimSchedule::brightness_at(int hour, bool recentlyTouched) const {
  if (recentlyTouched || start_ == end_) return day_;
  const int h = normalise_hour(hour);
  const bool night = start_ < end_ ? (h >= start_ && h < end_) : (h >= start_ || h < end_);
  return night ? dim_ : day_;
}
