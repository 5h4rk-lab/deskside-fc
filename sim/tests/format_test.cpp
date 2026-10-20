#include <doctest/doctest.h>

#include <stdint.h>

#include <string>

#include "format.h"

namespace {

constexpr int64_t kTue2000 = 1792526400;  // Tue 2026-10-20 20:00 UTC
constexpr int kIndia = 5 * 3600 + 30 * 60;

}  // namespace

TEST_CASE("floor_div rounds towards negative infinity") {
  CHECK(floor_div(7, 2) == 3);
  CHECK(floor_div(-7, 2) == -4);
  CHECK(floor_div(-8, 2) == -4);
  CHECK(floor_div(7, -2) == -4);
  CHECK(floor_div(0, 5) == 0);
  CHECK(floor_div(-1, 86400) == -1);
}

TEST_CASE("hh_mm and weekday use the shifted local time") {
  CHECK(hh_mm(kTue2000) == "20:00");
  CHECK(std::string(weekday(kTue2000)) == "Tue");
  // +05:30 moves 20:00 Tuesday past midnight.
  CHECK(hh_mm(kTue2000 + kIndia) == "01:30");
  CHECK(std::string(weekday(kTue2000 + kIndia)) == "Wed");
  // A negative offset (UTC-08:00) moves 03:00 Tuesday back into Monday.
  const int64_t tue0300 = kTue2000 - 17 * 3600;
  CHECK(hh_mm(tue0300 - 8 * 3600) == "19:00");
  CHECK(std::string(weekday(tue0300 - 8 * 3600)) == "Mon");
  // Day boundaries.
  const int64_t midnight = kTue2000 - 20 * 3600;
  CHECK(hh_mm(midnight) == "00:00");
  CHECK(std::string(weekday(midnight)) == "Tue");
  CHECK(hh_mm(midnight - 60) == "23:59");
  CHECK(std::string(weekday(midnight - 1)) == "Mon");
  // Before the epoch.
  CHECK(hh_mm(-60) == "23:59");
  CHECK(std::string(weekday(-1)) == "Wed");
  CHECK(std::string(weekday(0)) == "Thu");
}

TEST_CASE("countdown_text never says 0 min") {
  CHECK(countdown_text(30) == "Kick-off soon");
  CHECK(countdown_text(60) == "Kick-off in 1 min");
  CHECK(countdown_text(59 * 60 + 59) == "Kick-off in 59 min");
  CHECK(countdown_text(3600) == "Kick-off in 1 h");
  CHECK(countdown_text(3600 + 59) == "Kick-off in 1 h");
  CHECK(countdown_text(3600 + 5 * 60) == "Kick-off in 1 h 5 min");
  CHECK(countdown_text(23 * 3600) == "Kick-off in 23 h");
  CHECK(countdown_text(86400) == "Kick-off in 1 day");
  CHECK(countdown_text(3 * 86400 + 7) == "Kick-off in 3 days");
}

TEST_CASE("stale_text gives minutes, at least one, then hours") {
  CHECK(stale_text(10) == "Updated 1 min ago");
  CHECK(stale_text(95) == "Updated 1 min ago");
  CHECK(stale_text(200) == "Updated 3 min ago");
  CHECK(stale_text(59 * 60 + 59) == "Updated 59 min ago");
  CHECK(stale_text(3600) == "Updated 1 h ago");
  CHECK(stale_text(3 * 3600 + 1800) == "Updated 3 h ago");
}

TEST_CASE("to_upper changes ASCII letters only") {
  CHECK(to_upper("Arsenal FC 1") == "ARSENAL FC 1");
  CHECK(to_upper("M\xC3\xB6nchengladbach") == "M\xC3\xB6NCHENGLADBACH");
}

TEST_CASE("url_encode keeps unreserved characters and encodes the rest") {
  CHECK(url_encode("Kai Havertz") == "Kai%20Havertz");
  CHECK(url_encode("a-b_c.d~e") == "a-b_c.d~e");
  CHECK(url_encode("\xC3\x98") == "%C3%98");
  CHECK(url_encode("a&b=c") == "a%26b%3Dc");
}

