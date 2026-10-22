// CrashCounter: which resets count, the 10-minute window on the running-time
// axis, entering and leaving safe mode, the backoff, the rollback request and
// the stored text.
#include <doctest/doctest.h>

#include <optional>
#include <string>

#include "crash_counter.h"

namespace {

constexpr int64_t kMin = 60 * 1000;

// Runs one start: the previous run lasted `ranMs` (nullopt: unknown), then a
// reset of `kind`. Returns the verdict; `r` becomes what the board stores.
BootVerdict boot(CrashRecord& r, ResetKind kind, std::optional<int64_t> ranMs) {
  BootVerdict v = CrashCounter::on_boot(r, kind, ranMs);
  r = v.record;
  return v;
}

}  // namespace

TEST_CASE("crash counter: which resets are abnormal") {
  CHECK(reset_is_abnormal(ResetKind::Panic));
  CHECK(reset_is_abnormal(ResetKind::TaskWatchdog));
  CHECK(reset_is_abnormal(ResetKind::IntWatchdog));
  CHECK(reset_is_abnormal(ResetKind::OtherWatchdog));
  CHECK(reset_is_abnormal(ResetKind::Brownout));
  CHECK_FALSE(reset_is_abnormal(ResetKind::PowerOn));
  CHECK_FALSE(reset_is_abnormal(ResetKind::Software));
  CHECK_FALSE(reset_is_abnormal(ResetKind::External));
  CHECK_FALSE(reset_is_abnormal(ResetKind::DeepSleep));
  CHECK_FALSE(reset_is_abnormal(ResetKind::Unknown));
  CHECK(reset_is_software_fault(ResetKind::Panic));
  CHECK(reset_is_software_fault(ResetKind::TaskWatchdog));
  CHECK_FALSE(reset_is_software_fault(ResetKind::Brownout));
  CHECK(std::string(reset_kind_name(ResetKind::TaskWatchdog)) == "task watchdog");
  CHECK(std::string(reset_kind_name(ResetKind::Brownout)) == "brownout");
  CHECK(std::string(reset_kind_name(ResetKind::Software)) == "software restart");
}

TEST_CASE("crash counter: three crashes within 10 minutes of running enter safe mode") {
  CrashRecord r;
  BootVerdict v = boot(r, ResetKind::PowerOn, std::nullopt);
  CHECK_FALSE(v.safeMode);
  CHECK_FALSE(v.counted);
  v = boot(r, ResetKind::Panic, 2 * kMin);
  CHECK(v.counted);
  CHECK_FALSE(v.safeMode);
  CHECK(r.crashes.size() == 1);
  v = boot(r, ResetKind::TaskWatchdog, 3 * kMin);
  CHECK_FALSE(v.safeMode);
  CHECK(r.crashes.size() == 2);
  v = boot(r, ResetKind::Panic, 4 * kMin);
  CHECK(v.safeMode);
  CHECK(v.enteredSafe);
  CHECK(v.rollBack);
  CHECK(r.safe);
  CHECK(r.episodes == 1);
  CHECK(r.last == ResetKind::Panic);
}

TEST_CASE("crash counter: the window boundary (newest minus oldest <= 10 min counts, 1 ms more does not)") {
  // Crashes at axis 0, 5 min, 10 min exactly: inside.
  CrashRecord r;
  boot(r, ResetKind::Panic, 0);
  boot(r, ResetKind::Panic, 5 * kMin);
  BootVerdict v = boot(r, ResetKind::Panic, 5 * kMin);
  CHECK(v.safeMode);
  // At 0, 5 min, 10 min + 1 ms: the first has expired, two remain.
  CrashRecord s;
  boot(s, ResetKind::Panic, 0);
  boot(s, ResetKind::Panic, 5 * kMin);
  v = boot(s, ResetKind::Panic, 5 * kMin + 1);
  CHECK_FALSE(v.safeMode);
  CHECK(s.crashes.size() == 2);
  // ...and a fourth soon after makes three inside the window.
  v = boot(s, ResetKind::Panic, kMin);
  CHECK(v.safeMode);
}

TEST_CASE("crash counter: user restarts, power-on, USB flashing and deep sleep never count") {
  CrashRecord r;
  for (ResetKind k : {ResetKind::Software, ResetKind::PowerOn, ResetKind::External, ResetKind::DeepSleep,
                      ResetKind::Unknown, ResetKind::Software, ResetKind::Software}) {
    const BootVerdict v = boot(r, k, 1000);
    CHECK_FALSE(v.counted);
    CHECK_FALSE(v.safeMode);
  }
  CHECK(r.crashes.empty());
  // Two crashes with user restarts in between: still two, not safe mode.
  boot(r, ResetKind::Panic, 1000);
  boot(r, ResetKind::Software, 1000);
  boot(r, ResetKind::Software, 1000);
  const BootVerdict v = boot(r, ResetKind::TaskWatchdog, 1000);
  CHECK(r.crashes.size() == 2);
  CHECK_FALSE(v.safeMode);
}

