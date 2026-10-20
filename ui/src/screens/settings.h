// Settings screen: favourite team, time zone, brightness, night dimming,
// reduced motion and (when the host supports it) Wi-Fi, under a "< Back"
// header. The app hosts it as an overlay above the tab content.
//
// The screen keeps its own copy of the Settings. Every change made on it
// updates that copy, refreshes the affected texts and calls onChanged() with
// the new values; the host saves and applies them. Shared with the ESP32
// firmware: no clock reads, no SDL, no stdio.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>

#include "../settings.h"  // the Settings model (not this file)
#include "image_provider.h"

struct SettingsCallbacks {
  std::function<void()> onBack;
  std::function<void()> onTeam;  // the Favourite team row
  std::function<void()> onZone;  // the Time zone row
  std::function<void()> onWifi;  // the Wi-Fi row; when empty there is no Wi-Fi row
  // While the brightness slider is dragged (not saved yet): the host previews it.
  std::function<void(int percent)> onBrightnessPreview;
  // After any change made on the screen (brightness released, a switch, a
  // stepper), with the screen's new settings.
  std::function<void(const Settings&)> onChanged;
};

// Ranges and steps of the controls.
constexpr int kBrightnessMin = 10;
constexpr int kBrightnessMax = 100;
constexpr int kNightPercentMin = 5;
constexpr int kNightPercentMax = 100;
constexpr int kNightPercentStep = 5;

// Fixed texts (exact wording).
constexpr const char* kNightHelp = "Dims the screen at night. Touch the screen to wake it.";
constexpr const char* kMotionHelp = "Skips the goal animation effects.";

// Builds the screen as a child of `parent`, filling it. `now` (epoch seconds)
// gives the zone's current offset and the local time.
lv_obj_t* settings_create(lv_obj_t* parent, const Settings& settings, ImageProvider& images,
                          const SettingsCallbacks& callbacks, int64_t now);

// Shows `settings` as of `now`. Only texts and controls that changed are
// touched, so calling it every second (or while the slider is dragged) does
// not rebuild anything.
void settings_refresh(lv_obj_t* screen, const Settings& settings, int64_t now);

// The dim line under "Wi-Fi" (the host passes wifi_status_text()). Does
// nothing without a Wi-Fi row.
void settings_set_wifi_status(lv_obj_t* screen, const std::string& text);

// The dim line under the version in the About area: how the live data is
// doing (the host passes Store::data_status_text()). Amber when `problem`.
// Empty until the host sets it.
void settings_set_data_status(lv_obj_t* screen, const std::string& text, bool problem);

// The settings the screen currently shows (its copy).
const Settings* settings_current(lv_obj_t* screen);

// Widgets of a settings screen, for tests and screenshots.
struct SettingsParts {
  lv_obj_t* back = nullptr;   // "< Back" tap target
  lv_obj_t* title = nullptr;  // "Settings"
  lv_obj_t* list = nullptr;   // scrolling column of cards

  lv_obj_t* teamRow = nullptr;  // tap: onTeam
  lv_obj_t* teamCrest = nullptr;
  lv_obj_t* teamName = nullptr;
  lv_obj_t* teamLeague = nullptr;

  lv_obj_t* zoneRow = nullptr;    // tap: onZone
  lv_obj_t* zoneLabel = nullptr;  // "London, Dublin, Lisbon"
  lv_obj_t* zoneOffset = nullptr; // "UTC+1 · Local time 14:05"

  lv_obj_t* brightValue = nullptr;  // "80%"
  lv_obj_t* brightSlider = nullptr;

  lv_obj_t* nightSwitch = nullptr;
  lv_obj_t* nightControls = nullptr;  // the steppers, hidden while night dimming is off
  lv_obj_t* nightFrom = nullptr;      // "23:00"
  lv_obj_t* nightFromMinus = nullptr;
  lv_obj_t* nightFromPlus = nullptr;
  lv_obj_t* nightTo = nullptr;  // "07:00"
  lv_obj_t* nightToMinus = nullptr;
  lv_obj_t* nightToPlus = nullptr;
  lv_obj_t* nightPercent = nullptr;  // "20%"
  lv_obj_t* nightPercentMinus = nullptr;
  lv_obj_t* nightPercentPlus = nullptr;
  lv_obj_t* nightHelp = nullptr;

  lv_obj_t* motionSwitch = nullptr;
  lv_obj_t* motionHelp = nullptr;

  lv_obj_t* wifiRow = nullptr;     // nullptr when there is no Wi-Fi row
  lv_obj_t* wifiStatus = nullptr;  // "Connected to <ssid>" / "Not connected", one line
  lv_obj_t* about = nullptr;    // "Soccer display 0.1"
  lv_obj_t* dataStatus = nullptr;  // under about: "Live data is up to date", ...
};

// Parts of a screen made by settings_create(), or nullptr for any other object.
const SettingsParts* settings_parts(lv_obj_t* screen);
