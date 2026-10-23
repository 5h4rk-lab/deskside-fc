// update_flow: check timing, the row's phases, the install path and its
// refusals; trial records, the boot decision and the health check.
#include <doctest/doctest.h>

#include <string>

#include "update_flow.h"

namespace {

Version V(int a, int b, int c) {
  Version v;
  v.major = a;
  v.minor = b;
  v.patch = c;
  return v;
}

const std::string kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string manifest(const std::string& version) {
  return "{\"version\":\"" + version + "\",\"url\":\"https://soccer-display-helper.vercel.app/firmware/" + version +
         ".bin\",\"sha256\":\"" + kHash + "\",\"size\":1634512}";
}

PowerStatus battery(int percent, bool charging = false) {
  PowerStatus p;
  p.present = true;
  p.percent = percent;
  p.charging = charging;
  return p;
}

constexpr int64_t kMin = 60 * 1000;

UpdateFlow available_flow() {
  UpdateFlow f(V(0, 1, 0), ManifestRules());
  f.check_started(1000, true);
  f.check_answered(1500, false, 200, manifest("0.1.1"));
  return f;
}

}  // namespace

TEST_CASE("flow: starts idle, the first background check 2 minutes after start") {
  UpdateFlow f(V(0, 1, 0), ManifestRules());
  f.set_start_ms(5000);
  CHECK(f.status().phase == UpdatePhase::Idle);
  CHECK(f.status().current == V(0, 1, 0));
  CHECK_FALSE(f.check_due(5000));
  CHECK_FALSE(f.check_due(5000 + 2 * kMin - 1));
  CHECK(f.check_due(5000 + 2 * kMin));
}

TEST_CASE("flow: background checks every 6 hours, 30 minutes after a failure") {
  UpdateFlow f(V(0, 1, 0), ManifestRules());
  const int64_t t = 3 * kMin;
  f.check_started(t, false);
  CHECK(f.status().phase == UpdatePhase::Checking);
  CHECK_FALSE(f.check_due(t + 1));
  f.check_answered(t + 500, false, 200, manifest("0.1.0"));
  CHECK(f.status().phase == UpdatePhase::UpToDate);
  CHECK_FALSE(f.check_due(t + 6 * 60 * kMin - 1));
  CHECK(f.check_due(t + 6 * 60 * kMin));

  const int64_t t2 = t + 6 * 60 * kMin;
  f.check_started(t2, false);
  f.check_answered(t2 + 100, true, 0, "");
  // A background failure keeps what the row showed.
  CHECK(f.status().phase == UpdatePhase::UpToDate);
  CHECK_FALSE(f.check_due(t2 + 30 * kMin - 1));
  CHECK(f.check_due(t2 + 30 * kMin));
}

TEST_CASE("flow: opening Settings checks when the last check is older than 10 minutes") {
  UpdateFlow f(V(0, 1, 0), ManifestRules());
  CHECK(f.check_wanted_on_open(0));  // never checked
  f.check_started(1000, true);
  CHECK_FALSE(f.check_wanted_on_open(1000));  // checking
  f.check_answered(1200, false, 200, manifest("0.1.0"));
  CHECK_FALSE(f.check_wanted_on_open(1000 + 10 * kMin - 1));
  CHECK(f.check_wanted_on_open(1000 + 10 * kMin));
}

TEST_CASE("flow: a newer manifest offers it; same or older is up to date; 404 is up to date") {
  UpdateFlow f = available_flow();
  CHECK(f.status().phase == UpdatePhase::Available);
  CHECK(f.status().available == V(0, 1, 1));
  CHECK(f.info().sizeBytes == 1634512);
  CHECK_FALSE(f.can_check());  // a tap now installs instead

  UpdateFlow g(V(0, 1, 0), ManifestRules());
  g.check_started(0, true);
  g.check_answered(10, false, 200, manifest("0.0.9"));
  CHECK(g.status().phase == UpdatePhase::UpToDate);
  g.check_started(20, true);
  g.check_answered(30, false, 404, "{\"error\":\"no firmware published\"}");
  CHECK(g.status().phase == UpdatePhase::UpToDate);
}

