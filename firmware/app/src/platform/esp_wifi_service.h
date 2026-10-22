// The board's WifiService: ui/src/wifi_controller.{h,cpp} (the logic, sim-tested)
// over the ESP-IDF Wi-Fi driver and one NVS blob.
//
// Threads:
// - The LVGL task owns the controller: every WifiService call, pump(), wipe()
//   and describe happen there, and so do all callbacks (done callbacks never
//   run from a driver or worker task). app_host.cpp calls pump() from its
//   10 ms timer.
// - The Wi-Fi worker task (core 0) runs every call that can block: starting
//   the driver (so its interrupts live on core 0, away from the display), scans,
//   joins, and the 2 s signal-strength refresh. Commands reach it through a
//   queue; the ESP-IDF event handlers (system event task, core 0) only push
//   small records into a queue that pump() drains.
// - status() is answered from RAM: the controller's link state plus the signal
//   strength the worker last measured.
// The IDF driver's own Wi-Fi/lwIP tasks are pinned to core 0 by the Arduino
// core's prebuilt configuration (CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0,
// CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0).
//
// Secrets: the typed password goes from the UI to connect() to the worker's
// command record to esp_wifi_set_config() and nowhere else (not logged, not
// printed, not stored by the driver: its own config storage is RAM only). Saved
// passwords live only in the NVS blob "nets" of namespace "wifi", written by
// WifiStore::serialize().
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "app_diag.h"
#include "wifi_controller.h"
#include "wifi_service.h"

class EspWifiService : public WifiService {
 public:
  EspWifiService();
  ~EspWifiService() override;

  // Reads the saved list (NVS must be initialised: construct NvsPrefs first).
  // Call before building the UI, which opens the guided setup when the list is
  // empty. Does not touch the radio.
  void load();
  // Starts the worker on core 0, which brings the driver up. Returns false when
  // the task or the queues cannot be created (the service then reports
  // nothing found / not connected). Call once, after the first frame.
  bool start();
  // Any task: start() succeeded (the worker runs). A level, not an event, so
  // nobody can miss it (the update trial's health check reads it every tick).
  bool started() const;
  // LVGL task, every 10 ms: hands driver events to the controller and runs it.
  void pump();

  // WifiService (LVGL task only)
  void scan(std::function<void(std::vector<WifiNetwork>)> done) override;
  void connect(const std::string& ssid, const std::string& password,
               std::function<void(bool ok)> done) override;
  void connect_saved(const std::string& ssid, std::function<void(bool ok)> done) override;
  std::vector<std::string> saved() override;
  void forget(const std::string& ssid) override;
  WifiStatus status() override;

  // Serial command `wipe-wifi` (LVGL task): forgets every saved network.
  void wipe();
  // Serial command `wifi` (LVGL task): one line of state plus memory, never a password.
  void print_state();

  // Any task: true while the board has an IP address.
  bool link_up() const;

  // Switching off (LVGL task): the worker leaves the network and stops the
  // radio; waits up to `waitMs` for it. Joins and scans fail from then on
  // (until the restart). True when the radio is stopped (or never started).
  bool stop_radio(uint32_t waitMs);

  // Worker stack headroom in bytes (0 before start()).
  size_t worker_stack_free_bytes() const;

#if APP_DIAG
  // Diagnostics builds only (see main.cpp): LVGL task.
  void diag_print_scan();
  void diag_wrong_password_test();
  void diag_timeout_test();
#endif

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
