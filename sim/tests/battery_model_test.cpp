// Battery maths: the LiPo curve, the smoothing model, charging inference, the
// low-battery alert state machine and the texts.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
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

TEST_CASE("model: no charging inferred for a rising trend below 3500 mV") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3400, 60);
  ramp(m, t, 3400, 3460, 60);
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: a flat full pack is Full, never charging") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 4195, 600);
  CHECK_FALSE(m.status().charging);
  CHECK(m.status().percent == 100);
  CHECK(battery_status_text(m.status()) == "100% \xC2\xB7 Full");
}

TEST_CASE("model: steady discharge is never charging") {
  BatteryModel m;
  int64_t t = kT0;
  ramp(m, t, 4100, 3700, 3600);  // an hour of use
  CHECK_FALSE(m.status().charging);
  CHECK(m.status().percent == lipo_percent(m.status().millivolts));
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

TEST_CASE("model: charging ends at once when the voltage drops 30 mV") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3920, 60);
  ramp(m, t, 3920, 3990, 60);
  ramp(m, t, 3990, 3993, 30);  // the slow climb of a charge
  REQUIRE(m.status().charging);
  feed(m, t, 3993, 15);  // stopped rising: still charging inside the stall time
  CHECK(m.status().charging);
  feed(m, t, 3943, 12);  // unplugged: the charge current's rise goes
  CHECK_FALSE(m.status().charging);
}

TEST_CASE("model: a false start that stalls clears within the stall time, and the percent never jumps") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3920, 60);
  const int before = m.status().percent;
  ramp(m, t, 3920, 3945, 30);  // a load easing off, not a charger
  int started = -1;
  int cleared = -1;
  int highest = 0;
  for (int i = 0; i < 400; ++i) {
    m.add_sample(3945, t++);
    if (m.status().charging && started < 0) started = i;
    if (!m.status().charging && started >= 0 && cleared < 0) cleared = i;
    highest = std::max(highest, m.status().percent);
  }
  REQUIRE(started >= 0);  // it looks like a charger at first
  REQUIRE(cleared >= 0);
  CHECK(cleared - started <= battery::kStallSeconds);
  CHECK(highest <= before + 2);
}

TEST_CASE("model: the external charger signal overrides the inference") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3800, 30);
  m.set_external_charging(true);
  feed(m, t, 3800, 1);
  CHECK(m.status().charging);
  m.set_external_charging(false);
  feed(m, t, 3800, 60);
  ramp(m, t, 3800, 3900, 60);  // would be inferred as a charger
  CHECK_FALSE(m.status().charging);
  m.set_external_charging(true);
  feed(m, t, 4195, 2);  // flat and full: inferred Full, but the pin says charging
  CHECK(m.status().charging);
  m.set_external_charging(std::nullopt);
  feed(m, t, 4195, 60);
  CHECK(m.status().charging);  // the climb to 4195 mV looked like a charger too
  feed(m, t, 4195, static_cast<int>(battery::kStallSeconds) + 90);
  CHECK_FALSE(m.status().charging);  // flat: stalled, Full
  CHECK(battery_status_text(m.status()) == "100% \xC2\xB7 Full");
}

// ------------------------------------------------------------ timestamps

