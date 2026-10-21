// Battery in the UI: the status row icon on Live, Matches and News, the
// worst-case top row layout, the Settings line, app_refresh wiring and the
// low-battery banner.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "fonts.h"
#include "battery_model.h"
#include "power_service.h"
#include "prefs.h"
#include "screens/battery_banner.h"
#include "screens/goal.h"
#include "screens/live.h"
#include "screens/matches.h"
#include "screens/news.h"
#include "screens/settings.h"
#include "screens/widgets.h"
#include "snapshot.h"
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
  PowerStatus status() override {
    ++calls;
    return value;
  }
  PowerStatus value{true, 78, false, 3920};
  int calls = 0;
};

PowerStatus on_battery(int percent, int mv = 3900) { return PowerStatus{true, percent, false, mv}; }
PowerStatus charging(int percent, int mv = 4000) { return PowerStatus{true, percent, true, mv}; }

uint32_t u32(lv_color_t c) { return lv_color_to_u32(c); }
std::string text(lv_obj_t* label) { return lv_label_get_text(label); }
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
bool overlap(const lv_area_t& a, const lv_area_t& b) {
  return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}
uint32_t descendants(lv_obj_t* obj) {
  uint32_t n = 0;
  const uint32_t c = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < c; ++i) n += 1 + descendants(lv_obj_get_child(obj, static_cast<int32_t>(i)));
  return n;
}
int filled_segments(const widgets::StatusRow& r) {
  int n = 0;
  for (lv_obj_t* s : r.batterySegments) n += visible(s) ? 1 : 0;
  return n;
}

Team team(const char* id, const char* abbr, const char* name, uint32_t color) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  t.color = color;
  t.primary = color;
  t.league = "eng.1";
  return t;
}

MatchSummary live_match() {
  MatchSummary m;
  m.id = "m1";
  m.league = "uefa.champions";  // the longest league name on the left
  m.state = MatchState::Live;
  m.clock = "30'";
  m.kickoff = kNow - 1800;
  m.home = team("359", "ARS", "Arsenal", 0xEF0107);
  m.away = team("363", "CHE", "Chelsea", 0x034694);
  m.homeScore = 1;
  m.awayScore = 0;
  return m;
}

// A screen made directly (no app), refreshed from a Store.
struct ScreenHost {
  Store store;
  NoImages images;
  lv_obj_t* screen = nullptr;
  enum Kind { Live, Matches, News } kind;

  explicit ScreenHost(Kind k) : kind(k) {
    init_test_display();
    theme_reset();
    store.set_favourite("359");
    if (k == Live) {
      screen = live_create(lv_screen_active(), store, images, nullptr);
      live_set_on_settings(screen, [] {});
    } else if (k == Matches) {
      screen = matches_create(lv_screen_active(), store, images, nullptr);
    } else {
      screen = news_create(lv_screen_active(), store, images);
    }
    lv_obj_set_size(screen, 240, 276);
  }
  ~ScreenHost() {
    theme_reset();
    deinit_test_display();
  }
  void refresh(int64_t now = kNow) {
    if (kind == Live) live_refresh(screen, now);
    if (kind == Matches) matches_refresh(screen, now);
    if (kind == News) news_refresh(screen, now);
    lv_obj_update_layout(lv_screen_active());
  }
  const widgets::StatusRow& row() const {
    if (kind == Live) return live_parts(screen)->status;
    if (kind == Matches) return matches_parts(screen)->status;
    return news_parts(screen)->status;
  }
  void power(PowerStatus p) {
    store.set_power(p);
    refresh();
  }
};

struct AppHost {
  Store store;
  NoImages images;
  NoSource source;
  MemoryPrefs prefs;
  FakePower power;
  bool withPower = true;
  lv_obj_t* app = nullptr;

