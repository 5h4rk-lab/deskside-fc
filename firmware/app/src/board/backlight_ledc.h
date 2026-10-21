// Backlight on GPIO5 driven by LEDC (20 kHz, 10 bit), implements the UI's Backlight.
#pragma once

#include <stdint.h>

#include "backlight.h"

// Perceptual brightness curve: duty (0..1024) for a percentage (0..100).
// 0 is fully off; every percentage from 1 up lights the panel (duty >= 1).
uint16_t backlight_duty_for_percent(uint8_t percent);

class LedcBacklight : public Backlight {
 public:
  // Attaches the LEDC channel and leaves the backlight OFF (the panel RAM
  // holds garbage until the first frame is drawn).
  LedcBacklight();
  void set_percent(uint8_t percent) override;
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};
