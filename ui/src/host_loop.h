// The glue between a host's main loop and the UI: delivers finished requests,
// tells the Poller which match detail to keep fresh, runs the Poller, and
// refreshes the screens once a second and after every matches change. Shared
// by the simulator and the firmware. Pure: it never reads a clock, never
// touches LVGL itself (the host passes the calls in), and runs on one thread
// (the one that owns LVGL).
//
// Time: the host gives a 64-bit monotonic millisecond count and the epoch in
// seconds. An epoch <= 0 means "the clock is not set yet" (no SNTP, or a sync
// the host refused): then the Poller is not ticked, the detail watch is not
// updated and `refresh` is not called (no fetch may run on a wrong clock); the
// transport hooks still run so requests already in flight finish. The Store
// is told (Store::set_clock_valid(false): the status rows read "Waiting for
// the clock") and `refreshWithoutClock` runs at once and then every
// kClocklessRefreshMs, so the screens say so instead of freezing silently.
// The first valid epoch after that sets the Store's clock valid again and
// refreshes at once.
#pragma once

#include <stdint.h>

#include <functional>
#include <string>

#include "data_source.h"
#include "poller.h"
#include "store.h"

class HostLoop {
 public:
  struct Hooks {
    // Deliver finished requests (HTTP transports). May be empty.
    std::function<void()> pollTransports;
    // Time-driven work of the picture provider. May be empty.
    std::function<void(int64_t monotonicMs)> tickImages;
    // Id of the match whose detail overlay is open, "" for none (the host
    // passes app_open_match_id). May be empty.
    std::function<std::string()> openMatchId;
    // Poller::onMatchesChanged: the host passes app_matches_changed. Called
    // before the next refresh.
    std::function<void(int64_t epochSeconds)> onMatchesChanged;
    // Called when the epoch second changed (and at the first valid tick), just
    // before refresh, e.g. to sample the battery. May be empty.
    std::function<void(int64_t epochSeconds)> everySecond;
    // The host passes app_refresh.
    std::function<void(int64_t epochSeconds)> refresh;
    // While the epoch is invalid (see above), with the last valid epoch seen
    // (0 when there never was one). The host passes app_refresh, which
    // leaves the backlight and other clock-driven work alone while
    // Store::clock_valid() is false. May be empty.
    std::function<void(int64_t lastValidEpoch)> refreshWithoutClock;
  };

  static constexpr int64_t kClocklessRefreshMs = 1000;

  // `store` and `source` must outlive the loop.
  HostLoop(Store& store, DataSource& source, Hooks hooks);
  HostLoop(const HostLoop&) = delete;
  HostLoop& operator=(const HostLoop&) = delete;

  // One loop iteration, in this order: pollTransports, tickImages, (epoch
  // valid:) set_detail_watch, Poller::tick, then everySecond and refresh when
  // the epoch second changed since the last refresh or matches changed.
  void tick(int64_t monotonicMs, int64_t epochSeconds);

  // The first half of tick() without the refresh (for hosts that refresh by
  // themselves, such as the simulator's one-shot snapshots).
  void pump(int64_t monotonicMs, int64_t epochSeconds);

  // The host refreshed on its own at `epochSeconds`: the next tick refreshes
  // only when the second differs (or matches changed since, as before).
  void note_refreshed(int64_t epochSeconds);

  // True once after matches changed since the last call or refresh (the
  // host refreshes then).
  bool take_matches_changed();

  Poller& poller() { return poller_; }

  // While paused (a software update downloads) the Poller is not ticked: no
  // new fetch starts; requests already in flight are still delivered and the
  // screens still refresh every second. Requests made meanwhile
  // (request_all_now) run at the first tick after the pause ends.
  void set_fetches_paused(bool paused) { fetchesPaused_ = paused; }
  bool fetches_paused() const { return fetchesPaused_; }

 private:
  // The open detail, else the favourite's live match (its latest event feeds
  // the Live tab's strip).
  std::string detail_watch() const;

  Store& store_;
  Hooks hooks_;
  Poller poller_;
  bool matchesChanged_ = false;
  bool fetchesPaused_ = false;
  bool refreshed_ = false;
  int64_t lastRefresh_ = 0;
  int64_t lastValidEpoch_ = 0;
  bool clocklessRefreshed_ = false;  // refreshWithoutClock ran in this clockless spell
  int64_t lastClocklessMs_ = 0;
};