TEST_CASE("crash counter: running time of normal starts in between ages crashes out (stale entries expire)") {
  CrashRecord r;
  boot(r, ResetKind::Panic, kMin);
  boot(r, ResetKind::Panic, kMin);
  CHECK(r.crashes.size() == 2);
  // A user restart after 9 min, then another after 2 min: 11 min since the first two.
  boot(r, ResetKind::Software, 9 * kMin);
  CHECK(r.crashes.size() == 2);  // 9 min old: still inside
  boot(r, ResetKind::Software, 2 * kMin);
  CHECK(r.crashes.empty());      // 11 min: both expired
  CHECK(r.axisMs == 0);          // nothing refers to the axis any more: it starts over
  const BootVerdict v = boot(r, ResetKind::Panic, kMin);
  CHECK_FALSE(v.safeMode);
  CHECK(r.crashes.size() == 1);
}

TEST_CASE("crash counter: an unknown previous run counts as 0 ms (the safe side)") {
  CrashRecord r;
  boot(r, ResetKind::Brownout, std::nullopt);
  boot(r, ResetKind::Brownout, std::nullopt);
  const BootVerdict v = boot(r, ResetKind::Brownout, std::nullopt);
  CHECK(v.safeMode);
  CHECK_FALSE(v.rollBack);  // brownouts only: the supply, not the image
}

TEST_CASE("crash counter: a corrupt or huge previous run is clamped, a negative one is 0") {
  CrashRecord r;
  boot(r, ResetKind::Panic, 0);
  boot(r, ResetKind::Panic, -5000);
  CHECK(r.crashes.size() == 2);
  boot(r, ResetKind::Software, INT64_MAX);
  CHECK(r.crashes.empty());
  CHECK(r.axisMs <= CrashCounter::kMaxRunMs);
}

TEST_CASE("crash counter: the rollback is asked for when any counted reset was a software fault") {
  CrashRecord r;
  boot(r, ResetKind::Brownout, 1000);
  boot(r, ResetKind::Brownout, 1000);
  BootVerdict v = boot(r, ResetKind::TaskWatchdog, 1000);
  CHECK(v.enteredSafe);
  CHECK(v.rollBack);
}

TEST_CASE("crash counter: normal mode running 10 minutes clears the record") {
  CrashRecord r;
  boot(r, ResetKind::Panic, kMin);
  boot(r, ResetKind::Panic, kMin);
  CHECK(CrashCounter::on_uptime(r, CrashCounter::kStableMs - 1) == CrashCounter::Tick::Nothing);
  CHECK(CrashCounter::on_uptime(r, CrashCounter::kStableMs) == CrashCounter::Tick::ClearRecord);
  r = CrashCounter::fresh_record();
  CHECK(r.crashes.empty());
  CHECK(r.episodes == 0);
  // Nothing to clear: nothing to do (no NVS write every start).
  CHECK(CrashCounter::on_uptime(CrashCounter::fresh_record(), 3600 * 1000) == CrashCounter::Tick::Nothing);
}

TEST_CASE("crash counter: safe mode ends after its time at runtime, and the backoff doubles it") {
  CrashRecord r;
  boot(r, ResetKind::Panic, 0);
  boot(r, ResetKind::Panic, 0);
  BootVerdict v = boot(r, ResetKind::Panic, 0);
  REQUIRE(v.safeMode);
  CHECK(CrashCounter::safe_duration_ms(1) == 10 * kMin);
  CHECK(CrashCounter::safe_duration_ms(2) == 20 * kMin);
  CHECK(CrashCounter::safe_duration_ms(3) == 40 * kMin);
  CHECK(CrashCounter::safe_duration_ms(4) == 80 * kMin);
  CHECK(CrashCounter::safe_duration_ms(9) == 80 * kMin);
  CHECK(CrashCounter::on_uptime(r, 10 * kMin - 1) == CrashCounter::Tick::Nothing);
  CHECK(CrashCounter::on_uptime(r, 10 * kMin) == CrashCounter::Tick::LeaveSafeMode);
  r = CrashCounter::left_safe(r);
  CHECK_FALSE(r.safe);
  CHECK(r.crashes.empty());
  CHECK(r.episodes == 1);
  // Back in normal mode (a software restart), the fault comes back three times: 20 min this time.
  v = boot(r, ResetKind::Software, 10 * kMin);
  CHECK_FALSE(v.safeMode);
  boot(r, ResetKind::Panic, 1000);
  boot(r, ResetKind::Panic, 1000);
  v = boot(r, ResetKind::Panic, 1000);
  REQUIRE(v.safeMode);
  CHECK(r.episodes == 2);
  CHECK(CrashCounter::on_uptime(r, 10 * kMin) == CrashCounter::Tick::Nothing);
  CHECK(CrashCounter::on_uptime(r, 20 * kMin) == CrashCounter::Tick::LeaveSafeMode);
  // A stable normal run afterwards resets the backoff.
  r = CrashCounter::left_safe(r);
  boot(r, ResetKind::Software, 20 * kMin);
  CHECK(r.episodes == 2);
  CHECK(CrashCounter::on_uptime(r, CrashCounter::kStableMs) == CrashCounter::Tick::ClearRecord);
  CHECK(CrashCounter::fresh_record().episodes == 0);
}

