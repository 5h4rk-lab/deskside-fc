#include "mock_wifi.h"

#include <algorithm>

namespace {

constexpr const char* kMockIp = "192.168.1.42";

}  // namespace

const WifiNetwork* MockWifi::in_range(const std::string& ssid) const {
  const WifiNetwork* best = nullptr;
  for (const WifiNetwork& n : networks_) {
    if (n.ssid == ssid && (best == nullptr || n.rssi > best->rssi)) best = &n;
  }
  return best;
}

std::string MockWifi::password_for(const std::string& ssid) const {
  for (const auto& p : passwords_) {
    if (p.first == ssid) return p.second;
  }
  return "";
}

void MockWifi::remember(const std::string& ssid, const std::string& password) {
  saved_.erase(std::remove_if(saved_.begin(), saved_.end(),
                              [&](const Saved& s) { return s.ssid == ssid; }),
               saved_.end());
  saved_.insert(saved_.begin(), Saved{ssid, password});
  if (saved_.size() > kCapacity) saved_.resize(kCapacity);  // the least recently used goes
}

void MockWifi::join(const std::string& ssid) {
  const WifiNetwork* n = in_range(ssid);
  status_.connected = true;
  status_.ssid = ssid;
  status_.rssi = n != nullptr ? n->rssi : 0;
  status_.ip = kMockIp;
}

void MockWifi::deliver(int64_t latency, std::function<void()> run) {
  if (!async_) {
    run();
    return;
  }
  queue_.push_back(Job{clock_ + latency, std::move(run)});
}

void MockWifi::scan(std::function<void(std::vector<WifiNetwork>)> done) {
  ++scanCalls;
  deliver(scanMs_, [this, done] {
    if (done) done(networks_);
  });
}

void MockWifi::connect(const std::string& ssid, const std::string& password,
                       std::function<void(bool ok)> done) {
  ++connectCalls;
  lastConnectSsid = ssid;
  deliver(connectMs_, [this, ssid, password, done] {
    const WifiNetwork* n = in_range(ssid);
    const bool ok = n != nullptr && (n->open || (!password_for(ssid).empty() &&
                                                 password == password_for(ssid)));
    if (ok) {
      remember(ssid, n->open ? std::string() : password);
      join(ssid);
    }
    if (done) done(ok);
  });
}

void MockWifi::connect_saved(const std::string& ssid, std::function<void(bool ok)> done) {
  ++connectSavedCalls;
  lastConnectSsid = ssid;
  deliver(connectMs_, [this, ssid, done] {
    auto it = std::find_if(saved_.begin(), saved_.end(),
                           [&](const Saved& s) { return s.ssid == ssid; });
    const WifiNetwork* n = in_range(ssid);
    const bool ok = it != saved_.end() && n != nullptr &&
                    (n->open || it->password == password_for(ssid));
    if (ok) {
      const std::string stored = it->password;  // remember() erases the entry
      remember(ssid, stored);
      join(ssid);
    }
    if (done) done(ok);
  });
}

std::vector<std::string> MockWifi::saved() {
  std::vector<std::string> out;
  out.reserve(saved_.size());
  for (const Saved& s : saved_) out.push_back(s.ssid);
  return out;
}

void MockWifi::forget(const std::string& ssid) {
  ++forgetCalls;
  saved_.erase(std::remove_if(saved_.begin(), saved_.end(),
                              [&](const Saved& s) { return s.ssid == ssid; }),
               saved_.end());
  if (status_.connected && status_.ssid == ssid) status_ = WifiStatus{};
}

WifiStatus MockWifi::status() { return status_; }

void MockWifi::set_networks(std::vector<WifiNetwork> networks) { networks_ = std::move(networks); }

void MockWifi::set_password(const std::string& ssid, const std::string& password) {
  for (auto& p : passwords_) {
    if (p.first == ssid) {
      p.second = password;
      return;
    }
  }
  passwords_.emplace_back(ssid, password);
}

void MockWifi::set_saved(const std::vector<std::string>& ssids) {
  saved_.clear();
  for (const std::string& s : ssids) {
    if (saved_.size() == kCapacity) break;
    saved_.push_back(Saved{s, password_for(s)});
  }
}

bool MockWifi::auto_join() {
  const WifiNetwork* best = nullptr;
  for (const Saved& s : saved_) {
    const WifiNetwork* n = in_range(s.ssid);
    if (n != nullptr && (best == nullptr || n->rssi > best->rssi)) best = n;
  }
  if (best == nullptr) return false;
  join(best->ssid);
  return true;
}

void MockWifi::poll(int64_t elapsedMs) {
  clock_ += elapsedMs;
  // Due jobs in order; a job may queue another (delivered on a later poll).
  std::vector<Job> due;
  std::vector<Job> later;
  for (Job& j : queue_) (j.due <= clock_ ? due : later).push_back(std::move(j));
  queue_ = std::move(later);
  std::stable_sort(due.begin(), due.end(), [](const Job& a, const Job& b) { return a.due < b.due; });
  for (Job& j : due) j.run();
}
