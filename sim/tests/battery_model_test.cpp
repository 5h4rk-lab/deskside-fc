// Battery maths: the LiPo curve, the smoothing model, charging inference, the
// low-battery alert state machine and the texts.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "battery_model.h"

namespace {

constexpr int64_t kT0 = 1784116800;  // 2026-07-15 12:00 UTC

// Feeds `mv` once a second for `seconds` seconds, starting at `t` (updated).
void feed(BatteryModel& m, int64_t& t, int mv, int seconds) {
  for (int i = 0; i < seconds; ++i) m.add_sample(mv, t++);
}

// Feeds a linear ramp from `from` to `to` over `seconds` seconds.
void ramp(BatteryModel& m, int64_t& t, int from, int to, int seconds) {
  for (int i = 0; i < seconds; ++i) {
    m.add_sample(from + (to - from) * i / seconds, t++);
  }
}

// Deterministic noise in [-amp, amp].
struct Noise {
  uint32_t state = 12345;
  int next(int amp) {
    state = state * 1103515245u + 12345u;
    return static_cast<int>((state >> 16) % static_cast<uint32_t>(2 * amp + 1)) - amp;
  }
};

PowerStatus on_battery(int percent) { return PowerStatus{true, percent, false, 3800}; }
PowerStatus charging(int percent) { return PowerStatus{true, percent, true, 4000}; }

}  // namespace

// ------------------------------------------------------------ lipo_percent

TEST_CASE("lipo_percent: table points") {
  CHECK(lipo_percent(4200) == 100);
  CHECK(lipo_percent(4150) == 95);
  CHECK(lipo_percent(4110) == 90);
  CHECK(lipo_percent(4080) == 80);
  CHECK(lipo_percent(4020) == 70);
  CHECK(lipo_percent(3980) == 60);
  CHECK(lipo_percent(3950) == 50);
  CHECK(lipo_percent(3910) == 40);
  CHECK(lipo_percent(3870) == 30);
  CHECK(lipo_percent(3790) == 20);
  CHECK(lipo_percent(3600) == 10);
  CHECK(lipo_percent(3300) == 0);
}

TEST_CASE("lipo_percent: interpolates and rounds between points") {
  CHECK(lipo_percent(3965) == 55);   // half way 3950..3980
  CHECK(lipo_percent(3450) == 5);    // half way 3300..3600
  CHECK(lipo_percent(3695) == 15);   // half way 3600..3790
  CHECK(lipo_percent(3610) == 11);   // 10 + 10 * 10 / 190 = 10.5 -> 11
  CHECK(lipo_percent(4175) == 98);   // 95 + 5 * 25 / 50 = 97.5 -> 98
}

TEST_CASE("lipo_percent: clamps outside the table") {
  CHECK(lipo_percent(4350) == 100);
  CHECK(lipo_percent(5000) == 100);
  CHECK(lipo_percent(3000) == 0);
  CHECK(lipo_percent(0) == 0);
  CHECK(lipo_percent(-5) == 0);
}

TEST_CASE("lipo_percent: never decreases with voltage") {
  int last = 0;
  for (int mv = 3000; mv <= 4400; ++mv) {
    const int p = lipo_percent(mv);
    CHECK(p >= last);
    last = p;
  }
}

// ------------------------------------------------------------ BatteryModel

TEST_CASE("model: no samples means no battery") {
  BatteryModel m;
  CHECK_FALSE(m.status().present);
}

TEST_CASE("model: first sample gives the curve's percent at once") {
  BatteryModel m;
  m.add_sample(3950, kT0);
  const PowerStatus s = m.status();
  CHECK(s.present);
  CHECK(s.percent == 50);
  CHECK(s.millivolts == 3950);
  CHECK_FALSE(s.charging);
}

TEST_CASE("model: smoothing converges on a new level") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3950, 10);
  feed(m, t, 3870, 3);  // median and smoothing: not there yet
  CHECK(m.status().millivolts > 3880);
  feed(m, t, 3870, 40);
  CHECK(m.status().millivolts == doctest::Approx(3870).epsilon(0.001));
  CHECK(m.status().percent == 30);
}

