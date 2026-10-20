#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "demo_data.h"
#include "fonts.h"
#include "screens/live.h"
#include "snapshot.h"
#include "store.h"
#include "test_display.h"
#include "theme.h"

namespace {

// Never delivers an image: crests keep their placeholder.
class NullImages : public ImageProvider {
 public:
  void request(const std::string& path, std::function<void(ImagePtr)> done) override {
    requested.push_back(path);
    done(nullptr);
  }
  std::vector<std::string> requested;
};

constexpr int64_t kNow = 1792526400 - 3 * 3600;  // Tue 2026-10-20 17:00 UTC

Team team(const char* id, const char* abbr, const char* name, uint32_t color) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  t.color = color;
  t.crest = std::string("/api/img?u=x%2F") + id + ".png&w=48&h=48";
  return t;
}

MatchSummary live_match(int home = 2, int away = 1) {
  MatchSummary m;
  m.id = "m-live";
  m.league = "eng.1";
  m.state = MatchState::Live;
  m.clock = "67'";
  m.kickoff = kNow - 67 * 60;
  m.home = team("359", "ARS", "Arsenal", 0xEF0107);
  m.away = team("363", "CHE", "Chelsea", 0x034694);
  m.homeScore = home;
  m.awayScore = away;
  return m;
}

uint32_t u32(lv_color_t c) { return lv_color_to_u32(c); }

std::string text(lv_obj_t* label) { return lv_label_get_text(label); }

std::string score_text(const LiveParts* p) {
  return text(p->scoreHome) + text(p->scoreSep) + text(p->scoreAway);
}

bool visible(lv_obj_t* obj) {
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

// LVGL display, a store with the favourite set, and a live screen on top.
struct Fixture {
  Store store;
  NullImages images;
  std::vector<std::string> opened;
  lv_obj_t* live = nullptr;

  Fixture() {
    init_test_display();
    store.set_favourite("359");
    live = live_create(lv_screen_active(), store, images,
                       [this](std::string id) { opened.push_back(std::move(id)); });
  }
  ~Fixture() { deinit_test_display(); }

  const LiveParts* parts() const { return live_parts(live); }

  void refresh(int64_t now = kNow) {
    live_refresh(live, now);
    test_advance(20);
    lv_obj_update_layout(live);
  }
};

}  // namespace

TEST_CASE("split_colors keeps distinct colours and separates clashing ones") {
  const lv_color_t white = lv_color_hex(0xFFFFFF);
  auto same = split_colors(white, white);
  CHECK(u32(same.first) == u32(white));
  CHECK(u32(same.second) == u32(lv_color_hex(0x034694)));

  auto distinct = split_colors(lv_color_hex(0xEF0107), lv_color_hex(0x034694));
  CHECK(u32(distinct.first) == u32(lv_color_hex(0xEF0107)));
  CHECK(u32(distinct.second) == u32(lv_color_hex(0x034694)));

  // Bournemouth v Man United share #DA291C.
  auto reds = split_colors(lv_color_hex(0xDA291C), lv_color_hex(0xDA291C));
  CHECK(u32(reds.second) == u32(lv_color_hex(0x034694)));

  // Home already close to the opponent blue: away falls back to a shade of home.
  const lv_color_t navy = lv_color_hex(0x032F70);
  auto blues = split_colors(navy, navy);
  CHECK(u32(blues.second) != u32(navy));
  CHECK(u32(blues.second) != u32(lv_color_hex(0x034694)));
  auto d = [](lv_color_t a, lv_color_t b) {
    const int dr = a.red - b.red, dg = a.green - b.green, db = a.blue - b.blue;
    return dr * dr + dg * dg + db * db;
  };
  CHECK(d(blues.first, blues.second) >= kSplitMinDistance * kSplitMinDistance);

  // A near-black home still gets a visible away half.
  const lv_color_t black = lv_color_hex(0x050505);
  auto dark = split_colors(black, black);
  CHECK(d(dark.first, dark.second) >= kSplitMinDistance * kSplitMinDistance);
}

