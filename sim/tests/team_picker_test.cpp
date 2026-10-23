#include <doctest/doctest.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "screens/team_picker.h"
#include "test_display.h"
#include "theme.h"

namespace {

Team team(const char* id, const char* abbr, const char* name, const char* league,
          uint32_t color = 0x123456) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  t.league = league;
  t.color = color;
  t.crest = std::string("/api/img?u=crest") + id + "&w=48&h=48";
  return t;
}

// Deliberately not in display order.
std::vector<Team> sample_teams() {
  return {
      team("86", "RMA", "Real Madrid", "esp.1"),
      team("360", "MAN", "Manchester United", "eng.1"),
      team("132", "BAY", "Bayern Munich", "ger.1"),
      team("359", "ARS", "Arsenal", "eng.1", 0xEF0107),
      team("1068", "ATM", "Atl\xC3\xA9tico Madrid", "esp.1"),
      team("160", "PSG", "Paris Saint-Germain", "fra.1"),
      team("103", "MIL", "AC Milan", "ita.1"),
      team("382", "MNC", "Manchester City", "eng.1"),
      team("999", "XXX", "Somewhere Else", "usa.1"),
      team("367", "TOT", "Tottenham Hotspur", "eng.1", 0xFFFFFF),
  };
}

std::vector<std::string> names(const std::vector<Team>& teams) {
  std::vector<std::string> out;
  for (const Team& t : teams) out.push_back(t.name);
  return out;
}

class CountingImages : public ImageProvider {
 public:
  void request(const std::string& path, std::function<void(ImagePtr)> done) override {
    requested.push_back(path);
    done(nullptr);
  }
  std::vector<std::string> requested;
};

std::string text(lv_obj_t* label) { return lv_label_get_text(label); }

bool visible(lv_obj_t* obj) { return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }

// A picker on a 240x320 test display, filling the screen.
struct Picker {
  CountingImages images;
  std::vector<Team> picked;
  int backs = 0;
  lv_obj_t* obj = nullptr;

  explicit Picker(const std::vector<Team>& teams, const Team& current = team("359", "ARS", "Arsenal", "eng.1")) {
    init_test_display();
    theme_reset();
    obj = team_picker_create(lv_screen_active(), teams, current, images,
                             [this](Team t) { picked.push_back(t); }, [this] { ++backs; });
    settle();
  }
  ~Picker() { deinit_test_display(); }

  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  const TeamPickerParts* parts() const { return team_picker_parts(obj); }
  std::vector<std::string> listed() const {
    std::vector<std::string> out;
    for (const TeamRowParts& r : team_picker_rows(obj)) out.push_back(r.team.name);
    return out;
  }
};

}  // namespace

// ------------------------------------------------------------------ filtering

TEST_CASE("picker filter: case-insensitive substring of the name or abbreviation") {
  const std::vector<Team> teams = sample_teams();
  CHECK(names(filter_teams(teams, "ars")) == std::vector<std::string>{"Arsenal"});
  CHECK(names(filter_teams(teams, "ARS")) == std::vector<std::string>{"Arsenal"});
  // "psg" only matches the abbreviation.
  CHECK(names(filter_teams(teams, "psg")) == std::vector<std::string>{"Paris Saint-Germain"});
  CHECK(filter_teams(teams, "zzz").empty());
}

TEST_CASE("picker filter: \"man\" matches both Manchester clubs, ordered by league then name") {
  const std::vector<Team> teams = sample_teams();
  CHECK(names(filter_teams(teams, "man")) ==
        std::vector<std::string>{"Manchester City", "Manchester United"});
  // Premier League, then La Liga, then Ligue 1; by name inside a league.
  CHECK(names(filter_teams(teams, "ma")) ==
        std::vector<std::string>{"Manchester City", "Manchester United", "Atl\xC3\xA9tico Madrid",
                                 "Real Madrid", "Paris Saint-Germain"});
}

TEST_CASE("picker filter: the empty query lists every team by league then name") {
  CHECK(names(filter_teams(sample_teams(), "")) ==
        std::vector<std::string>{"Arsenal", "Manchester City", "Manchester United",
                                 "Tottenham Hotspur", "Atl\xC3\xA9tico Madrid", "Real Madrid",
                                 "AC Milan", "Bayern Munich", "Paris Saint-Germain",
                                 "Somewhere Else"});
}

