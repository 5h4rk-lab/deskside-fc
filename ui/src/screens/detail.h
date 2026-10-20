// Match detail: header (back, score or kick-off, formation chip) over three
// swipeable sub-tabs, Summary / Pitch / Stats. Mockup: screen 2 of
// docs/superpowers/specs/assets/ui-mockup-v2.html (Summary and Stats follow
// the same visual language).
//
// Shared with the ESP32 firmware: no clock reads, no SDL, no stdio.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "image_provider.h"
#include "model.h"

enum class DetailTab { Summary = 0, Pitch = 1, Stats = 2 };

// What the detail needs besides the match data.
struct DetailContext {
  int tzOffset = 0;         // seconds east of UTC, for the kick-off time
  std::string favouriteId;  // the chip shows this team's formation (home if it does not play)
};

// Builds the detail as a child of `parent`, filling it. `onBack` is called by
// the Back control. Default sub-tab: Pitch for a live match with lineups,
// otherwise Summary.
lv_obj_t* detail_create(lv_obj_t* parent, const MatchDetail& detail, ImageProvider& images,
                        std::function<void()> onBack, const DetailContext& ctx = {});

// Shows new data. Only what changed is touched: the header in place, a
// sub-tab's content rebuilt when its data changed. Also reveals player photos
// that arrived since the last call. Returns true when anything changed.
bool detail_update(lv_obj_t* screen, const MatchDetail& detail, const DetailContext& ctx);

// The same frame (Back, header) while the detail is loading or after it
// failed. `match` (may be nullptr) fills the header when known. The error
// state has a "Try again" button that calls `onRetry`.
enum class DetailPlaceholder { Loading, Error };
lv_obj_t* detail_create_placeholder(lv_obj_t* parent, const MatchSummary* match,
                                    DetailPlaceholder kind, const DetailContext& ctx,
                                    std::function<void()> onBack, std::function<void()> onRetry);

void detail_select_tab(lv_obj_t* screen, DetailTab tab, bool animate);
DetailTab detail_active_tab(lv_obj_t* screen);

// Widgets of a detail (or placeholder), for tests and screenshots. Fields that
// do not apply are nullptr (for example tabview on a placeholder).
struct DetailParts {
  lv_obj_t* back = nullptr;     // "< Back" tap target
  lv_obj_t* status = nullptr;   // top right: clock, "FT", kick-off time or "Postponed"
  lv_obj_t* liveDot = nullptr;  // amber dot next to the clock, live only
  lv_obj_t* title = nullptr;    // "ARS 2:1 CHE" or "ARS v CHE"
  lv_obj_t* chip = nullptr;     // formation chip, hidden when unknown
  lv_obj_t* chipLabel = nullptr;
  lv_obj_t* tabLabels[3] = {nullptr, nullptr, nullptr};  // Summary, Pitch, Stats
  lv_obj_t* tabview = nullptr;

  // Pitch tab.
  lv_obj_t* blurb = nullptr;          // "ARS: 4 defenders, 3 midfielders, 3 attackers"
  lv_obj_t* homeFormation = nullptr;  // "ARS 4-3-3" tag by the home goal (bottom)
  lv_obj_t* awayFormation = nullptr;  // "CHE 4-2-3-1" tag by the away goal (top)
  lv_obj_t* pitch = nullptr;         // its marker children are the players (see detail_marker)
  lv_obj_t* pitchMessage = nullptr;  // shown instead of the pitch when no lineup is out
  lv_obj_t* halfNote = nullptr;      // "CHE lineup not published yet" when only one is out
  lv_obj_t* nameCard = nullptr;
  lv_obj_t* nameCardName = nullptr;
  lv_obj_t* nameCardNumber = nullptr;

  // Summary tab, in display order.
  lv_obj_t* summaryEmpty = nullptr;
  std::vector<lv_obj_t*> eventMinutes;
  std::vector<lv_obj_t*> eventPlayers;

  // Stats tab.
  lv_obj_t* statsEmpty = nullptr;
  std::vector<lv_obj_t*> statRows;

  // Placeholder.
  lv_obj_t* message = nullptr;  // "Loading match..." or the error text
  lv_obj_t* retry = nullptr;    // "Try again", error only
};

// Parts of an object made by detail_create() or detail_create_placeholder(),
// else nullptr. Pointers stay valid until the next detail_update().
const DetailParts* detail_parts(lv_obj_t* screen);

// A player marker on the pitch.
struct MarkerInfo {
  Side side = Side::Home;
  int number = 0;
  std::string name;
  lv_obj_t* numberLabel = nullptr;  // shirt number, hidden once a photo shows
  lv_obj_t* photo = nullptr;        // nullptr when the player has no photo path
};

// Info of a marker object, or nullptr for any other object.
const MarkerInfo* detail_marker(lv_obj_t* obj);
