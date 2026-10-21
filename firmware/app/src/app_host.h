// Runs the shared UI (ui/src) on the board: builds it, drives the shared
// HostLoop from an LVGL timer (so every ui/ and LVGL call stays in the LVGL
// task) and takes commands from other tasks through a queue.
//
// This task: demo data only (no Wi-Fi, no real time). The data source is the
// simulator's MockSource filled by demo_load(), the clock is a fixed demo
// epoch that advances with the monotonic clock.
#pragma once

#include <stdint.h>

#include "backlight.h"
#include "prefs.h"

// Commands any task may post (see app_host_post). The serial console maps
// letters to them (main.cpp).
enum class AppCommand : uint8_t {
  Goal,        // the favourite scores in its live demo match
  Win,         // the favourite's live demo match ends with the favourite ahead
  ReloadDemo,  // restore the demo data (after a win the live match is gone)
  Tab,         // show tab `arg` (0 Live, 1 Matches, 2 News)
  NextTeam,    // make the next team of the team list the favourite (the path the team picker
               // takes: saved to NVS, colours and screens rebuilt), to test persistence
  Settings,    // print the settings in use
  Hide,        // diagnostics: toggle one part of the showing goal celebration (0 rays,
               // 1 confetti, 2 headline, 3 cutout, 4 bottom strip) to find what costs
               // frame time; no effect when none shows
  Call,        // run a function in the LVGL task (see app_host_post_call)
  Churn,       // diagnostics: toggle repainting the whole overlay every frame (profiling
               // single elements of the celebration at full-screen cost)
};

// Builds the UI. Call once, with the LVGL lock held (lvgl_lock) and after
// lvgl_port_start(). `prefs` and `backlight` must outlive the app. Returns
// false (after printing why) when memory is short; the app is then not
// running and the other calls do nothing.
bool app_host_start(Prefs& prefs, Backlight& backlight);

// Queues `fn` to run in the LVGL task (for serial commands that touch state
// the LVGL task owns, such as the settings in flash). Same queue and the same
// drop rule as app_host_post.
void app_host_post_call(void (*fn)());

// Queues a command for the LVGL task. Safe from any task; drops it (and says
// so) when the queue is full.
void app_host_post(AppCommand command, int arg = 0);

// Epoch seconds the app currently runs on (the fixed demo epoch plus the
// monotonic time), for the serial log.
int64_t app_host_epoch();
