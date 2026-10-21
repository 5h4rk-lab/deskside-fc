#include <doctest/doctest.h>

#include <stdint.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <math.h>

#include <fstream>
#include <sstream>

#include "app.h"
#include "prefs.h"
#include "data_source.h"
#include "json_parse.h"
#include "demo_data.h"
#include "screens/detail.h"
#include "screens/live.h"
#include "store.h"
#include "test_display.h"
#include "theme.h"

namespace {

constexpr int64_t kKickoff = 1792526400;  // Tue 2026-10-20 20:00 UTC
constexpr int kIndia = 5 * 3600 + 30 * 60;

// Counts requests; delivers a tiny image for paths containing `deliver`
// (nothing when empty), nullptr otherwise.
class CountingImages : public ImageProvider {
 public:
  explicit CountingImages(std::string deliver = "") : deliver_(std::move(deliver)) {
    static const uint16_t kPixels[4] = {0xF800, 0xF800, 0xF800, 0xF800};
    auto* dsc = new lv_image_dsc_t{};
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = 2;
    dsc->header.h = 2;
    dsc->header.stride = 4;
    dsc->data = reinterpret_cast<const uint8_t*>(kPixels);
    dsc->data_size = sizeof(kPixels);
    image_ = ImagePtr(dsc, [](const lv_image_dsc_t* d) { delete d; });
  }
  void request(const std::string& path, std::function<void(ImagePtr)> done) override {
    requested.push_back(path);
    const bool give = !deliver_.empty() && path.find(deliver_) != std::string::npos;
    done(give ? image_ : nullptr);
  }
  std::vector<std::string> requested;

 private:
  std::string deliver_;
  ImagePtr image_;
};

// Stores the detail callback instead of calling it, so a test decides when
// (and how) the fetch completes.
class DeferredSource : public DataSource {
 public:
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    cb(false, {});
  }
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override {
    calls.push_back(id);
    pending = std::move(cb);
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override { cb(false, {}); }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override { cb(false, {}); }

  void complete(bool ok, MatchDetail d = {}) {
    auto cb = std::move(pending);
    pending = nullptr;
    REQUIRE(cb);
    cb(ok, std::move(d));
  }

  std::vector<std::string> calls;
  std::function<void(bool, MatchDetail)> pending;
};

std::string text(lv_obj_t* label) { return lv_label_get_text(label); }

bool visible(lv_obj_t* obj) {
  if (obj == nullptr) return false;
  for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

uint32_t count_descendants(lv_obj_t* obj) {
  uint32_t n = 0;
  const uint32_t c = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < c; ++i) {
    n += 1 + count_descendants(lv_obj_get_child(obj, static_cast<int32_t>(i)));
  }
  return n;
}

std::vector<lv_obj_t*> markers(lv_obj_t* pitch) {
  std::vector<lv_obj_t*> out;
  const uint32_t n = lv_obj_get_child_count(pitch);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* c = lv_obj_get_child(pitch, static_cast<int32_t>(i));
    if (detail_marker(c) != nullptr) out.push_back(c);
  }
  return out;
}

lv_obj_t* marker_named(lv_obj_t* pitch, const std::string& name) {
  for (lv_obj_t* m : markers(pitch)) {
    if (detail_marker(m)->name == name) return m;
  }
  return nullptr;
}

// Centre of `obj` relative to the top-left corner of `ref`.
lv_point_t centre_in(lv_obj_t* obj, lv_obj_t* ref) {
  lv_area_t a;
  lv_area_t r;
  lv_obj_get_coords(obj, &a);
  lv_obj_get_coords(ref, &r);
  return {(a.x1 + a.x2) / 2 - r.x1, (a.y1 + a.y2) / 2 - r.y1};
}

MatchSummary summary(MatchState state) {
  MatchSummary m;
  m.id = "m1";
  m.league = "eng.1";
  m.state = state;
  m.kickoff = kKickoff;
  m.home = {"359", "ARS", "Arsenal", "", 0xEF0107, 0xEF0107, ""};
  m.away = {"363", "CHE", "Chelsea", "", 0xDBA111, 0x034694, ""};
  if (state == MatchState::Live) m.clock = "67'";
  if (state == MatchState::Post) m.clock = "FT";
  if (state == MatchState::Live || state == MatchState::Post) {
    m.homeScore = 2;
    m.awayScore = 1;
  }
  return m;
}

// The demo's live Arsenal v Chelsea detail (22 players, events, stats).
MatchDetail full_detail() { return demo_detail(demo::kLiveMatchId); }

MatchDetail empty_detail(MatchState state = MatchState::Pre) {
  MatchDetail d;
  d.match = summary(state);
  return d;
}

struct Screen {
  CountingImages images;
  int backs = 0;
  lv_obj_t* screen = nullptr;

