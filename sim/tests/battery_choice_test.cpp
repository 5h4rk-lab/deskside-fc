// The Battery setting (Auto / Installed / None): the stored value, how it
// changes the status the UI shows, the rows on the Settings screen and the
// app wiring. Also the "Needs night dimming on" line under Mute at night.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "battery_model.h"
#include "power_service.h"
#include "prefs.h"
#include "screens/goal.h"
#include "screens/live.h"
#include "screens/settings.h"
#include "settings.h"
#include "test_display.h"
#include "theme.h"

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

class FakePower : public PowerService {
 public:
  PowerStatus status() override { return value; }
  PowerStatus value{true, 78, false, 3920};
};

bool visible(lv_obj_t* obj) {
  for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

std::string label_text(lv_obj_t* parent) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
    lv_obj_t* c = lv_obj_get_child(parent, static_cast<int32_t>(i));
    if (lv_obj_check_type(c, &lv_label_class) && lv_label_get_text(c)[0] != '\xEF') return lv_label_get_text(c);
  }
  return "";
}

struct Screen {
  NoImages images;
  std::vector<Settings> changes;
  lv_obj_t* obj = nullptr;

  explicit Screen(bool battery = true, const Settings& s = Settings(), bool sound = true) {
    init_test_display();
    theme_reset();
    SettingsCallbacks cb;
    cb.battery = battery;
    if (sound) cb.onTestSound = [] {};
    cb.onWifi = [] {};
    cb.onChanged = [this](const Settings& n) { changes.push_back(n); };
    obj = settings_create(lv_screen_active(), s, images, cb, kNow);
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  ~Screen() { deinit_test_display(); }
  const SettingsParts* parts() const { return settings_parts(obj); }
  void click(lv_obj_t* o) { lv_obj_send_event(o, LV_EVENT_CLICKED, nullptr); }
};

struct AppHost {
  Store store;
  NoImages images;
  NoSource source;
  MemoryPrefs prefs;
  FakePower power;
  lv_obj_t* app = nullptr;

  AppHost() {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(true);
  }
  ~AppHost() {
    goal_dismiss();
    theme_reset();
    goal_set_reduced_motion(false);
    deinit_test_display();
  }
  void make(bool withPower = true) {
    AppDeps deps{store, images, source, nullptr, prefs};
    deps.power = withPower ? &power : nullptr;
    app = app_create(lv_screen_active(), deps);
    test_advance(20);
  }
  void refresh(int64_t now = kNow) {
    app_refresh(app, now);
    lv_obj_update_layout(lv_screen_active());
  }
  lv_obj_t* live() const {
    lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), 0);
    return lv_obj_get_child(tab, 0);
  }
  const SettingsParts* settings() const { return settings_parts(app_settings_view(app)); }
};

}  // namespace

// ------------------------------------------------------------ the stored value

TEST_CASE("battery setting: Auto by default, stored under batt as 0 1 2") {
  MemoryPrefs empty;
  CHECK(load_settings(empty).battery == BatteryChoice::Auto);
  CHECK(prefs_key_valid("batt"));
  const struct {
    BatteryChoice choice;
    const char* stored;
  } cases[] = {{BatteryChoice::Auto, "0"}, {BatteryChoice::Installed, "1"}, {BatteryChoice::None, "2"}};
  for (const auto& c : cases) {
    Settings s;
    s.battery = c.choice;
    MemoryPrefs p;
    save_settings(p, s);
    std::string v;
    REQUIRE(p.get("batt", v));
    CHECK(v == c.stored);
    CHECK(load_settings(p).battery == c.choice);
  }
}

TEST_CASE("battery setting: an unknown stored value is Auto") {
  for (const char* bad : {"3", "-1", "9", "none", "", "1.0", " 1"}) {
    MemoryPrefs p;
    p.set("batt", bad);
    INFO("stored '" << bad << "'");
    CHECK(load_settings(p).battery == BatteryChoice::Auto);
  }
}

// ------------------------------------------------------------ what the UI is told

TEST_CASE("apply_battery_choice: Auto passes the measured status through") {
  const PowerStatus cases[] = {PowerStatus{}, PowerStatus{true, 78, false, 3920}, PowerStatus{true, 100, false, 4180},
                               PowerStatus{true, 40, true, 3950}};
  for (const PowerStatus& s : cases) CHECK(apply_battery_choice(s, BatteryChoice::Auto) == s);
}

TEST_CASE("apply_battery_choice: None never shows a battery") {
  const PowerStatus cases[] = {PowerStatus{}, PowerStatus{true, 78, false, 3920}, PowerStatus{true, 100, false, 4180},
                               PowerStatus{true, 3, false, 3400}, PowerStatus{true, 40, true, 3950}};
  for (const PowerStatus& s : cases) {
    const PowerStatus r = apply_battery_choice(s, BatteryChoice::None);
    CHECK_FALSE(r.present);
    CHECK(r == PowerStatus{});
  }
}

