#include <doctest/doctest.h>

#include <string>

#include "prefs.h"
#include "settings.h"

TEST_CASE("defaults from empty prefs") {
  MemoryPrefs p;
  const Settings s = load_settings(p);
  CHECK(s.favourite.id == "359");
  CHECK(s.favourite.abbr == "ARS");
  CHECK(s.favourite.name == "Arsenal");
  CHECK(s.favourite.color == 0xEF0107u);
  CHECK(s.favourite.primary == 0xEF0107u);
  CHECK(s.favourite.crest ==
        "/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fi%2Fteamlogos%2Fsoccer%2F500%2F359.png&w=48&h=48");
  CHECK(s.favourite.league == "eng.1");
  CHECK(s.zoneId == "UTC");
  CHECK(s.dayBrightness == 100);
  CHECK(s.nightDim == true);
  CHECK(s.nightStartHour == 23);
  CHECK(s.nightEndHour == 7);
  CHECK(s.nightPercent == 20);
  CHECK(s.reducedMotion == false);
}

TEST_CASE("round trip") {
  Settings s;
  s.favourite.id = "364";
  s.favourite.abbr = "LIV";
  s.favourite.name = "Liverpool";
  s.favourite.color = 0xC8102E;
  s.favourite.primary = 0x9B0A1F;
  s.favourite.crest = "/api/img?u=x&w=48&h=48";
  s.favourite.league = "eng.1";
  s.zoneId = "Europe/Paris";
  s.dayBrightness = 55;
  s.nightDim = false;
  s.nightStartHour = 22;
  s.nightEndHour = 6;
  s.nightPercent = 35;
  s.reducedMotion = true;
  MemoryPrefs p;
  save_settings(p, s);
  const Settings r = load_settings(p);
  CHECK(r.favourite.id == "364");
  CHECK(r.favourite.abbr == "LIV");
  CHECK(r.favourite.name == "Liverpool");
  CHECK(r.favourite.color == 0xC8102Eu);
  CHECK(r.favourite.primary == 0x9B0A1Fu);
  CHECK(r.favourite.crest == "/api/img?u=x&w=48&h=48");
  CHECK(r.favourite.league == "eng.1");
  CHECK(r.zoneId == "Europe/Paris");
  CHECK(r.dayBrightness == 55);
  CHECK(r.nightDim == false);
  CHECK(r.nightStartHour == 22);
  CHECK(r.nightEndHour == 6);
  CHECK(r.nightPercent == 35);
  CHECK(r.reducedMotion == true);
}

TEST_CASE("stored under the short keys") {
  MemoryPrefs p;
  save_settings(p, Settings());
  std::string v;
  for (const char* k : {"fav.id", "fav.abbr", "fav.name", "fav.color", "fav.primary", "fav.crest", "fav.league", "zone",
                        "bright", "night", "night.start", "night.end", "night.pct", "reduced"}) {
    INFO("key " << k);
    CHECK(p.get(k, v));
  }
  p.get("fav.color", v);
  CHECK(v == "EF0107");
  p.get("fav.primary", v);
  CHECK(v == "EF0107");
}

TEST_CASE("load clamps out-of-range values") {
  MemoryPrefs p;
  set_pref_int(p, "bright", 3);
  set_pref_int(p, "night.start", 30);
  set_pref_int(p, "night.end", -4);
  set_pref_int(p, "night.pct", 1);
  Settings s = load_settings(p);
  CHECK(s.dayBrightness == 10);
  CHECK(s.nightStartHour == 23);
  CHECK(s.nightEndHour == 0);
  CHECK(s.nightPercent == 5);
  set_pref_int(p, "bright", 500);
  set_pref_int(p, "night.pct", 500);
  s = load_settings(p);
  CHECK(s.dayBrightness == 100);
  CHECK(s.nightPercent == 100);
}

TEST_CASE("unknown zone falls back to UTC") {
  MemoryPrefs p;
  p.set("zone", "Mars/Olympus");
  CHECK(load_settings(p).zoneId == "UTC");
  p.set("zone", "Asia/Tokyo");
  CHECK(load_settings(p).zoneId == "Asia/Tokyo");
}

TEST_CASE("a half-filled favourite falls back entirely to the default") {
  MemoryPrefs p;
  p.set("fav.id", "");
  p.set("fav.name", "Chelsea");
  p.set("fav.abbr", "CHE");
  p.set("fav.color", "034694");
  Settings s = load_settings(p);
  CHECK(s.favourite.id == "359");
  CHECK(s.favourite.name == "Arsenal");
  CHECK(s.favourite.abbr == "ARS");
  CHECK(s.favourite.color == 0xEF0107u);

  MemoryPrefs q;
  q.set("fav.id", "363");
  q.set("fav.name", "");
  q.set("fav.abbr", "CHE");
  s = load_settings(q);
  CHECK(s.favourite.id == "359");
  CHECK(s.favourite.abbr == "ARS");

  MemoryPrefs r;  // only the id: no name
  r.set("fav.id", "363");
  CHECK(load_settings(r).favourite.id == "359");
}