  explicit Screen(const MatchDetail& d, DetailContext ctx = {}, std::string deliver = "")
      : images(std::move(deliver)) {
    init_test_display();
    screen = detail_create(lv_screen_active(), d, images, [this] { ++backs; }, ctx);
    settle();
  }
  ~Screen() { deinit_test_display(); }

  const DetailParts* parts() const { return detail_parts(screen); }
  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
};

}  // namespace

// ------------------------------------------------------------------ pitch

TEST_CASE("pitch: 22 markers, home in the lower half and away in the upper half") {
  const MatchDetail d = full_detail();
  REQUIRE(d.home.players.size() == 11);
  REQUIRE(d.away.players.size() == 11);
  Screen s(d);
  const DetailParts* p = s.parts();
  REQUIRE(p != nullptr);
  REQUIRE(p->pitch != nullptr);
  CHECK(visible(p->pitch));
  CHECK_FALSE(visible(p->pitchMessage));

  const std::vector<lv_obj_t*> ms = markers(p->pitch);
  CHECK(ms.size() == 22);
  const int32_t h = lv_obj_get_height(p->pitch);
  const int32_t w = lv_obj_get_width(p->pitch);
  int home = 0;
  int away = 0;
  for (lv_obj_t* m : ms) {
    const MarkerInfo* info = detail_marker(m);
    const lv_point_t c = centre_in(m, p->pitch);
    CHECK(lv_obj_get_width(m) == 24);
    CHECK(lv_obj_get_height(m) == 24);
    if (info->side == Side::Home) {
      ++home;
      CHECK(c.y > h / 2);
    } else {
      ++away;
      CHECK(c.y < h / 2);
    }
    CHECK(c.x >= 0);
    CHECK(c.x <= w);
  }
  CHECK(home == 11);
  CHECK(away == 11);

  // Left-sided players (from their own perspective): home on the screen's
  // left, away mirrored onto the screen's right.
  int checked = 0;
  for (const LineupPlayer& pl : d.away.players) {
    if (pl.x >= 0.3f) continue;
    lv_obj_t* m = marker_named(p->pitch, pl.name);
    REQUIRE(m != nullptr);
    CHECK(centre_in(m, p->pitch).x > w / 2);
    ++checked;
  }
  for (const LineupPlayer& pl : d.home.players) {
    if (pl.x >= 0.3f) continue;
    lv_obj_t* m = marker_named(p->pitch, pl.name);
    REQUIRE(m != nullptr);
    CHECK(centre_in(m, p->pitch).x < w / 2);
    ++checked;
  }
  CHECK(checked >= 2);
}

TEST_CASE("pitch: markers use each team's primary colour (Chelsea blue, not the gold accent)") {
  MatchDetail d = full_detail();
  REQUIRE(d.match.away.abbr == "CHE");
  REQUIRE(d.match.away.color == 0xDBA111u);
  REQUIRE(d.match.away.primary == 0x034694u);
  Screen s(d);
  const DetailParts* p = s.parts();
  const auto expected = split_colors(lv_color_hex(d.match.home.primary), lv_color_hex(d.match.away.primary));
  int home = 0;
  int away = 0;
  for (lv_obj_t* m : markers(p->pitch)) {
    const MarkerInfo* info = detail_marker(m);
    const uint32_t bg = lv_color_to_u32(lv_obj_get_style_bg_color(m, 0));
    if (info->side == Side::Home) {
      CHECK(bg == lv_color_to_u32(expected.first));
      CHECK(bg == lv_color_to_u32(lv_color_hex(0xEF0107)));
      ++home;
    } else {
      CHECK(bg == lv_color_to_u32(expected.second));
      CHECK(bg == lv_color_to_u32(lv_color_hex(0x034694)));
      ++away;
    }
    // The white ring stays.
    CHECK(lv_color_to_u32(lv_obj_get_style_border_color(m, 0)) == lv_color_to_u32(theme().chalk));
  }
  CHECK(home == 11);
  CHECK(away == 11);
}

TEST_CASE("pitch: goalkeepers sit by their own goal lines") {
  MatchDetail d = full_detail();
  Screen s(d);
  const DetailParts* p = s.parts();
  const int32_t h = lv_obj_get_height(p->pitch);
  const LineupPlayer* homeKeeper = &d.home.players[0];
  const LineupPlayer* awayKeeper = &d.away.players[0];
  for (const LineupPlayer& pl : d.home.players) {
    if (pl.y < homeKeeper->y) homeKeeper = &pl;
  }
  for (const LineupPlayer& pl : d.away.players) {
    if (pl.y < awayKeeper->y) awayKeeper = &pl;
  }
  CHECK(centre_in(marker_named(p->pitch, homeKeeper->name), p->pitch).y > h * 3 / 4);
  CHECK(centre_in(marker_named(p->pitch, awayKeeper->name), p->pitch).y < h / 4);
}

