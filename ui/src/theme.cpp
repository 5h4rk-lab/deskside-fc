#include "theme.h"

#include <math.h>

namespace {

Palette default_palette() {
  Palette p;
  p.turf = lv_color_hex(0x0A1410);
  p.panel = lv_color_hex(0x0F1D17);
  p.line = lv_color_hex(0x1B3B2C);
  p.chalk = lv_color_hex(0xF1EFE4);
  p.dim = lv_color_hex(0x7C8F84);
  p.amber = lv_color_hex(0xFFC857);
  p.accent = lv_color_hex(0xEF0107);    // Arsenal red until a team is picked
  p.opponent = lv_color_hex(0x034694);  // Chelsea blue
  p.tabBar = lv_color_hex(0x050907);
  return p;
}

constexpr uint32_t kInvalidHex = 0x7C8F84;

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

float linear_channel(uint8_t c) {
  const float v = static_cast<float>(c) / 255.0f;
  return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

constexpr uint8_t kFillDarken = 110;  // white becomes a mid grey under a chalk knob
constexpr uint8_t kMarkLighten = 150;

uint8_t blend(uint8_t a, uint8_t b, uint8_t t) {
  return static_cast<uint8_t>((a * (255 - t) + b * t + 127) / 255);
}

}  // namespace

Palette& theme() {
  static Palette palette = default_palette();
  return palette;
}

void theme_set_team(uint32_t accentHex, uint32_t opponentHex) {
  theme().accent = lv_color_hex(accentHex);
  theme().opponent = lv_color_hex(opponentHex);
}

void theme_reset() { theme() = default_palette(); }

lv_color_t mix(lv_color_t a, lv_color_t b, uint8_t t) {
  lv_color_t out;
  out.red = blend(a.red, b.red, t);
  out.green = blend(a.green, b.green, t);
  out.blue = blend(a.blue, b.blue, t);
  return out;
}

lv_color_t darken(lv_color_t c, uint8_t amount) {
  lv_color_t black;
  black.red = 0;
  black.green = 0;
  black.blue = 0;
  return mix(c, black, amount);
}

uint32_t parse_hex(const char* s) {
  if (s == nullptr) return kInvalidHex;
  if (*s == '#') ++s;
  uint32_t value = 0;
  int digits = 0;
  for (; *s != '\0'; ++s, ++digits) {
    const int d = hex_digit(*s);
    if (d < 0 || digits >= 6) return kInvalidHex;
    value = (value << 4) | static_cast<uint32_t>(d);
  }
  return digits == 6 ? value : kInvalidHex;
}

float relative_luminance(lv_color_t c) {
  return 0.2126f * linear_channel(c.red) + 0.7152f * linear_channel(c.green) +
         0.0722f * linear_channel(c.blue);
}

lv_color_t accent_fill(lv_color_t accent) {
  return relative_luminance(accent) > kLightLuminance ? darken(accent, kFillDarken) : accent_mark(accent);
}

lv_color_t accent_mark(lv_color_t accent) {
  return relative_luminance(accent) < kDarkLuminance ? mix(accent, theme().chalk, kMarkLighten) : accent;
}
