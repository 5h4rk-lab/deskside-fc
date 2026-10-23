// Matches screen (second tab): every match in the Store grouped under day
// headers, live matches first. Mockup: screen 3 of
// docs/superpowers/specs/assets/ui-mockup-v2.html.
//
// Order: LIVE NOW (all live matches by kickoff), then upcoming days (TODAY,
// TOMORROW, weekday names, short dates from seven days out) with each day's
// matches by kickoff ascending, then past days, most recent day first and
// within a day the latest kickoff first. A day is the local day of the
// kickoff (UTC plus Store::tz_offset()). Postponed matches stay on their
// original day with "PPD" in the centre cell.
//
// Shared with the ESP32 firmware: no clock reads, no SDL, no stdio. Every
// time-dependent text uses the `now` passed to matches_refresh() and the
// Store's timezone offset.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "image_provider.h"
#include "screens/widgets.h"
#include "store.h"

// Builds the screen as a child of `parent`, filling it. `onOpen` is called
// with the match id when a row is tapped (not after a scroll gesture). The
// screen shows "Connecting..." until the first matches_refresh().
lv_obj_t* matches_create(lv_obj_t* parent, Store& store, ImageProvider& images,
                         std::function<void(std::string matchId)> onOpen);

// Updates the screen from the Store. Rows are reused: only data that changed
// since the last call is touched, and the scroll position is kept. A list
// that needs many new rows (the first data: about 70 rows and headers, about
// 15 ms per row on the board: 1.07 s at once) gets its first kMatchesFirstItems at once and the
// rest kMatchesItemsPerStep every kMatchesStepMs (an LVGL timer), in order,
// so the screen never stops for a second.
void matches_refresh(lv_obj_t* screen, int64_t now);
constexpr size_t kMatchesFirstItems = 8;
constexpr size_t kMatchesItemsPerStep = 3;
constexpr uint32_t kMatchesStepMs = 30;
// True when every row and header of the last refresh has its widget.
bool matches_complete(lv_obj_t* screen);

// Text of the empty states (exact wording).
constexpr const char* kMatchesConnecting = "Connecting...";
constexpr const char* kMatchesEmptyTitle = "No matches in the next few days";
constexpr const char* kMatchesEmptyHint = "New fixtures appear here as soon as they are announced.";

// Widgets of a matches screen, for tests and screenshots.
struct MatchesParts {
  widgets::StatusRow status;  // "Matches" left, stale marker and clock right
  lv_obj_t* list;             // scrolling container of headers and rows
  lv_obj_t* connecting;       // "Connecting..." before the first data
  lv_obj_t* emptyCard;        // no matches at all
  lv_obj_t* emptyTitle;
  lv_obj_t* emptyHint;
};

struct MatchesRowParts {
  std::string matchId;
  lv_obj_t* row;        // clickable card
  lv_obj_t* edge;       // amber left edge, visible on live rows only
  lv_obj_t* favDot;     // accent dot over the favourite's crest (home or away), its matches only
  lv_obj_t* homeCrest;
  lv_obj_t* homeAbbr;
  lv_obj_t* centre;     // kickoff time, score or PPD
  lv_obj_t* sub;        // league, clock or FT
  lv_obj_t* awayAbbr;
  lv_obj_t* awayCrest;
};

// One entry of the list, in display order: a day header or a match row.
struct MatchesItem {
  bool header = false;
  lv_obj_t* headerLabel = nullptr;  // when header
  MatchesRowParts row{};            // when not header
};

// Parts of a screen made by matches_create(), or nullptr for any other object.
const MatchesParts* matches_parts(lv_obj_t* screen);
// The list's headers and rows in display order (empty when not a matches screen).
std::vector<MatchesItem> matches_items(lv_obj_t* screen);
