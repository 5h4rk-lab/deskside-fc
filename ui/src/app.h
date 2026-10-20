// Top-level UI construction. Shared by the simulator and the ESP32 firmware, so
// it must stay free of SDL/desktop-only dependencies.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>

#include "data_source.h"
#include "image_provider.h"
#include "store.h"

// What the UI needs from its host. All references must outlive the UI.
struct AppDeps {
  Store& store;
  ImageProvider& images;
  // Match details are fetched from here when a match is opened.
  DataSource& source;
  // Optional: also called when the user opens a match (for example by tapping
  // the score on the Live tab), after the detail overlay is shown.
  std::function<void(const std::string& matchId)> onOpenMatch;
};

// Builds the whole UI under `root` (normally lv_screen_active()) and returns
// the app root object (the tab view).
lv_obj_t* app_create(lv_obj_t* root, const AppDeps& deps);

// Updates every screen from the Store. `now` is epoch seconds; the UI never
// reads the clock itself. Call it whenever data changes and about once a
// second for the clock.
void app_refresh(lv_obj_t* appRoot, int64_t now);

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
