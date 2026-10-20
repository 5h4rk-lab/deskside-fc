// Scriptable WifiService for the simulator and the tests. Simulator only.
//
// Networks in range, their passwords and the saved list are set by the
// caller. connect() succeeds for an open network in range, or a secured one
// in range with the password given to set_password(); success saves the
// network (most recently used first, same SSID replaced, up to kCapacity
// entries) and makes it the connected one. A failed join keeps the previous
// connection (as the firmware rejoins the strongest known network).
//
// Synchronous by default: callbacks run inside the call. With set_async(true)
// results are queued and delivered by poll() once their latency has passed;
// the state changes (saved list, status) happen at delivery. Passwords are
// never printed.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "wifi_service.h"

class MockWifi : public WifiService {
 public:
  static constexpr size_t kCapacity = 16;
  static constexpr int64_t kDefaultScanMs = 1200;
  static constexpr int64_t kDefaultConnectMs = 1500;

  // WifiService
  void scan(std::function<void(std::vector<WifiNetwork>)> done) override;
  void connect(const std::string& ssid, const std::string& password,
               std::function<void(bool ok)> done) override;
  void connect_saved(const std::string& ssid, std::function<void(bool ok)> done) override;
  std::vector<std::string> saved() override;
  void forget(const std::string& ssid) override;
  WifiStatus status() override;

  // Script
  void set_networks(std::vector<WifiNetwork> networks);
  // The password that joins secured network `ssid`.
  void set_password(const std::string& ssid, const std::string& password);
  // Replaces the saved list (most recently used first), each with the
  // password from set_password() (empty when none was set). Beyond kCapacity
  // the oldest are dropped.
  void set_saved(const std::vector<std::string>& ssids);
  // Connects to the strongest saved network in range at once (what the
  // firmware does at boot). False when none is in range.
  bool auto_join();
  void set_async(bool async) { async_ = async; }
  void set_latency(int64_t scanMs, int64_t connectMs) {
    scanMs_ = scanMs;
    connectMs_ = connectMs;
  }
  // Async mode: advances the mock's clock by `elapsedMs` and delivers every
  // result that is due, in order.
  void poll(int64_t elapsedMs);
  // Async results not delivered yet.
  size_t pending() const { return queue_.size(); }

  // For tests.
  int scanCalls = 0;
  int connectCalls = 0;
  int connectSavedCalls = 0;
  int forgetCalls = 0;
  std::string lastConnectSsid;

 private:
  struct Saved {
    std::string ssid;
    std::string password;
  };
  struct Job {
    int64_t due;
    std::function<void()> run;
  };

  const WifiNetwork* in_range(const std::string& ssid) const;
  std::string password_for(const std::string& ssid) const;
  void remember(const std::string& ssid, const std::string& password);
  void join(const std::string& ssid);
  void deliver(int64_t latency, std::function<void()> run);

  std::vector<WifiNetwork> networks_;
  std::vector<std::pair<std::string, std::string>> passwords_;
  std::vector<Saved> saved_;
  WifiStatus status_;
  bool async_ = false;
  int64_t scanMs_ = kDefaultScanMs;
  int64_t connectMs_ = kDefaultConnectMs;
  int64_t clock_ = 0;
  std::vector<Job> queue_;
};
