#include "mock_updater.h"

namespace {

ManifestRules rules() { return ManifestRules(); }

}  // namespace

MockUpdater::MockUpdater(Version current) : flow_(current, rules()), current_(current) {}

std::string MockUpdater::answer() const {
  if (!manifest.empty()) return manifest;
  Version next = current_;
  next.patch += 1;
  const std::string v = version_text(next);
  return "{\"version\":\"" + v + "\",\"url\":\"https://" + std::string(kDefaultHelperHost) + "/firmware/" + v +
         ".bin\",\"sha256\":\"" + std::string(64, 'a') + "\",\"size\":1634512,\"notes\":\"Simulator\"}";
}

void MockUpdater::start_check(bool userAsked) {
  ++checks;
  flow_.check_started(now_, userAsked);
  job_ = Job::Check;
  jobStart_ = now_;
}

void MockUpdater::check_now() {
  if (flow_.can_check()) start_check(true);
}

void MockUpdater::install(const PowerStatus& power) {
  ++installCalls;
  installPower.push_back(power);
  if (!flow_.install_requested(power)) return;
  ++installs;
  job_ = Job::Download;
  jobStart_ = now_;
}

void MockUpdater::settings_opened() {
  ++opens;
  if (flow_.check_wanted_on_open(now_)) start_check(true);
}

void MockUpdater::tick(int64_t ms) {
  now_ = ms;
  const int64_t t = now_ - jobStart_;
  switch (job_) {
    case Job::None:
      if (flow_.check_due(now_)) start_check(false);
      break;
    case Job::Check:
      if (t >= kCheckMs) {
        job_ = Job::None;
        flow_.check_answered(now_, false, 200, answer());
      }
      break;
    case Job::Download:
      if (t < kDownloadMs) {
        flow_.download_progress(static_cast<int>(t * 100 / kDownloadMs));
      } else {
        flow_.installing();
        flow_.restarting();
        job_ = Job::Restart;
        jobStart_ = now_;
      }
      break;
    case Job::Restart:
      if (t >= kRestartMs) {
        // "Restarted" into the new version.
        const Version v = flow_.info().version;
        current_ = v;
        flow_ = UpdateFlow(v, rules());
        flow_.set_start_ms(now_);
        flow_.set_notice(updated_notice(v), false);
        job_ = Job::None;
      }
      break;
  }
}

void MockUpdater::show_available() {
  flow_.check_started(now_, true);
  flow_.check_answered(now_, false, 200, answer());
  job_ = Job::None;
}

void MockUpdater::show_up_to_date() {
  flow_.check_started(now_, true);
  flow_.check_answered(now_, false, 404, "");  // nothing newer published
  job_ = Job::None;
}

void MockUpdater::show_downloading(int percent) {
  show_available();
  PowerStatus usb;
  if (flow_.install_requested(usb)) flow_.download_progress(percent);
  job_ = Job::None;  // frozen for the snapshot
}