TEST_CASE("live match: split background in home and away colours") {
  Fixture f;
  f.store.set_matches({live_match()}, kNow);
  f.refresh();
  const lv_color_t left = pixel_at(f.live, 6, 150);
  const lv_color_t right = pixel_at(f.live, 234, 150);
  CHECK(left.red > left.blue + 40);
  CHECK(right.blue > right.red + 30);
}

TEST_CASE("live match: pill shows the match clock and the score is shown big") {
  Fixture f;
  f.store.set_matches({live_match()}, kNow);
  f.refresh();
  const LiveParts* p = f.parts();
  REQUIRE(p != nullptr);
  CHECK(visible(p->pill));
  CHECK(text(p->pillLabel) == "67'");
  CHECK(score_text(p) == "2:1");
  CHECK(lv_obj_get_style_text_font(p->scoreHome, 0) == &bigshoulders_96);
  CHECK_FALSE(visible(p->message));
  CHECK(text(p->statusLeague) == "Premier League");
}

TEST_CASE("live match: a two-digit score switches to the smaller font and fits") {
  Fixture f;
  f.store.set_matches({live_match(10, 0)}, kNow);
  f.refresh();
  const LiveParts* p = f.parts();
  CHECK(score_text(p) == "10:0");
  CHECK(lv_obj_get_style_text_font(p->scoreHome, 0) == &bigshoulders_56);
  CHECK(lv_obj_get_style_text_font(p->scoreAway, 0) == &bigshoulders_56);
  CHECK(lv_obj_get_width(p->score) <= 200);
}

TEST_CASE("no live match: next fixture, no fixtures, and no data yet") {
  Fixture f;
  const LiveParts* p = f.parts();

  // Before any data: Connecting, never stale.
  f.refresh();
  CHECK(visible(p->message));
  CHECK(text(p->message) == "Connecting...");
  CHECK_FALSE(visible(p->staleDot));
  CHECK_FALSE(visible(p->staleLabel));
  CHECK_FALSE(visible(p->scoreArea));

  // Favourite plays at 20:00 UTC on Tuesday 2026-10-20.
  MatchSummary next = live_match();
  next.id = "m-next";
  next.state = MatchState::Pre;
  next.clock = "";
  next.kickoff = 1792526400;
  next.homeScore.reset();
  next.awayScore.reset();
  MatchSummary other = next;
  other.id = "m-other";
  other.home = team("1", "AAA", "Other", 0x111111);
  other.away = team("2", "BBB", "Else", 0x222222);
  other.kickoff = kNow + 600;
  f.store.set_matches({other, next}, kNow);
  f.refresh();
  CHECK(text(p->message) == "Next: CHE Tue 20:00");
  CHECK_FALSE(visible(p->scoreArea));

  // +05:30 moves it past midnight into Wednesday.
  f.store.set_tz_offset(5 * 3600 + 30 * 60);
  f.refresh();
  CHECK(text(p->message) == "Next: CHE Wed 01:30");
  CHECK(text(p->statusClock) == "22:30");

  // Favourite as the away side: the opponent is the home team.
  f.store.set_tz_offset(0);
  std::swap(next.home, next.away);
  f.store.set_matches({next}, kNow);
  f.refresh();
  CHECK(text(p->message) == "Next: CHE Tue 20:00");

  f.store.set_matches({other}, kNow);
  f.refresh();
  CHECK(text(p->message) == "No matches soon");
}

TEST_CASE("stale data shows an amber marker with the age in minutes") {
  Fixture f;
  const LiveParts* p = f.parts();
  const int64_t t0 = kNow;
  f.store.set_matches({live_match()}, t0);

  f.refresh(t0 + 10);
  CHECK_FALSE(visible(p->staleDot));
  CHECK_FALSE(visible(p->staleLabel));

  f.refresh(t0 + 200);
  CHECK(visible(p->staleDot));
  CHECK(visible(p->staleLabel));
  CHECK(text(p->staleLabel) == "Updated 3 min ago");
  CHECK(u32(lv_obj_get_style_bg_color(p->staleDot, 0)) == u32(theme().amber));

  f.refresh(t0 + 95);  // stale, but under a minute: still at least 1
  CHECK(text(p->staleLabel) == "Updated 1 min ago");

  f.store.set_matches({live_match()}, t0 + 300);  // a fresh success clears it
  f.refresh(t0 + 310);
  CHECK_FALSE(visible(p->staleDot));
}

