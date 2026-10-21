#include <doctest/doctest.h>

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "prefs.h"
#include "demo_data.h"
#include "mock_source.h"
#include "screens/goal.h"
#include "snapshot.h"
#include "store.h"
#include "test_display.h"
#include "theme.h"

namespace {

// Never delivers an image; records what was asked for.
class NullImages : public ImageProvider {
 public:
  void request(const std::string& path, std::function<void(ImagePtr)> done) override {
    requested.push_back(path);
    done(nullptr);
  }
  std::vector<std::string> requested;
};

// MockSource without fixtures that counts detail fetches.
class CountingSource : public MockSource {
 public:
  CountingSource() = default;
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override {
    details.push_back(id);
    MockSource::fetch_detail(id, std::move(cb));
  }
  std::vector<std::string> details;
};

std::string text(lv_obj_t* label) { return lv_label_get_text(label); }

bool visible(lv_obj_t* obj) {
  for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

// A colour as the RGB565 panel shows it (what pixel_at returns).
uint32_t q565(lv_color_t c) {
  const uint8_t r5 = c.red >> 3;
  const uint8_t g6 = c.green >> 2;
  const uint8_t b5 = c.blue >> 3;
  return lv_color_to_u32(lv_color_make(static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
                                       static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
                                       static_cast<uint8_t>((b5 << 3) | (b5 >> 2))));
}

// Advances LVGL time in display-refresh sized steps.
void run_for(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) test_advance(10);
}

GoalEvent goal_event(bool fullTime = false) {
  GoalEvent e;
  e.matchId = "m1";
  e.favourite = true;
  e.fullTime = fullTime;
  e.side = Side::Home;
  e.clock = fullTime ? "FT" : "58'";
  e.homeScore = fullTime ? 3 : 2;
  e.awayScore = 1;
  e.homeAbbr = "ARS";
  e.awayAbbr = "CHE";
  return e;
}

struct Fixture {
  NullImages images;
  Fixture() {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(false);
  }
  ~Fixture() {
    goal_set_reduced_motion(false);
    theme_reset();
    deinit_test_display();
  }
};

MatchSummary live_match(int home, int away) {
  MatchSummary m;
  m.id = "m1";
  m.league = "eng.1";
  m.state = MatchState::Live;
  m.clock = "67'";
  m.kickoff = 1792526400;
  m.home = {"359", "ARS", "Arsenal", "", 0xEF0107, 0xEF0107, ""};
  m.away = {"363", "CHE", "Chelsea", "", 0xDBA111, 0x034694, ""};
  m.homeScore = home;
  m.awayScore = away;
  return m;
}

MatchDetail demo_detail_for(const MatchSummary& m) {
  MatchDetail d = demo_detail(demo::kLiveMatchId);
  d.match = m;
  return d;
}

}  // namespace

// ------------------------------------------------------------ latest_scorer

TEST_CASE("latest_scorer: last goal of the side in minute order, added time included") {
  MatchDetail d;
  d.events = {
      {"45'+2'", EventType::Goal, Side::Home, "Late Firsthalf", ""},
      {"12'", EventType::Goal, Side::Home, "Early Goal", ""},
      {"46'", EventType::Goal, Side::Away, "Away Scorer", ""},
      {"45'", EventType::Goal, Side::Home, "On The Minute", ""},
      {"80'", EventType::Yellow, Side::Home, "Booked Player", ""},
  };
  auto home = latest_scorer(d, Side::Home);
  REQUIRE(home);
  CHECK(home->name == "Late Firsthalf");
  auto away = latest_scorer(d, Side::Away);
  REQUIRE(away);
  CHECK(away->name == "Away Scorer");

  d.events.push_back({"45'+2'", EventType::Goal, Side::Home, "Same Minute Later", ""});
  CHECK(latest_scorer(d, Side::Home)->name == "Same Minute Later");
}

TEST_CASE("latest_scorer: none for a side without goals") {
  MatchDetail d;
  CHECK_FALSE(latest_scorer(d, Side::Home));
  d.events = {{"30'", EventType::Goal, Side::Away, "Cole Palmer", ""},
              {"40'", EventType::Red, Side::Home, "Somebody", ""}};
  CHECK_FALSE(latest_scorer(d, Side::Home));
}

