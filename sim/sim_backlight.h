// Backlight for the simulator: a black, non-clickable overlay on the system
// layer whose opacity stands in for the panel's dimmed LED. 20 percent is very
// dark but still readable; 100 percent is fully clear. Simulator only.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "backlight.h"

// Opacity of the overlay for a brightness: (100 - percent) * 255 / 100 * 85 / 100.
lv_opa_t sim_backlight_opa(uint8_t percent);

class SimBacklight : public Backlight {
 public:
  // Needs a display: the overlay is created on its system layer.
  SimBacklight();
  void set_percent(uint8_t percent) override;
  uint8_t percent() const { return percent_; }

 private:
  lv_obj_t* shade_ = nullptr;
  uint8_t percent_ = 100;
};
