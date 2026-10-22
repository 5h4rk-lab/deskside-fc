// The wall clock: SNTP over Wi-Fi (pool.ntp.org, time.google.com), UTC, resynced
// every hour. Until the first sync the clock is "not valid": epoch_now_if_valid()
// answers 0, and the app neither polls the helper nor refreshes time-dependent
// screens (HostLoop treats epoch <= 0 as "no clock").
//
// The board's RTC chip is not used: its time is unknown until something sets it.
#pragma once

#include <stdint.h>

#include "app_diag.h"

// Starts SNTP (idempotent). Needs the network stack (esp_netif) up: call it once
// the Wi-Fi link has an address. Any task.
void time_sync_start();

// Epoch seconds (UTC) once a sync has happened and the year is 2024 or later;
// 0 otherwise. Any task.
int64_t epoch_now_if_valid();

// Number of completed syncs since boot (serial status line).
uint32_t time_sync_count();

#if APP_DIAG
// Diagnostics builds only: stops SNTP and sets the clock to 2023-06-01 (invalid),
// to see the app with a wrong clock; time_sync_diag_restore() starts SNTP again.
void time_sync_diag_break();
void time_sync_diag_restore();
#endif
