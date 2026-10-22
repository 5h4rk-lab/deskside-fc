// What the power key shows: the screen off (touch wakes it), the "Hold to
// power off" hint and the "Powering off" screen; and app_save_settings.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "power_policy.h"
#include "prefs.h"
#include "screens/goal.h"
#include "test_display.h"
#include "theme.h"

namespace {

constexpr int64_t kNoon = 1784116800;  // 2026-07-15 12:00 UTC
constexpr int64_t kMidnight = 1784073600 + 3600;  // 2026-07-15 01:00 UTC, inside the default night window

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

class FakeBacklight : public Backlight {
 public:
  void set_percent(uint8_t p) override { values.push_back(p); }
  int last() const { return values.empty() ? -1 : values.back(); }
  std::vector<int> values;
};

struct AppHost {
  Store store;
  NoImages images;
  NoSource source;
  MemoryPrefs prefs;
  FakeBacklight backlight;
  lv_obj_t* app = nullptr;

  AppHost() {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(true);
    AppDeps deps{store, images, source, nullptr, prefs};
    deps.backlight = &backlight;
    app = app_create(lv_screen_active(), deps);
    test_advance(20);
  }
  ~AppHost() {
    goal_dismiss();
    theme_reset();
    goal_set_reduced_motion(false);
    deinit_test_display();
  }
  void refresh(int64_t now = kNoon) {
    app_refresh(app, now);
    lv_obj_update_layout(lv_screen_active());
  }
};

bool visible(lv_obj_t* obj) {
  for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

lv_area_t coords(lv_obj_t* o) {
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  return a;
}

// The first label under `obj` (depth first) whose text is `text`.
lv_obj_t* find_label(lv_obj_t* obj, const std::string& text) {
  if (lv_obj_check_type(obj, &lv_label_class) && text == lv_label_get_text(obj)) return obj;
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
    if (lv_obj_t* f = find_label(lv_obj_get_child(obj, static_cast<int32_t>(i)), text)) return f;
  }
  return nullptr;
}

}  // namespace

// ------------------------------------------------------------ screen off

TEST_CASE("screen off: the backlight goes to 0 and stays there through refreshes") {
  AppHost h;
  h.refresh();
  CHECK(h.backlight.last() == 100);
  app_set_screen_off(h.app, true);
  CHECK(app_screen_off(h.app));
  CHECK(h.backlight.last() == 0);
  for (int i = 1; i <= 5; ++i) h.refresh(kNoon + i);
  CHECK(h.backlight.last() == 0);
  // Touch activity (night dimming's wake) does not light it either.
  lv_display_trigger_activity(nullptr);
  h.refresh(kNoon + 6);
  CHECK(h.backlight.last() == 0);
}

namespace {

// A virtual finger: a pointer input device read by LVGL's own indev timer.
struct Finger {
  static Finger*& current() {
    static Finger* f = nullptr;
    return f;
  }
  lv_indev_t* indev = nullptr;
  int32_t x = 0, y = 0;
  bool down = false;
  Finger() {
    current() = this;
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, [](lv_indev_t*, lv_indev_data_t* d) {
      Finger* f = current();
      d->point.x = f->x;
      d->point.y = f->y;
      d->state = f->down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
  }
  ~Finger() { current() = nullptr; }
  // Down for `cycles` indev periods at (x, y), then up for a few more.
  void tap(int32_t px, int32_t py, int cycles = 6) {
    x = px;
    y = py;
    down = true;
    for (int i = 0; i < cycles; ++i) test_advance(LV_DEF_REFR_PERIOD + 5);
    down = false;
    for (int i = 0; i < 4; ++i) test_advance(LV_DEF_REFR_PERIOD + 5);
  }
};

struct Counter {
  int pressed = 0, clicked = 0, released = 0;
};

void count_event(lv_event_t* e) {
  auto* c = static_cast<Counter*>(lv_event_get_user_data(e));
  switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: ++c->pressed; break;
    case LV_EVENT_CLICKED: ++c->clicked; break;
    case LV_EVENT_RELEASED: ++c->released; break;
    default: break;
  }
}

}  // namespace

