#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "goal_detector.h"
#include "model.h"

namespace {

const char* kFav = "100";
constexpr int64_t kT = 1792526400;  // every poll "now": baselines are always recent unless a test says otherwise

MatchSummary make(const std::string& id, const std::string& homeId, const std::string& awayId,
                  MatchState state, std::optional<int> hs, std::optional<int> as) {
  MatchSummary m;
  m.id = id;
  m.state = state;
  m.clock = "50'";
  m.home.id = homeId;
  m.home.abbr = "H" + homeId;
  m.away.id = awayId;
  m.away.abbr = "A" + awayId;
  m.homeScore = hs;
  m.awayScore = as;
  return m;
}

// Favourite "100" is at home.
MatchSummary fav_home(MatchState s, std::optional<int> hs, std::optional<int> as) {
  return make("m1", "100", "200", s, hs, as);
}

}  // namespace

TEST_CASE("first sight of a standing score is silent") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT).has_value());
}

TEST_CASE("favourite scoring fires a favourite event with the new score") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT);
  auto e = d.update({fav_home(MatchState::Live, 3, 1)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK_FALSE(e->fullTime);
  CHECK(e->matchId == "m1");
  CHECK(e->side == Side::Home);
  CHECK(e->scorer.empty());
  CHECK(e->clock == "50'");
  CHECK(e->homeScore == 3);
  CHECK(e->awayScore == 1);
  CHECK(e->homeAbbr == "H100");
  CHECK(e->awayAbbr == "A200");
  // Same score again: nothing.
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 3, 1)}, kFav, kT).has_value());
}

TEST_CASE("favourite scoring away reports the away side") {
  GoalDetector d;
  auto m = [](int hs, int as) { return make("m2", "300", "100", MatchState::Live, hs, as); };
  d.update({m(0, 0)}, kFav, kT);
  auto e = d.update({m(0, 1)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->side == Side::Away);
}

TEST_CASE("opponent scoring fires a non-favourite event") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT);
  auto e = d.update({fav_home(MatchState::Live, 2, 2)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK_FALSE(e->favourite);
  CHECK_FALSE(e->fullTime);
  CHECK(e->side == Side::Away);
  CHECK(e->homeScore == 2);
  CHECK(e->awayScore == 2);
}

TEST_CASE("a match first seen Pre baselines at 0:0 and fires on the first goal") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 0, 0)}, kFav, kT).has_value());
  auto e = d.update({fav_home(MatchState::Live, 1, 0)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("a Pre match going straight to a score still fires") {
  GoalDetector d;
  d.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT);
  auto e = d.update({fav_home(MatchState::Live, 1, 0)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("matches without the favourite never fire") {
  GoalDetector d;
  auto m = [](int hs, int as) { return make("x", "300", "400", MatchState::Live, hs, as); };
  d.update({m(0, 0)}, kFav, kT);
  CHECK_FALSE(d.update({m(1, 0)}, kFav, kT).has_value());
  CHECK_FALSE(d.update({m(1, 1)}, kFav, kT).has_value());
}

TEST_CASE("a score decrease is silent and the next increase fires again") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 3, 1)}, kFav, kT);
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT).has_value());
  auto e = d.update({fav_home(MatchState::Live, 3, 1)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("several goals in one poll give one event with the current score") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 0, 0)}, kFav, kT);
  auto e = d.update({fav_home(MatchState::Live, 2, 0)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->homeScore == 2);
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 0)}, kFav, kT).has_value());
}

TEST_CASE("both teams scoring in one poll returns the favourite's event") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 1)}, kFav, kT);
  auto e = d.update({fav_home(MatchState::Live, 2, 2)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->side == Side::Home);
  // The opponent's goal was absorbed into the baseline.
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 2)}, kFav, kT).has_value());
}

TEST_CASE("full time with the favourite ahead fires once") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT);
  auto e = d.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->fullTime);
  CHECK(e->favourite);
  CHECK(e->outcome == Outcome::Win);
  CHECK_FALSE(e->kickOff);
  CHECK(e->side == Side::Home);
  CHECK(e->matchId == "m1");
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT).has_value());
}