TEST_CASE("picker filter: accents are not folded") {
  const std::vector<Team> teams = sample_teams();
  CHECK(filter_teams(teams, "atle").empty());
  CHECK(names(filter_teams(teams, "atl\xC3\xA9")) ==
        std::vector<std::string>{"Atl\xC3\xA9tico Madrid"});
}

TEST_CASE("picker leagues: the five supported leagues in display order") {
  CHECK(picker_leagues() ==
        std::vector<std::string>{"eng.1", "esp.1", "ita.1", "ger.1", "fra.1"});
}

// ------------------------------------------------------------------ screen

TEST_CASE("picker: every team listed with crest, name and league; the favourite ticked") {
  Picker p(sample_teams());
  const std::vector<TeamRowParts> rows = team_picker_rows(p.obj);
  REQUIRE(rows.size() == 10);
  CHECK(text(rows[0].name) == "Arsenal");
  CHECK(text(rows[0].league) == "Premier League");
  CHECK(text(rows[5].league) == "La Liga");
  CHECK(text(rows[6].league) == "Serie A");
  CHECK(text(rows[7].league) == "Bundesliga");
  CHECK(text(rows[8].league) == "Ligue 1");
  CHECK(lv_obj_get_width(rows[0].crest) == 24);
  CHECK(visible(rows[0].tick));
  for (size_t i = 1; i < rows.size(); ++i) CHECK_FALSE(visible(rows[i].tick));
  CHECK_FALSE(visible(p.parts()->message));
  CHECK(text(p.parts()->title) == kPickerTitle);
  CHECK(std::string(lv_textarea_get_placeholder_text(p.parts()->field)) == kSearchPlaceholder);
  // Crests are requested at 24 px.
  bool sized = false;
  for (const std::string& path : p.images.requested) {
    sized = sized || path.find("w=24&h=24") != std::string::npos;
  }
  CHECK(sized);
}

TEST_CASE("picker: typing filters the list live") {
  Picker p(sample_teams());
  team_picker_set_query(p.obj, "ars");
  p.settle();
  CHECK(p.listed() == std::vector<std::string>{"Arsenal"});
  team_picker_set_query(p.obj, "man");
  p.settle();
  CHECK(p.listed() == std::vector<std::string>{"Manchester City", "Manchester United"});
  team_picker_set_query(p.obj, "");
  p.settle();
  CHECK(p.listed().size() == 10);
}

TEST_CASE("picker: typing into the field itself filters (value changed)") {
  Picker p(sample_teams());
  lv_textarea_add_text(p.parts()->field, "tot");
  p.settle();
  CHECK(p.listed() == std::vector<std::string>{"Tottenham Hotspur"});
}

TEST_CASE("picker: no match shows the typed text") {
  Picker p(sample_teams());
  team_picker_set_query(p.obj, "xyzzy");
  p.settle();
  CHECK(p.listed().empty());
  REQUIRE(visible(p.parts()->message));
  CHECK(text(p.parts()->message) == "No teams match \"xyzzy\"");
  CHECK_FALSE(visible(p.parts()->retry));
}

TEST_CASE("picker: tapping a row picks that team") {
  Picker p(sample_teams());
  team_picker_set_query(p.obj, "city");
  p.settle();
  const std::vector<TeamRowParts> rows = team_picker_rows(p.obj);
  REQUIRE(rows.size() == 1);
  lv_obj_send_event(rows[0].row, LV_EVENT_CLICKED, nullptr);
  REQUIRE(p.picked.size() == 1);
  CHECK(p.picked[0].id == "382");
  CHECK(p.picked[0].name == "Manchester City");
  CHECK(p.picked[0].league == "eng.1");
}

TEST_CASE("picker: Back calls onBack") {
  Picker p(sample_teams());
  lv_obj_send_event(p.parts()->back, LV_EVENT_CLICKED, nullptr);
  CHECK(p.backs == 1);
}