TEST_CASE("screen off: the waking touch (a real pointer, held, then lifted) never reaches the button below") {
  AppHost h;
  h.refresh();
  Finger finger;
  // A button over everything on the screen, where the finger lands.
  lv_obj_t* button = lv_button_create(lv_screen_active());
  lv_obj_set_size(button, 100, 60);
  lv_obj_set_pos(button, 70, 130);
  Counter c;
  lv_obj_add_event_cb(button, count_event, LV_EVENT_ALL, &c);
  test_advance(40);

  app_set_screen_off(h.app, true);
  REQUIRE(app_screen_off_shield(h.app) != nullptr);
  CHECK(lv_obj_get_parent(app_screen_off_shield(h.app)) == lv_layer_sys());
  finger.x = 120;
  finger.y = 160;
  finger.down = true;
  for (int i = 0; i < 6; ++i) test_advance(LV_DEF_REFR_PERIOD + 5);
  // Lit at the touch, while the finger is still down.
  CHECK_FALSE(app_screen_off(h.app));
  CHECK(h.backlight.last() == 100);
  CHECK(c.pressed == 0);
  finger.down = false;
  for (int i = 0; i < 4; ++i) test_advance(LV_DEF_REFR_PERIOD + 5);
  CHECK(c.pressed == 0);
  CHECK(c.released == 0);
  CHECK(c.clicked == 0);
  CHECK(app_screen_off_shield(h.app) == nullptr);
  CHECK(lv_obj_get_child_count(lv_layer_sys()) == 0);

  // The next tap is an ordinary tap.
  finger.tap(120, 160);
  CHECK(c.pressed == 1);
  CHECK(c.clicked == 1);
  CHECK_FALSE(app_screen_off(h.app));
}

TEST_CASE("screen off: the key turning it off and on during a held touch keeps one shield") {
  AppHost h;
  h.refresh();
  Finger finger;
  app_set_screen_off(h.app, true);
  finger.x = 50;
  finger.y = 50;
  finger.down = true;
  for (int i = 0; i < 3; ++i) test_advance(LV_DEF_REFR_PERIOD + 5);
  CHECK_FALSE(app_screen_off(h.app));
  app_set_screen_off(h.app, true);  // the key, finger still down
  CHECK(h.backlight.last() == 0);
  CHECK(lv_obj_get_child_count(lv_layer_sys()) == 1);
  finger.down = false;
  for (int i = 0; i < 4; ++i) test_advance(LV_DEF_REFR_PERIOD + 5);
  CHECK(app_screen_off(h.app));  // still off: the shield stays for the next touch
  REQUIRE(app_screen_off_shield(h.app) != nullptr);
  finger.tap(50, 50);
  CHECK_FALSE(app_screen_off(h.app));
  CHECK(app_screen_off_shield(h.app) == nullptr);
}

TEST_CASE("screen off: the key turns it back on to the level the schedule wants") {
  AppHost h;
  h.refresh(kMidnight);
  lv_display_t* d = lv_display_get_default();
  (void)d;
  // Night, untouched for a while: the night level.
  test_display::fake_ms() += kTouchWakeMs + 1000;
  h.refresh(kMidnight + 40);
  CHECK(h.backlight.last() == 20);
  app_set_screen_off(h.app, true);
  CHECK(h.backlight.last() == 0);
  app_set_screen_off(h.app, false);
  CHECK(h.backlight.last() == 20);
  CHECK(app_screen_off_shield(h.app) == nullptr);
}

TEST_CASE("screen off: setting the same state twice changes nothing") {
  AppHost h;
  h.refresh();
  app_set_screen_off(h.app, true);
  const size_t n = h.backlight.values.size();
  lv_obj_t* shield = app_screen_off_shield(h.app);
  app_set_screen_off(h.app, true);
  CHECK(h.backlight.values.size() == n);
  CHECK(app_screen_off_shield(h.app) == shield);
  app_set_screen_off(h.app, false);
  app_set_screen_off(h.app, false);
  CHECK(h.backlight.values.size() == n + 1);
}

// ------------------------------------------------------------ hold hint

TEST_CASE("hold hint: 'Hold to power off' over everything, with a bar that fills in the remaining 2 s") {
  AppHost h;
  h.refresh();
  CHECK(app_power_hint(h.app) == nullptr);
  app_show_power_hint(h.app, true);
  lv_obj_t* hint = app_power_hint(h.app);
  REQUIRE(hint != nullptr);
  CHECK(lv_obj_get_parent(hint) == lv_layer_sys());
  lv_obj_update_layout(lv_layer_sys());
  lv_obj_t* text = find_label(hint, power_key::kHoldHintText);
  REQUIRE(text != nullptr);
  CHECK(visible(text));
  const lv_area_t a = coords(hint);
  CHECK(a.x1 >= 0);
  CHECK(a.x2 < 240);
  CHECK(a.y1 >= 0);
  CHECK(a.y2 < 320);
  const lv_area_t t = coords(text);
  CHECK(t.x1 >= a.x1);
  CHECK(t.x2 <= a.x2);
  lv_obj_t* bar = app_power_hint_bar(h.app);
  REQUIRE(bar != nullptr);
  CHECK(lv_bar_get_value(bar) == 0);
  test_advance(1000);
  CHECK(lv_bar_get_value(bar) > 40);
  CHECK(lv_bar_get_value(bar) < 60);
  test_advance(1100);
  CHECK(lv_bar_get_value(bar) == 100);
  app_show_power_hint(h.app, false);
  CHECK(app_power_hint(h.app) == nullptr);
  test_advance(20);
}

