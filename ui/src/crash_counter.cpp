#include "crash_counter.h"

#include <stdlib.h>

#include <algorithm>

namespace {

constexpr int kMaxKind = static_cast<int>(ResetKind::Brownout);

int64_t clamp_run(std::optional<int64_t> ran) {
  if (!ran || *ran < 0) return 0;
  return std::min(*ran, CrashCounter::kMaxRunMs);
}

// Strict decimal parse of one comma-separated field; advances `p`.
bool next_field(const char*& p, int64_t& out) {
  if (*p == '\0') return false;
  const char* start = p;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    ++p;
  }
  if (*p < '0' || *p > '9') return false;
  int64_t v = 0;
  while (*p >= '0' && *p <= '9') {
    if (v > (INT64_MAX - 9) / 10) return false;
    v = v * 10 + (*p - '0');
    ++p;
  }
  if (*p == ',') {
    ++p;
  } else if (*p != '\0') {
    return false;
  }
  (void)start;
  out = neg ? -v : v;
  return true;
}

}  // namespace

bool reset_is_abnormal(ResetKind k) {
  switch (k) {
    case ResetKind::Panic:
    case ResetKind::TaskWatchdog:
    case ResetKind::IntWatchdog:
    case ResetKind::OtherWatchdog:
    case ResetKind::Brownout:
      return true;
    default:
      return false;
  }
}

bool reset_is_software_fault(ResetKind k) { return reset_is_abnormal(k) && k != ResetKind::Brownout; }

const char* reset_kind_name(ResetKind k) {
  switch (k) {
    case ResetKind::Unknown: return "unknown";
    case ResetKind::PowerOn: return "power-on";
    case ResetKind::Software: return "software restart";
    case ResetKind::External: return "external reset (USB/pin)";
    case ResetKind::DeepSleep: return "deep sleep wake";
    case ResetKind::Panic: return "panic";
    case ResetKind::TaskWatchdog: return "task watchdog";
    case ResetKind::IntWatchdog: return "interrupt watchdog";
    case ResetKind::OtherWatchdog: return "other watchdog";
    case ResetKind::Brownout: return "brownout";
  }
  return "unknown";
}

bool CrashRecord::operator==(const CrashRecord& o) const {
  if (axisMs != o.axisMs || safe != o.safe || safeSinceMs != o.safeSinceMs || episodes != o.episodes ||
      last != o.last || crashes.size() != o.crashes.size()) {
    return false;
  }
  for (size_t i = 0; i < crashes.size(); ++i) {
    if (crashes[i].atMs != o.crashes[i].atMs || crashes[i].kind != o.crashes[i].kind) return false;
  }
  return true;
}

// "c1,axis,safe,safeSince,episodes,last,n[,at,kind]*n"
std::string encode_crash_record(const CrashRecord& r) {
  std::string s = "c1," + std::to_string(r.axisMs) + "," + (r.safe ? "1" : "0") + "," +
                  std::to_string(r.safeSinceMs) + "," + std::to_string(r.episodes) + "," +
                  std::to_string(static_cast<int>(r.last)) + "," + std::to_string(r.crashes.size());
  for (const CrashEntry& e : r.crashes) {
    s += "," + std::to_string(e.atMs) + "," + std::to_string(static_cast<int>(e.kind));
  }
  return s;
}

bool decode_crash_record(const std::string& text, CrashRecord& out) {
  if (text.compare(0, 3, "c1,") != 0) return false;
  const char* p = text.c_str() + 3;
  int64_t axis, safe, since, episodes, last, n;
  if (!next_field(p, axis) || !next_field(p, safe) || !next_field(p, since) || !next_field(p, episodes) ||
      !next_field(p, last) || !next_field(p, n)) {
    return false;
  }
  if (axis < 0 || (safe != 0 && safe != 1) || since < 0 || since > axis || episodes < 0 || episodes > 1000 ||
      last < 0 || last > kMaxKind || n < 0 || n > CrashCounter::kSafeAfter) {
    return false;
  }
  CrashRecord r;
  r.axisMs = axis;
  r.safe = safe == 1;
  r.safeSinceMs = since;
  r.episodes = static_cast<int>(episodes);
  r.last = static_cast<ResetKind>(last);
  int64_t prev = 0;
  for (int64_t i = 0; i < n; ++i) {
    int64_t at, kind;
    if (!next_field(p, at) || !next_field(p, kind)) return false;
    if (at < prev || at > axis || kind < 0 || kind > kMaxKind) return false;
    if (!reset_is_abnormal(static_cast<ResetKind>(kind))) return false;
    prev = at;
    r.crashes.push_back({at, static_cast<ResetKind>(kind)});
  }
  if (*p != '\0') return false;
  out = r;
  return true;
}

int64_t CrashCounter::safe_duration_ms(int episodes) {
  const int steps = std::max(0, std::min(episodes - 1, kMaxBackoffSteps));
  return kSafeBaseMs << steps;
}

CrashRecord CrashCounter::fresh_record() { return CrashRecord(); }

CrashRecord CrashCounter::left_safe(const CrashRecord& r) {
  CrashRecord out;
  out.episodes = r.episodes;
  out.last = r.last;
  return out;
}

BootVerdict CrashCounter::on_boot(const CrashRecord& stored, ResetKind kind, std::optional<int64_t> previousRunMs) {
  BootVerdict v;
  CrashRecord r = stored;
  r.axisMs = std::min(r.axisMs + clamp_run(previousRunMs), 2 * kMaxRunMs);
  r.last = kind;
  // Crashes older than the window (on the running-time axis) no longer count.
  r.crashes.erase(std::remove_if(r.crashes.begin(), r.crashes.end(),
                                 [&](const CrashEntry& e) { return r.axisMs - e.atMs > kWindowMs; }),
                  r.crashes.end());
  const bool abnormal = reset_is_abnormal(kind);
  if (abnormal) {
    r.crashes.push_back({r.axisMs, kind});
    if (r.crashes.size() > static_cast<size_t>(kSafeAfter)) {
      r.crashes.erase(r.crashes.begin(), r.crashes.end() - kSafeAfter);
    }
  }
  v.counted = abnormal;
  if (r.safe) {
    if (abnormal) {
      r.safeSinceMs = r.axisMs;  // safe mode itself is not stable: its time starts again
    } else if (r.axisMs - r.safeSinceMs >= safe_duration_ms(r.episodes)) {
      r = left_safe(r);
      r.last = kind;
      v.leftSafe = true;
    }
  } else if (r.crashes.size() >= static_cast<size_t>(kSafeAfter)) {
    r.safe = true;
    r.safeSinceMs = r.axisMs;
    r.episodes = std::min(r.episodes + 1, 1000);
    v.enteredSafe = true;
    v.rollBack = std::any_of(r.crashes.begin(), r.crashes.end(),
                             [](const CrashEntry& e) { return reset_is_software_fault(e.kind); });
  }
  if (!r.safe && r.crashes.empty()) r.axisMs = 0;  // nothing refers to the axis: start it over
  v.safeMode = r.safe;
  v.record = r;
  return v;
}

CrashCounter::Tick CrashCounter::on_uptime(const CrashRecord& record, int64_t uptimeMs) {
  if (record.safe) {
    return record.axisMs + uptimeMs - record.safeSinceMs >= safe_duration_ms(record.episodes) ? Tick::LeaveSafeMode
                                                                                                : Tick::Nothing;
  }
  const bool somethingCounted = !record.crashes.empty() || record.episodes > 0 || record.axisMs != 0;
  return somethingCounted && uptimeMs >= kStableMs ? Tick::ClearRecord : Tick::Nothing;
}
