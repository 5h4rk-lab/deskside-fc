#include "goal_detector.h"

#include <iterator>

namespace {

GoalEvent make_event(const MatchSummary& m, int home, int away, Side side, bool favourite) {
  GoalEvent e;
  e.matchId = m.id;
  e.favourite = favourite;
  e.side = side;
  e.clock = m.clock;
  e.homeScore = home;
  e.awayScore = away;
  e.homeAbbr = m.home.abbr;
  e.awayAbbr = m.away.abbr;
  return e;
}

}  // namespace

int clock_minute(const std::string& clock) {
  int minute = 0;
  size_t i = 0;
  for (; i < clock.size() && clock[i] >= '0' && clock[i] <= '9' && i < 3; ++i) minute = minute * 10 + (clock[i] - '0');
  if (i == 0 || i >= clock.size() || clock[i] != '\'') return -1;
  return minute;
}

std::optional<GoalEvent> GoalDetector::update(const std::vector<MatchSummary>& next,
                                              const std::string& favTeamId, int64_t now) {
  // Drop stale state so changes that happened while untracked never celebrate.
  if (favTeamId != lastFav_) {
    baselines_.clear();
    lastFav_ = favTeamId;
  }
  for (auto it = baselines_.begin(); it != baselines_.end();) {
    bool present = false;
    for (const MatchSummary& m : next) {
      if (m.id == it->first) {
        present = true;
        break;
      }
    }
    it = present ? std::next(it) : baselines_.erase(it);
  }

  // Lower rank wins: favourite goal, opponent goal, full time, kick-off. Ties
  // keep the earlier match. Baselines of every match are updated before
  // returning.
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
      b.seenAt = now;
      baselines_[m.id] = b;
      continue;
    }

    Baseline& b = it->second;
    // The state before this poll was seen recently: a transition now was
    // watched as it happened (not found after an outage or in old data).
    const bool recent = now - b.seenAt <= kRecentSeconds && now >= b.seenAt;
    // A transient null score mid-match keeps the previous value.
    const int home = m.homeScore.value_or(b.home);
    const int away = m.awayScore.value_or(b.away);
    const int favNow = favHome ? home : away;
    const int oppNow = favHome ? away : home;
    const int favBase = favHome ? b.home : b.away;
    const int oppBase = favHome ? b.away : b.home;

    if (m.state != MatchState::Postponed) {
      if (favNow > favBase) {
        offer(0, make_event(m, home, away, favSide, true));
      } else if (oppNow > oppBase) {
        offer(1, make_event(m, home, away, oppSide, false));
      }
      if (b.state == MatchState::Live && m.state == MatchState::Post && recent && !b.fullTimeReported) {
        b.fullTimePending = true;
      }
      if (b.state == MatchState::Pre && m.state == MatchState::Live && recent) {
        const int minute = clock_minute(m.clock);
        if (minute <= kKickOffMaxMinute) {  // -1: no minutes to go by, the recency guard decides
          GoalEvent e = make_event(m, home, away, favSide, false);
          e.kickOff = true;
          offer(3, std::move(e));
        }
      }
    }

    if (b.fullTimePending) {
      GoalEvent e = make_event(m, home, away, favSide, favNow > oppNow);
      e.fullTime = true;
      e.outcome = favNow > oppNow ? Outcome::Win : favNow == oppNow ? Outcome::Draw : Outcome::Defeat;
      offer(2, std::move(e));
    }

    if (m.state == MatchState::Pre) b.fullTimeReported = false;
    b.home = home;
    b.away = away;
    b.state = m.state;
    b.seenAt = now;
  }

  if (best && best->fullTime) {
    Baseline& b = baselines_[best->matchId];
    b.fullTimePending = false;
    b.fullTimeReported = true;
  }
  return best;
}
