// Battery state as the UI sees it. The firmware measures it (ADC, charger
// pin, USB presence) however it can; the simulator scripts it.
#pragma once

struct PowerStatus {
  // False: no battery connected. percent, charging and millivolts are then
  // meaningless.
  bool present = false;
  int percent = 0;       // 0..100
  bool charging = false;
  int millivolts = 0;    // pack voltage, for the Settings line

  bool operator==(const PowerStatus& o) const {
    return present == o.present && percent == o.percent && charging == o.charging &&
           millivolts == o.millivolts;
  }
  bool operator!=(const PowerStatus& o) const { return !(*this == o); }
};

// The user's Battery setting: Auto believes the measurement (BatteryModel),
// Installed always shows a battery, None never does (the board measures about
// 4.18 V with no battery attached, the same as a full pack on USB, so only the
// user can tell). Stored as 0, 1, 2.
enum class BatteryChoice { Auto = 0, Installed = 1, None = 2 };

// Where the app gets the battery state. app_refresh() calls status() about
// once a second: implementations answer from RAM (a value kept up to date
// elsewhere, for example by a BatteryModel fed from an ADC task), may be
// synchronous and must never block.
class PowerService {
 public:
  virtual ~PowerService() = default;
  virtual PowerStatus status() = 0;
};