TEST_CASE("flow: no helper set up: a tap reads up to date, never an error") {
  UpdateFlow f(V(0, 1, 0), ManifestRules());
  f.check_unconfigured(1000, true);
  const UpdateStatus s = f.status();
  CHECK(s.phase == UpdatePhase::UpToDate);
  CHECK_FALSE(update_status_problem(s));
  CHECK(update_status_text(s) == "Up to date (0.1.0)");
}

TEST_CASE("flow: failures of a check the user asked for show in the row") {
  UpdateFlow f(V(0, 1, 0), ManifestRules());
  f.check_started(0, true);
  f.check_answered(10, true, 0, "");
  CHECK(f.status().phase == UpdatePhase::Failed);
  CHECK(f.status().error == UpdateError::CheckFailed);
  CHECK(f.can_check());

  f.check_started(20, true);
  f.check_answered(30, false, 401, "{\"error\":\"unauthorized\"}");
  CHECK(f.status().error == UpdateError::CheckFailed);

  f.check_started(40, true);
  f.check_answered(50, false, 200, "{\"version\":\"0.1.1\",\"url\":\"https://evil.example.com/a.bin\"}");
  CHECK(f.status().error == UpdateError::BadManifest);

  f.check_offline(60, true);
  CHECK(f.status().error == UpdateError::Offline);

  // Silent in the background: the row keeps the error the user saw last.
  UpdateFlow g(V(0, 1, 0), ManifestRules());
  g.check_offline(60, false);
  CHECK(g.status().phase == UpdatePhase::Idle);
  CHECK(g.check_due(60 + 30 * kMin));
  CHECK_FALSE(g.check_due(60 + 30 * kMin - 1));
}

TEST_CASE("flow: the install needs a tap on Available and the battery rule") {
  UpdateFlow none(V(0, 1, 0), ManifestRules());
  CHECK_FALSE(none.install_requested(battery(90)));  // nothing available
  CHECK(none.status().phase == UpdatePhase::Idle);

  UpdateFlow f = available_flow();
  CHECK_FALSE(f.install_requested(battery(29)));
  CHECK(f.status().phase == UpdatePhase::Failed);
  CHECK(f.status().error == UpdateError::LowBattery);
  CHECK(f.can_check());

  UpdateFlow g = available_flow();
  CHECK(g.install_requested(battery(29, true)));  // charging
  CHECK(g.status().phase == UpdatePhase::Preparing);
  CHECK(g.busy());
  CHECK_FALSE(g.can_check());
  CHECK_FALSE(g.check_due(100 * 60 * kMin));
  CHECK_FALSE(g.check_wanted_on_open(100 * 60 * kMin));
  CHECK_FALSE(g.install_requested(battery(90)));  // already installing

  UpdateFlow h = available_flow();
  PowerStatus usb;  // no battery: the board runs from USB
  CHECK(h.install_requested(usb));
}

TEST_CASE("flow: an image still on trial refuses a new install") {
  UpdateFlow f = available_flow();
  f.set_unconfirmed(true);
  CHECK_FALSE(f.install_requested(battery(90)));
  CHECK(f.status().error == UpdateError::Unconfirmed);
  f.set_unconfirmed(false);
  f.check_started(10 * kMin, true);
  f.check_answered(10 * kMin + 1, false, 200, manifest("0.1.1"));
  CHECK(f.install_requested(battery(90)));
}

