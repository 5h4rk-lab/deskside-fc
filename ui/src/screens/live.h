// Live screen (home tab): the favourite team's live match over a diagonal
// split in both team colours, else a card for its next match, else a card
// for its last result read favourite first (its crest, colour and goals on
// the left, the opponent's on the right; "vs CHE", "FT" and how long ago),
// else "No matches soon" ("Connecting..." before any data).
// The live and last-result cards open the match when tapped. Mockup: screen 1
// of docs/superpowers/specs/assets/ui-mockup-v2.html.
//
// Shared with the ESP32 firmware: no clock reads, no SDL, no stdio. Every
// time-dependent text uses the `now` passed to live_refresh() and the Store's
// timezone offset.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <utility>

#include "image_provider.h"
#include "screens/widgets.h"
#include "store.h"

// Colours for the home (left) and away (right) halves of the split. When the
// two are too close to tell apart (RGB distance below kSplitMinDistance), the
// away half becomes theme().opponent; if that is also too close to home, a
// darkened home colour (or, for very dark home colours, a lightened one).
constexpr int kSplitMinDistance = 80;  // on the 0..441 RGB distance scale
std::pair<lv_color_t, lv_color_t> split_colors(lv_color_t home, lv_color_t away);

// The Live background halves for two teams' primaries (Team::primary,
// 0xRRGGBB): each primary goes through background_tone() (theme.h) so a very
// dark one still shows under the vignette, then split_colors() separates
// them. The result is used as is (never toned again), so the two halves stay
// at least kSplitMinDistance apart whenever split_colors() can manage it.
std::pair<lv_color_t, lv_color_t> live_background_colors(uint32_t homePrimary, uint32_t awayPrimary);

// A crest on a half darker than this (relative luminance of its
// live_background_colors() side) gets a soft light halo behind it, so dark
// crests (Spurs, Fulham, Juventus...) read as emblems on a lit disc.
constexpr float kHaloMaxLuminance = 0.06f;
bool crest_needs_halo(lv_color_t side);

// Builds the screen as a child of `parent`, filling it. `onOpen` is called
// with the match id when the score area is tapped. The screen shows
// "Connecting..." until the first live_refresh().
lv_obj_t* live_create(lv_obj_t* parent, Store& store, ImageProvider& images,
                      std::function<void(std::string matchId)> onOpen);

// Shows the gear at the top right, in the room the status row leaves on every
// screen (widgets::kStatusRightInset), and calls `onSettings` when it is tapped. An empty function hides
// the gear again.
void live_set_on_settings(lv_obj_t* screen, std::function<void()> onSettings);

// Updates the screen from the Store. Widgets are reused: only data that
// changed since the last call is touched.
void live_refresh(lv_obj_t* screen, int64_t now);

// Widgets of a live screen, for tests and screenshots.
struct LiveParts {
  lv_obj_t* background;   // split in team colours (custom drawn)
  lv_obj_t* statusLeague; // status row, left
  lv_obj_t* statusClock;  // status row, right: local HH:MM
  lv_obj_t* staleDot;     // amber dot, shown only when the data is stale
  lv_obj_t* staleLabel;   // "Updated N min ago", shown only when stale
  lv_obj_t* message;      // "Connecting...", "No matches soon" or "Next: ..."
  lv_obj_t* countdown;    // under the next-match message: "Kick-off in ..."
  lv_obj_t* scoreArea;    // tap target holding crests, pill, score and names
  lv_obj_t* homeHalo;     // light disc behind the crest, shown on a dark half only
  lv_obj_t* awayHalo;
  lv_obj_t* homeCrest;
  lv_obj_t* awayCrest;
  lv_obj_t* pill;         // amber minute pill
  lv_obj_t* pillDot;      // pulsing red dot
  lv_obj_t* pillLabel;    // match clock
  lv_obj_t* score;        // row of scoreHome, scoreSep, scoreAway
  lv_obj_t* scoreHome;
  lv_obj_t* scoreSep;
  lv_obj_t* scoreAway;
  lv_obj_t* homeName;
  lv_obj_t* awayName;
  lv_obj_t* eventStrip;   // glass strip with the latest event
  lv_obj_t* eventCutout;
  lv_obj_t* eventCaption; // "58'  Goal"
  lv_obj_t* eventPlayer;
  lv_obj_t* lastCaption;  // last result card: "Last result" (Hanken 10, dim)
  lv_obj_t* lastLine;     // row holding lastOpponent and the FT tag
  lv_obj_t* lastOpponent; // "vs CHE"
  lv_obj_t* lastFt;       // "FT" inside an outlined tag
  lv_obj_t* lastWhen;     // relative_time() of the kick-off: "yesterday"
  lv_obj_t* gear;         // settings button (32x32), hidden until live_set_on_settings()
  widgets::StatusRow status;  // the whole status row (league left; stale, clock, battery right)
};

// Parts of a screen made by live_create(), or nullptr for any other object.
const LiveParts* live_parts(lv_obj_t* screen);
