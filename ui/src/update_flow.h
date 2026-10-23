// Software updates, the state machines (pure, shared with the simulator):
//
// - UpdateFlow: what the Settings row shows and when the device checks. The
//   host (firmware: EspUpdater in app_host.cpp; simulator: MockUpdater) does
//   the network and flash work and reports to it.
// - Updater: what the app asks of the host (app.h AppDeps::updater).
// - Trial records and the boot decision: an installed image runs "on trial"
//   until it proves healthy; one that keeps failing to start is rolled back
//   to the previous slot (decide_boot, TrialHealth).
//
// Checks: the first one kFirstCheckMs after start, then every kCheckEveryMs
// (kRetryAfterFailureMs after a failed one); opening Settings checks again
// when the last check is older than kOpenRecheckMs; a tap on the row checks at
// once. Nothing is ever installed without a tap on "Version X available ·
// Install"; checks in the background never show their failures (the row keeps
// what it showed), checks the user asked for do.
#pragma once

#include <stdint.h>

#include <string>

#include "power_service.h"
#include "update_check.h"

class UpdateFlow {
 public:
  static constexpr int64_t kFirstCheckMs = 2LL * 60 * 1000;
  static constexpr int64_t kCheckEveryMs = 6LL * 60 * 60 * 1000;
  static constexpr int64_t kRetryAfterFailureMs = 30LL * 60 * 1000;
  static constexpr int64_t kOpenRecheckMs = 10LL * 60 * 1000;

  UpdateFlow(Version current, ManifestRules rules);

  // The first check runs kFirstCheckMs after this (the host's start time).
  void set_start_ms(int64_t nowMs) { startMs_ = nowMs; }
  // A note from the last start ("Updated to 0.1.1", or a rollback).
  void set_notice(const std::string& text, bool problem);
  // The running image is still on trial (just installed): an install is
  // refused with UpdateError::Unconfirmed until it is confirmed.
  void set_unconfirmed(bool unconfirmed) { unconfirmed_ = unconfirmed; }
  bool unconfirmed() const { return unconfirmed_; }

  // A background check is due at `nowMs` (never while busy or checking).
  bool check_due(int64_t nowMs) const;
  // Settings was opened: a check is wanted (the last one is older than
  // kOpenRecheckMs, or there was none). Never while busy or checking.
  bool check_wanted_on_open(int64_t nowMs) const;
  // A tap may start a check now (not while busy or checking, nor on Available).
  bool can_check() const;

  // The host starts a check. `userAsked`: a tap or opening Settings (its
  // failure shows in the row); background checks fail silently.
  void check_started(int64_t nowMs, bool userAsked);
  // The host could not ask (no network or no valid clock).
  void check_offline(int64_t nowMs, bool userAsked);
  // No helper is set up, so there is nobody to ask: a tap reads "Up to date"
  // (the running version), never an error.
  void check_unconfigured(int64_t nowMs, bool userAsked);
  // The answer: any HTTP status, or a transport error. 200 with a manifest
  // decides (decide()); 404 means nothing is published (up to date); anything
  // else fails the check.
  void check_answered(int64_t nowMs, bool transportError, int status, const std::string& body);

  // A tap on "available · Install": true when the host should start (the
  // phase is Preparing then); false (with the row showing why) when the
  // battery rule or the trial refuses, or nothing is available.
  bool install_requested(const PowerStatus& power);
  void download_progress(int percent);  // Downloading
  void installing();                    // checking the download and switching
  void restarting();                    // the new image is set to boot
  void failed(UpdateError error);       // the install stopped; nothing changed

  UpdateStatus status() const;
  const UpdateInfo& info() const { return info_; }
  bool busy() const;
  int64_t last_check_ms() const { return lastCheckMs_; }

