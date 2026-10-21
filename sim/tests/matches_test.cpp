#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "app.h"
#include "prefs.h"
#include "data_source.h"
#include "fonts.h"
#include "format.h"
#include "screens/matches.h"
#include "test_display.h"
#include "theme.h"

namespace {

// Counts requests and never delivers an image: crests keep their placeholder.
class CountingImages : public ImageProvider {
 public:
  void request(const std::string& path, std::function<void(ImagePtr)> done) override {
    requested.push_back(path);
    done(nullptr);
  }
  std::vector<std::string> requested;
};

// Stores the detail callback instead of calling it.
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
  std::vector<std::string> calls;
  std::function<void(bool, MatchDetail)> pending;
};

constexpr int64_t kNow = 1792526400 - 3 * 3600;  // Tue 2026-10-20 17:00 UTC
constexpr int64_t kDay = 86400;
constexpr int64_t kMidnight = kNow - 17 * 3600;  // 00:00 UTC of that Tuesday
constexpr int kIndia = 5 * 3600 + 30 * 60;

Team team(const char* id, const char* abbr, const char* name, uint32_t color = 0x336699) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  t.color = color;
  t.crest = std::string("/api/img?u=x%2F") + id + ".png&w=48&h=48";
  return t;
}

MatchSummary make(const char* id, const char* league, MatchState state, int64_t kickoff, Team home,
                  Team away) {
  MatchSummary m;
  m.id = id;
  m.league = league;
  m.state = state;
  m.kickoff = kickoff;
  m.home = std::move(home);
  m.away = std::move(away);
  return m;
}

MatchSummary live(const char* id, int64_t kickoff, Team home, Team away, int hs, int as,
                  const char* clock = "67'") {
  MatchSummary m = make(id, "eng.1", MatchState::Live, kickoff, std::move(home), std::move(away));
  m.clock = clock;
  m.homeScore = hs;
  m.awayScore = as;
  return m;
}

MatchSummary post(const char* id, int64_t kickoff, Team home, Team away, int hs, int as) {
  MatchSummary m = make(id, "fra.1", MatchState::Post, kickoff, std::move(home), std::move(away));
  m.clock = "FT";
  m.homeScore = hs;
  m.awayScore = as;
  return m;
}

Team ars() { return team("359", "ARS", "Arsenal", 0xEF0107); }
Team che() { return team("363", "CHE", "Chelsea", 0x034694); }
Team rma() { return team("86", "RMA", "Real Madrid"); }
Team bar() { return team("83", "BAR", "Barcelona"); }
Team mnc() { return team("382", "MNC", "Manchester City"); }
Team liv() { return team("364", "LIV", "Liverpool"); }
Team bay() { return team("132", "BAY", "Bayern Munich"); }
Team dor() { return team("124", "DOR", "Borussia Dortmund"); }
Team psg() { return team("160", "PSG", "Paris Saint-Germain"); }
Team mar() { return team("176", "MAR", "Marseille"); }
Team atm() { return team("1068", "ATM", "Atletico Madrid"); }
Team sev() { return team("243", "SEV", "Sevilla"); }
Team bha() { return team("331", "BHA", "Brighton"); }
Team inter() { return team("110", "INT", "Inter Milan"); }
Team milan() { return team("103", "MIL", "AC Milan"); }