TEST_CASE("pitch: unpublished lineups replace the pitch with a message") {
  Screen s(empty_detail());
  const DetailParts* p = s.parts();
  CHECK_FALSE(visible(p->pitch));
  REQUIRE(p->pitchMessage != nullptr);
  CHECK(text(p->pitchMessage) == "Lineups appear about an hour before kickoff");
}

TEST_CASE("pitch: one missing lineup leaves that half empty with a note") {
  MatchDetail d = full_detail();
  d.away.players.clear();
  d.away.formation.clear();
  Screen s(d);
  const DetailParts* p = s.parts();
  detail_select_tab(s.screen, DetailTab::Pitch, false);
  s.settle();
  CHECK(visible(p->pitch));
  CHECK(markers(p->pitch).size() == 11);
  REQUIRE(p->halfNote != nullptr);
  CHECK(visible(p->halfNote));
  CHECK(text(p->halfNote) == "CHE lineup not published yet");
  CHECK(centre_in(p->halfNote, p->pitch).y < lv_obj_get_height(p->pitch) / 2);
}

TEST_CASE("pitch: formation chip, both formations and the plain-English blurb") {
  Screen s(full_detail());
  const DetailParts* p = s.parts();
  CHECK(text(p->chipLabel) == "4-3-3");
  CHECK(visible(p->chip));
  CHECK(text(p->blurb) == "ARS: 4 defenders, 3 midfielders, 3 attackers");
  // Formation tags by each goal: home at the bottom, away at the top.
  REQUIRE(p->homeFormation != nullptr);
  REQUIRE(p->awayFormation != nullptr);
  CHECK(text(p->homeFormation) == "ARS 4-3-3");
  CHECK(text(p->awayFormation) == "CHE 4-2-3-1");
  const int32_t h = lv_obj_get_height(p->pitch);
  CHECK(centre_in(p->homeFormation, p->pitch).y > h * 3 / 4);
  CHECK(centre_in(p->awayFormation, p->pitch).y < h / 4);
}

TEST_CASE("pitch: the chip follows the favourite team when it plays away") {
  DetailContext ctx;
  ctx.favouriteId = "363";
  Screen s(full_detail(), ctx);
  const DetailParts* p = s.parts();
  CHECK(text(p->chipLabel) == "4-2-3-1");
  CHECK(text(p->blurb) == "CHE: 4 defenders, 5 midfielders, 1 attacker");
}

TEST_CASE("pitch: no chip when the formation is unknown") {
  Screen s(empty_detail());
  CHECK_FALSE(visible(s.parts()->chip));
}

TEST_CASE("pitch: tapping a marker toggles a name card; tapping elsewhere hides it") {
  Screen s(full_detail());
  const DetailParts* p = s.parts();
  detail_select_tab(s.screen, DetailTab::Pitch, false);
  s.settle();
  lv_obj_t* saka = marker_named(p->pitch, "Bukayo Saka");
  REQUIRE(saka != nullptr);
  CHECK_FALSE(visible(p->nameCard));

  lv_obj_send_event(saka, LV_EVENT_CLICKED, nullptr);
  s.settle();
  CHECK(visible(p->nameCard));
  CHECK(text(p->nameCardName) == "Bukayo Saka");
  CHECK(text(p->nameCardNumber) == "7");

  lv_obj_send_event(saka, LV_EVENT_CLICKED, nullptr);
  s.settle();
  CHECK_FALSE(visible(p->nameCard));

  lv_obj_t* rice = marker_named(p->pitch, "Declan Rice");
  lv_obj_send_event(rice, LV_EVENT_CLICKED, nullptr);
  s.settle();
  CHECK(text(p->nameCardName) == "Declan Rice");
  lv_obj_send_event(saka, LV_EVENT_CLICKED, nullptr);  // another player: card moves
  s.settle();
  CHECK(visible(p->nameCard));
  CHECK(text(p->nameCardName) == "Bukayo Saka");

  lv_obj_send_event(p->pitch, LV_EVENT_CLICKED, nullptr);  // the grass
  s.settle();
  CHECK_FALSE(visible(p->nameCard));
}

