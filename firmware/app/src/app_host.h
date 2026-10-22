// Runs the shared UI (ui/src) on the board: builds it, drives the shared
// HostLoop from an LVGL timer (so every ui/ and LVGL call stays in the LVGL
// task) and takes commands from other tasks through a queue.
//
// Two builds:
// - Real data (release `app`, `app_diag`, `app_debug`): HttpSource over
//   EspHttpTransport (HTTPS to the helper, worker on core 0), the wall clock
//   from SNTP (time_sync). Until the clock is valid the HostLoop gets epoch 0:
//   no polling and no time-dependent refresh (the screens keep their
//   "Connecting..." state; the backlight follows the saved day brightness).
//   Every new Wi-Fi link (auto-join or a join on the setup pages) starts SNTP
//   once and asks the Poller for everything at once. Without a valid helper
//   URL and token the app makes no network requests at all ("Waiting for the
//   first update...").
//   Pictures: HttpImageProvider (3 MB cache in PSRAM, 1.5 KB charged per
//   picture, at most 64 remembered failures and 256 waiting requests) over a
//   second EspHttpTransport ("images", 512 KB cap, one kept connection), decoded
//   by LvglImageDecoder on the LVGL task, at most one picture per host-loop
//   tick. While the network or the clock is not ready, picture requests wait.
// - Demo data (`app_demo`, -DAPP_DEMO_DATA=1): the simulator's MockSource
//   filled by demo_load() and a fixed demo clock; no network use besides Wi-Fi.
#pragma once

#include <stdint.h>

#include <string>

#include "app_diag.h"

#include "backlight.h"
#include "platform/esp_wifi_service.h"
#include "prefs.h"

// Commands any task may post (see app_host_post). The serial console maps
// letters to them (main.cpp).
enum class AppCommand : uint8_t {
  Goal,        // demo: the favourite scores in its live demo match; real data: a test
               // celebration for the favourite (the data is not touched)
  Win,         // demo: the live demo match ends with the favourite ahead; real data: a
               // test full-time celebration
  ReloadDemo,  // demo: restore the demo data; real data: fetch everything now
  Tab,         // show tab `arg` (0 Live, 1 Matches, 2 News)
  NextTeam,    // make the next team of the team list the favourite (the path the team picker
               // takes: saved to NVS, colours and screens rebuilt), to test persistence
  Settings,    // print the settings in use
  Call,        // run a function in the LVGL task (see app_host_post_call)
#if APP_DIAG
  // Measurement commands, only in the diagnostics builds (env app_diag, app_debug).
  Hide,        // toggle one part of the showing goal celebration (0 rays, 1 confetti,
               // 2 stamp box, 3 cutout, 4 bottom strip, 5 baked stamp) to find what costs
               // frame time; no effect when none shows
  Churn,       // toggle repainting the whole overlay every frame (profiling single
               // elements of the celebration at full-screen cost)
#endif
};

// The helper's address and token as loaded and checked at start (main.cpp).
// `valid` false: the app makes no requests.
struct HelperConfig {
  std::string url;
  std::string token;
  bool valid = false;
};

// Builds the UI. Call once, with the LVGL lock held (lvgl_lock) and after
// lvgl_port_start(). `prefs`, `backlight` and `wifi` must outlive the app;
// `wifi` must have load()ed its saved list (the UI opens the first-run setup
// when it is empty) and is pumped from the app's 10 ms timer. `helper` is
// copied (the caller wipes its own copy of the token). Returns false (after
// printing why) when memory is short; the app is then not running and the
// other calls do nothing.
bool app_host_start(Prefs& prefs, Backlight& backlight, EspWifiService& wifi, const HelperConfig& helper);

// Queues `fn` to run in the LVGL task (for serial commands that touch state
// the LVGL task owns, such as the settings in flash). Same queue and the same
// drop rule as app_host_post.
void app_host_post_call(void (*fn)());

// Queues a command for the LVGL task. Safe from any task; drops it (and says
// so) when the queue is full.
void app_host_post(AppCommand command, int arg = 0);

// Epoch seconds the app currently runs on (0 while the clock is not valid),
// for the serial log. Any task.
int64_t app_host_epoch();

// One line about the data path ([net]): Wi-Fi link, clock, HTTP counters,
// internal RAM during TLS, the data status text; and one about pictures
// ([img]): cache bytes and count, requests waiting and in flight, remembered
// failures, decode times, the picture transport's counters. LVGL task (post it).
void app_host_print_net();

#if APP_DIAG && !APP_DEMO_DATA
// Diagnostics builds (LVGL task, post them): `n` matches requests one after
// the other through the real source, with internal/PSRAM free every 10; and a
// switch that refuses every request as if the router were off.
void app_host_diag_burst(int n);
// `n` distinct real pictures (team crests at seven sizes), two at a time
// through the real picture provider, with the cache and heap every 20.
void app_host_diag_img_churn(int n);
// The test goal celebration with a scorer, so its 190 px cutout is fetched.
void app_host_diag_goal_photo();
void app_host_diag_offline_toggle();
// One request to a deliberately slow public endpoint (no token): `headers`
// true holds the headers back, false trickles the body. Prints the time it took.
void app_host_diag_slow(bool headers);
#endif
