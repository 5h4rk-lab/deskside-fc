// Palette from the approved mockup (docs/superpowers/specs/assets/ui-mockup-v2.html).
// Shared by the simulator and the ESP32 firmware: no SDL or filesystem use here.
#pragma once

#include <lvgl.h>
#include <stdint.h>

struct Palette {
  lv_color_t turf;      // screen background
  lv_color_t panel;     // cards and panels
  lv_color_t line;      // hairlines
  lv_color_t chalk;     // primary text
  lv_color_t dim;       // secondary text
  lv_color_t amber;     // live indicators (floodlight amber)
  lv_color_t accent;    // favourite's readable accent for small details: tab icon, ticks,
                        // dots, switches, links, highlights (Arsenal red by default)
  lv_color_t opponent;  // opposing team colour (Chelsea blue by default)
  lv_color_t brand;     // favourite's real brand colour, for large surfaces (Arsenal red by default)
  lv_color_t tabBar;    // bottom bar backdrop
  lv_color_t warn;      // critical warnings (battery very low): warm red-orange
  lv_color_t charging;  // battery charging: a green kept clear of team accents
};

// The active palette. Mutable so the favourite team can recolour the UI.
Palette& theme();

// Sets the accent, opponent and brand colours (0xRRGGBB); other fields are unchanged.
void theme_set_team(uint32_t accentHex, uint32_t opponentHex, uint32_t brandHex);
// Two-argument form: the brand is the accent.
void theme_set_team(uint32_t accentHex, uint32_t opponentHex);

// Restores the default palette (used by tests to undo theme_set_team).
void theme_reset();

// Linear blend: t = 0 gives `a`, t = 255 gives `b`.
lv_color_t mix(lv_color_t a, lv_color_t b, uint8_t t);

// Scales each channel towards black: amount = 0 is unchanged, 255 is black.
lv_color_t darken(lv_color_t c, uint8_t amount);

// WCAG relative luminance, 0 (black) .. 1 (white).
float relative_luminance(lv_color_t c);

// Luminance above which an accent counts as light (white or near-white kits).
constexpr float kLightLuminance = 0.6f;
// Luminance below which an accent is lost on the dark turf and panels.
constexpr float kDarkLuminance = 0.02f;

// The accent as a fill behind chalk content (switch track, slider bar): a
// light accent is darkened so a chalk knob or glyph stays visible.
lv_color_t accent_fill(lv_color_t accent);

// The accent as a mark on the dark panels (selection ticks): a nearly black
// accent is lightened towards chalk so it stays visible.
lv_color_t accent_mark(lv_color_t accent);

// Parses "#RRGGBB" or "RRGGBB" (any case) into 0xRRGGBB. Anything else,
// including a null pointer or a wrong length, returns 0x7C8F84 (the dim colour).
uint32_t parse_hex(const char* s);

// Like parse_hex, but reports whether `s` is valid: true and `out` set, or
// false and `out` untouched.
bool try_parse_hex(const char* s, uint32_t& out);

// Minimum relative luminance of a team colour used as a large background.
constexpr float kBackgroundMinLuminance = 0.04f;

// A team's primary as a large background (the Live split): unchanged when its
// relative luminance is at least kBackgroundMinLuminance, else brightened just
// enough to reach it, keeping the hue (navy stays navy; black becomes a dark grey).
lv_color_t background_tone(lv_color_t primary);

namespace layout {

constexpr int32_t kTabBarHeight = 44;

}  // namespace layout
