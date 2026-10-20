#include "refresh_policy.h"

#include "model.h"

uint32_t RefreshPolicy::next_delay_ms(const Store& store, int64_t /*now*/) const {
  bool live = false;
  for (const MatchSummary& m : store.matches()) live = live || m.state == MatchState::Live;
  uint32_t delay = live ? kLiveMs : kIdleMs;
  // Doubling stops at the cap, so a long outage never overflows.
  for (int i = 1; i < store.failures() && delay < kMaxMs; ++i) delay *= 2;
  return delay < kMaxMs ? delay : kMaxMs;
}