  explicit AppHost(bool create = true) {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(true);
    if (create) make();
  }
  ~AppHost() {
    goal_dismiss();
    theme_reset();
    goal_set_reduced_motion(false);
    deinit_test_display();
  }
  void make() {
    AppDeps deps{store, images, source, nullptr, prefs};
    deps.power = withPower ? &power : nullptr;
    app = app_create(lv_screen_active(), deps);
    test_advance(20);
  }
  void refresh(int64_t now = kNow) {
    app_refresh(app, now);
    lv_obj_update_layout(lv_screen_active());
  }
  lv_obj_t* tab_screen(int i) const {
    lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), i);
    return lv_obj_get_child(tab, 0);
  }
  const SettingsParts* settings() const { return settings_parts(app_settings_view(app)); }
};

}  // namespace

// ------------------------------------------------------------ status row icon

TEST_CASE("battery icon: hidden without a battery on every screen") {
  for (auto kind : {ScreenHost::Live, ScreenHost::Matches, ScreenHost::News}) {
    CAPTURE(kind);
    ScreenHost h(kind);
    h.refresh();
    CHECK_FALSE(visible(h.row().battery));
    CHECK_FALSE(visible(h.row().batteryBolt));
    CHECK_FALSE(visible(h.row().batteryPercent));
    h.power(on_battery(78));
    CHECK(visible(h.row().battery));
    h.power(PowerStatus{});
    CHECK_FALSE(visible(h.row().battery));
  }
}

TEST_CASE("battery icon: 78 percent on battery is four chalk segments, no text, no bolt") {
  ScreenHost h(ScreenHost::Matches);
  h.power(on_battery(78));
  const widgets::StatusRow& r = h.row();
  CHECK(filled_segments(r) == 4);
  for (lv_obj_t* s : r.batterySegments) CHECK(u32(lv_obj_get_style_bg_color(s, 0)) == u32(theme().chalk));
  CHECK(u32(lv_obj_get_style_border_color(r.batteryBody, 0)) == u32(theme().chalk));
  CHECK_FALSE(visible(r.batteryPercent));
  CHECK_FALSE(visible(r.batteryBolt));
}

TEST_CASE("battery icon: geometry is 22 x 11 with a 2 x 5 terminal and 3 x 7 segments") {
  ScreenHost h(ScreenHost::News);
  h.power(on_battery(100));
  const widgets::StatusRow& r = h.row();
  const lv_area_t body = coords(r.batteryBody);
  const lv_area_t tip = coords(r.batteryTip);
  CHECK(lv_area_get_width(&body) == 22);
  CHECK(lv_area_get_height(&body) == 11);
  CHECK(lv_area_get_width(&tip) == 2);
  CHECK(lv_area_get_height(&tip) == 5);
  CHECK(tip.x1 == body.x2 + 1);
  CHECK(tip.y1 == body.y1 + 3);
  int32_t lastRight = body.x1;  // the border
  for (int i = 0; i < widgets::kBatterySegments; ++i) {
    const lv_area_t s = coords(r.batterySegments[i]);
    CAPTURE(i);
    CHECK(lv_area_get_width(&s) == 3);
    CHECK(lv_area_get_height(&s) == 7);
    CHECK(s.y1 == body.y1 + 2);
    CHECK(s.x1 == lastRight + (i == 0 ? 2 : 3));  // 1 px border + 1 px air, then 2 px gaps
    lastRight = s.x2;
  }
  CHECK(lastRight == body.x2 - 2);  // 1 px of air before the right border too
}

TEST_CASE("battery icon: one segment per quarter, at least one") {
  ScreenHost h(ScreenHost::Matches);
  const std::pair<int, int> cases[] = {{1, 1}, {10, 1}, {25, 1}, {26, 2}, {50, 2}, {51, 3}, {75, 3}, {76, 4}, {100, 4}};
  for (const auto& c : cases) {
    CAPTURE(c.first);
    h.power(on_battery(c.first));
    CHECK(filled_segments(h.row()) == c.second);
  }
}

