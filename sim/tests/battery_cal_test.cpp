// Battery ADC formula and calibration (`:cal-battery`, NVS batt.cal).
#include <doctest/doctest.h>

#include <string>

#include "battery_cal.h"
#include "prefs.h"

using namespace battery_cal;

TEST_CASE("battery cal: the factory formula is ADC x 3 / 0.990476, rounded") {
  CHECK(battery_mv_from_adc(0) == 0);
  CHECK(battery_mv_from_adc(1000) == 3029);   // 3028.85
  CHECK(battery_mv_from_adc(1354) == 4101);   // 4101.07: about what the board shows with the pack on USB
  CHECK(battery_mv_from_adc(1380) == 4180);   // 4179.8
  CHECK(battery_mv_from_adc(1400) == 4240);   // 4240.39
}

TEST_CASE("battery cal: the key fits NVS and the documented range") {
  CHECK(prefs_key_valid(kPrefKey));
  CHECK(std::string(kPrefKey) == "batt.cal");
  CHECK(kMinMv == 3000);
  CHECK(kMaxMv == 4400);
}

TEST_CASE("battery cal: parse the command argument") {
  CHECK(parse_cal_command("").kind == CalCommand::Kind::Show);
  CHECK(parse_cal_command("clear").kind == CalCommand::Kind::Clear);
  const struct {
    const char* arg;
    int mv;
  } ok[] = {{"3000", 3000}, {"3700", 3700}, {"4100", 4100}, {"4400", 4400}};
  for (const auto& c : ok) {
    CAPTURE(c.arg);
    const CalCommand r = parse_cal_command(c.arg);
    CHECK(r.kind == CalCommand::Kind::Set);
    CHECK(r.mv == c.mv);
  }
  for (const char* bad : {"2999", "4401", "0", "-3700", "3700mV", "3700 mV", "3.7", "37000", "03700", "abc",
                          "Clear", "clear now", " 3700", "3700 ", "99999999999999999999"}) {
    CAPTURE(bad);
    CHECK(parse_cal_command(bad).kind == CalCommand::Kind::Invalid);
  }
}

TEST_CASE("battery cal: the scale between a multimeter reading and the factory reading") {
  CHECK(battery_cal_ppm(4100, 4100) == kPpmOne);
  CHECK(battery_cal_ppm(4000, 4100) == 975610);   // 0.9756098
  CHECK(battery_cal_ppm(4150, 4100) == 1012195);  // 1.0121951
  CHECK(battery_cal_ppm(3280, 4100) == 800000);   // exactly -20 %
  CHECK(battery_cal_ppm(4920, 4100) == 1200000);  // exactly +20 %
  CHECK_FALSE(battery_cal_ppm(3270, 4100));       // more than 20 % off
  CHECK_FALSE(battery_cal_ppm(4400, 3000));
  CHECK_FALSE(battery_cal_ppm(4000, 0));
  CHECK_FALSE(battery_cal_ppm(4000, -5));
}

TEST_CASE("battery cal: applying a scale") {
  CHECK(apply_battery_cal(4100, kPpmOne) == 4100);
  CHECK(apply_battery_cal(4100, 975610) == 4000);
  CHECK(apply_battery_cal(4100, 1012195) == 4150);
  CHECK(apply_battery_cal(0, 1100000) == 0);
  // Round trip: the calibrated reading equals the multimeter value.
  for (int measured = 3300; measured <= 4300; measured += 37) {
    for (int truth : {measured - 300, measured - 41, measured, measured + 17, measured + 250}) {
      const std::optional<int> ppm = battery_cal_ppm(truth, measured);
      REQUIRE(ppm);
      CHECK(apply_battery_cal(measured, *ppm) == truth);
    }
  }
}

TEST_CASE("battery cal: stored, loaded, cleared; malformed or out of range is no calibration") {
  MemoryPrefs p;
  CHECK_FALSE(load_battery_cal(p));
  save_battery_cal(p, 1012195);
  std::string v;
  REQUIRE(p.get("batt.cal", v));
  CHECK(v == "1012195");
  CHECK(load_battery_cal(p) == 1012195);
  clear_battery_cal(p);
  CHECK_FALSE(p.get("batt.cal", v));
  CHECK_FALSE(load_battery_cal(p));
  for (const char* bad : {"", "abc", "799999", "1200001", "-1000000", "1e6"}) {
    CAPTURE(bad);
    p.set("batt.cal", bad);
    CHECK_FALSE(load_battery_cal(p));
  }
  p.set("batt.cal", "800000");
  CHECK(load_battery_cal(p) == 800000);
}
