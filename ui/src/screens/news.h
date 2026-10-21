// News screen (third tab): the newest article as a large lead story, two tiles
// with the favourite's last result and next fixture, then up to five more
// headlines. Mockup: screen 4 of docs/superpowers/specs/assets/ui-mockup-v2.html.
//
// Articles are shown newest first (by `published`, equal times keep their
// input order); an id that appears twice is shown once; at most six articles
// are shown, the rest are ignored. The screen scrolls vertically, and nothing
// on it is tappable (the device has no way to open a link).
//
// Shared with the ESP32 firmware: no clock reads, no SDL, no stdio. Every
// time-dependent text uses the `now` passed to news_refresh() and the Store's
// timezone offset.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "image_provider.h"
#include "screens/widgets.h"
#include "store.h"

// Builds the screen as a child of `parent`, filling it. It shows
// "Connecting..." until the first news_refresh() with data in the Store.
lv_obj_t* news_create(lv_obj_t* parent, Store& store, ImageProvider& images);

// Updates the screen from the Store. Only what changed since the last call is
// touched: rows are reused by article id, pictures are requested again only
// when an article's picture path changed, and the scroll position is kept.
void news_refresh(lv_obj_t* screen, int64_t now);

// Text of the empty states (exact wording). "Connecting..." shows until the
// Store has data (matches arrive with the same polling as the news); data with
// an empty news list shows the empty card.
constexpr const char* kNewsConnecting = "Connecting...";
constexpr const char* kNewsEmptyTitle = "No headlines right now";
constexpr const char* kNewsEmptyHint = "Check back after the next match.";

// One of the two tiles under the lead story.
struct NewsTileParts {
  lv_obj_t* card = nullptr;
  lv_obj_t* caption = nullptr;  // "Last: CHE", "Next: LIV", "Last result", "Next fixture"
  lv_obj_t* value = nullptr;    // "3:1", "Sat 17:30", "No result yet", "No fixture yet"
  lv_obj_t* suffix = nullptr;   // "FT" next to a result, hidden otherwise
};

// Widgets of a news screen, for tests and screenshots.
struct NewsParts {
  widgets::StatusRow status;  // "News" left, stale marker and clock right
  lv_obj_t* list = nullptr;   // scrolling container: lead, tiles, rows
  lv_obj_t* connecting = nullptr;
  lv_obj_t* emptyCard = nullptr;
  lv_obj_t* emptyTitle = nullptr;
  lv_obj_t* emptyHint = nullptr;

  lv_obj_t* lead = nullptr;          // 240x158 block; its background is the brand-colour gradient
  lv_obj_t* leadImage = nullptr;     // the picture, scaled and cropped to fill the block
  lv_obj_t* leadTime = nullptr;      // "2h ago"
  lv_obj_t* leadHeadline = nullptr;  // up to three lines
  std::string leadId;                // id of the article shown as lead

  NewsTileParts last;
  NewsTileParts next;
};

// One headline row below the tiles.
struct NewsRowParts {
  std::string id;
  lv_obj_t* row = nullptr;
  lv_obj_t* thumb = nullptr;     // 40x40 rounded box; its first child is the picture
  lv_obj_t* headline = nullptr;  // two lines, ends in dots when longer
  lv_obj_t* time = nullptr;
};

// Parts of a screen made by news_create(), or nullptr for any other object.
const NewsParts* news_parts(lv_obj_t* screen);
// The headline rows (without the lead) in display order; empty when `screen`
// is not a news screen.
std::vector<NewsRowParts> news_rows(lv_obj_t* screen);
