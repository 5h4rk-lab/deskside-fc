// The Software update row in the app: shown only with an updater, follows the
// updater's status, a tap installs only what is offered (with the battery the
// UI shows) and checks otherwise; nothing installs without that tap. Uses the
// simulator's MockUpdater (the real UpdateFlow behind it).
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "mock_power.h"
#include "mock_updater.h"
#include "prefs.h"
#include "screens/settings.h"
#include "sound_out.h"
#include "sound_synth.h"
#include "test_display.h"
#include "theme.h"
#include "update_check.h"

namespace {

constexpr int64_t kNow = 1784116800;  // 2026-07-15 12:00 UTC

class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

class NoSource : public DataSource {
 public:
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override { cb(false, {}); }
  void fetch_detail(const std::string&, std::function<void(bool, MatchDetail)> cb) override { cb(false, {}); }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override { cb(false, {}); }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override { cb(false, {}); }
};

Version V(int a, int b, int c) {
  Version v;
  v.major = a;
  v.minor = b;
  v.patch = c;
  return v;
}

// Counts what the app asks the speaker to play.
class CountingSound : public SoundOut {
 public:
  void play(const PcmClip&, uint8_t) override { ++plays; }
  void stop() override {}
  bool busy() const override { return false; }
  int plays = 0;
};

std::string text(lv_obj_t* l) { return lv_label_get_text(l); }
bool visible(lv_obj_t* o) { return !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }

struct Host {
  Store store;
  NoImages images;
  NoSource source;
  MemoryPrefs prefs;
  MockPower power;
  MockUpdater updater{V(0, 1, 0)};
  CountingSound sound;
  lv_obj_t* app = nullptr;

  explicit Host(bool withUpdater = true, bool withSound = false) {
    init_test_display();
    theme_reset();
    AppDeps deps{store, images, source, nullptr, prefs};
    deps.power = &power;
    if (withUpdater) deps.updater = &updater;
    if (withSound) deps.sound = &sound;
    app = app_create(lv_screen_active(), deps);
    app_refresh(app, kNow);
  }
  ~Host() {
    theme_reset();
    deinit_test_display();
  }
  const SettingsParts* parts() const { return settings_parts(app_settings_view(app)); }
  void tap_row() { lv_obj_send_event(parts()->updateRow, LV_EVENT_CLICKED, nullptr); }
  void advance(int64_t ms) {
    updater.tick(ms);
    app_refresh(app, kNow + ms / 1000);
  }
  void battery(int percent, bool charging = false) {
    PowerStatus p;
    p.present = true;
    p.percent = percent;
    p.charging = charging;
    p.millivolts = 3800;
    power.set(p);
    app_refresh(app, kNow);
  }
};

}  // namespace

TEST_CASE("update row: none without an updater") {
  Host h(false);
  app_open_settings(h.app);
  REQUIRE(h.parts() != nullptr);
  CHECK(h.parts()->updateRow == nullptr);
  CHECK(h.parts()->updateStatus == nullptr);
  settings_set_update(app_settings_view(h.app), UpdateStatus());  // harmless
}

TEST_CASE("update row: opening Settings checks, the row follows the status") {
  Host h;
  h.battery(80);
  app_open_settings(h.app);
  REQUIRE(h.parts()->updateRow != nullptr);
  CHECK(h.updater.opens == 1);
  CHECK(h.updater.checks == 1);
  CHECK(text(h.parts()->updateStatus) == "Checking for updates...");
  CHECK_FALSE(visible(h.parts()->updateBar));
  h.advance(MockUpdater::kCheckMs);
  CHECK(text(h.parts()->updateStatus) == "Version 0.1.1 available \xC2\xB7 Install");
  CHECK(h.updater.installs == 0);  // never without a tap
  // Opening again soon after does not check again.
  app_close_settings(h.app);
  app_open_settings(h.app);
  CHECK(h.updater.opens == 2);
  CHECK(h.updater.checks == 1);
  CHECK(text(h.parts()->updateStatus) == "Version 0.1.1 available \xC2\xB7 Install");
}

TEST_CASE("update row: a tap on the offer installs with the battery the UI shows; progress shows") {
  Host h;
  h.battery(80);
  app_open_settings(h.app);
  h.advance(MockUpdater::kCheckMs);
  h.tap_row();
  REQUIRE(h.updater.installCalls == 1);
  CHECK(h.updater.installs == 1);
  CHECK(h.updater.installPower.back().percent == 80);
  CHECK(text(h.parts()->updateStatus) == "Getting ready...");
  CHECK(visible(h.parts()->updateBar));
  h.advance(MockUpdater::kCheckMs + MockUpdater::kDownloadMs * 42 / 100 + 1);
  CHECK(text(h.parts()->updateStatus) == "Downloading 42%");
  CHECK(lv_bar_get_value(h.parts()->updateBar) == 42);
  // Taps while it runs change nothing.
  h.tap_row();
  CHECK(h.updater.installCalls == 1);
  CHECK(h.updater.checks == 1);
  h.advance(MockUpdater::kCheckMs + MockUpdater::kDownloadMs);
  CHECK(text(h.parts()->updateStatus) == "Restarting...");
}

