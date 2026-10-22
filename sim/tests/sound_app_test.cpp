// Sound in the app: what plays for a goal, a win, the battery banner, a
// Wi-Fi join and the Test sound button, through a recording speaker.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "demo_data.h"
#include "mock_source.h"
#include "mock_wifi.h"
#include "power_service.h"
#include "prefs.h"
#include "screens/goal.h"
#include "screens/settings.h"
#include "screens/wifi.h"
#include "sound_out.h"
#include "sound_synth.h"
#include "store.h"
#include "test_display.h"
#include "theme.h"

namespace {

constexpr int64_t kDay = 1784116800;            // 2026-07-15 12:00 UTC
constexpr int64_t kNight = kDay - 10 * 3600;    // 02:00 UTC, inside 23:00..07:00

class NullImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

class FakePower : public PowerService {
 public:
  PowerStatus status() override { return value; }
  PowerStatus value{true, 78, false, 3920};
};

class FakeSoundOut : public SoundOut {
 public:
  struct Call {
    const PcmClip* clip;
    int volume;
  };
  void play(const PcmClip& clip, uint8_t volumePercent) override { calls.push_back({&clip, volumePercent}); }
  void stop() override { ++stops; }
  bool busy() const override { return false; }
  size_t count() const { return calls.size(); }
  bool played(SoundEvent e) const {
    for (const Call& c : calls) {
      if (c.clip == &sound_clip(e)) return true;
    }
    return false;
  }
  std::vector<Call> calls;
  int stops = 0;
};

MatchSummary live_match(int home, int away, MatchState state = MatchState::Live) {
  MatchSummary m;
  m.id = "m1";
  m.league = "eng.1";
  m.state = state;
  m.clock = "67'";
  m.kickoff = 1792526400;
  m.home = {"359", "ARS", "Arsenal", "", 0xEF0107, 0xEF0107, ""};
  m.away = {"363", "CHE", "Chelsea", "", 0xDBA111, 0x034694, ""};
  m.homeScore = home;
  m.awayScore = away;
  return m;
}

struct Fixture {
  Store store;
  NullImages images;
  MockSource source;
  MemoryPrefs prefs;
  FakeSoundOut sound;
  MockWifi wifi;
  FakePower power;
  bool withPower = false;
  lv_obj_t* app = nullptr;
  int64_t now = kDay;

  explicit Fixture(bool withSound = true, bool create = true) {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(true);
    store.set_favourite("359");
    withSound_ = withSound;
    if (create) make();
  }
  ~Fixture() {
    goal_dismiss();
    theme_reset();
    goal_set_reduced_motion(false);
    deinit_test_display();
  }
  void make(WifiService* w = nullptr) {
    set_current(live_match(1, 0));
    AppDeps deps{store, images, source, nullptr, prefs};
    deps.wifi = w;
    deps.power = withPower ? &power : nullptr;
    deps.sound = withSound_ ? &sound : nullptr;
    app = app_create(lv_screen_active(), deps);
    app_refresh(app, now);
    test_advance(20);
  }
  void set_current(const MatchSummary& m) {
    source.set_matches({m});
    MatchDetail d = demo_detail(demo::kLiveMatchId);
    d.match = m;
    source.set_detail(d);
  }
  // One poll: matches into the Store, the app's goal check, a refresh.
  void poll(const MatchSummary& m) {
    set_current(m);
    source.fetch_matches([this](bool ok, std::vector<MatchSummary> ms) {
      if (ok) store.set_matches(std::move(ms), now);
    });
    app_matches_changed(app, now);
    app_refresh(app, now);
    test_advance(0);
  }
  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }

 private:
  bool withSound_;
};

}  // namespace

// ------------------------------------------------------------ goals

TEST_CASE("sound: a favourite goal plays the Goal clip exactly once, at the saved volume") {
  Fixture f;
  f.poll(live_match(1, 0));  // first sight of the match: silent baseline
  CHECK(f.sound.count() == 0);
  f.poll(live_match(2, 0));
  REQUIRE(f.sound.count() == 1);
  CHECK(f.sound.calls[0].clip == &sound_clip(SoundEvent::Goal));
  CHECK(f.sound.calls[0].volume == 40);
  f.poll(live_match(2, 0));  // the same score again: a replay
  f.poll(live_match(2, 0));
  CHECK(f.sound.count() == 1);
}