TEST_CASE("model: a few missed seconds weigh the new sample more") {
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

TEST_CASE("model: a long gap starts again from the new reading") {
  BatteryModel m;
  int64_t t = kT0;
  feed(m, t, 3950, 30);
  t += 3600;  // asleep for an hour
  feed(m, t, 3870, 1);
  CHECK(m.status().millivolts == 3870);
  CHECK(m.status().percent == 30);
}

TEST_CASE("model: a large jump back starts again; a small one is ignored") {
  SUBCASE("large: the clock before NTP") {
    BatteryModel m;
    int64_t t = kT0;
    feed(m, t, 3950, 30);
    m.add_sample(3870, 1000);  // a fresh start would read 3870 at once
    CHECK(m.status().millivolts == 3870);
    CHECK(m.status().percent == 30);
  }
  SUBCASE("small: kept, the sample is dropped") {
    BatteryModel m;
    int64_t t = kT0;
    feed(m, t, 3950, 30);
    const PowerStatus before = m.status();
    m.add_sample(3870, t - 4);
    CHECK(m.status() == before);
    m.add_sample(3870, t - 1);  // the same second as the last sample
    CHECK(m.status() == before);
  }
}

TEST_CASE("model: duplicate and jittery timestamps keep the state") {
  BatteryModel a;  // clean 1 Hz
  BatteryModel b;  // timestamps 0, 1, 1, 2, 3, ...: one repeated
  int64_t ta = kT0;
  feed(a, ta, 3950, 30);
  for (int i = 0; i < 30; ++i) {
    b.add_sample(3950, kT0 + i);
    if (i == 10) b.add_sample(3950, kT0 + i);
  }
  CHECK(a.status() == b.status());
}

TEST_CASE("model: sampling at 5 Hz behaves like 1 Hz") {
  BatteryModel a;
  BatteryModel b;
  for (int i = 0; i < 400; ++i) {
    const int mv = i < 100 ? 3900 : 3960 + (i - 100) / 20;  // a charger plugged in at 100 s
    a.add_sample(mv, kT0 + i);
    for (int k = 0; k < 5; ++k) b.add_sample(mv, kT0 + i);
    REQUIRE(a.status() == b.status());
  }
  CHECK(a.status().charging);
}

// ------------------------------------------------------------ scenarios

namespace {

// A deterministic 1 Hz trace with +-5 mV noise. `mv(i)` is the true voltage at
// second i, `ts(i)` the timestamp (default: one a second).
struct Trace {
  std::vector<PowerStatus> s;
  int first_charging(int from = 0) const {
    for (int i = from; i < static_cast<int>(s.size()); ++i) if (s[i].charging) return i;
    return -1;
  }
  int last_charging() const {
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) if (s[i].charging) return i;
    return -1;
  }
  int longest_charging_run() const {
    int best = 0;
    int run = 0;
    for (const PowerStatus& p : s) {
      run = p.charging ? run + 1 : 0;
      best = std::max(best, run);
    }
    return best;
  }
  int charging_seconds(int from = 0) const {
    int n = 0;
    for (int i = from; i < static_cast<int>(s.size()); ++i) n += s[i].charging ? 1 : 0;
    return n;
  }
  // Largest rise of the percent from one second to the next.
  int largest_step_up(int from = 0) const {
    int best = 0;
    for (int i = std::max(from, 1); i < static_cast<int>(s.size()); ++i) best = std::max(best, s[i].percent - s[i - 1].percent);
    return best;
  }
  int largest_step_down(int from = 0, int to = -1) const {
    if (to < 0) to = static_cast<int>(s.size());
    int best = 0;
    for (int i = std::max(from, 1); i < to; ++i) best = std::max(best, s[i - 1].percent - s[i].percent);
    return best;
  }
};

Trace simulate(int seconds, const std::function<int(int)>& mv, uint32_t seed = 42,
               const std::function<int64_t(int)>& ts = nullptr, int noise = 5) {
  BatteryModel m;
  Noise n{seed};
  Trace tr;
  for (int i = 0; i < seconds; ++i) {
    m.add_sample(mv(i) + n.next(noise), ts ? ts(i) : kT0 + i);
    tr.s.push_back(m.status());
  }
  return tr;
}

// Constant-current charge from `from` mV: plugged at `plug` s (+60 mV), then
// +3 mV a minute up to 4200 mV, where the charger holds it.
int cc_charge(int i, int from, int plug) {
  if (i < plug) return from;
  return std::min(4200, from + 60 + (i - plug) * 3 / 60);
}

}  // namespace

TEST_CASE("scenario a: constant-current charge from 3.6 V") {
  const int plug = 300;
  const int full = plug + (4200 - 3660) * 20;  // reaches 4200 mV at 3 mV a minute
  const Trace tr = simulate(full + 900, [&](int i) { return cc_charge(i, 3600, plug); });
  const int on = tr.first_charging();
  REQUIRE(on >= plug);
  CHECK(on - plug <= 120);
  // Charging all the way up, then the charger holds 4.2 V flat: stalled, Full.
  int off = -1;
  for (int i = on; i < static_cast<int>(tr.s.size()); ++i) {
    if (!tr.s[i].charging) {
      off = i;
      break;
    }
  }
  REQUIRE(off > 0);
  CHECK(off >= full - 120);
  CHECK(tr.charging_seconds(off) == 0);
  CHECK(tr.largest_step_down(plug, off) == 0);  // never down while charging
  CHECK(tr.s.back().percent == 100);
  CHECK(battery_status_text(tr.s.back()) == "100% \xC2\xB7 Full");
}

TEST_CASE("scenario b: the same charge with a duplicate timestamp and a 1 s jitter") {
  const int plug = 300;
  const int seconds = plug + 3600;
  // One sample repeats the previous second at 1500 s; around 2500 s one
  // reading comes a second late and the next one on time (0, 1, 1, 2, 3).
  auto ts = [](int i) -> int64_t {
    if (i == 1500) return kT0 + 1499;
    if (i == 2500) return kT0 + 2499;
    return kT0 + i;
  };
  const Trace clean = simulate(seconds, [&](int i) { return cc_charge(i, 3600, plug); });
  const Trace jitter = simulate(seconds, [&](int i) { return cc_charge(i, 3600, plug); }, 42, ts);
  const int on = jitter.first_charging();
  REQUIRE(on > 0);
  CHECK(on - plug <= 120);
  CHECK(jitter.charging_seconds(on) == seconds - on);  // never drops out
  CHECK(jitter.largest_step_down(plug) == 0);
  CHECK(std::abs(jitter.s.back().percent - clean.s.back().percent) <= 1);
}