TEST_CASE("latest_scorer: lineup photo when the player is in that side's lineup") {
  const MatchDetail d = demo_detail(demo::kLiveMatchId);
  auto s = latest_scorer(d, Side::Home);
  REQUIRE(s);
  CHECK(s->name == "Kai Havertz");
  CHECK(s->photo == "/api/cutout?name=Kai%20Havertz&w=40&h=40");

  MatchDetail other;
  other.events = {{"9'", EventType::Goal, Side::Home, "Jürgen Nobody", ""}};
  auto built = latest_scorer(other, Side::Home);
  REQUIRE(built);
  CHECK(built->photo == "/api/cutout?name=J%C3%BCrgen%20Nobody&w=96&h=96");

  // In the lineup but without a photo: built path.
  MatchDetail raya = demo_detail(demo::kLiveMatchId);
  raya.events = {{"90'", EventType::Goal, Side::Home, "David Raya", ""}};
  CHECK(latest_scorer(raya, Side::Home)->photo == "/api/cutout?name=David%20Raya&w=96&h=96");
}

TEST_CASE("latest_scorer: own goals still name the player") {
  MatchDetail d = demo_detail(demo::kLiveMatchId);
  d.events.push_back({"70'", EventType::Goal, Side::Home, "Levi Colwill", "Own goal"});
  auto s = latest_scorer(d, Side::Home);
  REQUIRE(s);
  CHECK(s->name == "Levi Colwill");
  CHECK(s->photo == "/api/cutout?name=Levi%20Colwill&w=96&h=96");
}

TEST_CASE("scorer_for_goal: a detail that predates the goal names nobody") {
  GoalEvent e = goal_event();
  e.homeScore = 2;
  e.awayScore = 0;
  MatchDetail d;
  d.events = {{"12'", EventType::Goal, Side::Home, "Bukayo Saka", ""}};
  CHECK_FALSE(scorer_for_goal(d, e));
  // An away goal does not count for the home side.
  d.events.push_back({"30'", EventType::Goal, Side::Away, "Cole Palmer", ""});
  CHECK_FALSE(scorer_for_goal(d, e));
}

TEST_CASE("scorer_for_goal: with as many goals as the score, the last one by minute") {
  GoalEvent e = goal_event();
  e.homeScore = 2;
  e.awayScore = 0;
  MatchDetail d;
  d.events = {{"58'", EventType::Goal, Side::Home, "Kai Havertz", ""},
              {"12'", EventType::Goal, Side::Home, "Bukayo Saka", ""}};
  auto s = scorer_for_goal(d, e);
  REQUIRE(s);
  CHECK(s->name == "Kai Havertz");

  // Away side: counts the away goals against awayScore.
  GoalEvent a = goal_event();
  a.side = Side::Away;
  a.homeScore = 0;
  a.awayScore = 1;
  MatchDetail da;
  CHECK_FALSE(scorer_for_goal(da, a));
  da.events = {{"70'", EventType::Goal, Side::Away, "Cole Palmer", ""}};
  REQUIRE(scorer_for_goal(da, a));
  CHECK(scorer_for_goal(da, a)->name == "Cole Palmer");
}

// ------------------------------------------------------------ texts

TEST_CASE("goal texts: headline, score line and waiting line") {
  GoalEvent e = goal_event();
  CHECK(goal_headline(e) == "GOAL!");
  CHECK(goal_score_line(e) == "58'  \xC2\xB7  ARS 2 : 1 CHE");
  CHECK(goal_waiting_line(e) == "ARS scored");
  e.clock.clear();
  CHECK(goal_score_line(e) == "ARS 2 : 1 CHE");
  e.side = Side::Away;
  CHECK(goal_waiting_line(e) == "CHE scored");

  const GoalEvent w = goal_event(true);
  CHECK(goal_headline(w) == "WIN!");
  CHECK(goal_score_line(w) == "Full time  \xC2\xB7  ARS 3 : 1 CHE");
}

TEST_CASE("goal background: the accent, darkened only when it is very light") {
  const lv_color_t red = lv_color_hex(0xEF0107);
  CHECK(lv_color_to_u32(goal_background(red)) == lv_color_to_u32(red));
  const lv_color_t white = lv_color_hex(0xFFFFFF);
  CHECK(lv_color_to_u32(goal_background(white)) == lv_color_to_u32(darken(white, 40)));
  const lv_color_t yellow = lv_color_hex(0xFDE100);  // Dortmund
  CHECK(lv_color_to_u32(goal_background(yellow)) == lv_color_to_u32(darken(yellow, 40)));
  const lv_color_t blue = lv_color_hex(0x6CABDD);  // Man City sky blue: mid luminance
  CHECK(lv_color_to_u32(goal_background(blue)) == lv_color_to_u32(blue));
}

