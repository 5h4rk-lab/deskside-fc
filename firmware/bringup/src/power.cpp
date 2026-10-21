#include "power.h"

#include <Arduino.h>

#include "board_pins.h"
#include "bringup_log.h"

namespace power {
namespace {

// Battery voltage measurement, copied from the Waveshare demo (BAT_Driver.cpp):
//   volts = analogReadMilliVolts(8) * 3.0 / 1000 / 0.990476
// The divider factor and the offset come from the vendor demo and are
// UNVERIFIED on hardware: compare the logged value with a multimeter.
constexpr int kAdcSamples = 16;
constexpr float kDividerFactor = 3.0f;
constexpr float kMeasurementOffset = 0.990476f;

// Approximate LiPo resting-voltage curve, 11 points, 10 % steps. Real cells
// differ by make, age and load, and the voltage sags under load or while
// charging, so the percentage is only a rough hint.
struct CurvePoint {
  uint16_t mv;
  uint8_t pct;
};
constexpr CurvePoint kCurve[] = {
    {3300, 0},  {3500, 10}, {3600, 20}, {3650, 30}, {3700, 40}, {3750, 50},
    {3800, 60}, {3870, 70}, {3950, 80}, {4050, 90}, {4200, 100},
};
constexpr int kCurveLen = sizeof(kCurve) / sizeof(kCurve[0]);

// Plausibility limits for the interpretation lines.
constexpr uint32_t kNoBatteryMv = 2500;
constexpr uint32_t kVeryLowMv = 3300;
constexpr uint32_t kTooHighMv = 4350;
constexpr uint32_t kChargingMinMv = 4000;

// Trend over the last 60 s: one reading per logStatus() call (5 s apart).
constexpr int kHistory = 13;                 // 12 intervals of 5 s = 60 s
constexpr int kMinHistoryForTrend = 7;       // need >= 30 s of data
constexpr int32_t kTrendThresholdMv = 15;    // below this the change is noise
constexpr uint32_t kRepeatMs = 60000;        // interpretation lines repeat rate

// Power-off test timing.
constexpr int kOffCountdownS = 3;
constexpr uint32_t kOffHoldKeyMs = 3000;   // PWR key hold that starts the test (armed only)
constexpr uint32_t kOffStayedOnMs = 5000;  // still running this long after GPIO7 low => USB powers us

constexpr uint32_t kKeyPollMs = 20;
constexpr uint32_t kKeyDebounceMs = 30;

enum class OffTest : uint8_t { Idle, Countdown, WaitAfterLow };

struct State {
  bool armed = false;
  OffTest offTest = OffTest::Idle;
  uint32_t offStartedAt = 0;
  int offSecondsLeft = 0;
  uint32_t offLowAt = 0;
  bool keyHoldHandled = false;

  uint32_t latestMv = 0;
  uint32_t history[kHistory] = {};
  int histCount = 0;  // valid entries, oldest first up to kHistory
  uint32_t lastChargingNote = 0;
  bool chargingNoted = false;
  int lastInterp = -1;
  uint32_t lastInterpAt = 0;

