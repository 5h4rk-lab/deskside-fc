#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "model.h"
#include "store.h"

namespace {

MatchSummary make(const std::string& id, const std::string& homeId, const std::string& awayId,
                  MatchState state, int64_t kickoff = 0) {
  MatchSummary m;
  m.id = id;
  m.state = state;
  m.kickoff = kickoff;
  m.home.id = homeId;
  m.away.id = awayId;
  return m;
}

}  // namespace

TEST_CASE("an empty store has no data and is never stale") {
  Store s;
  CHECK_FALSE(s.has_data());
  CHECK(s.matches().empty());
  CHECK_FALSE(s.stale(1000000));
  CHECK(s.live_for("1") == nullptr);
  CHECK(s.next_for("1", 0) == nullptr);
}

TEST_CASE("live_for finds the team's live match in either role") {
  Store s;
  s.set_matches({make("a", "1", "2", MatchState::Pre, 500), make("b", "3", "4", MatchState::Live),
                 make("c", "5", "1", MatchState::Live)},
                1000);
  const MatchSummary* m = s.live_for("1");
  REQUIRE(m != nullptr);
  CHECK(m->id == "c");  // away role
  m = s.live_for("3");
  REQUIRE(m != nullptr);
  CHECK(m->id == "b");  // home role
  CHECK(s.live_for("9") == nullptr);
  CHECK(s.live_for("2") == nullptr);  // only in a Pre match
}

TEST_CASE("next_for picks the earliest future pre match") {
  Store s;
  s.set_matches({make("late", "1", "2", MatchState::Pre, 3000),
                 make("past", "1", "3", MatchState::Pre, 500),
                 make("soon", "4", "1", MatchState::Pre, 2000),
                 make("live", "1", "5", MatchState::Live, 2500),
                 make("other", "7", "8", MatchState::Pre, 1500)},
                1000);
  const MatchSummary* m = s.next_for("1", 1000);
  REQUIRE(m != nullptr);
  CHECK(m->id == "soon");
  m = s.next_for("1", 2000);  // kickoff == now still counts
  REQUIRE(m != nullptr);
  CHECK(m->id == "soon");
  m = s.next_for("1", 2001);
  REQUIRE(m != nullptr);
  CHECK(m->id == "late");
  CHECK(s.next_for("1", 3001) == nullptr);
  CHECK(s.next_for("9", 0) == nullptr);
}

TEST_CASE("stale is false at exactly 90 s and true at 91 s") {
  Store s;
  s.set_matches({make("a", "1", "2", MatchState::Pre)}, 1000);
  CHECK(s.has_data());
  CHECK_FALSE(s.stale(1000));
  CHECK_FALSE(s.stale(1090));
  CHECK(s.stale(1091));
}

TEST_CASE("mark_failure does not reset the success time") {
  Store s;
  s.mark_failure();
  CHECK_FALSE(s.has_data());
  CHECK_FALSE(s.stale(5000));
  s.set_matches({make("a", "1", "2", MatchState::Pre)}, 1000);
  s.mark_failure();
  CHECK(s.has_data());
  CHECK(s.matches().size() == 1);
  CHECK_FALSE(s.stale(1090));
  CHECK(s.stale(1091));
}

TEST_CASE("set_matches with an empty list still counts as data") {
  Store s;
  s.set_matches({}, 1000);
  CHECK(s.has_data());
  CHECK(s.stale(1091));
}

TEST_CASE("tz offset, favourite, news and teams round trip") {
  Store s;
  CHECK(s.tz_offset() == 0);
  s.set_tz_offset(-18000);
  CHECK(s.tz_offset() == -18000);
  s.set_tz_offset(19800);
  CHECK(s.tz_offset() == 19800);

  CHECK(s.favourite().empty());
  s.set_favourite("367");
  CHECK(s.favourite() == "367");

  NewsItem n;
  n.id = "n1";
  s.set_news({n});
  REQUIRE(s.news().size() == 1);
  CHECK(s.news()[0].id == "n1");

  Team t;
  t.id = "t1";
  s.set_teams({t});
  REQUIRE(s.teams().size() == 1);
  CHECK(s.teams()[0].id == "t1");
}

TEST_CASE("last_success reports the time of the last set_matches") {
  Store s;
  CHECK(s.last_success() == 0);
  s.set_matches({}, 1234);
  CHECK(s.last_success() == 1234);
  s.mark_failure();
  CHECK(s.last_success() == 1234);
}

TEST_CASE("detail_for returns the detail stored for a match and keeps the last few") {
  Store s;
  CHECK(s.detail_for("a") == nullptr);
  MatchDetail d;
  d.match.id = "a";
  d.events.push_back(MatchEvent{"12'", EventType::Goal, Side::Home, "Saka", ""});
  s.set_detail(d);
  const MatchDetail* got = s.detail_for("a");
  REQUIRE(got != nullptr);
  REQUIRE(got->events.size() == 1);
  CHECK(got->events[0].player == "Saka");

  // Replacing the same id updates in place.
  d.events.push_back(MatchEvent{"58'", EventType::Goal, Side::Home, "Havertz", ""});
  s.set_detail(d);
  REQUIRE(s.detail_for("a") != nullptr);
  CHECK(s.detail_for("a")->events.size() == 2);

  // The cache is small: the oldest entry is dropped once it is full.
  for (int i = 0; i < Store::kDetailCacheSize; ++i) {
    MatchDetail other;
    other.match.id = "m" + std::to_string(i);
    s.set_detail(other);
  }
  CHECK(s.detail_for("a") == nullptr);
  CHECK(s.detail_for("m0") != nullptr);
  CHECK(s.detail_for("m" + std::to_string(Store::kDetailCacheSize - 1)) != nullptr);
}
