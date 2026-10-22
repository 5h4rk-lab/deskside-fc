// PowerKeyPolicy (debounce, short press, hold hint, long press, boot window)
// and PowerController (screen toggle, the switch-off order, the off state).
#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "power_policy.h"

namespace {

using namespace power_key;

struct Ev {
  PowerKeyEvent e;
  int64_t t;
};

// A key that is down during the given [from, to) intervals.
struct Pattern {
  std::vector<std::pair<int64_t, int64_t>> presses;
  bool down(int64_t t) const {
    for (const auto& p : presses) {
      if (t >= p.first && t < p.second) return true;
    }
    return false;
  }
};

// Samples the key every `step` ms from `from` to `to` (inclusive).
std::vector<Ev> run(PowerKeyPolicy& k, const Pattern& p, int64_t from, int64_t to, int64_t step = 10) {
  std::vector<Ev> out;
  for (int64_t t = from; t <= to; t += step) {
    const PowerKeyEvent e = k.update(p.down(t), t);
    if (e != PowerKeyEvent::None) out.push_back({e, t});
  }
  return out;
}

std::vector<PowerKeyEvent> kinds(const std::vector<Ev>& evs) {
  std::vector<PowerKeyEvent> out;
  for (const Ev& e : evs) out.push_back(e.e);
  return out;
}

constexpr int64_t kBoot = 1000;
constexpr int64_t kStart = kBoot + 5000;  // well after the boot window

}  // namespace

// ------------------------------------------------------------ PowerKeyPolicy

TEST_CASE("power key: the documented values") {
  CHECK(kDebounceMs == 30);
  CHECK(kShortPressMaxMs == 600);
  CHECK(kHintAfterMs == 1000);
  CHECK(kLongPressMs == 3000);
  CHECK(kBootIgnoreMs == 2000);
  CHECK(kOffPollMs == 10000);
  CHECK(kSoundIdleWaitMs == 300);
  CHECK(kPoweringOffShowMs == 1500);
  CHECK(std::string(kHoldHintText) == "Hold to power off");
  CHECK(std::string(kPoweringOffText) == "Powering off");
}

TEST_CASE("power key: nothing happens while the key stays up") {
  PowerKeyPolicy k(kBoot);
  CHECK(run(k, Pattern{}, kBoot, kBoot + 60000).empty());
  CHECK_FALSE(k.pressed());
  CHECK_FALSE(k.hint_visible());
}

TEST_CASE("power key: every press longer than 30 ms and shorter than 600 ms is one short press, at the debounced release") {
  for (int64_t d = 40; d < kShortPressMaxMs; d += 10) {
    CAPTURE(d);
    PowerKeyPolicy k(kBoot);
    const std::vector<Ev> evs = run(k, Pattern{{{kStart, kStart + d}}}, kBoot, kStart + 5000);
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].e == PowerKeyEvent::ShortPress);
    CHECK(evs[0].t == kStart + d + kDebounceMs);
    CHECK_FALSE(k.hint_visible());
  }
}

TEST_CASE("power key: 1 ms sampling puts the hint at 1 s and the long press at 3 s exactly") {
  for (int64_t d : {1000, 1001, 3000, 3001}) {
    CAPTURE(d);
    PowerKeyPolicy k(kBoot);
    const std::vector<PowerKeyEvent> got = kinds(run(k, Pattern{{{kStart, kStart + d}}}, kBoot, kStart + 5000, 1));
    std::vector<PowerKeyEvent> want;
    if (d > 1000) want = {PowerKeyEvent::LongPressArmed};
    if (d > 3000) want.push_back(PowerKeyEvent::LongPress);
    CHECK(got == want);
  }
}

TEST_CASE("power key: 1 ms sampling puts the short press limit exactly at 600 ms") {
  for (int64_t d : {599, 600, 601}) {
    CAPTURE(d);
    PowerKeyPolicy k(kBoot);
    const std::vector<Ev> evs = run(k, Pattern{{{kStart, kStart + d}}}, kBoot, kStart + 2000, 1);
    if (d < 600) {
      REQUIRE(evs.size() == 1);
      CHECK(evs[0].e == PowerKeyEvent::ShortPress);
    } else {
      CHECK(evs.empty());
    }
  }
}

