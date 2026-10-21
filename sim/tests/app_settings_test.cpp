// App-level settings: saved settings applied at start, Settings / zone list /
// team picker overlays, applying a favourite, time zones and the backlight.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "backlight.h"
#include "mock_wifi.h"
#include "prefs.h"
#include "screens/goal.h"
#include "screens/live.h"
#include "screens/settings.h"
#include "screens/team_picker.h"
#include "screens/wifi.h"
#include "screens/zone_list.h"
#include "settings.h"
#include "test_display.h"
#include "theme.h"

namespace {

// 2026-07-15 12:00 UTC and 2026-01-15 12:00 UTC.
constexpr int64_t kSummer = 1784116800;
constexpr int64_t kWinter = 1768478400;
// New York leaves daylight saving on 2026-11-01 at 06:00 UTC.
constexpr int64_t kUsFallBack = 1793512800;
// 2026-07-15 02:00 UTC.
constexpr int64_t kNight = kSummer - 10 * 3600;

class FakeBacklight : public Backlight {
 public:
  void set_percent(uint8_t percent) override { values.push_back(percent); }
  std::vector<int> values;
};

class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

// Matches, details and news fail at once; teams are delivered by the test
// (complete_teams) or at once when `immediate` is set.
class TeamsSource : public DataSource {
 public:
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    cb(false, {});
  }
  void fetch_detail(const std::string&, std::function<void(bool, MatchDetail)> cb) override {
    cb(false, {});
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override { cb(false, {}); }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override {
    ++teamCalls;
    if (immediate) {
      cb(true, teams);
    } else {
      pending = std::move(cb);
    }
  }
  void complete_teams(bool ok) {
    REQUIRE(pending);
    auto cb = std::move(pending);
    pending = nullptr;
    cb(ok, ok ? teams : std::vector<Team>{});
  }

  bool immediate = false;
  int teamCalls = 0;
  std::vector<Team> teams;
  std::function<void(bool, std::vector<Team>)> pending;
};

// Counts writes, so a test can tell "saved again" from "left alone".
class CountingPrefs : public MemoryPrefs {
 public:
  void set(const std::string& key, const std::string& value) override {
    ++writes;
    MemoryPrefs::set(key, value);
  }
  int writes = 0;
};

Team make_team(const char* id, const char* abbr, const char* name, uint32_t color,
               const char* league = "eng.1", uint32_t primary = 0) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  t.color = color;
  t.primary = primary != 0 ? primary : color;
  t.league = league;
  t.crest = std::string("/api/img?u=c") + id + "&w=48&h=48";
  return t;
}

Team arsenal() { return make_team("359", "ARS", "Arsenal", 0xEF0107); }
Team liverpool() { return make_team("364", "LIV", "Liverpool", 0xC8102E); }
Team tottenham() { return make_team("367", "TOT", "Tottenham Hotspur", 0xFFFFFF, "eng.1", 0x132257); }
Team real_madrid() { return make_team("86", "RMA", "Real Madrid", 0xFEBE10, "esp.1"); }

uint32_t u32(lv_color_t c) { return lv_color_to_u32(c); }
std::string text(lv_obj_t* label) { return lv_label_get_text(label); }
bool visible(lv_obj_t* obj) { return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }
std::string pref(const Prefs& p, const char* key) {
  std::string v;
  return p.get(key, v) ? v : "<missing>";
}

uint32_t descendants(lv_obj_t* obj) {
  uint32_t n = 0;
  const uint32_t c = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < c; ++i) n += 1 + descendants(lv_obj_get_child(obj, static_cast<int32_t>(i)));
  return n;
}

struct Host {
  Store store;
  NoImages images;
  TeamsSource source;
  CountingPrefs prefs;
  FakeBacklight backlight;
  WifiService* wifi = nullptr;
  int wifiConnected = 0;  // onWifiConnected calls
  lv_obj_t* app = nullptr;

  explicit Host(bool create = true) {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(false);
    source.teams = {arsenal(), liverpool(), tottenham(), real_madrid()};
    if (create) make();
  }
  ~Host() {
    theme_reset();
    goal_set_reduced_motion(false);
    deinit_test_display();
  }

  void make() {
    AppDeps deps{store, images, source, nullptr, prefs, &backlight, wifi};
    deps.onWifiConnected = [this] { ++wifiConnected; };
    app = app_create(lv_screen_active(), deps);
    settle();
  }
  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  lv_obj_t* tab_screen(int i) const {
    lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), i);
    return lv_obj_get_child(tab, 0);
  }
  lv_obj_t* tab_icon(int i) const {
    lv_obj_t* button = lv_obj_get_child(lv_tabview_get_tab_bar(app), i);
    return lv_obj_get_child(button, 1);
  }
  const SettingsParts* settings() const { return settings_parts(app_settings_view(app)); }
  void click(lv_obj_t* o) { lv_obj_send_event(o, LV_EVENT_CLICKED, nullptr); }
  void toggle(lv_obj_t* sw) {
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
      lv_obj_remove_state(sw, LV_STATE_CHECKED);
    } else {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
  }
};

MatchSummary live_match(const Team& home, const Team& away) {
  MatchSummary m;
  m.id = "m1";
  m.league = "eng.1";
  m.state = MatchState::Live;
  m.clock = "30'";
  m.kickoff = kSummer - 1800;
  m.home = home;
  m.away = away;
  m.homeScore = 1;
  m.awayScore = 0;
  return m;
}

}  // namespace

// ------------------------------------------------------------ start-up

