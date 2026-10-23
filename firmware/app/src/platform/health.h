// Reliability on the board: the task watchdog, the reset reason, the crash
// counter (ui/src/crash_counter.h) with safe mode, the core dump summary.
//
// Task watchdog. The prebuilt ESP-IDF starts it with 5 s, panic on, the idle
// task of core 0 watched (core 1's idle task is not: LVGL keeps it busy).
// health_wdt_setup() makes that kWdtTimeoutS (15 s) with panic -> reboot and
// keeps core 0's idle task. The LVGL task and the Wi-Fi worker subscribe
// themselves and feed from their own loops (health_wdt_subscribe /
// health_wdt_feed). Both HTTP workers (data, pictures) are watchdog USERS
// (WdtUser) fed from their loops: a connect may legitimately block longer than
// 15 s (lwIP's DNS lookup has no deadline of its own: about 20 s with the
// router up and the internet down), so it runs under cover(maxMs): a timer
// feeds the user meanwhile, for at most maxMs; a connect hung beyond that still
// resets the board. (Unsubscribing for the call instead would be unbounded, and
// every re-subscription restarts the watchdog's period for ALL tasks, which
// can hide another task's stall.) The Arduino loop task (serial console,
// battery sampling, health_tick) subscribes at the end of setup() and feeds at
// the top of every loop(). The OTA download task is not watched (it has its
// own stall and total limits).
//
// Reset reason and crash counter. health_boot() runs early in setup(): it
// prints one [health] line (the reason, how long the previous start ran, the
// crashes counted, safe mode), updates the record in NVS (namespace "health",
// key "crash": nothing else is touched) and returns whether this start runs in
// SAFE MODE. On entering safe mode with a software fault among the counted
// resets it rolls a newer update back (ota_switch_to_older_image) when the
// other slot holds a valid older image: the record is cleared and the board
// restarts into that image. How long the previous start ran comes from a few
// bytes of RTC memory (RTC_NOINIT) that health_tick() rewrites every second:
// they survive a panic, a watchdog and a software restart, not a power-on.
// A brownout cannot be logged while it happens (the IDF's own handler resets
// at once); the next start names it.
//
// Core dumps (to the flash partition "coredump", CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
// in the prebuilt libraries): health_boot() prints the summary of a stored
// dump (task, PC, reason, backtrace) and keeps it for :status, also in NVS
// (namespace "health", key "dump", at most kCrashSummaryMax bytes) so the line
// survives later starts. The release build then erases the dump (it may hold
// the helper token from RAM), a damaged one too; diagnostics builds keep it
// for `espcoredump.py`. :status and :safe-mode list the counted crashes by
// kind ("panic 7 min ago, ...") and the last kept summary.
#pragma once

#include <stdint.h>

#include <string>

#include "../app_diag.h"
#include "crash_counter.h"

constexpr uint32_t kWdtTimeoutS = 15;

void health_wdt_setup();
// The calling task: watch it / feed it / stop watching it. Feed and
// unsubscribe do nothing for a task that is not subscribed.
void health_wdt_subscribe(const char* name);
void health_wdt_feed();
void health_wdt_unsubscribe();

// A task watchdog user (one per HTTP worker), fed from the worker's loop.
class WdtUser {
 public:
  WdtUser() = default;
  ~WdtUser();
  WdtUser(const WdtUser&) = delete;
  WdtUser& operator=(const WdtUser&) = delete;
  // `name` must outlive this (a string literal or a member). False: not watched.
  bool begin(const char* name);
  void feed();
  // One blocking call that may last up to `maxMs`: fed by a timer until
  // uncover() (which feeds once more), never beyond maxMs.
  void cover(uint32_t maxMs);
  void uncover();

 private:
  static void on_timer(void* arg);
  void* user_ = nullptr;  // esp_task_wdt_user_handle_t
  void* timer_ = nullptr;  // esp_timer_handle_t
  volatile int64_t coverUntilUs_ = 0;
};

struct HealthBoot {
  ResetKind kind = ResetKind::Unknown;
  int espReason = 0;           // esp_reset_reason_t
  int64_t previousRunMs = -1;  // -1: unknown
  BootVerdict verdict;
};

// Early in setup(), after ota_boot(): may restart (an update rollback).
HealthBoot health_boot();
const HealthBoot& health_boot_info();
bool health_safe_mode();

// Arduino loop task, often (acts once a second): the RTC uptime, then the
// counter's runtime rules (clears the record after a stable run; leaves safe
// mode after its time with a restart).
void health_tick();

// :safe-mode off (loop task): clears the counter and restarts in normal mode.
// Says so and does nothing when not in safe mode.
void health_safe_mode_off();
// :safe-mode (any task): one line.
void health_print_safe_mode();

// The [status] lines of :status that belong here (any task): uptime, reset
// reason, crash counter, safe mode, the core dump summary.
void health_print_status();

#if APP_DIAG
// Fault injection (diagnostics builds only):
// a task that subscribes to the watchdog and then never feeds it (the board
// resets with reason "task watchdog" after kWdtTimeoutS);
void health_diag_wdt_stall();
// abort() now (a panic).
[[noreturn]] void health_diag_panic();
#endif