TEST_CASE("battery icon: low is amber with the percent beside it") {
  ScreenHost h(ScreenHost::Live);
  h.power(on_battery(18));
  const widgets::StatusRow& r = h.row();
  CHECK(filled_segments(r) == 1);
  CHECK(u32(lv_obj_get_style_bg_color(r.batterySegments[0], 0)) == u32(theme().amber));
  CHECK(u32(lv_obj_get_style_border_color(r.batteryBody, 0)) == u32(theme().amber));
  REQUIRE(visible(r.batteryPercent));
  CHECK(text(r.batteryPercent) == "18%");
  // Amber, not dim: dim is lost on the team colours behind the Live row.
  CHECK(u32(lv_obj_get_style_text_color(r.batteryPercent, 0)) == u32(theme().amber));
  CHECK(lv_obj_get_style_text_font(r.batteryPercent, 0) == &hanken_10);
  CHECK(coords(r.batteryPercent).x2 < coords(r.battery).x1);
  h.power(on_battery(21));
  CHECK_FALSE(visible(r.batteryPercent));
  CHECK(u32(lv_obj_get_style_bg_color(r.batterySegments[0], 0)) == u32(theme().chalk));
}

TEST_CASE("battery icon: critical is the warn colour") {
  ScreenHost h(ScreenHost::Matches);
  h.power(on_battery(8));
  const widgets::StatusRow& r = h.row();
  CHECK(u32(theme().warn) == u32(lv_color_hex(0xFF6B4A)));
  CHECK(filled_segments(r) == 1);
  CHECK(u32(lv_obj_get_style_bg_color(r.batterySegments[0], 0)) == u32(theme().warn));
  CHECK(u32(lv_obj_get_style_border_color(r.batteryBody, 0)) == u32(theme().warn));
  CHECK(text(r.batteryPercent) == "8%");
  CHECK(u32(lv_obj_get_style_text_color(r.batteryPercent, 0)) == u32(theme().warn));
}

TEST_CASE("battery icon: charging is green segments and a bolt beside the icon") {
  ScreenHost h(ScreenHost::News);
  h.power(charging(55));
  const widgets::StatusRow& r = h.row();
  CHECK(u32(theme().charging) == u32(lv_color_hex(0x5ED39B)));
  CHECK(filled_segments(r) == 3);
  for (int i = 0; i < 3; ++i) CHECK(u32(lv_obj_get_style_bg_color(r.batterySegments[i], 0)) == u32(theme().charging));
  CHECK(u32(lv_obj_get_style_border_color(r.batteryBody, 0)) == u32(theme().chalk));
  REQUIRE(visible(r.batteryBolt));
  CHECK_FALSE(overlap(coords(r.batteryBolt), coords(r.battery)));
  CHECK_FALSE(visible(r.batteryPercent));
  h.power(charging(8));  // low but charging: green, with the percent
  CHECK(u32(lv_obj_get_style_bg_color(r.batterySegments[0], 0)) == u32(theme().charging));
  CHECK(visible(r.batteryPercent));
  CHECK(u32(lv_obj_get_style_text_color(r.batteryPercent, 0)) == u32(theme().amber));
  CHECK_FALSE(overlap(coords(r.batteryBolt), coords(r.batteryPercent)));
}

