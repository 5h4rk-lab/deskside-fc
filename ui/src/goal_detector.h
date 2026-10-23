// Turns successive match polls into at most one event per poll, for the
// favourite's matches only: a goal (either side), full time (any result) and
// kick-off.
//
// The detector only sees MatchSummary, so it cannot know who scored: `scorer`
// is left empty and the UI fills it in from MatchDetail later.
//
// Recency guard (kick-off and full time): a transition counts only when the
// state before it was seen by a poll at most kRecentSeconds before the one
// that shows the change, so the device really watched it happen; after a long
// outage (or a replay of old data) the new state is taken silently. Kick-off
// also needs the match clock, when it reads as minutes, at kKickOffMaxMinute
// or less (a match first seen at 60' never "kicks off"). Goals are not
// guarded this way (a goal found after an outage still celebrates).
#pragma once

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "model.h"
#include "refresh_policy.h"

// The favourite's result at full time.
enum class Outcome { None, Win, Draw, Defeat };

struct GoalEvent {
  std::string matchId;
  std::string scorer;  // always empty here; filled in by the UI from MatchDetail
  // True for the favourite's goal and the favourite's win at full time: the
  // events that celebrate (with a sound). Opponents' goals, draws, defeats
  // and kick-off are false.
  bool favourite = false;
  bool fullTime = false;  // the favourite's match ended (outcome says how)
  bool kickOff = false;   // the favourite's match went live
  Outcome outcome = Outcome::None;  // full time only
  Side side = Side::Home;  // team that scored; for full time and kick-off the favourite's side
  std::string clock;
  int homeScore = 0;
  int awayScore = 0;
  std::string homeAbbr;
  std::string awayAbbr;
};

class GoalDetector {
 public:
  // Two idle polls (one may fail: the next success comes another kIdleMs
  // later) plus a minute for request times and tick jitter: 420 s.
  static constexpr int64_t kRecentSeconds = 2 * (RefreshPolicy::kIdleMs / 1000) + 60;
  static constexpr int kKickOffMaxMinute = 10;

  // Feed each poll's full match list with the poll's epoch seconds. Returns
  // an event for the favourite's matches only; the first sighting of a match
  // never produces one. Ranking when several happen in one poll: the
  // favourite's goal, an opponent's goal, full time, kick-off (ties: the
  // earlier match in the list). A match's baseline is dropped when it is
  // absent from a poll, and all baselines are dropped when favTeamId changes,
  // so a match that reappears (or becomes tracked after a favourite switch)
  // is "first seen" again and stays silent.
  std::optional<GoalEvent> update(const std::vector<MatchSummary>& next, const std::string& favTeamId,
                                  int64_t now);

 private:
  struct Baseline {
    int home = 0;
    int away = 0;
    MatchState state = MatchState::Pre;
    int64_t seenAt = 0;  // epoch of the poll that last saw this match
    bool fullTimePending = false;  // saw Live -> Post (recently), not yet reported
  };
  std::map<std::string, Baseline> baselines_;
  std::string lastFav_;
};

// The minute of a match clock such as "67'", "45'+2'" or "90'+4'" (the part
// before the first apostrophe), or -1 when it does not read as minutes ("HT",
// "FT", empty).
int clock_minute(const std::string& clock);