// A mixed list, deliberately out of order. Local time equals UTC here.
std::vector<MatchSummary> mixed() {
  std::vector<MatchSummary> v;
  v.push_back(post("m-old", kMidnight - 3 * kDay + 15 * 3600, mar(), psg(), 0, 0));
  v.push_back(make("m-inter", "ita.1", MatchState::Pre, kMidnight + 21 * 3600, inter(), milan()));
  v.push_back(live("m-ars", kNow - 67 * 60, ars(), che(), 2, 1));
  v.push_back(make("m-rma", "esp.1", MatchState::Pre, kMidnight + 19 * 3600, rma(), bar()));
  v.push_back(post("m-psg-late", kMidnight - kDay + 20 * 3600, psg(), mar(), 3, 1));
  v.push_back(make("m-bay", "ger.1", MatchState::Pre, kMidnight + 4 * kDay + 17 * 3600 + 1800, bay(),
                   dor()));
  v.push_back(make("m-mnc", "eng.1", MatchState::Pre, kMidnight + kDay + 16 * 3600 + 1800, mnc(),
                   liv()));
  v.push_back(live("m-atm", kNow - 20 * 60, atm(), sev(), 0, 0, "20'"));
  v.push_back(make("m-ppd", "eng.1", MatchState::Postponed, kMidnight + kDay + 15 * 3600, bha(),
                   liv()));
  v.push_back(post("m-psg-early", kMidnight - kDay + 12 * 3600, psg(), bay(), 1, 2));
  v.push_back(make("m-far", "esp.1", MatchState::Pre, kMidnight + 9 * kDay + 20 * 3600, bar(), atm()));
  return v;
}

uint32_t u32(lv_color_t c) { return lv_color_to_u32(c); }
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

struct Fixture {
  Store store;
  CountingImages images;
  std::vector<std::string> opened;
  lv_obj_t* screen = nullptr;

  Fixture() {
    init_test_display();
    store.set_favourite("359");
    screen = matches_create(lv_screen_active(), store, images,
                            [this](std::string id) { opened.push_back(std::move(id)); });
  }
  ~Fixture() { deinit_test_display(); }

  const MatchesParts* parts() const { return matches_parts(screen); }

  void refresh(int64_t now = kNow) {
    matches_refresh(screen, now);
    test_advance(20);
    lv_obj_update_layout(screen);
  }

  void load(std::vector<MatchSummary> v, int64_t now = kNow) {
    store.set_matches(std::move(v), now);
    refresh(now);
  }

  // "#TEXT" for a header, the match id for a row, in display order.
  std::vector<std::string> sequence() const {
    std::vector<std::string> out;
    for (const MatchesItem& it : matches_items(screen)) {
      out.push_back(it.header ? "#" + text(it.headerLabel) : it.row.matchId);
    }
    return out;
  }

  MatchesRowParts row(const std::string& id) const {
    for (const MatchesItem& it : matches_items(screen)) {
      if (!it.header && it.row.matchId == id) return it.row;
    }
    FAIL("no row for " << id);
    return {};
  }
};

using Seq = std::vector<std::string>;

}  // namespace

TEST_CASE("headers: LIVE NOW, TODAY, TOMORROW, weekdays, short dates, then past results") {
  Fixture f;
  f.load(mixed());
  CHECK(f.sequence() == Seq{"#LIVE NOW", "m-ars", "m-atm",
                            "#TODAY", "m-rma", "m-inter",
                            "#TOMORROW", "m-ppd", "m-mnc",
                            "#SATURDAY", "m-bay",
                            "#THU 29 OCT", "m-far",
                            "#YESTERDAY", "m-psg-late", "m-psg-early",
                            "#SATURDAY", "m-old"});
}

TEST_CASE("the LIVE NOW section holds every live match, whatever the league") {
  Fixture f;
  f.load(mixed());
  const std::vector<MatchesItem> items = matches_items(f.screen);
  REQUIRE(items.size() > 3);
  CHECK(items[0].header);
  CHECK(text(items[0].headerLabel) == "LIVE NOW");
  CHECK(items[1].row.matchId == "m-ars");  // Premier League
  CHECK(items[2].row.matchId == "m-atm");  // started 20 minutes ago
  CHECK(items[3].header);
  // Both live rows have the amber edge, no other row does.
  int edges = 0;
  for (const MatchesItem& it : items) {
    if (it.header) continue;
    const bool isLive = it.row.matchId == "m-ars" || it.row.matchId == "m-atm";
    CHECK(visible(it.row.edge) == isLive);
    if (visible(it.row.edge)) {
      ++edges;
      CHECK(u32(lv_obj_get_style_bg_color(it.row.edge, 0)) == u32(theme().amber));
    }
  }
  CHECK(edges == 2);
}

