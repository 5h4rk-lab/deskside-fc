#include <doctest/doctest.h>

#include "app.h"
#include "fonts.h"
#include "test_display.h"

TEST_CASE("app_create builds a three-tab view on a 240x320 display") {
  lv_display_t* disp = init_test_display();
  REQUIRE(disp != nullptr);
  CHECK(lv_display_get_horizontal_resolution(disp) == 240);
  CHECK(lv_display_get_vertical_resolution(disp) == 320);

  lv_obj_t* screen = lv_screen_active();
  CHECK(lv_obj_get_width(screen) == 240);
  CHECK(lv_obj_get_height(screen) == 320);

  app_create(screen);
  test_advance(50);

  lv_obj_t* tabview = lv_obj_get_child(screen, 0);
  REQUIRE(tabview != nullptr);
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
  app_create(screen);
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