TEST_CASE("app: defaults without saved settings (Arsenal, UTC)") {
  Host h;
  CHECK(h.store.favourite() == "359");
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xEF0107)));
  CHECK(u32(theme().brand) == u32(lv_color_hex(0xEF0107)));
  app_refresh(h.app, kSummer);
  CHECK(h.store.tz_offset() == 0);
  REQUIRE(app_settings(h.app) != nullptr);
  CHECK(app_settings(h.app)->zoneId == "UTC");
}

TEST_CASE("app: a saved favourite drives the Store, the accent, the tab icon and the Live screen") {
  Host h(false);
  Settings s;
  s.favourite = liverpool();
  save_settings(h.prefs, s);
  h.make();

  CHECK(h.store.favourite() == "364");
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xC8102E)));
  CHECK(u32(lv_obj_get_style_text_color(h.tab_icon(0), 0)) == u32(theme().accent));

  h.store.set_matches({live_match(liverpool(), arsenal())}, kSummer);
  app_refresh(h.app, kSummer);
  const LiveParts* live = live_parts(h.tab_screen(0));
  REQUIRE(live != nullptr);
  CHECK(visible(live->score));
  CHECK(text(live->homeName) == "LIVERPOOL");
}

TEST_CASE("app: saved reduced motion and zone are applied") {
  Host h(false);
  Settings s;
  s.reducedMotion = true;
  s.zoneId = "America/New_York";
  save_settings(h.prefs, s);
  h.make();
  CHECK(goal_reduced_motion());
  app_refresh(h.app, kSummer);
  CHECK(h.store.tz_offset() == -14400);
}

// ------------------------------------------------------------ Settings overlay

TEST_CASE("app: the Live gear opens Settings above the tab content; Back closes it") {
  Host h;
  const LiveParts* live = live_parts(h.tab_screen(0));
  REQUIRE(live != nullptr);
  REQUIRE(live->gear != nullptr);
  CHECK(visible(live->gear));
  CHECK(lv_obj_get_width(live->gear) >= 32);
  CHECK(lv_obj_get_height(live->gear) >= 32);
  CHECK(app_settings_view(h.app) == nullptr);

  h.click(live->gear);
  h.settle();
  lv_obj_t* view = app_settings_view(h.app);
  REQUIRE(view != nullptr);
  REQUIRE(h.settings() != nullptr);
  // Above the tab content, the tab bar stays visible.
  lv_obj_t* overlay = lv_obj_get_parent(view);
  CHECK(lv_obj_get_y(overlay) == 0);
  CHECK(lv_obj_get_height(overlay) == 320 - layout::kTabBarHeight);
  CHECK(lv_obj_get_index(overlay) > lv_obj_get_index(h.app));

  h.click(h.settings()->back);
  h.settle();
  CHECK(app_settings_view(h.app) == nullptr);
}

TEST_CASE("app: picking a tab closes Settings") {
  Host h;
  app_open_settings(h.app);
  h.settle();
  REQUIRE(app_settings_view(h.app) != nullptr);
  h.click(lv_obj_get_child(lv_tabview_get_tab_bar(h.app), 1));
  h.settle();
  CHECK(app_settings_view(h.app) == nullptr);
}

TEST_CASE("app: the Wi-Fi row only exists with a Wi-Fi service, shows its state and opens Wi-Fi") {
  {
    Host h;
    app_open_settings(h.app);
    CHECK(h.settings()->wifiRow == nullptr);
    CHECK(h.settings()->wifiStatus == nullptr);
  }
  {
    MockWifi wifi;
    wifi.set_networks({{"Home", -50, false}});
    wifi.set_password("Home", "homehome1");
    wifi.set_saved({"Home"});
    Host h(false);
    h.wifi = &wifi;
    h.make();
    CHECK(app_wifi_view(h.app) == nullptr);  // something is saved: no first setup
    app_open_settings(h.app);
    h.settle();  // one-line labels are cut to their laid-out width
    REQUIRE(h.settings()->wifiRow != nullptr);
    CHECK(text(h.settings()->wifiStatus) == "Not connected");
    wifi.auto_join();
    app_refresh(h.app, kSummer);
    h.settle();
    CHECK(text(h.settings()->wifiStatus) == "Connected to Home");
    h.click(h.settings()->wifiRow);
    h.settle();
    lv_obj_t* view = app_wifi_view(h.app);
    REQUIRE(view != nullptr);
    REQUIRE(wifi_parts(view) != nullptr);
    CHECK(text(wifi_parts(view)->title) == "Wi-Fi");
    // Back returns to Settings.
    h.click(wifi_parts(view)->back);
    h.settle();
    CHECK(app_wifi_view(h.app) == nullptr);
    CHECK(app_settings_view(h.app) != nullptr);
  }
}