// ------------------------------------------------------------ overlay

TEST_CASE("goal overlay: floods the top layer with the accent, then leaves after 4 s") {
  Fixture f;
  CHECK_FALSE(goal_active());
  CHECK(goal_parts() == nullptr);
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  CHECK(goal_active());
  REQUIRE(goal_parts() != nullptr);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 1);
  CHECK(lv_obj_get_width(goal_parts()->overlay) == 240);
  CHECK(lv_obj_get_height(goal_parts()->overlay) == 320);
  CHECK(lv_color_to_u32(pixel_at(lv_layer_top(), 0, 0)) == q565(theme().accent));

  run_for(3500);
  CHECK(goal_active());
  run_for(700);  // 4.2 s
  CHECK_FALSE(goal_active());
  CHECK(goal_parts() == nullptr);
  run_for(50);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
}

TEST_CASE("goal overlay: events that are not the favourite's are ignored") {
  Fixture f;
  GoalEvent other = goal_event();
  other.favourite = false;
  goal_celebrate(other, f.images);
  test_advance(10);
  CHECK_FALSE(goal_active());
  CHECK(goal_parts() == nullptr);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);

  // A showing overlay is left exactly as it is: same object, same timers.
  goal_celebrate(goal_event(), f.images);
  run_for(3000);
  lv_obj_t* overlay = goal_parts()->overlay;
  goal_celebrate(other, f.images);
  CHECK(goal_parts()->overlay == overlay);
  CHECK(text(goal_parts()->scoreLine) == "58'  \xC2\xB7  ARS 2 : 1 CHE");
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 1);
  run_for(1200);  // 4.2 s after the favourite's goal
  CHECK_FALSE(goal_active());
}

namespace {

void count_draw(lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); }

// Frames (of `step` ms each) in which the rays were drawn, over `ms`.
int frames_drawn(int& draws, uint32_t ms, uint32_t step = 10) {
  int frames = 0;
  for (uint32_t t = 0; t < ms; t += step) {
    const int before = draws;
    test_advance(step);
    if (draws != before) ++frames;
  }
  return frames;
}

}  // namespace

TEST_CASE("goal overlay: full frame rate for the entrance, then repaints every kIdleFrameMs (25 fps)") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  int draws = 0;
  lv_obj_add_event_cb(goal_parts()->rays, count_draw, LV_EVENT_DRAW_MAIN, &draws);

  // Entrance: every display refresh repaints. With 10 ms steps the 33 ms
  // refresh timer runs every 40 ms.
  const int refreshMs = (LV_DEF_REFR_PERIOD + 9) / 10 * 10;
  const int entrance = frames_drawn(draws, 400);
  CHECK(entrance >= 400 / refreshMs - 1);

  run_for(200);  // past kEntranceMs
  const int idle = frames_drawn(draws, 1000);
  const int expected = static_cast<int>(1000 / goal::kIdleFrameMs);
  CHECK(idle >= expected - 2);
  CHECK(idle <= expected + 2);
  CHECK(idle <= 1000 / refreshMs + 2);  // never faster than the display refresh
}

TEST_CASE("goal overlay: a full-time win stays about 8 s") {
  Fixture f;
  goal_celebrate(goal_event(true), f.images);
  run_for(4200);
  CHECK(goal_active());
  run_for(4300);  // 8.5 s
  CHECK_FALSE(goal_active());
  run_for(50);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
}

TEST_CASE("goal overlay: a second call replaces the content without stacking") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  run_for(2000);
  GoalEvent second = goal_event();
  second.homeScore = 3;
  second.clock = "71'";
  goal_celebrate(second, f.images);
  test_advance(0);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 1);
  REQUIRE(goal_parts() != nullptr);
  CHECK(text(goal_parts()->scoreLine) == "71'  \xC2\xB7  ARS 3 : 1 CHE");
  // Timers restarted: still showing 3 s after the second call.
  run_for(3000);
  CHECK(goal_active());

  // A full-time win right after a goal also replaces it.
  goal_celebrate(goal_event(true), f.images);
  test_advance(0);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 1);
  CHECK(text(goal_parts()->headline) == "WIN!");
  run_for(8600);
  CHECK_FALSE(goal_active());
}

