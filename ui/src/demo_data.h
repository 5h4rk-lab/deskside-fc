// Demo data for the simulator, the tests and the firmware's demo build: the
// matches shown in the approved mockup
// (docs/superpowers/specs/assets/ui-mockup-v2.html). Pure: no file access (the
// fixture JSON comes in as text), no stdio.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "mock_source.h"
#include "model.h"

namespace demo {

// Id of the live Arsenal v Chelsea match in demo_matches().
constexpr const char* kLiveMatchId = "demo-ars-che";
// Id of the Real Madrid v Barcelona match (pre-match: no lineups yet).
constexpr const char* kPreMatchId = "demo-rma-bar";
// Favourite team of the demo (Arsenal, ESPN id).
constexpr const char* kFavouriteId = "359";

}  // namespace demo

// A live Arsenal v Chelsea (67', 2:1), Real Madrid v Barcelona today 19:00 UTC,
// Man City v Liverpool tomorrow, Bayern v Dortmund on the coming Saturday and
// PSG v Marseille yesterday (3:1, full time), then Inter v Milan today 21:00
// UTC, Atletico v Sevilla in two days, Brighton v Liverpool tomorrow
// (postponed) and Real Madrid v Atletico three days ago (1:0, full time), so
// the Matches list scrolls. Arsenal also have a finished home match against
// Manchester City three days ago (3:1) and a coming away match at Tottenham on
// the Saturday at 17:30, so the News tiles have values. Times are relative to
// `now`.
std::vector<MatchSummary> demo_matches(int64_t now);

// News for the News tab: the six articles of `newsJson` (the text of
// contract/fixtures/news-normalised.json, with their image paths), published
// 25 minutes, 2 hours, 5 hours, a day plus 2 hours, 3 days and 9 days before
// `now` in the fixture's order. Empty when the text cannot be parsed.
std::vector<NewsItem> demo_news(const std::string& newsJson, int64_t now);

// Teams for the favourite-team picker: the Premier League teams of
// `teamsJson` (the text of contract/fixtures/teams-normalised.json) plus a handful of clubs from each of La Liga, Serie A,
// Bundesliga and Ligue 1 with their ESPN ids, abbreviations and the helper's
// curated accent and primary colours. Clubs with a crest in sim/assets use it.
std::vector<Team> demo_teams(const std::string& teamsJson);

// Detail for a demo match. The live match has lineups (Arsenal 4-3-3, Chelsea
// 4-2-3-1; faces for Saka, Havertz, Rice and Palmer), five events (goals by
// Saka 12', Palmer 34' and Havertz 58', a yellow card and a substitution) and
// five stats. The pre-match Real Madrid v Barcelona has only its summary. Any
// other id gives a detail with that id and nothing else.
MatchDetail demo_detail(const std::string& matchId);

// Puts the demo data into `source`: matches and details (kick-offs relative to
// `now`), news and teams from the two JSON texts, so the demo can be changed
// while it runs (demo_goal, demo_win).
void demo_load(MockSource& source, int64_t now, const std::string& newsJson,
               const std::string& teamsJson);

// The favourite scores in its live match: the score goes up and the detail
// gets a goal by one of the team's players with a photo, in turn. False when
// the favourite has no live match in `source`.
bool demo_goal(MockSource& source, const std::string& favouriteId);

enum class DemoWin { Ok, NoLiveMatch, NotAhead };
// The favourite's live match ends (full time) with the favourite ahead. Not
// ahead (no goal yet) or no live match changes nothing.
DemoWin demo_win(MockSource& source, const std::string& favouriteId);