TEST_CASE("apply_battery_choice: Installed keeps a measured battery, the 4.18 V float included") {
  const PowerStatus cases[] = {PowerStatus{true, 78, false, 3920}, PowerStatus{true, 100, false, 4180},
                               PowerStatus{true, 40, true, 3950}};
  for (const PowerStatus& s : cases) CHECK(apply_battery_choice(s, BatteryChoice::Installed) == s);
}

TEST_CASE("apply_battery_choice: Installed shows an empty battery when the model sees none") {
  const PowerStatus r = apply_battery_choice(PowerStatus{}, BatteryChoice::Installed);
  CHECK(r.present);
  CHECK(r.percent == 0);
  CHECK_FALSE(r.charging);
  CHECK(r.millivolts == 0);
}

// ------------------------------------------------------------ Settings rows

TEST_CASE("battery rows: absent when the host has no battery") {
  Screen s(false);
  const SettingsParts* p = s.parts();
  CHECK(p->batteryChoiceRows[0] == nullptr);
  CHECK(p->batteryChoiceRows[2] == nullptr);
  CHECK(p->batteryChoiceHelp == nullptr);
}

TEST_CASE("battery rows: the three choices, the helper line and the default tick") {
  Screen s;
  const SettingsParts* p = s.parts();
  REQUIRE(p->batteryChoiceRows[2] != nullptr);
  CHECK(label_text(p->batteryChoiceRows[0]) == "Auto");
  CHECK(label_text(p->batteryChoiceRows[1]) == "Installed");
  CHECK(label_text(p->batteryChoiceRows[2]) == "None");
  CHECK(std::string(lv_label_get_text(p->batteryChoiceHelp)) == "Pick None if no battery is connected.");
  CHECK(std::string(kBatteryHelp) == "Pick None if no battery is connected.");
  CHECK(visible(p->batteryChoiceTicks[0]));
  CHECK_FALSE(visible(p->batteryChoiceTicks[1]));
  CHECK_FALSE(visible(p->batteryChoiceTicks[2]));
  // The card is titled "Battery".
  CHECK(label_text(lv_obj_get_parent(p->batteryChoiceRows[0])) == "Battery");
}

TEST_CASE("battery rows: choosing moves the tick and reports one change; the same choice reports none") {
  Screen s;
  const SettingsParts* p = s.parts();
  s.click(p->batteryChoiceRows[2]);
  REQUIRE(s.changes.size() == 1);
  CHECK(s.changes[0].battery == BatteryChoice::None);
  CHECK_FALSE(visible(p->batteryChoiceTicks[0]));
  CHECK(visible(p->batteryChoiceTicks[2]));
  s.click(p->batteryChoiceRows[2]);
  CHECK(s.changes.size() == 1);
  s.click(p->batteryChoiceRows[1]);
  REQUIRE(s.changes.size() == 2);
  CHECK(s.changes[1].battery == BatteryChoice::Installed);
  CHECK(visible(p->batteryChoiceTicks[1]));
  CHECK_FALSE(visible(p->batteryChoiceTicks[2]));
  // Nothing else changed.
  CHECK(s.changes[1].soundMode == Settings().soundMode);
  CHECK(s.changes[1].dayBrightness == Settings().dayBrightness);
}

TEST_CASE("battery rows: show the settings given, and a refresh follows them") {
  Settings none;
  none.battery = BatteryChoice::None;
  Screen s(true, none);
  const SettingsParts* p = s.parts();
  CHECK(visible(p->batteryChoiceTicks[2]));
  CHECK_FALSE(visible(p->batteryChoiceTicks[0]));
  Settings installed;
  installed.battery = BatteryChoice::Installed;
  settings_refresh(s.obj, installed, kNow);
  CHECK(visible(p->batteryChoiceTicks[1]));
  CHECK_FALSE(visible(p->batteryChoiceTicks[2]));
  CHECK(s.changes.empty());
}

TEST_CASE("battery rows: after Mute at night and before Wi-Fi, every row fits and can be scrolled to") {
  Screen s;
  const SettingsParts* p = s.parts();
  lv_obj_t* card = lv_obj_get_parent(p->batteryChoiceRows[0]);
  lv_obj_t* quiet = lv_obj_get_parent(lv_obj_get_parent(p->soundNightSwitch));
  CHECK(lv_obj_get_index(card) == lv_obj_get_index(quiet) + 1);
  CHECK(lv_obj_get_index(p->wifiRow) == lv_obj_get_index(card) + 1);
  for (lv_obj_t* o : p->batteryChoiceRows) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    CHECK(a.x1 >= 0);
    CHECK(a.x2 < 240);
    CHECK(lv_obj_get_height(o) >= 22);
    lv_area_t list;
    lv_obj_get_coords(p->list, &list);
    lv_obj_scroll_to_y(p->list, lv_obj_get_scroll_y(p->list) + (a.y1 - list.y1) - 20, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_get_coords(o, &a);
    CHECK(a.y1 >= list.y1 - 1);
    CHECK(a.y2 <= list.y2 + 1);
  }
  // Without a speaker the card follows Reduced motion.
}

