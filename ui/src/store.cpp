#include "store.h"

#include "refresh_policy.h"

namespace {

bool involves(const MatchSummary& m, const std::string& teamId) {
  return m.home.id == teamId || m.away.id == teamId;
}
}  // namespace

void Store::set_matches(std::vector<MatchSummary> matches, int64_t now) {
  matches_ = std::move(matches);
  ++matchesVersion_;
  lastSuccess_ = now;
  hasData_ = true;
  failures_ = 0;
}

std::string Store::data_status_text() const {
  switch (error_) {
    case FetchError::Offline: return "Cannot reach the helper";
    case FetchError::Auth: return "Helper rejected the token";
    case FetchError::Server: return "Helper returned an error";
    case FetchError::Parse: return "Helper sent unreadable data";
    case FetchError::None: break;
  }
  return hasData_ ? "Live data is up to date" : "Waiting for the first update...";
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

const MatchSummary* Store::last_for(const std::string& teamId, int64_t now) const {
  const MatchSummary* best = nullptr;
  for (const MatchSummary& m : matches_) {
    if (m.state != MatchState::Post || m.kickoff > now || !involves(m, teamId)) continue;
    if (!best || m.kickoff > best->kickoff) best = &m;
  }
  return best;
}

int64_t Store::stale_after_s() const {
  bool live = false;
  for (const MatchSummary& m : matches_) live = live || m.state == MatchState::Live;
  const int64_t interval = (live ? RefreshPolicy::kLiveMs : RefreshPolicy::kIdleMs) / 1000;
  const int64_t after = interval + kStaleGraceSeconds;
  return after > kStaleMinSeconds ? after : kStaleMinSeconds;
}

bool Store::stale(int64_t now) const {
  return hasData_ && now - lastSuccess_ > stale_after_s();
}

void Store::set_detail(MatchDetail detail) {
  for (auto it = details_.begin(); it != details_.end(); ++it) {
    if (it->match.id == detail.match.id) {
      details_.erase(it);
      break;
    }
  }
  if (details_.size() >= static_cast<size_t>(kDetailCacheSize)) details_.erase(details_.begin());
  details_.push_back(std::move(detail));
}

const MatchDetail* Store::detail_for(const std::string& matchId) const {
  for (const MatchDetail& d : details_) {
    if (d.match.id == matchId) return &d;
  }
  return nullptr;
}
