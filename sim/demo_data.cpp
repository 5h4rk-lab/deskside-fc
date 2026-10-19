#include "demo_data.h"

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

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

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
  return out;
}

MatchDetail demo_detail(const std::string& matchId) {
  MatchDetail d;
  if (matchId != demo::kLiveMatchId) {
    d.match.id = matchId;
    return d;
  }
  d.match = demo_matches(0).front();
  d.events = {
      {"12'", EventType::Goal, Side::Home, "Bukayo Saka", "Assist: Martin Odegaard"},
      {"34'", EventType::Goal, Side::Away, "Cole Palmer", ""},
      {"58'", EventType::Goal, Side::Home, "Kai Havertz", "Assist: Declan Rice"},
  };
  return d;
}