TEST_CASE("picker: loading while empty, then the list when teams arrive") {
  Picker p({});
  CHECK(visible(p.parts()->message));
  CHECK(text(p.parts()->message) == kTeamsLoading);
  CHECK_FALSE(visible(p.parts()->retry));
  CHECK(team_picker_rows(p.obj).empty());

  team_picker_set_query(p.obj, "ars");  // typed while loading
  team_picker_set_teams(p.obj, sample_teams());
  p.settle();
  CHECK_FALSE(visible(p.parts()->message));
  CHECK(p.listed() == std::vector<std::string>{"Arsenal"});
}

TEST_CASE("picker: a failed load shows the error and Try again") {
  Picker p({});
  int retries = 0;
  team_picker_set_failed(p.obj, true, [&] { ++retries; });
  p.settle();
  CHECK(text(p.parts()->message) == kTeamsFailed);
  REQUIRE(visible(p.parts()->retry));
  lv_obj_send_event(p.parts()->retry, LV_EVENT_CLICKED, nullptr);
  CHECK(retries == 1);

  team_picker_set_failed(p.obj, false, nullptr);
  p.settle();
  CHECK(text(p.parts()->message) == kTeamsLoading);
  CHECK_FALSE(visible(p.parts()->retry));
}

TEST_CASE("picker: the keyboard shows on tapping the field and hides on OK or close") {
  Picker p(sample_teams());
  const TeamPickerParts* parts = p.parts();
  CHECK_FALSE(visible(parts->keyboard));
  lv_obj_send_event(parts->field, LV_EVENT_CLICKED, nullptr);
  p.settle();
  REQUIRE(visible(parts->keyboard));
  CHECK(lv_keyboard_get_textarea(parts->keyboard) == parts->field);
  CHECK(lv_keyboard_get_mode(parts->keyboard) == LV_KEYBOARD_MODE_TEXT_LOWER);

  // The list keeps room for at least two rows above the keyboard.
  const std::vector<TeamRowParts> rows = team_picker_rows(p.obj);
  REQUIRE(rows.size() >= 2);
  lv_area_t list;
  lv_obj_get_coords(parts->list, &list);
  lv_area_t kb;
  lv_obj_get_coords(parts->keyboard, &kb);
  CHECK(list.y2 < kb.y1);
  const int32_t rowPitch = lv_obj_get_y(rows[1].row) - lv_obj_get_y(rows[0].row);
  CHECK(lv_area_get_height(&list) >= 2 * rowPitch);

  lv_obj_send_event(parts->keyboard, LV_EVENT_READY, nullptr);
  p.settle();
  CHECK_FALSE(visible(parts->keyboard));

  lv_obj_send_event(parts->field, LV_EVENT_CLICKED, nullptr);
  p.settle();
  CHECK(visible(parts->keyboard));
  lv_obj_send_event(parts->keyboard, LV_EVENT_CANCEL, nullptr);
  p.settle();
  CHECK_FALSE(visible(parts->keyboard));
}

TEST_CASE("picker: scrolling the list hides the keyboard") {
  Picker p(sample_teams());
  team_picker_show_keyboard(p.obj, true);
  p.settle();
  REQUIRE(visible(p.parts()->keyboard));
  lv_obj_send_event(p.parts()->list, LV_EVENT_SCROLL_BEGIN, nullptr);
  p.settle();
  CHECK_FALSE(visible(p.parts()->keyboard));
}

TEST_CASE("picker: new teams keep the search text") {
  Picker p(sample_teams());
  team_picker_set_query(p.obj, "ars");
  std::vector<Team> more = sample_teams();
  more.push_back(team("1", "ARF", "Arsenal Fans", "eng.1"));
  team_picker_set_teams(p.obj, more);
  p.settle();
  CHECK(std::string(lv_textarea_get_text(p.parts()->field)) == "ars");
  CHECK(p.listed() == std::vector<std::string>{"Arsenal", "Arsenal Fans"});
}

TEST_CASE("picker: a white accent still shows a visible tick") {
  init_test_display();
  theme_set_team(0xFFFFFF, 0x034694);
  CHECK(lv_color_to_u32(accent_mark(theme().accent)) == lv_color_to_u32(lv_color_hex(0xFFFFFF)));
  // The fill behind a chalk knob is darkened.
  CHECK(relative_luminance(accent_fill(theme().accent)) < kLightLuminance);
  // A black accent is lightened as a mark.
  CHECK(relative_luminance(accent_mark(lv_color_hex(0x000000))) > 0.1f);
  theme_reset();
  deinit_test_display();
}

