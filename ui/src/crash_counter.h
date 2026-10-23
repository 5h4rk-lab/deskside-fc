// Counts abnormal resets (a panic, a watchdog, a brownout) and decides at each
// start whether the device runs normally or in SAFE MODE (no data, no
// pictures, no update checks: Settings and Wi-Fi only, with a banner
// "Safe mode: updates paused"). Pure logic: no clock reads, no storage; the
// board keeps the record in NVS (namespace "health", key "crash") and hands
// in the reset reason and how long the previous start ran.
//
// Time. The wall clock may not be set (no Wi-Fi yet) and a restart resets the
// monotonic clock, so the counter measures time as RUNNING TIME accumulated
// over the starts it covers (the "axis"): each start adds how long the previous
// one ran. The board learns that from a few bytes of RTC memory that survive a
// panic, a watchdog and a software restart and are rewritten every second.
// After a power-on (unplugged, battery) that memory is gone: the previous run
// then counts as 0 ms. That is the safe side: crashes never look further apart
// than they were.
//
// Rules:
// - Abnormal: a panic, the task / interrupt / other watchdogs, a brownout.
//   Not abnormal: power-on, a software restart (the user's :reboot, an update,
//   the power key in the off state), deep sleep, the external reset (USB
//   flashing), anything unknown.
// - kSafeAfter (3) abnormal resets within kWindowMs (10 min) of running time
//   (the newest minus the oldest of the three <= kWindowMs) enter safe mode at
//   that start. Crashes older than the window expire at each start.
// - Running kStableMs (10 min) in normal mode clears the record (nothing is
//   counted any more, and the safe-mode backoff starts over).
// - Safe mode lasts safe_duration_ms(episodes) of running time: 10 min the
//   first time in a row, then 20, 40, 80 min (a fault that comes back each
//   time it leaves safe mode is retried less and less often). It ends:
//   * at runtime, once that much ran (the board then restarts into normal
//     mode: on_uptime() says LeaveSafeMode);
//   * at a start that is not abnormal once that much has run in total (a
//     normal restart "after the window");
//   * with the serial word `:safe-mode off` (cleared, then a restart).
//   An abnormal reset in safe mode keeps it and starts its time again.
// - Entering safe mode asks for an update rollback (rollBack) when at least
//   one of the counted resets was a software fault (not only brownouts: a
//   weak battery is not the image's fault); the board does it only when the
//   other app slot holds a valid older image, and then clears the record so
//   the older image gets a fresh start in normal mode.
#pragma once

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

enum class ResetKind : uint8_t {
  Unknown = 0,
  PowerOn,
  Software,       // esp_restart(): the user, an update, the power key
  External,       // the reset pin / USB flashing
  DeepSleep,
  Panic,          // an exception, abort(), an assert
  TaskWatchdog,   // a subscribed task stopped feeding the task watchdog
  IntWatchdog,    // interrupts blocked too long
  OtherWatchdog,  // RTC / main system watchdogs, CPU lockup
  Brownout,       // the supply dipped
};

bool reset_is_abnormal(ResetKind k);
// A fault of the running code (a panic or a watchdog), not of the supply.
bool reset_is_software_fault(ResetKind k);
// Short English name for the console ("panic", "task watchdog", ...).
const char* reset_kind_name(ResetKind k);

struct CrashRecord;
// The counted crashes for the console, oldest first, with how long ago they
// were in running time: "panic 7 min ago, task watchdog under 1 min ago"; ""
// when none. `axisNowMs`: the record's axis now (axisMs + this start's uptime).
std::string crash_list_text(const CrashRecord& r, int64_t axisNowMs);

// A crash summary for keeping in NVS: `text` cut to at most kCrashSummaryMax
// bytes (at a UTF-8 boundary), control characters replaced by spaces.
constexpr size_t kCrashSummaryMax = 200;
std::string crash_summary_for_storage(const std::string& text);

struct CrashEntry {
  int64_t atMs = 0;  // on the running-time axis
  ResetKind kind = ResetKind::Unknown;
};

struct CrashRecord {
  int64_t axisMs = 0;               // running time since the record started (at this start)
  std::vector<CrashEntry> crashes;  // abnormal resets inside the window, oldest first (at most kSafeAfter)
  bool safe = false;                // in safe mode
  int64_t safeSinceMs = 0;          // axis time safe mode (re)started
  int episodes = 0;                 // safe-mode entries in a row (backoff)
  ResetKind last = ResetKind::Unknown;  // the reason of this start
  bool operator==(const CrashRecord& o) const;
};

// Compact text for NVS ("c1,..."); decode returns false (record untouched)
// for anything malformed, which the board treats as a fresh record.
std::string encode_crash_record(const CrashRecord& r);
bool decode_crash_record(const std::string& text, CrashRecord& out);

struct BootVerdict {
  CrashRecord record;        // store this
  bool safeMode = false;     // run this start in safe mode
  bool enteredSafe = false;  // safe mode starts at this start
  bool leftSafe = false;     // safe mode ended at this start (its time had run)
  bool counted = false;      // this start's reset counted as a crash
  bool rollBack = false;     // entered because of software faults: roll a newer image back if possible
};

class CrashCounter {
 public:
  static constexpr int kSafeAfter = 3;
  static constexpr int64_t kWindowMs = 10LL * 60 * 1000;
  static constexpr int64_t kStableMs = 10LL * 60 * 1000;
  static constexpr int64_t kSafeBaseMs = 10LL * 60 * 1000;
  static constexpr int kMaxBackoffSteps = 3;  // 10, 20, 40, 80 min
  // A previous run longer than this is clamped (a corrupt RTC value cannot
  // push everything out of the window at once... nor overflow).
  static constexpr int64_t kMaxRunMs = 7LL * 24 * 60 * 60 * 1000;

  // At start: the stored record (a fresh one when none or unreadable), why
  // this start happened, and how long the previous start ran (nullopt:
  // unknown, counts as 0).
  static BootVerdict on_boot(const CrashRecord& stored, ResetKind kind, std::optional<int64_t> previousRunMs);

  // How long safe mode lasts for its `episodes`-th entry in a row (>= 1).
  static int64_t safe_duration_ms(int episodes);

  enum class Tick : uint8_t {
    Nothing,
    ClearRecord,    // normal mode ran kStableMs: store fresh_record() (once)
    LeaveSafeMode,  // safe mode's time has run: store left_safe(record), restart into normal mode
  };
  // While running: `record` is what on_boot() stored at this start,
  // `uptimeMs` the time since this start. Once it said ClearRecord the caller
  // stores the fresh record and asks no more.
  static Tick on_uptime(const CrashRecord& record, int64_t uptimeMs);

  // The record after leaving safe mode (at runtime or `:safe-mode off`):
  // nothing counted, not safe; the backoff (episodes) is kept until a stable
  // normal run clears it.
  static CrashRecord left_safe(const CrashRecord& r);
  // Nothing counted at all (a stable normal run, or after a rollback).
  static CrashRecord fresh_record();
};