TEST_CASE("with_param and sized replace or add query parameters") {
  CHECK(with_param("/api/img", "w", "48") == "/api/img?w=48");
  CHECK(with_param("/api/img?u=x", "w", "48") == "/api/img?u=x&w=48");
  CHECK(with_param("/api/img?u=x&w=10&h=10", "w", "48") == "/api/img?u=x&w=48&h=10");
  CHECK(with_param("/api/img?wide=1", "w", "48") == "/api/img?wide=1&w=48");
  CHECK(sized("/api/img?u=x&w=48&h=48", 84, 84) == "/api/img?u=x&w=84&h=84");
  CHECK(sized("", 84, 84).empty());
}

TEST_CASE("league_name maps known codes and passes unknown ones through") {
  CHECK(std::string(league_name("eng.1")) == "Premier League");
  CHECK(std::string(league_name("uefa.champions")) == "Champions League");
  const std::string unknown = "xyz.9";
  CHECK(std::string(league_name(unknown)) == "xyz.9");
}

TEST_CASE("formation_blurb explains a formation in plain English") {
  CHECK(formation_blurb("4-3-3") == "4 defenders, 3 midfielders, 3 attackers");
  CHECK(formation_blurb("3-1-4-2") == "3 defenders, 5 midfielders, 2 attackers");
  CHECK(formation_blurb("4-2-3-1") == "4 defenders, 5 midfielders, 1 attacker");
  CHECK(formation_blurb("4-4-2") == "4 defenders, 4 midfielders, 2 attackers");
  CHECK(formation_blurb("5-4-1") == "5 defenders, 4 midfielders, 1 attacker");
  CHECK(formation_blurb("1-3-2") == "1 defender, 3 midfielders, 2 attackers");
  CHECK(formation_blurb("4-1-4-1") == "4 defenders, 5 midfielders, 1 attacker");
  CHECK(formation_blurb("").empty());
  CHECK(formation_blurb("4-4").empty());
  CHECK(formation_blurb("4--3-3").empty());
  CHECK(formation_blurb("4-3-3-").empty());
  CHECK(formation_blurb("a-b-c").empty());
  CHECK(formation_blurb("4-x-3").empty());
}

TEST_CASE("minute_sort_key orders added time after the base minute") {
  CHECK(minute_sort_key("12'") < minute_sort_key("34'"));
  CHECK(minute_sort_key("45'") < minute_sort_key("45'+2'"));
  CHECK(minute_sort_key("45'+2'") < minute_sort_key("46'"));
  CHECK(minute_sort_key("90'+4'") < minute_sort_key("105'"));
  CHECK(minute_sort_key("9'") < minute_sort_key("10'"));
  // Unparsable minutes go last.
  CHECK(minute_sort_key("120'") < minute_sort_key("?"));
  CHECK(minute_sort_key("") == minute_sort_key("?"));
}

TEST_CASE("stat_number reads the leading number of a stat value") {
  double v = -1;
  CHECK(stat_number("58%", v));
  CHECK(v == doctest::Approx(58));
  CHECK(stat_number("11", v));
  CHECK(v == doctest::Approx(11));
  CHECK(stat_number("1.75", v));
  CHECK(v == doctest::Approx(1.75));
  CHECK(stat_number(" 7", v));
  CHECK(v == doctest::Approx(7));
  CHECK_FALSE(stat_number("", v));
  CHECK_FALSE(stat_number("n/a", v));
  CHECK_FALSE(stat_number("%", v));
}

TEST_CASE("weekday_full and short_date name the local day") {
  CHECK(std::string(weekday_full(kTue2000)) == "Tuesday");
  CHECK(std::string(weekday_full(kTue2000 + 4 * 86400)) == "Saturday");
  CHECK(std::string(weekday_full(kTue2000 - 2 * 86400)) == "Sunday");
  CHECK(short_date(kTue2000 + 4 * 86400) == "SAT 24 OCT");
  CHECK(short_date(kTue2000) == "TUE 20 OCT");
  CHECK(short_date(kTue2000 + 12 * 86400) == "SUN 1 NOV");
}

TEST_CASE("short_date handles month and year ends and leap days") {
  const int64_t dec31 = 1798675200;  // Thu 2026-12-31 00:00 UTC
  CHECK(short_date(dec31) == "THU 31 DEC");
  CHECK(short_date(dec31 + 86400) == "FRI 1 JAN");
  const int64_t feb29 = 1709164800;  // Thu 2024-02-29 00:00 UTC
  CHECK(short_date(feb29) == "THU 29 FEB");
  CHECK(short_date(feb29 + 86400) == "FRI 1 MAR");
  CHECK(short_date(-86400) == "WED 31 DEC");  // 1969-12-31
}