// ------------------------------------------------------------------ a long list

namespace {

// `n` teams in two leagues, named so their display order is easy to predict.
std::vector<Team> many_teams(int n) {
  std::vector<Team> out;
  for (int i = 0; i < n; ++i) {
    char id[8], abbr[8], name[32];
    snprintf(id, sizeof id, "%d", 1000 + i);
    snprintf(abbr, sizeof abbr, "T%02d", i);
    snprintf(name, sizeof name, "Club %02d", i);
    out.push_back(team(id, abbr, name, i % 2 ? "esp.1" : "eng.1"));
  }
  return out;
}

// Runs LVGL timers until the picker has every row (at most `limitMs`).
void fill(Picker& p, uint32_t limitMs = 5000) {
  for (uint32_t t = 0; t < limitMs && !team_picker_complete(p.obj); t += 10) test_advance(10);
}

}  // namespace

TEST_CASE("picker: a long list opens with its first rows and fills in steps, in display order") {
  const std::vector<Team> teams = many_teams(60);
  Picker p(teams);
  CHECK_FALSE(team_picker_complete(p.obj));
  CHECK(p.listed().size() >= kPickerFirstRows);
  CHECK(p.listed().size() < teams.size());
  // A step adds kPickerRowsPerStep rows every kPickerStepMs.
  const size_t before = p.listed().size();
  test_advance(kPickerStepMs);
  CHECK(p.listed().size() == before + kPickerRowsPerStep);
  fill(p);
  REQUIRE(team_picker_complete(p.obj));
  CHECK(p.listed() == names(filter_teams(teams, "")));
  CHECK_FALSE(visible(p.parts()->message));
}

TEST_CASE("picker: a short list is built at once") {
  Picker p(sample_teams());
  CHECK(team_picker_complete(p.obj));
  CHECK(p.listed().size() == sample_teams().size());
}

TEST_CASE("picker: a search typed while the list fills also filters the rows still to come") {
  const std::vector<Team> teams = many_teams(60);
  Picker p(teams);
  REQUIRE_FALSE(team_picker_complete(p.obj));
  team_picker_set_query(p.obj, "club 5");  // Club 50 to Club 59
  fill(p);
  REQUIRE(team_picker_complete(p.obj));
  CHECK(p.listed() == names(filter_teams(teams, "club 5")));
  CHECK(p.listed().size() == 10);
  CHECK_FALSE(visible(p.parts()->message));
}

TEST_CASE("picker: 'no match' counts the rows still to come") {
  const std::vector<Team> teams = many_teams(60);
  Picker p(teams);
  REQUIRE_FALSE(team_picker_complete(p.obj));
  // Only a team near the end matches: nothing built yet matches, but the
  // list is not empty, so no "No teams match".
  team_picker_set_query(p.obj, "club 59");
  CHECK_FALSE(visible(p.parts()->message));
  fill(p);
  CHECK(p.listed() == std::vector<std::string>{"Club 59"});
  team_picker_set_query(p.obj, "zzz");
  REQUIRE(visible(p.parts()->message));
  CHECK(text(p.parts()->message) == std::string(kNoMatchPrefix) + "zzz\"");
}

TEST_CASE("picker: deleted while it fills, its build step goes with it") {
  Picker p(many_teams(60));
  REQUIRE_FALSE(team_picker_complete(p.obj));
  lv_obj_delete(p.obj);
  for (int i = 0; i < 50; ++i) test_advance(kPickerStepMs);  // no step may touch the deleted picker (ASan)
  CHECK(true);
}

TEST_CASE("picker: a new team list while filling starts over with the new list") {
  Picker p(many_teams(60));
  REQUIRE_FALSE(team_picker_complete(p.obj));
  const std::vector<Team> fewer = many_teams(30);
  team_picker_set_teams(p.obj, fewer);
  fill(p);
  CHECK(p.listed() == names(filter_teams(fewer, "")));
}