TEST_CASE("power key: a press from 600 ms up to 1 s does nothing at all") {
  for (int64_t d = kShortPressMaxMs; d <= kHintAfterMs; d += 10) {
    CAPTURE(d);
    PowerKeyPolicy k(kBoot);
    CHECK(run(k, Pattern{{{kStart, kStart + d}}}, kBoot, kStart + 5000).empty());
  }
}

TEST_CASE("power key: held 1 to 3 s shows the hint from 1 s until the release, and nothing else") {
  for (int64_t d = kHintAfterMs + 10; d <= kLongPressMs; d += 10) {
    CAPTURE(d);
    PowerKeyPolicy k(kBoot);
    const Pattern p{{{kStart, kStart + d}}};
    std::vector<Ev> evs;
    for (int64_t t = kBoot; t <= kStart + 5000; t += 10) {
      const PowerKeyEvent e = k.update(p.down(t), t);
      if (e != PowerKeyEvent::None) evs.push_back({e, t});
      const bool hint = t >= kStart + kHintAfterMs && t < kStart + d + kDebounceMs;
      if (k.hint_visible() != hint) {
        FAIL("hint " << k.hint_visible() << " at t-start " << t - kStart);
        break;
      }
    }
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].e == PowerKeyEvent::LongPressArmed);
    CHECK(evs[0].t == kStart + kHintAfterMs);
  }
}

TEST_CASE("power key: held 3 s or more gives the hint at 1 s and one long press at 3 s, whatever follows") {
  for (int64_t d : {3001, 3010, 3500, 5000, 10000, 60000}) {
    CAPTURE(d);
    PowerKeyPolicy k(kBoot);
    const std::vector<Ev> evs = run(k, Pattern{{{kStart, kStart + d}}}, kBoot, kStart + d + 5000);
    REQUIRE(evs.size() == 2);
    CHECK(evs[0].e == PowerKeyEvent::LongPressArmed);
    CHECK(evs[0].t == kStart + kHintAfterMs);
    CHECK(evs[1].e == PowerKeyEvent::LongPress);
    CHECK(evs[1].t == kStart + kLongPressMs);
    CHECK_FALSE(k.hint_visible());
  }
}

TEST_CASE("power key: exhaustive press lengths 0 to 4 s give exactly the expected events") {
  for (int64_t d = 0; d <= 4000; d += 10) {
    CAPTURE(d);
    PowerKeyPolicy k(kBoot);
    const std::vector<PowerKeyEvent> got = kinds(run(k, Pattern{{{kStart, kStart + d}}}, kBoot, kStart + 8000));
    std::vector<PowerKeyEvent> want;
    if (d > kDebounceMs && d < kShortPressMaxMs) want = {PowerKeyEvent::ShortPress};
    if (d > kHintAfterMs) want = {PowerKeyEvent::LongPressArmed};
    if (d > kLongPressMs) want = {PowerKeyEvent::LongPressArmed, PowerKeyEvent::LongPress};
    CHECK(got == want);
  }
}

TEST_CASE("power key: bounces shorter than 30 ms are ignored, a bouncing press counts once") {
  for (int64_t pulse : {1, 5, 10, 20, 29, 30}) {
    CAPTURE(pulse);
    PowerKeyPolicy k(kBoot);
    CHECK(run(k, Pattern{{{kStart, kStart + pulse}}}, kBoot, kStart + 3000, 1).empty());
    CHECK_FALSE(k.pressed());
  }
  // Contact chatter at both ends of a 200 ms press.
  PowerKeyPolicy k(kBoot);
  const Pattern p{{{kStart, kStart + 3},
                   {kStart + 8, kStart + 12},
                   {kStart + 15, kStart + 200},
                   {kStart + 205, kStart + 209},
                   {kStart + 214, kStart + 216}}};
  const std::vector<Ev> evs = run(k, p, kBoot, kStart + 2000, 1);
  REQUIRE(evs.size() == 1);
  CHECK(evs[0].e == PowerKeyEvent::ShortPress);
}

TEST_CASE("power key: two short presses are two events") {
  PowerKeyPolicy k(kBoot);
  const std::vector<PowerKeyEvent> got =
      kinds(run(k, Pattern{{{kStart, kStart + 150}, {kStart + 400, kStart + 550}}}, kBoot, kStart + 3000));
  CHECK(got == std::vector<PowerKeyEvent>{PowerKeyEvent::ShortPress, PowerKeyEvent::ShortPress});
}