TEST_CASE("day_header names the day relative to now") {
  const int64_t now = kTue2000;  // Tuesday evening, "local" for this test
  auto at = [&](int days, int hour) {
    return day_header(now - 20 * 3600 + days * 86400 + hour * 3600, now);
  };
  CHECK(at(0, 0) == "TODAY");
  CHECK(at(0, 23) == "TODAY");
  CHECK(at(1, 0) == "TOMORROW");
  CHECK(at(2, 12) == "THURSDAY");
  CHECK(at(6, 12) == "MONDAY");
  CHECK(at(7, 12) == "TUE 27 OCT");
  CHECK(at(30, 12) == "THU 19 NOV");
  CHECK(at(-1, 23) == "YESTERDAY");
  CHECK(at(-1, 0) == "YESTERDAY");
  CHECK(at(-2, 12) == "SUNDAY");
  CHECK(at(-6, 12) == "WEDNESDAY");
  CHECK(at(-7, 12) == "TUE 13 OCT");
}

TEST_CASE("relative_time: just now below a minute and for the future") {
  const int64_t now = kTue2000;
  CHECK(relative_time(now, now) == "just now");
  CHECK(relative_time(now, now - 59) == "just now");
  CHECK(relative_time(now, now + 1) == "just now");       // a future timestamp
  CHECK(relative_time(now, now + 86400) == "just now");
}

TEST_CASE("relative_time: minutes, hours, yesterday, days, then a date") {
  const int64_t now = kTue2000;
  CHECK(relative_time(now, now - 60) == "1 min ago");
  CHECK(relative_time(now, now - 119) == "1 min ago");
  CHECK(relative_time(now, now - 25 * 60) == "25 min ago");
  CHECK(relative_time(now, now - (59 * 60 + 59)) == "59 min ago");
  CHECK(relative_time(now, now - 3600) == "1h ago");
  CHECK(relative_time(now, now - 2 * 3600) == "2h ago");
  CHECK(relative_time(now, now - (23 * 3600 + 59 * 60)) == "23h ago");
  CHECK(relative_time(now, now - 24 * 3600) == "yesterday");
  CHECK(relative_time(now, now - 47 * 3600) == "yesterday");
  CHECK(relative_time(now, now - 48 * 3600) == "2 days ago");
  CHECK(relative_time(now, now - 3 * 86400) == "3 days ago");
  CHECK(relative_time(now, now - 29 * 86400) == "29 days ago");
  // 30 days back is Sun 20 Sep 2026.
  CHECK(relative_time(now, now - 30 * 86400) == "20 Sep");
  CHECK(relative_time(now, now - 400 * 86400) == "15 Sep");
}

TEST_CASE("relative_time dates use the local day when given an offset") {
  const int64_t now = kTue2000;
  const int64_t published = now - 30 * 86400;  // 20 Sep 20:00 UTC
  CHECK(relative_time(now, published, 0) == "20 Sep");
  CHECK(relative_time(now, published, kIndia) == "21 Sep");  // 01:30 next day
}

TEST_CASE("day_month is a short date without the weekday") {
  CHECK(day_month(kTue2000) == "20 Oct");
  CHECK(day_month(kTue2000 - 8 * 86400) == "12 Oct");
  CHECK(day_month(kTue2000 + 12 * 86400) == "1 Nov");
  CHECK(day_month(-86400) == "31 Dec");
}

TEST_CASE("local_hour applies the offset and wraps correctly") {
  CHECK(local_hour(kTue2000, 0) == 20);
  CHECK(local_hour(kTue2000, 3600) == 21);
  CHECK(local_hour(kTue2000, 4 * 3600) == 0);  // past midnight
  CHECK(local_hour(kTue2000, kIndia) == 1);
  CHECK(local_hour(kTue2000, -21 * 3600) == 23);  // back across midnight
  CHECK(local_hour(0, 0) == 0);
  CHECK(local_hour(0, -3600) == 23);
  CHECK(local_hour(-1, 0) == 23);  // before the epoch
  CHECK(local_hour(-86400 * 400 + 5 * 3600, 0) == 5);
  CHECK(local_hour(-3600, -5 * 3600) == 18);
}