TEST_CASE("pitch: a missing photo shows the shirt number, a loaded one hides it") {
  Screen s(full_detail(), {}, "Kai%20Havertz");
  const DetailParts* p = s.parts();
  bool askedSaka = false;
  for (const std::string& r : s.images.requested) {
    if (r == "/api/cutout?name=Bukayo%20Saka&w=40&h=40") askedSaka = true;
  }
  CHECK(askedSaka);

  const MarkerInfo* saka = detail_marker(marker_named(p->pitch, "Bukayo Saka"));
  REQUIRE(saka != nullptr);
  CHECK(visible(saka->numberLabel));
  CHECK(text(saka->numberLabel) == "7");
  CHECK(lv_image_get_src(saka->photo) == nullptr);

  const MarkerInfo* havertz = detail_marker(marker_named(p->pitch, "Kai Havertz"));
  REQUIRE(havertz != nullptr);
  CHECK(lv_image_get_src(havertz->photo) != nullptr);
  CHECK_FALSE(visible(havertz->numberLabel));

  // Players without a photo path never ask for one.
  const MarkerInfo* raya = detail_marker(marker_named(p->pitch, "David Raya"));
  REQUIRE(raya != nullptr);
  CHECK(raya->photo == nullptr);
  CHECK(visible(raya->numberLabel));
}

// ------------------------------------------------------- pitch geometry