TEST_CASE("flow: download, install, restart; a failure ends the install and allows a new check") {
  UpdateFlow f = available_flow();
  REQUIRE(f.install_requested(battery(80)));
  f.download_progress(0);
  CHECK(f.status().phase == UpdatePhase::Downloading);
  f.download_progress(42);
  CHECK(f.status().percent == 42);
  CHECK(update_status_text(f.status()) == "Downloading 42%");
  f.download_progress(150);
  CHECK(f.status().percent == 100);
  f.download_progress(-3);
  CHECK(f.status().percent == 0);
  f.installing();
  CHECK(f.status().phase == UpdatePhase::Installing);
  f.restarting();
  CHECK(f.status().phase == UpdatePhase::Restarting);
  CHECK(f.busy());

  UpdateFlow g = available_flow();
  REQUIRE(g.install_requested(battery(80)));
  g.download_progress(10);
  g.failed(UpdateError::Damaged);
  CHECK(g.status().phase == UpdatePhase::Failed);
  CHECK(g.status().error == UpdateError::Damaged);
  CHECK_FALSE(g.busy());
  CHECK(g.can_check());
  // Progress reports after the end change nothing.
  g.download_progress(50);
  CHECK(g.status().phase == UpdatePhase::Failed);
}

TEST_CASE("flow: the notice shows while idle or up to date") {
  UpdateFlow f(V(0, 1, 1), ManifestRules());
  f.set_notice(updated_notice(V(0, 1, 1)), false);
  CHECK(update_status_text(f.status()) == "Updated to 0.1.1");
  CHECK_FALSE(update_status_problem(f.status()));
  f.check_started(0, true);
  f.check_answered(1, false, 200, manifest("0.1.1"));
  CHECK(update_status_text(f.status()) == "Updated to 0.1.1");
}

TEST_CASE("trial record: round trip and refusal of garbage") {
  TrialRecord r;
  r.slot = "app1";
  r.boots = 2;
  r.version = V(0, 1, 1);
  r.from = V(0, 1, 0);
  CHECK(encode_trial(r) == "app1,2,0.1.1,0.1.0");
  TrialRecord back;
  REQUIRE(decode_trial(encode_trial(r), back));
  CHECK(back.slot == "app1");
  CHECK(back.boots == 2);
  CHECK(back.version == V(0, 1, 1));
  CHECK(back.from == V(0, 1, 0));
  const char* bad[] = {"", "app1", "app1,2,0.1.1", "app2,0,0.1.1,0.1.0", "app1,x,0.1.1,0.1.0", "app1,-1,0.1.1,0.1.0",
                       "app1,0,0.1,0.1.0", "app1,0,0.1.1,0.1.0,extra", "app1,100,0.1.1,0.1.0"};
  for (const char* b : bad) CHECK_MESSAGE(!decode_trial(b, back), b);
  CHECK(std::string(kTrialKey).size() <= 15);
}

TEST_CASE("boot: no record is a normal start; a broken record is erased") {
  BootDecision d = decide_boot("", "app0", V(0, 1, 0), false);
  CHECK(d.action == BootAction::Normal);
  CHECK_FALSE(d.write);
  CHECK_FALSE(d.erase);
  d = decide_boot("nonsense", "app0", V(0, 1, 0), false);
  CHECK(d.action == BootAction::Normal);
  CHECK(d.erase);
}

TEST_CASE("boot: a trial image is counted, and rolled back on its third start") {
  TrialRecord r;
  r.slot = "app1";
  r.boots = 0;
  r.version = V(0, 1, 1);
  r.from = V(0, 1, 0);
  BootDecision d = decide_boot(encode_trial(r), "app1", V(0, 1, 1), false);
  CHECK(d.action == BootAction::Trial);
  CHECK(d.write);
  CHECK(d.record.boots == 1);

  d = decide_boot(encode_trial(d.record), "app1", V(0, 1, 1), false);
  CHECK(d.action == BootAction::Trial);
  CHECK(d.record.boots == 2);

  d = decide_boot(encode_trial(d.record), "app1", V(0, 1, 1), false);
  CHECK(d.action == BootAction::RollBack);
  CHECK(d.write);
  CHECK(d.record.boots == 3);
}

