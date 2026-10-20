// What the UI needs from the Wi-Fi hardware. The firmware implements it over
// the ESP32 Wi-Fi driver and NVS; the simulator and the tests use MockWifi.
// Shared with the firmware: no stdio, no SDL, no clock reads.
//
// Contract:
// - Callbacks (scan, connect, connect_saved) may be invoked synchronously,
//   from inside the call, or later from the UI thread. They are never invoked
//   from inside another call to this service. Callers must cope with both.
// - connect() saves the network with its password in the service's own
//   storage when (and only when) it succeeds, replacing an existing entry with
//   the same SSID. connect_saved() joins a saved network with the password the
//   service stored for it. A successful join of a saved network makes it the
//   most recently used.
// - saved() lists the saved SSIDs, most recently used first.
// - The service joins the strongest saved network in range by itself (at
//   boot and when the connection drops). That is the firmware's job, not the
//   UI's.
// - The UI never sees or stores saved passwords. A password typed by the user
//   goes to connect() and nowhere else (no logs, no prefs).
// - forget() removes one saved entry (unknown SSIDs are ignored).
// - At least kMinSavedNetworks networks must be storable. A service may cap
//   the list above that (dropping the least recently used entry when full),
//   but must never silently drop saved entries while it holds fewer than
//   kMinSavedNetworks.
// - Only WPA2-Personal and open networks are supported.
#pragma once

#include <stddef.h>

#include <functional>
#include <string>
#include <vector>

// One network seen by a scan. `rssi` is the signal strength in dBm (for
// example -55; closer to 0 is stronger). Secured means WPA2-Personal.
struct WifiNetwork {
  std::string ssid;
  int rssi = 0;
  bool open = false;
};

struct WifiStatus {
  bool connected = false;
  std::string ssid;  // of the joined network, empty when not connected
  int rssi = 0;      // dBm, when connected
  std::string ip;    // dotted IPv4 address, when connected
};

constexpr size_t kMinSavedNetworks = 8;

class WifiService {
 public:
  virtual ~WifiService() = default;
  // Scans for networks in range. The list may hold duplicates and hidden
  // networks (empty SSID); the UI cleans it up.
  virtual void scan(std::function<void(std::vector<WifiNetwork>)> done) = 0;
  // Joins `ssid` with `password` (empty for an open network); saves it on success.
  virtual void connect(const std::string& ssid, const std::string& password,
                       std::function<void(bool ok)> done) = 0;
  // Joins a saved network with its stored password.
  virtual void connect_saved(const std::string& ssid, std::function<void(bool ok)> done) = 0;
  // Saved SSIDs, most recently used first.
  virtual std::vector<std::string> saved() = 0;
  virtual void forget(const std::string& ssid) = 0;
  virtual WifiStatus status() = 0;
};
