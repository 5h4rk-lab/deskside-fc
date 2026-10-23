#include "app_diag.h"
#include "time_sync.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>

namespace {

constexpr int64_t kValidFrom = 1704067200;  // 2024-01-01 00:00:00 UTC
constexpr uint32_t kResyncMs = 60u * 60u * 1000u;

std::atomic<bool> s_started{false};
std::atomic<bool> s_synced{false};
std::atomic<uint32_t> s_syncs{0};
std::atomic<uint32_t> s_refused{0};
// The last good sync: its UTC time and the monotonic time it happened at, in
// microseconds (0: none yet). A later sync to a time before kValidFrom is
// refused and the clock is put back to this anchor plus the time since.
std::atomic<int64_t> s_anchorUtcUs{0};
std::atomic<int64_t> s_anchorMonoUs{0};

// Runs in the lwIP task right after SNTP set the clock: no printing.
void onSync(struct timeval* tv) {
  s_syncs.fetch_add(1);
  if (tv != nullptr && tv->tv_sec >= kValidFrom) {
    s_anchorMonoUs.store(esp_timer_get_time());
    s_anchorUtcUs.store(static_cast<int64_t>(tv->tv_sec) * 1000000 + tv->tv_usec);
    s_synced.store(true);
    return;
  }
  // A time before 2024 is wrong (a bad server answer): never take it. With a
  // good sync before, the clock goes back to that one carried forward (the
  // screens never notice); without one it stays "not valid" until a good sync.
  s_refused.fetch_add(1);
  const int64_t anchor = s_anchorUtcUs.load();
  if (anchor > 0) {
    const int64_t now = anchor + (esp_timer_get_time() - s_anchorMonoUs.load());
    struct timeval back = {};
    back.tv_sec = static_cast<time_t>(now / 1000000);
    back.tv_usec = static_cast<suseconds_t>(now % 1000000);
    settimeofday(&back, nullptr);
  }
}

}  // namespace

void time_sync_start() {
  bool expected = false;
  if (!s_started.compare_exchange_strong(expected, true)) return;
  setenv("TZ", "UTC0", 1);  // the UI converts to the chosen zone itself
  tzset();
  esp_sntp_config_t config =
      ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
  config.wait_for_sync = false;  // nobody waits: the app polls epoch_now_if_valid()
  config.sync_cb = onSync;
  esp_sntp_set_sync_interval(kResyncMs);
  const esp_err_t err = esp_netif_sntp_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[time] SNTP did not start: %s\n", esp_err_to_name(err));
    s_started.store(false);  // tried again at the next link-up
    return;
  }
  Serial.printf("[time] SNTP started (pool.ntp.org, time.google.com), resync every %lu s\n",
                (unsigned long)(esp_sntp_get_sync_interval() / 1000));
}

int64_t epoch_now_if_valid() {
  if (!s_synced.load()) return 0;
  const time_t now = time(nullptr);
  return static_cast<int64_t>(now) >= kValidFrom ? static_cast<int64_t>(now) : 0;
}

uint32_t time_sync_count() { return s_syncs.load(); }

uint32_t time_sync_refused() { return s_refused.load(); }

#if APP_DIAG
void time_sync_diag_break() {
  if (s_started.exchange(false)) esp_netif_sntp_deinit();
  s_synced.store(false);
  struct timeval tv = {};
  tv.tv_sec = 1685577600;  // 2023-06-01 00:00:00 UTC
  settimeofday(&tv, nullptr);
  Serial.println("[time] diag: SNTP stopped, clock set to 2023-06-01 (not valid)");
}

void time_sync_diag_bad_sync() {
  // Exactly what SNTP does with a bad answer: set the clock, then call back.
  struct timeval tv = {};
  tv.tv_sec = 1685577600;  // 2023-06-01 00:00:00 UTC
  settimeofday(&tv, nullptr);
  onSync(&tv);
  const int64_t e = epoch_now_if_valid();
  Serial.printf("[time] diag: a sync to 2023-06-01 arrived: refused (%lu so far), epoch now %lld (%s)\n",
                (unsigned long)s_refused.load(), (long long)e,
                e > 0 ? "the last good sync carried forward" : "not valid: no good sync yet");
}

void time_sync_diag_restore() {
  Serial.println("[time] diag: starting SNTP again");
  time_sync_start();
}
#endif
