#include "demo_data.h"

#include <utility>

#include "format.h"
#include "mock_source.h"

namespace {

constexpr int64_t kDay = 86400;

std::string espn_crest(const char* id) {
  return std::string("/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fi%2Fteamlogos%2Fsoccer%2F500%2F") +
         id + ".png&w=48&h=48";
}

std::string fd_crest(const char* id) {
  return std::string("/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F") + id +
         ".png&w=48&h=48";
}

// Ids are ESPN team ids. Crests use the ESPN logo when sim/assets has it,
// otherwise the football-data.org crest. Colours are {accent, primary} as the
// helper sends them (helper/src/colors.ts): Chelsea's accent is gold, its
// primary royal blue; Tottenham's accent is white, its primary navy.
// Abbreviations and colours follow the helper's tables (helper/src/colors.ts)
// and contract/fixtures/teams-normalised.json: Bayern is MUN and Marseille OLM
// as ESPN has them. Names stay short for the demo rows.
Team arsenal() { return {"359", "ARS", "Arsenal", espn_crest("359"), 0xEF0107, 0xEF0107, ""}; }
Team chelsea() { return {"363", "CHE", "Chelsea", espn_crest("363"), 0xDBA111, 0x034694, ""}; }
Team man_city() { return {"382", "MNC", "Manchester City", espn_crest("382"), 0x6CABDD, 0x6CABDD, ""}; }
Team liverpool() { return {"364", "LIV", "Liverpool", espn_crest("364"), 0xC8102E, 0xC8102E, ""}; }
Team real_madrid() { return {"86", "RMA", "Real Madrid", fd_crest("86"), 0xFFFFFF, 0x00529F, ""}; }
Team barcelona() { return {"83", "BAR", "Barcelona", fd_crest("81"), 0xEDBB00, 0xA50044, ""}; }
Team bayern() { return {"132", "MUN", "Bayern Munich", fd_crest("5"), 0xDC052D, 0xDC052D, ""}; }
Team dortmund() { return {"124", "DOR", "Borussia Dortmund", fd_crest("4"), 0xFDE100, 0xFDE100, ""}; }
Team psg() { return {"160", "PSG", "Paris Saint-Germain", fd_crest("524"), 0xDA291C, 0x004170, ""}; }
Team inter() { return {"110", "INT", "Inter Milan", fd_crest("108"), 0x0068A8, 0x0068A8, ""}; }
Team milan() { return {"103", "MIL", "AC Milan", fd_crest("98"), 0xFB090B, 0xFB090B, ""}; }
Team atletico() { return {"1068", "ATM", "Atletico Madrid", fd_crest("78"), 0xCB3524, 0xCB3524, ""}; }
Team sevilla() { return {"243", "SEV", "Sevilla", fd_crest("559"), 0xD81022, 0xD81022, ""}; }
Team brighton() { return {"331", "BHA", "Brighton", espn_crest("331"), 0x0057B8, 0x0057B8, ""}; }
Team tottenham() { return {"367", "TOT", "Tottenham Hotspur", espn_crest("367"), 0xFFFFFF, 0x132257, ""}; }
Team marseille() { return {"176", "OLM", "Marseille", fd_crest("516"), 0x2FAEE0, 0x2FAEE0, ""}; }

MatchSummary match(const char* id, const char* league, MatchState state, const char* clock,
                   int64_t kickoff, Team home, Team away) {
  MatchSummary m;
  m.id = id;
  m.league = league;
  m.state = state;
  m.clock = clock;
  m.kickoff = kickoff;
  m.home = std::move(home);
  m.away = std::move(away);
  return m;
}

}  // namespace

