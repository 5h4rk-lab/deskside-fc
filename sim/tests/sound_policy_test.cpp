// When a sound may play: mode x event x night x mute-at-night, and the gate.
#include <doctest/doctest.h>

#include "settings.h"
#include "sound_policy.h"
#include "sound_synth.h"

namespace {

const SoundEvent kAll[] = {SoundEvent::Goal,       SoundEvent::WinFanfare,      SoundEvent::KickOff,
                           SoundEvent::FullTime,   SoundEvent::BatteryLow,      SoundEvent::BatteryCritical,
                           SoundEvent::WifiOk,     SoundEvent::Preview};

Settings with(SoundMode mode, bool muteAtNight) {
  Settings s;
  s.soundMode = mode;
  s.soundMuteAtNight = muteAtNight;
  return s;
}

bool alert_only(SoundEvent e) { return e == SoundEvent::KickOff || e == SoundEvent::FullTime; }

}  // namespace

TEST_CASE("sound policy: the defaults are Goals only, volume 40, mute at night") {
  const Settings s;
  CHECK(s.soundMode == SoundMode::GoalsOnly);
  CHECK(s.soundVolume == 40);
  CHECK(s.soundMuteAtNight);
}

TEST_CASE("sound policy: the full table of mode x event x night x mute-at-night") {
  for (SoundMode mode : {SoundMode::Off, SoundMode::GoalsOnly, SoundMode::GoalsAndAlerts}) {
    for (bool mute : {false, true}) {
      for (bool night : {false, true}) {
        for (SoundEvent e : kAll) {
          bool expected;
          if (e == SoundEvent::Preview) {
            expected = true;  // ignores mode and night mute
          } else if (mode == SoundMode::Off) {
            expected = false;
          } else if (mute && night) {
            expected = false;
          } else if (alert_only(e)) {
            expected = mode == SoundMode::GoalsAndAlerts;
          } else {
            expected = true;  // goals, fanfare, battery beeps, Wi-Fi chime
          }
          INFO("mode " << static_cast<int>(mode) << " mute " << mute << " night " << night << " event "
                       << static_cast<int>(e));
          CHECK(sound_should_play(with(mode, mute), e, night) == expected);
        }
      }
    }
  }
}

TEST_CASE("sound policy: mute at night off keeps the sound at night") {
  CHECK(sound_should_play(with(SoundMode::GoalsOnly, false), SoundEvent::Goal, true));
  CHECK_FALSE(sound_should_play(with(SoundMode::GoalsOnly, true), SoundEvent::Goal, true));
  CHECK(sound_should_play(with(SoundMode::GoalsOnly, true), SoundEvent::Goal, false));
}

TEST_CASE("sound policy: Preview plays when sound is off and at night") {
  CHECK(sound_should_play(with(SoundMode::Off, true), SoundEvent::Preview, true));
}

TEST_CASE("sound gate: the first event is allowed") {
  SoundGate g;
  for (SoundEvent e : kAll) {
    SoundGate fresh;
    CHECK(fresh.allow(e, 0));
  }
  CHECK(g.allow(SoundEvent::Goal, 123456));
}

TEST_CASE("sound gate: the same event is not repeated inside 10 s, and is after") {
  SoundGate g;
  CHECK(g.allow(SoundEvent::BatteryLow, 1000));
  CHECK_FALSE(g.allow(SoundEvent::BatteryLow, 1001));
  CHECK_FALSE(g.allow(SoundEvent::BatteryLow, 10999));
  CHECK(g.allow(SoundEvent::BatteryLow, 11001));  // 10.001 s later
  CHECK_FALSE(g.allow(SoundEvent::BatteryLow, 12000));
  // A refused attempt does not push the window.
  SoundGate h;
  CHECK(h.allow(SoundEvent::WifiOk, 0));
  CHECK_FALSE(h.allow(SoundEvent::WifiOk, 9000));
  CHECK(h.allow(SoundEvent::WifiOk, 10001));
}

TEST_CASE("sound gate: different events do not block each other's repeat window") {
  SoundGate g;
  CHECK(g.allow(SoundEvent::BatteryLow, 0));
  CHECK(g.allow(SoundEvent::WifiOk, 100));
  CHECK_FALSE(g.allow(SoundEvent::BatteryLow, 200));
}

TEST_CASE("sound gate: a new Goal starts whatever is playing") {
  const int64_t goalMs = sound_clip(SoundEvent::Goal).duration_ms();
  SoundGate g;
  CHECK(g.allow(SoundEvent::BatteryLow, 0));
  CHECK(g.allow(SoundEvent::Goal, 100));        // over a beep
  SoundGate h;
  CHECK(h.allow(SoundEvent::WinFanfare, 0));
  CHECK(h.allow(SoundEvent::Goal, 500));        // over a fanfare
  SoundGate i;
  CHECK(i.allow(SoundEvent::Goal, 0));
  CHECK(i.allow(SoundEvent::Goal, 10001));      // a second goal, after the repeat window
  CHECK(goalMs > 0);
}

TEST_CASE("sound gate: KickOff and FullTime never cut a Goal or a WinFanfare short") {
  const int64_t goalMs = sound_clip(SoundEvent::Goal).duration_ms();
  const int64_t fanfareMs = sound_clip(SoundEvent::WinFanfare).duration_ms();
  for (SoundEvent alert : {SoundEvent::KickOff, SoundEvent::FullTime}) {
    SoundGate g;
    CHECK(g.allow(SoundEvent::Goal, 1000));
    CHECK_FALSE(g.allow(alert, 1000 + 10));
    CHECK_FALSE(g.allow(alert, 1000 + goalMs - 1));
    CHECK(g.allow(alert, 1000 + goalMs));  // the horn is over
    SoundGate h;
    CHECK(h.allow(SoundEvent::WinFanfare, 0));
    CHECK_FALSE(h.allow(alert, fanfareMs - 1));
    CHECK(h.allow(alert, fanfareMs));
  }
}

TEST_CASE("sound gate: a refused alert is not recorded, so it can play once the horn is over") {
  const int64_t goalMs = sound_clip(SoundEvent::Goal).duration_ms();
  SoundGate g;
  CHECK(g.allow(SoundEvent::Goal, 0));
  CHECK_FALSE(g.allow(SoundEvent::FullTime, 100));
  CHECK(g.allow(SoundEvent::FullTime, goalMs + 1));
}

TEST_CASE("sound gate: beeps and the Wi-Fi chime wait out a goal too") {
  const int64_t goalMs = sound_clip(SoundEvent::Goal).duration_ms();
  for (SoundEvent e : {SoundEvent::BatteryLow, SoundEvent::BatteryCritical, SoundEvent::WifiOk}) {
    SoundGate g;
    CHECK(g.allow(SoundEvent::Goal, 0));
    CHECK_FALSE(g.allow(e, goalMs / 2));
    CHECK(g.allow(e, goalMs + 1));
  }
}

TEST_CASE("sound gate: Preview can be pressed again and again, and starts over a playing Preview") {
  SoundGate g;
  CHECK(g.allow(SoundEvent::Preview, 0));
  CHECK(g.allow(SoundEvent::Preview, 500));
  CHECK(g.allow(SoundEvent::Preview, 600));
}

TEST_CASE("sound gate: a Preview in progress is not cut short by a low priority alert") {
  SoundGate g;
  CHECK(g.allow(SoundEvent::Preview, 0));
  CHECK_FALSE(g.allow(SoundEvent::KickOff, 100));
}