TEST_CASE("crash counter: a normal restart after the safe-mode window leaves it; one inside the window does not") {
  CrashRecord r;
  boot(r, ResetKind::Panic, 0);
  boot(r, ResetKind::Panic, 0);
  REQUIRE(boot(r, ResetKind::Panic, 0).safeMode);
  // The user restarts after 3 min: still safe mode.
  BootVerdict v = boot(r, ResetKind::Software, 3 * kMin);
  CHECK(v.safeMode);
  CHECK_FALSE(v.enteredSafe);
  CHECK_FALSE(v.rollBack);
  // ...and after 4 more (7 min in total): still.
  v = boot(r, ResetKind::Software, 4 * kMin);
  CHECK(v.safeMode);
  // Runtime agrees: 3 min more in this start reaches the 10 min.
  CHECK(CrashCounter::on_uptime(r, 3 * kMin) == CrashCounter::Tick::LeaveSafeMode);
  // A restart after 3 min more instead: the window has run, normal mode.
  v = boot(r, ResetKind::Software, 3 * kMin);
  CHECK_FALSE(v.safeMode);
  CHECK(v.leftSafe);
  CHECK(r.crashes.empty());
  CHECK(r.episodes == 1);
}

TEST_CASE("crash counter: a power-on in safe mode (previous run unknown) stays in safe mode") {
  CrashRecord r;
  boot(r, ResetKind::Panic, 0);
  boot(r, ResetKind::Panic, 0);
  REQUIRE(boot(r, ResetKind::Panic, 0).safeMode);
  const BootVerdict v = boot(r, ResetKind::PowerOn, std::nullopt);
  CHECK(v.safeMode);
}

TEST_CASE("crash counter: a crash in safe mode keeps it and starts its time again") {
  CrashRecord r;
  boot(r, ResetKind::Panic, 0);
  boot(r, ResetKind::Panic, 0);
  REQUIRE(boot(r, ResetKind::Panic, 0).safeMode);
  BootVerdict v = boot(r, ResetKind::TaskWatchdog, 9 * kMin);
  CHECK(v.safeMode);
  CHECK_FALSE(v.enteredSafe);
  CHECK_FALSE(v.rollBack);  // the rollback is asked for once, on entering
  CHECK(r.episodes == 1);
  CHECK(CrashCounter::on_uptime(r, 9 * kMin) == CrashCounter::Tick::Nothing);
  CHECK(CrashCounter::on_uptime(r, 10 * kMin) == CrashCounter::Tick::LeaveSafeMode);
}

TEST_CASE("crash counter: :safe-mode off clears the crashes and leaves safe mode") {
  CrashRecord r;
  boot(r, ResetKind::Panic, 0);
  boot(r, ResetKind::Panic, 0);
  REQUIRE(boot(r, ResetKind::Panic, 0).safeMode);
  r = CrashCounter::left_safe(r);
  const BootVerdict v = boot(r, ResetKind::Software, 1000);
  CHECK_FALSE(v.safeMode);
  CHECK_FALSE(v.counted);
  // One more crash is the first of a new count, not the fourth.
  const BootVerdict w = boot(r, ResetKind::Panic, 1000);
  CHECK_FALSE(w.safeMode);
  CHECK(r.crashes.size() == 1);
}

TEST_CASE("crash counter: at most kSafeAfter crashes are kept") {
  CrashRecord r;
  for (int i = 0; i < 10; ++i) boot(r, ResetKind::Panic, 1000);
  CHECK(r.crashes.size() == 3);
  CHECK(r.safe);
}

TEST_CASE("crash counter: the stored text round-trips and garbage is refused") {
  CrashRecord r;
  boot(r, ResetKind::Panic, 1234);
  boot(r, ResetKind::Brownout, 5678);
  boot(r, ResetKind::TaskWatchdog, 999);
  REQUIRE(r.safe);
  const std::string text = encode_crash_record(r);
  CHECK(text.size() <= 120);
  CrashRecord back;
  REQUIRE(decode_crash_record(text, back));
  CHECK(back == r);
  CHECK(encode_crash_record(back) == text);
  CrashRecord fresh;
  REQUIRE(decode_crash_record(encode_crash_record(fresh), back));
  CHECK(back == fresh);
  CrashRecord untouched = r;
  for (const char* bad : {"", "x", "c1", "c2,0,0,0,0,0", "c1,0,0,0,0,0,9", "c1,-5,0,0,0,0,0", "c1,0,0,0,0,0,4,1,2",
                          "c1,0,0,0,0,0,1,5", "c1,0,0,0,0,0,1,5,99", "c1,a,0,0,0,0,0", "c1,0,2,0,0,0,0",
                          "c1,0,0,0,0,99,0"}) {
    CAPTURE(bad);
    CHECK_FALSE(decode_crash_record(bad, untouched));
    CHECK(untouched == r);
  }
}
