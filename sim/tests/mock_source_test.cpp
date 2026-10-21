#include <doctest/doctest.h>

#include "fixture_source.h"
#include "mock_source.h"

TEST_CASE("MockSource serves the fixture by default") {
  FixtureSource src(CONTRACT_FIXTURES_DIR);
  bool called = false;
  src.fetch_matches([&](bool ok, std::vector<MatchSummary> v) {
    called = true;
    CHECK(ok);
    REQUIRE(v.size() == 1);
    CHECK(v[0].id == "401879257");
    CHECK(v[0].homeScore == 3);
  });
  CHECK(called);

  called = false;
  src.fetch_detail("401879257", [&](bool ok, MatchDetail d) {
    called = true;
    CHECK(ok);
    CHECK_FALSE(d.match.id.empty());
  });
  CHECK(called);

  called = false;
  src.fetch_news([&](bool ok, std::vector<NewsItem> v) {
    called = true;
    CHECK(ok);
    CHECK_FALSE(v.empty());
  });
  CHECK(called);

  called = false;
  src.fetch_teams([&](bool ok, std::vector<Team> v) {
    called = true;
    CHECK(ok);
    CHECK_FALSE(v.empty());
  });
  CHECK(called);
}

TEST_CASE("MockSource set_fail gives ok=false and empty data") {
  FixtureSource src(CONTRACT_FIXTURES_DIR);
  src.set_fail(true);
  int calls = 0;
  src.fetch_matches([&](bool ok, std::vector<MatchSummary> v) {
    ++calls;
    CHECK_FALSE(ok);
    CHECK(v.empty());
  });
  src.fetch_detail("x", [&](bool ok, MatchDetail d) {
    ++calls;
    CHECK_FALSE(ok);
    CHECK(d.match.id.empty());
  });
  src.fetch_news([&](bool ok, std::vector<NewsItem> v) {
    ++calls;
    CHECK_FALSE(ok);
    CHECK(v.empty());
  });
  src.fetch_teams([&](bool ok, std::vector<Team> v) {
    ++calls;
    CHECK_FALSE(ok);
    CHECK(v.empty());
  });
  CHECK(calls == 4);
  src.set_fail(false);
  src.fetch_matches([&](bool ok, std::vector<MatchSummary>) { CHECK(ok); });
}

TEST_CASE("MockSource advance_score increments and starts a Pre match") {
  FixtureSource src(CONTRACT_FIXTURES_DIR);
  MatchSummary m;
  m.id = "p";
  m.state = MatchState::Pre;
  src.set_matches({m});

  src.advance_score("p", Side::Home);
  src.fetch_matches([&](bool, std::vector<MatchSummary> v) {
    REQUIRE(v.size() == 1);
    CHECK(v[0].state == MatchState::Live);
    CHECK(v[0].homeScore == 1);
    CHECK(v[0].awayScore == 0);
  });
  src.advance_score("p", Side::Away);
  src.advance_score("p", Side::Away);
  src.advance_score("nope", Side::Away);  // unknown id: ignored
  src.fetch_matches([&](bool, std::vector<MatchSummary> v) {
    CHECK(v[0].homeScore == 1);
    CHECK(v[0].awayScore == 2);
  });
}

TEST_CASE("MockSource returns scripted detail, news and teams") {
  FixtureSource src(CONTRACT_FIXTURES_DIR);
  MatchDetail d;
  d.match.id = "scripted";
  src.set_detail(d);
  src.fetch_detail("scripted", [](bool ok, MatchDetail got) {
    CHECK(ok);
    CHECK(got.match.id == "scripted");
  });
  NewsItem n;
  n.id = "nn";
  src.set_news({n});
  src.fetch_news([](bool, std::vector<NewsItem> v) {
    REQUIRE(v.size() == 1);
    CHECK(v[0].id == "nn");
  });
  Team t;
  t.id = "tt";
  src.set_teams({t});
  src.fetch_teams([](bool, std::vector<Team> v) {
    REQUIRE(v.size() == 1);
    CHECK(v[0].id == "tt");
  });
}
