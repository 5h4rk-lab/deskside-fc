// Favourite team picker: a search field with the on-screen keyboard over the
// list of teams of the supported leagues. Opened from Settings. Shared with
// the ESP32 firmware: no clock reads, no SDL, no stdio.
#pragma once

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "image_provider.h"
#include "model.h"

// Display order of the leagues; teams of other leagues come after them.
// Premier League, La Liga, Serie A, Bundesliga, Ligue 1.
const std::vector<std::string>& picker_leagues();

// True when `query` is empty or a substring of the team's name or
// abbreviation, ignoring ASCII case. Accents are not folded: "atle" does not
// match "Atlético".
bool team_matches(const Team& team, const std::string& query);

// The teams matching `query`, ordered by league (picker_leagues() order, then
// unknown league codes alphabetically) and then by name (ignoring ASCII case).
std::vector<Team> filter_teams(const std::vector<Team>& teams, const std::string& query);

// Texts (exact wording). The empty-filter text is kNoMatchPrefix + query + "\"".
constexpr const char* kPickerTitle = "Favourite team";
constexpr const char* kSearchPlaceholder = "Search teams";
constexpr const char* kTeamsLoading = "Loading teams...";
constexpr const char* kTeamsFailed = "Could not load the team list. Check the connection.";
constexpr const char* kNoMatchPrefix = "No teams match \"";

// Building the rows: the first kPickerFirstRows at once, the rest
// kPickerRowsPerStep every kPickerStepMs (an LVGL timer), so a long list opens
// at once and the screen stays responsive while it fills (a row costs about
// 6 ms on the board: 96 rows built at once held the screen for 0.56 s). A
// search typed meanwhile filters the rows still to come too.
constexpr size_t kPickerFirstRows = 12;
constexpr size_t kPickerRowsPerStep = 3;
constexpr uint32_t kPickerStepMs = 30;

// Builds the picker as a child of `parent`, filling it. An empty `teams` shows
// the loading state until team_picker_set_teams() or
// team_picker_set_failed(). Tapping a row calls `onPick` (not after a scroll
// gesture); Back calls `onBack`.
lv_obj_t* team_picker_create(lv_obj_t* parent, const std::vector<Team>& teams, const Team& current,
                             ImageProvider& images, std::function<void(Team)> onPick,
                             std::function<void()> onBack);

// Replaces the team list (for example when the fetch completes). The search
// text, keyboard and scroll position stay.
void team_picker_set_teams(lv_obj_t* picker, const std::vector<Team>& teams);

// While the list is empty: the error state with "Try again" (calls onRetry)
// instead of the loading state.
void team_picker_set_failed(lv_obj_t* picker, bool failed, std::function<void()> onRetry);

// Sets the search text, as if typed.
void team_picker_set_query(lv_obj_t* picker, const std::string& query);

// Removes the crest placeholders of pictures that arrived since (and loads
// crests of rows that came into view). Cheap; the app calls it on refresh.
void team_picker_refresh(lv_obj_t* picker);

// Shows or hides the keyboard (tapping the field shows it; its OK and close
// keys and scrolling the list hide it).
void team_picker_show_keyboard(lv_obj_t* picker, bool show);

struct TeamRowParts {
  Team team;
  lv_obj_t* row = nullptr;
  lv_obj_t* crest = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* league = nullptr;
  lv_obj_t* tick = nullptr;  // shown on the current favourite only
};

struct TeamPickerParts {
  lv_obj_t* back = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* field = nullptr;     // lv_textarea, one line
  lv_obj_t* keyboard = nullptr;  // lv_keyboard, hidden until the field is tapped
  lv_obj_t* list = nullptr;
  lv_obj_t* message = nullptr;  // loading, error or no-match text; hidden over a list
  lv_obj_t* retry = nullptr;    // "Try again", error state only
};

// Parts of a picker made by team_picker_create(), or nullptr for any other object.
const TeamPickerParts* team_picker_parts(lv_obj_t* picker);
// The rows currently listed (built so far and matching the search), in display order.
std::vector<TeamRowParts> team_picker_rows(lv_obj_t* picker);
// True once every team has its row (see kPickerFirstRows).
bool team_picker_complete(lv_obj_t* picker);