TEST_CASE("battery icon: pixels are crisp: segments filled, gaps and air empty, bolt drawn") {
  ScreenHost h(ScreenHost::Matches);
  h.power(charging(55));
  lv_obj_t* scr = lv_screen_active();
  const widgets::StatusRow& r = h.row();
  const lv_area_t body = coords(r.batteryBody);
  const lv_area_t seg0 = coords(r.batterySegments[0]);
  const lv_color_t green = theme().charging;
  auto px = [&](int32_t x, int32_t y) { return u32(pixel_at(scr, x, y)); };
  auto close_to = [](lv_color_t a, lv_color_t b) {
    return std::abs(a.red - b.red) < 12 && std::abs(a.green - b.green) < 12 && std::abs(a.blue - b.blue) < 12;
  };
  // Every pixel of a lit segment is the full green (RGB565 rounding aside).
  for (int32_t y = seg0.y1; y <= seg0.y2; ++y) {
    for (int32_t x = seg0.x1; x <= seg0.x2; ++x) CHECK(close_to(pixel_at(scr, x, y), green));
  }
  // The gap after it and the air inside the border are background.
  const lv_color_t bg = pixel_at(scr, body.x1 - 30, body.y1 + 5);
  CHECK(px(seg0.x2 + 1, seg0.y1 + 3) == u32(bg));
  CHECK(px(body.x1 + 1, body.y1 + 5) == u32(bg));
  // Segment 4 is off at 55 percent: its place is background.
  const lv_area_t seg3 = coords(r.batterySegments[3]);
  CHECK(px(seg3.x1 + 1, seg3.y1 + 3) == u32(bg));
  // The border's straight edges are solid.
  CHECK_FALSE(px(body.x1 + 8, body.y1) == u32(bg));
  CHECK_FALSE(px(body.x1, body.y1 + 5) == u32(bg));
  // The bolt has green pixels in it.
  const lv_area_t bolt = coords(r.batteryBolt);
  int green_px = 0;
  for (int32_t y = bolt.y1; y <= bolt.y2; ++y) {
    for (int32_t x = bolt.x1; x <= bolt.x2; ++x) green_px += close_to(pixel_at(scr, x, y), green) ? 1 : 0;
  }
  CHECK(green_px >= 12);
}

