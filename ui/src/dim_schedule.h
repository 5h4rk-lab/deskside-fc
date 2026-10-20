// Night-time dimming. Pure: the caller supplies the local hour.
#pragma once

#include <stdint.h>

class DimSchedule {
 public:
  // The night window runs from `startHour` (inclusive) to `endHour`
  // (exclusive) and wraps past midnight when startHour > endHour; equal hours
  // mean no night window. Hours are normalised modulo 24.
  DimSchedule(int startHour, int endHour, uint8_t dimPercent, uint8_t dayPercent = 100);

  // `dimPercent` inside the night window unless `recentlyTouched`, otherwise
  // `dayPercent`. `hour` is normalised modulo 24.
  uint8_t brightness_at(int hour, bool recentlyTouched) const;

 private:
  int start_;
  int end_;
  uint8_t dim_;
  uint8_t day_;
};