TEST_CASE("full time with the favourite ahead away reports the away side") {
  GoalDetector d;
  auto m = [](MatchState s) { return make("m2", "300", "100", s, 0, 2); };
  d.update({m(MatchState::Live)}, kFav, kT);
  auto e = d.update({m(MatchState::Post)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->fullTime);
  CHECK(e->side == Side::Away);
}

TEST_CASE("full time level or behind: a full-time event that is not the favourite's (no celebration), once") {
  {
    GoalDetector d;
    d.update({fav_home(MatchState::Live, 1, 1)}, kFav, kT);
    auto e = d.update({fav_home(MatchState::Post, 1, 1)}, kFav, kT);
    REQUIRE(e.has_value());
    CHECK(e->fullTime);
    CHECK_FALSE(e->favourite);
    CHECK(e->outcome == Outcome::Draw);
    CHECK(e->side == Side::Home);
    CHECK_FALSE(d.update({fav_home(MatchState::Post, 1, 1)}, kFav, kT).has_value());
  }
  {
    GoalDetector d;
    d.update({fav_home(MatchState::Live, 0, 2)}, kFav, kT);
    auto e = d.update({fav_home(MatchState::Post, 0, 2)}, kFav, kT);
    REQUIRE(e.has_value());
    CHECK(e->fullTime);
    CHECK_FALSE(e->favourite);
    CHECK(e->outcome == Outcome::Defeat);
    CHECK_FALSE(d.update({fav_home(MatchState::Post, 0, 2)}, kFav, kT).has_value());
  }
}

TEST_CASE("a match first seen Post is silent") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 3, 0)}, kFav, kT).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 3, 0)}, kFav, kT).has_value());
}

TEST_CASE("postponed matches never fire") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Postponed, std::nullopt, std::nullopt)}, kFav, kT).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Postponed, std::nullopt, std::nullopt)}, kFav, kT).has_value());
}

TEST_CASE("matches not involving the favourite are ignored alongside the favourite's") {
  GoalDetector d;
  auto other = [](int hs) { return make("o", "300", "400", MatchState::Live, hs, 0); };
  d.update({other(0), fav_home(MatchState::Live, 0, 0)}, kFav, kT);
  auto e = d.update({other(1), fav_home(MatchState::Live, 1, 0)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->matchId == "m1");
  // Only the other match scores: nothing.
  CHECK_FALSE(d.update({other(2), fav_home(MatchState::Live, 1, 0)}, kFav, kT).has_value());
}

// A team can appear in two matches in test data, which exercises the ranking
// across matches involving the favourite.
TEST_CASE("a favourite goal outranks an opponent goal in another match") {
  GoalDetector d;
  auto a = [](int as) { return make("a", "100", "200", MatchState::Live, 0, as); };
  auto b = [](int hs) { return make("b", "100", "300", MatchState::Live, hs, 0); };
  d.update({a(0), b(0)}, kFav, kT);
  // Opponent scores in the first-listed match, favourite in the second.
  auto e = d.update({a(1), b(1)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->matchId == "b");
  // Baselines of both matches were updated: nothing more to report.
  CHECK_FALSE(d.update({a(1), b(1)}, kFav, kT).has_value());
}

TEST_CASE("at the same rank the earlier match in the list wins") {
  GoalDetector d;
  auto a = [](int hs) { return make("a", "100", "200", MatchState::Live, hs, 0); };
  auto b = [](int hs) { return make("b", "100", "300", MatchState::Live, hs, 0); };
  d.update({a(0), b(0)}, kFav, kT);
  auto e = d.update({a(1), b(1)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->matchId == "a");
  CHECK_FALSE(d.update({a(1), b(1)}, kFav, kT).has_value());

  GoalDetector d2;
  auto c = [](int as) { return make("c", "100", "200", MatchState::Live, 0, as); };
  auto f = [](int as) { return make("f", "100", "300", MatchState::Live, 0, as); };
  d2.update({c(0), f(0)}, kFav, kT);
  auto e2 = d2.update({c(1), f(1)}, kFav, kT);
  REQUIRE(e2.has_value());
  CHECK_FALSE(e2->favourite);
  CHECK(e2->matchId == "c");
}

TEST_CASE("switching the favourite drops baselines so the return poll is silent") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, "100", kT);
  d.update({fav_home(MatchState::Live, 1, 0)}, "200", kT);
  // Score changed while "100" was not the favourite.
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 0)}, "100", kT).has_value());
  auto e = d.update({fav_home(MatchState::Live, 3, 0)}, "100", kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->homeScore == 3);
}

