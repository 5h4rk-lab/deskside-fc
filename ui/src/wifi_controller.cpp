#include "wifi_controller.h"

#include <algorithm>

namespace {

constexpr size_t kWpa2MinPassword = 8;

}  // namespace

bool wifi_reason_is_auth_failure(int reason) {
  // 14 MIC failure, 15 4-way handshake timeout, 23 802.1X failed, 202 auth fail,
  // 204 handshake timeout.
  return reason == 14 || reason == 15 || reason == 23 || reason == 202 || reason == 204;
}

std::string wifi_printable(const std::string& ssid) {
  std::string out = ssid;
  for (char& ch : out) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F) ch = '?';
  }
  return out;
}

WifiController::WifiController(WifiRadio& radio, WifiBlobStorage& storage, std::function<int64_t()> clock,
                               std::function<void(const std::string&)> log, WifiTiming timing)
    : radio_(radio), storage_(storage), clock_(std::move(clock)), log_(std::move(log)), timing_(timing) {}

void WifiController::say(const std::string& line) const {
  if (log_) log_(line);
}

// ---------------------------------------------------------------- storage

void WifiController::load() {
  std::string blob;
  if (storage_.load(blob)) {
    const WifiStore::ParseResult r = store_.parse(blob);
    lastSaved_ = blob;
    lastSavedKnown_ = true;
    say("[wifi] " + std::to_string(store_.size()) + " saved network(s) loaded");
    if (r.skippedBytes > 0) {
      say("[wifi] skipped " + std::to_string(r.skippedBytes) + " damaged byte(s) in the saved list");
      persist();  // rewrite it without the damage
    }
  } else {
    store_.clear();
    lastSaved_.clear();
    lastSavedKnown_ = true;
    say("[wifi] no saved networks");
  }
  if (store_.size() > 0) schedule_auto(0);
}

void WifiController::persist() {
  const std::string blob = store_.serialize();
  if (lastSavedKnown_ && blob == lastSaved_) return;  // unchanged: no flash write
  if (storage_.save(blob)) {
    lastSaved_ = blob;
    lastSavedKnown_ = true;
    say("[wifi] saved list stored: " + std::to_string(store_.size()) + " network(s)");
  } else {
    say("[wifi] could not save the network list; it stays in memory and is written at the next change");
  }
}

// ---------------------------------------------------------------- helpers

std::vector<WifiNetwork> WifiController::ui_list(const std::vector<WifiScanEntry>& results) const {
  std::vector<WifiNetwork> out;
  for (const WifiScanEntry& e : results) {
    if (e.auth == WifiAuth::Unsupported || !wifi_ssid_valid(e.ssid)) continue;
    auto it = std::find_if(out.begin(), out.end(), [&](const WifiNetwork& n) { return n.ssid == e.ssid; });
    if (it == out.end()) {
      WifiNetwork n;
      n.ssid = e.ssid;
      n.rssi = e.rssi;
      n.open = e.auth == WifiAuth::Open;
      out.push_back(n);
    } else if (e.rssi > it->rssi) {
      it->rssi = e.rssi;
      it->open = e.auth == WifiAuth::Open;
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });
  if (out.size() > kWifiMaxScanResults) out.resize(kWifiMaxScanResults);
  return out;
}

// Decides how to join `ssid` from what the last scan saw. `password` is
// cleared for an open network. False for a network type we do not support or
// a passphrase WPA2 cannot use.
bool WifiController::classify(const std::string& ssid, const std::string& password, bool& open) const {
  const WifiScanEntry* best = nullptr;
  bool sawUnsupported = false;
  for (const WifiScanEntry& e : lastScan_) {
    if (e.ssid != ssid) continue;
    if (e.auth == WifiAuth::Unsupported) {
      sawUnsupported = true;
    } else if (best == nullptr || e.rssi > best->rssi) {
      best = &e;
    }
  }
  if (best != nullptr) {
    open = best->auth == WifiAuth::Open;
  } else if (sawUnsupported) {
    return false;  // WEP, WPA-Enterprise, WPA3 only
  } else {
    open = password.empty();  // not seen lately: trust what we were given
  }
  if (open) return true;
  return password.size() >= kWpa2MinPassword;
}