TEST_CASE("power key: presses that begin in the first 2 s after boot are ignored, whatever their length") {
  for (int64_t at = 0; at < kBootIgnoreMs; at += 10) {
    for (int64_t d : {100, 1500, 4000}) {
      CAPTURE(at);
      CAPTURE(d);
      PowerKeyPolicy k(kBoot);
      CHECK(run(k, Pattern{{{kBoot + at, kBoot + at + d}}}, kBoot, kBoot + at + d + 3000).empty());
      CHECK_FALSE(k.hint_visible());
    }
  }
  // From 2 s on they count.
  PowerKeyPolicy k(kBoot);
  const std::vector<Ev> evs = run(k, Pattern{{{kBoot + kBootIgnoreMs, kBoot + kBootIgnoreMs + 100}}}, kBoot, kBoot + 5000);
  REQUIRE(evs.size() == 1);
  CHECK(evs[0].e == PowerKeyEvent::ShortPress);
}

TEST_CASE("power key: the key that switched the board on (down at the first sample) is ignored until released") {
  // Still held long after the boot window: no long press from it.
  PowerKeyPolicy k(kBoot);
  CHECK(run(k, Pattern{{{0, kBoot + 9000}}}, kBoot + 3000, kBoot + 12000).empty());
  // The next press counts.
  const std::vector<Ev> evs = run(k, Pattern{{{kBoot + 13000, kBoot + 13200}}}, kBoot + 12010, kBoot + 15000);
  REQUIRE(evs.size() == 1);
  CHECK(evs[0].e == PowerKeyEvent::ShortPress);
}

TEST_CASE("power key: irregular sampling still gives the same events") {
  for (int64_t step : {1, 3, 7, 16, 25}) {
    CAPTURE(step);
    PowerKeyPolicy a(kBoot);
    CHECK(kinds(run(a, Pattern{{{kStart, kStart + 300}}}, kBoot, kStart + 3000, step)) ==
          std::vector<PowerKeyEvent>{PowerKeyEvent::ShortPress});
    PowerKeyPolicy b(kBoot);
    CHECK(kinds(run(b, Pattern{{{kStart, kStart + 3500}}}, kBoot, kStart + 6000, step)) ==
          std::vector<PowerKeyEvent>{PowerKeyEvent::LongPressArmed, PowerKeyEvent::LongPress});
  }
}

TEST_CASE("power key: require_release ends the press in progress") {
  PowerKeyPolicy k(kBoot);
  const Pattern p{{{kStart, kStart + 5000}, {kStart + 6000, kStart + 6100}}};
  std::vector<Ev> evs = run(k, p, kBoot, kStart + 1500);
  REQUIRE(evs.size() == 1);
  CHECK(k.hint_visible());
  k.require_release();
  CHECK_FALSE(k.hint_visible());
  evs = run(k, p, kStart + 1510, kStart + 7000);
  REQUIRE(evs.size() == 1);
  CHECK(evs[0].e == PowerKeyEvent::ShortPress);
  CHECK(evs[0].t > kStart + 6000);
}

TEST_CASE("power key: settling while a change waits for its debounce") {
  PowerKeyPolicy k(kBoot);
  k.update(false, kStart - 10);
  CHECK_FALSE(k.settling());
  k.update(true, kStart);
  CHECK(k.settling());
  CHECK_FALSE(k.pressed());
  k.update(true, kStart + 30);
  CHECK_FALSE(k.settling());
  CHECK(k.pressed());
}

// ------------------------------------------------------------ PowerController

