#include "host_loop.h"

#include <utility>

HostLoop::HostLoop(Store& store, DataSource& source, Hooks hooks)
    : store_(store),
      hooks_(std::move(hooks)),
      poller_(store, source, [this](int64_t now) {
        if (hooks_.onMatchesChanged) hooks_.onMatchesChanged(now);
        matchesChanged_ = true;
      }) {}

std::string HostLoop::detail_watch() const {
  std::string id = hooks_.openMatchId ? hooks_.openMatchId() : std::string();
  if (id.empty()) {
    if (const MatchSummary* m = store_.live_for(store_.favourite())) id = m->id;
  }
  return id;
}

void HostLoop::pump(int64_t monotonicMs, int64_t epochSeconds) {
  if (hooks_.pollTransports) hooks_.pollTransports();
  if (hooks_.tickImages) hooks_.tickImages(monotonicMs);
  if (epochSeconds <= 0 || fetchesPaused_) return;
  poller_.set_detail_watch(detail_watch());
  poller_.tick(monotonicMs, epochSeconds);
}

void HostLoop::tick(int64_t monotonicMs, int64_t epochSeconds) {
  pump(monotonicMs, epochSeconds);
  if (epochSeconds <= 0) return;
  const bool newSecond = !refreshed_ || epochSeconds != lastRefresh_;
  if (!newSecond && !matchesChanged_) return;
  if (newSecond && hooks_.everySecond) hooks_.everySecond(epochSeconds);
  if (hooks_.refresh) hooks_.refresh(epochSeconds);
  lastRefresh_ = epochSeconds;
  refreshed_ = true;
  matchesChanged_ = false;
}

void HostLoop::note_refreshed(int64_t epochSeconds) {
  lastRefresh_ = epochSeconds;
  refreshed_ = true;
}

bool HostLoop::take_matches_changed() {
  const bool was = matchesChanged_;
  matchesChanged_ = false;
  return was;
}