TEST_CASE("a match missing from a poll is first seen again when it returns") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, kFav, kT);
  CHECK_FALSE(d.update({}, kFav, kT).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 0)}, kFav, kT).has_value());
  auto e = d.update({fav_home(MatchState::Live, 3, 0)}, kFav, kT);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("a goal in the poll that ends the match is reported, then full time once") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, kFav, kT);
  auto goal = d.update({fav_home(MatchState::Post, 2, 0)}, kFav, kT);
  REQUIRE(goal.has_value());
  CHECK_FALSE(goal->fullTime);
  CHECK(goal->favourite);
  CHECK(goal->homeScore == 2);
  auto ft = d.update({fav_home(MatchState::Post, 2, 0)}, kFav, kT);
  REQUIRE(ft.has_value());
  CHECK(ft->fullTime);
  CHECK(ft->favourite);
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 2, 0)}, kFav, kT).has_value());
}

TEST_CASE("an equaliser in the poll that ends the match: the goal, then a full-time draw, no win") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, kFav, kT);
  auto goal = d.update({fav_home(MatchState::Post, 1, 1)}, kFav, kT);
  REQUIRE(goal.has_value());
  CHECK_FALSE(goal->favourite);
  CHECK_FALSE(goal->fullTime);
  auto ft = d.update({fav_home(MatchState::Post, 1, 1)}, kFav, kT);
  REQUIRE(ft.has_value());
  CHECK(ft->fullTime);
  CHECK_FALSE(ft->favourite);
  CHECK(ft->outcome == Outcome::Draw);
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 1, 1)}, kFav, kT).has_value());
}

// ------------------------------------------------------------ kick-off and the recency guard

namespace {

MatchSummary at(MatchSummary m, const char* clock) {
  m.clock = clock;
  return m;
}

}  // namespace

TEST_CASE("kick-off: the favourite's match going live in the first minutes is a kick-off event, once") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT).has_value());
  auto e = d.update({at(fav_home(MatchState::Live, 0, 0), "1'")}, kFav, kT + 180);
  REQUIRE(e.has_value());
  CHECK(e->kickOff);
  CHECK_FALSE(e->favourite);
  CHECK_FALSE(e->fullTime);
  CHECK(e->side == Side::Home);
  CHECK_FALSE(d.update({at(fav_home(MatchState::Live, 0, 0), "4'")}, kFav, kT + 205).has_value());
  // Away too, and with no minutes in the clock the recency guard alone decides.
  GoalDetector a;
  auto away = [](MatchState s) { return make("m2", "300", "100", s, std::nullopt, std::nullopt); };
  a.update({away(MatchState::Pre)}, kFav, kT);
  auto k = a.update({at(away(MatchState::Live), "")}, kFav, kT + 30);
  REQUIRE(k.has_value());
  CHECK(k->kickOff);
  CHECK(k->side == Side::Away);
}

TEST_CASE("kick-off: a goal in the same poll wins; a match first seen live never kicks off") {
  GoalDetector d;
  d.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT);
  auto e = d.update({at(fav_home(MatchState::Live, 1, 0), "2'")}, kFav, kT + 180);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK_FALSE(e->kickOff);
  GoalDetector f;
  CHECK_FALSE(f.update({at(fav_home(MatchState::Live, 0, 0), "1'")}, kFav, kT).has_value());
}