// ---------------------------------------------------------------- joins

void WifiController::begin_join(const std::string& ssid, const std::string& password, bool open, bool user,
                                bool fromStore, std::function<void(bool)> done) {
  Join j;
  j.active = true;
  j.gen = ++genCounter_;
  j.ssid = ssid;
  j.password = open ? std::string() : password;
  j.open = open;
  j.user = user;
  j.fromStore = fromStore;
  j.deadline = now() + timing_.joinTimeoutMs;
  j.done = std::move(done);
  join_ = std::move(j);
  connected_ = false;  // starting a join drops the current link
  status_ = WifiStatus();
  autoPending_ = true;  // a cycle is running: no auto-join steps meanwhile
  autoNextAt_ = -1;
  if (user) autoScan_ = false;
  say("[wifi] joining " + wifi_printable(ssid) + (open ? " (open network)" : ""));
  issue_join();
}

// Hands the attempt to the radio, or, while the driver is not up yet, tries
// again a moment later (the join's own deadline still ends it).
void WifiController::issue_join() {
  if (!radio_.ready()) {
    join_.retryAt = now() + timing_.retryDelayMs;
    return;
  }
  radio_.join(join_.gen, join_.ssid, join_.password);
}

std::function<void(bool)> WifiController::take_join() {
  std::function<void(bool)> d = std::move(join_.done);
  join_ = Join();
  return d;
}

void WifiController::fail_join() {
  const bool user = join_.user;
  const std::string ssid = join_.ssid;
  std::function<void(bool)> d = take_join();
  radio_.disconnect();
  say("[wifi] could not join " + wifi_printable(ssid));
  if (user) {
    autoBackoffIdx_ = 0;
    schedule_auto(0);  // the previous network is rejoined by the auto-join
  } else {
    auto_try_next_candidate();
  }
  if (d) d(false);  // last: the state is consistent when the caller's code runs
}

void WifiController::drop_link() {
  radio_.disconnect();
  connected_ = false;
  status_ = WifiStatus();
}

void WifiController::on_ip(uint32_t gen, const std::string& ip) {
  if (!join_.active) {
    // A new address on the link we hold (DHCP renewal): keep it current.
    if (connected_ && gen == connGen_) status_.ip = ip;
    return;
  }
  if (gen != join_.gen) return;  // an older attempt
  Join j = std::move(join_);
  join_ = Join();
  connected_ = true;
  connGen_ = gen;
  status_.connected = true;
  status_.ssid = j.ssid;
  status_.ip = ip;
  autoPending_ = false;
  autoNextAt_ = -1;
  autoBackoffIdx_ = 0;
  candidates_.clear();
  if (j.fromStore) {
    store_.touch(j.ssid);
  } else {
    store_.add_or_replace(j.ssid, j.password);
  }
  persist();
  say("[wifi] joined " + wifi_printable(j.ssid) + " rssi=" + std::to_string(radio_.rssi()) + " ip=" + ip);
  if (j.done) j.done(true);
}

void WifiController::on_disconnected(uint32_t gen, int reason) {
  if (join_.active) {
    if (gen != join_.gen) return;                 // an older attempt
    if (reason == kWifiReasonAssocLeave) return;  // the old link going away on our request
    if (wifi_reason_is_auth_failure(reason) && ++join_.authFailures >= 2) {
      say("[wifi] authentication failed for " + wifi_printable(join_.ssid) + " (reason " + std::to_string(reason) + ")");
      fail_join();
      return;
    }
    join_.retryAt = now() + timing_.retryDelayMs;  // try again within the window
    return;
  }
  if (connected_ && gen == connGen_) {
    say("[wifi] lost " + wifi_printable(status_.ssid) + " (reason " + std::to_string(reason) + ")");
    connected_ = false;
    status_ = WifiStatus();
    autoBackoffIdx_ = 0;
    schedule_auto(0);
  }
}

// ---------------------------------------------------------------- WifiService