TEST_CASE("app: the brightness slider previews on the backlight and saves on release") {
  Host h;
  app_refresh(h.app, kSummer);
  h.backlight.values.clear();
  app_open_settings(h.app);
  lv_obj_t* slider = h.settings()->brightSlider;
  lv_slider_set_value(slider, 40, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_slider_set_value(slider, 35, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  CHECK(h.backlight.values == std::vector<int>{40, 35});
  CHECK(pref(h.prefs, "bright") == "<missing>");
  lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
  CHECK(pref(h.prefs, "bright") == "35");
  CHECK(app_settings(h.app)->dayBrightness == 35);
  // The next refresh (screen just touched, daytime) keeps 35: no new call.
  app_refresh(h.app, kSummer + 1);
  CHECK(h.backlight.values == std::vector<int>{40, 35});
}

TEST_CASE("app: a refresh during a drag keeps the dragged value; release saves it") {
  Host h;
  app_refresh(h.app, kSummer);
  app_open_settings(h.app);
  const SettingsParts* p = h.settings();
  lv_obj_t* slider = p->brightSlider;
  lv_obj_add_state(slider, LV_STATE_PRESSED);  // the finger is down
  lv_slider_set_value(slider, 55, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  // The finger pauses: refreshes come and go.
  app_refresh(h.app, kSummer + 1);
  CHECK(text(p->brightValue) == "55%");
  CHECK(lv_slider_get_value(slider) == 55);
  app_refresh(h.app, kSummer + 2);
  app_refresh(h.app, kSummer + 3);
  CHECK(text(p->brightValue) == "55%");
  CHECK(lv_slider_get_value(slider) == 55);
  CHECK(h.backlight.values.back() == 55);
  // The finger lifts without moving.
  lv_obj_remove_state(slider, LV_STATE_PRESSED);
  lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
  CHECK(app_settings(h.app)->dayBrightness == 55);
  CHECK(pref(h.prefs, "bright") == "55");
  CHECK(h.backlight.values.back() == 55);
  for (int v : h.backlight.values) CHECK(v != 0);
  app_refresh(h.app, kSummer + 4);
  CHECK(text(p->brightValue) == "55%");
  CHECK(lv_slider_get_value(slider) == 55);
  CHECK(h.backlight.values.back() == 55);
}

TEST_CASE("app: refreshes while the slider is held never move the label or the slider") {
  Host h;
  app_refresh(h.app, kSummer);
  app_open_settings(h.app);
  const SettingsParts* p = h.settings();
  lv_obj_add_state(p->brightSlider, LV_STATE_PRESSED);
  for (int v : {70, 45, 30}) {
    lv_slider_set_value(p->brightSlider, v, LV_ANIM_OFF);
    lv_obj_send_event(p->brightSlider, LV_EVENT_VALUE_CHANGED, nullptr);
    for (int i = 1; i <= 3; ++i) {
      app_refresh(h.app, kSummer + v + i);
      CHECK(text(p->brightValue) == std::to_string(v) + "%");
      CHECK(lv_slider_get_value(p->brightSlider) == v);
    }
  }
  CHECK(pref(h.prefs, "bright") == "<missing>");
}

TEST_CASE("app: a tap on the slider that leaves the value alone writes nothing") {
  Host h;
  app_refresh(h.app, kSummer);
  app_open_settings(h.app);
  const int before = h.prefs.writes;
  lv_obj_t* slider = h.settings()->brightSlider;
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);  // same value
  lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
  CHECK(h.prefs.writes == before);
  // Moved and moved back before lifting: nothing to save either.
  lv_slider_set_value(slider, 60, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_slider_set_value(slider, 100, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
  CHECK(h.prefs.writes == before);
  CHECK(h.backlight.values.back() == 100);
}

TEST_CASE("app: night dimming switch and steppers are saved") {
  Host h;
  app_open_settings(h.app);
  const SettingsParts* p = h.settings();
  h.toggle(p->nightSwitch);
  CHECK(pref(h.prefs, "night") == "0");
  CHECK_FALSE(app_settings(h.app)->nightDim);
  h.toggle(p->nightSwitch);
  CHECK(pref(h.prefs, "night") == "1");
  h.click(p->nightFromPlus);
  CHECK(pref(h.prefs, "night.start") == "0");
  h.click(p->nightToMinus);
  CHECK(pref(h.prefs, "night.end") == "6");
  h.click(p->nightPercentPlus);
  CHECK(pref(h.prefs, "night.pct") == "25");
  CHECK(load_settings(h.prefs).nightPercent == 25);
}

TEST_CASE("app: the reduced motion switch applies and saves") {
  Host h;
  app_open_settings(h.app);
  CHECK_FALSE(goal_reduced_motion());
  h.toggle(h.settings()->motionSwitch);
  CHECK(goal_reduced_motion());
  CHECK(pref(h.prefs, "reduced") == "1");
  h.toggle(h.settings()->motionSwitch);
  CHECK_FALSE(goal_reduced_motion());
  CHECK(pref(h.prefs, "reduced") == "0");
}

// ------------------------------------------------------------ time zones

TEST_CASE("app: choosing a zone applies its offset, saves it and returns to Settings") {
  Host h;
  app_refresh(h.app, kSummer);
  app_open_settings(h.app);
  h.click(h.settings()->zoneRow);
  h.settle();
  lv_obj_t* zv = app_zone_view(h.app);
  REQUIRE(zv != nullptr);
  const ZoneListParts* zp = zone_list_parts(zv);
  REQUIRE(zp != nullptr);
  const ZoneRowParts* ny = nullptr;
  for (const ZoneRowParts& r : zp->rows) {
    if (r.id == "America/New_York") ny = &r;
  }
  REQUIRE(ny != nullptr);
  h.click(ny->row);
  h.settle();
  CHECK(app_zone_view(h.app) == nullptr);
  REQUIRE(app_settings_view(h.app) != nullptr);
  CHECK(h.store.tz_offset() == -14400);
  CHECK(pref(h.prefs, "zone") == "America/New_York");
  CHECK(text(h.settings()->zoneLabel) == "New York, Toronto");
  CHECK(text(h.settings()->zoneOffset) == "UTC-4 \xC2\xB7 Local time 08:00");

  // Later refreshes keep the offset right across daylight saving changes.
  app_refresh(h.app, kWinter);
  CHECK(h.store.tz_offset() == -18000);
  app_refresh(h.app, kUsFallBack - 1);
  CHECK(h.store.tz_offset() == -14400);
  app_refresh(h.app, kUsFallBack);
  CHECK(h.store.tz_offset() == -18000);
}

TEST_CASE("app: choosing a zone at a winter instant gives the winter offset") {
  Host h;
  app_refresh(h.app, kWinter);
  app_open_zones(h.app);
  for (const ZoneRowParts& r : zone_list_parts(app_zone_view(h.app))->rows) {
    if (r.id == "America/New_York") h.click(r.row);
  }
  CHECK(h.store.tz_offset() == -18000);
}

TEST_CASE("app: Back on the zone list returns to Settings without changes") {
  Host h;
  app_open_zones(h.app);
  h.settle();
  REQUIRE(app_zone_view(h.app) != nullptr);
  h.click(zone_list_parts(app_zone_view(h.app))->back);
  h.settle();
  CHECK(app_zone_view(h.app) == nullptr);
  CHECK(app_settings_view(h.app) != nullptr);
  CHECK(pref(h.prefs, "zone") == "<missing>");
}

// ------------------------------------------------------------ backlight

TEST_CASE("app: the backlight dims inside the night window until touched") {
  Host h(false);
  Settings s;
  s.dayBrightness = 80;
  s.nightDim = true;
  s.nightStartHour = 23;
  s.nightEndHour = 7;
  s.nightPercent = 20;
  save_settings(h.prefs, s);
  h.make();

  // Fresh start counts as a touch: day level at once.
  app_refresh(h.app, kNight);
  CHECK(h.backlight.values == std::vector<int>{80});
  // No touch for longer than kTouchWakeMs.
  test_advance(kTouchWakeMs + 1000);
  app_refresh(h.app, kNight + 31);
  CHECK(h.backlight.values == std::vector<int>{80, 20});
  app_refresh(h.app, kNight + 32);  // unchanged: no call
  CHECK(h.backlight.values.size() == 2);
  // A touch wakes it.
  lv_display_trigger_activity(nullptr);
  app_refresh(h.app, kNight + 33);
  CHECK(h.backlight.values == std::vector<int>{80, 20, 80});
  // Daytime without a touch stays at the day level.
  test_advance(kTouchWakeMs + 1000);
  app_refresh(h.app, kSummer);
  CHECK(h.backlight.values == std::vector<int>{80, 20, 80});
}

TEST_CASE("app: the night level never exceeds the day brightness") {
  Host h(false);
  Settings s;
  s.dayBrightness = 50;
  s.nightPercent = 90;
  save_settings(h.prefs, s);
  h.make();
  test_advance(kTouchWakeMs + 1000);
  app_refresh(h.app, kNight);
  CHECK(h.backlight.values == std::vector<int>{50});
}

TEST_CASE("app: with night dimming off the day level applies all night") {
  Host h2(false);
  Settings off;
  off.dayBrightness = 70;
  off.nightDim = false;
  save_settings(h2.prefs, off);
  h2.make();
  test_advance(kTouchWakeMs + 1000);
  app_refresh(h2.app, kNight);
  CHECK(h2.backlight.values == std::vector<int>{70});
}

TEST_CASE("app: the night window follows the chosen zone") {
  Host h(false);
  Settings s;
  s.zoneId = "Asia/Tokyo";  // 02:00 UTC is 11:00 in Tokyo
  s.dayBrightness = 100;
  save_settings(h.prefs, s);
  h.make();
  test_advance(kTouchWakeMs + 1000);
  app_refresh(h.app, kNight);
  CHECK(h.backlight.values == std::vector<int>{100});
  app_refresh(h.app, kNight + 13 * 3600);  // 00:00 in Tokyo
  CHECK(h.backlight.values == std::vector<int>{100, 20});
}

// ------------------------------------------------------------ favourite team

TEST_CASE("app: app_apply_team recolours, saves and survives a restart") {
  Host h;
  app_refresh(h.app, kSummer);
  app_apply_team(h.app, liverpool());
  h.settle();
  CHECK(h.store.favourite() == "364");
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xC8102E)));
  CHECK(u32(lv_obj_get_style_text_color(h.tab_icon(0), 0)) == u32(theme().accent));
  CHECK(pref(h.prefs, "fav.id") == "364");
  CHECK(pref(h.prefs, "fav.name") == "Liverpool");
  CHECK(pref(h.prefs, "fav.color") == "C8102E");
  CHECK(pref(h.prefs, "fav.league") == "eng.1");

  // A fresh app over the same prefs restores it.
  lv_obj_delete(h.app);
  theme_reset();
  h.store.set_favourite("");
  h.make();
  CHECK(h.store.favourite() == "364");
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xC8102E)));
}

