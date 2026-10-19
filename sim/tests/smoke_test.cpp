#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app.h"
#include "fonts.h"
#include "screens/live.h"
#include "test_display.h"

namespace {

class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

// Store, images and the deps that point at them, for app_create().
struct Host {
  Store store;
  NoImages images;
  std::vector<std::string> opened;
  AppDeps deps{store, images, [this](const std::string& id) { opened.push_back(id); }};
};

}  // namespace

TEST_CASE("app_create builds a three-tab view on a 240x320 display") {
  lv_display_t* disp = init_test_display();
  REQUIRE(disp != nullptr);
  CHECK(lv_display_get_horizontal_resolution(disp) == 240);
  CHECK(lv_display_get_vertical_resolution(disp) == 320);

  lv_obj_t* screen = lv_screen_active();
  CHECK(lv_obj_get_width(screen) == 240);
  CHECK(lv_obj_get_height(screen) == 320);

  Host host;
  lv_obj_t* app = app_create(screen, host.deps);
  test_advance(50);

  lv_obj_t* tabview = lv_obj_get_child(screen, 0);
  REQUIRE(tabview != nullptr);
  CHECK(app == tabview);
  lv_obj_t* content = lv_tabview_get_content(tabview);
  REQUIRE(content != nullptr);
  CHECK(lv_obj_get_child_count(content) == 3);
  CHECK(lv_tabview_get_tab_count(tabview) == 3);

  // Tab bar sits at the bottom and is 44 px tall.
  lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
  REQUIRE(bar != nullptr);
  CHECK(lv_obj_get_height(bar) == 44);
  CHECK(lv_obj_get_y(bar) == 320 - 44);

  deinit_test_display();
}

TEST_CASE("active tab has an accent icon and chalk text, others are dim") {
  init_test_display();
  lv_obj_t* screen = lv_screen_active();
  Host host;
  app_create(screen, host.deps);
  test_advance(50);

  lv_obj_t* tabview = lv_obj_get_child(screen, 0);
  lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
  auto icon_color = [&](int tab) {
    lv_obj_t* button = lv_obj_get_child(bar, tab);
    return lv_color_to_u32(lv_obj_get_style_text_color(lv_obj_get_child(button, 1), 0));
  };
  auto text_color = [&](int tab) {
    lv_obj_t* button = lv_obj_get_child(bar, tab);
    return lv_color_to_u32(lv_obj_get_style_text_color(lv_obj_get_child(button, 0), 0));
  };
  const uint32_t accent = lv_color_to_u32(lv_color_hex(0xEF0107));
  const uint32_t dim = lv_color_to_u32(lv_color_hex(0x7C8F84));
  const uint32_t chalk = lv_color_to_u32(lv_color_hex(0xF1EFE4));

  CHECK(icon_color(0) == accent);
  CHECK(text_color(0) == chalk);
  CHECK(icon_color(1) == dim);
  CHECK(text_color(1) == dim);

  lv_tabview_set_active(tabview, 1, LV_ANIM_OFF);
  test_advance(400);
  CHECK(lv_tabview_get_tab_active(tabview) == 1);
  CHECK(icon_color(1) == accent);
  CHECK(text_color(1) == chalk);
  CHECK(icon_color(0) == dim);

  deinit_test_display();
}

TEST_CASE("the Live tab hosts the live screen, refreshed through app_refresh") {
  init_test_display();
  Host host;
  host.store.set_favourite("1");
  lv_obj_t* app = app_create(lv_screen_active(), host.deps);
  lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), 0);
  lv_obj_t* live = lv_obj_get_child(tab, 0);
  const LiveParts* parts = live_parts(live);
  REQUIRE(parts != nullptr);

  MatchSummary m;
  m.id = "abc";
  m.state = MatchState::Live;
  m.clock = "12'";
  m.home.id = "1";
  m.away.id = "2";
  m.homeScore = 0;
  m.awayScore = 0;
  host.store.set_matches({m}, 1000);
  app_refresh(app, 1000);
  test_advance(50);
  CHECK(std::string(lv_label_get_text(parts->pillLabel)) == "12'");
  CHECK(lv_obj_get_height(live) == 320 - 44);

  lv_obj_send_event(parts->scoreArea, LV_EVENT_CLICKED, nullptr);
  REQUIRE(host.opened.size() == 1);
  CHECK(host.opened[0] == "abc");
  deinit_test_display();
}

TEST_CASE("generated fonts are linked") {
  const lv_font_t* fonts[] = {
      &bigshoulders_16, &bigshoulders_22, &bigshoulders_28,
      &bigshoulders_40, &bigshoulders_56, &bigshoulders_96,
      &hanken_10,       &hanken_12,       &hanken_14,
  };
  for (const lv_font_t* f : fonts) {
    CHECK(f != nullptr);
    CHECK(f->line_height > 0);
  }
  // Big sizes carry digits only; full sizes carry accented Latin.
  lv_font_glyph_dsc_t g;
  CHECK(lv_font_get_glyph_dsc(&bigshoulders_96, &g, '7', 0));
  CHECK(lv_font_get_glyph_dsc(&bigshoulders_28, &g, 0xD6 /* O-umlaut */, 0));
  CHECK(lv_font_get_glyph_dsc(&hanken_12, &g, 0x107 /* c-acute */, 0));
}