TEST_CASE("a day is the local day of the kickoff: 23:30 UTC at +05:30 is tomorrow") {
  Fixture f;
  f.store.set_tz_offset(kIndia);
  // 23:30 UTC today is 05:00 tomorrow in India. now (17:00 UTC) is 22:30 local.
  f.load({make("m-late", "eng.1", MatchState::Pre, kMidnight + 23 * 3600 + 1800, rma(), bar()),
          make("m-soon", "eng.1", MatchState::Pre, kMidnight + 18 * 3600, mnc(), liv())});
  CHECK(f.sequence() == Seq{"#TODAY", "m-soon", "#TOMORROW", "m-late"});
  CHECK(text(f.row("m-late").centre) == "05:00");
  CHECK(text(f.row("m-soon").centre) == "23:30");
  CHECK(text(f.parts()->status.clock) == "22:30");

  // The same kickoff seen from 22:00 UTC (03:30 local, next day) is today.
  f.refresh(kMidnight + 22 * 3600);
  CHECK(f.sequence() == Seq{"#TODAY", "m-late", "#YESTERDAY", "m-soon"});
}

TEST_CASE("rows show the abbreviation, with a fallback for a missing or long one") {
  Fixture f;
  Team noAbbr = team("1", "", "Real Sociedad");
  Team longAbbr = team("2", "SOCIEDAD", "Athletic Club");
  Team five = team("3", "ABCDE", "Villarreal");
  Team four = team("4", "ABCD", "Getafe");
  f.load({make("m1", "esp.1", MatchState::Pre, kNow + 3600, noAbbr, longAbbr),
          make("m2", "esp.1", MatchState::Pre, kNow + 7200, five, four)});
  CHECK(text(f.row("m1").homeAbbr) == "REA");
  CHECK(text(f.row("m1").awayAbbr) == "ATH");
  CHECK(text(f.row("m2").homeAbbr) == "VIL");
  CHECK(text(f.row("m2").awayAbbr) == "ABCD");  // four letters are fine
  f.load(mixed());
  CHECK(text(f.row("m-ars").homeAbbr) == "ARS");
  CHECK(text(f.row("m-ars").awayAbbr) == "CHE");
}

TEST_CASE("the centre cell: kickoff and league, score and clock, score and FT, PPD") {
  Fixture f;
  f.load(mixed());
  MatchesRowParts r = f.row("m-rma");
  CHECK(text(r.centre) == "19:00");
  CHECK(text(r.sub) == "La Liga");
  CHECK(u32(lv_obj_get_style_text_color(r.centre, 0)) == u32(theme().chalk));
  CHECK(u32(lv_obj_get_style_text_color(r.sub, 0)) == u32(theme().dim));
  CHECK(text(f.row("m-inter").sub) == "Serie A");
  CHECK(text(f.row("m-bay").sub) == "Bundesliga");

  r = f.row("m-ars");
  CHECK(text(r.centre) == "2:1");
  CHECK(text(r.sub) == "67'");
  CHECK(u32(lv_obj_get_style_text_color(r.centre, 0)) == u32(theme().amber));

  r = f.row("m-psg-late");
  CHECK(text(r.centre) == "3:1");
  CHECK(text(r.sub) == "FT");
  CHECK(u32(lv_obj_get_style_text_color(r.centre, 0)) == u32(theme().chalk));

  r = f.row("m-ppd");
  CHECK(text(r.centre) == "PPD");
  CHECK(text(r.sub) == "");
  CHECK(lv_obj_get_style_text_font(r.centre, 0) == &bigshoulders_22);
}