TEST_CASE("model: a single spike is ignored") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3950, 20);
  m.add_sample(3300, t++);  // a load spike or a bad reading
  CHECK(m.status().millivolts == 3950);
  CHECK(m.status().percent == 50);
  m.add_sample(4400, t++);
  feed(m, t, 3950, 1);
  CHECK(m.status().millivolts == 3950);
  CHECK(m.status().percent == 50);
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: the percent never rises while discharging") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3950, 30);
  REQUIRE(m.status().percent == 50);
  feed(m, t, 3940, 30);  // a little lower
  const int lower = m.status().percent;
  CHECK(lower < 50);
  feed(m, t, 3950, 30);  // the load eases and the voltage recovers a bit
  CHECK(m.status().percent == lower);
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: the percent never falls while charging and follows the voltage when the direction changes") {
  BatteryModel m;
  m.set_external_charging(true);
  int64_t t = kT0;
  feed(m, t, 3950, 30);
  REQUIRE(m.status().percent == 50);
  feed(m, t, 3980, 40);
  CHECK(m.status().percent == 60);
  feed(m, t, 3965, 40);  // a dip while charging: held
  CHECK(m.status().percent == 60);
  m.set_external_charging(false);
  feed(m, t, 3965, 2);  // discharging now: follows the voltage down
  CHECK(m.status().percent == 55);
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: absent below 2500 mV and above 4500 mV, present again afterwards") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3950, 10);
  REQUIRE(m.status().present);
  m.add_sample(30, t++);  // one bad reading: the median keeps the battery
  CHECK(m.status().present);
  feed(m, t, 30, 2);  // the pack is unplugged: the ADC reads near zero
  CHECK_FALSE(m.status().present);
  feed(m, t, 4700, 3);  // nonsense on the line
  CHECK_FALSE(m.status().present);
  feed(m, t, 3800, 3);  // a different, emptier pack: starts fresh
  CHECK(m.status().present);
  CHECK(m.status().millivolts == 3800);
  CHECK(m.status().percent == lipo_percent(3800));
}

TEST_CASE("model: charging inferred from a rising trend") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3920, 60);
  CHECK_FALSE(m.status().charging);
  // The charger is plugged in: the voltage rises (the charge current across
  // the cell's resistance, then the slow climb).
  ramp(m, t, 3920, 3990, 60);
  CHECK(m.status().charging);
  // A slow climb from there keeps it charging well past the hold time.
  ramp(m, t, 3990, 4005, 600);
  CHECK(m.status().charging);
}

TEST_CASE("model: no charging inferred for a rising trend below 3900 mV") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3700, 60);
  ramp(m, t, 3700, 3760, 60);
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: a full pack on the charger counts as charging") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 4195, 90);
  CHECK(m.status().charging);
  CHECK(m.status().percent >= battery::kFullPercent);
  CHECK(battery_status_text(m.status()) == "100% \xC2\xB7 Fully charged");
}

TEST_CASE("model: steady discharge is never charging") {
  BatteryModel m;
  int64_t t = kT0;
  ramp(m, t, 4100, 3700, 3600);  // an hour of use
  CHECK_FALSE(m.status().charging);
  CHECK(m.status().percent == lipo_percent(m.status().millivolts) );
}

TEST_CASE("model: noisy flat data is never taken for charging") {
  // +-25 mV on every one-second sample (the firmware averages several ADC
  // readings per sample; this is worse than that), an hour each, 10 seeds.
  for (uint32_t seed = 1; seed <= 10; ++seed) {
    CAPTURE(seed);
    BatteryModel m;
    Noise n{seed * 7919u};
    int64_t t = kT0;
    int chargingSeconds = 0;
    int highest = 0;
    for (int i = 0; i < 3600; ++i) {
      m.add_sample(3950 + n.next(25), t++);
      if (m.status().charging) ++chargingSeconds;
      if (i > 60) highest = std::max(highest, m.status().percent);
    }
    CHECK(chargingSeconds == 0);
    CHECK(highest <= 52);  // discharging: the noise never lifts the percent
  }
}

TEST_CASE("model: charging is debounced, and ends at once when the voltage drops 30 mV") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3920, 60);
  ramp(m, t, 3920, 3990, 60);
  REQUIRE(m.status().charging);
  // The voltage stops rising: still charging inside the hold time.
  feed(m, t, 3990, 15);
  CHECK(m.status().charging);
  // Unplugged: the voltage falls by the charge current's rise.
  feed(m, t, 3940, 12);
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: a false start that stalls ends after the stall window") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3920, 60);
  ramp(m, t, 3920, 3945, 30);  // a recovery bump, not a charger
  feed(m, t, 3945, 30);
  REQUIRE(m.status().charging);  // looks like a charger at first
  feed(m, t, 3944, 300);
  CHECK(m.status().charging);  // within the stall window: kept
  feed(m, t, 3944, static_cast<int>(battery::kStallWindowSeconds));
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: the external charger signal overrides the inference") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3800, 30);
  m.set_external_charging(true);
  feed(m, t, 3800, 1);
  CHECK(m.status().charging);
  m.set_external_charging(false);
  feed(m, t, 4195, 120);  // would be inferred as a full pack on the charger
  CHECK_FALSE(m.status().charging);
  m.set_external_charging(std::nullopt);
  feed(m, t, 4195, 2);
  CHECK(m.status().charging);
}

