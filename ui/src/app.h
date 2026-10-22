// Top-level UI construction. Shared by the simulator and the ESP32 firmware, so
// it must stay free of SDL/desktop-only dependencies.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>

#include "backlight.h"
#include "battery_model.h"
#include "data_source.h"
#include "image_provider.h"
#include "model.h"
#include "power_service.h"
#include "prefs.h"
#include "settings.h"
#include "sound_out.h"
#include "store.h"
#include "wifi_service.h"

// What the UI needs from its host. All references must outlive the UI.
struct AppDeps {
  Store& store;
  ImageProvider& images;
  // Match details are fetched from here when a match is opened, and the team
  // list when the team picker opens without one.
  DataSource& source;
  // Optional: also called when the user opens a match (for example by tapping
  // the score on the Live tab), after the detail overlay is shown.
  std::function<void(const std::string& matchId)> onOpenMatch;
  // User settings (favourite, time zone, brightness...). Loaded by
  // app_create() and saved on every change made in Settings.
  Prefs& prefs;
  // Optional: driven from app_refresh() (brightness and night dimming).
  Backlight* backlight = nullptr;
  // Optional: the Wi-Fi hardware. Without it Settings has no Wi-Fi row and
  // there is no guided setup. With it, app_create() opens the guided setup
  // when nothing is saved and the device is not connected.
  WifiService* wifi = nullptr;
  // Optional: called once for every successful join started on the Wi-Fi
  // pages (first setup or from Settings), when the service reports it, even
  // if the user has left the page or closed the Wi-Fi overlay meanwhile;
  // never for failed or replaced attempts, nor after the app is deleted.
  // The host asks for fresh data here (Poller::request_all_now()).
  std::function<void()> onWifiConnected = nullptr;
  // Optional: the battery. app_refresh() reads it once per call into the
  // Store (Store::power()); the status rows show the battery icon, Settings a
  // Battery line, and a banner warns when it runs low. Without it there is no
  // battery anywhere in the UI.
  PowerService* power = nullptr;
  // Optional: the speaker. The UI plays through it for the favourite's goals
  // and wins, the low-battery banner and a Wi-Fi join (what, when and how
  // loud: sound_policy.h and the Sound rows in Settings). Without it there is
  // no sound and no Sound row in Settings.
  SoundOut* sound = nullptr;
};

// Night dimming lifts for this long after the last touch.
constexpr uint32_t kTouchWakeMs = 30000;

// Builds the whole UI under `root` (normally lv_screen_active()) and returns
// the app root object (the tab view). Loads the settings from deps.prefs and
// applies them: the Store's favourite, the accent colour and reduced motion.
lv_obj_t* app_create(lv_obj_t* root, const AppDeps& deps);

// Updates every screen from the Store. `now` is epoch seconds; the UI never
// reads the clock itself. Call it whenever data changes and about once a
// second for the clock. Also sets the Store's timezone offset from the chosen
// zone at `now` (daylight saving included) and drives the backlight: the day
// brightness, or the night level inside the night window unless the screen
// was touched in the last kTouchWakeMs. The backlight is only told about
// changes (and once at the first call). With a PowerService it reads the
// battery once (into the Store; widgets change only when it does) and drives
// the low-battery banner (BatteryAlert: at most once per level and discharge
// cycle, held back while the goal celebration shows).
void app_refresh(lv_obj_t* appRoot, int64_t now);

// The settings in use.
const Settings* app_settings(lv_obj_t* appRoot);

// Opens Settings as an overlay above the tab content (the tab bar stays
// visible; picking a tab closes it). The gear on the Live tab calls this.
void app_open_settings(lv_obj_t* appRoot);
void app_close_settings(lv_obj_t* appRoot);
// The settings screen in the overlay, or nullptr when Settings is closed.
lv_obj_t* app_settings_view(lv_obj_t* appRoot);

// Opens the time zone list over Settings (opening Settings first if needed).
void app_open_zones(lv_obj_t* appRoot);
// The zone list, or nullptr when it is closed.
lv_obj_t* app_zone_view(lv_obj_t* appRoot);

// Opens the team picker, covering the whole screen (tab bar included, so the
// keyboard leaves room for the list). Fetches the team list when the Store
// has none; a failed fetch shows an error with "Try again".
void app_open_team_picker(lv_obj_t* appRoot);
// The picker, or nullptr when it is closed.
lv_obj_t* app_team_picker_view(lv_obj_t* appRoot);

// Opens the Wi-Fi pages (network list, password page) as a full-screen
// overlay: from the Settings Wi-Fi row, or by app_create() in first-setup
// mode. Back closes it. Does nothing without a Wi-Fi service.
void app_open_wifi(lv_obj_t* appRoot);
void app_close_wifi(lv_obj_t* appRoot);
// The Wi-Fi screen (see screens/wifi.h), or nullptr when it is closed.
lv_obj_t* app_wifi_view(lv_obj_t* appRoot);

// Makes `team` the favourite: saves it, sets the Store's favourite and the
// accent colour, rebuilds the three tab screens and the tab bar icons (and an
// open Settings screen) in the new colour, keeps the selected tab, closes the
// picker and refreshes with the last `now`.
void app_apply_team(lv_obj_t* appRoot, const Team& team);

// Opens the match detail as an overlay above the tab content (the tab bar
// stays visible). Shows the cached detail from the Store at once, or a loading
// skeleton, and fetches a fresh one (once per call); the result goes into the
// Store and onto the overlay. A failed fetch without cached data shows an
// error with "Try again". Back (the control or a swipe down/right) closes it.
void app_open_match(lv_obj_t* appRoot, const std::string& matchId);

// Closes the detail overlay, if open.
void app_close_detail(lv_obj_t* appRoot);

// The detail (or loading/error placeholder) shown in the overlay, or nullptr
// when no detail is open. For tests and screenshots.
lv_obj_t* app_detail_view(lv_obj_t* appRoot);

// Id of the match whose detail overlay is open, or "" when none is (the host
// passes it to Poller::set_detail_watch).
std::string app_open_match_id(lv_obj_t* appRoot);

// Call after every Store::set_matches(). Runs the goal detector on the new
// match list; for any goal or full-time event it fetches that match's detail
// into the Store (the Live strip shows the new event) and, for the favourite's
// goals and wins, starts the goal celebration (with its sound: the goal horn,
// or at a win the full-time whistles and then the fanfare; opponents' goals
// and first sightings never make a sound) and then fills in the scorer
// from the detail, once a detail has as many goals of the scoring side as its
// score (scorer_for_goal): app_refresh() keeps checking the Store's detail
// while the overlay shows. The first call never celebrates (first sight of a
// match).
// `now` is epoch seconds, used to refresh the screens when the detail arrives.
void app_matches_changed(lv_obj_t* appRoot, int64_t now);

// The low-battery banner while it shows (above the status row and above any
// overlay), or nullptr. It goes by itself after the alert's duration or when
// tapped.
lv_obj_t* app_battery_banner(lv_obj_t* appRoot);

// Shows the banner for `alert` now, replacing one that shows (screenshots
// and tests; app_refresh() decides by itself otherwise).
void app_show_battery_banner(lv_obj_t* appRoot, const Alert& alert);
