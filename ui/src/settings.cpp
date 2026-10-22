#include "settings.h"

#include <cstdlib>

#include "timezone.h"

namespace {

const uint32_t kDefaultColor = 0xEF0107;

Team default_favourite() {
  Team t;
  t.id = "359";
  t.abbr = "ARS";
  t.name = "Arsenal";
  t.color = kDefaultColor;
  t.primary = kDefaultColor;
  t.crest = "/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fi%2Fteamlogos%2Fsoccer%2F500%2F359.png&w=48&h=48";
  t.league = "eng.1";
  return t;
}

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

std::string get_or(const Prefs& prefs, const char* key, const std::string& def) {
  std::string v;
  return prefs.get(key, v) ? v : def;
}

// Exactly six hex digits, either case.
bool parse_color(const std::string& s, uint32_t& out) {
  if (s.size() != 6) return false;
  uint32_t v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = v * 16 + static_cast<uint32_t>(d);
  }
  out = v;
  return true;
}

std::string color_text(uint32_t color) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string s(6, '0');
  for (int i = 5; i >= 0; --i) {
    s[static_cast<size_t>(i)] = kHex[color & 0xF];
    color >>= 4;
  }
  return s;
}

// A stored volume: the nearest step of kSoundVolumes (ties go to the lower),
// the default when it is further than half a step outside the range.
int sound_volume_from(int v) {
  if (v < kSoundVolumes[0] - 10 || v > kSoundVolumes[4] + 10) return kDefaultSoundVolume;
  int best = kSoundVolumes[0];
  for (int step : kSoundVolumes) {
    if (std::abs(step - v) < std::abs(best - v)) best = step;
  }
  return best;
}

}  // namespace

Settings::Settings() : favourite(default_favourite()) {}

Settings load_settings(const Prefs& prefs) {
  Settings s;
  const Team def = default_favourite();

  const std::string id = get_or(prefs, "fav.id", "");
  const std::string name = get_or(prefs, "fav.name", "");
  if (!id.empty() && !name.empty()) {
    Team t;
    t.id = id;
    t.name = name;
    t.abbr = get_or(prefs, "fav.abbr", "");
    t.crest = get_or(prefs, "fav.crest", "");
    t.league = get_or(prefs, "fav.league", "");
    t.color = def.color;
    uint32_t c;
    if (parse_color(get_or(prefs, "fav.color", ""), c)) t.color = c;
    t.primary = t.color;
    if (parse_color(get_or(prefs, "fav.primary", ""), c)) t.primary = c;
    s.favourite = t;
  }

  const std::string zone = get_or(prefs, "zone", s.zoneId);
  s.zoneId = find_zone(zone) ? zone : "UTC";

  s.dayBrightness = clamp(pref_int(prefs, "bright", s.dayBrightness), 10, 100);
  s.nightDim = pref_bool(prefs, "night", s.nightDim);
  s.nightStartHour = clamp(pref_int(prefs, "night.start", s.nightStartHour), 0, 23);
  s.nightEndHour = clamp(pref_int(prefs, "night.end", s.nightEndHour), 0, 23);
  s.nightPercent = clamp(pref_int(prefs, "night.pct", s.nightPercent), 5, 100);
  s.reducedMotion = pref_bool(prefs, "reduced", s.reducedMotion);
  const int mode = pref_int(prefs, "snd.mode", static_cast<int>(s.soundMode));
  if (mode >= 0 && mode <= 2) s.soundMode = static_cast<SoundMode>(mode);
  s.soundVolume = sound_volume_from(pref_int(prefs, "snd.vol", s.soundVolume));
  s.soundMuteAtNight = pref_bool(prefs, "snd.night", s.soundMuteAtNight);
  return s;
}

void save_settings(Prefs& prefs, const Settings& s) {
  prefs.set("fav.id", s.favourite.id);
  prefs.set("fav.abbr", s.favourite.abbr);
  prefs.set("fav.name", s.favourite.name);
  prefs.set("fav.color", color_text(s.favourite.color));
  prefs.set("fav.primary", color_text(s.favourite.primary));
  prefs.set("fav.crest", s.favourite.crest);
  prefs.set("fav.league", s.favourite.league);
  prefs.set("zone", s.zoneId);
  set_pref_int(prefs, "bright", s.dayBrightness);
  set_pref_bool(prefs, "night", s.nightDim);
  set_pref_int(prefs, "night.start", s.nightStartHour);
  set_pref_int(prefs, "night.end", s.nightEndHour);
  set_pref_int(prefs, "night.pct", s.nightPercent);
  set_pref_bool(prefs, "reduced", s.reducedMotion);
  set_pref_int(prefs, "snd.mode", static_cast<int>(s.soundMode));
  set_pref_int(prefs, "snd.vol", s.soundVolume);
  set_pref_bool(prefs, "snd.night", s.soundMuteAtNight);
}