TEST_CASE("app: applying a team rebuilds the tabs without leaking and keeps the tab") {
  Host h;
  app_refresh(h.app, kSummer);
  lv_tabview_set_active(h.app, 1, LV_ANIM_OFF);
  h.settle();
  const uint32_t before = descendants(h.app);
  lv_obj_t* oldLive = h.tab_screen(0);

  app_apply_team(h.app, liverpool());
  h.settle();
  app_apply_team(h.app, tottenham());
  h.settle();
  app_apply_team(h.app, arsenal());
  h.settle();
  CHECK(descendants(h.app) == before);
  CHECK(lv_tabview_get_tab_active(h.app) == 1);
  CHECK(h.tab_screen(0) != oldLive);
  CHECK(live_parts(h.tab_screen(0)) != nullptr);
  // The rebuilt Live screen still has a working gear.
  h.click(live_parts(h.tab_screen(0))->gear);
  CHECK(app_settings_view(h.app) != nullptr);
}

TEST_CASE("app: the favourite's brand colour is applied, saved and restored") {
  Host h;
  app_apply_team(h.app, tottenham());
  h.settle();
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xFFFFFF)));
  CHECK(u32(theme().brand) == u32(lv_color_hex(0x132257)));
  CHECK(pref(h.prefs, "fav.primary") == "132257");

  lv_obj_delete(h.app);
  theme_reset();
  h.store.set_favourite("");
  h.make();
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xFFFFFF)));
  CHECK(u32(theme().brand) == u32(lv_color_hex(0x132257)));
}