  bool keyDown = false;       // debounced state
  bool keyCandidate = false;  // raw state waiting for the debounce time
  uint32_t candidateSince = 0;
  uint32_t pressedAt = 0;
  uint32_t lastPoll = 0;
} g;

uint32_t readBatteryMv() {
  uint32_t sum = 0;
  for (int i = 0; i < kAdcSamples; ++i) sum += analogReadMilliVolts(pins::kBatteryAdc);
  const float adcMv = float(sum) / kAdcSamples;
  return uint32_t(adcMv * kDividerFactor / kMeasurementOffset + 0.5f);
}

Trend computeTrend() {
  if (g.histCount < kMinHistoryForTrend) return Trend::Steady;
  const int32_t delta = int32_t(g.history[g.histCount - 1]) - int32_t(g.history[0]);
  if (delta >= kTrendThresholdMv) return Trend::Rising;
  if (delta <= -kTrendThresholdMv) return Trend::Falling;
  return Trend::Steady;
}

void pushHistory(uint32_t mv) {
  if (g.histCount == kHistory) {
    for (int i = 1; i < kHistory; ++i) g.history[i - 1] = g.history[i];
    g.histCount = kHistory - 1;
  }
  g.history[g.histCount++] = mv;
}

// 0 = no battery, 1 = very low / offset wrong, 2 = too high, 3 = plausible.
int interpretation(uint32_t mv) {
  if (mv < kNoBatteryMv) return 0;
  if (mv < kVeryLowMv) return 1;
  if (mv > kTooHighMv) return 2;
  return 3;
}

void logInterpretation(uint32_t mv, Trend trend, uint32_t now) {
  const int kind = interpretation(mv);
  if (kind != 3 && (kind != g.lastInterp || now - g.lastInterpAt >= kRepeatMs)) {
    g.lastInterp = kind;
    g.lastInterpAt = now;
    switch (kind) {
      case 0: logf("[power] no battery detected (ADC reads near 0)"); break;
      case 1: logf("[power] battery very low or ADC offset wrong"); break;
      case 2: logf("[power] voltage above 4.35 V: check wiring"); break;
    }
  } else if (kind == 3) {
    g.lastInterp = kind;
  }
  if (trend == Trend::Rising && mv >= kChargingMinMv &&
      (!g.chargingNoted || now - g.lastChargingNote >= kRepeatMs)) {
    g.chargingNoted = true;
    g.lastChargingNote = now;
    logf("[power] probably charging from USB (inferred from voltage trend, not a charger status pin)");
  }
}

}  // namespace

const char *trendName(Trend t) {
  switch (t) {
    case Trend::Rising: return "rising";
    case Trend::Falling: return "falling";
    default: return "steady";
  }
}

int lipoPercent(uint32_t mv) {
  if (mv <= kCurve[0].mv) return 0;
  if (mv >= kCurve[kCurveLen - 1].mv) return 100;
  for (int i = 1; i < kCurveLen; ++i) {
    if (mv <= kCurve[i].mv) {
      const int lo = kCurve[i - 1].mv, hi = kCurve[i].mv;
      const int plo = kCurve[i - 1].pct, phi = kCurve[i].pct;
      return plo + (int(mv) - lo) * (phi - plo) / (hi - lo);
    }
  }
  return 100;
}

uint32_t batteryMv() { return g.latestMv; }

bool batteryPresent() { return g.latestMv >= kNoBatteryMv; }

void begin() {
  // Internal pull-up is harmless if the board already has an external one.
  pinMode(pins::kPowerKey, INPUT_PULLUP);
  pinMode(pins::kBatteryAdc, INPUT);
  g.keyDown = g.keyCandidate = (digitalRead(pins::kPowerKey) == LOW);
  g.latestMv = readBatteryMv();
  logf("[power] battery ADC GPIO%d, divider %.1f, offset %.6f (vendor values, unverified on hardware)",
       pins::kBatteryAdc, (double)kDividerFactor, (double)kMeasurementOffset);
  logf("[power] power key GPIO%d is only logged (vendor long-press shutdown not implemented)",
       pins::kPowerKey);
}

void armPowerOffTest() {
  g.armed = true;
  logf("[power] power-off test ARMED for this boot: hold the PWR key for 3 s or send 'o' to start it");
}

void startPowerOffTest() {
  if (g.offTest != OffTest::Idle) {
    logf("[power] power-off test already running");
    return;
  }
  logf("[power] POWER-OFF TEST: driving GPIO%d low in %d s (unplug nothing)", pins::kPowerHold, kOffCountdownS);
  g.offTest = OffTest::Countdown;
  g.offStartedAt = millis();
  g.offSecondsLeft = kOffCountdownS;
}

// Advances the power-off test state machine; called from pollKey().
static void stepPowerOffTest(uint32_t now) {
  if (g.offTest == OffTest::Countdown) {
    const int left = kOffCountdownS - int((now - g.offStartedAt) / 1000);
    if (left > 0 && left < g.offSecondsLeft) {
      g.offSecondsLeft = left;
      logf("[power] POWER-OFF TEST: %d ...", left);
    } else if (left <= 0) {
      logf("[power] POWER-OFF TEST: driving GPIO%d LOW now (hold=%s before)", pins::kPowerHold,
           digitalRead(pins::kPowerHold) ? "high" : "low");
      Serial.flush();
      digitalWrite(pins::kPowerHold, LOW);
      g.offLowAt = millis();
      g.offTest = OffTest::WaitAfterLow;
    }
  } else if (g.offTest == OffTest::WaitAfterLow && now - g.offLowAt >= kOffStayedOnMs) {
    logf("[power] board stayed on after GPIO%d low: USB (or something else) keeps it powered; GPIO%d restored high",
         pins::kPowerHold, pins::kPowerHold);
    digitalWrite(pins::kPowerHold, HIGH);
    g.offTest = OffTest::Idle;
  }
}

void pollKey() {
  const uint32_t now = millis();
  if (now - g.lastPoll < kKeyPollMs) return;
  g.lastPoll = now;
  stepPowerOffTest(now);
  if (g.keyDown && g.armed && !g.keyHoldHandled && now - g.pressedAt >= kOffHoldKeyMs) {
    g.keyHoldHandled = true;
    startPowerOffTest();
  }

  const bool raw = digitalRead(pins::kPowerKey) == LOW;
  if (raw != g.keyCandidate) {
    g.keyCandidate = raw;
    g.candidateSince = now;
    return;
  }
  if (raw == g.keyDown || now - g.candidateSince < kKeyDebounceMs) return;

  g.keyDown = raw;
  if (raw) {
    g.pressedAt = g.candidateSince;
    g.keyHoldHandled = false;
    logf("[power] pwr_key pressed");
  } else {
    logf("[power] pwr_key released after %lu ms", (unsigned long)(g.candidateSince - g.pressedAt));
  }
}

void logStatus() {
  const uint32_t now = millis();
  g.latestMv = readBatteryMv();
  pushHistory(g.latestMv);
  const Trend trend = computeTrend();
  logf("[power] battery=%lu mV (~%d%%) trend=%s pwr_key=%s hold=%s", (unsigned long)g.latestMv,
       lipoPercent(g.latestMv), trendName(trend), g.keyDown ? "down" : "up",
       digitalRead(pins::kPowerHold) ? "high" : "low");
  logInterpretation(g.latestMv, trend, now);
}

}  // namespace power
