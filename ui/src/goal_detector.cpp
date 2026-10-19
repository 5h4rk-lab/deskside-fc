#include "goal_detector.h"

namespace {

GoalEvent make_event(const MatchSummary& m, int home, int away, Side side, bool favourite,
                     bool fullTime) {
  GoalEvent e;
  e.matchId = m.id;
  e.favourite = favourite;
  e.fullTime = fullTime;
  e.side = side;
  e.clock = m.clock;
  e.homeScore = home;
  e.awayScore = away;
  e.homeAbbr = m.home.abbr;
  e.awayAbbr = m.away.abbr;
  return e;
}

}  // namespace

std::optional<GoalEvent> GoalDetector::update(const std::vector<MatchSummary>& next,
                                              const std::string& favTeamId) {
  // Lower rank wins: favourite goal, opponent goal, full time. Ties keep the
  // earlier match. Baselines of every match are updated before returning.
  std::optional<GoalEvent> best;
  int bestRank = 99;
  auto offer = [&](int rank, GoalEvent e) {
    if (rank < bestRank) {
      bestRank = rank;
      best = std::move(e);
    }
  };

  for (const MatchSummary& m : next) {
    const bool favHome = m.home.id == favTeamId;
    const bool favAway = m.away.id == favTeamId;
    if (favTeamId.empty() || (!favHome && !favAway)) continue;
    const Side favSide = favHome ? Side::Home : Side::Away;
    const Side oppSide = favHome ? Side::Away : Side::Home;

    auto it = baselines_.find(m.id);
    if (it == baselines_.end()) {
      // First sight: silent baseline. A null score (Pre) counts as 0:0.
      Baseline b;
      b.home = m.homeScore.value_or(0);
      b.away = m.awayScore.value_or(0);
      b.state = m.state;
      baselines_[m.id] = b;
      continue;
    }

    Baseline& b = it->second;
    // A transient null score mid-match keeps the previous value.
    const int home = m.homeScore.value_or(b.home);
    const int away = m.awayScore.value_or(b.away);
    const int favNow = favHome ? home : away;
    const int oppNow = favHome ? away : home;
    const int favBase = favHome ? b.home : b.away;
    const int oppBase = favHome ? b.away : b.home;

    if (m.state != MatchState::Postponed) {
      if (favNow > favBase) {
        offer(0, make_event(m, home, away, favSide, true, false));
      } else if (oppNow > oppBase) {
        offer(1, make_event(m, home, away, oppSide, false, false));
      }
      if (b.state == MatchState::Live && m.state == MatchState::Post) b.fullTimePending = true;
    }

    if (b.fullTimePending) {
      if (favNow > oppNow) {
        offer(2, make_event(m, home, away, favSide, true, true));
      } else {
        b.fullTimePending = false;
      }
    }

    b.home = home;
    b.away = away;
    b.state = m.state;
  }

  if (best && best->fullTime) baselines_[best->matchId].fullTimePending = false;
  return best;
}