// ------------------------------------------------------------ saved favourite vs team list

namespace {

Team chelsea_listed() {
  return make_team("363", "CHE", "Chelsea", 0xDBA111, "eng.1", 0x034694);
}

// Prefs as an older build saved them: no fav.primary.
void save_old_chelsea(MemoryPrefs& p, const char* color) {
  p.set("fav.id", "363");
  p.set("fav.abbr", "CHE");
  p.set("fav.name", "Chelsea FC");
  p.set("fav.color", color);
  p.set("fav.crest", "/old-crest");
  p.set("fav.league", "eng.1");
}

}  // namespace

TEST_CASE("app: a saved favourite without a primary is refreshed from the team list") {
  Host h(false);
  save_old_chelsea(h.prefs, "034694");  // an old accent, no primary
  h.make();
  CHECK(u32(theme().brand) == u32(lv_color_hex(0x034694)));
  lv_obj_t* oldLive = h.tab_screen(0);

  h.store.set_teams({arsenal(), chelsea_listed()});
  app_refresh(h.app, kSummer);
  h.settle();
  const Settings* s = app_settings(h.app);
  REQUIRE(s != nullptr);
  CHECK(s->favourite.color == 0xDBA111u);
  CHECK(s->favourite.primary == 0x034694u);
  CHECK(s->favourite.name == "Chelsea");
  CHECK(s->favourite.crest == chelsea_listed().crest);
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xDBA111)));
  CHECK(u32(theme().brand) == u32(lv_color_hex(0x034694)));
  CHECK(pref(h.prefs, "fav.color") == "DBA111");
  CHECK(pref(h.prefs, "fav.primary") == "034694");
  CHECK(pref(h.prefs, "fav.name") == "Chelsea");
  // The screens were rebuilt in the new colours.
  CHECK(h.tab_screen(0) != oldLive);
  CHECK(u32(lv_obj_get_style_text_color(h.tab_icon(0), 0)) == u32(lv_color_hex(0xDBA111)));
}

TEST_CASE("app: an old gold-only Chelsea gets its blue brand once the teams arrive") {
  Host h(false);
  save_old_chelsea(h.prefs, "DBA111");  // primary defaults to the gold accent
  h.make();
  CHECK(u32(theme().brand) == u32(lv_color_hex(0xDBA111)));
  h.store.set_teams({chelsea_listed()});
  app_refresh(h.app, kSummer);
  CHECK(app_settings(h.app)->favourite.primary == 0x034694u);
  CHECK(u32(theme().brand) == u32(lv_color_hex(0x034694)));
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xDBA111)));
  CHECK(pref(h.prefs, "fav.primary") == "034694");
}

TEST_CASE("app: a favourite equal to its list entry is not saved or rebuilt") {
  Host h(false);
  Settings s;
  s.favourite = chelsea_listed();
  save_settings(h.prefs, s);
  h.make();
  app_refresh(h.app, kSummer);
  h.settle();
  lv_obj_t* live = h.tab_screen(0);
  const int writes = h.prefs.writes;
  h.store.set_teams({arsenal(), chelsea_listed()});
  app_refresh(h.app, kSummer);
  app_refresh(h.app, kSummer + 1);
  CHECK(h.prefs.writes == writes);
  CHECK(h.tab_screen(0) == live);
}

TEST_CASE("app: a favourite missing from the team list is left untouched") {
  Host h(false);
  save_old_chelsea(h.prefs, "034694");
  h.make();
  app_refresh(h.app, kSummer);
  h.settle();
  lv_obj_t* live = h.tab_screen(0);
  const int writes = h.prefs.writes;
  h.store.set_teams({arsenal(), liverpool()});
  app_refresh(h.app, kSummer);
  CHECK(h.prefs.writes == writes);
  CHECK(h.tab_screen(0) == live);
  CHECK(app_settings(h.app)->favourite.color == 0x034694u);
  CHECK(app_settings(h.app)->favourite.primary == 0x034694u);
  CHECK(app_settings(h.app)->favourite.crest == "/old-crest");
}

TEST_CASE("app: a name or crest change is saved without rebuilding the screens") {
  Host h(false);
  Settings s;
  s.favourite = chelsea_listed();
  s.favourite.crest = "/old-crest";
  save_settings(h.prefs, s);
  h.make();
  app_refresh(h.app, kSummer);
  h.settle();
  lv_obj_t* live = h.tab_screen(0);
  h.store.set_teams({chelsea_listed()});
  app_refresh(h.app, kSummer);
  CHECK(pref(h.prefs, "fav.crest") == chelsea_listed().crest);
  CHECK(h.tab_screen(0) == live);
}

TEST_CASE("app: a white accent keeps the active tab icon at the accent") {
  Host h;
  app_apply_team(h.app, tottenham());
  CHECK(u32(lv_obj_get_style_text_color(h.tab_icon(0), 0)) == u32(lv_color_hex(0xFFFFFF)));
}

// ------------------------------------------------------------ team picker

TEST_CASE("app: the picker fetches the team list once when the Store has none") {
  Host h;
  app_open_settings(h.app);
  h.click(h.settings()->teamRow);
  h.settle();
  lv_obj_t* picker = app_team_picker_view(h.app);
  REQUIRE(picker != nullptr);
  CHECK(h.source.teamCalls == 1);
  CHECK(text(team_picker_parts(picker)->message) == kTeamsLoading);
  // The picker covers the whole screen, tab bar included.
  lv_obj_t* overlay = lv_obj_get_parent(picker);
  CHECK(lv_obj_get_height(overlay) == 320);

  h.source.complete_teams(true);
  h.settle();
  CHECK(h.store.teams().size() == 4);
  CHECK(team_picker_rows(picker).size() == 4);
  CHECK_FALSE(visible(team_picker_parts(picker)->message));

  // Opening again uses the Store.
  lv_obj_send_event(team_picker_parts(picker)->back, LV_EVENT_CLICKED, nullptr);
  h.settle();
  CHECK(app_team_picker_view(h.app) == nullptr);
  app_open_team_picker(h.app);
  CHECK(h.source.teamCalls == 1);
  CHECK(team_picker_rows(app_team_picker_view(h.app)).size() == 4);
}

