// The simulator's scripted battery: --battery parsing, the b-key cycle and
// the terminal line; the raw-voltage service on the real model.
#include <doctest/doctest.h>

#include "mock_power.h"

TEST_CASE("mock power: demo default is 78 percent on battery") {
  MockPower p;
  const PowerStatus s = p.status();
  CHECK(s.present);
  CHECK(s.percent == 78);
  CHECK_FALSE(s.charging);
  CHECK(lipo_percent(s.millivolts) == 78);
}

TEST_CASE("mock power: --battery values") {
  PowerStatus s;
  REQUIRE(MockPower::parse("78", s));
  CHECK(s.present);
  CHECK(s.percent == 78);
  CHECK_FALSE(s.charging);
  REQUIRE(MockPower::parse("55,charging", s));
  CHECK(s.percent == 55);
  CHECK(s.charging);
  REQUIRE(MockPower::parse("0", s));
  CHECK(s.percent == 0);
  REQUIRE(MockPower::parse("100,charging", s));
  CHECK(s.percent == 100);
  REQUIRE(MockPower::parse("none", s));
  CHECK_FALSE(s.present);
  for (const char* bad : {"", "101", "-1", "abc", "50,", "50,charge", "50,charging,x", "5x", ",charging"}) {
    CAPTURE(bad);
    CHECK_FALSE(MockPower::parse(bad, s));
  }
}

TEST_CASE("mock power: the b key cycles through the demo states and wraps") {
  MockPower p;
  struct Want {
    bool present;
    int percent;
    bool charging;
  };
  const Want want[] = {{true, 40, false}, {true, 18, false}, {true, 8, false}, {true, 55, true},
                       {true, 100, true}, {false, 0, false}, {true, 78, false}, {true, 40, false}};
  for (const Want& w : want) {
    const PowerStatus& s = p.next_demo();
    CHECK(p.status() == s);
    CHECK(s.present == w.present);
    if (!w.present) continue;
    CHECK(s.percent == w.percent);
    CHECK(s.charging == w.charging);
  }
}

TEST_CASE("mock power: terminal line") {
  CHECK(MockPower::describe(PowerStatus{true, 18, false, 3770}) == "battery 18% on battery (3.77 V)");
  CHECK(MockPower::describe(PowerStatus{true, 55, true, 4010}) == "battery 55% charging (4.01 V)");
  CHECK(MockPower::describe(PowerStatus{}) == "no battery");
}

TEST_CASE("mock power: voltages match the curve") {
  for (int p = 0; p <= 100; ++p) {
    CAPTURE(p);
    CHECK(lipo_percent(MockPower::millivolts_for(p, false)) == p);
    CHECK(MockPower::millivolts_for(p, true) >= MockPower::millivolts_for(p, false));
    CHECK(MockPower::millivolts_for(p, true) <= 4200);
  }
}

TEST_CASE("model power: raw millivolts go through the model") {
  ModelPower p(3950);
  CHECK_FALSE(p.status().present);
  p.sample(1000);
  CHECK(p.status().present);
  CHECK(p.status().percent == 50);
  p.set_millivolts(10);  // unplugged pack
  p.sample(1001);
  p.sample(1002);
  CHECK_FALSE(p.status().present);
}
