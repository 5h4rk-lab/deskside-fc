// The simulator's software updates: the real UpdateFlow with a pretend helper
// and a pretend download, driven by tick(). A check answers after kCheckMs
// with a manifest offering the next patch version (or `manifest` when set);
// an install "downloads" for kDownloadMs, then shows Restarting for
// kRestartMs and comes back up to date with the notice "Updated to X" (the
// simulator cannot restart). Counts what the app asked, for tests.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "update_flow.h"

class MockUpdater : public Updater {
 public:
  static constexpr int64_t kCheckMs = 800;
  static constexpr int64_t kDownloadMs = 6000;
  static constexpr int64_t kRestartMs = 1500;

  explicit MockUpdater(Version current);

  UpdateStatus status() const override { return flow_.status(); }
  void check_now() override;
  void install(const PowerStatus& power) override;
  void settings_opened() override;

  // Advances the pretend check and download to monotonic time `ms`.
  void tick(int64_t ms);

  // Screenshots: a version on offer at once, or an install at `percent`.
  void show_available();
  void show_up_to_date();
  void show_downloading(int percent);

  UpdateFlow& flow() { return flow_; }
  std::string manifest;  // the pretend helper's answer ("" = the next patch version)
  int checks = 0;
  int installs = 0;  // install() calls that started a download
  int installCalls = 0;
  int opens = 0;
  std::vector<PowerStatus> installPower;

 private:
  enum class Job { None, Check, Download, Restart };
  void start_check(bool userAsked);
  std::string answer() const;

  UpdateFlow flow_;
  Version current_;
  Job job_ = Job::None;
  int64_t now_ = 0;
  int64_t jobStart_ = 0;
};
