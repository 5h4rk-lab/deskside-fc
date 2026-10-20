#include <doctest/doctest.h>

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
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
  CountingSource() : MockSource("/nonexistent-fixtures") {}
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
  m.home = {"359", "ARS", "Arsenal", "", 0xEF0107, ""};
  m.away = {"363", "CHE", "Chelsea", "", 0x034694, ""};
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

TEST_CASE("goal overlay: a light accent darkens the background") {
  Fixture f;
  theme_set_team(0xFFFFFF, 0x034694);
  goal_set_reduced_motion(true);
  goal_celebrate(goal_event(), f.images);
  test_advance(0);
  CHECK(lv_color_to_u32(pixel_at(lv_layer_top(), 0, 0)) ==
        q565(darken(lv_color_hex(0xFFFFFF), goal::kLightDarken)));
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
    app = app_create(lv_screen_active(), AppDeps{store, images, source, nullptr});
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
};

}  // namespace

TEST_CASE("app goal path: first sight is silent, a favourite goal celebrates and names the scorer") {
  AppFixture f;
  f.poll();
  CHECK_FALSE(goal_active());
  CHECK(f.source.details.empty());

  f.source.advance_score("m1", Side::Home);  // 3:1
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
