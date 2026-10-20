#include <doctest/doctest.h>

#include <stdint.h>

#include <set>
#include <string>

#include "timezone.h"

namespace {

struct Case {
  const char* zone;
  int64_t utc;
  int offset;
};

// Expected offsets below were computed with Python's zoneinfo
// (datetime.fromtimestamp(t, UTC).astimezone(ZoneInfo(id)).utcoffset()):
// each DST zone just before (t - 1) and at (t) every 2026 and 2027
// transition, found by bisecting to the second, and mid-January and mid-July
// of both years.
const Case kDst[] = {
    {"Europe/London", 1774745999, 0},
    {"Europe/London", 1774746000, 3600},
    {"Europe/London", 1792889999, 3600},
    {"Europe/London", 1792890000, 0},
    {"Europe/London", 1806195599, 0},
    {"Europe/London", 1806195600, 3600},
    {"Europe/London", 1824944399, 3600},
    {"Europe/London", 1824944400, 0},
    {"Europe/London", 1768478400, 0},
    {"Europe/London", 1784116800, 3600},
    {"Europe/London", 1800014400, 0},
    {"Europe/London", 1815652800, 3600},
    {"Europe/Paris", 1774745999, 3600},
    {"Europe/Paris", 1774746000, 7200},
    {"Europe/Paris", 1792889999, 7200},
    {"Europe/Paris", 1792890000, 3600},
    {"Europe/Paris", 1806195599, 3600},
    {"Europe/Paris", 1806195600, 7200},
    {"Europe/Paris", 1824944399, 7200},
    {"Europe/Paris", 1824944400, 3600},
    {"Europe/Paris", 1768478400, 3600},
    {"Europe/Paris", 1784116800, 7200},
    {"Europe/Paris", 1800014400, 3600},
    {"Europe/Paris", 1815652800, 7200},
    {"Europe/Athens", 1774745999, 7200},
    {"Europe/Athens", 1774746000, 10800},
    {"Europe/Athens", 1792889999, 10800},
    {"Europe/Athens", 1792890000, 7200},
    {"Europe/Athens", 1806195599, 7200},
    {"Europe/Athens", 1806195600, 10800},
    {"Europe/Athens", 1824944399, 10800},
    {"Europe/Athens", 1824944400, 7200},
    {"Europe/Athens", 1768478400, 7200},
    {"Europe/Athens", 1784116800, 10800},
    {"Europe/Athens", 1800014400, 7200},
    {"Europe/Athens", 1815652800, 10800},
    {"America/New_York", 1772953199, -18000},
    {"America/New_York", 1772953200, -14400},
    {"America/New_York", 1793512799, -14400},
    {"America/New_York", 1793512800, -18000},
    {"America/New_York", 1805007599, -18000},
    {"America/New_York", 1805007600, -14400},
    {"America/New_York", 1825567199, -14400},
    {"America/New_York", 1825567200, -18000},
    {"America/New_York", 1768478400, -18000},
    {"America/New_York", 1784116800, -14400},
    {"America/New_York", 1800014400, -18000},
    {"America/New_York", 1815652800, -14400},
    {"America/Chicago", 1772956799, -21600},
    {"America/Chicago", 1772956800, -18000},
    {"America/Chicago", 1793516399, -18000},
    {"America/Chicago", 1793516400, -21600},
    {"America/Chicago", 1805011199, -21600},
    {"America/Chicago", 1805011200, -18000},
    {"America/Chicago", 1825570799, -18000},
    {"America/Chicago", 1825570800, -21600},
    {"America/Chicago", 1768478400, -21600},
    {"America/Chicago", 1784116800, -18000},
    {"America/Chicago", 1800014400, -21600},
    {"America/Chicago", 1815652800, -18000},
    {"America/Denver", 1772960399, -25200},
    {"America/Denver", 1772960400, -21600},
    {"America/Denver", 1793519999, -21600},
    {"America/Denver", 1793520000, -25200},
    {"America/Denver", 1805014799, -25200},
    {"America/Denver", 1805014800, -21600},
    {"America/Denver", 1825574399, -21600},
    {"America/Denver", 1825574400, -25200},
    {"America/Denver", 1768478400, -25200},
    {"America/Denver", 1784116800, -21600},
    {"America/Denver", 1800014400, -25200},
    {"America/Denver", 1815652800, -21600},
    {"America/Los_Angeles", 1772963999, -28800},
    {"America/Los_Angeles", 1772964000, -25200},
    {"America/Los_Angeles", 1793523599, -25200},
    {"America/Los_Angeles", 1793523600, -28800},
    {"America/Los_Angeles", 1805018399, -28800},
    {"America/Los_Angeles", 1805018400, -25200},
    {"America/Los_Angeles", 1825577999, -25200},
    {"America/Los_Angeles", 1825578000, -28800},
    {"America/Los_Angeles", 1768478400, -28800},
    {"America/Los_Angeles", 1784116800, -25200},
    {"America/Los_Angeles", 1800014400, -28800},
    {"America/Los_Angeles", 1815652800, -25200},
    {"America/Anchorage", 1772967599, -32400},
    {"America/Anchorage", 1772967600, -28800},
    {"America/Anchorage", 1793527199, -28800},
    {"America/Anchorage", 1793527200, -32400},
    {"America/Anchorage", 1805021999, -32400},
    {"America/Anchorage", 1805022000, -28800},
    {"America/Anchorage", 1825581599, -28800},
    {"America/Anchorage", 1825581600, -32400},
    {"America/Anchorage", 1768478400, -32400},
    {"America/Anchorage", 1784116800, -28800},
    {"America/Anchorage", 1800014400, -32400},
    {"America/Anchorage", 1815652800, -28800},
    {"Australia/Sydney", 1775318399, 39600},
    {"Australia/Sydney", 1775318400, 36000},
    {"Australia/Sydney", 1791043199, 36000},
    {"Australia/Sydney", 1791043200, 39600},
    {"Australia/Sydney", 1806767999, 39600},
    {"Australia/Sydney", 1806768000, 36000},
    {"Australia/Sydney", 1822492799, 36000},
    {"Australia/Sydney", 1822492800, 39600},
    {"Australia/Sydney", 1768478400, 39600},
    {"Australia/Sydney", 1784116800, 36000},
    {"Australia/Sydney", 1800014400, 39600},
    {"Australia/Sydney", 1815652800, 36000},
    {"Pacific/Auckland", 1775311199, 46800},
    {"Pacific/Auckland", 1775311200, 43200},
    {"Pacific/Auckland", 1790431199, 43200},
    {"Pacific/Auckland", 1790431200, 46800},
    {"Pacific/Auckland", 1806760799, 46800},
    {"Pacific/Auckland", 1806760800, 43200},
    {"Pacific/Auckland", 1821880799, 43200},
    {"Pacific/Auckland", 1821880800, 46800},
    {"Pacific/Auckland", 1768478400, 46800},
    {"Pacific/Auckland", 1784116800, 43200},
    {"Pacific/Auckland", 1800014400, 46800},
    {"Pacific/Auckland", 1815652800, 43200},};

// Same zoneinfo check for zones without daylight saving, at six instants
// across 2026 and 2027.
const Case kConst[] = {
    {"UTC", 1768478400, 0},
    {"UTC", 1775044800, 0},
    {"UTC", 1784116800, 0},
    {"UTC", 1793361600, 0},
    {"UTC", 1806235200, 0},
    {"UTC", 1815652800, 0},
    {"Europe/Istanbul", 1768478400, 10800},
    {"Europe/Istanbul", 1775044800, 10800},
    {"Europe/Istanbul", 1784116800, 10800},
    {"Europe/Istanbul", 1793361600, 10800},
    {"Europe/Istanbul", 1806235200, 10800},
    {"Europe/Istanbul", 1815652800, 10800},
    {"Europe/Moscow", 1768478400, 10800},
    {"Europe/Moscow", 1775044800, 10800},
    {"Europe/Moscow", 1784116800, 10800},
    {"Europe/Moscow", 1793361600, 10800},
    {"Europe/Moscow", 1806235200, 10800},
    {"Europe/Moscow", 1815652800, 10800},
    {"Africa/Lagos", 1768478400, 3600},
    {"Africa/Lagos", 1775044800, 3600},
    {"Africa/Lagos", 1784116800, 3600},
    {"Africa/Lagos", 1793361600, 3600},
    {"Africa/Lagos", 1806235200, 3600},
    {"Africa/Lagos", 1815652800, 3600},
    {"Africa/Johannesburg", 1768478400, 7200},
    {"Africa/Johannesburg", 1775044800, 7200},
    {"Africa/Johannesburg", 1784116800, 7200},
    {"Africa/Johannesburg", 1793361600, 7200},
    {"Africa/Johannesburg", 1806235200, 7200},
    {"Africa/Johannesburg", 1815652800, 7200},
    {"Africa/Nairobi", 1768478400, 10800},
    {"Africa/Nairobi", 1775044800, 10800},
    {"Africa/Nairobi", 1784116800, 10800},
    {"Africa/Nairobi", 1793361600, 10800},
    {"Africa/Nairobi", 1806235200, 10800},
    {"Africa/Nairobi", 1815652800, 10800},
    {"Asia/Dubai", 1768478400, 14400},
    {"Asia/Dubai", 1775044800, 14400},
    {"Asia/Dubai", 1784116800, 14400},
    {"Asia/Dubai", 1793361600, 14400},
    {"Asia/Dubai", 1806235200, 14400},
    {"Asia/Dubai", 1815652800, 14400},
    {"Asia/Karachi", 1768478400, 18000},
    {"Asia/Karachi", 1775044800, 18000},
    {"Asia/Karachi", 1784116800, 18000},
    {"Asia/Karachi", 1793361600, 18000},
    {"Asia/Karachi", 1806235200, 18000},
    {"Asia/Karachi", 1815652800, 18000},
    {"Asia/Kolkata", 1768478400, 19800},
    {"Asia/Kolkata", 1775044800, 19800},
    {"Asia/Kolkata", 1784116800, 19800},
    {"Asia/Kolkata", 1793361600, 19800},
    {"Asia/Kolkata", 1806235200, 19800},
    {"Asia/Kolkata", 1815652800, 19800},
    {"Asia/Dhaka", 1768478400, 21600},
    {"Asia/Dhaka", 1775044800, 21600},
    {"Asia/Dhaka", 1784116800, 21600},
    {"Asia/Dhaka", 1793361600, 21600},
    {"Asia/Dhaka", 1806235200, 21600},
    {"Asia/Dhaka", 1815652800, 21600},
    {"Asia/Bangkok", 1768478400, 25200},
    {"Asia/Bangkok", 1775044800, 25200},
    {"Asia/Bangkok", 1784116800, 25200},
    {"Asia/Bangkok", 1793361600, 25200},
    {"Asia/Bangkok", 1806235200, 25200},
    {"Asia/Bangkok", 1815652800, 25200},
    {"Asia/Shanghai", 1768478400, 28800},
    {"Asia/Shanghai", 1775044800, 28800},
    {"Asia/Shanghai", 1784116800, 28800},
    {"Asia/Shanghai", 1793361600, 28800},
    {"Asia/Shanghai", 1806235200, 28800},
    {"Asia/Shanghai", 1815652800, 28800},
    {"Asia/Tokyo", 1768478400, 32400},
    {"Asia/Tokyo", 1775044800, 32400},
    {"Asia/Tokyo", 1784116800, 32400},
    {"Asia/Tokyo", 1793361600, 32400},
    {"Asia/Tokyo", 1806235200, 32400},
    {"Asia/Tokyo", 1815652800, 32400},
    {"America/Sao_Paulo", 1768478400, -10800},
    {"America/Sao_Paulo", 1775044800, -10800},
    {"America/Sao_Paulo", 1784116800, -10800},
    {"America/Sao_Paulo", 1793361600, -10800},
    {"America/Sao_Paulo", 1806235200, -10800},
    {"America/Sao_Paulo", 1815652800, -10800},
    {"America/Phoenix", 1768478400, -25200},
    {"America/Phoenix", 1775044800, -25200},
    {"America/Phoenix", 1784116800, -25200},
    {"America/Phoenix", 1793361600, -25200},
    {"America/Phoenix", 1806235200, -25200},
    {"America/Phoenix", 1815652800, -25200},
    {"Pacific/Honolulu", 1768478400, -36000},
    {"Pacific/Honolulu", 1775044800, -36000},
    {"Pacific/Honolulu", 1784116800, -36000},
    {"Pacific/Honolulu", 1793361600, -36000},
    {"Pacific/Honolulu", 1806235200, -36000},
    {"Pacific/Honolulu", 1815652800, -36000},
};

}  // namespace