TEST_CASE("scenario c: unplugged during a charge") {
  for (uint32_t seed = 42; seed < 72; ++seed)
  for (int drop : {40, 80, 150}) {
    CAPTURE(seed);
    CAPTURE(drop);
    const int plug = 120;
    const int unplug = plug + 1200;
    const Trace tr = simulate(unplug + 1200, [&](int i) {
      if (i < unplug) return cc_charge(i, 3800, plug);
      return cc_charge(unplug, 3800, plug) - drop - (i - unplug) / 60;
    }, seed);
    REQUIRE(tr.s[unplug - 1].charging);
    const int off = [&] {
      for (int i = unplug; i < static_cast<int>(tr.s.size()); ++i) if (!tr.s[i].charging) return i;
      return -1;
    }();
    REQUIRE(off > 0);
    CHECK(off - unplug <= 20);
    CHECK(tr.charging_seconds(off) == 0);
    CHECK(tr.largest_step_up(unplug) == 0);  // falls from here, never rises
    CHECK(tr.s.back().percent < tr.s[unplug - 1].percent);
  }
}

TEST_CASE("scenario d: a load easing off while discharging is never more than a brief charge") {
  for (uint32_t seed = 42; seed < 52; ++seed)
  for (int at : {1200, 120}) {  // well into the run, and two minutes after boot
    for (int step : {12, 15, 20, 25, 40}) {
      CAPTURE(seed);
      CAPTURE(at);
      CAPTURE(step);
      const Trace tr = simulate(at + 1800, [&](int i) {
        const int base = 3960 - i * 7 / 600;  // -0.7 mV a minute
        return i < at ? base : base + step;
      }, seed);
      CHECK(tr.longest_charging_run() <= battery::kStallSeconds);
      CHECK(tr.largest_step_up() <= 2);
      int highest = 0;
      for (int i = at; i < static_cast<int>(tr.s.size()); ++i) highest = std::max(highest, tr.s[i].percent);
      CHECK(highest <= tr.s[at - 1].percent + 2);
    }
  }
}

TEST_CASE("scenario e: a full pack on USB, flat at 4.2 V") {
  const Trace tr = simulate(1800, [](int) { return 4200; });
  CHECK(tr.charging_seconds() == 0);
  CHECK(tr.s.back().percent == 100);
  CHECK(battery_status_text(tr.s.back()) == "100% \xC2\xB7 Full");
}

TEST_CASE("scenario f: a full pack unplugged is never charging") {
  for (int step : {5, 10, 15, 20, 35}) {
    CAPTURE(step);
    const Trace tr = simulate(1200 + 3600, [&](int i) { return i < 1200 ? 4200 : 4200 - step - (i - 1200) / 60; });
    CHECK(tr.charging_seconds() == 0);
    CHECK(tr.largest_step_up() == 0);
  }
}

TEST_CASE("scenario g: booting on battery at 4.19 V") {
  const Trace tr = simulate(3600, [](int i) { return 4190 - i / 60; });
  CHECK(tr.charging_seconds() == 0);
  CHECK(tr.largest_step_up() == 0);
}

TEST_CASE("scenario h: noisy flat data at 3.8 V") {
  for (uint32_t seed = 1; seed <= 10; ++seed) {
    CAPTURE(seed);
    const Trace tr = simulate(3600, [](int) { return 3800; }, seed * 104729u, nullptr, 25);
    CHECK(tr.charging_seconds() == 0);
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
  CHECK(battery_status_text(PowerStatus{true, 100, false, 4200}) == "100% \xC2\xB7 Full");
  CHECK(battery_status_text(PowerStatus{true, 98, false, 4180}) == "98% \xC2\xB7 Full");
  CHECK(battery_status_text(PowerStatus{true, 98, false, 4179}) == "98% \xC2\xB7 On battery");
  CHECK(battery_status_text(PowerStatus{}) == "No battery detected");
}

TEST_CASE("battery_voltage_text: volts with two decimals") {
  CHECK(battery_voltage_text(PowerStatus{true, 60, false, 3920}) == "3.92 V");
  CHECK(battery_voltage_text(PowerStatus{true, 60, false, 3925}) == "3.93 V");
  CHECK(battery_voltage_text(PowerStatus{true, 100, true, 4200}) == "4.20 V");
  CHECK(battery_voltage_text(PowerStatus{true, 0, false, 3004}) == "3.00 V");
  CHECK(battery_voltage_text(PowerStatus{}) == "");
}