TEST_CASE("battery icon: the bolt stays inside its 7 x 11 box") {
  init_test_display();
  theme_reset();
  // Drawn unclipped into a larger plain object, with its box at (8, 6): any
  // pixel outside the box would show.
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_t* canvas = widgets::plain(scr);
  lv_obj_set_pos(canvas, 20, 20);
  lv_obj_set_size(canvas, widgets::kBoltW + 16, widgets::kBoltH + 12);
  lv_obj_add_event_cb(
      canvas,
      [](lv_event_t* e) {
        lv_area_t a;
        lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
        widgets::draw_bolt_at(lv_event_get_layer(e), a.x1 + 8, a.y1 + 6);
      },
      LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_update_layout(scr);
  const uint32_t black = u32(lv_color_black());
  int outside = 0;
  int inside = 0;
  int topRow = 0;
  int bottomRow = 0;
  for (int32_t y = 20; y < 20 + widgets::kBoltH + 12; ++y) {
    for (int32_t x = 20; x < 20 + widgets::kBoltW + 16; ++x) {
      const bool lit = u32(pixel_at(scr, x, y)) != black;
      const int32_t bx = x - 28;
      const int32_t by = y - 26;
      const bool inBox = bx >= 0 && bx < widgets::kBoltW && by >= 0 && by < widgets::kBoltH;
      if (lit && !inBox) ++outside;
      if (lit && inBox) ++inside;
      if (lit && by == 0) ++topRow;
      if (lit && by == widgets::kBoltH - 1) ++bottomRow;
    }
  }
  CHECK(outside == 0);
  CHECK(inside >= 12);
  CHECK(topRow > 0);     // the tips reach the box's edges: nothing cut off
  CHECK(bottomRow > 0);
  deinit_test_display();
}

TEST_CASE("battery icon: repeated refreshes reuse the same objects") {
  ScreenHost h(ScreenHost::Live);
  h.power(on_battery(78));
  const uint32_t before = descendants(lv_screen_active());
  lv_obj_t* icon = h.row().battery;
  const PowerStatus states[] = {on_battery(40), on_battery(18), on_battery(8), charging(55), charging(100),
                                PowerStatus{}, on_battery(78)};
  for (int round = 0; round < 30; ++round) {
    for (const PowerStatus& s : states) h.power(s);
  }
  CHECK(descendants(lv_screen_active()) == before);
  CHECK(h.row().battery == icon);
}

// ------------------------------------------------------------ worst-case top row

namespace {

// Every visible item of the top row, with a name for the failure message.
std::vector<std::pair<std::string, lv_area_t>> top_row_items(const widgets::StatusRow& r, lv_obj_t* gearIcon) {
  std::vector<std::pair<std::string, lv_area_t>> out;
  auto add = [&](const char* name, lv_obj_t* o) {
    if (o != nullptr && visible(o) && lv_obj_get_width(o) > 0) out.emplace_back(name, coords(o));
  };
  add("left", r.left);
  add("staleDot", r.staleDot);
  add("staleLabel", r.staleLabel);
  add("clock", r.clock);
  add("freshDot", r.freshDot);
  add("batteryPercent", r.batteryPercent);
  add("batteryBolt", r.batteryBolt);
  add("battery", r.battery);
  add("gear", gearIcon);
  return out;
}

void check_no_collisions(const widgets::StatusRow& r, lv_obj_t* gearIcon) {
  const auto items = top_row_items(r, gearIcon);
  for (size_t i = 0; i < items.size(); ++i) {
    CAPTURE(items[i].first);
    CHECK(items[i].second.x1 >= 0);
    CHECK(items[i].second.x2 < 240);
    for (size_t j = i + 1; j < items.size(); ++j) {
      CAPTURE(items[j].first);
      CHECK_FALSE(overlap(items[i].second, items[j].second));
    }
  }
}

}  // namespace

TEST_CASE("top row: worst case (stale, charging and low, gear) has no collisions on any screen") {
  const int64_t ages[] = {12 * 60, 59 * 60, 125 * 3600};  // "12 min", "59 min", "125 h"
  for (auto kind : {ScreenHost::Live, ScreenHost::Matches, ScreenHost::News}) {
    for (int64_t age : ages) {
      CAPTURE(kind);
      CAPTURE(age);
      ScreenHost h(kind);
      h.store.set_matches({live_match()}, kNow - age);
      h.store.set_power(charging(12));
      h.refresh();
      const widgets::StatusRow& r = h.row();
      REQUIRE(visible(r.staleLabel));
      REQUIRE(visible(r.batteryBolt));
      REQUIRE(visible(r.batteryPercent));
      lv_obj_t* gearIcon = nullptr;
      if (kind == ScreenHost::Live) {
        const LiveParts* p = live_parts(h.screen);
        REQUIRE(visible(p->gear));
        gearIcon = lv_obj_get_child(p->gear, 0);
        CHECK_FALSE(overlap(coords(p->gear), coords(r.battery)));
        CHECK(text(r.left) == "Champions League");
      }
      check_no_collisions(r, gearIcon);
    }
  }
}

TEST_CASE("top row: the clock and the battery sit at the same x on all three screens") {
  int32_t clockX[3];
  int32_t batteryX[3];
  int i = 0;
  for (auto kind : {ScreenHost::Live, ScreenHost::Matches, ScreenHost::News}) {
    ScreenHost h(kind);
    h.store.set_matches({live_match()}, kNow);
    h.store.set_power(on_battery(78));
    h.refresh();
    clockX[i] = coords(h.row().clock).x1;
    batteryX[i] = coords(h.row().battery).x1;
    ++i;
  }
  CHECK(clockX[0] == clockX[1]);
  CHECK(clockX[1] == clockX[2]);
  CHECK(batteryX[0] == batteryX[1]);
  CHECK(batteryX[1] == batteryX[2]);
  CHECK(batteryX[0] + widgets::kBatteryBodyW + widgets::kBatteryTipW == 240 - widgets::kStatusRightInset);
}

TEST_CASE("top row: a long left text is shortened, not run into the right group") {
  ScreenHost h(ScreenHost::Live);
  h.store.set_matches({live_match()}, kNow - 59 * 60);
  h.store.set_power(charging(12));
  h.refresh();
  const widgets::StatusRow& r = h.row();
  // The worst case leaves no room at all: the league goes, nothing overlaps.
  CHECK(lv_obj_get_width(r.left) == 0);
  CHECK(widgets::status_row_right_start(r) >= widgets::kStatusSide);
  // Stale with a normal battery: the league is cut short with "...".
  h.store.set_power(on_battery(78));
  h.refresh();
  CHECK(lv_obj_get_width(r.left) > 20);
  CHECK(lv_obj_get_width(r.left) < 85);
  CHECK(lv_label_get_long_mode(r.left) == LV_LABEL_LONG_DOT);
  CHECK(coords(r.left).x2 + widgets::kStatusGap <= widgets::status_row_right_start(r));
  // Fresh data and a normal battery: the full text fits again.
  h.store.set_matches({live_match()}, kNow);
  h.store.set_power(on_battery(78));
  h.refresh();
  lv_point_t size;
  lv_text_get_size(&size, "Champions League", &hanken_10, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  CHECK(lv_obj_get_width(r.left) == size.x);
}

TEST_CASE("top row: the Live \"Last result\" caption gives way to a crowded right group") {
  ScreenHost h(ScreenHost::Live);
  MatchSummary m = live_match();
  m.state = MatchState::Post;
  m.clock = "FT";
  m.kickoff = kNow - 86400;
  h.store.set_matches({m}, kNow);
  h.store.set_power(on_battery(78));
  h.refresh();
  const LiveParts* p = live_parts(h.screen);
  CHECK(visible(p->lastCaption));
  CHECK_FALSE(overlap(coords(p->lastCaption), coords(h.row().clock)));
  check_no_collisions(h.row(), lv_obj_get_child(p->gear, 0));
  h.store.set_power(PowerStatus{});  // no battery: centred, as in the mockup
  h.refresh();
  CHECK(visible(p->lastCaption));
  const lv_area_t centred = coords(p->lastCaption);
  CHECK(std::abs((centred.x1 + centred.x2) / 2 - 120) <= 1);
  h.store.set_power(charging(12));
  h.refresh(kNow + 59 * 60);  // stale for an hour and charging from low
  CHECK_FALSE(visible(p->lastCaption));
  check_no_collisions(h.row(), lv_obj_get_child(p->gear, 0));
}

// ------------------------------------------------------------ app wiring

TEST_CASE("app: reads the power service once per refresh and shows it on all three tabs") {
  AppHost h;
  h.store.set_matches({live_match()}, kNow);
  h.power.calls = 0;
  h.refresh();
  CHECK(h.power.calls == 1);
  CHECK(h.store.power() == h.power.value);
  const widgets::StatusRow rows[] = {live_parts(h.tab_screen(0))->status, matches_parts(h.tab_screen(1))->status,
                                     news_parts(h.tab_screen(2))->status};
  for (const widgets::StatusRow& r : rows) {
    CHECK(visible(r.battery));
    CHECK(filled_segments(r) == 4);
  }
  h.power.value = charging(40);
  h.refresh(kNow + 1);
  CHECK(h.power.calls == 2);
  for (const widgets::StatusRow& r : rows) {
    CHECK(visible(r.batteryBolt));
    CHECK(filled_segments(r) == 2);
  }
}

TEST_CASE("app: an unchanged battery touches nothing") {
  AppHost h;
  h.store.set_matches({live_match()}, kNow);
  h.refresh();
  const uint32_t before = descendants(lv_screen_active());
  lv_obj_t* icon = matches_parts(h.tab_screen(1))->status.battery;
  int invalidated = 0;
  lv_display_t* d = lv_display_get_default();
  lv_display_add_event_cb(
      d, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); }, LV_EVENT_INVALIDATE_AREA,
      &invalidated);
  for (int i = 0; i < 5; ++i) h.refresh();  // same second, same battery
  CHECK(invalidated == 0);
  CHECK(h.power.calls == 6);
  CHECK(descendants(lv_screen_active()) == before);
  CHECK(matches_parts(h.tab_screen(1))->status.battery == icon);
  h.power.value = on_battery(77);  // still four segments: only the stored value changes
  h.refresh();
  CHECK(descendants(lv_screen_active()) == before);
}

