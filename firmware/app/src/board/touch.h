// Touch controller detection and polling (CST328 on V1 boards, CST3530 on V2).
// One image runs on both. Coordinates are screen coordinates as the
// controller reports them (x 0..239, y 0..319), used as is.
#pragma once

#include <stdint.h>

enum class TouchController : uint8_t { None, CST328, CST3530 };

struct TouchPoint {
  uint16_t x;
  uint16_t y;
};

const char* touch_controller_name(TouchController c);

// Starts Wire1, runs the vendor reset sequences and detects the controller
// (CST328 first, else CST3530). Takes up to about 0.9 s and never blocks
// forever. Returns None if no controller answered (touch then never reports a
// press; the rest of the firmware keeps working).
TouchController touch_begin();

TouchController touch_controller();

// Polls the controller once and returns true while a finger is down, with
// its position in `out`. On release it returns false (`out` keeps the last
// position). Called by the LVGL indev read callback only: the I2C bus is not
// shared with any other task.
//
// Release: an empty read while the INT pin gave no new edge since the last
// poll; one such poll is tolerated before the release is reported (debounce
// of one poll).
bool touch_poll(TouchPoint& out);

uint32_t touch_interrupt_count();  // rising edges seen on the INT pin
uint32_t touch_i2c_error_count();

// Bus recoveries run since boot, and whether the I2C bus is currently failing
// (polling slowed down). See touch_health.h for the rules.
uint32_t touch_recovery_count();
bool touch_in_backoff();
