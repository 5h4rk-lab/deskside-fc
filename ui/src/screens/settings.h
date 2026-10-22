// Settings screen: favourite team, time zone, brightness, night dimming,
// reduced motion, sound (when the host has a speaker), battery (when the host
// has battery hardware), (when the host supports it) Wi-Fi and Software
// update, under a "< Back" header. The app hosts it as an overlay above the tab content.
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
#include "battery_model.h"
#include "image_provider.h"
#include "update_check.h"

struct SettingsCallbacks {
  std::function<void()> onBack;
  std::function<void()> onTeam;  // the Favourite team row
  std::function<void()> onZone;  // the Time zone row
  std::function<void()> onWifi;  // the Wi-Fi row; when empty there is no Wi-Fi row
  // The "Test sound" button; when empty there are no sound rows (no speaker).
  std::function<void()> onTestSound;
  // The host has battery hardware: the Battery card (Auto, Installed, None).
  bool battery = false;
  // A tap on the Software update row; when empty there is no such row. What
  // the tap does (check, install) is the host's business: the row only
  // shows what settings_set_update() gives it.
  std::function<void()> onUpdate;
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
constexpr const char* kSoundHelp = "The speaker only plays for your team.";
constexpr const char* kSoundNightHelp = "Quiet during the night dimming hours.";
// Under Mute at night while night dimming is off (it only mutes inside the night window).
constexpr const char* kSoundNightNeedsDim = "Needs night dimming on";
constexpr const char* kBatteryHelp = "Pick None if no battery is connected.";
// The three choices of Battery, in BatteryChoice order.
constexpr const char* kBatteryChoiceText[3] = {"Auto", "Installed", "None"};
// The three choices of Sound, in SoundMode order.
constexpr const char* kSoundModeText[3] = {"Off", "Goals only", "Goals and match alerts"};

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

// The Software update row: update_status_text(status) under the title,
// amber for a problem, the chosen version's line brighter while an install is
// offered, and a progress bar while an install runs (update_status_busy).
// Does nothing without the row (no onUpdate).
void settings_set_update(lv_obj_t* screen, const UpdateStatus& status);

// The Battery line in the About area, between the version and the data line:
// battery_status_text() ("78% · On battery", "Low: 18% · On battery", ...)
// with battery_voltage_text() after it in smaller dim text. Amber when low on
// battery. Hidden until the host sets it (hosts without a battery never do).
void settings_set_battery(lv_obj_t* screen, const PowerStatus& power);

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

  // Sound, all nullptr without onTestSound.
  lv_obj_t* soundRows[3] = {nullptr, nullptr, nullptr};   // tap: choose the mode
  lv_obj_t* soundTicks[3] = {nullptr, nullptr, nullptr};  // visible on the chosen mode
  lv_obj_t* soundHelp = nullptr;
  lv_obj_t* volumeValue = nullptr;  // "40%"
  lv_obj_t* volumeMinus = nullptr;
  lv_obj_t* volumePlus = nullptr;
  lv_obj_t* soundNightSwitch = nullptr;  // "Mute at night"
  lv_obj_t* soundNightHelp = nullptr;
  lv_obj_t* soundNightNeedsDim = nullptr;  // "Needs night dimming on", shown while night dimming is off
  lv_obj_t* testSound = nullptr;  // "Test sound" button

  // Battery choice, all nullptr without SettingsCallbacks::battery.
  lv_obj_t* batteryChoiceRows[3] = {nullptr, nullptr, nullptr};   // tap: choose Auto / Installed / None
  lv_obj_t* batteryChoiceTicks[3] = {nullptr, nullptr, nullptr};  // visible on the chosen one
  lv_obj_t* batteryChoiceHelp = nullptr;

  lv_obj_t* wifiRow = nullptr;     // nullptr when there is no Wi-Fi row
  lv_obj_t* wifiStatus = nullptr;  // "Connected to <ssid>" / "Not connected", one line
  lv_obj_t* updateRow = nullptr;     // nullptr when there is no Software update row (tap: onUpdate)
  lv_obj_t* updateStatus = nullptr;  // "Up to date (0.1.0)", "Downloading 42%", ... (wraps to two lines)
  lv_obj_t* updateBar = nullptr;     // progress, hidden unless an install runs
  lv_obj_t* about = nullptr;    // "Soccer display 0.1.0"
  lv_obj_t* batteryLine = nullptr;     // under about: status and voltage, hidden until set
  lv_obj_t* batteryStatus = nullptr;   // "78% · On battery"
  lv_obj_t* batteryVoltage = nullptr;  // "3.92 V", hidden without a battery
  lv_obj_t* dataStatus = nullptr;  // under about: "Live data is up to date", ...
};

// Parts of a screen made by settings_create(), or nullptr for any other object.
const SettingsParts* settings_parts(lv_obj_t* screen);