namespace {

struct FakeHost : PowerHost {
  std::vector<std::string> calls;
  std::vector<int64_t> times;
  int64_t now = 0;
  void log(const std::string& s) {
    calls.push_back(s);
    times.push_back(now);
  }
  bool dark = false;  // the screen, as the host would have it
  void toggle_screen() override {
    log("toggle_screen");
    dark = !dark;
  }
  bool screen_on() override {
    log("screen_on");
    const bool was = dark;
    dark = false;
    return was;
  }
  void screen_off() override {
    log("screen_off");
    dark = true;
  }
  void show_hold_hint(bool show) override { log(show ? "hint_on" : "hint_off"); }
  void stop_sound() override { log("stop_sound"); }
  void wait_sound_idle(uint32_t ms) override { log("wait_sound_idle(" + std::to_string(ms) + ")"); }
  void show_powering_off() override { log("show_powering_off"); }
  void save_prefs() override { log("save_prefs"); }
  void stop_wifi() override { log("stop_wifi"); }
  void cut_power() override { log("cut_power"); }
  void enter_off_state() override { log("enter_off_state"); }
  void wake_from_off() override { log("wake_from_off"); }
  int count(const std::string& s) const {
    int n = 0;
    for (const std::string& c : calls) n += c == s ? 1 : 0;
    return n;
  }
  int64_t time_of(const std::string& s) const {
    for (size_t i = 0; i < calls.size(); ++i) {
      if (calls[i] == s) return times[i];
    }
    return -1;
  }
};

// Drives the controller the way the firmware does: a 10 ms tick that samples
// the key when poll_interval_ms() has passed or the level changed.
struct Rig {
  FakeHost host;
  PowerController c{host, kBoot};
  int64_t lastSample = -1000000;
  bool lastLevel = false;
  int samples = 0;

  void run(const Pattern& p, int64_t from, int64_t to) {
    for (int64_t t = from; t <= to; t += 10) {
      host.now = t;
      const bool level = p.down(t);
      if (t - lastSample >= c.poll_interval_ms() || level != lastLevel) {
        c.update(level, t);
        lastSample = t;
        ++samples;
      }
      lastLevel = level;
    }
  }
};

const std::vector<std::string> kOffOrder = {"stop_sound",  "wait_sound_idle(300)", "show_powering_off", "save_prefs",
                                            "stop_wifi",   "cut_power",            "enter_off_state"};

}  // namespace

TEST_CASE("controller: a short press toggles the screen, each time") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 200}, {kStart + 1000, kStart + 1200}}}, kBoot, kStart + 3000);
  CHECK(r.host.calls == std::vector<std::string>{"toggle_screen", "toggle_screen"});
  CHECK(r.c.mode() == PowerController::Mode::On);
}

TEST_CASE("controller: a 2 s hold lights the screen, shows the hint and hides it on release") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 2000}}}, kBoot, kStart + 4000);
  CHECK(r.host.calls == std::vector<std::string>{"screen_on", "hint_on", "hint_off"});
  CHECK(r.host.time_of("hint_on") == kStart + 1000);
  CHECK(r.host.time_of("hint_off") == kStart + 2000 + kDebounceMs);
  CHECK(r.c.mode() == PowerController::Mode::On);
}

TEST_CASE("controller: a 3 s hold switches off in the exact order, the power cut 1.5 s after the screen") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 3200}}}, kBoot, kStart + 8000);
  std::vector<std::string> want = {"screen_on", "hint_on", "hint_off"};
  want.insert(want.end(), kOffOrder.begin(), kOffOrder.end());
  CHECK(r.host.calls == want);
  CHECK(r.host.time_of("stop_sound") == kStart + 3000);
  const int64_t shown = r.host.time_of("show_powering_off");
  CHECK(shown == kStart + 3000);
  const int64_t saved = r.host.time_of("save_prefs");
  CHECK(saved - shown >= kPoweringOffShowMs);
  CHECK(saved - shown <= kPoweringOffShowMs + 20);
  CHECK(r.host.time_of("cut_power") == saved);
  CHECK(r.c.mode() == PowerController::Mode::Off);
}

TEST_CASE("controller: nothing after the screen until its 1.5 s have passed") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 3100}}}, kBoot, kStart + 3000);
  REQUIRE(r.host.count("show_powering_off") == 1);
  const int64_t shown = r.host.time_of("show_powering_off");
  r.run(Pattern{{{kStart, kStart + 3100}}}, kStart + 3010, shown + kPoweringOffShowMs - 10);
  CHECK(r.host.count("save_prefs") == 0);
  CHECK(r.host.count("cut_power") == 0);
  CHECK(r.c.mode() == PowerController::Mode::PoweringOff);
  r.run(Pattern{}, shown + kPoweringOffShowMs, shown + kPoweringOffShowMs + 20);
  CHECK(r.host.count("cut_power") == 1);
}

