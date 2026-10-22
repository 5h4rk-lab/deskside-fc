// The speaker, as the UI sees it. The simulator and the firmware implement
// it; the UI only ever calls it from the LVGL thread (implementations that
// play on another thread queue internally). Shared with the ESP32 firmware.
#pragma once

#include <stdint.h>

#include "sound_event.h"

class SoundOut {
 public:
  virtual ~SoundOut() = default;
  // Starts `clip` now at `volumePercent` (1..100) of full scale, replacing a
  // clip that is still playing. The clip stays valid for the program's life.
  virtual void play(const PcmClip& clip, uint8_t volumePercent) = 0;
  // Silences whatever plays.
  virtual void stop() = 0;
  virtual bool busy() const = 0;
};