TEST_CASE("app: a failed team fetch shows the error; Try again fetches again") {
  Host h;
  app_open_team_picker(h.app);
  lv_obj_t* picker = app_team_picker_view(h.app);
  h.source.complete_teams(false);
  h.settle();
  const TeamPickerParts* p = team_picker_parts(picker);
  CHECK(text(p->message) == kTeamsFailed);
  REQUIRE(visible(p->retry));
  lv_obj_send_event(p->retry, LV_EVENT_CLICKED, nullptr);
  CHECK(h.source.teamCalls == 2);
  CHECK(text(p->message) == kTeamsLoading);
  h.source.complete_teams(true);
  CHECK(team_picker_rows(picker).size() == 4);
}

TEST_CASE("app: a team list arriving after the picker closed only fills the Store") {
  Host h;
  app_open_team_picker(h.app);
  lv_obj_send_event(team_picker_parts(app_team_picker_view(h.app))->back, LV_EVENT_CLICKED,
                    nullptr);
  h.settle();
  CHECK(app_team_picker_view(h.app) == nullptr);
  h.source.complete_teams(true);
  h.settle();
  CHECK(h.store.teams().size() == 4);
}

TEST_CASE("app: a team list arriving after the app is gone does nothing") {
  Host h;
  app_open_team_picker(h.app);
  lv_obj_clean(lv_screen_active());
  h.settle();
  h.source.complete_teams(true);
  CHECK(h.store.teams().empty());
}

TEST_CASE("app: teams fetched elsewhere reach an open picker on refresh") {
  Host h;
  app_open_team_picker(h.app);
  lv_obj_t* picker = app_team_picker_view(h.app);
  h.store.set_teams({arsenal(), liverpool()});
  app_refresh(h.app, kSummer);
  CHECK(team_picker_rows(picker).size() == 2);
}

TEST_CASE("app: picking a team in the picker applies it and returns to Settings") {
  Host h;
  h.source.immediate = true;
  app_refresh(h.app, kSummer);
  app_open_settings(h.app);
  h.click(h.settings()->teamRow);
  lv_obj_t* picker = app_team_picker_view(h.app);
  REQUIRE(picker != nullptr);
  team_picker_set_query(picker, "liv");
  const std::vector<TeamRowParts> rows = team_picker_rows(picker);
  REQUIRE(rows.size() == 1);
  h.click(rows[0].row);
  h.settle();
  CHECK(app_team_picker_view(h.app) == nullptr);
  CHECK(h.store.favourite() == "364");
  REQUIRE(app_settings_view(h.app) != nullptr);
  CHECK(text(h.settings()->teamName) == "Liverpool");
  CHECK(pref(h.prefs, "fav.id") == "364");
}

// ------------------------------------------------------------ Wi-Fi

namespace {

class ConnectedWifi : public MockWifi {
 public:
  WifiStatus status() override {
    WifiStatus s;
    s.connected = true;
    s.ssid = "Lab";
    s.ip = "10.0.0.7";
    return s;
  }
};

MockWifi demo_wifi() {
  MockWifi w;
  w.set_networks({{"Home", -50, false}, {"Cafe", -70, true}});
  w.set_password("Home", "homehome1");
  return w;
}

}  // namespace

TEST_CASE("app: first start without a saved network opens the guided Wi-Fi setup") {
  MockWifi wifi = demo_wifi();
  Host h(false);
  h.wifi = &wifi;
  h.make();
  lv_obj_t* view = app_wifi_view(h.app);
  REQUIRE(view != nullptr);
  const WifiParts* p = wifi_parts(view);
  REQUIRE(p != nullptr);
  CHECK(text(p->title) == "Connect to Wi-Fi");
  CHECK(visible(p->help));
  CHECK(text(p->help) == "Pick your network to get live scores.");
  // Full screen, above the tab view.
  lv_obj_t* overlay = lv_obj_get_parent(view);
  CHECK(lv_obj_get_height(overlay) == 320);
  CHECK(lv_obj_get_index(overlay) > lv_obj_get_index(h.app));

  // Joining closes the setup after the "Connected" pause.
  wifi_open_password(view, "Home");
  lv_textarea_set_text(wifi_password_parts(view)->field, "homehome1");
  h.click(wifi_password_parts(view)->connect);
  h.settle();
  CHECK(app_wifi_view(h.app) != nullptr);
  test_advance(kWifiConnectedHoldMs + 50);
  h.settle();
  CHECK(app_wifi_view(h.app) == nullptr);
  CHECK(wifi.status().connected);
}

TEST_CASE("app: Back leaves the guided setup") {
  MockWifi wifi = demo_wifi();
  Host h(false);
  h.wifi = &wifi;
  h.make();
  REQUIRE(app_wifi_view(h.app) != nullptr);
  h.click(wifi_parts(app_wifi_view(h.app))->back);
  h.settle();
  CHECK(app_wifi_view(h.app) == nullptr);
}

