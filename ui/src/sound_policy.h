// When a sound may play: the settings (mode, night mute) and the gate (no
// stacking, no repeats). Pure; the caller supplies the monotonic time and
// whether night dimming is active. Shared with the ESP32 firmware.
#pragma once

#include <stdint.h>

#include "settings.h"
#include "sound_event.h"

// Mode and night mute. Off plays nothing; Goals only plays Goal, WinFanfare,
// the battery beeps and WifiOk; Goals and match alerts adds KickOff and
// FullTime. Night mute silences everything while night dimming is active.
// Preview ignores both (the volume still applies, it is the caller's).
bool sound_should_play(const Settings& settings, SoundEvent event, bool nightDimActive);

constexpr int64_t kSoundRepeatMs = 10000;  // the same event is not repeated inside this

class SoundGate {
 public:
  // True when `event` may start at `nowMs` (monotonic), and records it. Rules:
  //  - the same event never repeats within kSoundRepeatMs (Preview may: the
  //    user is pressing a button);
  //  - Goal, WinFanfare and Preview start whatever is playing (the host's
  //    play() replaces a clip still playing);
  //  - every other event waits out a Goal, WinFanfare or Preview that is
  //    still playing (KickOff and FullTime never cut a goal short).
  bool allow(SoundEvent event, int64_t nowMs);

 private:
  bool seen_[kSoundEventCount] = {};
  int64_t at_[kSoundEventCount] = {};
  bool playingLoud_ = false;  // the latest clip is a Goal, WinFanfare or Preview
  int64_t playingUntil_ = 0;
};
