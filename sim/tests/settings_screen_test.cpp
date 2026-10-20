#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "fonts.h"
#include "format.h"
#include "screens/settings.h"
#include "screens/zone_list.h"
#include "test_display.h"
#include "theme.h"
#include "timezone.h"
#include "version.h"

namespace {

// 2026-07-15 12:00 UTC (summer: New York UTC-4, London UTC+1).
constexpr int64_t kSummer = 1784116800;
// 2026-01-15 12:00 UTC (winter).
constexpr int64_t kWinter = 1768478400;

class NoImages : public ImageProvider {
 public:
  void request(const std::string& path, std::function<void(ImagePtr)> done) override {
    requested.push_back(path);
    done(nullptr);
  }
  std::vector<std::string> requested;
};

std::string text(lv_obj_t* label) { return lv_label_get_text(label); }
bool visible(lv_obj_t* obj) { return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }

Settings sample_settings() {
  Settings s;
  s.zoneId = "Europe/London";
  s.dayBrightness = 80;
  s.nightDim = true;
  s.nightStartHour = 23;
  s.nightEndHour = 7;
  s.nightPercent = 20;
  s.reducedMotion = false;
  return s;
}

// A settings screen on a 240x320 test display, recording the callbacks.
struct Screen {
  NoImages images;
  std::vector<Settings> changes;
  std::vector<int> previews;
  int backs = 0;
  int teams = 0;
  int zones = 0;
  int wifis = 0;
  lv_obj_t* obj = nullptr;

