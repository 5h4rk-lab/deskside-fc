#include "demo_data.h"

#include "format.h"

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
// otherwise the football-data.org crest. Arsenal and Chelsea colours come
// from contract/fixtures/teams-normalised.json.
Team arsenal() { return {"359", "ARS", "Arsenal", espn_crest("359"), 0xEF0107, ""}; }
Team chelsea() { return {"363", "CHE", "Chelsea", espn_crest("363"), 0xDBA111, ""}; }
Team man_city() { return {"382", "MNC", "Manchester City", espn_crest("382"), 0x6CABDD, ""}; }
Team liverpool() { return {"364", "LIV", "Liverpool", espn_crest("364"), 0xC8102E, ""}; }
Team real_madrid() { return {"86", "RMA", "Real Madrid", fd_crest("86"), 0xFEBE10, ""}; }
Team barcelona() { return {"83", "BAR", "Barcelona", fd_crest("81"), 0xA50044, ""}; }
Team bayern() { return {"132", "BAY", "Bayern Munich", fd_crest("5"), 0xDC052D, ""}; }
Team dortmund() { return {"124", "DOR", "Borussia Dortmund", fd_crest("4"), 0xFDE100, ""}; }
Team psg() { return {"160", "PSG", "Paris Saint-Germain", fd_crest("524"), 0x004170, ""}; }
Team inter() { return {"110", "INT", "Inter Milan", fd_crest("108"), 0x0068A8, ""}; }
Team milan() { return {"103", "MIL", "AC Milan", fd_crest("98"), 0xFB090B, ""}; }
Team atletico() { return {"1068", "ATM", "Atletico Madrid", fd_crest("78"), 0xCB3524, ""}; }
Team sevilla() { return {"243", "SEV", "Sevilla", fd_crest("559"), 0xD6001C, ""}; }
Team brighton() { return {"331", "BHA", "Brighton", espn_crest("331"), 0x0057B8, ""}; }
Team marseille() { return {"176", "MAR", "Marseille", fd_crest("516"), 0x2FAEE0, ""}; }

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
  return out;
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
