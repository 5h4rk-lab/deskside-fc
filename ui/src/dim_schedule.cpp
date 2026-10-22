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

bool DimSchedule::in_night(int hour) const {
  if (start_ == end_) return false;
  const int h = normalise_hour(hour);
  return start_ < end_ ? (h >= start_ && h < end_) : (h >= start_ || h < end_);
}

uint8_t DimSchedule::brightness_at(int hour, bool recentlyTouched) const {
  if (recentlyTouched) return day_;
  return in_night(hour) ? dim_ : day_;
}
