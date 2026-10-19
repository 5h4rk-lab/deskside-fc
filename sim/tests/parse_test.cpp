#include <doctest/doctest.h>

#include <ArduinoJson.h>

#include <fstream>
#include <sstream>
#include <string>

#include "json_parse.h"
#include "model.h"
#include "theme.h"

namespace {

std::string read_fixture(const char* name) {
  std::ifstream in(std::string(CONTRACT_FIXTURES_DIR) + "/" + name, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open fixture ", name);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

JsonDocument load_doc(const char* name) {
  JsonDocument doc;
  REQUIRE(deserializeJson(doc, read_fixture(name)) == DeserializationError::Ok);
  return doc;
}

std::string dump(const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

}  // namespace

TEST_CASE("parse_matches reads the shared fixture") {
  JsonDocument doc = load_doc("matches-normalised.json");
  std::vector<MatchSummary> matches;
  int64_t updated = 0;
  REQUIRE(parse_matches(dump(doc), matches, updated));

  CHECK(updated == doc["updated"].as<int64_t>());
  REQUIRE(matches.size() == doc["matches"].size());
  REQUIRE(matches.size() > 0);

  JsonObject first = doc["matches"][0];
  const MatchSummary& m = matches[0];
  CHECK(m.id == first["id"].as<std::string>());
  CHECK(m.league == first["league"].as<std::string>());
  CHECK(m.state == MatchState::Post);
  CHECK(m.clock.empty());
  CHECK(m.kickoff == first["kickoff"].as<int64_t>());
  CHECK(m.home.id == first["home"]["id"].as<std::string>());
  CHECK(m.home.abbr == first["home"]["abbr"].as<std::string>());
  CHECK(m.home.name == first["home"]["name"].as<std::string>());
  CHECK(m.home.crest == first["home"]["crest"].as<std::string>());
  CHECK(m.home.league.empty());
  CHECK(m.home.color == parse_hex(first["home"]["color"].as<const char*>()));
  CHECK(m.away.color == parse_hex(first["away"]["color"].as<const char*>()));
  REQUIRE(m.homeScore.has_value());
  REQUIRE(m.awayScore.has_value());
  CHECK(*m.homeScore == first["homeScore"].as<int>());
  CHECK(*m.awayScore == first["awayScore"].as<int>());
}

TEST_CASE("parse_matches maps null scores to nullopt and keeps the clock") {
  JsonDocument doc = load_doc("matches-normalised.json");
  doc["matches"][0]["state"] = "live";
  doc["matches"][0]["clock"] = "63'";
  doc["matches"][0]["homeScore"] = nullptr;
  doc["matches"][0]["awayScore"] = nullptr;
  std::vector<MatchSummary> matches;
  int64_t updated = 0;
  REQUIRE(parse_matches(dump(doc), matches, updated));
  CHECK(matches[0].state == MatchState::Live);
  CHECK(matches[0].clock == "63'");
  CHECK_FALSE(matches[0].homeScore.has_value());
  CHECK_FALSE(matches[0].awayScore.has_value());
}

TEST_CASE("parse_matches accepts every state") {
  struct Case {
    const char* name;
    MatchState state;
  };
  const Case cases[] = {{"pre", MatchState::Pre},
                        {"live", MatchState::Live},
                        {"post", MatchState::Post},
                        {"postponed", MatchState::Postponed}};
  for (const Case& c : cases) {
    JsonDocument doc = load_doc("matches-normalised.json");
    doc["matches"][0]["state"] = c.name;
    std::vector<MatchSummary> matches;
    int64_t updated = 0;
    REQUIRE(parse_matches(dump(doc), matches, updated));
    CHECK(matches[0].state == c.state);
  }
}

TEST_CASE("parse_detail reads the shared fixture") {
  JsonDocument doc = load_doc("detail-normalised.json");
  MatchDetail d;
  REQUIRE(parse_detail(dump(doc), d));

  CHECK(d.match.id == doc["match"]["id"].as<std::string>());
  CHECK(d.match.state == MatchState::Post);
  CHECK(d.home.formation == doc["home"]["formation"].as<std::string>());
  CHECK(d.home.players.size() == 11);
  CHECK(d.away.players.size() == 11);
  for (const Lineup* l : {&d.home, &d.away}) {
    for (const LineupPlayer& p : l->players) {
      CHECK(p.x >= 0.0f);
      CHECK(p.x <= 1.0f);
      CHECK(p.y >= 0.0f);
      CHECK(p.y <= 1.0f);
      CHECK_FALSE(p.name.empty());
    }
  }
  CHECK(d.home.players[0].number == doc["home"]["players"][0]["number"].as<int>());
  CHECK(d.home.players[0].photo == doc["home"]["players"][0]["photo"].as<std::string>());

  REQUIRE(d.events.size() == doc["events"].size());
  bool sawGoal = false;
  for (const MatchEvent& e : d.events) {
    if (e.type != EventType::Goal) continue;
    sawGoal = true;
    REQUIRE_FALSE(e.minute.empty());
    CHECK(e.minute.back() == '\'');
  }
  CHECK(sawGoal);
  CHECK(d.events[0].team == Side::Home);
  CHECK(d.events[0].detail == doc["events"][0]["detail"].as<std::string>());
  CHECK(d.events[1].type == EventType::Yellow);
  CHECK(d.events[1].team == Side::Away);
  CHECK(d.events[1].detail.empty());

  REQUIRE(d.stats.size() == doc["stats"].size());
  CHECK(d.stats[0].label == "Possession");
  CHECK(d.stats[0].home == "65%");
  CHECK(d.stats[0].away == "35%");
}

TEST_CASE("parse_detail accepts a detail with no published lineups") {
  JsonDocument doc = load_doc("detail-normalised.json");
  doc["home"]["players"].to<JsonArray>();
  doc["away"]["players"].to<JsonArray>();
  doc["home"]["formation"] = "";
  doc["away"]["formation"] = "";
  MatchDetail d;
  REQUIRE(parse_detail(dump(doc), d));
  CHECK(d.home.players.empty());
  CHECK(d.away.players.empty());
  CHECK(d.home.formation.empty());
  CHECK(d.away.formation.empty());
  CHECK_FALSE(d.events.empty());
}

TEST_CASE("parse_news reads the shared fixture") {
  JsonDocument doc = load_doc("news-normalised.json");
  std::vector<NewsItem> items;
  int64_t updated = 0;
  REQUIRE(parse_news(dump(doc), items, updated));
  CHECK(updated == doc["updated"].as<int64_t>());
  REQUIRE(items.size() == doc["articles"].size());
  CHECK(items[0].id == doc["articles"][0]["id"].as<std::string>());
  CHECK(items[0].headline == doc["articles"][0]["headline"].as<std::string>());
  CHECK(items[0].published == doc["articles"][0]["published"].as<int64_t>());
  CHECK(items[0].image == doc["articles"][0]["image"].as<std::string>());
}

TEST_CASE("parse_news leaves image empty when null") {
  JsonDocument doc = load_doc("news-normalised.json");
  doc["articles"][0]["image"] = nullptr;
  std::vector<NewsItem> items;
  int64_t updated = 0;
  REQUIRE(parse_news(dump(doc), items, updated));
  CHECK(items[0].image.empty());
  CHECK_FALSE(items[1].image.empty());
}

TEST_CASE("parse_teams reads the shared fixture") {
  JsonDocument doc = load_doc("teams-normalised.json");
  std::vector<Team> teams;
  REQUIRE(parse_teams(dump(doc), teams));
  REQUIRE(teams.size() == doc["teams"].size());
  CHECK(teams.size() == 20);
  for (size_t i = 0; i < teams.size(); ++i) {
    JsonObject t = doc["teams"][i];
    CHECK(teams[i].id == t["id"].as<std::string>());
    CHECK(teams[i].abbr == t["abbr"].as<std::string>());
    CHECK(teams[i].league == "eng.1");
    CHECK(teams[i].color == parse_hex(t["color"].as<const char*>()));
  }
}

TEST_CASE("parse_matches rejects bad input and leaves the output untouched") {
  const std::string good = read_fixture("matches-normalised.json");

  JsonDocument badState = load_doc("matches-normalised.json");
  badState["matches"][0]["state"] = "halftime";
  JsonDocument noHome = load_doc("matches-normalised.json");
  noHome["matches"][0].remove("home");
  JsonDocument noUpdated = load_doc("matches-normalised.json");
  noUpdated.remove("updated");
  // A bad second match must not leave the first one behind.
  JsonDocument badSecond = load_doc("matches-normalised.json");
  badSecond["matches"][1] = badSecond["matches"][0];
  badSecond["matches"][1].remove("kickoff");

  const std::string bodies[] = {good.substr(0, good.size() / 2), "", "[]", "{}", dump(badState),
                                dump(noHome), dump(noUpdated), dump(badSecond)};
  for (const std::string& body : bodies) {
    MatchSummary sentinel;
    sentinel.id = "keep";
    std::vector<MatchSummary> matches{sentinel};
    int64_t updated = 77;
    CHECK_FALSE(parse_matches(body, matches, updated));
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].id == "keep");
    CHECK(updated == 77);
  }
}

TEST_CASE("parse_detail rejects bad input and leaves the output untouched") {
  const std::string good = read_fixture("detail-normalised.json");

  JsonDocument badType = load_doc("detail-normalised.json");
  badType["events"][0]["type"] = "penalty";
  JsonDocument badTeam = load_doc("detail-normalised.json");
  badTeam["events"][0]["team"] = "neither";
  JsonDocument noMatch = load_doc("detail-normalised.json");
  noMatch.remove("match");
  JsonDocument noStats = load_doc("detail-normalised.json");
  noStats.remove("stats");

  const std::string bodies[] = {good.substr(0, good.size() / 2), "", "[]", dump(badType),
                                dump(badTeam), dump(noMatch), dump(noStats)};
  for (const std::string& body : bodies) {
    MatchDetail d;
    d.match.id = "keep";
    CHECK_FALSE(parse_detail(body, d));
    CHECK(d.match.id == "keep");
    CHECK(d.events.empty());
    CHECK(d.home.players.empty());
  }
}

TEST_CASE("parse_news rejects bad input and leaves the output untouched") {
  const std::string good = read_fixture("news-normalised.json");
  JsonDocument noHeadline = load_doc("news-normalised.json");
  noHeadline["articles"][0].remove("headline");

  const std::string bodies[] = {good.substr(0, good.size() / 2), "", "[]", dump(noHeadline)};
  for (const std::string& body : bodies) {
    NewsItem sentinel;
    sentinel.id = "keep";
    std::vector<NewsItem> items{sentinel};
    int64_t updated = 77;
    CHECK_FALSE(parse_news(body, items, updated));
    REQUIRE(items.size() == 1);
    CHECK(items[0].id == "keep");
    CHECK(updated == 77);
  }
}

TEST_CASE("parse_teams rejects bad input and leaves the output untouched") {
  const std::string good = read_fixture("teams-normalised.json");
  JsonDocument noLeague = load_doc("teams-normalised.json");
  noLeague["teams"][0].remove("league");

  const std::string bodies[] = {good.substr(0, good.size() / 2), "", "[]", dump(noLeague)};
  for (const std::string& body : bodies) {
    Team sentinel;
    sentinel.id = "keep";
    std::vector<Team> teams{sentinel};
    CHECK_FALSE(parse_teams(body, teams));
    REQUIRE(teams.size() == 1);
    CHECK(teams[0].id == "keep");
  }
}