TEST_CASE("hold hint: showing it twice keeps one; hiding without one is harmless") {
  AppHost h;
  app_show_power_hint(h.app, false);
  app_show_power_hint(h.app, true);
  lv_obj_t* hint = app_power_hint(h.app);
  app_show_power_hint(h.app, true);
  CHECK(app_power_hint(h.app) == hint);
  CHECK(lv_obj_get_child_count(lv_layer_sys()) == 1);
}

// ------------------------------------------------------------ Powering off

TEST_CASE("powering off: a full screen 'Powering off' above everything, lit even if the screen was off") {
  AppHost h;
  h.refresh();
  app_set_screen_off(h.app, true);
  app_show_power_hint(h.app, true);
  app_show_powering_off(h.app);
  CHECK_FALSE(app_screen_off(h.app));
  CHECK(h.backlight.last() == 100);
  CHECK(app_power_hint(h.app) == nullptr);
  lv_obj_t* v = app_powering_off_view(h.app);
  REQUIRE(v != nullptr);
  CHECK(lv_obj_get_parent(v) == lv_layer_sys());
  lv_obj_update_layout(lv_layer_sys());
  const lv_area_t a = coords(v);
  CHECK(a.x1 == 0);
  CHECK(a.y1 == 0);
  CHECK(a.x2 == 239);
  CHECK(a.y2 == 319);
  CHECK(lv_obj_get_style_bg_opa(v, 0) == LV_OPA_COVER);
  lv_obj_t* text = find_label(v, power_key::kPoweringOffText);
  REQUIRE(text != nullptr);
  const lv_area_t t = coords(text);
  CHECK(t.x1 > 0);
  CHECK(t.x2 < 239);
  // It swallows touches.
  CHECK(lv_obj_has_flag(v, LV_OBJ_FLAG_CLICKABLE));
  // A refresh never lights the screen off again or removes it.
  h.refresh(kNoon + 1);
  CHECK(app_powering_off_view(h.app) == v);
}

TEST_CASE("powering off: shown once however often it is asked for") {
  AppHost h;
  app_show_powering_off(h.app);
  lv_obj_t* v = app_powering_off_view(h.app);
  app_show_powering_off(h.app);
  CHECK(app_powering_off_view(h.app) == v);
}

TEST_CASE("powering off: going dark for the off state turns the backlight to 0") {
  AppHost h;
  h.refresh();
  app_show_powering_off(h.app);
  app_set_screen_off(h.app, true);
  CHECK(h.backlight.last() == 0);
}

// ------------------------------------------------------------ saving

TEST_CASE("app_save_settings writes the settings in use") {
  AppHost h;
  MemoryPrefs& p = h.prefs;
  std::string v;
  CHECK_FALSE(p.get("batt", v));
  app_save_settings(h.app);
  REQUIRE(p.get("batt", v));
  CHECK(v == "0");
  REQUIRE(p.get("fav.id", v));
  CHECK(v == "359");
}

TEST_CASE("the power UI is safe after the app is gone") {
  init_test_display();
  app_set_screen_off(nullptr, true);
  app_show_power_hint(nullptr, true);
  app_show_powering_off(nullptr);
  app_save_settings(nullptr);
  CHECK_FALSE(app_screen_off(nullptr));
  CHECK(app_power_hint(nullptr) == nullptr);
  deinit_test_display();
}

TEST_CASE("the power widgets are freed once whether the app or the display goes first") {
  {
    AppHost h;  // lv_deinit() at the end deletes lv_layer_sys() before the app
    app_set_screen_off(h.app, true);
    app_show_power_hint(h.app, true);
    app_show_powering_off(h.app);
    app_set_screen_off(h.app, true);
  }
  {
    AppHost h;
    app_set_screen_off(h.app, true);
    app_show_power_hint(h.app, true);
    lv_obj_delete(h.app);  // the app first
    CHECK(lv_obj_get_child_count(lv_layer_sys()) == 0);
    h.app = nullptr;
  }
}
