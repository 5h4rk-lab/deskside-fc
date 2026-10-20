// How long to wait between fetches. Pure: no clock reads; shared with the
// firmware.
#pragma once

#include <stdint.h>

#include "store.h"

class RefreshPolicy {
 public:
  static constexpr uint32_t kLiveMs = 25000;     // any match in the Store is live
  static constexpr uint32_t kIdleMs = 180000;    // nothing live
  static constexpr uint32_t kMaxMs = 300000;     // back-off cap
  static constexpr uint32_t kNewsMs = 300000;
  static constexpr uint32_t kDetailLiveMs = 15000;
  static constexpr uint32_t kDetailIdleMs = 60000;

  // Delay before the next match fetch, counted from the start of the last
  // attempt: kLiveMs when any match in store.matches() is Live, else kIdleMs;
  // after n >= 1 failures (Store::failures()) that base times 2^(n - 1),
  // capped at kMaxMs. A success resets the failures (the Store does that).
  // `now` (epoch seconds) is unused today; it keeps time-based rules possible.
  uint32_t next_delay_ms(const Store& store, int64_t now) const;

  uint32_t news_delay_ms() const { return kNewsMs; }
  // The open match detail: every 15 s while its match is live, else 60 s.
  uint32_t detail_delay_ms(bool live = true) const { return live ? kDetailLiveMs : kDetailIdleMs; }
};
