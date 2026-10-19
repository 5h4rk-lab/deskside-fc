#include "store.h"

namespace {
constexpr int64_t kStaleAfterSeconds = 90;

bool involves(const MatchSummary& m, const std::string& teamId) {
  return m.home.id == teamId || m.away.id == teamId;
}
}  // namespace

void Store::set_matches(std::vector<MatchSummary> matches, int64_t now) {
  matches_ = std::move(matches);
  lastSuccess_ = now;
  hasData_ = true;
}

const MatchSummary* Store::live_for(const std::string& teamId) const {
  for (const MatchSummary& m : matches_) {
    if (m.state == MatchState::Live && involves(m, teamId)) return &m;
  }
  return nullptr;
}

const MatchSummary* Store::next_for(const std::string& teamId, int64_t now) const {
  const MatchSummary* best = nullptr;
  for (const MatchSummary& m : matches_) {
    if (m.state != MatchState::Pre || m.kickoff < now || !involves(m, teamId)) continue;
    if (!best || m.kickoff < best->kickoff) best = &m;
  }
  return best;
}

bool Store::stale(int64_t now) const {
  return hasData_ && now - lastSuccess_ > kStaleAfterSeconds;
}