 private:
  Version current_;
  ManifestRules rules_;
  UpdatePhase phase_ = UpdatePhase::Idle;
  UpdatePhase beforeCheck_ = UpdatePhase::Idle;
  UpdateError error_ = UpdateError::None;
  UpdateInfo info_;
  bool haveInfo_ = false;
  int percent_ = 0;
  bool userCheck_ = false;
  bool unconfirmed_ = false;
  int64_t startMs_ = 0;
  int64_t lastCheckMs_ = -1;  // start of the last check, -1: none yet
  bool lastCheckFailed_ = false;
  std::string notice_;
  bool noticeProblem_ = false;
};

// What the app asks of the host. All calls on the UI thread (LVGL task).
class Updater {
 public:
  virtual ~Updater() = default;
  virtual UpdateStatus status() const = 0;
  // A tap on the row while it is not offering an install.
  virtual void check_now() = 0;
  // A tap on "Version X available · Install". `power` is the battery as the
  // UI shows it (the host applies update_power_ok through UpdateFlow).
  virtual void install(const PowerStatus& power) = 0;
  // Settings was opened (UpdateFlow::check_wanted_on_open decides).
  virtual void settings_opened() = 0;
};

// ---- trial of a newly installed image and rollback ---------------------------

// Kept in NVS (namespace "ota", key kTrialKey) while an installed image is on
// trial: the slot it was written to ("app0"/"app1"), how many times it has
// started, its version and the version it replaced.
constexpr const char* kTrialKey = "ota.trial";  // 9 characters (NVS keys: at most 15)
constexpr int kMaxTrialBoots = 2;              // a third start of an unconfirmed image rolls back

struct TrialRecord {
  std::string slot;
  int boots = 0;
  Version version;
  Version from;
};

// "app1,0,0.1.1,0.1.0".
std::string encode_trial(const TrialRecord& r);
bool decode_trial(const std::string& text, TrialRecord& out);

enum class BootAction {
  Normal,      // no trial: nothing to do
  Trial,       // this image is on trial: write the counted record, confirm when healthy
  RollBack,    // on trial and started too often: write the record, boot the other slot, restart
  RolledBack,  // a trial image did not make it and the previous one runs: erase the record, tell the user
};

struct BootDecision {
  BootAction action = BootAction::Normal;
  TrialRecord record;  // Trial / RollBack: the record to write
  bool write = false;  // write `record`
  bool erase = false;  // erase the stored record
  std::string notice;  // RolledBack: what the Settings row says
};

// At start: `stored` is the record's text ("" when there is none), the
// running slot's label and the running version. `trialSlotRejected`: the
// bootloader marked the record's slot aborted or invalid (it was booted and
// failed). A record for another slot that never counted a start (boots 0)
// and was not rejected means the power went between writing the record and
// switching the boot slot: the update simply did not happen (erased quietly,
// no "did not start" notice; the next check offers it again).
BootDecision decide_boot(const std::string& stored, const std::string& runningSlot, const Version& running,
                         bool trialSlotRejected);

// "Updated to 0.1.1": the notice once a trial image is confirmed.
std::string updated_notice(const Version& v);

// Health of an image on trial: the display drew, the UI task runs (the host
// calls ready() from it), Wi-Fi was started; then the first successful data
// fetch, or kHealthyMs of running, confirms it.
class TrialHealth {
 public:
  static constexpr int64_t kHealthyMs = 60000;
  void display_up() { display_ = true; }
  void wifi_attempted() { wifi_ = true; }
  // The host reads both as levels every tick (frames drawn so far > 0, the
  // Wi-Fi service started) and hands them here: once true they stay true, and
  // no single dropped notification can keep a good image unconfirmed.
  void observe(bool displayDrew, bool wifiStarted) {
    if (displayDrew) display_ = true;
    if (wifiStarted) wifi_ = true;
  }
  void data_ok() { data_ = true; }
  bool ready(int64_t uptimeMs) const { return display_ && wifi_ && (data_ || uptimeMs >= kHealthyMs); }

 private:
  bool display_ = false;
  bool wifi_ = false;
  bool data_ = false;
};
