#include "timezone.h"

#include "format.h"

namespace {

const int kSunday = 0;

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
int64_t days_from_civil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = floor_div(y, 400);
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Civil year of a day count since 1970-01-01.
int64_t civil_year(int64_t days) {
  const int64_t z = days + 719468;
  const int64_t era = floor_div(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  return yoe + era * 400 + (mp >= 10 ? 1 : 0);
}

// 0 = Sunday .. 6 = Saturday; 1970-01-01 was a Thursday.
int weekday_of(int64_t days) {
  int64_t d = (days + 4) % 7;
  if (d < 0) d += 7;
  return static_cast<int>(d);
}

bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int64_t y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : kDays[m - 1];
}

// Day count of the nth (1-based) `wd` of a month.
int64_t nth_weekday(int64_t y, int m, int wd, int n) {
  const int64_t first = days_from_civil(y, m, 1);
  const int shift = (wd - weekday_of(first) + 7) % 7;
  return first + shift + 7 * (n - 1);
}

// Day count of the last `wd` of a month.
int64_t last_weekday(int64_t y, int m, int wd) {
  const int64_t last = days_from_civil(y, m, days_in_month(y, m));
  return last - (weekday_of(last) - wd + 7) % 7;
}

// UTC instant of a local wall-clock time at the given UTC offset.
int64_t at_local(int64_t day, int hour, int offsetSeconds) {
  return day * 86400 + hour * 3600 - offsetSeconds;
}

bool dst_active(Dst rule, int stdSeconds, int64_t utc) {
  if (rule == Dst::None) return false;
  const int64_t year = civil_year(floor_div(utc, 86400));
  const int dstSeconds = stdSeconds + 3600;
  switch (rule) {
    case Dst::Eu: {
      // Last Sunday of March to last Sunday of October, both at 01:00 UTC.
      const int64_t start = at_local(last_weekday(year, 3, kSunday), 1, 0);
      const int64_t end = at_local(last_weekday(year, 10, kSunday), 1, 0);
      return utc >= start && utc < end;
    }
    case Dst::Us: {
      // Second Sunday of March 02:00 standard to first Sunday of November 02:00 daylight.
      const int64_t start = at_local(nth_weekday(year, 3, kSunday, 2), 2, stdSeconds);
      const int64_t end = at_local(nth_weekday(year, 11, kSunday, 1), 2, dstSeconds);
      return utc >= start && utc < end;
    }
    case Dst::Au: {
      // Southern hemisphere: DST runs from October of one year to April of the next.
      const int64_t end = at_local(nth_weekday(year, 4, kSunday, 1), 3, dstSeconds);
      const int64_t start = at_local(nth_weekday(year, 10, kSunday, 1), 2, stdSeconds);
      return utc < end || utc >= start;
    }
    case Dst::Nz: {
      const int64_t end = at_local(nth_weekday(year, 4, kSunday, 1), 3, dstSeconds);
      const int64_t start = at_local(last_weekday(year, 9, kSunday), 2, stdSeconds);
      return utc < end || utc >= start;
    }
    case Dst::None:
      break;
  }
  return false;
}

}  // namespace

const std::vector<Zone>& zones() {
  static const std::vector<Zone> kZones = {
      {"UTC", "UTC", 0, Dst::None},
      {"Europe/London", "London, Dublin, Lisbon", 0, Dst::Eu},
      {"Europe/Paris", "Paris, Berlin, Madrid, Rome", 60, Dst::Eu},
      {"Europe/Athens", "Athens, Helsinki, Kyiv", 120, Dst::Eu},
      {"Europe/Istanbul", "Istanbul", 180, Dst::None},
      {"Europe/Moscow", "Moscow", 180, Dst::None},
      {"Africa/Lagos", "Lagos", 60, Dst::None},
      {"Africa/Johannesburg", "Johannesburg, Cairo", 120, Dst::None},
      {"Africa/Nairobi", "Nairobi", 180, Dst::None},
      {"Asia/Dubai", "Dubai", 240, Dst::None},
      {"Asia/Karachi", "Karachi", 300, Dst::None},
      {"Asia/Kolkata", "India", 330, Dst::None},
      {"Asia/Dhaka", "Dhaka", 360, Dst::None},
      {"Asia/Bangkok", "Bangkok, Jakarta", 420, Dst::None},
      {"Asia/Shanghai", "Beijing, Singapore, Perth", 480, Dst::None},
      {"Asia/Tokyo", "Tokyo, Seoul", 540, Dst::None},
      {"Australia/Sydney", "Sydney, Melbourne", 600, Dst::Au},
      {"Pacific/Auckland", "Auckland", 720, Dst::Nz},
      {"America/Sao_Paulo", "Sao Paulo, Buenos Aires", -180, Dst::None},
      {"America/New_York", "New York, Toronto", -300, Dst::Us},
      {"America/Chicago", "Chicago", -360, Dst::Us},
      {"America/Denver", "Denver", -420, Dst::Us},
      {"America/Phoenix", "Phoenix", -420, Dst::None},
      {"America/Los_Angeles", "Los Angeles, Vancouver", -480, Dst::Us},
      {"America/Anchorage", "Anchorage", -540, Dst::Us},
      {"Pacific/Honolulu", "Honolulu", -600, Dst::None},
  };
  return kZones;
}

const Zone* find_zone(const std::string& id) {
  for (const Zone& z : zones()) {
    if (id == z.id) return &z;
  }
  return nullptr;
}

int offset_seconds(const Zone& zone, int64_t utc) {
  const int stdSeconds = zone.stdOffsetMin * 60;
  return dst_active(zone.dst, stdSeconds, utc) ? stdSeconds + 3600 : stdSeconds;
}

int offset_seconds(const std::string& zoneId, int64_t utc) {
  const Zone* z = find_zone(zoneId);
  return z ? offset_seconds(*z, utc) : 0;
}