TEST_CASE("app: no power service, no battery anywhere") {
  AppHost h(false);
  h.withPower = false;
  h.make();
  h.store.set_matches({live_match()}, kNow);
  h.refresh();
  CHECK_FALSE(h.store.power().present);
  CHECK_FALSE(visible(live_parts(h.tab_screen(0))->status.battery));
  app_open_settings(h.app);
  h.refresh();
  REQUIRE(h.settings() != nullptr);
  CHECK_FALSE(visible(h.settings()->batteryLine));
}

// ------------------------------------------------------------ Settings line

TEST_CASE("settings: the battery line above the data line, in every state") {
  AppHost h;
  h.store.set_matches({live_match()}, kNow);
  app_open_settings(h.app);
  h.refresh();
  const SettingsParts* p = h.settings();
  REQUIRE(p != nullptr);
  REQUIRE(p->batteryLine != nullptr);
  CHECK(visible(p->batteryLine));
  CHECK(lv_obj_get_index(p->batteryLine) == lv_obj_get_index(p->dataStatus) - 1);
  CHECK(lv_obj_get_index(p->about) < lv_obj_get_index(p->batteryLine));

  struct Case {
    PowerStatus power;
    const char* text;
    const char* volts;
  };
  const Case cases[] = {
      {on_battery(78, 3920), "78% \xC2\xB7 On battery", "3.92 V"},
      {charging(55, 4010), "55% \xC2\xB7 Charging", "4.01 V"},
      {charging(100, 4200), "100% \xC2\xB7 Fully charged", "4.20 V"},
      {on_battery(18, 3770), "Low: 18% \xC2\xB7 On battery", "3.77 V"},
      {on_battery(100, 4200), "100% \xC2\xB7 Full", "4.20 V"},
      {PowerStatus{}, "No battery detected", ""},
  };
  int64_t t = kNow;
  for (const Case& c : cases) {
    CAPTURE(c.text);
    h.power.value = c.power;
    h.refresh(++t);
    CHECK(text(p->batteryStatus) == c.text);
    CHECK(text(p->batteryVoltage) == c.volts);
    CHECK(visible(p->batteryVoltage) == (c.volts[0] != '\0'));
  }
  // Colours: amber when low on battery, dim otherwise; the voltage stays small and dim.
  h.power.value = on_battery(18, 3770);
  h.refresh(++t);
  CHECK(u32(lv_obj_get_style_text_color(p->batteryStatus, 0)) == u32(theme().amber));
  CHECK(u32(lv_obj_get_style_text_color(p->batteryVoltage, 0)) == u32(theme().dim));
  CHECK(lv_obj_get_style_text_font(p->batteryVoltage, 0) == &hanken_10);
  h.power.value = on_battery(78, 3920);
  h.refresh(++t);
  CHECK(u32(lv_obj_get_style_text_color(p->batteryStatus, 0)) == u32(theme().dim));
  // Status and voltage on one line, centred, clear of each other.
  lv_obj_update_layout(lv_screen_active());
  CHECK(coords(p->batteryStatus).x2 < coords(p->batteryVoltage).x1);
  CHECK(coords(p->batteryStatus).y1 == coords(p->batteryVoltage).y1);
}

