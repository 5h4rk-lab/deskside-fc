// The demo data the firmware's demo build shares with the simulator: loading
// it from JSON text and the goal / full-time changes the serial command `g`
// and the simulator's keys make.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "demo_data.h"
#include "fixture_source.h"
#include "mock_source.h"

namespace {

constexpr int64_t kNow = 1792526400;

MatchSummary live_of(MockSource& source, const std::string& team) {
  MatchSummary out;
  source.fetch_matches([&](bool, std::vector<MatchSummary> ms) {
    for (const MatchSummary& m : ms) {
      if (m.state == MatchState::Live && (m.home.id == team || m.away.id == team)) out = m;
    }
  });
  return out;
}

MatchDetail detail_of(MockSource& source, const std::string& id) {
  MatchDetail d;
  source.fetch_detail(id, [&](bool, MatchDetail x) { d = std::move(x); });
  return d;
}

struct Loaded {
  MockSource source;
  Loaded() {
    demo_load(source, kNow, fixture_text("news-normalised.json"), fixture_text("teams-normalised.json"));
  }
};

}  // namespace

TEST_CASE("demo_load fills matches, details, news and teams from the JSON text") {
  Loaded d;
  std::vector<MatchSummary> matches;
  d.source.fetch_matches([&](bool ok, std::vector<MatchSummary> ms) {
    CHECK(ok);
    matches = std::move(ms);
  });
  CHECK(matches.size() == 11);
  std::vector<NewsItem> news;
  d.source.fetch_news([&](bool, std::vector<NewsItem> n) { news = std::move(n); });
  CHECK(news.size() == 6);
  CHECK(news.front().published == kNow - 25 * 60);
  std::vector<Team> teams;
  d.source.fetch_teams([&](bool, std::vector<Team> t) { teams = std::move(t); });
  CHECK(teams.size() > 20);
  CHECK(detail_of(d.source, demo::kLiveMatchId).events.size() == 5);
}

TEST_CASE("demo_load without JSON text still loads the matches, with no news or teams") {
  MockSource source;
  demo_load(source, kNow, "", "");
  CHECK(live_of(source, demo::kFavouriteId).id == demo::kLiveMatchId);
  std::vector<NewsItem> news;
  source.fetch_news([&](bool, std::vector<NewsItem> n) { news = std::move(n); });
  CHECK(news.empty());
  std::vector<Team> teams;
  source.fetch_teams([&](bool, std::vector<Team> t) { teams = std::move(t); });
  CHECK(teams.size() == 18);  // only the built-in clubs
}

TEST_CASE("demo_goal: the favourite scores, with a scorer who has a photo, in turn") {
  Loaded d;
  CHECK(live_of(d.source, "359").homeScore.value() == 2);
  REQUIRE(demo_goal(d.source, "359"));
  CHECK(live_of(d.source, "359").homeScore.value() == 3);
  MatchDetail first = detail_of(d.source, demo::kLiveMatchId);
  REQUIRE(first.events.size() == 6);
  CHECK(first.events.back().type == EventType::Goal);
  CHECK(first.events.back().team == Side::Home);
  const std::string firstScorer = first.events.back().player;
  REQUIRE(demo_goal(d.source, "359"));
  MatchDetail second = detail_of(d.source, demo::kLiveMatchId);
  CHECK(second.events.back().player != firstScorer);
}

TEST_CASE("demo_goal: false without a live match of the favourite") {
  Loaded d;
  CHECK_FALSE(demo_goal(d.source, "no-such-team"));
  MockSource empty;
  CHECK_FALSE(demo_goal(empty, "359"));
}

TEST_CASE("demo_win: full time when the favourite is ahead") {
  Loaded d;
  CHECK(demo_win(d.source, "359") == DemoWin::Ok);
  CHECK(live_of(d.source, "359").id.empty());  // no longer live
  CHECK(demo_win(d.source, "359") == DemoWin::NoLiveMatch);
}

TEST_CASE("demo_win: nothing changes when the favourite is behind") {
  Loaded d;
  CHECK(demo_win(d.source, "363") == DemoWin::NotAhead);  // Chelsea trail 1:2
  CHECK(live_of(d.source, "363").id == demo::kLiveMatchId);
}
