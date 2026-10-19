#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "goal_detector.h"
#include "model.h"

namespace {

const char* kFav = "100";

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
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 1)}, kFav).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 1)}, kFav).has_value());
}

TEST_CASE("favourite scoring fires a favourite event with the new score") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 2, 1)}, kFav);
  auto e = d.update({fav_home(MatchState::Live, 3, 1)}, kFav);
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
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 3, 1)}, kFav).has_value());
}

TEST_CASE("favourite scoring away reports the away side") {
  GoalDetector d;
  auto m = [](int hs, int as) { return make("m2", "300", "100", MatchState::Live, hs, as); };
  d.update({m(0, 0)}, kFav);
  auto e = d.update({m(0, 1)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->side == Side::Away);
}

TEST_CASE("opponent scoring fires a non-favourite event") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 2, 1)}, kFav);
  auto e = d.update({fav_home(MatchState::Live, 2, 2)}, kFav);
  REQUIRE(e.has_value());
  CHECK_FALSE(e->favourite);
  CHECK_FALSE(e->fullTime);
  CHECK(e->side == Side::Away);
  CHECK(e->homeScore == 2);
  CHECK(e->awayScore == 2);
}

TEST_CASE("a match first seen Pre baselines at 0:0 and fires on the first goal") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 0, 0)}, kFav).has_value());
  auto e = d.update({fav_home(MatchState::Live, 1, 0)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("a Pre match going straight to a score still fires") {
  GoalDetector d;
  d.update({fav_home(MatchState::Pre, std::nullopt, std::nullopt)}, kFav);
  auto e = d.update({fav_home(MatchState::Live, 1, 0)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("matches without the favourite never fire") {
  GoalDetector d;
  auto m = [](int hs, int as) { return make("x", "300", "400", MatchState::Live, hs, as); };
  d.update({m(0, 0)}, kFav);
  CHECK_FALSE(d.update({m(1, 0)}, kFav).has_value());
  CHECK_FALSE(d.update({m(1, 1)}, kFav).has_value());
}

TEST_CASE("a score decrease is silent and the next increase fires again") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 3, 1)}, kFav);
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 1)}, kFav).has_value());
  auto e = d.update({fav_home(MatchState::Live, 3, 1)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("several goals in one poll give one event with the current score") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 0, 0)}, kFav);
  auto e = d.update({fav_home(MatchState::Live, 2, 0)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->homeScore == 2);
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 0)}, kFav).has_value());
}

TEST_CASE("both teams scoring in one poll returns the favourite's event") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 1)}, kFav);
  auto e = d.update({fav_home(MatchState::Live, 2, 2)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->side == Side::Home);
  // The opponent's goal was absorbed into the baseline.
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 2)}, kFav).has_value());
}

TEST_CASE("full time with the favourite ahead fires once") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 2, 1)}, kFav);
  auto e = d.update({fav_home(MatchState::Post, 2, 1)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->fullTime);
  CHECK(e->favourite);
  CHECK(e->side == Side::Home);
  CHECK(e->matchId == "m1");
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 2, 1)}, kFav).has_value());
}

TEST_CASE("full time with the favourite ahead away reports the away side") {
  GoalDetector d;
  auto m = [](MatchState s) { return make("m2", "300", "100", s, 0, 2); };
  d.update({m(MatchState::Live)}, kFav);
  auto e = d.update({m(MatchState::Post)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->fullTime);
  CHECK(e->side == Side::Away);
}

TEST_CASE("full time level or behind is silent") {
  {
    GoalDetector d;
    d.update({fav_home(MatchState::Live, 1, 1)}, kFav);
    CHECK_FALSE(d.update({fav_home(MatchState::Post, 1, 1)}, kFav).has_value());
  }
  {
    GoalDetector d;
    d.update({fav_home(MatchState::Live, 0, 2)}, kFav);
    CHECK_FALSE(d.update({fav_home(MatchState::Post, 0, 2)}, kFav).has_value());
  }
}

TEST_CASE("a match first seen Post is silent") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 3, 0)}, kFav).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 3, 0)}, kFav).has_value());
}

TEST_CASE("postponed matches never fire") {
  GoalDetector d;
  CHECK_FALSE(d.update({fav_home(MatchState::Postponed, std::nullopt, std::nullopt)}, kFav).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Postponed, std::nullopt, std::nullopt)}, kFav).has_value());
}