void WifiController::connect(const std::string& ssid, const std::string& password,
                             std::function<void(bool ok)> done) {
  const bool wasJoining = join_.active;
  std::function<void(bool)> replaced = take_join();
  bool open = false;
  const bool ok = wifi_ssid_valid(ssid) && wifi_password_valid(password) && classify(ssid, password, open);
  if (ok) {
    begin_join(ssid, password, open, true, false, std::move(done));
  } else {
    if (wasJoining) {  // the replaced attempt is over; the link is down either way
      radio_.disconnect();
      autoBackoffIdx_ = 0;
      schedule_auto(0);
    }
    say("[wifi] cannot join " + wifi_printable(ssid) + ": not an open or WPA2-Personal network, or an unusable key");
  }
  if (replaced) replaced(false);
  if (!ok && done) done(false);
}

void WifiController::connect_saved(const std::string& ssid, std::function<void(bool ok)> done) {
  const bool wasJoining = join_.active;
  std::function<void(bool)> replaced = take_join();
  std::string password;
  bool open = false;
  const bool ok = store_.password_of(ssid, password) && classify(ssid, password, open);
  if (ok) {
    begin_join(ssid, password, open, true, true, std::move(done));
  } else {
    if (wasJoining) {
      radio_.disconnect();
      autoBackoffIdx_ = 0;
      schedule_auto(0);
    }
    say("[wifi] cannot join saved network " + wifi_printable(ssid));
  }
  if (replaced) replaced(false);
  if (!ok && done) done(false);
}

void WifiController::forget(const std::string& ssid) {
  if (!store_.remove(ssid)) return;
  persist();
  say("[wifi] forgot " + wifi_printable(ssid));
  if (connected_ && status_.ssid == ssid) {
    drop_link();
    autoBackoffIdx_ = 0;
    schedule_auto(0);
  }
  if (join_.active && join_.fromStore && join_.ssid == ssid) {
    std::function<void(bool)> d = take_join();
    radio_.disconnect();
    autoBackoffIdx_ = 0;
    schedule_auto(0);
    // Only a replaced join may be answered from inside a service call: this one
    // is answered by the next tick().
    if (d) deferred_.push_back(std::move(d));
  }
}

WifiStatus WifiController::status() {
  WifiStatus s = status_;
  s.connected = connected_;
  if (connected_) s.rssi = radio_.rssi();
  return s;
}

void WifiController::wipe() {
  store_.clear();
  persist();
  std::function<void(bool)> d = take_join();
  drop_link();
  autoPending_ = false;
  autoNextAt_ = -1;
  autoScan_ = false;
  candidates_.clear();
  say("[wifi] saved networks wiped");
  if (d) deferred_.push_back(std::move(d));  // answered by the next tick()
}

std::string WifiController::describe() const {
  std::string s;
  if (connected_) {
    s = "connected ssid=" + wifi_printable(status_.ssid) + " rssi=" + std::to_string(radio_.rssi()) +
        " ip=" + status_.ip;
  } else if (join_.active) {
    s = "joining ssid=" + wifi_printable(join_.ssid);
  } else {
    s = "not connected";
  }
  return s + " saved=" + std::to_string(store_.size());
}

// ---------------------------------------------------------------- scans

void WifiController::scan(std::function<void(std::vector<WifiNetwork>)> done) {
  if (done) scanWaiters_.push_back(std::move(done));
  if (!scanWanted_) {
    scanWanted_ = true;
    scanWantedAt_ = now();
  }
}

void WifiController::tick_scan() {
  const int64_t t = now();
  if (scanRunning_) {
    std::vector<WifiScanEntry> results;
    const WifiRadio::ScanState st = radio_.poll_scan(results);
    if (st == WifiRadio::ScanState::Done) {
      finish_scan(results, true);
    } else if (st == WifiRadio::ScanState::Failed) {
      finish_scan({}, false);
    } else if (t - scanStartedAt_ > timing_.scanTimeoutMs) {
      radio_.cancel_scan();
      say("[wifi] scan timed out");
      finish_scan({}, false);
    }
    return;
  }
  if (!scanWanted_) return;
  if (join_.active && join_.user) {
    // The radio is busy with the user's join; the scan waits for it (the join
    // ends within its timeout).
    return;
  }
  if (radio_.ready()) {
    if (join_.active) {
      // A user scan outranks a background join: drop it, and let the scan's
      // result restart the auto-join.
      take_join();
      radio_.disconnect();
      autoScan_ = true;
      autoPending_ = true;
    }
    if (radio_.start_scan()) {
      scanRunning_ = true;
      scanStartedAt_ = t;
      return;
    }
  }
  if (t - scanWantedAt_ > timing_.scanTimeoutMs) {
    say("[wifi] could not start a scan");
    finish_scan({}, false);
  }
}

