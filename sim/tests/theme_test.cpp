#include <doctest/doctest.h>

#include "theme.h"

namespace {

// Restores the default palette when it goes out of scope, so a test that
// changes the global theme cannot leak into the others.
struct ThemeGuard {
  ~ThemeGuard() { theme_reset(); }
};

uint32_t u32(lv_color_t c) { return lv_color_to_u32(c); }

}  // namespace

TEST_CASE("parse_hex accepts valid colours") {
  CHECK(parse_hex("#EF0107") == 0xEF0107);
  CHECK(parse_hex("EF0107") == 0xEF0107);
  CHECK(parse_hex("#ef0107") == 0xEF0107);
  CHECK(parse_hex("#034694") == 0x034694);
}

TEST_CASE("parse_hex falls back to dim on invalid input") {
  CHECK(parse_hex("zzz") == 0x7C8F84);
  CHECK(parse_hex("#GG0107") == 0x7C8F84);
  CHECK(parse_hex("#EF01") == 0x7C8F84);
  CHECK(parse_hex("#EF01070") == 0x7C8F84);
  CHECK(parse_hex("#") == 0x7C8F84);
  CHECK(parse_hex("") == 0x7C8F84);
  CHECK(parse_hex(nullptr) == 0x7C8F84);
}

TEST_CASE("mix interpolates from a to b") {
  const lv_color_t a = lv_color_hex(0x102030);
  const lv_color_t b = lv_color_hex(0xF0E0D0);
  CHECK(u32(mix(a, b, 0)) == u32(a));
  CHECK(u32(mix(a, b, 255)) == u32(b));
  const lv_color_t m = mix(lv_color_hex(0x000000), lv_color_hex(0xFFFFFF), 128);
  CHECK(m.red == 128);
  CHECK(m.green == 128);
  CHECK(m.blue == 128);
}

TEST_CASE("darken reduces channels and never underflows") {
  const lv_color_t c = lv_color_hex(0x806040);
  const lv_color_t d = darken(c, 100);
  CHECK(d.red < c.red);
  CHECK(d.green < c.green);
  CHECK(d.blue < c.blue);
  CHECK(u32(darken(c, 0)) == u32(c));
  CHECK(u32(darken(c, 255)) == u32(lv_color_hex(0x000000)));
  const lv_color_t dark = darken(lv_color_hex(0x010203), 255);
  CHECK(dark.red == 0);
  CHECK(dark.green == 0);
  CHECK(dark.blue == 0);
}

TEST_CASE("default palette matches the mockup") {
  ThemeGuard guard;
  theme_reset();
  const Palette& p = theme();
  CHECK(u32(p.turf) == u32(lv_color_hex(0x0A1410)));
  CHECK(u32(p.panel) == u32(lv_color_hex(0x0F1D17)));
  CHECK(u32(p.line) == u32(lv_color_hex(0x1B3B2C)));
  CHECK(u32(p.chalk) == u32(lv_color_hex(0xF1EFE4)));
  CHECK(u32(p.dim) == u32(lv_color_hex(0x7C8F84)));
  CHECK(u32(p.amber) == u32(lv_color_hex(0xFFC857)));
  CHECK(u32(p.accent) == u32(lv_color_hex(0xEF0107)));
  CHECK(u32(p.opponent) == u32(lv_color_hex(0x034694)));
  CHECK(u32(p.tabBar) == u32(lv_color_hex(0x050907)));
}

TEST_CASE("theme_set_team changes accent and opponent only") {
  ThemeGuard guard;
  const uint32_t turf = u32(theme().turf);
  theme_set_team(0x123456, 0x654321);
  CHECK(u32(theme().accent) == u32(lv_color_hex(0x123456)));
  CHECK(u32(theme().opponent) == u32(lv_color_hex(0x654321)));
  CHECK(u32(theme().turf) == turf);
}

TEST_CASE("theme_reset restores the defaults") {
  theme_set_team(0x123456, 0x654321);
  theme_reset();
  CHECK(u32(theme().accent) == u32(lv_color_hex(0xEF0107)));
  CHECK(u32(theme().opponent) == u32(lv_color_hex(0x034694)));
}
