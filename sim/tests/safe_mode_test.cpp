// Safe mode in the app (AppDeps::safeMode): the banner "Safe mode: updates
// paused" sits at the top of the tabs without hiding or blocking the Settings
// gear; Settings and the Wi-Fi pages work as usual and cover the banner while
// they are open.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "mock_wifi.h"
#include "prefs.h"
#include "screens/battery_banner.h"
#include "screens/live.h"
#include "screens/settings.h"
#include "screens/wifi.h"
#include "test_display.h"
#include "theme.h"

namespace {

constexpr int64_t kNow = 1784116800;  // 2026-07-15 12:00 UTC

class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

class CountingSource : public DataSource {
 public:
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    ++calls;
    cb(false, {});
  }
  void fetch_detail(const std::string&, std::function<void(bool, MatchDetail)> cb) override {
    ++calls;
    cb(false, {});
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override {
    ++calls;
    cb(false, {});
  }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override {
    ++calls;
    cb(false, {});
  }
  int calls = 0;
};

struct Host {
  Store store;
  NoImages images;
  CountingSource source;
  MemoryPrefs prefs;
  MockWifi wifi;
  lv_obj_t* app = nullptr;

  explicit Host(bool safe) {
    init_test_display();
    theme_reset();
    wifi.set_networks({{"Home", -50, false}});
    wifi.set_password("Home", "correct horse");
    wifi.set_saved({"Home"});
    wifi.auto_join();
    AppDeps deps{store, images, source, nullptr, prefs};
    deps.wifi = &wifi;
    deps.safeMode = safe;
    app = app_create(lv_screen_active(), deps);
    app_refresh(app, kNow);
    settle();
  }
  ~Host() {
    theme_reset();
    deinit_test_display();
  }
  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  lv_obj_t* live_screen() const {
    lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), 0);
    return lv_obj_get_child(tab, 0);
  }
  // What a finger at (x, y) would press: the topmost clickable object there.
  lv_obj_t* hit(int32_t x, int32_t y) const {
    lv_point_t p{x, y};
    return lv_indev_search_obj(lv_screen_active(), &p);
  }
};

bool inside(lv_obj_t* o, lv_obj_t* ancestor) {
  for (; o != nullptr; o = lv_obj_get_parent(o)) {
    if (o == ancestor) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("safe mode: no banner in normal mode") {
  Host h(false);
  CHECK(app_safe_banner(h.app) == nullptr);
}

TEST_CASE("safe mode: the banner says so, at the top, clear of the gear, and takes no touches") {
  Host h(true);
  lv_obj_t* b = app_safe_banner(h.app);
  REQUIRE(b != nullptr);
  lv_obj_t* text = safe_mode_banner_text(b);
  REQUIRE(text != nullptr);
  CHECK(std::string(lv_label_get_text(text)) == "Safe mode: updates paused");
  CHECK(lv_obj_get_y(b) == 0);
  CHECK(lv_obj_get_x(b) == 0);
  CHECK(lv_obj_get_height(b) == kBatteryBannerHeight);
  CHECK(lv_obj_get_width(b) == 240 - kSafeModeGearRoom);
  CHECK_FALSE(lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE));
  CHECK(lv_obj_get_index(b) > lv_obj_get_index(h.app));  // above the tabs
  // The gear (top right) is not under the banner and a finger there presses it.
  const LiveParts* live = live_parts(h.live_screen());
  REQUIRE(live != nullptr);
  REQUIRE(live->gear != nullptr);
  lv_area_t g;
  lv_obj_get_coords(live->gear, &g);
  CHECK(g.x1 >= 240 - kSafeModeGearRoom);
  CHECK(inside(h.hit((g.x1 + g.x2) / 2, (g.y1 + g.y2) / 2), live->gear));
  // A finger on the banner reaches what lies below it, never the banner.
  lv_obj_t* under = h.hit(20, 10);
  CHECK_FALSE(inside(under, b));
  // It is not a battery banner (no tap-to-dismiss, no timer).
  CHECK(battery_banner_parts(b) == nullptr);
  CHECK(app_battery_banner(h.app) == nullptr);
}

TEST_CASE("safe mode: Settings and the Wi-Fi pages open over the banner and work") {
  Host h(true);
  lv_obj_t* b = app_safe_banner(h.app);
  REQUIRE(b != nullptr);
  const LiveParts* live = live_parts(h.live_screen());
  lv_obj_send_event(live->gear, LV_EVENT_CLICKED, nullptr);
  h.settle();
  lv_obj_t* view = app_settings_view(h.app);
  REQUIRE(view != nullptr);
  CHECK(lv_obj_get_index(lv_obj_get_parent(view)) > lv_obj_get_index(b));  // Settings covers it
  const SettingsParts* p = settings_parts(view);
  REQUIRE(p != nullptr);
  REQUIRE(p->wifiRow != nullptr);
  CHECK(p->updateRow == nullptr);  // no updater in safe mode
  lv_obj_send_event(p->wifiRow, LV_EVENT_CLICKED, nullptr);
  h.settle();
  REQUIRE(app_wifi_view(h.app) != nullptr);
  CHECK(wifi_parts(app_wifi_view(h.app)) != nullptr);
  // Back out of both: the banner is still there.
  app_close_settings(h.app);
  h.settle();
  CHECK(app_safe_banner(h.app) == b);
  CHECK_FALSE(lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE("safe mode: a battery banner shows above it and the safe banner returns after") {
  Host h(true);
  const Alert a{Alert::Level::Low, "Battery low", 1000};
  app_show_battery_banner(h.app, a);
  h.settle();
  lv_obj_t* bat = app_battery_banner(h.app);
  REQUIRE(bat != nullptr);
  CHECK(lv_obj_get_index(bat) > lv_obj_get_index(app_safe_banner(h.app)));
  test_advance(1100);
  h.settle();
  CHECK(app_battery_banner(h.app) == nullptr);
  CHECK(app_safe_banner(h.app) != nullptr);
}