TEST_CASE("battery rows: without a speaker the card follows Reduced motion") {
  Screen s(true, Settings(), false);
  const SettingsParts* p = s.parts();
  lv_obj_t* card = lv_obj_get_parent(p->batteryChoiceRows[0]);
  lv_obj_t* motion = lv_obj_get_parent(p->motionHelp);
  CHECK(lv_obj_get_index(card) == lv_obj_get_index(motion) + 1);
}

// ------------------------------------------------------------ Mute at night hint

TEST_CASE("mute at night: 'Needs night dimming on' shows only while night dimming is off") {
  Settings off;
  off.nightDim = false;
  Screen s(true, off);
  const SettingsParts* p = s.parts();
  REQUIRE(p->soundNightNeedsDim != nullptr);
  CHECK(std::string(lv_label_get_text(p->soundNightNeedsDim)) == "Needs night dimming on");
  CHECK(visible(p->soundNightNeedsDim));
  // Switching night dimming on hides it at once.
  lv_obj_add_state(p->nightSwitch, LV_STATE_CHECKED);
  lv_obj_send_event(p->nightSwitch, LV_EVENT_VALUE_CHANGED, nullptr);
  CHECK_FALSE(visible(p->soundNightNeedsDim));
  // A refresh with it off shows it again.
  settings_refresh(s.obj, off, kNow);
  CHECK(visible(p->soundNightNeedsDim));
}

TEST_CASE("mute at night: no hint with the default settings (night dimming on)") {
  Screen s;
  CHECK_FALSE(visible(s.parts()->soundNightNeedsDim));
}

// ------------------------------------------------------------ app wiring

TEST_CASE("app: the Battery rows exist only with a power service") {
  {
    AppHost h;
    h.make(true);
    app_open_settings(h.app);
    REQUIRE(h.settings() != nullptr);
    CHECK(h.settings()->batteryChoiceRows[0] != nullptr);
  }
  {
    AppHost h;
    h.make(false);
    app_open_settings(h.app);
    REQUIRE(h.settings() != nullptr);
    CHECK(h.settings()->batteryChoiceRows[0] == nullptr);
  }
}

TEST_CASE("app: None hides the battery at once, is saved, and Auto brings it back") {
  AppHost h;
  h.make();
  h.refresh();
  REQUIRE(h.store.power().present);
  CHECK(visible(live_parts(h.live())->status.battery));
  app_open_settings(h.app);
  h.refresh();
  lv_obj_send_event(h.settings()->batteryChoiceRows[2], LV_EVENT_CLICKED, nullptr);
  CHECK(app_settings(h.app)->battery == BatteryChoice::None);
  std::string v;
  REQUIRE(h.prefs.get("batt", v));
  CHECK(v == "2");
  // The Settings line follows at once; the status rows with the next refresh.
  CHECK_FALSE(h.store.power().present);
  CHECK(std::string(lv_label_get_text(h.settings()->batteryStatus)) == "No battery detected");
  h.refresh(kNow + 1);
  CHECK_FALSE(visible(live_parts(h.live())->status.battery));
  lv_obj_send_event(h.settings()->batteryChoiceRows[0], LV_EVENT_CLICKED, nullptr);
  h.refresh(kNow + 2);
  CHECK(h.store.power().present);
  CHECK(visible(live_parts(h.live())->status.battery));
  REQUIRE(h.prefs.get("batt", v));
  CHECK(v == "0");
}

TEST_CASE("app: a saved None survives a restart and never raises the low-battery banner") {
  AppHost h;
  h.prefs.set("batt", "2");
  h.power.value = PowerStatus{true, 4, false, 3400};
  h.make();
  h.refresh();
  h.refresh(kNow + 1);
  CHECK(app_settings(h.app)->battery == BatteryChoice::None);
  CHECK_FALSE(h.store.power().present);
  CHECK(app_battery_banner(h.app) == nullptr);
}

TEST_CASE("app: Installed shows the battery even when the model sees none") {
  AppHost h;
  h.prefs.set("batt", "1");
  h.power.value = PowerStatus{};
  h.make();
  h.refresh();
  CHECK(h.store.power().present);
  CHECK(h.store.power().percent == 0);
  CHECK(visible(live_parts(h.live())->status.battery));
}

TEST_CASE("app: the low-battery banner comes at 15 percent with Auto (injected readings)") {
  AppHost h;
  h.power.value = PowerStatus{true, 40, false, 3900};
  h.make();
  h.refresh();
  CHECK(app_battery_banner(h.app) == nullptr);
  h.power.value = PowerStatus{true, 16, false, 3740};
  h.refresh(kNow + 1);
  CHECK(app_battery_banner(h.app) == nullptr);
  h.power.value = PowerStatus{true, 15, false, 3730};
  h.refresh(kNow + 2);
  CHECK(app_battery_banner(h.app) != nullptr);
}