TEST_CASE("update row: below 30 percent on battery the install is refused in plain English") {
  Host h;
  h.battery(20);
  app_open_settings(h.app);
  h.advance(MockUpdater::kCheckMs);
  h.tap_row();
  CHECK(h.updater.installCalls == 1);
  CHECK(h.updater.installs == 0);
  CHECK(text(h.parts()->updateStatus) == update_error_text(UpdateError::LowBattery));
  CHECK(lv_color_to_u32(lv_obj_get_style_text_color(h.parts()->updateStatus, 0)) == lv_color_to_u32(theme().amber));
  CHECK_FALSE(visible(h.parts()->updateBar));
  // Charging: the next tap checks again (the row offered nothing), then the
  // offer installs.
  h.battery(20, true);
  h.tap_row();
  CHECK(h.updater.checks == 2);
  h.advance(2 * MockUpdater::kCheckMs);
  h.tap_row();
  CHECK(h.updater.installs == 1);
}

TEST_CASE("update row: a tap while up to date checks again") {
  Host h;
  h.updater.manifest = "{\"version\":\"0.1.0\",\"url\":\"https://soccer-display-helper.vercel.app/f.bin\",\"sha256\":\"" +
                       std::string(64, 'b') + "\",\"size\":200000}";
  app_open_settings(h.app);
  h.advance(MockUpdater::kCheckMs);
  CHECK(text(h.parts()->updateStatus) == "Up to date (0.1.0)");
  h.tap_row();
  CHECK(h.updater.checks == 2);
  CHECK(text(h.parts()->updateStatus) == "Checking for updates...");
  h.tap_row();  // a check is running: no second one
  CHECK(h.updater.checks == 2);
  CHECK(h.updater.installCalls == 0);
}

TEST_CASE("update row: a background check never installs") {
  Host h;
  h.battery(90);
  for (int64_t t = 0; t <= 30 * 60 * 1000; t += 1000) h.updater.tick(t);
  CHECK(h.updater.checks == 1);
  CHECK(h.updater.status().phase == UpdatePhase::Available);
  CHECK(h.updater.installCalls == 0);
}

TEST_CASE("update row: no sound plays while an update runs; Test sound is disabled until it ends") {
  Host h(true, true);
  h.battery(80);
  app_open_settings(h.app);
  h.advance(MockUpdater::kCheckMs);
  lv_obj_t* test = h.parts()->testSound;
  REQUIRE(test != nullptr);
  CHECK_FALSE(lv_obj_has_state(test, LV_STATE_DISABLED));
  lv_obj_send_event(test, LV_EVENT_CLICKED, nullptr);
  CHECK(h.sound.plays == 1);  // before the install: it plays
  h.tap_row();
  REQUIRE(h.updater.installs == 1);
  CHECK(lv_obj_has_state(test, LV_STATE_DISABLED));
  // An event sent straight to the button (as LVGL would not for a disabled
  // one) still makes no sound: the app itself refuses while the update is busy.
  lv_obj_send_event(test, LV_EVENT_CLICKED, nullptr);
  CHECK(h.sound.plays == 1);
  h.advance(MockUpdater::kCheckMs + MockUpdater::kDownloadMs / 2);
  CHECK(text(h.parts()->updateStatus).find("Downloading") == 0);
  lv_obj_send_event(test, LV_EVENT_CLICKED, nullptr);
  CHECK(h.sound.plays == 1);
  CHECK(lv_obj_has_state(test, LV_STATE_DISABLED));
  // The sim's pretend update ends up to date (no restart): Test sound answers again.
  h.advance(MockUpdater::kCheckMs + MockUpdater::kDownloadMs + 10);  // restarting
  CHECK(lv_obj_has_state(test, LV_STATE_DISABLED));
  h.advance(MockUpdater::kCheckMs + MockUpdater::kDownloadMs + MockUpdater::kRestartMs + 20);
  CHECK_FALSE(update_status_busy(h.updater.status()));
  CHECK_FALSE(lv_obj_has_state(test, LV_STATE_DISABLED));
  lv_obj_send_event(test, LV_EVENT_CLICKED, nullptr);
  CHECK(h.sound.plays == 2);
}