TEST_CASE("app: no guided setup with a saved network, when connected, or without Wi-Fi") {
  {
    MockWifi wifi = demo_wifi();
    wifi.set_saved({"Somewhere"});
    Host h(false);
    h.wifi = &wifi;
    h.make();
    CHECK(app_wifi_view(h.app) == nullptr);
  }
  {
    // Connected (for example by the firmware) though nothing is saved.
    ConnectedWifi wifi;
    REQUIRE(wifi.saved().empty());
    Host h(false);
    h.wifi = &wifi;
    h.make();
    CHECK(app_wifi_view(h.app) == nullptr);
  }
  {
    Host h;
    CHECK(app_wifi_view(h.app) == nullptr);
    app_open_wifi(h.app);
    CHECK(app_wifi_view(h.app) == nullptr);
  }
}

TEST_CASE("app: joining from Settings keeps Wi-Fi open and updates the Settings row") {
  MockWifi wifi = demo_wifi();
  wifi.set_saved({"Home"});
  Host h(false);
  h.wifi = &wifi;
  h.make();
  app_open_settings(h.app);
  app_open_wifi(h.app);
  lv_obj_t* view = app_wifi_view(h.app);
  REQUIRE(view != nullptr);
  CHECK(text(wifi_parts(view)->title) == "Wi-Fi");
  std::vector<WifiSavedRow> rows = wifi_saved_rows(view);
  REQUIRE(rows.size() == 1);
  h.click(rows[0].row);
  h.settle();
  CHECK(app_wifi_view(h.app) == view);
  h.settle();
  CHECK(text(h.settings()->wifiStatus) == "Connected to Home");
  app_close_wifi(h.app);
  CHECK(app_wifi_view(h.app) == nullptr);
  CHECK(app_settings_view(h.app) != nullptr);
}

TEST_CASE("app: Wi-Fi results after the overlay closed or the app is gone do nothing") {
  {
    MockWifi wifi = demo_wifi();
    wifi.set_async(true);
    Host h(false);
    h.wifi = &wifi;
    h.make();
    REQUIRE(app_wifi_view(h.app) != nullptr);
    app_close_wifi(h.app);
    wifi.poll(MockWifi::kDefaultScanMs);
    h.settle();
    CHECK(app_wifi_view(h.app) == nullptr);
  }
  {
    MockWifi wifi = demo_wifi();
    wifi.set_async(true);
    Host h(false);
    h.wifi = &wifi;
    h.make();
    lv_obj_t* view = app_wifi_view(h.app);
    wifi.poll(MockWifi::kDefaultScanMs);
    std::vector<WifiNearbyRow> rows = wifi_nearby_rows(view);
    REQUIRE(rows.size() == 2);
    h.click(rows[1].row);  // Cafe, open: joins at once
    CHECK(wifi.pending() == 1);
    lv_obj_clean(lv_screen_active());
    h.settle();
    wifi.poll(MockWifi::kDefaultConnectMs);
    h.settle();
    CHECK(wifi.status().connected);
  }
}

// ------------------------------------------------------------ wifi connected

TEST_CASE("app: a first-setup join calls onWifiConnected once") {
  MockWifi wifi = demo_wifi();
  Host h(false);
  h.wifi = &wifi;
  h.make();
  lv_obj_t* view = app_wifi_view(h.app);
  REQUIRE(view != nullptr);
  // A wrong password first: no call.
  wifi_open_password(view, "Home");
  lv_textarea_set_text(wifi_password_parts(view)->field, "wrongwrong");
  h.click(wifi_password_parts(view)->connect);
  h.settle();
  test_advance(kWifiConnectedHoldMs + 50);
  CHECK(h.wifiConnected == 0);
  lv_textarea_set_text(wifi_password_parts(view)->field, "homehome1");
  h.click(wifi_password_parts(view)->connect);
  h.settle();
  test_advance(kWifiConnectedHoldMs + 50);
  h.settle();
  CHECK(app_wifi_view(h.app) == nullptr);
  CHECK(h.wifiConnected == 1);
  test_advance(kWifiConnectedHoldMs + 50);
  CHECK(h.wifiConnected == 1);
}

TEST_CASE("app: a join from Settings calls onWifiConnected once per success") {
  MockWifi wifi = demo_wifi();
  wifi.set_saved({"Home"});
  Host h(false);
  h.wifi = &wifi;
  h.make();
  app_open_settings(h.app);
  app_open_wifi(h.app);
  lv_obj_t* view = app_wifi_view(h.app);
  REQUIRE(view != nullptr);
  std::vector<WifiSavedRow> rows = wifi_saved_rows(view);
  REQUIRE(rows.size() == 1);
  h.click(rows[0].row);
  h.settle();
  test_advance(kWifiConnectedHoldMs + 50);
  h.settle();
  CHECK(h.wifiConnected == 1);
  CHECK(app_wifi_view(h.app) == view);  // normal mode stays open
}

TEST_CASE("app: a failed open-network join does not call onWifiConnected") {
  MockWifi wifi = demo_wifi();
  wifi.set_saved({"Somewhere"});
  Host h(false);
  h.wifi = &wifi;
  h.make();
  app_open_wifi(h.app);
  lv_obj_t* view = app_wifi_view(h.app);
  REQUIRE(view != nullptr);
  wifi.set_networks({{"Home", -50, false}});  // Cafe went out of range: its join fails
  bool clicked = false;
  for (const WifiNearbyRow& r : wifi_nearby_rows(view)) {
    if (r.network.ssid == "Cafe") {
      h.click(r.row);
      clicked = true;
    }
  }
  REQUIRE(clicked);
  h.settle();
  test_advance(kWifiConnectedHoldMs + 50);
  CHECK(h.wifiConnected == 0);
}

// ------------------------------------------------------------ preview reset