TEST_CASE("two-digit scores fit the 54 px centre cell") {
  Fixture f;
  f.load({live("m-a", kNow - 600, ars(), che(), 10, 0), post("m-b", kNow - 2 * 86400, psg(), mar(), 10, 10),
          live("m-c", kNow - 300, rma(), bar(), 0, 0)});
  for (const char* id : {"m-a", "m-b", "m-c"}) {
    MatchesRowParts r = f.row(id);
    lv_obj_update_layout(r.centre);
    INFO(id << ": " << text(r.centre));
    CHECK(lv_obj_get_width(r.centre) <= 54);
    CHECK(lv_obj_get_width(r.centre) > 0);
  }
  CHECK(text(f.row("m-a").centre) == "10:0");
  CHECK(text(f.row("m-b").centre) == "10:10");
  CHECK(lv_obj_get_style_text_font(f.row("m-b").centre, 0) == &bigshoulders_22);
  // Wider than the cell would be even with a bigger score: it steps down.
  f.load({live("m-d", kNow - 300, ars(), che(), 100, 100)});
  MatchesRowParts r = f.row("m-d");
  lv_obj_update_layout(r.centre);
  CHECK(lv_obj_get_width(r.centre) <= 54);
}

TEST_CASE("the favourite's dot appears on the home and the away role only") {
  Fixture f;
  f.load({live("m-home", kNow - 600, ars(), che(), 1, 0),
          make("m-away", "eng.1", MatchState::Pre, kNow + 3 * kDay, bha(), ars()),
          make("m-none", "eng.1", MatchState::Pre, kNow + 3600, mnc(), liv())});
  CHECK(visible(f.row("m-home").favDot));
  CHECK(visible(f.row("m-away").favDot));
  CHECK_FALSE(visible(f.row("m-none").favDot));
  CHECK(u32(lv_obj_get_style_bg_color(f.row("m-home").favDot, 0)) == u32(theme().accent));

  // No favourite chosen: no dots.
  f.store.set_favourite("");
  f.refresh();
  CHECK_FALSE(visible(f.row("m-home").favDot));
  CHECK_FALSE(visible(f.row("m-away").favDot));

  // Another favourite follows.
  f.store.set_favourite("364");
  f.refresh();
  CHECK(visible(f.row("m-none").favDot));
  CHECK_FALSE(visible(f.row("m-home").favDot));
}

TEST_CASE("empty states: connecting, and no matches") {
  Fixture f;
  f.refresh();
  const MatchesParts* p = f.parts();
  REQUIRE(p != nullptr);
  CHECK(visible(p->connecting));
  CHECK(text(p->connecting) == "Connecting...");
  CHECK_FALSE(visible(p->emptyCard));
  CHECK_FALSE(visible(p->list));
  CHECK_FALSE(visible(p->status.staleDot));
  CHECK_FALSE(visible(p->status.staleLabel));
  CHECK(text(p->status.left) == "Matches");
  // Even long after nothing arrived, "Connecting" carries no stale marker.
  f.refresh(kNow + 3600);
  CHECK_FALSE(visible(p->status.staleLabel));

  f.load({});
  CHECK_FALSE(visible(p->connecting));
  CHECK_FALSE(visible(p->list));
  CHECK(visible(p->emptyCard));
  CHECK(text(p->emptyTitle) == "No matches in the next few days");
  CHECK(text(p->emptyHint) == "New fixtures appear here as soon as they are announced.");
  CHECK(f.sequence().empty());

  // The card is centred horizontally and sits inside the screen.
  const int32_t cx = lv_obj_get_x(p->emptyCard) + lv_obj_get_width(p->emptyCard) / 2;
  CHECK(cx == 120);
  CHECK(lv_obj_get_width(p->emptyCard) <= 240 - 2 * 16);

  // Matches arriving replace the empty state.
  f.load(mixed());
  CHECK(visible(p->list));
  CHECK_FALSE(visible(p->emptyCard));
  CHECK_FALSE(visible(p->connecting));
}