// ------------------------------------------------------------ banner

TEST_CASE("banner: shown at boot when already low, for 8 s, amber on dark, 20 px at the top") {
  AppHost h(false);
  h.power.value = on_battery(12);
  h.make();
  h.refresh();
  lv_obj_t* b = app_battery_banner(h.app);
  REQUIRE(b != nullptr);
  const BatteryBannerParts* p = battery_banner_parts(b);
  REQUIRE(p != nullptr);
  CHECK(text(p->text) == "Battery low. Plug in the charger.");
  CHECK(u32(lv_obj_get_style_text_color(p->text, 0)) == u32(theme().amber));
  const lv_area_t a = coords(b);
  CHECK(a.y1 == 0);
  CHECK(lv_area_get_height(&a) == kBatteryBannerHeight);
  CHECK(lv_area_get_width(&a) == 240);
  CHECK(kBatteryBannerHeight == 20);
  test_advance(7900);
  CHECK(app_battery_banner(h.app) != nullptr);
  test_advance(200);
  test_advance(20);
  CHECK(app_battery_banner(h.app) == nullptr);
  // Once per discharge cycle.
  for (int i = 0; i < 30; ++i) h.refresh(kNow + 10 + i);
  CHECK(app_battery_banner(h.app) == nullptr);
}

