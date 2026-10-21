#include <doctest/doctest.h>

#include <string>

#include <vector>

#include "prefs.h"
#include "settings.h"

TEST_CASE("MemoryPrefs get, set and remove") {
  MemoryPrefs p;
  std::string out = "untouched";
  CHECK_FALSE(p.get("a", out));
  CHECK(out == "untouched");
  p.set("a", "1");
  CHECK(p.get("a", out));
  CHECK(out == "1");
  p.set("a", "2");
  CHECK(p.get("a", out));
  CHECK(out == "2");
  p.remove("a");
  CHECK_FALSE(p.get("a", out));
  p.remove("missing");  // no-op
}

TEST_CASE("pref_int parses, defaults on missing or garbage") {
  MemoryPrefs p;
  CHECK(pref_int(p, "n", 7) == 7);
  set_pref_int(p, "n", 42);
  CHECK(pref_int(p, "n", 7) == 42);
  set_pref_int(p, "n", -15);
  CHECK(pref_int(p, "n", 7) == -15);
  set_pref_int(p, "n", 0);
  CHECK(pref_int(p, "n", 7) == 0);
  for (const char* bad : {"", "-", "abc", "12x", " 5", "5 ", "+3", "--4", "1.5", "99999999999999999999"}) {
    p.set("n", bad);
    INFO("value '" << bad << "'");
    CHECK(pref_int(p, "n", 7) == 7);
  }
}

TEST_CASE("pref_bool stores 1 and 0") {
  MemoryPrefs p;
  CHECK(pref_bool(p, "b", true) == true);
  CHECK(pref_bool(p, "b", false) == false);
  set_pref_bool(p, "b", true);
  std::string raw;
  REQUIRE(p.get("b", raw));
  CHECK(raw == "1");
  CHECK(pref_bool(p, "b", false) == true);
  set_pref_bool(p, "b", false);
  p.get("b", raw);
  CHECK(raw == "0");
  CHECK(pref_bool(p, "b", true) == false);
  p.set("b", "maybe");
  CHECK(pref_bool(p, "b", true) == true);
  CHECK(pref_bool(p, "b", false) == false);
}

TEST_CASE("prefs_key_valid: 1 to 15 characters of [a-z0-9._]") {
  CHECK(prefs_key_valid("a"));
  CHECK(prefs_key_valid("zone"));
  CHECK(prefs_key_valid("fav.primary"));
  CHECK(prefs_key_valid("night.start"));
  CHECK(prefs_key_valid("a_b.c0"));
  CHECK(prefs_key_valid("123456789012345"));    // exactly 15
  CHECK_FALSE(prefs_key_valid("1234567890123456"));  // 16: over the NVS limit
  CHECK_FALSE(prefs_key_valid(""));
  CHECK_FALSE(prefs_key_valid("Zone"));
  CHECK_FALSE(prefs_key_valid("a b"));
  CHECK_FALSE(prefs_key_valid("a/b"));
  CHECK_FALSE(prefs_key_valid("a-b"));
  CHECK_FALSE(prefs_key_valid("caf\xC3\xA9"));
  CHECK_FALSE(prefs_key_valid(std::string("a\0b", 3)));
  CHECK_FALSE(prefs_key_valid("a\n"));
}

TEST_CASE("prefs_value_valid: up to 1000 bytes") {
  CHECK(prefs_value_valid(""));
  CHECK(prefs_value_valid(std::string(1000, 'x')));
  CHECK_FALSE(prefs_value_valid(std::string(1001, 'x')));
}

namespace {
// Remembers every key written.
class KeyLog : public Prefs {
 public:
  bool get(const std::string&, std::string&) const override { return false; }
  void set(const std::string& key, const std::string&) override { keys.push_back(key); }
  void remove(const std::string& key) override { keys.push_back(key); }
  std::vector<std::string> keys;
};
}  // namespace

TEST_CASE("every key the settings use is valid for NVS") {
  KeyLog log;
  Settings s;
  s.favourite = Team{"359", "ARS", "Arsenal", "/crest", 0xEF0107, 0xEF0107, "eng.1"};
  save_settings(log, s);
  CHECK(log.keys.size() >= 14);
  for (const std::string& k : log.keys) {
    INFO("key '" << k << "'");
    CHECK(prefs_key_valid(k));
  }
}