std::vector<MatchSummary> demo_matches(int64_t now) {
  const int64_t today = floor_div(now, kDay) * kDay;  // 00:00 UTC
  // 1970-01-01 was a Thursday; 0 = Sunday ... 6 = Saturday.
  const int64_t weekday = (floor_div(now, kDay) + 4) % 7;
  int64_t toSaturday = (6 - weekday + 7) % 7;
  if (toSaturday == 0) toSaturday = 7;

  std::vector<MatchSummary> out;

  MatchSummary live = match(demo::kLiveMatchId, "eng.1", MatchState::Live, "67'",
                            now - 67 * 60, arsenal(), chelsea());
  live.homeScore = 2;
  live.awayScore = 1;
  out.push_back(live);

  out.push_back(match("demo-rma-bar", "esp.1", MatchState::Pre, "", today + 19 * 3600,
                      real_madrid(), barcelona()));
  out.push_back(match("demo-mnc-liv", "eng.1", MatchState::Pre, "", today + kDay + 16 * 3600 + 1800,
                      man_city(), liverpool()));
  out.push_back(match("demo-bay-dor", "ger.1", MatchState::Pre, "",
                      today + toSaturday * kDay + 17 * 3600 + 1800, bayern(), dortmund()));

  MatchSummary post = match("demo-psg-mar", "fra.1", MatchState::Post, "FT",
                            today - kDay + 19 * 3600, psg(), marseille());
  post.homeScore = 3;
  post.awayScore = 1;
  out.push_back(post);

  // More rows, so the Matches list needs scrolling.
  out.push_back(match("demo-int-mil", "ita.1", MatchState::Pre, "", today + 21 * 3600, inter(),
                      milan()));
  out.push_back(match("demo-atm-sev", "esp.1", MatchState::Pre, "",
                      today + 2 * kDay + 20 * 3600, atletico(), sevilla()));
  out.push_back(match("demo-bha-liv", "eng.1", MatchState::Postponed, "",
                      today + kDay + 15 * 3600, brighton(), liverpool()));
  MatchSummary old = match("demo-rma-atm", "esp.1", MatchState::Post, "FT",
                           today - 3 * kDay + 20 * 3600, real_madrid(), atletico());
  old.homeScore = 1;
  old.awayScore = 0;
  out.push_back(old);

  // The favourite's last result and next fixture, for the News tiles.
  MatchSummary last = match("demo-ars-mnc", "eng.1", MatchState::Post, "FT",
                            today - 3 * kDay + 15 * 3600, arsenal(), man_city());
  last.homeScore = 3;
  last.awayScore = 1;
  out.push_back(last);
  out.push_back(match("demo-tot-ars", "eng.1", MatchState::Pre, "",
                      today + toSaturday * kDay + 17 * 3600 + 1800, tottenham(), arsenal()));
  return out;
}

std::vector<NewsItem> demo_news(int64_t now) {
  std::vector<NewsItem> items;
  MockSource source(CONTRACT_FIXTURES_DIR);
  source.fetch_news([&](bool ok, std::vector<NewsItem> news) {
    if (ok) items = std::move(news);
  });
  const int64_t ages[] = {25 * 60, 2 * 3600, 5 * 3600, kDay + 2 * 3600, 3 * kDay, 9 * kDay};
  for (size_t i = 0; i < items.size() && i < sizeof(ages) / sizeof(ages[0]); ++i) {
    items[i].published = now - ages[i];
  }
  return items;
}

namespace {

Team league_team(const char* id, const char* abbr, const char* name, std::string crest,
                 uint32_t color, uint32_t primary, const char* league) {
  return {id, abbr, name, std::move(crest), color, primary, league};
}

}  // namespace

std::vector<Team> demo_teams() {
  std::vector<Team> teams;
  MockSource source(CONTRACT_FIXTURES_DIR);
  source.fetch_teams([&](bool ok, std::vector<Team> t) {
    if (ok) teams = std::move(t);
  });
  const Team more[] = {
      league_team("86", "RMA", "Real Madrid", fd_crest("86"), 0xFFFFFF, 0x00529F, "esp.1"),
      league_team("83", "BAR", "Barcelona", fd_crest("81"), 0xEDBB00, 0xA50044, "esp.1"),
      league_team("1068", "ATM", "Atl\xC3\xA9tico Madrid", fd_crest("78"), 0xCB3524, 0xCB3524, "esp.1"),
      league_team("243", "SEV", "Sevilla", fd_crest("559"), 0xD81022, 0xD81022, "esp.1"),
      league_team("93", "ATH", "Athletic Club", espn_crest("93"), 0xEE2523, 0xEE2523, "esp.1"),
      league_team("89", "RSO", "Real Sociedad", espn_crest("89"), 0x0067B1, 0x0067B1, "esp.1"),
      league_team("110", "INT", "Internazionale", fd_crest("108"), 0x0068A8, 0x0068A8, "ita.1"),
      league_team("103", "MIL", "AC Milan", fd_crest("98"), 0xFB090B, 0xFB090B, "ita.1"),
      league_team("111", "JUV", "Juventus", espn_crest("111"), 0xFFFFFF, 0x000000, "ita.1"),
      league_team("114", "NAP", "Napoli", espn_crest("114"), 0x12A0D7, 0x12A0D7, "ita.1"),
      league_team("132", "MUN", "Bayern Munich", fd_crest("5"), 0xDC052D, 0xDC052D, "ger.1"),
      league_team("124", "DOR", "Borussia Dortmund", fd_crest("4"), 0xFDE100, 0xFDE100, "ger.1"),
      league_team("131", "B04", "Bayer Leverkusen", espn_crest("131"), 0xE32221, 0xE32221, "ger.1"),
      league_team("11420", "RBL", "RB Leipzig", espn_crest("11420"), 0xDD0741, 0xDD0741, "ger.1"),
      league_team("160", "PSG", "Paris Saint-Germain", fd_crest("524"), 0xDA291C, 0x004170, "fra.1"),
      league_team("176", "OLM", "Marseille", fd_crest("516"), 0x2FAEE0, 0x2FAEE0, "fra.1"),
      league_team("167", "LYON", "Lyon", espn_crest("167"), 0xD8232A, 0x14387F, "fra.1"),
      league_team("174", "MON", "AS Monaco", espn_crest("174"), 0xE51B22, 0xE51B22, "fra.1"),
  };
  for (const Team& t : more) teams.push_back(t);
  return teams;
}