TEST_CASE("goal overlay: a tap dismisses it at once") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  run_for(500);
  lv_obj_t* overlay = goal_parts()->overlay;
  lv_obj_send_event(overlay, LV_EVENT_CLICKED, nullptr);
  CHECK_FALSE(goal_active());
  test_advance(10);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);

  // Dismissed and immediately celebrated again: still a single overlay.
  goal_celebrate(goal_event(), f.images);
  lv_obj_send_event(goal_parts()->overlay, LV_EVENT_CLICKED, nullptr);
  goal_celebrate(goal_event(), f.images);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 1);
  test_advance(10);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 1);
  CHECK(goal_active());

  // A tap during the fade-out ends it once, cleanly.
  run_for(goal::kGoalMs - goal::kFadeMs + 100);
  REQUIRE(goal_active());
  lv_obj_send_event(goal_parts()->overlay, LV_EVENT_CLICKED, nullptr);
  CHECK_FALSE(goal_active());
  run_for(400);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
}

TEST_CASE("goal overlay: strip texts for a goal and for a win") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  const GoalParts* p = goal_parts();
  CHECK(text(p->headline) == "GOAL!");
  CHECK(text(p->headlineShadow) == "GOAL!");
  CHECK(text(p->name) == "ARS scored");
  CHECK(visible(p->name));
  CHECK(text(p->scoreLine) == "58'  \xC2\xB7  ARS 2 : 1 CHE");
  CHECK(p->matchId == "m1");
  CHECK_FALSE(p->fullTime);
  CHECK(p->confetti.size() == static_cast<size_t>(goal::kConfettiCount));

  goal_celebrate(goal_event(true), f.images);
  test_advance(0);
  p = goal_parts();
  CHECK(text(p->headline) == "WIN!");
  CHECK_FALSE(visible(p->name));
  CHECK(text(p->scoreLine) == "Full time  \xC2\xB7  ARS 3 : 1 CHE");
  CHECK(p->fullTime);
}

TEST_CASE("goal overlay: goal_set_scorer fills the name and cutout in place") {
  Fixture f;
  goal_set_scorer("m1", "Kai Havertz", "/api/cutout?name=Kai%20Havertz&w=40&h=40");  // inactive
  CHECK(f.images.requested.empty());

  goal_celebrate(goal_event(), f.images);
  run_for(100);
  lv_obj_t* overlay = goal_parts()->overlay;
  lv_obj_t* name = goal_parts()->name;
  const int32_t scaleBefore = lv_obj_get_style_transform_scale_x(goal_parts()->stamp, 0);

  goal_set_scorer("other-match", "Cole Palmer", "/api/cutout?name=Cole%20Palmer&w=40&h=40");
  CHECK(text(name) == "ARS scored");
  CHECK(f.images.requested.empty());

  goal_set_scorer("m1", "Kai Havertz", "/api/cutout?name=Kai%20Havertz&w=40&h=40");
  CHECK(goal_parts()->overlay == overlay);
  CHECK(goal_parts()->name == name);
  CHECK(text(name) == "KAI HAVERTZ");
  REQUIRE(f.images.requested.size() == 1);
  CHECK(f.images.requested[0] == "/api/cutout?name=Kai%20Havertz&w=192&h=192");
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 1);
  // The stamp keeps going rather than starting over.
  test_advance(10);
  CHECK(lv_obj_get_style_transform_scale_x(goal_parts()->stamp, 0) <= scaleBefore);

  // The lifetime is not extended either.
  run_for(4100);
  CHECK_FALSE(goal_active());
}

TEST_CASE("goal overlay: the stamp lands at scale 1.0 tilted by 4 degrees") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  lv_obj_t* stamp = goal_parts()->stamp;
  CHECK(lv_obj_get_style_transform_scale_x(stamp, 0) > 2 * LV_SCALE_NONE);
  CHECK(lv_obj_get_style_transform_rotation(stamp, 0) == goal::kStampRotation);
  run_for(300);
  CHECK(lv_obj_get_style_transform_scale_x(stamp, 0) == LV_SCALE_NONE);
  CHECK(lv_obj_get_style_transform_scale_y(stamp, 0) == LV_SCALE_NONE);
  CHECK(lv_obj_get_style_opa(stamp, 0) == LV_OPA_COVER);
}