TEST_CASE("app: closing Settings mid-drag ends the preview and restores the saved brightness") {
  Host h(false);
  Settings st;
  st.dayBrightness = 80;
  st.nightDim = true;
  st.nightStartHour = 23;
  st.nightEndHour = 7;
  st.nightPercent = 20;
  save_settings(h.prefs, st);
  h.make();
  app_refresh(h.app, kSummer);
  REQUIRE(h.backlight.values.back() == 80);
  app_open_settings(h.app);
  lv_obj_t* slider = h.settings()->brightSlider;
  lv_obj_add_state(slider, LV_STATE_PRESSED);
  lv_slider_set_value(slider, 40, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);  // no RELEASED follows
  CHECK(h.backlight.values.back() == 40);
  app_close_settings(h.app);
  CHECK(app_settings(h.app)->dayBrightness == 80);
  CHECK(pref(h.prefs, "bright") == "80");
  CHECK(h.backlight.values.back() == 80);  // restored at once
  // The schedule drives the backlight again: night dimming without a touch.
  test_advance(kTouchWakeMs + 1000);
  app_refresh(h.app, kNight);
  CHECK(h.backlight.values.back() == 20);
}

TEST_CASE("app: applying a team mid-drag ends the preview too") {
  Host h;
  app_refresh(h.app, kSummer);
  const int saved = app_settings(h.app)->dayBrightness;
  app_open_settings(h.app);
  lv_obj_t* slider = h.settings()->brightSlider;
  lv_slider_set_value(slider, 30, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  app_apply_team(h.app, liverpool());
  CHECK(app_settings(h.app)->dayBrightness == saved);
  CHECK(h.backlight.values.back() == saved);
}

// ------------------------------------------------------------ wifi connected, async

namespace {

// An app whose Wi-Fi answers later (MockWifi::poll), with the list scanned.
struct AsyncWifiHost {
  MockWifi wifi = demo_wifi();
  Host h{false};
  explicit AsyncWifiHost(bool firstSetup) {
    wifi.set_async(true);
    if (!firstSetup) wifi.set_saved({"Somewhere"});
    h.wifi = &wifi;
    h.make();
    if (!firstSetup) app_open_wifi(h.app);
    wifi.poll(MockWifi::kDefaultScanMs);
    h.settle();
  }
  lv_obj_t* view() const { return app_wifi_view(h.app); }
  void type_and_connect(const char* ssid, const char* password) {
    wifi_open_password(view(), ssid);
    lv_textarea_set_text(wifi_password_parts(view())->field, password);
    h.click(wifi_password_parts(view())->connect);
  }
  void tap_nearby(const char* ssid) {
    for (const WifiNearbyRow& r : wifi_nearby_rows(view())) {
      if (r.network.ssid == ssid) h.click(r.row);
    }
  }
  void answer() {
    wifi.poll(MockWifi::kDefaultConnectMs);
    h.settle();
    test_advance(kWifiConnectedHoldMs + 50);
    h.settle();
  }
};

}  // namespace

TEST_CASE("app: Back on the password page before the join answers still notifies once") {
  AsyncWifiHost a(false);
  a.type_and_connect("Home", "homehome1");
  a.h.click(wifi_password_parts(a.view())->back);
  a.h.settle();
  CHECK(a.h.wifiConnected == 0);
  a.answer();
  CHECK(a.h.wifiConnected == 1);
  CHECK(a.wifi.status().connected);
  a.answer();
  CHECK(a.h.wifiConnected == 1);
}

TEST_CASE("app: closing the Wi-Fi overlay before the join answers still notifies once") {
  AsyncWifiHost a(false);
  a.type_and_connect("Home", "homehome1");
  app_close_wifi(a.h.app);
  CHECK(app_wifi_view(a.h.app) == nullptr);
  a.answer();
  CHECK(a.h.wifiConnected == 1);
}

TEST_CASE("app: first setup closed before the join answers still notifies once") {
  AsyncWifiHost a(true);
  REQUIRE(a.view() != nullptr);
  a.type_and_connect("Home", "homehome1");
  a.h.click(wifi_password_parts(a.view())->back);
  a.h.click(wifi_parts(a.view())->back);  // leaves the guided setup
  a.h.settle();
  CHECK(app_wifi_view(a.h.app) == nullptr);
  a.answer();
  CHECK(a.h.wifiConnected == 1);
}

TEST_CASE("app: an in-page success notifies exactly once (async)") {
  for (bool first : {false, true}) {
    INFO("first setup: ", first);
    AsyncWifiHost a(first);
    a.type_and_connect("Home", "homehome1");
    a.answer();
    CHECK(a.h.wifiConnected == 1);
    a.answer();
    CHECK(a.h.wifiConnected == 1);
  }
}

TEST_CASE("app: a failed async join never notifies") {
  AsyncWifiHost a(false);
  a.type_and_connect("Home", "wrongwrong");
  a.answer();
  CHECK(a.h.wifiConnected == 0);
  CHECK_FALSE(a.wifi.status().connected);
}

TEST_CASE("app: a replaced join does not notify; the replacing success notifies once") {
  AsyncWifiHost a(false);
  a.type_and_connect("Home", "homehome1");
  a.h.click(wifi_password_parts(a.view())->back);
  a.h.settle();
  a.tap_nearby("Cafe");  // open network: replaces the Home attempt
  a.h.settle();
  CHECK(a.h.wifiConnected == 0);
  a.answer();
  CHECK(a.h.wifiConnected == 1);
  CHECK(a.wifi.status().ssid == "Cafe");
}

TEST_CASE("app: a join answered after the app is deleted notifies nobody") {
  AsyncWifiHost a(false);
  a.type_and_connect("Home", "homehome1");
  lv_obj_delete(a.h.app);  // the Wi-Fi overlay is a sibling: it stays
  a.h.app = nullptr;
  a.answer();
  CHECK(a.h.wifiConnected == 0);
  CHECK(a.wifi.status().connected);
}
