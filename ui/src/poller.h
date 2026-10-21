// Decides when to fetch what, and puts the results into the Store. Pure: it
// never reads a clock; the host calls tick() often (every loop iteration is
// fine) with a monotonic millisecond count and the epoch time. Shared with the
// firmware.
//
// - The first tick fetches matches, teams and news, in that order.
// - Matches are fetched again policy.next_delay_ms() after the start of the
//   last attempt (25 s with a live match, 180 s otherwise, longer after
//   failures), or at the next tick after request_matches_now() (or once the
//   fetch in flight when it was asked has ended, however it ended).
// - News every policy.news_delay_ms(). Teams only while the Store has none,
//   at most once every kTeamsRetryMs.
// - The watched detail (set_detail_watch) every policy.detail_delay_ms(live).
//   A newly watched id with nothing cached is fetched at the next tick.
// - Never two fetches of one kind in flight. A fetch unanswered for kLostMs is
//   considered lost and may be retried; its late answer is then ignored.
// - Matches success: Store::set_matches(data, epoch), then onMatchesChanged.
//   Failure: Store::mark_failure(); old data is kept; onMatchesChanged is not
//   called. News/teams results replace the Store's lists only on success.
//   Detail results go to Store::set_detail() on success.
// - Store::set_error(): a matches result sets it (None on success, the
//   source's last_error() on failure). News/teams failures set it only while
//   matches are fine (matches errors win); a news/teams success clears it once
//   neither news nor teams is failing (and matches are fine). A failure
//   reported with FetchError::None counts as Offline. Detail failures never
//   touch it (the detail screen shows its own).
// - Epoch times given to the Store are those of the latest tick.
// - Sources may call back synchronously (inside fetch_*) or later.
// - The monotonic clock must be 64-bit and never wrap (on the ESP32:
//   esp_timer_get_time() / 1000, not a 32-bit millis()). If it ever goes
//   backwards anyway, every attempt is treated as started at that moment, so
//   polling resumes (delays and lost-fetch expiry count from there).
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>

#include "data_source.h"
#include "refresh_policy.h"
#include "store.h"

class Poller {
 public:
  static constexpr int64_t kLostMs = 30000;
  static constexpr int64_t kTeamsRetryMs = 60000;

  // `store` and `source` must outlive the poller. `onMatchesChanged` (may be
  // empty) gets the epoch seconds of the success; the host passes
  // app_matches_changed.
  Poller(Store& store, DataSource& source, std::function<void(int64_t epochSeconds)> onMatchesChanged,
         RefreshPolicy policy = RefreshPolicy());
  ~Poller();
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  void tick(int64_t monotonicMs, int64_t epochSeconds);

  // Fetch matches at the next tick, whatever the delay or failure back-off (a
  // manual refresh, or after Wi-Fi connects). While a matches fetch is in
  // flight the request is remembered and honoured at the first tick after
  // that fetch ends (success, failure or lost).
  void request_matches_now();

  // After Wi-Fi connects: matches as request_matches_now(), plus teams when
  // the Store has none and news when the Store has none or the last news
  // fetch failed. The teams spacing (kTeamsRetryMs), the news delay and the
  // failure back-off do not apply; a fetch of that kind in flight is waited
  // for first, as for matches.
  void request_all_now();

  // The match whose detail to keep fresh (the open detail overlay); empty for
  // none. Setting the same id again changes nothing.
  void set_detail_watch(const std::string& matchId);

 private:
  struct Slot {
    bool pending = false;
    bool everStarted = false;
    int64_t startedMs = 0;
    uint32_t request = 0;  // id of the latest attempt
    FetchError error = FetchError::None;
  };

  void rebase(Slot& slot);
  void expire(Slot& slot);
  bool due(const Slot& slot, int64_t delayMs) const;
  uint32_t begin(Slot& slot);
  void publish_side_error(FetchError latest);
  void fetch_matches();
  void fetch_news();
  void fetch_teams();
  void fetch_detail();

  Store& store_;
  DataSource& source_;
  std::function<void(int64_t)> onMatchesChanged_;
  RefreshPolicy policy_;
  int64_t nowMs_ = 0;
  int64_t epoch_ = 0;
  bool forceMatches_ = false;
  bool forceTeams_ = false;
  bool forceNews_ = false;
  Slot matches_;
  Slot news_;
  Slot teams_;
  Slot detail_;
  std::string watchId_;
  bool detailNow_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
