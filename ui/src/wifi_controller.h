// The Wi-Fi logic behind the firmware's WifiService: scans, joins, the saved
// list, auto-join with back-off, timeouts. Pure and single-threaded: no ESP
// types, no stdio, no clock reads of its own, no locks. The firmware wraps a
// real radio around it (esp_wifi_service.cpp), the tests drive it with a fake
// one (sim/tests/wifi_controller_test.cpp).
//
// Threading: every method runs on ONE thread (the firmware's LVGL task). The
// radio's driver events are queued by the wrapper and handed over with
// on_ip() and on_disconnected(), from the same thread, between ticks. All
// WifiService callbacks are therefore invoked on that thread, from inside
// tick(), on_ip(), on_disconnected() or the service call itself, with no lock
// held: status() and saved() are plain RAM reads and are safe inside them.
//
// What it adds to the WifiService contract (wifi_service.h):
// - Callbacks: a replaced join is answered false from inside the replacing
//   call, as the contract allows. A join cancelled by forget() or wipe() is
//   answered by the next tick(), never from inside those calls.
// - A join requested before the radio is ready waits for it (retrying every
//   second) and ends at its 15 s deadline like any other.
// - Nothing blocks: scan(), connect() and the others only change state and
//   start asynchronous radio work. tick() (call it every 10 to 50 ms) polls
//   the scan, drives the timers and starts the next step.
// - A join waits at most kJoinTimeoutMs for an IP address. A wrong password
//   is reported as soon as the driver has failed authentication twice.
// - Auto-join (boot, and whenever the link drops): scan, join the strongest
//   saved network in range; when none is available or all fail, try again
//   after 5, 10, 30, then every 60 s (each attempt scans again). It never
//   runs while a join started by connect() / connect_saved() is pending, and
//   any such call (or a user scan) cancels an auto-join in progress.
// - Networks that are neither open nor WPA/WPA2-Personal are hidden from scan
//   results and rejected by connect() (false at once).
// - Saved networks are written to the storage only when the list changed.
//   A failed write is logged and retried at the next change; the list in RAM
//   stays authoritative.
// - The log callback receives SSIDs, signal strengths and addresses only,
//   never a password.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "wifi_service.h"
#include "wifi_store.h"

enum class WifiAuth { Open, Wpa2Personal, Unsupported };

// One access point from the radio's scan.
struct WifiScanEntry {
  std::string ssid;
  int rssi = 0;
  WifiAuth auth = WifiAuth::Open;
};

// What the controller needs from the radio. Every call must return at once.
class WifiRadio {
 public:
  enum class ScanState { Running, Done, Failed };

  virtual ~WifiRadio() = default;
  // False until the driver is up (start_scan and join are not called before).
  virtual bool ready() = 0;
  // Starts an asynchronous scan. False: it could not start now.
  virtual bool start_scan() = 0;
  // Running, or Done with `out` filled (the radio frees its copy), or Failed.
  virtual ScanState poll_scan(std::vector<WifiScanEntry>& out) = 0;
  virtual void cancel_scan() = 0;
  // Starts joining `ssid` (empty password: open network), dropping any link or
  // attempt first. Driver events that follow carry `gen` (on_ip, on_disconnected).
  virtual void join(uint32_t gen, const std::string& ssid, const std::string& password) = 0;
  // Drops the link and any attempt in progress.
  virtual void disconnect() = 0;
  // True while the link has an IP address (updated by the driver's events).
  virtual bool link_up() = 0;
  // Signal strength of the joined network in dBm, refreshed in the background
  // (0 when unknown).
  virtual int rssi() = 0;
};

// Where the serialized list lives (one NVS blob in the firmware).
class WifiBlobStorage {
 public:
  virtual ~WifiBlobStorage() = default;
  // False when nothing is stored (or it cannot be read).
  virtual bool load(std::string& out) = 0;
  // Stores `data`; an empty string removes the entry. False on failure.
  virtual bool save(const std::string& data) = 0;
};

struct WifiTiming {
  int64_t joinTimeoutMs = 15000;
  int64_t scanTimeoutMs = 12000;
  int64_t retryDelayMs = 1000;                   // between driver attempts inside one join
  int64_t backoffMs[4] = {5000, 10000, 30000, 60000};
};

constexpr size_t kWifiMaxScanResults = 30;
// Disconnect reasons the controller treats specially (ESP-IDF numbering).
constexpr int kWifiReasonAssocLeave = 8;       // we dropped the link ourselves
bool wifi_reason_is_auth_failure(int reason);  // wrong password or handshake failure