void WifiController::finish_scan(const std::vector<WifiScanEntry>& results, bool ok) {
  scanRunning_ = false;
  scanWanted_ = false;
  if (ok) lastScan_ = results;
  std::vector<std::function<void(std::vector<WifiNetwork>)>> waiters = std::move(scanWaiters_);
  scanWaiters_.clear();
  const bool forAuto = autoScan_;
  autoScan_ = false;
  const std::vector<WifiNetwork> list = ok ? ui_list(results) : std::vector<WifiNetwork>();
  if (ok) say("[wifi] scan: " + std::to_string(list.size()) + " network(s)");
  for (auto& w : waiters) w(list);
  if (forAuto) auto_scan_done(ok);
}

// ---------------------------------------------------------------- auto-join

void WifiController::schedule_auto(int64_t delayMs) {
  autoPending_ = false;
  autoNextAt_ = now() + delayMs;
}

void WifiController::schedule_backoff() {
  const int idx = std::min(autoBackoffIdx_, 3);
  if (autoBackoffIdx_ < 3) ++autoBackoffIdx_;
  schedule_auto(timing_.backoffMs[idx]);
}

void WifiController::auto_scan_done(bool ok) {
  autoPending_ = false;
  if (join_.active || connected_ || store_.size() == 0) {
    autoNextAt_ = -1;  // a user join took over, or there is nothing to join
    return;
  }
  if (!ok) {
    schedule_backoff();
    return;
  }
  candidates_ = store_.ordered_by_signal(ui_list(lastScan_));
  candidateIdx_ = 0;
  if (candidates_.empty()) {
    schedule_backoff();
    return;
  }
  candidateIdx_ = static_cast<size_t>(-1);
  auto_try_next_candidate();
}

// Starts the next saved network of the candidate list, or backs off when the
// list is used up.
void WifiController::auto_try_next_candidate() {
  while (++candidateIdx_ < candidates_.size()) {
    const WifiNetwork c = candidates_[candidateIdx_];
    std::string password;
    if (!store_.password_of(c.ssid, password)) continue;  // forgotten meanwhile
    begin_join(c.ssid, password, c.open, false, true, nullptr);
    return;
  }
  candidates_.clear();
  schedule_backoff();
}

// ---------------------------------------------------------------- tick

void WifiController::tick() {
  const int64_t t = now();

  // Joins cancelled by forget() / wipe(): answered false, once, now that no
  // service call is running. A callback may queue more; they wait for the next tick.
  if (!deferred_.empty()) {
    std::vector<std::function<void(bool)>> due = std::move(deferred_);
    deferred_.clear();
    for (auto& d : due) d(false);
  }

  // A link that vanished without a driver event.
  if (connected_ && !radio_.link_up()) {
    say("[wifi] lost " + wifi_printable(status_.ssid));
    connected_ = false;
    status_ = WifiStatus();
    autoBackoffIdx_ = 0;
    schedule_auto(0);
  }

  // Join timers.
  if (join_.active) {
    if (t >= join_.deadline) {
      say("[wifi] join timed out: " + wifi_printable(join_.ssid));
      fail_join();
    } else if (join_.retryAt != 0 && t >= join_.retryAt) {
      join_.retryAt = 0;
      join_.gen = ++genCounter_;
      issue_join();
    }
  }

  // The next auto-join cycle starts with a scan (shared with a user scan).
  if (!autoPending_ && autoNextAt_ >= 0 && t >= autoNextAt_ && !join_.active && !connected_ &&
      store_.size() > 0 && radio_.ready()) {
    autoPending_ = true;
    autoNextAt_ = -1;
    autoScan_ = true;
    if (!scanWanted_) {
      scanWanted_ = true;
      scanWantedAt_ = t;
    }
  }

  tick_scan();
}
