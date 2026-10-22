// Low-battery banner: a slim strip across the top of the screen (over the
// status row) with a warning sign and one line of text, amber for "low" and
// the warn colour for "very low", on a dark backdrop. It removes itself after the alert's
// duration or when tapped; its timer goes with it, so it is safe to outlive
// whoever showed it. Shared with the ESP32 firmware: no clock reads, no SDL,
// no stdio.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "battery_model.h"

constexpr int32_t kBatteryBannerHeight = 20;

// Creates the banner as a child of `parent` at its top left, full width.
lv_obj_t* battery_banner_create(lv_obj_t* parent, const Alert& alert);

struct BatteryBannerParts {
  lv_obj_t* icon = nullptr;  // LV_SYMBOL_WARNING in the alert colour
  lv_obj_t* text = nullptr;
  Alert::Level level = Alert::Level::Low;
};
// Parts of a banner made by battery_banner_create(), or nullptr for any other object.
const BatteryBannerParts* battery_banner_parts(lv_obj_t* banner);

// The safe-mode banner (the device restarted abnormally too often: no data,
// no pictures, no update checks): the same slim amber strip at the top left,
// permanent, saying kSafeModeText. It leaves the top right kSafeModeGearRoom
// pixels free so the Settings gear stays visible, and it takes no touches
// (they reach what lies below).
constexpr const char* kSafeModeText = "Safe mode: updates paused";
constexpr int32_t kSafeModeGearRoom = 32;
lv_obj_t* safe_mode_banner_create(lv_obj_t* parent);
// The text label of a banner made by safe_mode_banner_create(), or nullptr.
lv_obj_t* safe_mode_banner_text(lv_obj_t* banner);
