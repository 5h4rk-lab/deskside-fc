#include <doctest/doctest.h>

#include "dim_schedule.h"

TEST_CASE("night window wraps past midnight") {
  const DimSchedule s(23, 7, 20);
  CHECK(s.brightness_at(2, false) == 20);
  CHECK(s.brightness_at(12, false) == 100);
  CHECK(s.brightness_at(2, true) == 100);
  CHECK(s.brightness_at(23, false) == 20);
  CHECK(s.brightness_at(0, false) == 20);
  CHECK(s.brightness_at(6, false) == 20);
  CHECK(s.brightness_at(7, false) == 100);  // end hour is exclusive
  CHECK(s.brightness_at(22, false) == 100);
}

TEST_CASE("non-wrapping night window") {
  const DimSchedule s(1, 5, 30);
  CHECK(s.brightness_at(0, false) == 100);
  CHECK(s.brightness_at(1, false) == 30);
  CHECK(s.brightness_at(4, false) == 30);
  CHECK(s.brightness_at(5, false) == 100);
  CHECK(s.brightness_at(23, false) == 100);
}

TEST_CASE("equal start and end means no night window") {
  const DimSchedule s(8, 8, 10);
  for (int h = 0; h < 24; ++h) CHECK(s.brightness_at(h, false) == 100);
}

TEST_CASE("out-of-range hours are normalised modulo 24") {
  const DimSchedule s(23, 7, 20);
  CHECK(s.brightness_at(25, false) == 20);   // 1
  CHECK(s.brightness_at(-1, false) == 20);   // 23
  CHECK(s.brightness_at(-13, false) == 100); // 11
  CHECK(s.brightness_at(31, false) == 100);  // 7
  CHECK(s.brightness_at(-48, false) == 20);  // 0
}

TEST_CASE("day brightness applies outside the window and when touched") {
  const DimSchedule s(23, 7, 20, 80);
  CHECK(s.brightness_at(12, false) == 80);
  CHECK(s.brightness_at(3, true) == 80);
  CHECK(s.brightness_at(3, false) == 20);
}

TEST_CASE("in_night: the window, wrapping past midnight, and no window when the hours are equal") {
  const DimSchedule wrap(23, 7, 20);
  CHECK(wrap.in_night(23));
  CHECK(wrap.in_night(0));
  CHECK(wrap.in_night(6));
  CHECK_FALSE(wrap.in_night(7));
  CHECK_FALSE(wrap.in_night(12));
  CHECK(wrap.in_night(-1));   // 23, normalised
  CHECK(wrap.in_night(47));
  const DimSchedule inside(1, 5, 20);
  CHECK(inside.in_night(1));
  CHECK_FALSE(inside.in_night(5));
  CHECK_FALSE(inside.in_night(0));
  const DimSchedule none(8, 8, 20);
  for (int h = 0; h < 24; ++h) CHECK_FALSE(none.in_night(h));
}
