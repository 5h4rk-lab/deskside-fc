#include <doctest/doctest.h>

#include <math.h>
#include <cstdlib>

#include <string>
#include <vector>

#include "demo_data.h"
#include "fixture_source.h"
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

Team team(const char* id, const char* abbr, const char* name, uint32_t color,
          uint32_t primary = 0) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  t.color = color;
  t.primary = primary != 0 ? primary : color;
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

TEST_CASE("split_colors with real primaries (Chelsea blue v Arsenal red) keeps both") {
  auto c = split_colors(lv_color_hex(0x034694), lv_color_hex(0xEF0107));
  CHECK(u32(c.first) == u32(lv_color_hex(0x034694)));
  CHECK(u32(c.second) == u32(lv_color_hex(0xEF0107)));
  // Spurs navy v Chelsea blue clash: the clash handling applies to primaries.
  auto clash = split_colors(lv_color_hex(0x132257), lv_color_hex(0x034694));
  CHECK(u32(clash.first) == u32(lv_color_hex(0x132257)));
  CHECK(u32(clash.second) != u32(lv_color_hex(0x034694)));
}

TEST_CASE("live match: the split uses each team's primary, not its accent (Chelsea blue)") {
  Fixture f;
  f.store.set_favourite("363");
  MatchSummary m = live_match();
  // Chelsea at home: gold accent, royal blue brand. Arsenal away.
  m.home = team("363", "CHE", "Chelsea", 0xDBA111, 0x034694);
  m.away = team("359", "ARS", "Arsenal", 0xEF0107, 0xEF0107);
  f.store.set_matches({m}, kNow);
  f.refresh();
  const lv_color_t left = pixel_at(f.live, 6, 150);
  const lv_color_t right = pixel_at(f.live, 234, 150);
  CHECK(left.blue > left.red + 30);    // blue dominates ...
  CHECK(left.blue > left.green + 10);
  CHECK(left.red < left.blue);         // ... and it is not gold
  CHECK(right.red > right.blue + 40);  // Arsenal red on the right
}

