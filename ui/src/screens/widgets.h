// Small widget helpers and the status row shared by the screens (Live,
// Matches and News). Shared with the ESP32 firmware: no clock reads, no SDL, no stdio.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <string>

#include "battery_model.h"
#include "image_provider.h"
#include "store.h"

namespace widgets {

// A bare object: no style, not scrollable, not clickable.
lv_obj_t* plain(lv_obj_t* parent);
// A label with `font` and `color`, empty.
lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color);
// A filled circle.
lv_obj_t* dot(lv_obj_t* parent, int32_t size, lv_color_t color);
// Sets the text only when it differs, so unchanged labels are not invalidated.
void set_text(lv_obj_t* label, const std::string& text);
// Shows or hides an object, touching the flag only when needed.
void show(lv_obj_t* obj, bool visible);

// Short team label for narrow cells: the abbreviation when it is one to four
// letters long, otherwise the first three letters of the name in capitals.
std::string team_abbr(const Team& team);

// Crest image of `size` x `size` px with a faint circle as placeholder until
// the picture arrives. The caller positions it.
lv_obj_t* crest_image(lv_obj_t* parent, int32_t size);
// The placeholder goes away once an image is shown: call after a picture may
// have arrived (cheap when nothing changed).
void crest_sync(lv_obj_t* img);
// Shows `path` in the crest, dropping the old picture first so a different
// team never shows a stale crest. An empty path leaves the placeholder.
void crest_load(lv_obj_t* img, ImageProvider& images, const std::string& path);

// Glyph top offset and cap height of the digits in `font`, for vertical
// centring independent of the font's line metrics.
void digit_metrics(const lv_font_t* font, int32_t& top, int32_t& height);

// Pages opened from Settings (Settings itself, the zone list, the team
// picker): "< Back" at the top left over a Big Shoulders title.
constexpr int32_t kPageSide = 10;      // left edge of the Back text and the title
constexpr int32_t kPageTitleY = 16;
constexpr int32_t kPageBodyY = 54;     // where the page's content starts
constexpr int32_t kCardRadius = 10;
constexpr int32_t kListSide = 8;       // side margins of a scrolling list of cards
constexpr int32_t kListGap = 5;

struct PageHeader {
  lv_obj_t* back = nullptr;   // tap target, 80x28, holding "< Back"
  lv_obj_t* title = nullptr;
};
PageHeader page_header_create(lv_obj_t* parent, const char* title);

// A panel card (rounded, panel colour). A clickable one lightens while pressed.
lv_obj_t* card(lv_obj_t* parent, bool clickable);

// A vertical scrolling column for cards: no scrollbar, momentum, side margins
// kListSide and gaps kListGap. Clickable so a drag between cards scrolls too.
lv_obj_t* scroll_column(lv_obj_t* parent);

// A selection tick (LV_SYMBOL_OK) in the accent, kept visible on the dark
// panels whatever the accent (see accent_mark()).
lv_obj_t* tick(lv_obj_t* parent);

// A one-line text field (lv_textarea) on a panel-coloured rounded box,
// `height` px tall, Hanken 14 text, dim placeholder and an accent cursor
// shown only while the field is focused. The caller sizes and places it.
void style_text_field(lv_obj_t* ta, int32_t height);
// The on-screen keyboard in the dark palette (team picker, Wi-Fi password).
void style_keyboard(lv_obj_t* kb);

// What the status row shows besides its left text.
struct StatusInfo {
  std::string clock;      // local HH:MM
  bool connecting = true; // no data yet: no stale marker, no freshness dot
  bool stale = false;
  std::string staleText;  // "Updated N min ago" while stale
  // Battery (from Store::power()); the icon is hidden while !battery.
  bool battery = false;
  int batteryPercent = 0;
  bool charging = false;

  bool operator==(const StatusInfo& o) const {
    return clock == o.clock && connecting == o.connecting && stale == o.stale &&
           staleText == o.staleText && battery == o.battery &&
           batteryPercent == o.batteryPercent && charging == o.charging;
  }
  bool operator!=(const StatusInfo& o) const { return !(*this == o); }
};

