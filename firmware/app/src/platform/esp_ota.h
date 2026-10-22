// Over-the-air updates on the board: the start-up trial of a newly installed
// image (with the rollback), its confirmation, and the download of a new image
// into the idle app slot.
//
// Rollback. The prebuilt Arduino libraries are built with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE / CONFIG_APP_ROLLBACK_ENABLE, but
// whether the prebuilt bootloader acts on it cannot be relied on, so the
// firmware runs its own trial on top (ui/src/update_flow.h decide_boot):
// before switching, the download writes a trial record (NVS namespace "ota",
// key "ota.trial": slot, starts, version, previous version). ota_boot() runs
// first thing in setup(): it counts every start of the image on trial, and on
// its third unconfirmed start (kMaxTrialBoots = 2) it sets the boot slot back
// to the previous image and restarts. If the bootloader rolled back by itself,
// ota_boot() finds the previous image running and reports it. The image
// confirms itself with ota_confirm() once it is healthy (TrialHealth: drew,
// started Wi-Fi, then the first data or a minute of running), which also
// tells the bootloader (esp_ota_mark_app_valid_cancel_rollback). main.cpp
// defines verifyRollbackLater() so that the Arduino core does not mark a new
// image valid at once. Nothing else in NVS is touched.
//
// Download (EspOta::start): one task on core 0 with an internal-RAM stack
// (flash writes need it), one HTTP(S) session (esp_http_client, server
// certificate checked against the ESP-IDF bundle; plain http only when the
// caller allows it: diagnostics builds with a local test server). The image
// streams into the idle slot (esp_ota_begin with sequential erase) while its
// SHA-256 is computed; the first kImageHeadBytes are checked before anything is
// written (check_image_head: ours, this chip, the announced version); the
// size must match the manifest exactly. Only when the whole image is in, the
// SHA-256 matches and esp_ota_end() validated it, the trial record is written
// and the boot slot switched. Any failure aborts the write and leaves the boot
// slot alone. The caller restarts.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../app_diag.h"
#include "update_check.h"
#include "update_flow.h"

// What ota_boot() found.
struct OtaBoot {
  std::string runningSlot;  // "app0" / "app1"
  Version running;
  BootAction action = BootAction::Normal;
  bool onTrial = false;  // confirm it (ota_confirm) once healthy
  std::string notice;    // RolledBack: for the Settings row
  int imageState = -1;   // esp_ota_img_states_t of the running slot (-1: unknown)
  int boots = 0;         // starts counted on trial
};

// First thing in setup() (after Serial): may switch the boot slot and restart
// (a trial image that failed to start too often); otherwise returns what it
// found and logs one [ota] line.
OtaBoot ota_boot();
// The running image is healthy: no rollback any more. True when done.
bool ota_confirm();
// One [ota] line: running and idle slots, states, the trial record.
void ota_print_state();

class EspOta {
 public:
  enum class State : uint8_t { Idle, Running, Done, Failed };
  static constexpr uint32_t kStackBytes = 10 * 1024;
  static constexpr size_t kChunkBytes = 4096;
  static constexpr int kReadTimeoutMs = 10000;   // one read (or the connect) may take this long
  static constexpr int kMaxStalls = 2;           // read timeouts in a row before giving up
  static constexpr int64_t kTotalLimitMs = 10LL * 60 * 1000;
  static constexpr size_t kMinInternalFree = 60u * 1024u;  // refuse to start below this

  // Starts the download task. False (and error() set) when it cannot start.
  // `allowHttp`: accept an http:// URL (diagnostics builds only).
  bool start(const UpdateInfo& info, const Version& running, bool allowHttp);

  State state() const { return state_.load(); }
  int percent() const { return percent_.load(); }
  UpdateError error() const { return error_.load(); }
  // Measurements of the last run (valid once it ended).
  struct Stats {
    uint32_t bytes = 0;
    uint32_t ms = 0;
    uint32_t handshakeMs = 0;
    size_t internalFreeMin = 0;  // lowest internal free seen while it ran
    size_t internalLargestMin = 0;
    uint32_t stackFree = 0;  // lowest free stack of the task
    const char* why = "";    // a short reason for the log (never shown to the user)
    int espErr = 0;
  };
  Stats stats() const { return stats_; }

 private:
  static void task_main(void* arg);
  void run();
  void fail(UpdateError e, const char* why, int err = 0);

  UpdateInfo info_;
  Version running_;
#if APP_DIAG
  bool allowHttp_ = false;
#else
  static constexpr bool allowHttp_ = false;
#endif
  bool inTask_ = false;  // fail() runs in the download task (set there before run())
  std::atomic<State> state_{State::Idle};
  std::atomic<int> percent_{0};
  std::atomic<UpdateError> error_{UpdateError::None};
  Stats stats_;
  TaskHandle_t task_ = nullptr;
};