TEST_CASE("live match: a very dark primary (Spurs navy) still gives a visible split") {
  Fixture f;
  f.store.set_favourite("367");
  MatchSummary m = live_match();
  m.home = team("367", "TOT", "Tottenham Hotspur", 0xFFFFFF, 0x132257);
  m.away = team("359", "ARS", "Arsenal", 0xEF0107, 0xEF0107);
  f.store.set_matches({m}, kNow);
  f.refresh();
  const lv_color_t left = pixel_at(f.live, 6, 150);
  const lv_color_t turf = theme().turf;
  // Navy, clearly not the white accent and clearly not the plain turf.
  CHECK(left.blue > left.red + 15);
  CHECK(left.red < 100);
  CHECK(left.blue > turf.blue + 25);
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
  REQUIRE(ms.size() == 11);
  for (const MatchSummary& m : ms) CHECK_FALSE(m.league.empty());
  CHECK(ms[4].state == MatchState::Post);  // yesterday
  CHECK(ms[7].state == MatchState::Postponed);
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

// ------------------------------------------------------------ last result

namespace {

MatchSummary past_match(const char* id, Team home, Team away, int hs, int as, int64_t kickoff) {
  MatchSummary m;
  m.id = id;
  m.league = "eng.1";
  m.state = MatchState::Post;
  m.clock = "FT";
  m.kickoff = kickoff;
  m.home = std::move(home);
  m.away = std::move(away);
  m.homeScore = hs;
  m.awayScore = as;
  return m;
}

Team ars() { return team("359", "ARS", "Arsenal", 0xEF0107); }
// Chelsea as the helper sends it: gold accent, royal blue primary.
Team che() { return team("363", "CHE", "Chelsea", 0xDBA111, 0x034694); }

}  // namespace

TEST_CASE("last result: only a past match shows the last-result card (home win)") {
  Fixture f;
  f.store.set_matches({past_match("m-past", ars(), che(), 3, 1, kNow - 26 * 3600)}, kNow);
  f.refresh();
  const LiveParts* p = f.parts();
  CHECK(visible(p->lastCaption));
  CHECK(text(p->lastCaption) == "Last result");
  CHECK(score_text(p) == "3:1");
  CHECK(visible(p->score));
  CHECK(text(p->lastOpponent) == "vs CHE");
  CHECK(text(p->lastFt) == "FT");
  CHECK(visible(p->lastFt));
  CHECK(text(p->lastWhen) == "yesterday");
  CHECK(visible(p->lastWhen));
  CHECK_FALSE(visible(p->message));
  CHECK_FALSE(visible(p->pill));
  CHECK_FALSE(visible(p->homeName));
  CHECK(visible(p->homeCrest));
  CHECK(visible(p->awayCrest));
  CHECK(lv_obj_get_style_text_font(p->lastCaption, 0) == &hanken_10);
  CHECK(u32(lv_obj_get_style_text_color(p->lastCaption, 0)) == u32(theme().dim));
  // Tapping opens the match detail.
  REQUIRE(visible(p->scoreArea));
  lv_obj_send_event(p->scoreArea, LV_EVENT_CLICKED, nullptr);
  REQUIRE(f.opened.size() == 1);
  CHECK(f.opened[0] == "m-past");
}

// The last-result card reads favourite first: its crest and colour on the
// left, its goals first, the opponent on the right. `fresh` is a fixture
// refreshed once, so its first two image requests are the left then the
// right crest.
void check_favourite_first(Fixture& f, const char* score, const char* id) {
  const LiveParts* p = f.parts();
  REQUIRE(f.images.requested.size() >= 2);
  CHECK(f.images.requested[0].find("x%2F359.png") != std::string::npos);  // ARS left
  CHECK(f.images.requested[1].find("x%2F363.png") != std::string::npos);  // CHE right
  CHECK(score_text(p) == score);
  CHECK(text(p->lastOpponent) == "vs CHE");
  // Split: Arsenal red on the left, Chelsea blue on the right.
  const lv_color_t left = pixel_at(f.live, 6, 150);
  const lv_color_t right = pixel_at(f.live, 234, 150);
  CHECK(left.red > left.blue + 40);
  CHECK(right.blue > right.red + 30);
  lv_obj_send_event(p->scoreArea, LV_EVENT_CLICKED, nullptr);
  REQUIRE(f.opened.size() == 1);
  CHECK(f.opened[0] == id);
}

TEST_CASE("last result: a home win reads favourite first") {
  Fixture f;
  f.store.set_matches({past_match("m-hw", ars(), che(), 3, 1, kNow - 26 * 3600)}, kNow);
  f.refresh();
  check_favourite_first(f, "3:1", "m-hw");
}

TEST_CASE("last result: an away loss puts the favourite on the left (CHE 2-0 ARS reads 0:2)") {
  Fixture f;
  f.store.set_matches({past_match("m-al", che(), ars(), 2, 0, kNow - 3 * 86400 - 600)}, kNow);
  f.refresh();
  check_favourite_first(f, "0:2", "m-al");
  CHECK(text(f.parts()->lastWhen) == "3 days ago");
}

TEST_CASE("last result: an away win puts the favourite on the left (CHE 1-3 ARS reads 3:1)") {
  Fixture f;
  f.store.set_matches({past_match("m-aw", che(), ars(), 1, 3, kNow - 2 * 3600)}, kNow);
  f.refresh();
  check_favourite_first(f, "3:1", "m-aw");
}

TEST_CASE("last result: a draw away from home") {
  Fixture f;
  f.store.set_matches({past_match("m-d", che(), ars(), 1, 1, kNow - 5 * 3600)}, kNow);
  f.refresh();
  check_favourite_first(f, "1:1", "m-d");
  CHECK(text(f.parts()->lastWhen) == "5h ago");
}

TEST_CASE("last result: a live match still wins, and so does the next fixture") {
  Fixture f;
  const LiveParts* p = f.parts();
  const MatchSummary past = past_match("m-past", ars(), che(), 3, 1, kNow - 26 * 3600);
  f.store.set_matches({past, live_match()}, kNow);
  f.refresh();
  CHECK(visible(p->pill));
  CHECK(score_text(p) == "2:1");
  CHECK_FALSE(visible(p->lastCaption));
  CHECK_FALSE(visible(p->lastOpponent));
  CHECK_FALSE(visible(p->lastWhen));

  MatchSummary next = live_match();
  next.id = "m-next";
  next.state = MatchState::Pre;
  next.clock = "";
  next.kickoff = 1792526400;
  next.homeScore.reset();
  next.awayScore.reset();
  f.store.set_matches({past, next}, kNow);
  f.refresh();
  CHECK(text(p->message) == "Next: CHE Tue 20:00");
  CHECK(visible(p->message));
  CHECK_FALSE(visible(p->lastCaption));
  CHECK_FALSE(visible(p->score));
  CHECK_FALSE(visible(p->lastWhen));

  // Back to only the past match, then to nothing of the favourite's.
  f.store.set_matches({past}, kNow);
  f.refresh();
  CHECK(visible(p->lastCaption));
  CHECK_FALSE(visible(p->message));
  MatchSummary other = past;
  other.home = team("1", "AAA", "Other", 0x111111);
  other.away = team("2", "BBB", "Else", 0x222222);
  f.store.set_matches({other}, kNow);
  f.refresh();
  CHECK(text(p->message) == "No matches soon");
  CHECK(visible(p->message));
  CHECK_FALSE(visible(p->lastCaption));
  CHECK_FALSE(visible(p->score));
}

TEST_CASE("demo data carries brand primaries next to the accents") {
  const std::vector<MatchSummary> ms = demo_matches(kNow);
  const MatchSummary* live = nullptr;
  for (const MatchSummary& m : ms) {
    if (m.id == demo::kLiveMatchId) live = &m;
  }
  REQUIRE(live != nullptr);
  CHECK(live->home.primary == 0xEF0107u);  // Arsenal
  CHECK(live->away.abbr == "CHE");
  CHECK(live->away.color == 0xDBA111u);    // gold accent
  CHECK(live->away.primary == 0x034694u);  // royal blue brand
  bool sawSpurs = false;
  for (const MatchSummary& m : ms) {
    for (const Team* t : {&m.home, &m.away}) {
      if (t->abbr == "TOT") {
        sawSpurs = true;
        CHECK(t->primary == 0x132257u);
      }
    }
  }
  CHECK(sawSpurs);
  for (const Team& t : demo_teams()) {
    INFO(t.abbr);
    CHECK(t.primary != 0xFFFFFFu);  // white kits use their dark identity colour
    if (t.abbr == "CHE") CHECK(t.primary == 0x034694u);
    if (t.abbr == "RMA") CHECK(t.primary == 0x00529Fu);
    if (t.abbr == "JUV") CHECK(t.primary == 0x000000u);
  }
}

// ---------------------------------------------------------------- similar primaries

namespace {

int rgb_distance(lv_color_t a, lv_color_t b) {
  const int dr = a.red - b.red, dg = a.green - b.green, db = a.blue - b.blue;
  return static_cast<int>(sqrtf(static_cast<float>(dr * dr + dg * dg + db * db)) + 0.5f);
}

struct Pair {
  const char* name;
  uint32_t home;
  uint32_t away;
};

// Pairings whose real primaries are close: blues against Chelsea, black
// against black, and clubs whose white kit was replaced by a dark colour.
const Pair kSimilar[] = {
    {"CHE v EVE", 0x034694, 0x003399}, {"CHE v CRY", 0x034694, 0x1B458F},
    {"CHE v BHA", 0x034694, 0x0057B8}, {"CHE v TOT", 0x034694, 0x132257},
    {"EVE v CHE", 0x003399, 0x034694}, {"CRY v CHE", 0x1B458F, 0x034694},
    {"TOT v CHE", 0x132257, 0x034694}, {"FUL v JUV", 0x000000, 0x000000},
    {"FUL v NEW", 0x000000, 0x241F20}, {"TOT v LEE", 0x132257, 0x1D428A},
    {"RMA v LEE", 0x00529F, 0x1D428A}, {"LEE v TOT", 0x1D428A, 0x132257},
    {"JUV v UDI", 0x000000, 0x000000},
};

// Rendered halves are mixed into the turf (opacity .9) and sit under the
// vignette, which roughly halves colour differences at the sample points, so
// the colour rule (>= kSplitMinDistance = 80) cannot hold pixel for pixel.
// Measured at (6,150)/(234,150): 38 (LEE v TOT, colours 86 apart) to 77
// (TOT v CHE); CHE v EVE/CRY/BHA/TOT 39 (colours 89). Black v black (FUL v
// JUV, JUV v UDI) cannot be separated by shade: split_colors falls back to the
// opponent blue for the away half (colours 109, pixels 49).
constexpr int kRenderedMinDistance = 35;

}  // namespace

TEST_CASE("live_background_colors tones the primaries, then separates them") {
  // Distinct primaries are only toned.
  auto cheArs = live_background_colors(0x034694, 0xEF0107);
  CHECK(u32(cheArs.first) == u32(lv_color_hex(0x034694)));
  CHECK(u32(cheArs.second) == u32(lv_color_hex(0xEF0107)));
  auto totArs = live_background_colors(0x132257, 0xEF0107);
  CHECK(u32(totArs.first) == u32(background_tone(lv_color_hex(0x132257))));

  for (const Pair& p : kSimilar) {
    INFO(p.name);
    auto c = live_background_colors(p.home, p.away);
    // Home is always its own toned primary.
    CHECK(u32(c.first) == u32(background_tone(lv_color_hex(p.home))));
    CHECK(rgb_distance(c.first, c.second) >= kSplitMinDistance);
  }
}

TEST_CASE("live match: similar primaries still give two distinct halves") {
  for (const Pair& p : kSimilar) {
    INFO(p.name);
    Fixture f;
    MatchSummary m = live_match();
    m.home = team("359", "HOM", "Home", 0xEF0107, p.home);
    m.away = team("363", "AWY", "Away", 0x6CABDD, p.away);
    m.home.primary = p.home;  // black is a real primary, not "unset"
    m.away.primary = p.away;
    f.store.set_matches({m}, kNow);
    f.refresh();
    const lv_color_t left = pixel_at(f.live, 6, 150);
    const lv_color_t right = pixel_at(f.live, 234, 150);
    CHECK(rgb_distance(left, right) >= kRenderedMinDistance);
  }
}

// ---------------------------------------------------------------- crest halo

TEST_CASE("crest_needs_halo: dark halves only") {
  CHECK(crest_needs_halo(background_tone(lv_color_hex(0x132257))));  // Spurs navy
  CHECK(crest_needs_halo(background_tone(lv_color_hex(0x000000))));  // black
  CHECK(crest_needs_halo(background_tone(lv_color_hex(0x241F20))));  // Newcastle
  CHECK_FALSE(crest_needs_halo(lv_color_hex(0xEF0107)));             // Arsenal red
  CHECK_FALSE(crest_needs_halo(lv_color_hex(0x6CABDD)));             // City sky blue
  CHECK_FALSE(crest_needs_halo(lv_color_hex(0xFDE100)));             // Dortmund yellow
}

namespace {

lv_point_t centre(lv_obj_t* obj) {
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

int32_t index_of(lv_obj_t* obj) { return lv_obj_get_index(obj); }

}  // namespace

TEST_CASE("live match: a dark half gets a light halo behind its crest, a light half none") {
  Fixture f;
  f.store.set_favourite("367");
  MatchSummary m = live_match();
  m.home = team("367", "TOT", "Tottenham Hotspur", 0xFFFFFF, 0x132257);
  m.away = team("359", "ARS", "Arsenal", 0xEF0107, 0xEF0107);
  f.store.set_matches({m}, kNow);
  f.refresh();
  const LiveParts* p = f.parts();
  REQUIRE(p->homeHalo != nullptr);
  REQUIRE(p->awayHalo != nullptr);
  CHECK(visible(p->homeHalo));
  CHECK_FALSE(visible(p->awayHalo));
  // Centred on the crest and drawn behind it.
  const lv_point_t h = centre(p->homeHalo);
  const lv_point_t c = centre(p->homeCrest);
  CHECK(std::abs(h.x - c.x) <= 1);
  CHECK(std::abs(h.y - c.y) <= 1);
  CHECK(lv_obj_get_parent(p->homeHalo) == lv_obj_get_parent(p->homeCrest));
  CHECK(index_of(p->homeHalo) < index_of(p->homeCrest));
  CHECK(lv_obj_get_width(p->homeHalo) >= 100);

  // The lit disc shows: just outside the crest box the pixel is lighter with
  // the halo than without it, and it stays a subtle tint (still navy-dark).
  const lv_area_t box = [&] {
    lv_area_t a;
    lv_obj_get_coords(p->homeCrest, &a);
    return a;
  }();
  const int32_t sx = c.x;
  const int32_t sy = box.y2 + 8;
  const lv_color_t lit = pixel_at(f.live, sx, sy);
  lv_obj_add_flag(p->homeHalo, LV_OBJ_FLAG_HIDDEN);
  const lv_color_t plain = pixel_at(f.live, sx, sy);
  lv_obj_remove_flag(p->homeHalo, LV_OBJ_FLAG_HIDDEN);
  const int litSum = lit.red + lit.green + lit.blue;
  const int plainSum = plain.red + plain.green + plain.blue;
  CHECK(litSum >= plainSum + 15);
  CHECK(relative_luminance(lit) < 0.2f);

  // The match changes: Arsenal at home, Spurs away: the halos follow.
  m.home = team("359", "ARS", "Arsenal", 0xEF0107, 0xEF0107);
  m.away = team("367", "TOT", "Tottenham Hotspur", 0xFFFFFF, 0x132257);
  f.store.set_matches({m}, kNow);
  f.refresh();
  CHECK_FALSE(visible(p->homeHalo));
  CHECK(visible(p->awayHalo));
  const lv_point_t ah = centre(p->awayHalo);
  const lv_point_t ac = centre(p->awayCrest);
  CHECK(std::abs(ah.x - ac.x) <= 1);
  CHECK(std::abs(ah.y - ac.y) <= 1);
  CHECK(index_of(p->awayHalo) < index_of(p->awayCrest));

  // No split (no matches): no halo.
  f.store.set_matches({}, kNow);
  f.refresh();
  CHECK_FALSE(visible(p->homeHalo));
  CHECK_FALSE(visible(p->awayHalo));
}

TEST_CASE("live match: two light halves have no halo") {
  Fixture f;
  f.store.set_matches({live_match()}, kNow);  // Arsenal red v Chelsea blue
  f.refresh();
  CHECK_FALSE(visible(f.parts()->homeHalo));
  CHECK_FALSE(visible(f.parts()->awayHalo));
}
