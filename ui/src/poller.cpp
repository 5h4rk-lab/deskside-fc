#include "poller.h"

#include <utility>
#include <vector>

#include "model.h"

Poller::Poller(Store& store, DataSource& source, std::function<void(int64_t)> onMatchesChanged,
               RefreshPolicy policy)
    : store_(store), source_(source), onMatchesChanged_(std::move(onMatchesChanged)), policy_(policy) {}

Poller::~Poller() { *alive_ = false; }

void Poller::request_matches_now() {
  // A fetch in flight serves the request.
  if (!matches_.pending) forceMatches_ = true;
}

void Poller::set_detail_watch(const std::string& matchId) {
  if (matchId == watchId_) return;
  watchId_ = matchId;
  // An answer for the previous id is ignored; the new id starts its own cycle.
  detail_.pending = false;
  ++detail_.request;
  detail_.everStarted = true;
  detail_.startedMs = nowMs_;
  detailNow_ = !matchId.empty() && store_.detail_for(matchId) == nullptr;
}

void Poller::expire(Slot& slot) {
  if (slot.pending && nowMs_ - slot.startedMs >= kLostMs) slot.pending = false;
}

bool Poller::due(const Slot& slot, int64_t delayMs) const {
  return !slot.pending && (!slot.everStarted || nowMs_ - slot.startedMs >= delayMs);
}

uint32_t Poller::begin(Slot& slot) {
  slot.pending = true;
  slot.everStarted = true;
  slot.startedMs = nowMs_;
  return ++slot.request;
}

void Poller::tick(int64_t monotonicMs, int64_t epochSeconds) {
  nowMs_ = monotonicMs;
  epoch_ = epochSeconds;
  expire(matches_);
  expire(news_);
  expire(teams_);
  expire(detail_);

  if (!matches_.pending &&
      (forceMatches_ || due(matches_, policy_.next_delay_ms(store_, epoch_)))) {
    forceMatches_ = false;  // before the fetch: a synchronous answer may ask again
    fetch_matches();
  }
  if (store_.teams().empty() && due(teams_, kTeamsRetryMs)) fetch_teams();
  if (due(news_, policy_.news_delay_ms())) fetch_news();
  if (!watchId_.empty() && !detail_.pending) {
    bool live = false;
    for (const MatchSummary& m : store_.matches()) {
      if (m.id == watchId_) live = m.state == MatchState::Live;
    }
    if (detailNow_ || due(detail_, policy_.detail_delay_ms(live))) {
      detailNow_ = false;
      fetch_detail();
    }
  }
}

// After a news or teams result. Matches errors win; while matches are fine a
// news/teams failure shows, and clears once neither of them is failing.
void Poller::publish_side_error(FetchError latest) {
  if (matches_.error != FetchError::None) return;
  if (latest != FetchError::None) {
    store_.set_error(latest);
  } else if (news_.error == FetchError::None && teams_.error == FetchError::None) {
    store_.set_error(FetchError::None);
  }
}

namespace {
// A failure must never read as "up to date".
FetchError failure_reason(const DataSource& source) {
  const FetchError e = source.last_error();
  return e == FetchError::None ? FetchError::Offline : e;
}
}  // namespace

void Poller::fetch_matches() {
  const uint32_t id = begin(matches_);
  std::shared_ptr<bool> alive = alive_;
  source_.fetch_matches([this, alive, id](bool ok, std::vector<MatchSummary> data) {
    if (!*alive || id != matches_.request) return;
    matches_.pending = false;
    if (ok) {
      matches_.error = FetchError::None;
      store_.set_matches(std::move(data), epoch_);
      store_.set_error(FetchError::None);
      if (onMatchesChanged_) onMatchesChanged_(epoch_);
    } else {
      matches_.error = failure_reason(source_);
      store_.mark_failure();
      store_.set_error(matches_.error);
    }
  });
}

void Poller::fetch_news() {
  const uint32_t id = begin(news_);
  std::shared_ptr<bool> alive = alive_;
  source_.fetch_news([this, alive, id](bool ok, std::vector<NewsItem> data) {
    if (!*alive || id != news_.request) return;
    news_.pending = false;
    news_.error = ok ? FetchError::None : failure_reason(source_);
    if (ok) store_.set_news(std::move(data));
    publish_side_error(news_.error);
  });
}

void Poller::fetch_teams() {
  const uint32_t id = begin(teams_);
  std::shared_ptr<bool> alive = alive_;
  source_.fetch_teams([this, alive, id](bool ok, std::vector<Team> data) {
    if (!*alive || id != teams_.request) return;
    teams_.pending = false;
    teams_.error = ok ? FetchError::None : failure_reason(source_);
    if (ok) store_.set_teams(std::move(data));
    publish_side_error(teams_.error);
  });
}

void Poller::fetch_detail() {
  const uint32_t id = begin(detail_);
  std::shared_ptr<bool> alive = alive_;
  source_.fetch_detail(watchId_, [this, alive, id](bool ok, MatchDetail detail) {
    if (!*alive || id != detail_.request) return;
    detail_.pending = false;
    if (ok) store_.set_detail(std::move(detail));
  });
}