// An SSID safe to print: control characters become '?'.
std::string wifi_printable(const std::string& ssid);

class WifiController : public WifiService {
 public:
  WifiController(WifiRadio& radio, WifiBlobStorage& storage, std::function<int64_t()> clock,
                 std::function<void(const std::string&)> log, WifiTiming timing = WifiTiming());

  // Reads the saved list from the storage and, when it holds networks, plans
  // the boot auto-join. Call once before use.
  void load();

  // Timers, scan polling, the next auto-join step. Call every 10 to 50 ms.
  void tick();
  // The driver got an IP address / lost the link (reason is the driver's
  // code), for the attempt numbered `gen` (the radio echoes the number passed
  // to join()). Events of older attempts are ignored.
  void on_ip(uint32_t gen, const std::string& ip);
  void on_disconnected(uint32_t gen, int reason);

  // Forgets every saved network and drops the link.
  void wipe();
  // "connected to <ssid> rssi=<n> ip=<ip>, <n> saved" or "not connected, ..."; no passwords.
  std::string describe() const;

  // WifiService
  void scan(std::function<void(std::vector<WifiNetwork>)> done) override;
  void connect(const std::string& ssid, const std::string& password,
               std::function<void(bool ok)> done) override;
  void connect_saved(const std::string& ssid, std::function<void(bool ok)> done) override;
  std::vector<std::string> saved() override { return store_.ssids(); }
  void forget(const std::string& ssid) override;
  WifiStatus status() override;

  // For tests and the status line.
  bool joining() const { return join_.active; }
  bool joining_for_user() const { return join_.active && join_.user; }
  size_t saved_count() const { return store_.size(); }

 private:
  struct Join {
    bool active = false;
    uint32_t gen = 0;
    std::string ssid;
    std::string password;
    bool open = false;
    bool user = false;       // started by connect() / connect_saved(); else the auto-join
    bool fromStore = false;  // the password came from the saved list (touch, no re-save)
    int64_t deadline = 0;
    int64_t retryAt = 0;     // 0: none
    int authFailures = 0;
    std::function<void(bool)> done;
  };

  int64_t now() const { return clock_(); }
  void say(const std::string& line) const;
  void persist();

  // Joins
  void begin_join(const std::string& ssid, const std::string& password, bool open, bool user,
                  bool fromStore, std::function<void(bool)> done);
  // Cancels the join in progress (radio untouched) and returns its callback
  // (null for an auto-join), to be called once the caller's state is consistent.
  std::function<void(bool)> take_join();
  void fail_join();
  void issue_join();
  bool classify(const std::string& ssid, const std::string& password, bool& open) const;

  // Scans
  void tick_scan();
  void finish_scan(const std::vector<WifiScanEntry>& results, bool ok);
  std::vector<WifiNetwork> ui_list(const std::vector<WifiScanEntry>& results) const;

  // Auto-join
  void schedule_auto(int64_t delayMs);
  void schedule_backoff();
  void auto_scan_done(bool ok);
  void auto_try_next_candidate();
  void drop_link();

  WifiRadio& radio_;
  WifiBlobStorage& storage_;
  std::function<int64_t()> clock_;
  std::function<void(const std::string&)> log_;
  WifiTiming timing_;

  WifiStore store_;
  std::string lastSaved_;
  bool lastSavedKnown_ = false;

  // Link
  bool connected_ = false;
  uint32_t connGen_ = 0;
  WifiStatus status_;
  uint32_t genCounter_ = 0;
  Join join_;
  // Callbacks of joins cancelled by forget() / wipe(), run by the next tick().
  std::vector<std::function<void(bool)>> deferred_;

  // Scan
  std::vector<std::function<void(std::vector<WifiNetwork>)>> scanWaiters_;
  bool scanWanted_ = false;
  bool scanRunning_ = false;
  bool autoScan_ = false;  // the pending scan also serves the auto-join
  int64_t scanWantedAt_ = 0;
  int64_t scanStartedAt_ = 0;
  std::vector<WifiScanEntry> lastScan_;

  // Auto-join
  bool autoPending_ = false;  // a cycle (scan or join) is in progress
  int64_t autoNextAt_ = -1;   // -1: none planned
  int autoBackoffIdx_ = 0;
  std::vector<WifiNetwork> candidates_;
  size_t candidateIdx_ = 0;
};
