// Text and time formatting shared by the screens. Pure functions: no clock
// reads, no LVGL, no stdio. Times are epoch seconds; "local" values are
// already shifted by the Store's timezone offset.
#pragma once

#include <stdint.h>

#include <string>

// Division rounding towards negative infinity (b > 0 or b < 0).
int64_t floor_div(int64_t a, int64_t b);

// "HH:MM" of a local time.
std::string hh_mm(int64_t local);

// "Mon" .. "Sun" of a local time.
const char* weekday(int64_t local);

// "Sunday" .. "Saturday" of a local time.
const char* weekday_full(int64_t local);

// "SAT 24 OCT" of a local time (day without a leading zero).
std::string short_date(int64_t local);

// Header of the day group a local kickoff belongs to, relative to the local
// time `nowLocal`: "TODAY", "TOMORROW", "YESTERDAY", the weekday name ("SATURDAY")
// within six days either way, otherwise a short date ("SAT 24 OCT").
std::string day_header(int64_t kickoffLocal, int64_t nowLocal);

// ASCII letters to upper case; other bytes (UTF-8 included) are unchanged.
std::string to_upper(std::string s);

// Percent-encodes everything except A-Z a-z 0-9 - _ . ~ (RFC 3986 unreserved).
std::string url_encode(const std::string& s);

// Replaces (or adds) the query parameter `key` in a helper path.
std::string with_param(const std::string& path, const std::string& key, const std::string& value);

// `path` with w and h set; an empty path stays empty.
std::string sized(const std::string& path, int w, int h);

// Display name of an ESPN league code ("eng.1" -> "Premier League"); unknown
// codes are returned as they are (the pointer is into `code`).
const char* league_name(const std::string& code);

// "Kick-off soon", "Kick-off in 25 min", "Kick-off in 2 h", "Kick-off in
// 2 h 5 min", "Kick-off in 3 days". Never says "0 min".
std::string countdown_text(int64_t seconds);

// "Updated N min ago" (at least 1), or "Updated N h ago" from an hour on.
std::string stale_text(int64_t age);

// Plain-English reading of a formation: the first number is defenders, the
// last attackers, everything between midfielders. "4-2-3-1" gives
// "4 defenders, 5 midfielders, 1 attacker". Anything that is not at least
// three dash-separated numbers gives "".
std::string formation_blurb(const std::string& formation);

// Sort key of an event minute such as "12'", "45'+2'" or "90'+4'": added time
// sorts after its base minute and before the next one. Unparsable minutes
// sort last.
int minute_sort_key(const std::string& minute);

// Leading number of a stat value ("58%" -> 58, "1.75" -> 1.75), skipping
// leading spaces. False when the value does not start with a digit.
bool stat_number(const std::string& value, double& out);