namespace {

MatchDetail fixture_detail() {
  std::ifstream in(std::string(CONTRACT_FIXTURES_DIR) + "/detail-normalised.json",
                   std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  MatchDetail d;
  REQUIRE(parse_detail(ss.str(), d));
  return d;
}

// A formation as the helper lays it out: lines evenly over y 0.08..0.92,
// players of a line evenly across x. `lines` excludes the keeper.
Lineup lineup(const std::string& formation, const std::vector<int>& lines) {
  Lineup l;
  l.formation = formation;
  l.players.push_back({"Keeper", 1, 0.5f, 0.08f, ""});
  int number = 2;
  for (size_t li = 0; li < lines.size(); ++li) {
    const float y = 0.08f + 0.84f * static_cast<float>(li + 1) / static_cast<float>(lines.size());
    for (int k = 0; k < lines[li]; ++k) {
      const float x = static_cast<float>(k + 1) / static_cast<float>(lines[li] + 1);
      l.players.push_back({"Player " + std::to_string(number), number, x, y, ""});
      ++number;
    }
  }
  return l;
}

// The detail in a 240x276 parent: the real overlay size above the tab bar.
struct OverlayScreen {
  CountingImages images;
  lv_obj_t* screen = nullptr;
  explicit OverlayScreen(const MatchDetail& d) {
    init_test_display();
    lv_obj_t* box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 240, 320 - 44);
    screen = detail_create(box, d, images, [] {});
    detail_select_tab(screen, DetailTab::Pitch, false);
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  ~OverlayScreen() { deinit_test_display(); }
};

double dist(lv_point_t a, lv_point_t b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return sqrt(dx * dx + dy * dy);
}

// No two markers overlap, every marker is inside the pitch and in its own
// half, and the formation tags do not cover a marker.
void check_layout(const MatchDetail& d) {
  OverlayScreen s(d);
  const DetailParts* p = detail_parts(s.screen);
  const std::vector<lv_obj_t*> ms = markers(p->pitch);
  REQUIRE(ms.size() == d.home.players.size() + d.away.players.size());
  const int32_t w = lv_obj_get_width(p->pitch);
  const int32_t h = lv_obj_get_height(p->pitch);
  CHECK(h >= 196);  // close to the mockup's 206 px
  lv_area_t pitch;
  lv_obj_get_coords(p->pitch, &pitch);
  const int32_t size = lv_obj_get_width(ms[0]);
  CHECK(size >= 20);
  for (size_t i = 0; i < ms.size(); ++i) {
    lv_area_t a;
    lv_obj_get_coords(ms[i], &a);
    CHECK(lv_obj_get_width(ms[i]) == size);
    CHECK(a.x1 >= pitch.x1);
    CHECK(a.x2 <= pitch.x2);
    CHECK(a.y1 >= pitch.y1);
    CHECK(a.y2 <= pitch.y2);
    if (detail_marker(ms[i])->side == Side::Home) {
      CHECK(a.y1 - pitch.y1 >= h / 2);
    } else {
      CHECK(a.y2 - pitch.y1 < h / 2 + 1);
    }
    for (size_t j = i + 1; j < ms.size(); ++j) {
      INFO(detail_marker(ms[i])->name << " vs " << detail_marker(ms[j])->name);
      CHECK(dist(centre_in(ms[i], p->pitch), centre_in(ms[j], p->pitch)) >= size);
    }
    for (lv_obj_t* tag : {p->homeFormation, p->awayFormation}) {
      if (tag == nullptr) continue;
      lv_area_t t;
      lv_obj_get_coords(tag, &t);
      const bool overlap = a.x1 <= t.x2 && t.x1 <= a.x2 && a.y1 <= t.y2 && t.y1 <= a.y2;
      CHECK_FALSE(overlap);
    }
  }
  (void)w;
}

// Centre y of each distinct row of one side, sorted.
std::vector<int32_t> row_centres(const DetailParts* p, Side side) {
  std::vector<int32_t> ys;
  for (lv_obj_t* m : markers(p->pitch)) {
    if (detail_marker(m)->side != side) continue;
    const int32_t y = centre_in(m, p->pitch).y;
    if (std::find(ys.begin(), ys.end(), y) == ys.end()) ys.push_back(y);
  }
  std::sort(ys.begin(), ys.end());
  return ys;
}

}  // namespace

TEST_CASE("pitch geometry: the contract fixture (3-1-4-2 v 4-2-3-1) has no overlaps") {
  const MatchDetail d = fixture_detail();
  REQUIRE(d.home.formation == "3-1-4-2");
  REQUIRE(d.away.formation == "4-2-3-1");
  check_layout(d);
}

TEST_CASE("pitch geometry: the demo detail has no overlaps") { check_layout(full_detail()); }

TEST_CASE("pitch geometry: rows are spread evenly, whatever the numbers in y") {
  MatchDetail d = empty_detail(MatchState::Live);
  d.home = lineup("5-3-2", {5, 3, 2});
  d.away = lineup("4-4-2", {4, 4, 2});
  check_layout(d);
  // Uneven raw y values: only their order counts.
  d.away.players[1].y = d.away.players[2].y = d.away.players[3].y = d.away.players[4].y = 0.15f;
  check_layout(d);

  OverlayScreen s(d);
  const DetailParts* p = detail_parts(s.screen);
  const int32_t size = lv_obj_get_width(markers(p->pitch)[0]);
  for (Side side : {Side::Home, Side::Away}) {
    const std::vector<int32_t> ys = row_centres(p, side);
    REQUIRE(ys.size() == 4);
    const int32_t step = ys[1] - ys[0];
    CHECK(step >= size);
    for (size_t i = 1; i < ys.size(); ++i) {
      CHECK(ys[i] - ys[i - 1] == step);
    }
  }
  // Keepers sit by their own goal lines.
  const int32_t h = lv_obj_get_height(p->pitch);
  const std::vector<int32_t> home = row_centres(p, Side::Home);
  const std::vector<int32_t> away = row_centres(p, Side::Away);
  CHECK(home.back() + size / 2 >= h - 1);
  CHECK(away.front() - size / 2 <= 1);
}

TEST_CASE("pitch geometry: five-row formations on both sides fit") {
  MatchDetail d = empty_detail(MatchState::Live);
  d.home = lineup("4-1-4-1", {4, 1, 4, 1});
  d.away = lineup("3-4-2-1", {3, 4, 2, 1});
  check_layout(d);
}

TEST_CASE("name card: a long name stays inside the pitch near either edge") {
  MatchDetail d = full_detail();
  const std::string longName = "Aleksandar Mitrovic-Vanderbilt";  // 30 characters
  REQUIRE(longName.size() == 30);
  d.home.players[1].name = longName;  // left back
  d.home.players[1].x = 0.0f;
  d.home.players[4].name = longName + " Jr";  // right back
  d.home.players[4].x = 1.0f;
  OverlayScreen s(d);
  const DetailParts* p = detail_parts(s.screen);
  lv_area_t pitch;
  lv_obj_get_coords(p->pitch, &pitch);
  for (const std::string& name : {longName, longName + " Jr"}) {
    lv_obj_t* m = marker_named(p->pitch, name);
    REQUIRE(m != nullptr);
    lv_obj_send_event(m, LV_EVENT_CLICKED, nullptr);
    test_advance(400);
    lv_obj_update_layout(lv_screen_active());
    REQUIRE(visible(p->nameCard));
    lv_area_t c;
    lv_obj_get_coords(p->nameCard, &c);
    CHECK(c.x1 - pitch.x1 >= 0);
    CHECK(c.x2 - pitch.x1 + 1 <= lv_area_get_width(&pitch));
    CHECK(lv_label_get_long_mode(p->nameCardName) == LV_LABEL_LONG_DOT);
    lv_obj_send_event(p->pitch, LV_EVENT_CLICKED, nullptr);
  }
}

// ---------------------------------------------------------------- summary

TEST_CASE("summary: events in minute order with added time; ties keep input order") {
  MatchDetail d = empty_detail(MatchState::Live);
  d.events = {
      {"58'", EventType::Goal, Side::Home, "Kai Havertz", "Assist: Declan Rice"},
      {"12'", EventType::Goal, Side::Home, "Bukayo Saka", "Assist: Martin Odegaard"},
      {"45'+2'", EventType::Yellow, Side::Away, "Moises Caicedo", ""},
      {"34'", EventType::Goal, Side::Away, "Cole Palmer", ""},
      {"46'", EventType::Sub, Side::Away, "Pedro Neto", "For: Noni Madueke"},
      {"46'", EventType::Red, Side::Away, "Wesley Fofana", ""},
  };
  Screen s(d);
  const DetailParts* p = s.parts();
  REQUIRE(p->eventPlayers.size() == 6);
  REQUIRE(p->eventMinutes.size() == 6);
  const char* const names[] = {"Bukayo Saka",  "Cole Palmer",  "Moises Caicedo",
                               "Pedro Neto",   "Wesley Fofana", "Kai Havertz"};
  const char* const minutes[] = {"12'", "34'", "45'+2'", "46'", "46'", "58'"};
  for (size_t i = 0; i < 6; ++i) {
    CHECK(text(p->eventPlayers[i]) == names[i]);
    CHECK(text(p->eventMinutes[i]) == minutes[i]);
  }
  CHECK_FALSE(visible(p->summaryEmpty));
}

TEST_CASE("summary: no events shows a message") {
  Screen s(empty_detail());
  const DetailParts* p = s.parts();
  CHECK(p->eventPlayers.empty());
  REQUIRE(p->summaryEmpty != nullptr);
  CHECK(text(p->summaryEmpty) == "No events yet");
}

// ------------------------------------------------------------------ stats

TEST_CASE("stats: one row per stat, or a message when there are none") {
  {
    MatchDetail d = full_detail();
    REQUIRE(d.stats.size() == 5);
    Screen s(d);
    CHECK(s.parts()->statRows.size() == 5);
    CHECK_FALSE(visible(s.parts()->statsEmpty));
  }
  {
    Screen s(empty_detail());
    CHECK(s.parts()->statRows.empty());
    CHECK(text(s.parts()->statsEmpty) == "Stats are not available for this match");
  }
}

// ----------------------------------------------------------------- header

TEST_CASE("header: live, post, pre (local kick-off time) and postponed") {
  {
    Screen s(empty_detail(MatchState::Live));
    const DetailParts* p = s.parts();
    CHECK(text(p->title) == "ARS 2:1 CHE");
    CHECK(text(p->status) == "67'");
    CHECK(visible(p->liveDot));
  }
  {
    Screen s(empty_detail(MatchState::Post));
    const DetailParts* p = s.parts();
    CHECK(text(p->title) == "ARS 2:1 CHE");
    CHECK(text(p->status) == "FT");
    CHECK_FALSE(visible(p->liveDot));
  }
  {
    DetailContext ctx;
    ctx.tzOffset = kIndia;
    Screen s(empty_detail(MatchState::Pre), ctx);
    const DetailParts* p = s.parts();
    CHECK(text(p->title) == "ARS v CHE");
    CHECK(text(p->status) == "Wed 01:30");
    CHECK_FALSE(visible(p->liveDot));
  }
  {
    Screen s(empty_detail(MatchState::Postponed));
    const DetailParts* p = s.parts();
    CHECK(text(p->title) == "ARS v CHE");
    CHECK(text(p->status) == "Postponed");
  }
}

TEST_CASE("the Back control calls onBack") {
  Screen s(full_detail());
  REQUIRE(s.parts()->back != nullptr);
  lv_obj_send_event(s.parts()->back, LV_EVENT_CLICKED, nullptr);
  CHECK(s.backs == 1);
}

TEST_CASE("sub-tabs: default is Pitch for a live match with lineups, else Summary") {
  {
    Screen s(full_detail());
    CHECK(detail_active_tab(s.screen) == DetailTab::Pitch);
  }
  {
    MatchDetail d = full_detail();
    d.match.state = MatchState::Post;
    Screen s(d);
    CHECK(detail_active_tab(s.screen) == DetailTab::Summary);
  }
  {
    Screen s(empty_detail(MatchState::Live));
    CHECK(detail_active_tab(s.screen) == DetailTab::Summary);
  }
}

TEST_CASE("sub-tabs: tapping a label selects it") {
  Screen s(full_detail());
  const DetailParts* p = s.parts();
  lv_obj_send_event(p->tabLabels[2], LV_EVENT_CLICKED, nullptr);
  s.settle();
  CHECK(detail_active_tab(s.screen) == DetailTab::Stats);
  lv_obj_send_event(p->tabLabels[0], LV_EVENT_CLICKED, nullptr);
  s.settle();
  CHECK(detail_active_tab(s.screen) == DetailTab::Summary);
}

TEST_CASE("detail_update: unchanged data does nothing; new events rebuild the summary") {
  MatchDetail d = full_detail();
  Screen s(d);
  const uint32_t before = count_descendants(s.screen);
  const size_t requests = s.images.requested.size();
  for (int i = 0; i < 5; ++i) CHECK_FALSE(detail_update(s.screen, d, {}));
  CHECK(count_descendants(s.screen) == before);
  CHECK(s.images.requested.size() == requests);

  d.match.clock = "68'";
  CHECK(detail_update(s.screen, d, {}));
  CHECK(text(s.parts()->status) == "68'");
  CHECK(count_descendants(s.screen) == before);
  CHECK(s.images.requested.size() == requests);

  const size_t events = s.parts()->eventPlayers.size();
  d.events.push_back({"70'", EventType::Goal, Side::Away, "Cole Palmer", ""});
  CHECK(detail_update(s.screen, d, {}));
  CHECK(s.parts()->eventPlayers.size() == events + 1);
  CHECK(s.images.requested.size() == requests);  // the pitch was not rebuilt
}

// -------------------------------------------------------------------- app

namespace {

struct AppHost {
  Store store;
  CountingImages images;
  DeferredSource source;
  std::vector<std::string> opened;
  MemoryPrefs prefs;
  AppDeps deps{store, images, source, [this](const std::string& id) { opened.push_back(id); }, prefs};
  lv_obj_t* app = nullptr;