TEST_CASE("sound: the volume setting is what the speaker is told") {
  for (int vol : {20, 60, 80, 100}) {
    Fixture f(true, false);
    f.prefs.set("snd.vol", std::to_string(vol));
    f.make();
    f.poll(live_match(1, 0));
    f.poll(live_match(2, 0));
    REQUIRE(f.sound.count() == 1);
    CHECK(f.sound.calls[0].volume == vol);
  }
}

TEST_CASE("sound: an opponent's goal makes no sound") {
  Fixture f;
  f.poll(live_match(1, 0));
  f.poll(live_match(1, 1));
  CHECK(f.sound.count() == 0);
}

TEST_CASE("sound: a match of other teams makes no sound") {
  Fixture f;
  f.store.set_favourite("999");
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  f.poll(live_match(2, 1));
  CHECK(f.sound.count() == 0);
}

TEST_CASE("sound: the first poll after a favourite switch is silent (stale baseline)") {
  Fixture f;
  f.store.set_favourite("363");  // Chelsea
  f.poll(live_match(1, 0));
  f.poll(live_match(1, 1));      // their goal
  REQUIRE(f.sound.count() == 1); // the baseline was taken after the switch: this one is real
  Fixture g;
  g.poll(live_match(1, 0));
  g.store.set_favourite("363");
  g.poll(live_match(1, 3));      // first sight for Chelsea: baseline only
  CHECK(g.sound.count() == 0);
}

TEST_CASE("sound: a second goal inside 10 s is held back, and one after it plays") {
  Fixture f;
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  f.poll(live_match(3, 0));
  CHECK(f.sound.count() == 1);
  test_advance(10001);
  f.poll(live_match(4, 0));
  CHECK(f.sound.count() == 2);
}

TEST_CASE("sound: mode Off plays nothing, not even a goal; battery and Wi-Fi sounds are off too") {
  Fixture f(true, false);
  f.prefs.set("snd.mode", "0");
  f.withPower = true;
  f.power.value = PowerStatus{true, 14, false, 3500};
  f.make();
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  f.poll(live_match(2, 0, MatchState::Post));
  CHECK(f.sound.count() == 0);
}

TEST_CASE("sound: a null sound pointer is safe everywhere") {
  Fixture f(false);
  f.withPower = true;
  f.power.value = PowerStatus{true, 14, false, 3500};
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  f.poll(live_match(2, 0, MatchState::Post));
  app_show_battery_banner(f.app, Alert{Alert::Level::Low, battery::kAlertLowText, battery::kAlertLowMs});
  app_open_settings(f.app);
  f.settle();
  const SettingsParts* p = settings_parts(app_settings_view(f.app));
  REQUIRE(p != nullptr);
  CHECK(p->testSound == nullptr);  // no speaker, no Sound rows
  CHECK(p->soundRows[0] == nullptr);
  CHECK(app_settings(f.app)->soundMode == SoundMode::GoalsOnly);
}

// ------------------------------------------------------------ quiet hours

TEST_CASE("sound: night mute silences a goal while night dimming is active") {
  Fixture f;
  f.now = kNight;
  app_refresh(f.app, f.now);
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  CHECK(f.sound.count() == 0);
}

TEST_CASE("sound: with mute at night off a night goal plays") {
  Fixture f(true, false);
  f.prefs.set("snd.night", "0");
  f.make();
  f.now = kNight;
  app_refresh(f.app, f.now);
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  CHECK(f.sound.count() == 1);
}

TEST_CASE("sound: the mute only applies while night dimming is on") {
  Fixture f(true, false);
  f.prefs.set("night", "0");  // night dimming off
  f.make();
  f.now = kNight;
  app_refresh(f.app, f.now);
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  CHECK(f.sound.count() == 1);
}

TEST_CASE("sound: the night window follows the chosen zone") {
  Fixture f(true, false);
  f.prefs.set("zone", "Asia/Tokyo");  // 12:00 UTC is 21:00 there: day
  f.make();
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  CHECK(f.sound.count() == 1);
  Fixture g(true, false);
  g.prefs.set("zone", "Asia/Tokyo");
  g.make();
  g.now = kDay + 3 * 3600;  // 15:00 UTC is midnight in Tokyo: night
  app_refresh(g.app, g.now);
  g.poll(live_match(1, 0));
  g.poll(live_match(2, 0));
  CHECK(g.sound.count() == 0);
}

TEST_CASE("sound: touching the screen lifts the dimming, not the mute") {
  Fixture f;
  f.now = kNight;
  lv_display_trigger_activity(nullptr);
  app_refresh(f.app, f.now);
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  CHECK(f.sound.count() == 0);
}