TEST_CASE("boot: back on the old slot after a failed trial: erase and tell the user") {
  TrialRecord r;
  r.slot = "app1";
  r.boots = 3;
  r.version = V(0, 1, 2);
  r.from = V(0, 1, 1);
  BootDecision d = decide_boot(encode_trial(r), "app0", V(0, 1, 1), false);
  CHECK(d.action == BootAction::RolledBack);
  CHECK(d.erase);
  CHECK(d.notice == "Update to 0.1.2 did not start. Kept 0.1.1.");
  // Rolled back by the bootloader after one start: same outcome.
  r.boots = 1;
  d = decide_boot(encode_trial(r), "app0", V(0, 1, 1), false);
  CHECK(d.action == BootAction::RolledBack);
  // Rejected by the bootloader before our code ever counted a start: same.
  r.boots = 0;
  d = decide_boot(encode_trial(r), "app0", V(0, 1, 1), true);
  CHECK(d.action == BootAction::RolledBack);
  CHECK(d.notice == "Update to 0.1.2 did not start. Kept 0.1.1.");
}

TEST_CASE("boot: power cut between the trial record and the slot switch: quietly forgotten, no notice") {
  TrialRecord r;
  r.slot = "app1";
  r.boots = 0;
  r.version = V(0, 1, 2);
  r.from = V(0, 1, 1);
  const BootDecision d = decide_boot(encode_trial(r), "app0", V(0, 1, 1), false);
  CHECK(d.action == BootAction::Normal);
  CHECK(d.erase);
  CHECK_FALSE(d.write);
  CHECK(d.notice.empty());
}

TEST_CASE("boot: a record for the running slot but another version (a USB flash) is dropped") {
  TrialRecord r;
  r.slot = "app0";
  r.boots = 0;
  r.version = V(0, 1, 1);
  r.from = V(0, 1, 0);
  BootDecision d = decide_boot(encode_trial(r), "app0", V(0, 1, 0), false);
  CHECK(d.action == BootAction::Normal);
  CHECK(d.erase);
}

TEST_CASE("health: display, Wi-Fi tried, then data or a minute") {
  TrialHealth h;
  CHECK_FALSE(h.ready(120000));
  h.display_up();
  CHECK_FALSE(h.ready(120000));
  h.wifi_attempted();
  CHECK_FALSE(h.ready(59999));
  CHECK(h.ready(60000));
  TrialHealth g;
  g.display_up();
  g.wifi_attempted();
  g.data_ok();
  CHECK(g.ready(5000));
  TrialHealth noDisplay;
  noDisplay.wifi_attempted();
  noDisplay.data_ok();
  CHECK_FALSE(noDisplay.ready(600000));
  CHECK(updated_notice(V(0, 1, 1)) == "Updated to 0.1.1");
}

TEST_CASE("health: inputs read as levels every tick latch, so no missed message keeps a good image unconfirmed") {
  // The board's host calls observe(frames > 0, wifi started) every tick.
  TrialHealth h;
  h.observe(false, false);  // the first ticks: nothing drawn yet, radio not started
  CHECK_FALSE(h.ready(120000));
  h.observe(true, false);   // a frame went out
  CHECK_FALSE(h.ready(120000));
  h.observe(true, true);    // the Wi-Fi service started
  CHECK_FALSE(h.ready(59999));
  CHECK(h.ready(60000));
  // A level that reads false later (never in practice: both only grow) does not undo it.
  h.observe(false, false);
  CHECK(h.ready(60000));
  TrialHealth d;
  d.observe(true, true);
  d.data_ok();
  CHECK(d.ready(3000));
  TrialHealth noWifi;  // the radio never starts: only the minute rule cannot help, it stays unconfirmed
  for (int i = 0; i < 100; ++i) noWifi.observe(true, false);
  noWifi.data_ok();
  CHECK_FALSE(noWifi.ready(600000));
}