  AppHost() {
    init_test_display();
    store.set_favourite("359");
    app = app_create(lv_screen_active(), deps);
    settle();
  }
  ~AppHost() { deinit_test_display(); }

  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  uint32_t top_level() const { return lv_obj_get_child_count(lv_screen_active()); }
};

MatchDetail app_detail() {
  MatchDetail d = full_detail();
  return d;
}

}  // namespace

TEST_CASE("app: opening a cached match shows the detail at once and fetches once") {
  AppHost h;
  h.store.set_detail(app_detail());
  const uint32_t before = h.top_level();
  app_open_match(h.app, demo::kLiveMatchId);
  h.settle();
  CHECK(h.top_level() == before + 1);
  lv_obj_t* view = app_detail_view(h.app);
  REQUIRE(view != nullptr);
  const DetailParts* p = detail_parts(view);
  REQUIRE(p != nullptr);
  REQUIRE(p->tabview != nullptr);
  CHECK(text(p->title) == "ARS 2:1 CHE");
  CHECK(h.source.calls.size() == 1);
  CHECK(h.source.calls[0] == demo::kLiveMatchId);

  // The overlay covers the area above the tab bar, which stays visible.
  lv_obj_t* overlay = lv_obj_get_child(lv_screen_active(), -1);
  CHECK(lv_obj_get_height(overlay) == 320 - 44);
  CHECK(lv_obj_get_y(overlay) == 0);

  h.source.complete(true, app_detail());
  h.settle();
  CHECK(app_detail_view(h.app) != nullptr);
  CHECK(h.source.calls.size() == 1);

  app_close_detail(h.app);
  h.settle();
  CHECK(h.top_level() == before);
  CHECK(app_detail_view(h.app) == nullptr);
}

TEST_CASE("app: a skeleton while loading, replaced by the detail when it arrives") {
  AppHost h;
  app_open_match(h.app, demo::kLiveMatchId);
  h.settle();
  lv_obj_t* view = app_detail_view(h.app);
  REQUIRE(view != nullptr);
  const DetailParts* p = detail_parts(view);
  REQUIRE(p != nullptr);
  CHECK(p->tabview == nullptr);
  REQUIRE(p->message != nullptr);
  CHECK(text(p->message) == "Loading match...");
  CHECK_FALSE(visible(p->retry));
  CHECK(visible(p->back));

  h.source.complete(true, app_detail());
  h.settle();
  view = app_detail_view(h.app);
  REQUIRE(view != nullptr);
  REQUIRE(detail_parts(view)->tabview != nullptr);
  CHECK(h.store.detail_for(demo::kLiveMatchId) != nullptr);
}

TEST_CASE("app: a failed fetch shows an error with Try again") {
  AppHost h;
  app_open_match(h.app, "m-missing");
  h.source.complete(false);
  h.settle();
  const DetailParts* p = detail_parts(app_detail_view(h.app));
  REQUIRE(p != nullptr);
  CHECK(text(p->message) == "Could not load this match. Check the connection.");
  REQUIRE(visible(p->retry));

  lv_obj_send_event(p->retry, LV_EVENT_CLICKED, nullptr);
  h.settle();
  CHECK(h.source.calls.size() == 2);
  p = detail_parts(app_detail_view(h.app));
  CHECK(text(p->message) == "Loading match...");

  // Back from the error state closes the overlay.
  const uint32_t withOverlay = h.top_level();
  lv_obj_send_event(p->back, LV_EVENT_CLICKED, nullptr);
  h.settle();
  CHECK(h.top_level() == withOverlay - 1);
  CHECK(app_detail_view(h.app) == nullptr);
}

TEST_CASE("app: a late result for a closed overlay only fills the store") {
  AppHost h;
  const uint32_t before = h.top_level();
  app_open_match(h.app, demo::kLiveMatchId);
  app_close_detail(h.app);
  h.source.complete(true, app_detail());
  h.settle();
  CHECK(h.top_level() == before);
  CHECK(h.store.detail_for(demo::kLiveMatchId) != nullptr);
}

TEST_CASE("app: app_refresh rebuilds nothing when the data is unchanged") {
  AppHost h;
  h.store.set_matches(demo_matches(kKickoff), kKickoff);
  h.store.set_detail(app_detail());
  app_refresh(h.app, kKickoff);
  app_open_match(h.app, demo::kLiveMatchId);
  h.source.complete(true, app_detail());
  app_refresh(h.app, kKickoff);
  h.settle();
  const uint32_t before = count_descendants(lv_screen_active());
  const size_t requests = h.images.requested.size();
  for (int i = 1; i <= 5; ++i) {
    app_refresh(h.app, kKickoff + i);
    h.settle();
  }
  CHECK(count_descendants(lv_screen_active()) == before);
  CHECK(h.images.requested.size() == requests);

  // A published change in the store reaches the open detail.
  MatchDetail more = app_detail();
  more.events.push_back({"70'", EventType::Yellow, Side::Home, "Declan Rice", ""});
  h.store.set_detail(more);
  app_refresh(h.app, kKickoff + 10);
  h.settle();
  CHECK(detail_parts(app_detail_view(h.app))->eventPlayers.size() == more.events.size());
}

TEST_CASE("app: lineups published while the detail is open appear on refresh") {
  AppHost h;
  MatchDetail bare = app_detail();
  bare.home.players.clear();
  bare.away.players.clear();
  h.store.set_detail(bare);
  app_open_match(h.app, demo::kLiveMatchId);
  h.source.complete(true, bare);
  h.settle();
  CHECK(markers(detail_parts(app_detail_view(h.app))->pitch).empty());

  h.store.set_detail(app_detail());
  app_refresh(h.app, kKickoff);
  h.settle();
  const DetailParts* p = detail_parts(app_detail_view(h.app));
  CHECK(markers(p->pitch).size() == 22);
  CHECK(visible(p->pitch));
}

TEST_CASE("app: the header follows the match list (fresher clock and score)") {
  AppHost h;
  std::vector<MatchSummary> ms = demo_matches(kKickoff);
  ms[0].clock = "71'";
  ms[0].homeScore = 3;
  h.store.set_matches(ms, kKickoff);
  h.store.set_detail(app_detail());
  app_open_match(h.app, demo::kLiveMatchId);
  h.settle();
  const DetailParts* p = detail_parts(app_detail_view(h.app));
  CHECK(text(p->status) == "71'");
  CHECK(text(p->title) == "ARS 3:1 CHE");
}

TEST_CASE("app: tapping the live score opens the detail overlay") {
  AppHost h;
  h.store.set_matches(demo_matches(kKickoff), kKickoff);
  app_refresh(h.app, kKickoff);
  h.settle();
  lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(h.app), 0);
  const LiveParts* live = live_parts(lv_obj_get_child(tab, 0));
  REQUIRE(live != nullptr);
  lv_obj_send_event(live->scoreArea, LV_EVENT_CLICKED, nullptr);
  h.settle();
  CHECK(app_detail_view(h.app) != nullptr);
  REQUIRE(h.source.calls.size() == 1);
  CHECK(h.source.calls[0] == demo::kLiveMatchId);
  REQUIRE(h.opened.size() == 1);  // the optional host callback is still told
  CHECK(h.opened[0] == demo::kLiveMatchId);
}