TEST_CASE("controller: presses while switching off are ignored") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 3050}, {kStart + 3300, kStart + 3400}, {kStart + 3600, kStart + 3700}}}, kBoot,
        kStart + 4000);
  CHECK(r.host.count("toggle_screen") == 0);
  CHECK(r.host.count("wake_from_off") == 0);
}

TEST_CASE("controller: power_off() runs the same order without the hint; repeats are ignored") {
  Rig r;
  r.host.now = kStart;
  r.c.power_off();
  r.c.power_off();
  r.run(Pattern{}, kStart, kStart + 3000);
  CHECK(r.host.calls == kOffOrder);
  r.c.power_off();  // in the off state
  CHECK(r.host.calls == kOffOrder);
}

TEST_CASE("controller: in the off state a press wakes the board, once") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 3100}}}, kBoot, kStart + 6000);
  REQUIRE(r.c.mode() == PowerController::Mode::Off);
  r.run(Pattern{{{kStart + 20000, kStart + 20200}}}, kStart + 6010, kStart + 25000);
  CHECK(r.host.count("wake_from_off") == 1);
  CHECK(r.host.time_of("wake_from_off") == kStart + 20000 + kDebounceMs);
}

TEST_CASE("controller: the key still held from the long press must be released before a press wakes") {
  Rig r;
  // Held for 30 s through the whole switch-off: no wake from it.
  r.run(Pattern{{{kStart, kStart + 30000}}}, kBoot, kStart + 40000);
  REQUIRE(r.c.mode() == PowerController::Mode::Off);
  CHECK(r.host.count("wake_from_off") == 0);
  r.run(Pattern{{{kStart + 50000, kStart + 50100}}}, kStart + 40010, kStart + 52000);
  CHECK(r.host.count("wake_from_off") == 1);
}

TEST_CASE("controller: bounces do not wake the off state") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 3100}}}, kBoot, kStart + 6000);
  r.run(Pattern{{{kStart + 7000, kStart + 7010}, {kStart + 9000, kStart + 9020}}}, kStart + 6010, kStart + 12000);
  CHECK(r.host.count("wake_from_off") == 0);
}

TEST_CASE("controller: polls every 10 ms when on, every 10 s when off with the key up, and samples far less") {
  Rig r;
  CHECK(r.c.poll_interval_ms() == kPollMs);
  r.run(Pattern{{{kStart, kStart + 3100}}}, kBoot, kStart + 6000);
  REQUIRE(r.c.mode() == PowerController::Mode::Off);
  CHECK(r.c.poll_interval_ms() == kOffPollMs);
  const int before = r.samples;
  r.run(Pattern{}, kStart + 6010, kStart + 66000);  // one minute off
  CHECK(r.samples - before <= 7);
  // A press is still seen at once (the host samples on a level change).
  r.run(Pattern{{{kStart + 70000, kStart + 70150}}}, kStart + 66010, kStart + 71000);
  CHECK(r.host.count("wake_from_off") == 1);
}

TEST_CASE("controller: the boot window and the hold-at-boot rule apply to the controller too") {
  Rig r;
  r.run(Pattern{{{0, kBoot + 8000}}}, kBoot, kBoot + 12000);
  CHECK(r.host.calls.empty());
  CHECK(r.c.mode() == PowerController::Mode::On);
}

TEST_CASE("controller: a hold that lit a dark screen leaves it dark again when it ends before 3 s") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 200}, {kStart + 1000, kStart + 2500}}}, kBoot, kStart + 4000);
  CHECK(r.host.calls ==
        std::vector<std::string>{"toggle_screen", "screen_on", "hint_on", "hint_off", "screen_off"});
  CHECK(r.host.dark);
}

TEST_CASE("controller: a hold on a lit screen leaves it lit; a long press from a dark screen does not darken it") {
  Rig r;
  r.run(Pattern{{{kStart, kStart + 2000}}}, kBoot, kStart + 3000);
  CHECK(r.host.count("screen_off") == 0);
  CHECK_FALSE(r.host.dark);
  Rig d;
  d.run(Pattern{{{kStart, kStart + 200}, {kStart + 1000, kStart + 4500}}}, kBoot, kStart + 7000);
  CHECK(d.host.count("screen_off") == 0);
  CHECK(d.host.count("show_powering_off") == 1);
}
