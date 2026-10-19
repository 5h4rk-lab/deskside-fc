// Top-level UI construction. Shared by the simulator and the ESP32 firmware, so
// it must stay free of SDL/desktop-only dependencies.
#pragma once

#include <lvgl.h>

// Builds the whole UI under `root` (normally lv_screen_active()).
// Later tasks extend this to take an AppDeps struct; keep construction here.
void app_create(lv_obj_t* root);
