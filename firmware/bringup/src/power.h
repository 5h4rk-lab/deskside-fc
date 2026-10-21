// Battery voltage, LiPo percentage estimate, PWR key logging.
//
// The power button is only logged. The vendor long-press logic (10 s sleep,
// 15 s restart, 20 s shutdown) is NOT implemented. GPIO7 (power hold) stays
// high all the time (see main.cpp holdPower()), EXCEPT during the opt-in
// power-off test below, which never starts by itself.
#pragma once

#include <stdint.h>

namespace power {

enum class Trend : uint8_t { Steady, Rising, Falling };

const char *trendName(Trend t);

// Sets up the PWR key input and the ADC, takes the first battery reading.
void begin();

// Call from loop(): polls the PWR key every 20 ms (30 ms debounce) and logs
// press / release events with the press duration. Cheap when nothing is due.
void pollKey();

// Takes a fresh averaged reading, stores it for the trend, and logs the
// [power] status line plus the interpretation lines. Call every 5 s.
void logStatus();

// Opt-in power-off test (finds out whether driving GPIO7 low really switches
// the board off). Never automatic. Serial 'o' starts it; holding the PWR key
// for 3 s starts it only after serial 'a' armed it in this boot.
void armPowerOffTest();
void startPowerOffTest();

// Latest averaged battery voltage in mV (also available before logStatus()).
uint32_t batteryMv();

// True when the latest reading looks like a connected battery (>= 2500 mV).
bool batteryPresent();

// Approximate LiPo charge 0..100 from a resting voltage (see power.cpp).
int lipoPercent(uint32_t mv);

}  // namespace power
