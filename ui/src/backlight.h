// Screen backlight. The firmware drives the panel's LED with LEDC PWM; the
// simulator darkens the window with an overlay. Shared with the firmware: no
// SDL, no stdio.
#pragma once

#include <stdint.h>

class Backlight {
 public:
  virtual ~Backlight() = default;
  // 0 (off) .. 100 (full).
  virtual void set_percent(uint8_t percent) = 0;
};
