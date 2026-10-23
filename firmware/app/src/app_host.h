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
//   A new picture connection is opened only with at least 80 KB of internal
//   RAM free (EspHttpTransport::kImagesMinInternalFree; it waits within the
//   request's deadline otherwise).
// - Demo data (`app_demo`, -DAPP_DEMO_DATA=1): the simulator's MockSource
//   filled by demo_load() and a fixed demo clock; no network use besides Wi-Fi.
#pragma once

#include <stdint.h>

#include <string>

#include "app_diag.h"

#include "backlight.h"
#include "platform/esp_power_service.h"
#include "platform/esp_wifi_service.h"
#include "platform/esp_ota.h"
#include "platform/i2s_sound_out.h"
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
  Sound,       // play a sound at the saved volume, ignoring the sound settings and the
               // gate (the serial :sound command): arg = kSoundArg* (see below)
  Call,        // run a function in the LVGL task (see app_host_post_call)
  CallArg,     // run a function with an int argument in the LVGL task (app_host_post_call_arg)
#if APP_DIAG
  // Measurement commands, only in the diagnostics builds (env app_diag, app_debug).
  Hide,        // toggle one part of the showing goal celebration (0 rays, 1 confetti,
               // 2 stamp box, 3 cutout, 4 bottom strip, 5 baked stamp) to find what costs
               // frame time; no effect when none shows
  Churn,       // toggle repainting the whole overlay every frame (profiling single
               // elements of the celebration at full-screen cost)
#endif
};

// AppCommand::Sound's argument: bits 0-3 the SoundEvent, or 15 for the test
// tone; bits 4-5 the channels for the tone (SoundChannels); bits 8-15 a volume
// 1..100 (0: the saved volume); kSoundArgStop stops whatever plays.
constexpr int kSoundArgTone = 15;
constexpr int kSoundArgStop = -1;
constexpr int sound_arg(int what, SoundChannels channels, int volume) {
  return what | (static_cast<int>(channels) << 4) | (volume << 8);
}

// The helper's address and token as loaded and checked at start (main.cpp).
// `valid` false: the app makes no requests.
struct HelperConfig {
  std::string url;
  std::string token;
  bool valid = false;
};

// Builds the UI. Call once, with the LVGL lock held (lvgl_lock) and after
// lvgl_port_start(). `prefs`, `backlight`, `wifi`, `power` (begun) and
// `speaker` (nullable: then no sound and no Sound rows in Settings) must
// outlive the app;
// `wifi` must have load()ed its saved list (the UI opens the first-run setup
// when it is empty) and is pumped from the app's 10 ms timer. `helper` is
// copied (the caller wipes its own copy of the token). Returns false (after
// printing why) when memory is short; the app is then not running and the
// other calls do nothing.
//
// The power key (GPIO6) is sampled in the app's 10 ms timer and runs the shared
// PowerController (ui/src/power_policy.h): a short press turns the screen off
// or on (a touch also wakes it), holding it shows "Hold to power off" after
// 1 s and switches off at 3 s: the speaker is stopped (wait up to 300 ms), the
// "Powering off" screen shows for 1.5 s, the settings are saved, the radio
// stops and the power hold (GPIO7) goes low. On battery the board goes dark
// once the key is up; still powered (USB), it stays in the off state
// (backlight 0, nothing drawn, no network, the key looked at every 10 s or
// when its level changes) until a press restarts it.
//
// Software updates (real-data builds with a helper): the Settings row
// "Software update" checks the helper's /api/firmware/version (see
// ui/src/update_flow.h for when) and installs on a tap: the data poller is
// paused and both transports refuse requests, requests in flight are waited
// for, the picture connection is closed and the speaker stopped, then
// EspOta downloads into the idle slot (the only network session), checks it
// and switches the boot slot; "Restarting..." shows for 1.5 s before the
// restart. A failed download resumes everything and shows why in the row.
// `boot` is what ota_boot() found: an image on trial is confirmed
// (ota_confirm) from the 10 ms timer once TrialHealth is ready (display up
// and Wi-Fi started, both read as levels every tick: lvgl_port_frames_total()
// > 0 and EspWifiService::started(); then the first matches or a minute of
// running).
// `safeMode` (platform/health.h): the app runs as without a helper (no data
// transport, no pictures, no update checks; `helper` is ignored) and shows the
// banner "Safe mode: updates paused"; Settings and Wi-Fi work as usual.
bool app_host_start(Prefs& prefs, Backlight& backlight, EspWifiService& wifi, EspPowerService& power,
                    I2sSoundOut* speaker, const HelperConfig& helper, const OtaBoot& boot, bool safeMode);

// The [status] lines of :status the LVGL task owns: Wi-Fi, battery, the
// update trial and row, data path. LVGL task (post it).
void app_host_print_status();

// Any task: the running image was just installed and is not confirmed yet
// (until the first data, or 60 s of running). Any restart before then (the
// power key, :restart, :wipe-wifi, :erase-settings, a crash) brings the
// previous image back.
bool app_host_trial_pending();

// The [ota] lines (slots, states, trial record, the update row). LVGL task (post it).
void app_host_print_ota();

// The [power] line (battery reading, calibration, model, setting, key, hold
// pin, USB data). LVGL task (post it).
void app_host_print_power();

// Queues `fn` to run in the LVGL task (for serial commands that touch state
// the LVGL task owns, such as the settings in flash). Same queue and the same
// drop rule as app_host_post.
void app_host_post_call(void (*fn)());

// Queues `fn(arg)` to run in the LVGL task: the value travels with the
// command, so two quick commands each run with their own.
void app_host_post_call_arg(void (*fn)(int), int arg);

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

#if APP_DIAG
// Diagnostics builds (LVGL task, post them). Both make the next power-off a DRY
// RUN: every step but the GPIO7 write (logged instead), then the off state.
// The switch-off sequence now, as a long press would start it.
void app_host_diag_power_off();
// A power key press of `ms` milliseconds, as if the key were held (OR'ed with
// the real key).
void app_host_diag_key(int ms);
// Opens the team picker (closes it when open) and prints how long building it
// took (create + layout), the rows built, and the internal/PSRAM/LVGL memory it
// took; a frame trace starts with it (v prints the frames that followed).
void app_host_diag_picker();
// The active screen as LVGL rendered it, printed as [shot] lines of RGB565 hex
// (240 x 320): what the renderer produced, for comparing with the simulator.
void app_host_diag_shot();
// Measurement: one kind of decoration off on every object of the shown tab
// (0 radius, 1 images, 2 labels, 3 backgrounds, 4 gradients, 5 count layered objects).
void app_host_diag_strip(int what);
#endif

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
// Software update tests against a local server (diagnostics builds only):
// the manifest address (http allowed, no token sent), a check now and an
// install now (exactly as the row's tap: the battery rule applies).
void app_host_diag_ota_url(const std::string& url);  // any task
void app_host_diag_ota_check();                      // LVGL task (post it)
void app_host_diag_ota_install();                    // LVGL task (post it)
// The next `n` picture decodes run with every LVGL allocation of 1 KB or more
// failing (as with PSRAM full); they must end as placeholders (LVGL task).
void app_host_diag_img_fail(int n);
#endif