TEST_CASE("model: gaps in time") {
  SUBCASE("a few missed seconds weigh the new sample more") {
    BatteryModel a;
    BatteryModel b;
    int64_t ta = kT0;
    int64_t tb = kT0;
    feed(a, ta, 3950, 10);
    feed(b, tb, 3950, 10);
    feed(a, ta, 3870, 3);
    feed(b, tb, 3870, 2);
    b.add_sample(3870, tb + 10);  // ten seconds later
    CHECK(b.status().millivolts < a.status().millivolts);
  }
  SUBCASE("a long gap starts again from the new reading") {
    BatteryModel m;
    int64_t t = kT0;
    feed(m, t, 3950, 30);
    t += 3600;  // asleep for an hour
    feed(m, t, 3870, 1);
    CHECK(m.status().millivolts == 3870);
    CHECK(m.status().percent == 30);
  }
  SUBCASE("the clock jumping back does not break anything") {
    BatteryModel m;
    int64_t t = kT0;
    feed(m, t, 3950, 30);
    int64_t early = 1000;  // the clock before NTP
    feed(m, early, 3950, 30);
    CHECK(m.status().present);
    CHECK(m.status().percent == 50);
    CHECK_FALSE(m.status().charging);
  }
}

// ------------------------------------------------------------ BatteryAlert

TEST_CASE("alert: nothing above 15 percent or while charging") {
  BatteryAlert a;
  CHECK_FALSE(a.update(on_battery(78), false, kT0));
  CHECK_FALSE(a.update(on_battery(16), false, kT0 + 1));
  CHECK_FALSE(a.update(charging(10), false, kT0 + 2));
  CHECK_FALSE(a.update(charging(3), false, kT0 + 3));
  CHECK_FALSE(a.update(PowerStatus{}, false, kT0 + 4));  // no battery
}

TEST_CASE("alert: low at 15 percent, once per discharge cycle") {
  BatteryAlert a;
  CHECK_FALSE(a.update(on_battery(16), false, kT0));
  const std::optional<Alert> low = a.update(on_battery(15), false, kT0 + 1);
  REQUIRE(low);
  CHECK(low->level == Alert::Level::Low);
  CHECK(std::string(low->text) == "Battery low. Plug in the charger.");
  CHECK(low->durationMs == 8000);
  for (int i = 0; i < 100; ++i) {
    CHECK_FALSE(a.update(on_battery(i % 2 == 0 ? 14 : 16), false, kT0 + 20 + i));
  }
}

TEST_CASE("alert: very low at 5 percent, once") {
  BatteryAlert a;
  REQUIRE(a.update(on_battery(15), false, kT0));
  CHECK_FALSE(a.update(on_battery(6), false, kT0 + 100));
  const std::optional<Alert> crit = a.update(on_battery(5), false, kT0 + 200);
  REQUIRE(crit);
  CHECK(crit->level == Alert::Level::Critical);
  CHECK(std::string(crit->text) == "Battery very low. Plug in the charger now.");
  CHECK(crit->durationMs == 10000);
  CHECK_FALSE(a.update(on_battery(4), false, kT0 + 300));
  CHECK_FALSE(a.update(on_battery(1), false, kT0 + 400));
}

TEST_CASE("alert: re-armed by charging or by rising above 25 percent") {
  SUBCASE("charging") {
    BatteryAlert a;
    REQUIRE(a.update(on_battery(15), false, kT0));
    CHECK_FALSE(a.update(charging(15), false, kT0 + 100));
    CHECK(a.update(on_battery(15), false, kT0 + 200));  // unplugged again, still low
  }
  SUBCASE("above 25 percent") {
    BatteryAlert a;
    REQUIRE(a.update(on_battery(15), false, kT0));
    CHECK_FALSE(a.update(on_battery(25), false, kT0 + 100));  // not above 25: still spent
    CHECK_FALSE(a.update(on_battery(15), false, kT0 + 200));
    CHECK_FALSE(a.update(on_battery(26), false, kT0 + 300));
    CHECK(a.update(on_battery(15), false, kT0 + 400));
  }
}

TEST_CASE("alert: at boot when already low; the very low one alone when already that low") {
  SUBCASE("low") {
    BatteryAlert a;
    const std::optional<Alert> first = a.update(on_battery(12), false, kT0);
    REQUIRE(first);
    CHECK(first->level == Alert::Level::Low);
  }
  SUBCASE("very low") {
    BatteryAlert a;
    const std::optional<Alert> first = a.update(on_battery(3), false, kT0);
    REQUIRE(first);
    CHECK(first->level == Alert::Level::Critical);
    CHECK_FALSE(a.update(on_battery(3), false, kT0 + 60));
    CHECK_FALSE(a.update(on_battery(10), false, kT0 + 120));
  }
}

