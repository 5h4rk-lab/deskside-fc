// Goal celebration: a full-screen overlay on LVGL's top layer when the
// favourite team scores (about 4 s) or wins at full time (about 8 s). Mockup:
// screen 5 of docs/superpowers/specs/assets/ui-mockup-v2.html.
//
// The screen floods with the accent colour, light rays turn slowly behind the
// player, "GOAL!" (or "WIN!") stamps in, the scorer's cutout rises from the
// bottom over a dark strip with the name and the score, and a few confetti
// pieces fall. A tap dismisses it at once.
//
// Shared with the ESP32 firmware: no clock reads, no SDL, no stdio. All timing
// runs on LVGL time (animations and timers).
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "goal_detector.h"
#include "image_provider.h"
#include "model.h"

namespace goal {

constexpr uint32_t kGoalMs = 4000;        // goal overlay lifetime
constexpr uint32_t kWinMs = 8000;         // full-time win overlay lifetime
constexpr uint32_t kFadeMs = 250;         // fade-out at the end of the lifetime
constexpr uint32_t kStampMs = 220;        // "GOAL!" scale 2.2 -> 1.0 with an overshoot
constexpr uint32_t kStampFadeInMs = 90;   // "GOAL!" opacity 0 -> 1
constexpr int32_t kStampScaleStart = 563; // 2.2 x LV_SCALE_NONE
constexpr int32_t kStampRotation = -40;   // 0.1 degree units: -4 degrees
constexpr uint32_t kRiseDelayMs = 120;    // cutout starts rising after the stamp starts
constexpr uint32_t kRiseMs = 280;         // cutout rise duration (ease out)
constexpr uint32_t kRayTurnMs = 6000;     // one full turn of the rays
constexpr uint32_t kEntranceMs = 500;     // stamp and rise: rays and confetti at full frame rate
constexpr uint32_t kIdleFrameMs = 80;     // afterwards they move (and repaint) in steps of this
constexpr int kRayCount = 24;             // wedges of 7.5 degrees with equal gaps
constexpr lv_opa_t kRayOpa = 31;          // about 12 percent white
constexpr int kConfettiCount = 14;
constexpr int32_t kCutoutSize = 190;      // shown size (px), bottom aligned
constexpr int kCutoutRequest = 192;       // requested size of the cutout picture
constexpr float kLightAccent = 0.6f;      // relative luminance above which the background darkens
constexpr uint8_t kLightDarken = 40;      // darken() amount for a light accent

}  // namespace goal

// The scorer to show for a goal of `side`: the last Goal event of that side in
// minute order (equal minutes keep their input order). The photo is the
// lineup player's photo path when a player of that side has the same name
// and a photo, otherwise "/api/cutout?name=<name>&w=96&h=96". Own goals count
// for the side recorded in the event (the team that benefits). nullopt when
// that side has no goal event.
struct Scorer {
  std::string name;
  std::string photo;
};
std::optional<Scorer> latest_scorer(const MatchDetail& detail, Side side);

// The scorer for a celebration: latest_scorer(detail, event.side), but only
// when the detail has at least as many Goal events of that side (own goals
// counted for the side recorded in the event) as the side's score in the
// event; otherwise the detail predates the goal and the result is nullopt.
std::optional<Scorer> scorer_for_goal(const MatchDetail& detail, const GoalEvent& event);

// Texts of the overlay. Headline: "GOAL!" or, at full time, "WIN!". Score
// line: "58'  ·  ARS 2 : 1 CHE" (no minute part when the clock is empty), or
// "Full time  ·  ARS 3 : 1 CHE". Waiting line (until the scorer is known):
// "ARS scored" with the scoring side's abbreviation.
std::string goal_headline(const GoalEvent& event);
std::string goal_score_line(const GoalEvent& event);
std::string goal_waiting_line(const GoalEvent& event);

// Overlay background for an accent: the accent itself, or darken(accent, 40)
// when its relative luminance is above 0.6 (chalk text stays legible).
lv_color_t goal_background(lv_color_t accent);

// Shows the celebration on lv_layer_top() for a favourite event; events with
// favourite == false are ignored (nothing shown, a showing overlay is left as
// it is). A second favourite call while one is showing replaces it and
// restarts the timers: there is never more than one overlay. The scorer cutout and name
// start empty (a silhouette and goal_waiting_line()) until goal_set_scorer().
void goal_celebrate(const GoalEvent& event, ImageProvider& images);

// True while an overlay is showing (false as soon as it is dismissed).
bool goal_active();

// Fills in the scorer of the overlay showing `matchId`: name (in capitals) and
// cutout (`photoPath` re-sized to the cutout request size; empty keeps the
// silhouette). Updates in place without restarting anything. Ignored when no
// overlay is showing or it belongs to another match. The full-time version
// takes only the picture (it shows no scorer line).
void goal_set_scorer(const std::string& matchId, const std::string& name,
                     const std::string& photoPath);

// Removes the overlay now, if any.
void goal_dismiss();

// Static celebration: no ray rotation, stamp, rise, confetti or fade; the
// same content and lifetime. Applies to the next goal_celebrate().
void goal_set_reduced_motion(bool reduced);
bool goal_reduced_motion();

// Widgets of the showing overlay, for tests and screenshots; nullptr when
// none is showing.
struct GoalParts {
  lv_obj_t* overlay;      // full screen, background goal_background(accent)
  lv_obj_t* rays;         // custom-drawn rotating wedges
  lv_obj_t* stamp;        // transformed box holding the headline and its shadow
  lv_obj_t* headline;     // "GOAL!" / "WIN!"
  lv_obj_t* headlineShadow;
  lv_obj_t* cutout;       // scorer picture, silhouette drawn while it has no source
  lv_obj_t* strip;        // dark gradient at the bottom
  lv_obj_t* name;         // scorer name, or goal_waiting_line(); hidden at full time
  lv_obj_t* scoreLine;    // goal_score_line()
  std::vector<lv_obj_t*> confetti;  // empty in reduced motion
  std::string matchId;
  bool fullTime;
};
const GoalParts* goal_parts();
