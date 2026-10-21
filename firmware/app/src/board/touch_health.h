// Decision logic for touch I2C failures: when to back off polling and when to
// recover the bus. Pure (no Arduino, no clock reads: the caller passes the time),
// so it can be reasoned about and unit-tested on a host.
//
// A "poll" is one touch_poll() that talked to the controller; it FAILED if any
// I2C transaction in it returned an error. Reading an empty frame is not a
// failure.
//
//   - after kBackoffAfter consecutive failed polls the state becomes "backoff":
//     the controller is polled only every kBackoffPollMs (a hung bus then costs
//     one Wire timeout per 200 ms instead of per 20 ms);
//   - at every kRecoverEvery-th consecutive failure the caller must run the bus
//     recovery (re-init, SCL clocking, controller reset);
//   - the first successful poll leaves the backoff and clears the streak.
#pragma once

#include <stdint.h>

struct TouchHealth {
  static constexpr uint32_t kBackoffAfter = 3;
  static constexpr uint32_t kRecoverEvery = 10;
  static constexpr uint32_t kBackoffPollMs = 200;

  struct Result {
    bool enteredBackoff = false;  // log once
    bool leftBackoff = false;     // log once
    bool recover = false;         // run the bus recovery now
  };

  uint32_t failStreak = 0;
  bool backoff = false;
  uint32_t nextPollMs = 0;

  // Should the controller be polled at `nowMs`? Wrap-safe.
  bool shouldPoll(uint32_t nowMs) const { return !backoff || (int32_t)(nowMs - nextPollMs) >= 0; }

  // Reports the outcome of a poll made at `nowMs`.
  Result onPoll(bool ok, uint32_t nowMs) {
    Result r;
    if (ok) {
      r.leftBackoff = backoff;
      failStreak = 0;
      backoff = false;
      return r;
    }
    ++failStreak;
    if (failStreak >= kBackoffAfter) {
      r.enteredBackoff = !backoff;
      backoff = true;
      nextPollMs = nowMs + kBackoffPollMs;
    }
    r.recover = failStreak % kRecoverEvery == 0;
    return r;
  }
};