TEST_CASE("matches not involving the favourite are ignored alongside the favourite's") {
  GoalDetector d;
  auto other = [](int hs) { return make("o", "300", "400", MatchState::Live, hs, 0); };
  d.update({other(0), fav_home(MatchState::Live, 0, 0)}, kFav);
  auto e = d.update({other(1), fav_home(MatchState::Live, 1, 0)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->matchId == "m1");
  // Only the other match scores: nothing.
  CHECK_FALSE(d.update({other(2), fav_home(MatchState::Live, 1, 0)}, kFav).has_value());
}

// A team can appear in two matches in test data, which exercises the ranking
// across matches involving the favourite.
TEST_CASE("a favourite goal outranks an opponent goal in another match") {
  GoalDetector d;
  auto a = [](int as) { return make("a", "100", "200", MatchState::Live, 0, as); };
  auto b = [](int hs) { return make("b", "100", "300", MatchState::Live, hs, 0); };
  d.update({a(0), b(0)}, kFav);
  // Opponent scores in the first-listed match, favourite in the second.
  auto e = d.update({a(1), b(1)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->matchId == "b");
  // Baselines of both matches were updated: nothing more to report.
  CHECK_FALSE(d.update({a(1), b(1)}, kFav).has_value());
}

TEST_CASE("at the same rank the earlier match in the list wins") {
  GoalDetector d;
  auto a = [](int hs) { return make("a", "100", "200", MatchState::Live, hs, 0); };
  auto b = [](int hs) { return make("b", "100", "300", MatchState::Live, hs, 0); };
  d.update({a(0), b(0)}, kFav);
  auto e = d.update({a(1), b(1)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->matchId == "a");
  CHECK_FALSE(d.update({a(1), b(1)}, kFav).has_value());

  GoalDetector d2;
  auto c = [](int as) { return make("c", "100", "200", MatchState::Live, 0, as); };
  auto f = [](int as) { return make("f", "100", "300", MatchState::Live, 0, as); };
  d2.update({c(0), f(0)}, kFav);
  auto e2 = d2.update({c(1), f(1)}, kFav);
  REQUIRE(e2.has_value());
  CHECK_FALSE(e2->favourite);
  CHECK(e2->matchId == "c");
}

TEST_CASE("switching the favourite drops baselines so the return poll is silent") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, "100");
  d.update({fav_home(MatchState::Live, 1, 0)}, "200");
  // Score changed while "100" was not the favourite.
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 0)}, "100").has_value());
  auto e = d.update({fav_home(MatchState::Live, 3, 0)}, "100");
  REQUIRE(e.has_value());
  CHECK(e->favourite);
  CHECK(e->homeScore == 3);
}

TEST_CASE("a match missing from a poll is first seen again when it returns") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, kFav);
  CHECK_FALSE(d.update({}, kFav).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Live, 2, 0)}, kFav).has_value());
  auto e = d.update({fav_home(MatchState::Live, 3, 0)}, kFav);
  REQUIRE(e.has_value());
  CHECK(e->favourite);
}

TEST_CASE("a goal in the poll that ends the match is reported, then full time once") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, kFav);
  auto goal = d.update({fav_home(MatchState::Post, 2, 0)}, kFav);
  REQUIRE(goal.has_value());
  CHECK_FALSE(goal->fullTime);
  CHECK(goal->favourite);
  CHECK(goal->homeScore == 2);
  auto ft = d.update({fav_home(MatchState::Post, 2, 0)}, kFav);
  REQUIRE(ft.has_value());
  CHECK(ft->fullTime);
  CHECK(ft->favourite);
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 2, 0)}, kFav).has_value());
}

TEST_CASE("an equaliser in the poll that ends the match means no full time later") {
  GoalDetector d;
  d.update({fav_home(MatchState::Live, 1, 0)}, kFav);
  auto goal = d.update({fav_home(MatchState::Post, 1, 1)}, kFav);
  REQUIRE(goal.has_value());
  CHECK_FALSE(goal->favourite);
  CHECK_FALSE(goal->fullTime);
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 1, 1)}, kFav).has_value());
  CHECK_FALSE(d.update({fav_home(MatchState::Post, 1, 1)}, kFav).has_value());
}