// ------------------------------------------------------------ wins

TEST_CASE("sound: Goals only plays the fanfare at the win, straight away, and no whistles") {
  Fixture f;
  f.poll(live_match(2, 1));
  f.poll(live_match(2, 1, MatchState::Post));
  REQUIRE(f.sound.count() == 1);
  CHECK(f.sound.calls[0].clip == &sound_clip(SoundEvent::WinFanfare));
}

TEST_CASE("sound: Goals and match alerts plays the full-time whistles, then the fanfare") {
  Fixture f(true, false);
  f.prefs.set("snd.mode", "2");
  f.make();
  f.poll(live_match(2, 1));
  f.poll(live_match(2, 1, MatchState::Post));
  REQUIRE(f.sound.count() == 1);
  CHECK(f.sound.calls[0].clip == &sound_clip(SoundEvent::FullTime));
  test_advance(static_cast<uint32_t>(sound_clip(SoundEvent::FullTime).duration_ms()) - 100);
  CHECK(f.sound.count() == 1);
  test_advance(200);
  REQUIRE(f.sound.count() == 2);
  CHECK(f.sound.calls[1].clip == &sound_clip(SoundEvent::WinFanfare));
  test_advance(3000);
  CHECK(f.sound.count() == 2);  // once
}

TEST_CASE("sound: a draw or a defeat at full time makes no sound") {
  Fixture f(true, false);
  f.prefs.set("snd.mode", "2");
  f.make();
  f.poll(live_match(1, 1));
  f.poll(live_match(1, 1, MatchState::Post));
  test_advance(5000);
  CHECK(f.sound.count() == 0);
  Fixture g(true, false);
  g.prefs.set("snd.mode", "2");
  g.make();
  g.poll(live_match(0, 1));
  g.poll(live_match(0, 1, MatchState::Post));
  CHECK(g.sound.count() == 0);
}

TEST_CASE("sound: the app deleted while the fanfare waits leaves nothing behind") {
  Fixture f(true, false);
  f.prefs.set("snd.mode", "2");
  f.make();
  f.poll(live_match(2, 1));
  f.poll(live_match(2, 1, MatchState::Post));
  REQUIRE(f.sound.count() == 1);
  lv_obj_delete(f.app);
  test_advance(5000);
  CHECK(f.sound.count() == 1);
}

// ------------------------------------------------------------ battery banner

TEST_CASE("sound: the 15 percent banner beeps once, the 5 percent one beeps three times once") {
  Fixture f(true, false);
  f.withPower = true;
  f.make();
  f.power.value = PowerStatus{true, 14, false, 3500};
  app_refresh(f.app, f.now);
  REQUIRE(app_battery_banner(f.app) != nullptr);
  REQUIRE(f.sound.count() == 1);
  CHECK(f.sound.calls[0].clip == &sound_clip(SoundEvent::BatteryLow));
  // The banner stays up: many refreshes, no more beeps.
  for (int i = 1; i <= 5; ++i) app_refresh(f.app, f.now + i);
  CHECK(f.sound.count() == 1);
  // It goes by itself; then the critical one.
  test_advance(battery::kAlertLowMs + 500);
  f.power.value = PowerStatus{true, 4, false, 3300};
  app_refresh(f.app, f.now + 20);
  REQUIRE(f.sound.count() == 2);
  CHECK(f.sound.calls[1].clip == &sound_clip(SoundEvent::BatteryCritical));
  for (int i = 21; i < 26; ++i) app_refresh(f.app, f.now + i);
  CHECK(f.sound.count() == 2);
}

TEST_CASE("sound: a battery that stays low beeps once per discharge, not per refresh") {
  Fixture f(true, false);
  f.withPower = true;
  f.power.value = PowerStatus{true, 10, false, 3500};
  f.make();
  test_advance(battery::kAlertLowMs + 500);
  for (int i = 0; i < 30; ++i) app_refresh(f.app, f.now + 100 + i);
  CHECK(f.sound.count() == 1);
}

TEST_CASE("sound: the banner is silent in mode Off and during the night mute") {
  Fixture f(true, false);
  f.withPower = true;
  f.power.value = PowerStatus{true, 14, false, 3500};
  f.make();  // at day: plays
  CHECK(f.sound.count() == 1);
  Fixture g(true, false);
  g.withPower = true;
  g.power.value = PowerStatus{true, 14, false, 3500};
  g.now = kNight;
  g.make();
  CHECK(g.sound.count() == 0);
}