TEST_CASE("stale data shows the same marker as the Live screen") {
  Fixture f;
  f.load(mixed());
  const MatchesParts* p = f.parts();
  CHECK_FALSE(visible(p->status.staleDot));
  CHECK_FALSE(visible(p->status.staleLabel));
  CHECK(visible(p->status.freshDot));
  CHECK(text(p->status.clock) == "17:00");

  f.refresh(kNow + 180);
  CHECK(visible(p->status.staleDot));
  CHECK(visible(p->status.staleLabel));
  CHECK_FALSE(visible(p->status.freshDot));
  CHECK(text(p->status.staleLabel) == "Updated 3 min ago");
  CHECK(u32(lv_obj_get_style_bg_color(p->status.staleDot, 0)) == u32(theme().amber));
  CHECK(text(p->status.clock) == "17:03");

  f.load(mixed(), kNow + 200);  // fresh data again
  CHECK_FALSE(visible(p->status.staleLabel));
}

TEST_CASE("tapping a row opens that match") {
  Fixture f;
  f.load(mixed());
  lv_obj_send_event(f.row("m-bay").row, LV_EVENT_CLICKED, nullptr);
  lv_obj_send_event(f.row("m-ars").row, LV_EVENT_CLICKED, nullptr);
  CHECK(f.opened == Seq{"m-bay", "m-ars"});
  CHECK(lv_obj_has_flag(f.row("m-bay").row, LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE("the list scrolls vertically and keeps its position across refreshes") {
  Fixture f;
  f.load(mixed());
  lv_obj_t* list = f.parts()->list;
  CHECK(lv_obj_has_flag(list, LV_OBJ_FLAG_SCROLLABLE));
  CHECK(lv_obj_has_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM));
  CHECK(lv_obj_get_scrollbar_mode(list) == LV_SCROLLBAR_MODE_OFF);
  CHECK((lv_obj_get_scroll_dir(list) & LV_DIR_HOR) == 0);
  CHECK(lv_obj_get_scroll_bottom(list) > 0);  // more rows than fit
  // The status row stays above the list, which ends at the screen bottom.
  CHECK(lv_obj_get_y(list) >= 20);
  CHECK(lv_obj_get_y(list) + lv_obj_get_height(list) <= 320);

  lv_obj_scroll_to_y(list, 60, LV_ANIM_OFF);
  test_advance(20);
  CHECK(lv_obj_get_scroll_y(list) == 60);

  // At the end of the list the last row is fully visible above the list's bottom.
  lv_obj_scroll_to_y(list, 100000, LV_ANIM_OFF);
  test_advance(20);
  lv_area_t listArea;
  lv_area_t lastArea;
  lv_obj_get_coords(list, &listArea);
  lv_obj_get_coords(f.row("m-old").row, &lastArea);
  CHECK(lastArea.y2 <= listArea.y2);
  CHECK(lv_obj_get_scroll_bottom(list) <= 0);

  lv_obj_scroll_to_y(list, 60, LV_ANIM_OFF);
  test_advance(20);
  std::vector<MatchSummary> v = mixed();
  v[2].homeScore = 3;  // m-ars scores
  f.load(v, kNow + 5);
  CHECK(lv_obj_get_scroll_y(list) == 60);
  // Even when the structure changes (a new match appears).
  v.push_back(make("m-new", "eng.1", MatchState::Pre, kNow + 2 * 3600, rma(), che()));
  f.load(v, kNow + 10);
  CHECK(lv_obj_get_scroll_y(list) == 60);
}

TEST_CASE("an unchanged refresh touches nothing; a new score updates one row") {
  Fixture f;
  f.load(mixed());
  const uint32_t descendants = count_descendants(f.screen);
  const size_t requests = f.images.requested.size();
  CHECK(requests == 2 * 11);  // two crests per match, sized for the row
  CHECK(f.images.requested.front().find("w=24") != std::string::npos);
  const Seq before = f.sequence();
  std::vector<lv_obj_t*> rows;
  for (const MatchesItem& it : matches_items(f.screen)) {
    if (!it.header) rows.push_back(it.row.row);
  }

  for (int i = 0; i < 5; ++i) f.refresh(kNow + i);
  CHECK(count_descendants(f.screen) == descendants);
  CHECK(f.images.requested.size() == requests);
  CHECK(f.sequence() == before);

  std::vector<MatchSummary> v = mixed();
  for (MatchSummary& m : v) {
    if (m.id == "m-ars") m.homeScore = 3;
  }
  f.load(v, kNow + 5);
  CHECK(text(f.row("m-ars").centre) == "3:1");
  CHECK(text(f.row("m-atm").centre) == "0:0");
  CHECK(text(f.row("m-psg-late").centre) == "3:1");
  CHECK(count_descendants(f.screen) == descendants);
  CHECK(f.images.requested.size() == requests);
  std::vector<lv_obj_t*> after;
  for (const MatchesItem& it : matches_items(f.screen)) {
    if (!it.header) after.push_back(it.row.row);
  }
  CHECK(after == rows);  // the same row objects
}

TEST_CASE("a match going live moves to LIVE NOW and keeps its row; a changed crest reloads") {
  Fixture f;
  f.load(mixed());
  lv_obj_t* row = f.row("m-rma").row;
  const size_t requests = f.images.requested.size();

  std::vector<MatchSummary> v = mixed();
  for (MatchSummary& m : v) {
    if (m.id == "m-rma") {
      m.state = MatchState::Live;
      m.clock = "1'";
      m.homeScore = 0;
      m.awayScore = 0;
    }
  }
  f.load(v, kNow + 5);
  CHECK(f.sequence() == Seq{"#LIVE NOW", "m-ars", "m-atm", "m-rma",
                            "#TODAY", "m-inter",
                            "#TOMORROW", "m-ppd", "m-mnc",
                            "#SATURDAY", "m-bay",
                            "#THU 29 OCT", "m-far",
                            "#YESTERDAY", "m-psg-late", "m-psg-early",
                            "#SATURDAY", "m-old"});
  CHECK(f.row("m-rma").row == row);
  CHECK(visible(f.row("m-rma").edge));
  CHECK(text(f.row("m-rma").centre) == "0:0");
  CHECK(f.images.requested.size() == requests);

  for (MatchSummary& m : v) {
    if (m.id == "m-rma") m.home.crest = "/api/img?u=new&w=48&h=48";
  }
  f.load(v, kNow + 6);
  REQUIRE(f.images.requested.size() == requests + 1);
  CHECK(f.images.requested.back().find("u=new") != std::string::npos);

  // Matches that disappear take their rows (and headers) with them.
  v.erase(v.begin() + 5);  // m-bay, the only match on that Saturday
  f.load(v, kNow + 7);
  const Seq seq = f.sequence();
  CHECK(std::count(seq.begin(), seq.end(), "#SATURDAY") == 1);
}

TEST_CASE("rows are cards in the panel colour; live rows are tinted") {
  Fixture f;
  f.load(mixed());
  const MatchesRowParts plain = f.row("m-rma");
  CHECK(u32(lv_obj_get_style_bg_color(plain.row, 0)) == u32(lv_color_hex(0x0F1D17)));
  CHECK(lv_obj_get_width(plain.row) == 240 - 16);
  lv_area_t area;
  lv_obj_get_coords(plain.row, &area);
  CHECK(area.x1 == 8);
  CHECK(lv_obj_get_style_radius(plain.row, 0) >= 8);
  CHECK(u32(lv_obj_get_style_bg_color(f.row("m-ars").row, 0)) != u32(lv_color_hex(0x0F1D17)));
  CHECK(lv_obj_get_width(f.row("m-ars").edge) == 3);
  for (const MatchesItem& it : matches_items(f.screen)) {
    if (it.header) {
      CHECK(lv_obj_get_style_text_font(it.headerLabel, 0) == &bigshoulders_16);
      CHECK(u32(lv_obj_get_style_text_color(it.headerLabel, 0)) == u32(theme().dim));
    }
  }
}

TEST_CASE("matches_parts and matches_items ignore other objects") {
  Fixture f;
  CHECK(matches_parts(nullptr) == nullptr);
  CHECK(matches_parts(lv_screen_active()) == nullptr);
  CHECK(matches_items(lv_screen_active()).empty());
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
  lv_obj_t* matches() const {
    lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), 1);
    return lv_obj_get_child(tab, 0);
  }
};

}  // namespace

