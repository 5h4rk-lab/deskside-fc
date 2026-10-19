// Top-level UI construction. Shared by the simulator and the ESP32 firmware, so
// it must stay free of SDL/desktop-only dependencies.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>

#include "image_provider.h"
#include "store.h"

// What the UI needs from its host. All references must outlive the UI.
struct AppDeps {
  Store& store;
  ImageProvider& images;
  // Called when the user asks to open a match (for example by tapping the
  // score on the Live tab).
  std::function<void(const std::string& matchId)> onOpenMatch;
};

// Builds the whole UI under `root` (normally lv_screen_active()) and returns
// the app root object (the tab view).
lv_obj_t* app_create(lv_obj_t* root, const AppDeps& deps);

// Updates every screen from the Store. `now` is epoch seconds; the UI never
// reads the clock itself. Call it whenever data changes and about once a
// second for the clock.
void app_refresh(lv_obj_t* appRoot, int64_t now);