TEST_CASE("long team names are truncated with dots within 100 px") {
  Fixture f;
  MatchSummary m = live_match();
  m.home.name = "Borussia Monchengladbach";  // 24 characters
  REQUIRE(m.home.name.size() == 24);
  f.store.set_matches({m}, kNow);
  f.refresh();
  const LiveParts* p = f.parts();
  CHECK(lv_label_get_long_mode(p->homeName) == LV_LABEL_LONG_DOT);
  CHECK(lv_obj_get_width(p->homeName) <= 100);
  // The full uppercased name would not fit, so truncation really happens.
  lv_point_t full;
  lv_text_get_size(&full, "BORUSSIA MONCHENGLADBACH", lv_obj_get_style_text_font(p->homeName, 0),
                   0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  CHECK(full.x > 100);
  CHECK(text(p->awayName) == "CHELSEA");
}

TEST_CASE("tapping the score area opens the match") {
  Fixture f;
  f.store.set_matches({live_match()}, kNow);
  f.refresh();
  lv_obj_send_event(f.parts()->scoreArea, LV_EVENT_CLICKED, nullptr);
  REQUIRE(f.opened.size() == 1);
  CHECK(f.opened[0] == "m-live");
}

TEST_CASE("refreshing unchanged data creates no objects; a new score updates the label") {
  Fixture f;
  f.store.set_matches({live_match()}, kNow);
  f.store.set_detail(demo_detail(demo::kLiveMatchId));
  f.refresh();
  const uint32_t before = count_descendants(f.live);
  const size_t requests = f.images.requested.size();
  for (int i = 0; i < 20; ++i) f.refresh(kNow + i);
  CHECK(count_descendants(f.live) == before);
  CHECK(f.images.requested.size() == requests);  // no repeated image requests

  f.store.set_matches({live_match(3, 1)}, kNow + 30);
  f.refresh(kNow + 30);
  CHECK(score_text(f.parts()) == "3:1");
  CHECK(count_descendants(f.live) == before);
}

TEST_CASE("latest event strip shows the last event from the match detail") {
  Fixture f;
  const LiveParts* p = f.parts();
  MatchSummary m = live_match();
  m.id = demo::kLiveMatchId;
  f.store.set_matches({m}, kNow);
  f.refresh();
  CHECK_FALSE(visible(p->eventStrip));

  f.store.set_detail(demo_detail(demo::kLiveMatchId));
  f.refresh();
  CHECK(visible(p->eventStrip));
  CHECK(text(p->eventPlayer) == "Kai Havertz");
  CHECK(text(p->eventCaption) == "58'  Goal");
  bool askedCutout = false;
  for (const std::string& r : f.images.requested) {
    if (r == "/api/cutout?name=Kai%20Havertz&w=40&h=40") askedCutout = true;
  }
  CHECK(askedCutout);
}

TEST_CASE("demo data: live Arsenal v Chelsea with the latest goal by Havertz") {
  const std::vector<MatchSummary> ms = demo_matches(kNow);
  REQUIRE(ms.size() == 5);
  CHECK(ms[0].id == demo::kLiveMatchId);
  CHECK(ms[0].state == MatchState::Live);
  CHECK(ms[0].home.id == "359");
  CHECK(ms[0].away.id == "363");
  CHECK(ms[0].clock == "67'");
  CHECK(ms[0].homeScore == 2);
  CHECK(ms[0].awayScore == 1);
  CHECK(ms[0].home.crest ==
        "/api/img?u=https%3A%2F%2Fa.espncdn.com%2Fi%2Fteamlogos%2Fsoccer%2F500%2F359.png&w=48&h=48");
  const MatchDetail d = demo_detail(demo::kLiveMatchId);
  REQUIRE(d.events.size() == 5);
  CHECK(d.events.back().player == "Kai Havertz");
  CHECK(demo_detail("other").events.empty());
}
