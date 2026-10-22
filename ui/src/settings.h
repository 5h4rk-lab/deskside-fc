// User settings kept in Prefs. Loading validates and clamps, so a corrupt
// store never yields a half-filled favourite or out-of-range values.
#pragma once

#include <string>

#include "model.h"
#include "power_service.h"
#include "prefs.h"

// What the speaker plays: nothing, the goals and wins of the favourite, or
// those plus kick-off and full-time whistles. Stored as 0, 1, 2.
enum class SoundMode { Off = 0, GoalsOnly = 1, GoalsAndAlerts = 2 };

constexpr int kSoundVolumes[5] = {20, 40, 60, 80, 100};  // percent, the steps of the volume control
constexpr int kDefaultSoundVolume = 40;

struct Settings {
  Team favourite;  // defaults to Arsenal
  std::string zoneId = "UTC";
  int dayBrightness = 100;  // percent, 10..100
  bool nightDim = true;
  int nightStartHour = 23;  // 0..23, inclusive
  int nightEndHour = 7;     // 0..23, exclusive
  int nightPercent = 20;    // percent, 5..100
  bool reducedMotion = false;
  SoundMode soundMode = SoundMode::GoalsOnly;
  int soundVolume = kDefaultSoundVolume;  // one of kSoundVolumes
  bool soundMuteAtNight = true;           // silent while night dimming is active
  BatteryChoice battery = BatteryChoice::Auto;  // Auto, Installed, None

  Settings();
};

// Keys: fav.id fav.abbr fav.name fav.color (hex) fav.primary (hex) fav.crest
// fav.league zone bright night night.start night.end night.pct reduced snd.mode
// snd.vol snd.night batt.
// A missing or malformed fav.primary (settings saved by an older build) is the
// favourite's colour. A stored sound mode that is not 0..2 is the default; a
// stored volume is the nearest of 20/40/60/80/100, or the default (40) when it
// is not a number or outside 10..110. A stored battery choice that is not
// 0..2 is Auto.
Settings load_settings(const Prefs& prefs);
void save_settings(Prefs& prefs, const Settings& settings);
