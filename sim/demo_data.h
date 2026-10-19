// Demo data for the simulator and tests: the matches shown in the approved
// mockup (docs/superpowers/specs/assets/ui-mockup-v2.html). Simulator only.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "model.h"

namespace demo {

// Id of the live Arsenal v Chelsea match in demo_matches().
constexpr const char* kLiveMatchId = "demo-ars-che";
// Favourite team of the demo (Arsenal, ESPN id).
constexpr const char* kFavouriteId = "359";

}  // namespace demo

// A live Arsenal v Chelsea (67', 2:1), Real Madrid v Barcelona today 19:00 UTC,
// Man City v Liverpool tomorrow, Bayern v Dortmund on the coming Saturday and
// PSG v Marseille yesterday (3:1, full time). Times are relative to `now`.
std::vector<MatchSummary> demo_matches(int64_t now);

// Detail for a demo match. The live match has three goals (Saka 12', Palmer
// 34', Havertz 58'); any other id gives a detail with that id and no events.
MatchDetail demo_detail(const std::string& matchId);