TEST_CASE("app: the Matches tab hosts the matches screen and follows app_refresh") {
  AppHost h;
  REQUIRE(matches_parts(h.matches()) != nullptr);
  CHECK(text(matches_parts(h.matches())->connecting) == "Connecting...");

  h.store.set_matches(mixed(), kNow);
  app_refresh(h.app, kNow);
  h.settle();
  CHECK(matches_items(h.matches()).size() == 11 + 7);  // eleven rows, seven headers

  // The tab ends above the tab bar and the list fits inside it.
  lv_tabview_set_active(h.app, 1, LV_ANIM_OFF);
  h.settle();
  lv_obj_t* tab = lv_obj_get_parent(h.matches());
  const lv_obj_t* list = matches_parts(h.matches())->list;
  CHECK(lv_obj_get_y(const_cast<lv_obj_t*>(list)) + lv_obj_get_height(const_cast<lv_obj_t*>(list)) <=
        lv_obj_get_height(tab));
  CHECK(lv_obj_get_height(tab) == 320 - layout::kTabBarHeight);
}

TEST_CASE("app: tapping a Matches row opens the detail overlay") {
  AppHost h;
  h.store.set_matches(mixed(), kNow);
  app_refresh(h.app, kNow);
  h.settle();
  CHECK(app_detail_view(h.app) == nullptr);

  MatchesRowParts row;
  for (const MatchesItem& it : matches_items(h.matches())) {
    if (!it.header && it.row.matchId == "m-mnc") row = it.row;
  }
  REQUIRE(row.row != nullptr);
  lv_obj_send_event(row.row, LV_EVENT_CLICKED, nullptr);
  h.settle();
  CHECK(app_detail_view(h.app) != nullptr);
  REQUIRE(h.source.calls.size() == 1);
  CHECK(h.source.calls[0] == "m-mnc");
  CHECK(h.opened == std::vector<std::string>{"m-mnc"});
}