TEST_CASE("malformed colour falls back to the default colour") {
  for (const char* bad : {"", "zzzzzz", "12345", "1234567", "#034694", "-12345"}) {
    MemoryPrefs p;
    p.set("fav.id", "363");
    p.set("fav.name", "Chelsea");
    p.set("fav.abbr", "CHE");
    p.set("fav.color", bad);
    const Settings s = load_settings(p);
    INFO("colour '" << bad << "'");
    CHECK(s.favourite.id == "363");
    CHECK(s.favourite.color == 0xEF0107u);
  }
  MemoryPrefs p;
  p.set("fav.id", "363");
  p.set("fav.name", "Chelsea");
  p.set("fav.color", "03469a");  // lower-case hex accepted
  CHECK(load_settings(p).favourite.color == 0x03469Au);
}

TEST_CASE("fav.primary: saved, absent or malformed falls back to the favourite's colour") {
  // Chelsea: gold accent, royal blue brand.
  MemoryPrefs p;
  p.set("fav.id", "363");
  p.set("fav.name", "Chelsea");
  p.set("fav.color", "DBA111");
  p.set("fav.primary", "034694");
  Settings s = load_settings(p);
  CHECK(s.favourite.color == 0xDBA111u);
  CHECK(s.favourite.primary == 0x034694u);
  save_settings(p, s);
  std::string v;
  REQUIRE(p.get("fav.primary", v));
  CHECK(v == "034694");

  // Settings saved by an older build have no fav.primary.
  MemoryPrefs old;
  old.set("fav.id", "363");
  old.set("fav.name", "Chelsea");
  old.set("fav.color", "DBA111");
  CHECK(load_settings(old).favourite.primary == 0xDBA111u);

  for (const char* bad : {"", "zzzzzz", "12345", "1234567", "#034694", "-12345"}) {
    MemoryPrefs q;
    q.set("fav.id", "363");
    q.set("fav.name", "Chelsea");
    q.set("fav.color", "DBA111");
    q.set("fav.primary", bad);
    INFO("primary '" << bad << "'");
    CHECK(load_settings(q).favourite.primary == 0xDBA111u);
  }

  // A malformed colour and no primary: both are the default red.
  MemoryPrefs r;
  r.set("fav.id", "363");
  r.set("fav.name", "Chelsea");
  r.set("fav.color", "nope");
  CHECK(load_settings(r).favourite.primary == 0xEF0107u);
}

TEST_CASE("sound settings: defaults are Goals only, volume 40, mute at night") {
  MemoryPrefs p;
  const Settings s = load_settings(p);
  CHECK(s.soundMode == SoundMode::GoalsOnly);
  CHECK(s.soundVolume == 40);
  CHECK(s.soundMuteAtNight == true);
}

TEST_CASE("sound settings: round trip under the keys snd.mode, snd.vol and snd.night") {
  Settings s;
  s.soundMode = SoundMode::GoalsAndAlerts;
  s.soundVolume = 80;
  s.soundMuteAtNight = false;
  MemoryPrefs p;
  save_settings(p, s);
  std::string v;
  REQUIRE(p.get("snd.mode", v));
  CHECK(v == "2");
  REQUIRE(p.get("snd.vol", v));
  CHECK(v == "80");
  REQUIRE(p.get("snd.night", v));
  CHECK(v == "0");
  const Settings r = load_settings(p);
  CHECK(r.soundMode == SoundMode::GoalsAndAlerts);
  CHECK(r.soundVolume == 80);
  CHECK(r.soundMuteAtNight == false);
  for (const char* key : {"snd.mode", "snd.vol", "snd.night"}) CHECK(prefs_key_valid(key));
}

TEST_CASE("sound settings: every volume step and mode survives a round trip") {
  for (int vol : {20, 40, 60, 80, 100}) {
    for (SoundMode m : {SoundMode::Off, SoundMode::GoalsOnly, SoundMode::GoalsAndAlerts}) {
      Settings s;
      s.soundVolume = vol;
      s.soundMode = m;
      MemoryPrefs p;
      save_settings(p, s);
      const Settings r = load_settings(p);
      CHECK(r.soundVolume == vol);
      CHECK(r.soundMode == m);
    }
  }
}

TEST_CASE("sound settings: an unknown mode is the default") {
  for (const char* bad : {"3", "-1", "99", "loud", "", "1.5"}) {
    MemoryPrefs p;
    p.set("snd.mode", bad);
    CHECK(load_settings(p).soundMode == SoundMode::GoalsOnly);
  }
  MemoryPrefs off;
  off.set("snd.mode", "0");
  CHECK(load_settings(off).soundMode == SoundMode::Off);
}

TEST_CASE("sound settings: a stored volume snaps to the nearest step, or the default when out of range") {
  const struct {
    const char* stored;
    int expected;
  } cases[] = {{"20", 20},  {"25", 20},  {"31", 40},  {"50", 40},  {"51", 60}, {"70", 60}, {"71", 80},
               {"90", 80},  {"91", 100}, {"100", 100}, {"110", 100}, {"10", 20}, {"9", 40},  {"111", 40},
               {"0", 40},   {"-40", 40}, {"5000", 40}, {"loud", 40}, {"", 40}};
  for (const auto& c : cases) {
    MemoryPrefs p;
    p.set("snd.vol", c.stored);
    INFO("stored '" << c.stored << "'");
    CHECK(load_settings(p).soundVolume == c.expected);
  }
}

TEST_CASE("sound settings: a malformed night mute flag is the default (on)") {
  MemoryPrefs p;
  p.set("snd.night", "maybe");
  CHECK(load_settings(p).soundMuteAtNight == true);
  p.set("snd.night", "0");
  CHECK(load_settings(p).soundMuteAtNight == false);
}