TEST_CASE("kick-off: not once the match clock is past 10 minutes (old data)") {
  GoalDetector d;
  d.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT);
  CHECK_FALSE(d.update({at(fav_home(MatchState::Live, 0, 0), "34'")}, kFav, kT + 180).has_value());
  GoalDetector b;
  b.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT);
  CHECK(b.update({at(fav_home(MatchState::Live, 0, 0), "10'")}, kFav, kT + 180).has_value());
}

TEST_CASE("recency guard: after a long outage kick-off and full time are taken silently") {
  // Last seen before kick-off, next seen live 40 minutes later.
  GoalDetector k;
  k.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT);
  CHECK_FALSE(k.update({at(fav_home(MatchState::Live, 0, 0), "1'")}, kFav, kT + 40 * 60).has_value());
  // Last seen live, next seen finished an hour later: no whistle, no win.
  GoalDetector w;
  w.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT);
  CHECK_FALSE(w.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT + 3600).has_value());
  CHECK_FALSE(w.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT + 3625).has_value());
  // Exactly at the limit still counts; a poll from the past (a clock step back) does not.
  GoalDetector e;
  e.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT);
  CHECK(e.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT + GoalDetector::kRecentSeconds).has_value());
  GoalDetector p;
  p.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT);
  CHECK_FALSE(p.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT - 10).has_value());
}

TEST_CASE("recency guard: a replay of the same finished match is first sight, then silent") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT);
  REQUIRE(d.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT + 30).has_value());
  // The match drops out of the list and comes back (an old answer replayed).
  d.update({}, kFav, kT + 60);
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 2, 1)}, kFav, kT + 90).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 1)}, kFav, kT + 120).has_value());
}

TEST_CASE("kick-off and full time only for the favourite's matches") {
  GoalDetector d;
  auto other = [](MatchState s) { return make("m9", "300", "400", s, 0, 0); };
  d.update({other(MatchState::Pre)}, kFav, kT);
  CHECK_FALSE(d.update({at(other(MatchState::Live), "1'")}, kFav, kT + 30).has_value());
  CHECK_FALSE(d.update({other(MatchState::Post)}, kFav, kT + 60).has_value());
}

TEST_CASE("clock_minute reads the minutes of a match clock") {
  CHECK(clock_minute("67'") == 67);
  CHECK(clock_minute("1'") == 1);
  CHECK(clock_minute("45'+2'") == 45);
  CHECK(clock_minute("90'+4'") == 90);
  CHECK(clock_minute("HT") == -1);
  CHECK(clock_minute("FT") == -1);
  CHECK(clock_minute("") == -1);
  CHECK(clock_minute("12") == -1);
  CHECK(clock_minute("1234'") == -1);
}

TEST_CASE("recency guard: one failed poll at the idle cadence still whistles (two idle polls apart)") {
  CHECK(GoalDetector::kRecentSeconds == 420);
  // Pre seen; the next poll (180 s later) failed; the one after (360 s plus a
  // slow request) shows the match live: still a kick-off.
  GoalDetector k;
  k.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav, kT);
  auto e = k.update({at(fav_home(MatchState::Live, 0, 0), "5'")}, kFav, kT + 2 * 180 + 8);
  REQUIRE(e.has_value());
  CHECK(e->kickOff);
  // The same for the end of a match.
  GoalDetector f;
  f.update({fav_home(MatchState::Live, 1, 1)}, kFav, kT);
  auto ft = f.update({fav_home(MatchState::Post, 1, 1)}, kFav, kT + 2 * 180 + 8);
  REQUIRE(ft.has_value());
  CHECK(ft->fullTime);
  // Beyond the limit: silent.
  GoalDetector g;
  g.update({fav_home(MatchState::Live, 1, 1)}, kFav, kT);
  CHECK_FALSE(g.update({fav_home(MatchState::Post, 1, 1)}, kFav, kT + GoalDetector::kRecentSeconds + 1).has_value());
}
