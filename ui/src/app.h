// Top-level UI construction. Shared by the simulator and the ESP32 firmware, so
// it must stay free of SDL/desktop-only dependencies.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>

#include "backlight.h"
#include "data_source.h"
#include "image_provider.h"
#include "model.h"
#include "prefs.h"
#include "settings.h"
#include "store.h"

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
  // Optional: opens the Wi-Fi setup. Without it Settings has no Wi-Fi row.
  std::function<void()> onOpenWifi = nullptr;
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
// changes (and once at the first call).
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

// Call after every Store::set_matches(). Runs the goal detector on the new
// match list; for any goal or full-time event it fetches that match's detail
// into the Store (the Live strip shows the new event) and, for the favourite's
// goals and wins, starts the goal celebration and then fills in the scorer
// from the detail. The first call never celebrates (first sight of a match).
// `now` is epoch seconds, used to refresh the screens when the detail arrives.
void app_matches_changed(lv_obj_t* appRoot, int64_t now);
