// Touch controller detection and polling (CST328 on V1 boards, CST3530 on V2).
#pragma once

#include <stdint.h>

namespace touch {

enum class Controller : uint8_t { None, CST328, CST3530 };

const char *name(Controller c);

// Starts Wire1, runs the vendor reset sequences, detects the controller,
// scans Wire1 and logs everything. Never blocks forever; returns None if no
// controller answered.
Controller begin();

// Starts the main I2C bus (Wire, SDA 11 / SCL 10) and logs every address.
void scanMainBus();

struct Point {
  uint16_t x;
  uint16_t y;
};

// Polls the detected controller. Returns the number of touch points (0 when
// not touched or on error) and the first point in *first.
int read(Point *first);

uint32_t interruptCount();  // rising edges seen on the INT pin
uint32_t i2cErrorCount();

}  // namespace touch
