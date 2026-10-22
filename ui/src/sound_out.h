// The speaker, as the UI sees it. The simulator and the firmware implement
// it; the UI only ever calls it from the LVGL thread (implementations that
// play on another thread queue internally). Shared with the ESP32 firmware.
#pragma once

#include <stdint.h>

#include "sound_event.h"

class SoundOut {
 public:
  virtual ~SoundOut() = default;
  // Starts `clip` now at `volumePercent` (1..100), replacing a clip that is
  // still playing. The clip stays valid for the program's life. Never blocks.
  // The volume curve is perceptual: the samples are multiplied by
  // gain = (volumePercent / 100)^2 (20 % = -28 dB, 40 % = -16 dB,
  // 100 % = 0 dB), as sound_gain_q15() in sound_voice.h computes it.
  virtual void play(const PcmClip& clip, uint8_t volumePercent) = 0;
  // Silences whatever plays (a short fade, no click). Callable from any
  // task; returns at once, the fade runs on (busy() turns false after it).
  virtual void stop() = 0;
  // True from play() until the clip (or the stop fade) has been played out.
  virtual bool busy() const = 0;
};