namespace {

lv_area_t coords(lv_obj_t* o) {
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  return a;
}

// True when the two rectangles, one grown by `clearance` px, overlap.
bool too_near(const lv_area_t& a, const lv_area_t& b, int32_t clearance) {
  return a.x1 - clearance <= b.x2 && b.x1 <= a.x2 + clearance && a.y1 - clearance <= b.y2 &&
         b.y1 <= a.y2 + clearance;
}

}  // namespace

TEST_CASE("the favourite's dot sits by its own crest, clear of both crests") {
  Fixture f;
  f.load({live("m-home", kNow - 600, ars(), che(), 1, 0),
          make("m-away", "eng.1", MatchState::Pre, kNow + 3 * kDay, bha(), ars())});
  for (const char* id : {"m-home", "m-away"}) {
    INFO("row: ", id);
    MatchesRowParts r = f.row(id);
    lv_obj_update_layout(r.row);
    const lv_area_t dot = coords(r.favDot);
    const lv_area_t home = coords(r.homeCrest);
    const lv_area_t away = coords(r.awayCrest);
    const lv_area_t row = coords(r.row);
    CHECK_FALSE(too_near(dot, home, 2));
    CHECK_FALSE(too_near(dot, away, 2));
    // Inside the row.
    CHECK(dot.x1 >= row.x1);
    CHECK(dot.x2 <= row.x2);
    CHECK(dot.y1 >= row.y1);
    CHECK(dot.y2 <= row.y2);
    // On the favourite's side.
    const int32_t rowMid = (row.x1 + row.x2) / 2;
    if (std::string(id) == "m-home") {
      CHECK(dot.x2 < rowMid);
    } else {
      CHECK(dot.x1 > rowMid);
    }
  }
}
