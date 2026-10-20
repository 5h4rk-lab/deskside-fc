// User settings kept in Prefs. Loading validates and clamps, so a corrupt
// store never yields a half-filled favourite or out-of-range values.
#pragma once

#include <string>

#include "model.h"
#include "prefs.h"

struct Settings {
  Team favourite;  // defaults to Arsenal
  std::string zoneId = "UTC";
  int dayBrightness = 100;  // percent, 10..100
  bool nightDim = true;
  int nightStartHour = 23;  // 0..23, inclusive
  int nightEndHour = 7;     // 0..23, exclusive
  int nightPercent = 20;    // percent, 5..100
  bool reducedMotion = false;

  Settings();
};

// Keys: fav.id fav.abbr fav.name fav.color (hex) fav.crest fav.league zone
// bright night night.start night.end night.pct reduced.
Settings load_settings(const Prefs& prefs);
void save_settings(Prefs& prefs, const Settings& settings);