TEST_CASE("alert: waits while suppressed and comes afterwards if still due") {
  BatteryAlert a;
  CHECK_FALSE(a.update(on_battery(14), true, kT0));
  CHECK_FALSE(a.update(on_battery(14), true, kT0 + 5));
  const std::optional<Alert> late = a.update(on_battery(14), false, kT0 + 6);
  REQUIRE(late);
  CHECK(late->level == Alert::Level::Low);

  BatteryAlert b;
  CHECK_FALSE(b.update(on_battery(14), true, kT0));
  CHECK_FALSE(b.update(charging(14), false, kT0 + 6));  // plugged in meanwhile: no longer due
}

TEST_CASE("alert: the next alert waits until the shown one's time is over") {
  BatteryAlert a;
  REQUIRE(a.update(on_battery(15), false, kT0));
  // A sudden drop to 5 percent while the low banner shows (8 s).
  CHECK_FALSE(a.update(on_battery(5), false, kT0 + 3));
  CHECK_FALSE(a.update(on_battery(5), false, kT0 + 7));
  const std::optional<Alert> crit = a.update(on_battery(5), false, kT0 + 8);
  REQUIRE(crit);
  CHECK(crit->level == Alert::Level::Critical);
}

// ------------------------------------------------------------ levels and texts

TEST_CASE("battery_segments: one per quarter, at least one") {
  CHECK(battery_segments(0) == 1);
  CHECK(battery_segments(1) == 1);
  CHECK(battery_segments(25) == 1);
  CHECK(battery_segments(26) == 2);
  CHECK(battery_segments(50) == 2);
  CHECK(battery_segments(51) == 3);
  CHECK(battery_segments(75) == 3);
  CHECK(battery_segments(76) == 4);
  CHECK(battery_segments(100) == 4);
  CHECK(battery_segments(150) == 4);
}

TEST_CASE("battery_level: normal, low, critical, charging") {
  CHECK(battery_level(on_battery(78)) == BatteryLevel::Normal);
  CHECK(battery_level(on_battery(21)) == BatteryLevel::Normal);
  CHECK(battery_level(on_battery(20)) == BatteryLevel::Low);
  CHECK(battery_level(on_battery(11)) == BatteryLevel::Low);
  CHECK(battery_level(on_battery(10)) == BatteryLevel::Critical);
  CHECK(battery_level(on_battery(0)) == BatteryLevel::Critical);
  CHECK(battery_level(charging(8)) == BatteryLevel::Charging);
  CHECK(battery_level(charging(100)) == BatteryLevel::Charging);
}

TEST_CASE("battery_shows_percent: only when low") {
  CHECK_FALSE(battery_shows_percent(on_battery(78)));
  CHECK_FALSE(battery_shows_percent(on_battery(21)));
  CHECK(battery_shows_percent(on_battery(20)));
  CHECK(battery_shows_percent(on_battery(8)));
  CHECK(battery_shows_percent(charging(8)));
  CHECK_FALSE(battery_shows_percent(charging(55)));
  CHECK_FALSE(battery_shows_percent(PowerStatus{}));
}

TEST_CASE("battery_status_text: every case") {
  CHECK(battery_status_text(PowerStatus{true, 78, false, 3920}) == "78% \xC2\xB7 On battery");
  CHECK(battery_status_text(PowerStatus{true, 55, true, 4010}) == "55% \xC2\xB7 Charging");
  CHECK(battery_status_text(PowerStatus{true, 99, true, 4190}) == "99% \xC2\xB7 Fully charged");
  CHECK(battery_status_text(PowerStatus{true, 100, true, 4200}) == "100% \xC2\xB7 Fully charged");
  CHECK(battery_status_text(PowerStatus{true, 18, false, 3770}) == "Low: 18% \xC2\xB7 On battery");
  CHECK(battery_status_text(PowerStatus{true, 20, false, 3790}) == "Low: 20% \xC2\xB7 On battery");
  CHECK(battery_status_text(PowerStatus{true, 21, false, 3792}) == "21% \xC2\xB7 On battery");
  CHECK(battery_status_text(PowerStatus{true, 12, true, 3900}) == "12% \xC2\xB7 Charging");
  CHECK(battery_status_text(PowerStatus{}) == "No battery detected");
}

TEST_CASE("battery_voltage_text: volts with two decimals") {
  CHECK(battery_voltage_text(PowerStatus{true, 60, false, 3920}) == "3.92 V");
  CHECK(battery_voltage_text(PowerStatus{true, 60, false, 3925}) == "3.93 V");
  CHECK(battery_voltage_text(PowerStatus{true, 100, true, 4200}) == "4.20 V");
  CHECK(battery_voltage_text(PowerStatus{true, 0, false, 3004}) == "3.00 V");
  CHECK(battery_voltage_text(PowerStatus{}) == "");
}