// Local clock, "connecting" until the Store has data, stale marker when the
// last success is older than the Store's limit, and the battery.
StatusInfo status_info(const Store& store, int64_t now);

// The status row's right-hand group ends this far from the screen's right
// edge on every screen, so the clock and the battery stay put when the tabs
// change (the Live gear sits in the space on its screen).
constexpr int32_t kStatusRightInset = 32;
constexpr int32_t kStatusSide = 10;   // left margin of the left text
constexpr int32_t kStatusTop = 4;
constexpr int32_t kStatusRowHeight = 13;
constexpr int32_t kStatusGap = 6;     // least room between the left text and the right group

// Battery icon geometry (px): a 22 x 11 outline with a 1 px border and a
// 2 x 5 terminal, four 3 x 7 segments inside with 2 px gaps.
constexpr int32_t kBatteryBodyW = 22;
constexpr int32_t kBatteryH = 11;
constexpr int32_t kBatteryTipW = 2;
constexpr int32_t kBatteryTipH = 5;
constexpr int32_t kBatterySegW = 3;
constexpr int32_t kBatterySegH = 7;
constexpr int32_t kBatterySegGap = 2;
constexpr int kBatterySegments = 4;
constexpr int32_t kBoltW = 7;
constexpr int32_t kBoltH = 11;
// The bolt's two triangles (upper stroke, lower stroke) as pixel-corner
// coordinates of the box: LVGL fills a triangle up to, not past, its
// vertices, so x = kBoltW / y = kBoltH is the box's right / bottom edge and
// every lit pixel stays inside 0..kBoltW-1 x 0..kBoltH-1 (tested unclipped).
struct BoltPoint {
  int32_t x;
  int32_t y;
};
constexpr BoltPoint kBoltPoints[6] = {{5, 0}, {0, 6}, {4, 6}, {3, 5}, {7, 5}, {2, 11}};
// Draws the bolt with its box's top left at (x, y), in the charging green.
void draw_bolt_at(lv_layer_t* layer, int32_t x, int32_t y);

// Top row of a screen: free text on the left; on the right, in one
// right-aligned row: the stale marker (amber dot and "Updated N min ago"),
// the local clock with a quiet dot while the data is fresh, then the battery
// (percent text when low, charge bolt while charging, the icon). The left
// text is shortened with "..." rather than run into the right group.
struct StatusRow {
  lv_obj_t* left = nullptr;
  lv_obj_t* right = nullptr;  // the right-hand group (fixed box, children right-aligned)
  lv_obj_t* clock = nullptr;
  lv_obj_t* staleDot = nullptr;
  lv_obj_t* staleLabel = nullptr;
  lv_obj_t* freshDot = nullptr;
  lv_obj_t* batteryPercent = nullptr;  // "18%", only when low
  lv_obj_t* batteryBolt = nullptr;     // drawn charge bolt, only while charging
  lv_obj_t* battery = nullptr;         // the icon: body + terminal, hidden without a battery
  lv_obj_t* batteryBody = nullptr;
  lv_obj_t* batteryTip = nullptr;
  lv_obj_t* batterySegments[kBatterySegments] = {nullptr, nullptr, nullptr, nullptr};
};

// Creates the row at the top of `parent` (left text at x = 10, right group
// ending kStatusRightInset from the right edge, y = 4). The stale and
// freshness markers and the battery start hidden.
StatusRow status_row_create(lv_obj_t* parent);
void status_row_apply(const StatusRow& row, const std::string& left, const StatusInfo& info);

// The screen x where the right group's first visible item starts (brings
// the layout up to date first). Screens use it to keep their own top-row
// texts clear of it.
int32_t status_row_right_start(const StatusRow& row);

// Colour of the battery icon for a level (chalk, amber, warn; charging: the
// segments in the charging green).
lv_color_t battery_color(BatteryLevel level);

}  // namespace widgets