TEST_CASE("banner: a tap dismisses it") {
  AppHost h(false);
  h.power.value = on_battery(12);
  h.make();
  h.refresh();
  lv_obj_t* b = app_battery_banner(h.app);
  REQUIRE(b != nullptr);
  lv_obj_send_event(b, LV_EVENT_CLICKED, nullptr);
  CHECK(app_battery_banner(h.app) == nullptr);
  test_advance(20);
  test_advance(10000);  // its timer is gone with it
  CHECK(app_battery_banner(h.app) == nullptr);
}

TEST_CASE("banner: very low at 5 percent, in the warn colour, for 10 s") {
  AppHost h;
  h.power.value = on_battery(16);
  h.refresh();
  CHECK(app_battery_banner(h.app) == nullptr);
  h.power.value = on_battery(5);
  h.refresh(kNow + 60);
  lv_obj_t* b = app_battery_banner(h.app);
  REQUIRE(b != nullptr);
  const BatteryBannerParts* p = battery_banner_parts(b);
  CHECK(text(p->text) == "Battery very low. Plug in the charger now.");
  CHECK(u32(lv_obj_get_style_text_color(p->text, 0)) == u32(theme().warn));
  CHECK(text(p->icon) == LV_SYMBOL_WARNING);
  CHECK(u32(lv_obj_get_style_text_color(p->icon, 0)) == u32(theme().warn));
  CHECK_FALSE(overlap(coords(p->icon), coords(p->text)));
  test_advance(9900);
  CHECK(app_battery_banner(h.app) != nullptr);
  test_advance(200);
  test_advance(20);
  CHECK(app_battery_banner(h.app) == nullptr);
}

TEST_CASE("banner: waits for the goal celebration to end") {
  AppHost h;
  h.store.set_matches({live_match()}, kNow);
  h.refresh();
  GoalEvent ev;
  ev.matchId = "m1";
  ev.favourite = true;
  ev.homeAbbr = "ARS";
  ev.awayAbbr = "CHE";
  goal_celebrate(ev, h.images);
  REQUIRE(goal_active());
  h.power.value = on_battery(14);
  h.refresh(kNow + 1);
  CHECK(app_battery_banner(h.app) == nullptr);
  goal_dismiss();
  test_advance(20);
  h.refresh(kNow + 2);
  CHECK(app_battery_banner(h.app) != nullptr);
}

TEST_CASE("banner: stays above overlays opened after it") {
  AppHost h(false);
  h.power.value = on_battery(12);
  h.make();
  h.refresh();
  lv_obj_t* b = app_battery_banner(h.app);
  REQUIRE(b != nullptr);
  app_open_settings(h.app);
  h.refresh(kNow + 1);
  lv_obj_t* scr = lv_screen_active();
  CHECK(lv_obj_get_index(b) == static_cast<int32_t>(lv_obj_get_child_count(scr)) - 1);
}

TEST_CASE("banner: forced for screenshots, and safe when the app goes first") {
  AppHost h;
  app_show_battery_banner(h.app, Alert{Alert::Level::Critical, battery::kAlertCriticalText, 10000});
  REQUIRE(app_battery_banner(h.app) != nullptr);
  lv_obj_delete(h.app);
  h.app = nullptr;
  test_advance(20);
  test_advance(11000);  // the banner's timer runs out with no app around
  test_advance(20);
}