TEST_CASE("goal overlay: the cutout rises into place") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  lv_obj_t* cutout = goal_parts()->cutout;
  lv_obj_update_layout(cutout);
  const int32_t startY = lv_obj_get_y(cutout);
  run_for(500);
  lv_obj_update_layout(cutout);
  const int32_t restY = lv_obj_get_y(cutout);
  CHECK(startY >= 320);
  CHECK(restY == 320 - goal::kCutoutSize);
  CHECK(lv_obj_get_height(cutout) == goal::kCutoutSize);
}

TEST_CASE("goal overlay: reduced motion is static but shows the same content") {
  Fixture f;
  goal_set_reduced_motion(true);
  CHECK(goal_reduced_motion());
  const uint16_t baseline = lv_anim_count_running();
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  CHECK(lv_anim_count_running() == baseline);
  const GoalParts* p = goal_parts();
  REQUIRE(p != nullptr);
  CHECK(text(p->headline) == "GOAL!");
  CHECK(p->confetti.empty());
  CHECK(lv_obj_get_style_transform_scale_x(p->stamp, 0) == LV_SCALE_NONE);
  CHECK(lv_obj_get_style_opa(p->stamp, 0) == LV_OPA_COVER);
  CHECK(lv_obj_get_y(p->cutout) == 320 - goal::kCutoutSize);
  CHECK(lv_color_to_u32(pixel_at(lv_layer_top(), 0, 0)) == q565(theme().accent));
  run_for(1000);
  CHECK(lv_anim_count_running() == baseline);
  run_for(3200);
  CHECK_FALSE(goal_active());
}

TEST_CASE("goal overlay: floods with the brand colour, not the accent (Chelsea blue, not gold)") {
  Fixture f;
  theme_set_team(0xDBA111, 0xEF0107, 0x034694);
  goal_set_reduced_motion(true);
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  const uint32_t px = lv_color_to_u32(pixel_at(lv_layer_top(), 0, 0));
  CHECK(px == q565(goal_background(lv_color_hex(0x034694))));
  CHECK(px == q565(lv_color_hex(0x034694)));
  CHECK(px != q565(lv_color_hex(0xDBA111)));
}

TEST_CASE("goal overlay: a light brand colour is darkened like a light accent") {
  Fixture f;
  theme_set_team(0x123456, 0x034694, 0xFDE100);  // Dortmund yellow as the brand
  goal_set_reduced_motion(true);
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  CHECK(lv_color_to_u32(pixel_at(lv_layer_top(), 0, 0)) ==
        q565(darken(lv_color_hex(0xFDE100), goal::kLightDarken)));
}

TEST_CASE("goal background: a very dark brand is toned up like the Live split") {
  const lv_color_t navy = lv_color_hex(0x132257);
  CHECK(lv_color_to_u32(goal_background(navy)) == lv_color_to_u32(background_tone(navy)));
  const lv_color_t black = lv_color_hex(0x000000);
  CHECK(lv_color_to_u32(goal_background(black)) == lv_color_to_u32(background_tone(black)));
}

TEST_CASE("goal overlay: a dark brand (Spurs navy, Fulham black) is lifted but stays dark") {
  {
    Fixture f;
    theme_set_team(0xFFFFFF, 0xEF0107, 0x132257);
    goal_set_reduced_motion(true);
    goal_celebrate(goal_event(), f.images);
    test_advance(0);
    const lv_color_t px = pixel_at(lv_layer_top(), 0, 0);
    CHECK(lv_color_to_u32(px) == q565(background_tone(lv_color_hex(0x132257))));
    CHECK(relative_luminance(px) >= kBackgroundMinLuminance - 0.005f);  // RGB565 rounding
    CHECK(relative_luminance(px) < 0.08f);
    CHECK(px.blue > px.red + 50);  // still navy
  }
  {
    Fixture f;
    theme_set_team(0xFFFFFF, 0xEF0107, 0x000000);
    goal_set_reduced_motion(true);
    goal_celebrate(goal_event(), f.images);
    test_advance(0);
    const lv_color_t px = pixel_at(lv_layer_top(), 0, 0);
    CHECK(lv_color_to_u32(px) == q565(background_tone(lv_color_hex(0x000000))));
    CHECK(relative_luminance(px) >= kBackgroundMinLuminance - 0.005f);
    CHECK(relative_luminance(px) < 0.08f);
  }
}