  explicit Screen(const Settings& s = sample_settings(), bool wifi = false, int64_t now = kSummer) {
    init_test_display();
    theme_reset();
    SettingsCallbacks cb;
    cb.onBack = [this] { ++backs; };
    cb.onTeam = [this] { ++teams; };
    cb.onZone = [this] { ++zones; };
    if (wifi) cb.onWifi = [this] { ++wifis; };
    cb.onBrightnessPreview = [this](int p) { previews.push_back(p); };
    cb.onChanged = [this](const Settings& n) { changes.push_back(n); };
    obj = settings_create(lv_screen_active(), s, images, cb, now);
    settle();
  }
  ~Screen() { deinit_test_display(); }

  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  const SettingsParts* parts() const { return settings_parts(obj); }
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

}  // namespace

TEST_CASE("settings: rows and texts for the given settings and time") {
  Screen s;
  const SettingsParts* p = s.parts();
  REQUIRE(p != nullptr);
  CHECK(text(p->title) == "Settings");
  CHECK(text(p->teamName) == "Arsenal");
  CHECK(text(p->teamLeague) == "Premier League");
  CHECK(text(p->zoneLabel) == "London, Dublin, Lisbon");
  // 12:00 UTC in July is 13:00 in London.
  CHECK(text(p->zoneOffset) == "UTC+1 \xC2\xB7 Local time 13:00");
  CHECK(text(p->brightValue) == "80%");
  CHECK(lv_slider_get_value(p->brightSlider) == 80);
  CHECK(lv_slider_get_min_value(p->brightSlider) == 10);
  CHECK(lv_slider_get_max_value(p->brightSlider) == 100);
  CHECK(lv_obj_has_state(p->nightSwitch, LV_STATE_CHECKED));
  CHECK(visible(p->nightControls));
  CHECK(text(p->nightFrom) == "23:00");
  CHECK(text(p->nightTo) == "07:00");
  CHECK(text(p->nightPercent) == "20%");
  CHECK(text(p->nightHelp) == kNightHelp);
  CHECK_FALSE(lv_obj_has_state(p->motionSwitch, LV_STATE_CHECKED));
  CHECK(text(p->motionHelp) == kMotionHelp);
  CHECK(text(p->about) == std::string(kAppName) + " " + kAppVersion);
  CHECK(text(p->about) == "Soccer display 0.1");
  // The crest of the favourite is requested.
  REQUIRE_FALSE(s.images.requested.empty());
  CHECK(s.images.requested[0].find("359.png") != std::string::npos);
}

TEST_CASE("settings: a long team name never meets the league name") {
  const char* names[] = {"Brighton & Hove Albion", "Wolverhampton Wanderers",
                         "A Very Long Football Club Name Indeed FC"};  // 40 characters
  for (const char* name : names) {
    Settings st = sample_settings();
    st.favourite.name = name;
    st.favourite.league = "eng.1";
    Screen s(st);
    const SettingsParts* p = s.parts();
    // The whole name, or its start with "..." when it is too long.
    const std::string shown = text(p->teamName);
    CHECK((shown == name || (shown.size() > 3 && shown.compare(shown.size() - 3, 3, "...") == 0 &&
                             std::string(name).compare(0, shown.size() - 3, shown, 0,
                                                       shown.size() - 3) == 0)));
    lv_area_t n;
    lv_area_t l;
    lv_area_t row;
    lv_obj_get_coords(p->teamName, &n);
    lv_obj_get_coords(p->teamLeague, &l);
    lv_obj_get_coords(p->teamRow, &row);
    // The league sits under the name, both inside the card.
    CHECK(l.y1 > n.y2);
    CHECK(n.x2 < row.x2 - 10);
    CHECK(l.x2 < row.x2 - 10);
    CHECK(lv_label_get_long_mode(p->teamName) == LV_LABEL_LONG_DOT);
    CHECK(lv_obj_get_height(p->teamName) == hanken_14.line_height);  // one line
  }
}

TEST_CASE("settings: the team card keeps one height whatever the name") {
  int32_t heights[2] = {0, 0};
  const char* names[] = {"Arsenal", "A Very Long Football Club Name Indeed FC"};
  for (int i = 0; i < 2; ++i) {
    Settings st = sample_settings();
    st.favourite.name = names[i];
    Screen s(st);
    heights[i] = lv_obj_get_height(s.parts()->teamRow);
  }
  CHECK(heights[0] == heights[1]);
}

TEST_CASE("settings: the zone row follows daylight saving and the time") {
  Settings st = sample_settings();
  st.zoneId = "America/New_York";
  Screen s(st, false, kWinter);
  CHECK(text(s.parts()->zoneOffset) == "UTC-5 \xC2\xB7 Local time 07:00");
  settings_refresh(s.obj, st, kSummer + 65 * 60);
  CHECK(text(s.parts()->zoneOffset) == "UTC-4 \xC2\xB7 Local time 09:05");
  st.zoneId = "Asia/Kolkata";
  settings_refresh(s.obj, st, kSummer);
  CHECK(text(s.parts()->zoneLabel) == "India");
  CHECK(text(s.parts()->zoneOffset) == "UTC+5:30 \xC2\xB7 Local time 17:30");
}

TEST_CASE("settings: the Wi-Fi row exists only with onWifi, and calls it") {
  {
    Screen s;
    CHECK(s.parts()->wifiRow == nullptr);
  }
  {
    Screen s(sample_settings(), true);
    REQUIRE(s.parts()->wifiRow != nullptr);
    s.click(s.parts()->wifiRow);
    CHECK(s.wifis == 1);
  }
}

TEST_CASE("settings: Back, team and zone rows call their callbacks") {
  Screen s;
  s.click(s.parts()->back);
  s.click(s.parts()->teamRow);
  s.click(s.parts()->zoneRow);
  CHECK(s.backs == 1);
  CHECK(s.teams == 1);
  CHECK(s.zones == 1);
  CHECK(s.changes.empty());
}

TEST_CASE("settings: dragging the slider previews, releasing saves") {
  Screen s;
  lv_obj_t* slider = s.parts()->brightSlider;
  lv_slider_set_value(slider, 55, LV_ANIM_OFF);
  lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
  CHECK(s.previews == std::vector<int>{55});
  CHECK(text(s.parts()->brightValue) == "55%");
  CHECK(s.changes.empty());
  lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
  REQUIRE(s.changes.size() == 1);
  CHECK(s.changes[0].dayBrightness == 55);
  CHECK(settings_current(s.obj)->dayBrightness == 55);
}

TEST_CASE("settings: night dimming switch hides the steppers and reports the change") {
  Screen s;
  const SettingsParts* p = s.parts();
  s.toggle(p->nightSwitch);
  s.settle();
  REQUIRE(s.changes.size() == 1);
  CHECK_FALSE(s.changes[0].nightDim);
  CHECK_FALSE(visible(p->nightControls));
  s.toggle(p->nightSwitch);
  s.settle();
  CHECK(s.changes.back().nightDim);
  CHECK(visible(p->nightControls));
}

TEST_CASE("settings: hour steppers wrap around midnight") {
  Screen s;
  const SettingsParts* p = s.parts();
  s.click(p->nightFromPlus);  // 23 -> 0
  CHECK(text(p->nightFrom) == "00:00");
  CHECK(s.changes.back().nightStartHour == 0);
  s.click(p->nightFromMinus);  // 0 -> 23
  CHECK(text(p->nightFrom) == "23:00");
  CHECK(s.changes.back().nightStartHour == 23);
  for (int i = 0; i < 7; ++i) s.click(p->nightToMinus);  // 7 -> 0
  CHECK(text(p->nightTo) == "00:00");
  s.click(p->nightToMinus);  // 0 -> 23
  CHECK(text(p->nightTo) == "23:00");
  CHECK(s.changes.back().nightEndHour == 23);
  s.click(p->nightToPlus);
  CHECK(s.changes.back().nightEndHour == 0);
}

TEST_CASE("settings: the night level steps by 5 and clamps to 5..100") {
  Screen s;
  const SettingsParts* p = s.parts();
  s.click(p->nightPercentPlus);
  CHECK(text(p->nightPercent) == "25%");
  CHECK(s.changes.back().nightPercent == 25);
  for (int i = 0; i < 10; ++i) s.click(p->nightPercentMinus);
  CHECK(text(p->nightPercent) == "5%");
  CHECK(s.changes.back().nightPercent == 5);
  const size_t n = s.changes.size();
  s.click(p->nightPercentMinus);  // at the minimum: nothing changes
  CHECK(s.changes.size() == n);
  for (int i = 0; i < 30; ++i) s.click(p->nightPercentPlus);
  CHECK(text(p->nightPercent) == "100%");
  CHECK(s.changes.back().nightPercent == 100);
}

TEST_CASE("settings: reduced motion switch reports the change") {
  Screen s;
  s.toggle(s.parts()->motionSwitch);
  REQUIRE(s.changes.size() == 1);
  CHECK(s.changes[0].reducedMotion);
}

TEST_CASE("settings: refreshing with unchanged settings touches nothing") {
  Screen s;
  const SettingsParts* p = s.parts();
  const uint32_t before = lv_obj_get_child_count(p->list);
  lv_obj_t* slider = p->brightSlider;
  settings_refresh(s.obj, *settings_current(s.obj), kSummer);
  settings_refresh(s.obj, *settings_current(s.obj), kSummer + 1);
  CHECK(lv_obj_get_child_count(p->list) == before);
  CHECK(s.parts()->brightSlider == slider);
}

// ------------------------------------------------------------------ zone list

namespace {

struct Zones {
  std::vector<std::string> picked;
  int backs = 0;
  lv_obj_t* obj = nullptr;