TEST_CASE("DST zones match zoneinfo at every 2026 and 2027 transition") {
  for (const Case& c : kDst) {
    INFO(c.zone << " @ " << c.utc);
    CHECK(offset_seconds(std::string(c.zone), c.utc) == c.offset);
    const Zone* z = find_zone(c.zone);
    REQUIRE(z != nullptr);
    CHECK(offset_seconds(*z, c.utc) == c.offset);
  }
}

TEST_CASE("zones without DST are constant") {
  for (const Case& c : kConst) {
    INFO(c.zone << " @ " << c.utc);
    CHECK(offset_seconds(std::string(c.zone), c.utc) == c.offset);
  }
}

TEST_CASE("zone list has the agreed order and unique ids") {
  const std::vector<Zone>& zs = zones();
  REQUIRE(zs.size() == 26);
  CHECK(std::string(zs.front().id) == "UTC");
  CHECK(std::string(zs[1].id) == "Europe/London");
  CHECK(std::string(zs[16].id) == "Australia/Sydney");
  CHECK(std::string(zs.back().id) == "Pacific/Honolulu");
  std::set<std::string> ids;
  for (const Zone& z : zs) ids.insert(z.id);
  CHECK(ids.size() == zs.size());
  CHECK(std::string(find_zone("America/Chicago")->label) == "Chicago");
  CHECK(find_zone("America/New_York")->stdOffsetMin == -300);
  CHECK(find_zone("America/New_York")->dst == Dst::Us);
  CHECK(find_zone("Asia/Kolkata")->dst == Dst::None);
}