namespace {

LineupPlayer player(const char* name, int number, float x, float y, bool photo = false) {
  LineupPlayer p;
  p.name = name;
  p.number = number;
  p.x = x;
  p.y = y;
  if (photo) p.photo = "/api/cutout?name=" + url_encode(name) + "&w=40&h=40";
  return p;
}

// Contract convention: y = 0 is the team's own goal line and y grows towards
// the halfway line (the helper puts a formation's lines evenly over
// 0.08..0.92); x runs
// 0..1 left to right as the team sees the pitch when attacking.
Lineup arsenal_lineup() {
  return {"4-3-3",
          {
              player("David Raya", 22, 0.50f, 0.08f),
              player("Riccardo Calafiori", 33, 0.14f, 0.36f),
              player("Gabriel", 6, 0.38f, 0.36f),
              player("William Saliba", 2, 0.62f, 0.36f),
              player("Jurrien Timber", 12, 0.86f, 0.36f),
              player("Declan Rice", 41, 0.25f, 0.64f, true),
              player("Martin Zubimendi", 36, 0.50f, 0.64f),
              player("Martin Odegaard", 8, 0.75f, 0.64f),
              player("Gabriel Martinelli", 11, 0.18f, 0.92f),
              player("Kai Havertz", 29, 0.50f, 0.92f, true),
              player("Bukayo Saka", 7, 0.82f, 0.92f, true),
          }};
}

Lineup chelsea_lineup() {
  return {"4-2-3-1",
          {
              player("Robert Sanchez", 1, 0.50f, 0.08f),
              player("Marc Cucurella", 3, 0.14f, 0.29f),
              player("Levi Colwill", 6, 0.38f, 0.29f),
              player("Wesley Fofana", 29, 0.62f, 0.29f),
              player("Reece James", 24, 0.86f, 0.29f),
              player("Enzo Fernandez", 8, 0.33f, 0.50f),
              player("Moises Caicedo", 25, 0.67f, 0.50f),
              player("Noni Madueke", 11, 0.20f, 0.71f),
              player("Cole Palmer", 10, 0.50f, 0.71f, true),
              player("Pedro Neto", 7, 0.80f, 0.71f),
              player("Liam Delap", 9, 0.50f, 0.92f),
          }};
}

}  // namespace

MatchDetail demo_detail(const std::string& matchId) {
  MatchDetail d;
  const std::vector<MatchSummary> matches = demo_matches(0);
  if (matchId == demo::kPreMatchId) {
    d.match = matches[1];
    return d;
  }
  if (matchId != demo::kLiveMatchId) {
    d.match.id = matchId;
    return d;
  }
  d.match = matches.front();
  d.events = {
      {"12'", EventType::Goal, Side::Home, "Bukayo Saka", "Assist: Martin Odegaard"},
      {"34'", EventType::Goal, Side::Away, "Cole Palmer", ""},
      {"45'+2'", EventType::Yellow, Side::Away, "Moises Caicedo", ""},
      {"46'", EventType::Sub, Side::Away, "Jadon Sancho", "For: Noni Madueke"},
      {"58'", EventType::Goal, Side::Home, "Kai Havertz", "Assist: Declan Rice"},
  };
  d.home = arsenal_lineup();
  d.away = chelsea_lineup();
  d.stats = {
      {"Possession", "58%", "42%"},
      {"Shots", "11", "7"},
      {"Shots on target", "5", "3"},
      {"Corners", "6", "2"},
      {"Fouls", "9", "12"},
  };
  return d;
}