  Zones(const std::string& current, int64_t now) {
    init_test_display();
    theme_reset();
    obj = zone_list_create(lv_screen_active(), current, now,
                           [this](std::string id) { picked.push_back(id); }, [this] { ++backs; });
    test_advance(20);
  }
  ~Zones() { deinit_test_display(); }
  const ZoneListParts* parts() const { return zone_list_parts(obj); }
};

}  // namespace

TEST_CASE("zone list: every zone in order with its current offset, the current one ticked") {
  Zones z("America/New_York", kSummer);
  const ZoneListParts* p = z.parts();
  REQUIRE(p != nullptr);
  CHECK(text(p->title) == "Time zone");
  REQUIRE(p->rows.size() == zones().size());
  CHECK(zones().size() == 26);
  for (size_t i = 0; i < zones().size(); ++i) {
    CHECK(p->rows[i].id == zones()[i].id);
    CHECK(text(p->rows[i].label) == zones()[i].label);
    CHECK(text(p->rows[i].offset) == utc_offset_text(offset_seconds(zones()[i], kSummer)));
    CHECK(visible(p->rows[i].tick) == (p->rows[i].id == "America/New_York"));
  }
  const ZoneRowParts* ny = nullptr;
  const ZoneRowParts* india = nullptr;
  for (const ZoneRowParts& r : p->rows) {
    if (r.id == "America/New_York") ny = &r;
    if (r.id == "Asia/Kolkata") india = &r;
  }
  REQUIRE(ny != nullptr);
  REQUIRE(india != nullptr);
  CHECK(text(ny->offset) == "UTC-4");
  CHECK(text(india->offset) == "UTC+5:30");
}

TEST_CASE("zone list: winter offsets") {
  Zones z("UTC", kWinter);
  for (const ZoneRowParts& r : z.parts()->rows) {
    if (r.id == "America/New_York") CHECK(text(r.offset) == "UTC-5");
    if (r.id == "UTC") CHECK(text(r.offset) == "UTC+0");
  }
}

TEST_CASE("zone list: tapping a row picks it; Back goes back") {
  Zones z("UTC", kSummer);
  lv_obj_send_event(z.parts()->rows[19].row, LV_EVENT_CLICKED, nullptr);
  REQUIRE(z.picked.size() == 1);
  CHECK(z.picked[0] == zones()[19].id);
  lv_obj_send_event(z.parts()->back, LV_EVENT_CLICKED, nullptr);
  CHECK(z.backs == 1);
}

TEST_CASE("utc_offset_text: whole hours, half hours and negatives") {
  CHECK(utc_offset_text(0) == "UTC+0");
  CHECK(utc_offset_text(3600) == "UTC+1");
  CHECK(utc_offset_text(-14400) == "UTC-4");
  CHECK(utc_offset_text(19800) == "UTC+5:30");
  CHECK(utc_offset_text(-9000) == "UTC-2:30");
  CHECK(utc_offset_text(45 * 60 + 5 * 3600) == "UTC+5:45");
}
