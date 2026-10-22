#include "update_flow.h"

#include <stdlib.h>

#include <utility>

#include <vector>

UpdateFlow::UpdateFlow(Version current, ManifestRules rules) : current_(current), rules_(std::move(rules)) {}

void UpdateFlow::set_notice(const std::string& text, bool problem) {
  notice_ = text;
  noticeProblem_ = problem;
}

bool UpdateFlow::busy() const {
  return phase_ == UpdatePhase::Preparing || phase_ == UpdatePhase::Downloading ||
         phase_ == UpdatePhase::Installing || phase_ == UpdatePhase::Restarting;
}

bool UpdateFlow::check_due(int64_t nowMs) const {
  if (busy() || phase_ == UpdatePhase::Checking) return false;
  if (lastCheckMs_ < 0) return nowMs - startMs_ >= kFirstCheckMs;
  return nowMs - lastCheckMs_ >= (lastCheckFailed_ ? kRetryAfterFailureMs : kCheckEveryMs);
}

bool UpdateFlow::check_wanted_on_open(int64_t nowMs) const {
  if (busy() || phase_ == UpdatePhase::Checking) return false;
  return lastCheckMs_ < 0 || nowMs - lastCheckMs_ >= kOpenRecheckMs;
}

bool UpdateFlow::can_check() const {
  return !busy() && phase_ != UpdatePhase::Checking && phase_ != UpdatePhase::Available;
}

void UpdateFlow::check_started(int64_t nowMs, bool userAsked) {
  if (busy() || phase_ == UpdatePhase::Checking) return;
  beforeCheck_ = phase_;
  phase_ = UpdatePhase::Checking;
  userCheck_ = userAsked;
  lastCheckMs_ = nowMs;
}

void UpdateFlow::check_offline(int64_t nowMs, bool userAsked) {
  if (busy()) return;
  lastCheckMs_ = nowMs;
  lastCheckFailed_ = true;
  if (userAsked) {
    phase_ = UpdatePhase::Failed;
    error_ = UpdateError::Offline;
  } else if (phase_ == UpdatePhase::Checking) {
    phase_ = beforeCheck_;
  }
}

void UpdateFlow::check_answered(int64_t nowMs, bool transportError, int status, const std::string& body) {
  (void)nowMs;
  if (phase_ != UpdatePhase::Checking) return;
  UpdateError error = UpdateError::None;
  UpdatePhase next = UpdatePhase::UpToDate;
  if (transportError) {
    error = UpdateError::CheckFailed;
  } else if (status == 404) {
    next = UpdatePhase::UpToDate;  // nothing published yet
  } else if (status != 200) {
    error = UpdateError::CheckFailed;
  } else {
    UpdateInfo info;
    switch (decide(current_, body, rules_, &info)) {
      case UpdateDecision::Invalid: error = UpdateError::BadManifest; break;
      case UpdateDecision::UpToDate: next = UpdatePhase::UpToDate; break;
      case UpdateDecision::Available:
        info_ = info;
        haveInfo_ = true;
        next = UpdatePhase::Available;
        break;
    }
  }
  lastCheckFailed_ = error != UpdateError::None;
  if (error == UpdateError::None) {
    if (next != UpdatePhase::Available) haveInfo_ = false;
    phase_ = next;
  } else if (userCheck_) {
    phase_ = UpdatePhase::Failed;
    error_ = error;
  } else {
    phase_ = beforeCheck_;
  }
}

bool UpdateFlow::install_requested(const PowerStatus& power) {
  if (phase_ != UpdatePhase::Available || !haveInfo_) return false;
  if (unconfirmed_) {
    phase_ = UpdatePhase::Failed;
    error_ = UpdateError::Unconfirmed;
    return false;
  }
  if (!update_power_ok(power)) {
    phase_ = UpdatePhase::Failed;
    error_ = UpdateError::LowBattery;
    return false;
  }
  phase_ = UpdatePhase::Preparing;
  percent_ = 0;
  error_ = UpdateError::None;
  return true;
}

void UpdateFlow::download_progress(int percent) {
  if (phase_ != UpdatePhase::Preparing && phase_ != UpdatePhase::Downloading) return;
  phase_ = UpdatePhase::Downloading;
  percent_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

void UpdateFlow::installing() {
  if (!busy()) return;
  phase_ = UpdatePhase::Installing;
}

void UpdateFlow::restarting() {
  if (!busy()) return;
  phase_ = UpdatePhase::Restarting;
}

void UpdateFlow::failed(UpdateError error) {
  if (!busy()) return;
  phase_ = UpdatePhase::Failed;
  error_ = error;
}

UpdateStatus UpdateFlow::status() const {
  UpdateStatus s;
  s.phase = phase_;
  s.current = current_;
  if (haveInfo_) s.available = info_.version;
  s.percent = percent_;
  s.error = phase_ == UpdatePhase::Failed ? error_ : UpdateError::None;
  s.notice = notice_;
  s.noticeIsProblem = noticeProblem_;
  return s;
}

// ---- trial records -------------------------------------------------------------

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t p = s.find(sep, start);
    out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
    if (p == std::string::npos) return out;
    start = p + 1;
  }
}

}  // namespace

std::string encode_trial(const TrialRecord& r) {
  return r.slot + "," + std::to_string(r.boots) + "," + version_text(r.version) + "," + version_text(r.from);
}

bool decode_trial(const std::string& text, TrialRecord& out) {
  const std::vector<std::string> f = split(text, ',');
  if (f.size() != 4) return false;
  TrialRecord r;
  if (f[0] != "app0" && f[0] != "app1") return false;
  r.slot = f[0];
  if (f[1].empty() || f[1].size() > 2 || f[1].find_first_not_of("0123456789") != std::string::npos) return false;
  r.boots = atoi(f[1].c_str());
  if (!parse_version(f[2], r.version) || !parse_version(f[3], r.from)) return false;
  out = r;
  return true;
}

BootDecision decide_boot(const std::string& stored, const std::string& runningSlot, const Version& running) {
  BootDecision d;
  if (stored.empty()) return d;
  TrialRecord r;
  if (!decode_trial(stored, r)) {
    d.erase = true;
    return d;
  }
  if (r.slot != runningSlot) {
    d.action = BootAction::RolledBack;
    d.erase = true;
    d.notice = "Update to " + version_text(r.version) + " did not start. Kept " + version_text(running) + ".";
    return d;
  }
  if (r.version != running) {  // the slot was rewritten by other means (a USB flash)
    d.erase = true;
    return d;
  }
  r.boots += 1;
  d.record = r;
  d.write = true;
  d.action = r.boots > kMaxTrialBoots ? BootAction::RollBack : BootAction::Trial;
  return d;
}

std::string updated_notice(const Version& v) { return "Updated to " + version_text(v); }