TEST_CASE("unknown zone ids") {
  CHECK(find_zone("bogus") == nullptr);
  CHECK(find_zone("") == nullptr);
  CHECK(offset_seconds(std::string("bogus"), 1784116800) == 0);
  CHECK(offset_seconds(std::string(""), 0) == 0);
}

TEST_CASE("negative and pre-2000 epochs give the rule-based answer") {
  // New York, checked against zoneinfo: 1969-07-20 12:00 UTC, 1969-01-20, 1965-07-01.
  CHECK(offset_seconds(std::string("America/New_York"), -14212800) == -14400);
  CHECK(offset_seconds(std::string("America/New_York"), -29851200) == -18000);
  CHECK(offset_seconds(std::string("America/New_York"), -142084800) == -14400);
  // Paris and Sydney 1999-07-01 and 1999-01-15, 12:00 UTC.
  CHECK(offset_seconds(std::string("Europe/Paris"), 930830400) == 7200);
  CHECK(offset_seconds(std::string("Europe/Paris"), 916401600) == 3600);
  CHECK(offset_seconds(std::string("Australia/Sydney"), 930830400) == 36000);
  CHECK(offset_seconds(std::string("Australia/Sydney"), 916401600) == 39600);
  // Extreme values do not crash and stay within a sane range.
  for (const Zone& z : zones()) {
    for (int64_t t : {int64_t(-4000000000000), int64_t(-1), int64_t(0), int64_t(4000000000000)}) {
      const int o = offset_seconds(z, t);
      CHECK(o >= -12 * 3600);
      CHECK(o <= 14 * 3600);
    }
  }
}
