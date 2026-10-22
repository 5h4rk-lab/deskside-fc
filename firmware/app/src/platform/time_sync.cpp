#include "app_diag.h"
#include "time_sync.h"

#include <Arduino.h>
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

// Runs in the lwIP task after SNTP set the clock: flags only, no printing.
void onSync(struct timeval* tv) {
  if (tv != nullptr && tv->tv_sec >= kValidFrom) s_synced.store(true);
  s_syncs.fetch_add(1);
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

#if APP_DIAG
void time_sync_diag_break() {
  if (s_started.exchange(false)) esp_netif_sntp_deinit();
  s_synced.store(false);
  struct timeval tv = {};
  tv.tv_sec = 1685577600;  // 2023-06-01 00:00:00 UTC
  settimeofday(&tv, nullptr);
  Serial.println("[time] diag: SNTP stopped, clock set to 2023-06-01 (not valid)");
}

void time_sync_diag_restore() {
  Serial.println("[time] diag: starting SNTP again");
  time_sync_start();
}
#endif