TEST_CASE("demo data: the live detail has lineups, every event type and stats") {
  const MatchDetail d = demo_detail(demo::kLiveMatchId);
  CHECK(d.home.formation == "4-3-3");
  CHECK(d.away.formation == "4-2-3-1");
  bool seen[4] = {false, false, false, false};
  for (const MatchEvent& e : d.events) seen[static_cast<int>(e.type)] = true;
  CHECK(seen[static_cast<int>(EventType::Goal)]);
  CHECK(seen[static_cast<int>(EventType::Yellow)]);
  CHECK(seen[static_cast<int>(EventType::Sub)]);
  REQUIRE(d.stats.size() == 5);
  CHECK(d.stats[0].label == "Possession");
  CHECK(d.stats[0].home == "58%");
  CHECK(d.stats[0].away == "42%");
  int photos = 0;
  for (const Lineup* l : {&d.home, &d.away}) {
    for (const LineupPlayer& p : l->players) {
      CHECK(p.x >= 0.0f);
      CHECK(p.x <= 1.0f);
      CHECK(p.y >= 0.0f);
      CHECK(p.y <= 1.0f);
      if (!p.photo.empty()) ++photos;
    }
  }
  CHECK(photos == 4);

  const MatchDetail pre = demo_detail(demo::kPreMatchId);
  CHECK(pre.match.id == demo::kPreMatchId);
  CHECK(pre.match.state == MatchState::Pre);
  CHECK(pre.match.home.abbr == "RMA");
  CHECK(pre.home.players.empty());
  CHECK(pre.events.empty());
  CHECK(pre.stats.empty());
}