// ------------------------------------------------------------ Wi-Fi

namespace {
MockWifi demo_wifi() {
  MockWifi w;
  w.set_networks({{"Home", -50, false}, {"Cafe", -70, true}});
  w.set_password("Home", "homehome1");
  return w;
}
}  // namespace

TEST_CASE("sound: a successful Wi-Fi join plays the chime once; a wrong password does not") {
  Fixture f(true, false);
  MockWifi wifi = demo_wifi();
  f.make(&wifi);
  lv_obj_t* view = app_wifi_view(f.app);
  REQUIRE(view != nullptr);
  wifi_open_password(view, "Home");
  lv_textarea_set_text(wifi_password_parts(view)->field, "wrongwrong");
  lv_obj_send_event(wifi_password_parts(view)->connect, LV_EVENT_CLICKED, nullptr);
  f.settle();
  CHECK(f.sound.count() == 0);
  lv_textarea_set_text(wifi_password_parts(view)->field, "homehome1");
  lv_obj_send_event(wifi_password_parts(view)->connect, LV_EVENT_CLICKED, nullptr);
  f.settle();
  REQUIRE(f.sound.count() == 1);
  CHECK(f.sound.calls[0].clip == &sound_clip(SoundEvent::WifiOk));
  test_advance(3000);
  CHECK(f.sound.count() == 1);
}

// ------------------------------------------------------------ Test sound

TEST_CASE("sound: Test sound plays the Goal clip at the chosen volume, whatever the mode and the hour") {
  Fixture f(true, false);
  f.prefs.set("snd.mode", "0");
  f.prefs.set("snd.vol", "80");
  f.now = kNight;
  f.make();
  app_open_settings(f.app);
  f.settle();
  const SettingsParts* p = settings_parts(app_settings_view(f.app));
  REQUIRE(p != nullptr);
  REQUIRE(p->testSound != nullptr);
  lv_obj_send_event(p->testSound, LV_EVENT_CLICKED, nullptr);
  REQUIRE(f.sound.count() == 1);
  CHECK(f.sound.calls[0].clip == &sound_clip(SoundEvent::Goal));
  CHECK(f.sound.calls[0].volume == 80);
  lv_obj_send_event(p->testSound, LV_EVENT_CLICKED, nullptr);  // again: the button always answers
  CHECK(f.sound.count() == 2);
}

TEST_CASE("sound: Settings changes are applied at once and saved under snd.mode, snd.vol and snd.night") {
  Fixture f;
  app_open_settings(f.app);
  f.settle();
  const SettingsParts* p = settings_parts(app_settings_view(f.app));
  REQUIRE(p != nullptr);
  lv_obj_send_event(p->soundRows[2], LV_EVENT_CLICKED, nullptr);
  CHECK(app_settings(f.app)->soundMode == SoundMode::GoalsAndAlerts);
  lv_obj_send_event(p->volumePlus, LV_EVENT_CLICKED, nullptr);
  CHECK(app_settings(f.app)->soundVolume == 60);
  lv_obj_add_state(p->soundNightSwitch, LV_STATE_CHECKED);  // already on: remove then add
  lv_obj_remove_state(p->soundNightSwitch, LV_STATE_CHECKED);
  lv_obj_send_event(p->soundNightSwitch, LV_EVENT_VALUE_CHANGED, nullptr);
  CHECK_FALSE(app_settings(f.app)->soundMuteAtNight);
  std::string v;
  REQUIRE(f.prefs.get("snd.mode", v));
  CHECK(v == "2");
  REQUIRE(f.prefs.get("snd.vol", v));
  CHECK(v == "60");
  REQUIRE(f.prefs.get("snd.night", v));
  CHECK(v == "0");
  // The new volume is what the next sound uses.
  f.poll(live_match(1, 0));
  f.poll(live_match(2, 0));
  REQUIRE(f.sound.count() == 1);
  CHECK(f.sound.calls[0].volume == 60);
}

TEST_CASE("sound: after Settings changes the mode, a restart keeps it") {
  Fixture f;
  app_open_settings(f.app);
  f.settle();
  const SettingsParts* p = settings_parts(app_settings_view(f.app));
  lv_obj_send_event(p->soundRows[0], LV_EVENT_CLICKED, nullptr);  // Off
  const Settings reloaded = load_settings(f.prefs);
  CHECK(reloaded.soundMode == SoundMode::Off);
}