TEST_CASE("goal overlay: a light accent darkens the background") {
  Fixture f;
  theme_set_team(0xFFFFFF, 0x034694);
  goal_set_reduced_motion(true);
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  CHECK(lv_color_to_u32(pixel_at(lv_layer_top(), 0, 0)) ==
        q565(darken(lv_color_hex(0xFFFFFF), goal::kLightDarken)));
}

TEST_CASE("goal overlay: the screen under it is hidden while it animates and shown again at the fade, a tap or a replacement") {
  Fixture f;
  lv_obj_t* screen = lv_screen_active();
  CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));

  goal_celebrate(goal_event(), f.images);
  run_for(1000);
  CHECK(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
  run_for(2600);  // 3.6 s: the fade starts at 3.75 s
  CHECK(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
  run_for(300);   // the fade shows what is below
  CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
  run_for(600);
  CHECK_FALSE(goal_active());
  CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));

  // A replacement keeps it hidden; a dismissal brings it back at once.
  goal_celebrate(goal_event(), f.images);
  run_for(200);
  goal_celebrate(goal_event(true), f.images);
  run_for(200);
  CHECK(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
  goal_dismiss();
  CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
  run_for(100);

  // Reduced motion hides it too (the overlay covers it all the same).
  goal_set_reduced_motion(true);
  goal_celebrate(goal_event(), f.images);
  run_for(100);
  CHECK(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
  run_for(4200);
  CHECK_FALSE(goal_active());
  CHECK_FALSE(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE("goal overlay: a screen that was hidden before stays hidden") {
  Fixture f;
  lv_obj_t* screen = lv_screen_active();
  lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
  goal_celebrate(goal_event(), f.images);
  run_for(4500);
  CHECK_FALSE(goal_active());
  CHECK(lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN));
}

namespace {

// Hides everything of the overlay except the flood and the rays.
void leave_rays_only(const GoalParts* p) {
  for (lv_obj_t* c : p->confetti) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(p->stamp, LV_OBJ_FLAG_HIDDEN);
  if (p->stampImage != nullptr) lv_obj_add_flag(p->stampImage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(p->cutout, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(p->strip, LV_OBJ_FLAG_HIDDEN);
}

std::vector<uint32_t> probe_rays() {
  std::vector<uint32_t> out;
  for (int y = 8; y < 220; y += 11) {
    for (int x = 3; x < 240; x += 17) out.push_back(lv_color_to_u32(pixel_at(lv_layer_top(), x, y)));
  }
  return out;
}

int differing(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  int n = 0;
  for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
  return n;
}

}  // namespace

TEST_CASE("goal overlay: the rays turn and repeat every 15 degrees (250 ms), and sit on the flood colour") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  run_for(1000);  // all ray pictures are made by now
  const GoalParts* p = goal_parts();
  REQUIRE(p != nullptr);
  leave_rays_only(p);
  run_for(100);
  const uint32_t flood = q565(theme().accent);
  // Only two colours appear besides edge pixels: the flood and the lit ray.
  std::vector<uint32_t> a = probe_rays();
  int plain = 0;
  int lit = 0;
  for (uint32_t c : a) {
    if (c == flood) ++plain;
    else ++lit;
  }
  CHECK(plain > static_cast<int>(a.size()) / 4);
  CHECK(lit > static_cast<int>(a.size()) / 4);  // half the area is lit, give or take edges and the flood's own tint

  // Settle on a frame boundary, then compare one period (250 ms) and half a period apart.
  run_for(40);
  const std::vector<uint32_t> base = probe_rays();
  run_for(250);
  const std::vector<uint32_t> period = probe_rays();
  run_for(125);
  const std::vector<uint32_t> half = probe_rays();
  CHECK(differing(base, period) < static_cast<int>(base.size()) / 20);
  CHECK(differing(period, half) > static_cast<int>(base.size()) / 10);
}

TEST_CASE("goal overlay: the settled stamp is baked into a picture that looks like the live one") {
  Fixture f;
  goal_celebrate(goal_event(), f.images);
  run_for(100);
  const GoalParts* p = goal_parts();
  REQUIRE(p != nullptr);
  CHECK(p->stampImage == nullptr);  // still the live, animated stamp
  CHECK(visible(p->headline));
  for (lv_obj_t* c : p->confetti) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
  // Rays are a different picture every few frames: compare with them still.
  lv_obj_add_flag(p->rays, LV_OBJ_FLAG_HIDDEN);
  run_for(380);  // 480 ms: settled, not yet baked (the bake comes at 500 ms)
  std::vector<uint32_t> before;
  for (int y = 16; y < 112; y += 3) {
    for (int x = 20; x < 220; x += 3) before.push_back(lv_color_to_u32(pixel_at(lv_layer_top(), x, y)));
  }
  run_for(120);
  REQUIRE(p->stampImage != nullptr);
  CHECK_FALSE(visible(p->headline));  // its box is hidden now
  CHECK(visible(p->stampImage));
  std::vector<uint32_t> after;
  for (int y = 16; y < 112; y += 3) {
    for (int x = 20; x < 220; x += 3) after.push_back(lv_color_to_u32(pixel_at(lv_layer_top(), x, y)));
  }
  REQUIRE(before.size() == after.size());
  int far = 0;
  for (size_t i = 0; i < before.size(); ++i) {
    const lv_color_t a = lv_color_hex(before[i]);
    const lv_color_t b = lv_color_hex(after[i]);
    const int d = abs(a.red - b.red) + abs(a.green - b.green) + abs(a.blue - b.blue);
    if (d > 36) ++far;
  }
  CHECK(far < static_cast<int>(before.size()) / 100);
  // The picture sits where the live stamp was: the headline's colour is there.
  const lv_color_t chalk = theme().chalk;
  int chalky = 0;
  for (uint32_t c : after) {
    const lv_color_t px = lv_color_hex(c);
    if (abs(px.red - chalk.red) + abs(px.green - chalk.green) + abs(px.blue - chalk.blue) < 60) ++chalky;
  }
  CHECK(chalky > 40);
  run_for(4000);
  CHECK_FALSE(goal_active());
}

TEST_CASE("goal overlay: reduced motion has no baked stamp and no ray pictures") {
  Fixture f;
  goal_set_reduced_motion(true);
  goal_celebrate(goal_event(), f.images);
  run_for(1000);
  const GoalParts* p = goal_parts();
  REQUIRE(p != nullptr);
  CHECK(p->stampImage == nullptr);
  CHECK(visible(p->headline));
}

TEST_CASE("goal overlay: deleted with the display, no state left behind") {
  {
    Fixture f;
    goal_celebrate(goal_event(), f.images);
    run_for(100);
    CHECK(goal_active());
  }
  CHECK_FALSE(goal_active());
  Fixture f;
  CHECK_FALSE(goal_active());
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
}

// ------------------------------------------------------------ app path

namespace {

struct AppFixture {
  Store store;
  NullImages images;
  CountingSource source;
  MemoryPrefs prefs;
  lv_obj_t* app = nullptr;
  int64_t now = 1792526400;

  AppFixture() {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(false);
    store.set_favourite("359");
    MatchSummary m = live_match(2, 1);
    source.set_matches({m});
    source.set_detail(demo_detail_for(m));
    app = app_create(lv_screen_active(), AppDeps{store, images, source, nullptr, prefs});
  }
  ~AppFixture() {
    theme_reset();
    deinit_test_display();
  }

  // One poll: matches into the Store, then the app's goal check.
  void poll() {
    source.fetch_matches([this](bool ok, std::vector<MatchSummary> ms) {
      if (ok) store.set_matches(std::move(ms), now);
    });
    app_matches_changed(app, now);
    app_refresh(app, now);
    test_advance(0);
  }

  MatchSummary current() {
    MatchSummary out;
    source.fetch_matches([&](bool, std::vector<MatchSummary> ms) { out = ms.front(); });
    return out;
  }
  void set_current(const MatchSummary& m) {
    source.set_matches({m});
    source.set_detail(demo_detail_for(m));
  }
  // The demo detail of the current match plus one goal.
  MatchDetail with_goal(const char* minute, Side side, const char* player) {
    MatchDetail d = demo_detail_for(current());
    d.events.push_back({minute, EventType::Goal, side, player, ""});
    return d;
  }
  void add_goal(const char* minute, Side side, const char* player) {
    source.set_detail(with_goal(minute, side, player));
  }
};

}  // namespace

TEST_CASE("app goal path: first sight is silent, a favourite goal celebrates and names the scorer") {
  AppFixture f;
  f.poll();
  CHECK_FALSE(goal_active());
  CHECK(f.source.details.empty());

  f.source.advance_score("m1", Side::Home);  // 3:1
  f.add_goal("70'", Side::Home, "Kai Havertz");  // the detail already has the goal
  f.poll();
  CHECK(goal_active());
  REQUIRE(f.source.details.size() == 1);
  CHECK(f.source.details[0] == "m1");
  REQUIRE(goal_parts() != nullptr);
  CHECK(text(goal_parts()->name) == "KAI HAVERTZ");
  CHECK(text(goal_parts()->scoreLine) == "67'  \xC2\xB7  ARS 3 : 1 CHE");
  CHECK(f.store.detail_for("m1") != nullptr);
  bool askedCutout = false;
  for (const std::string& r : f.images.requested) {
    if (r == "/api/cutout?name=Kai%20Havertz&w=192&h=192") askedCutout = true;
  }
  CHECK(askedCutout);

  // Nothing new: nothing happens.
  run_for(4300);
  CHECK_FALSE(goal_active());
  f.poll();
  CHECK_FALSE(goal_active());
  CHECK(f.source.details.size() == 1);
  run_for(50);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
}

TEST_CASE("app goal path: opponent goals and VAR decreases do not celebrate") {
  AppFixture f;
  f.poll();
  f.source.advance_score("m1", Side::Away);  // 2:2
  f.poll();
  CHECK_FALSE(goal_active());
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
  CHECK(f.source.details.size() == 1);  // the Live strip still gets the new event
  CHECK(f.store.detail_for("m1") != nullptr);

  MatchSummary m = f.current();
  m.awayScore = 1;  // goal ruled out
  f.set_current(m);
  f.poll();
  CHECK_FALSE(goal_active());
  CHECK(f.source.details.size() == 1);
}

TEST_CASE("app goal path: a favourite win at full time celebrates WIN!") {
  AppFixture f;
  f.poll();
  MatchSummary m = f.current();
  m.state = MatchState::Post;
  m.clock = "FT";
  f.set_current(m);
  f.poll();
  CHECK(goal_active());
  REQUIRE(goal_parts() != nullptr);
  CHECK(text(goal_parts()->headline) == "WIN!");
  CHECK(goal_parts()->fullTime);
  CHECK(text(goal_parts()->scoreLine) == "Full time  \xC2\xB7  ARS 2 : 1 CHE");
  CHECK(f.source.details.size() == 1);
  run_for(8300);
  CHECK_FALSE(goal_active());
  run_for(50);
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
}

TEST_CASE("app goal path: a detail that lags behind the score leaves the scorer blank") {
  AppFixture f;
  f.poll();
  f.source.advance_score("m1", Side::Home);  // 3:1, the detail still has two home goals
  f.poll();
  CHECK(goal_active());
  REQUIRE(goal_parts() != nullptr);
  CHECK(text(goal_parts()->name) == "ARS scored");
  for (const std::string& r : f.images.requested) {
    CHECK(r.find("Kai%20Havertz&w=192") == std::string::npos);  // not the previous scorer
  }

  // A stale detail on refresh changes nothing.
  app_refresh(f.app, f.now + 1);
  CHECK(text(goal_parts()->name) == "ARS scored");

  // The detail watch brings the goal: the next refresh names the scorer.
  f.store.set_detail(f.with_goal("67'", Side::Home, "Gabriel Martinelli"));
  app_refresh(f.app, f.now + 15);
  CHECK(text(goal_parts()->name) == "GABRIEL MARTINELLI");
  bool asked = false;
  for (const std::string& r : f.images.requested) {
    if (r == "/api/cutout?name=Gabriel%20Martinelli&w=192&h=192") asked = true;
  }
  CHECK(asked);
}

TEST_CASE("app goal path: once the overlay is gone a late detail does nothing") {
  AppFixture f;
  f.poll();
  f.source.advance_score("m1", Side::Home);
  f.poll();
  REQUIRE(goal_active());
  goal_dismiss();
  run_for(50);
  CHECK_FALSE(goal_active());
  const size_t requests = f.images.requested.size();
  f.store.set_detail(f.with_goal("67'", Side::Home, "Gabriel Martinelli"));
  app_refresh(f.app, f.now + 15);
  run_for(50);
  CHECK_FALSE(goal_active());
  CHECK(lv_obj_get_child_count(lv_layer_top()) == 0);
  for (size_t i = requests; i < f.images.requested.size(); ++i) {
    CHECK(f.images.requested[i].find("w=192") == std::string::npos);  // no celebration cutout
  }
}
