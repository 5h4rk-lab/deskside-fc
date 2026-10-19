// Turns successive match polls into at most one celebratory event per poll.
//
// The detector only sees MatchSummary, so it cannot know who scored: `scorer`
// is left empty and the UI fills it in from MatchDetail later.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "model.h"

struct GoalEvent {
  std::string matchId;
  std::string scorer;  // always empty here; filled in by the UI from MatchDetail
  bool favourite = false;  // true: the favourite scored (or won, for fullTime)
  bool fullTime = false;
  Side side = Side::Home;  // team that scored; for full time the favourite's side
  std::string clock;
  int homeScore = 0;
  int awayScore = 0;
  std::string homeAbbr;
  std::string awayAbbr;
};

class GoalDetector {
 public:
  // Feed each poll's full match list. Returns an event for the favourite's
  // matches only; the first sighting of a match never produces one. A match's
  // baseline is dropped when it is absent from a poll, and all baselines are
  // dropped when favTeamId changes, so a match that reappears (or becomes
  // tracked after a favourite switch) is "first seen" again and stays silent.
  std::optional<GoalEvent> update(const std::vector<MatchSummary>& next,
                                  const std::string& favTeamId);

 private:
  struct Baseline {
    int home = 0;
    int away = 0;
    MatchState state = MatchState::Pre;
    bool fullTimePending = false;  // saw Live -> Post, not yet reported or dismissed
  };
  std::map<std::string, Baseline> baselines_;
  std::string lastFav_;
};
