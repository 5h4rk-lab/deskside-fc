#include <doctest/doctest.h>

#include <string>

#include "prefs.h"

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
