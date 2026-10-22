#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "app.h"
#include "prefs.h"
#include "data_source.h"
#include "fonts.h"
#include "format.h"
#include "screens/news.h"
#include "snapshot.h"
#include "test_display.h"
#include "theme.h"

namespace {

// Counts requests; delivers nothing unless `deliver` is set, then a solid
// 240x135 picture (what the helper sends for a news photo).
class FakeImages : public ImageProvider {
 public:
  void request(const std::string& path, std::function<void(ImagePtr)> done) override {
    requested.push_back(path);
    // A row thumbnail asks for a 48x48 crop, the lead for the 240x135 photo.
    const bool thumb = path.find("&w=48&h=48") != std::string::npos;
    done(deliver ? (thumb ? solid(48, 48) : solid()) : nullptr);
  }

  static ImagePtr solid(int32_t w = 240, int32_t h = 135) {
    struct Owned {
      lv_image_dsc_t dsc{};
      std::vector<uint16_t> pixels;
    };
    auto owned = std::make_shared<Owned>();
    owned->pixels.assign(static_cast<size_t>(w) * h, 0x07E0);  // green
    owned->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    owned->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    owned->dsc.header.w = w;
    owned->dsc.header.h = h;
    owned->dsc.header.stride = w * 2;
    owned->dsc.data_size = owned->pixels.size() * 2;
    owned->dsc.data = reinterpret_cast<const uint8_t*>(owned->pixels.data());
    return ImagePtr(owned, &owned->dsc);
  }

  bool deliver = false;
  std::vector<std::string> requested;
};

class IdleSource : public DataSource {
 public:
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    cb(false, {});
  }
  void fetch_detail(const std::string&, std::function<void(bool, MatchDetail)> cb) override {
    cb(false, {});
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override { cb(false, {}); }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override { cb(false, {}); }
};

constexpr int64_t kNow = 1792526400 - 3 * 3600;  // Tue 2026-10-20 17:00 UTC
constexpr int64_t kDay = 86400;
constexpr int64_t kMidnight = kNow - 17 * 3600;  // 00:00 UTC of that Tuesday
constexpr int kIndia = 5 * 3600 + 30 * 60;

std::string photo(const std::string& name) { return "/api/img?u=x%2F" + name + ".jpg&fit=cover&w=240&h=135"; }
// What a row asks for: the same photo as a 48x48 cover crop.
std::string thumb(const std::string& name) { return "/api/img?u=x%2F" + name + ".jpg&fit=cover&w=48&h=48"; }

NewsItem article(const std::string& id, int64_t ageSeconds, const std::string& headline = "",
                 const std::string& image = "") {
  NewsItem n;
  n.id = id;
  n.headline = headline.empty() ? "Headline " + id : headline;
  n.published = kNow - ageSeconds;
  n.image = image.empty() ? photo(id) : image;
  return n;
}

Team team(const char* id, const char* abbr, const char* name) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  return t;
}

MatchSummary match(const char* id, MatchState state, int64_t kickoff, Team home, Team away) {
  MatchSummary m;
  m.id = id;
  m.league = "eng.1";
  m.state = state;
  m.kickoff = kickoff;
  m.home = std::move(home);
  m.away = std::move(away);
  return m;
}

MatchSummary finished(const char* id, int64_t kickoff, Team home, Team away, int hs, int as) {
  MatchSummary m = match(id, MatchState::Post, kickoff, std::move(home), std::move(away));
  m.clock = "FT";
  m.homeScore = hs;
  m.awayScore = as;
  return m;
}

Team ars() { return team("359", "ARS", "Arsenal"); }
Team che() { return team("363", "CHE", "Chelsea"); }
Team liv() { return team("364", "LIV", "Liverpool"); }
Team mnc() { return team("382", "MNC", "Manchester City"); }

uint32_t u32(lv_color_t c) { return lv_color_to_u32(c); }
uint32_t rgb(lv_color_t c) { return lv_color_to_u32(c) & 0xFFFFFF; }
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
  FakeImages images;
  lv_obj_t* screen = nullptr;

  Fixture() {
    init_test_display();
    store.set_favourite("359");
    screen = news_create(lv_screen_active(), store, images);
  }
  ~Fixture() { deinit_test_display(); }

  const NewsParts* parts() const { return news_parts(screen); }

  void refresh(int64_t now = kNow) {
    news_refresh(screen, now);
    test_advance(20);
    lv_obj_update_layout(screen);
  }

  // Matches (the Store has data) and news.
  void load(std::vector<NewsItem> news, std::vector<MatchSummary> matches = {}, int64_t now = kNow) {
    store.set_matches(std::move(matches), now);
    store.set_news(std::move(news));
    refresh(now);
  }

  std::vector<std::string> row_texts() const {
    std::vector<std::string> out;
    for (const NewsRowParts& r : news_rows(screen)) out.push_back(text(r.headline));
    return out;
  }
};

std::vector<NewsItem> nine() {
  std::vector<NewsItem> v;
  for (int i = 0; i < 9; ++i) v.push_back(article("a" + std::to_string(i), (9 - i) * 3600));
  return v;  // a8 is the newest, a0 the oldest
}

}  // namespace

TEST_CASE("the lead story is the newest article, whatever the input order") {
  Fixture f;
  f.load({article("old", 5 * 3600), article("newest", 600), article("mid", 2 * 3600)});
  const NewsParts* p = f.parts();
  REQUIRE(p != nullptr);
  CHECK(p->leadId == "newest");
  CHECK(text(p->leadHeadline) == "Headline newest");
  CHECK(f.row_texts() == std::vector<std::string>{"Headline mid", "Headline old"});
  CHECK(news_rows(f.screen)[0].id == "mid");
}

TEST_CASE("articles with the same time keep their input order") {
  Fixture f;
  f.load({article("x", 3600), article("y", 3600), article("z", 3600)});
  CHECK(f.parts()->leadId == "x");
  CHECK(f.row_texts() == std::vector<std::string>{"Headline y", "Headline z"});
}

TEST_CASE("at most six articles show: one lead and five rows, newest first") {
  Fixture f;
  f.load(nine());
  CHECK(f.parts()->leadId == "a8");
  std::vector<std::string> ids;
  for (const NewsRowParts& r : news_rows(f.screen)) ids.push_back(r.id);
  CHECK(ids == std::vector<std::string>{"a7", "a6", "a5", "a4", "a3"});
  // An article id that appears twice is shown once.
  f.load({article("d", 600), article("d", 600), article("e", 1200)});
  CHECK(f.parts()->leadId == "d");
  CHECK(news_rows(f.screen).size() == 1);
}

TEST_CASE("the lead and the rows show the relative time") {
  Fixture f;
  f.load({article("a", 25 * 60), article("b", 2 * 3600), article("c", 5 * 3600),
          article("d", 24 * 3600 + 60), article("e", 3 * kDay), article("f", 9 * kDay)});
  CHECK(text(f.parts()->leadTime) == "25 min ago");
  const std::vector<NewsRowParts> rows = news_rows(f.screen);
  REQUIRE(rows.size() == 5);
  CHECK(text(rows[0].time) == "2h ago");
  CHECK(text(rows[1].time) == "5h ago");
  CHECK(text(rows[2].time) == "yesterday");
  CHECK(text(rows[3].time) == "3 days ago");
  CHECK(text(rows[4].time) == "9 days ago");

  // The text follows `now`, and only changes when the wording does.
  f.refresh(kNow + 3600);
  CHECK(text(f.parts()->leadTime) == "1h ago");
  CHECK(text(news_rows(f.screen)[0].time) == "3h ago");
  CHECK(text(news_rows(f.screen)[2].time) == "yesterday");
}

TEST_CASE("relative times in the screen match relative_time at a fixed now") {
  Fixture f;
  std::vector<NewsItem> items = {article("a", 40), article("b", 3599), article("c", 86399),
                                 article("d", 47 * 3600), article("e", 29 * kDay),
                                 article("f", 40 * kDay)};
  f.load(items);
  std::vector<std::string> expected;
  for (const NewsItem& n : items) expected.push_back(relative_time(kNow, n.published, 0));
  std::vector<std::string> shown = {text(f.parts()->leadTime)};
  for (const NewsRowParts& r : news_rows(f.screen)) shown.push_back(text(r.time));
  CHECK(shown == expected);
}

TEST_CASE("a 120 character headline in a row is cut to two lines with dots") {
  Fixture f;
  const std::string longText =
      "Manchester United confirm a sweeping review of their recruitment department after another "
      "difficult afternoon at home";
  REQUIRE(longText.size() >= 110);
  f.load({article("lead", 600), article("long", 3600, longText)});
  const NewsRowParts row = news_rows(f.screen)[0];
  lv_obj_t* label = row.headline;
  const int32_t line = hanken_12.line_height;
  CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_DOT);
  CHECK(lv_obj_get_height(label) == 2 * line);
  CHECK(lv_obj_get_width(label) <= 154);
  // The label cuts its own text and ends it in dots.
  CHECK(text(label).size() < longText.size());
  CHECK(text(label).substr(text(label).size() - 3) == "...");
  CHECK(longText.compare(0, 20, text(label), 0, 20) == 0);
  // Unclipped, the text needs more than two lines at that width.
  lv_point_t size;
  lv_text_get_size(&size, longText.c_str(), &hanken_12, 0, 0, lv_obj_get_width(label), LV_TEXT_FLAG_NONE);
  CHECK(size.y > 2 * line);
  // A short headline keeps the same label size.
  CHECK(lv_obj_get_height(news_rows(f.screen)[0].headline) == 2 * line);
  f.load({article("lead", 600), article("short", 3600, "Short one")});
  CHECK(lv_obj_get_height(news_rows(f.screen)[0].headline) == 2 * line);
}

TEST_CASE("the lead headline is clamped to three lines with dots") {
  Fixture f;
  const std::string longText =
      "Dejan Kulusevski makes Tottenham return 526 days after suffering a serious knee injury that "
      "kept him out for most of last season and the summer";
  f.load({article("lead", 600, longText)});
  lv_obj_t* label = f.parts()->leadHeadline;
  CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_DOT);
  CHECK(lv_obj_get_width(label) <= 216);
  const lv_font_t* font = lv_obj_get_style_text_font(label, 0);
  int32_t space = lv_obj_get_style_text_line_space(label, 0);
  auto lines_tall = [&](int32_t n) { return n * font->line_height + (n - 1) * space; };
  CHECK(lv_obj_get_height(label) == lines_tall(3));
  CHECK(text(label).size() < longText.size());
  CHECK(text(label).substr(text(label).size() - 3) == "...");
  CHECK(lv_obj_get_style_text_font(label, 0) == &bigshoulders_16);  // too long for the big face
  // A one-line headline gives a one-line label.
  f.load({article("lead", 600, "Short")});
  font = lv_obj_get_style_text_font(label, 0);
  space = lv_obj_get_style_text_line_space(label, 0);
  CHECK(lv_obj_get_height(label) == lines_tall(1));
  CHECK(text(label) == "Short");
  f.load({article("lead", 600, "Arsenal beat Chelsea at home")});
  CHECK(lv_obj_get_height(label) == lines_tall(2));
  CHECK(text(label) == "Arsenal beat Chelsea at home");
}

TEST_CASE("the lead headline uses the big face when it fits three lines, else the small one") {
  Fixture f;
  f.load({article("lead", 600, "Arsenal beat Chelsea at home")});
  lv_obj_t* label = f.parts()->leadHeadline;
  CHECK(lv_obj_get_style_text_font(label, 0) == &bigshoulders_22);
  f.load({article("lead", 600, "Nicol: Manchester United's recruitment has to change")});
  CHECK(lv_obj_get_style_text_font(label, 0) == &bigshoulders_22);  // exactly three lines
  CHECK(text(label) == "Nicol: Manchester United's recruitment has to change");
  const std::string real = "Dejan Kulusevski makes Tottenham return 526 days after suffering knee injury";
  f.load({article("lead", 600, real)});
  CHECK(lv_obj_get_style_text_font(label, 0) == &bigshoulders_16);
  // All 76 characters fit three lines of the small face: no dots.
  CHECK(text(label) == real);
  f.load({article("lead", 600, "Short")});
  CHECK(lv_obj_get_style_text_font(label, 0) == &bigshoulders_22);
}

TEST_CASE("the last-result tile shows the favourite's goals first") {
  Fixture f;
  // Favourite at home, 3:1 win.
  f.load({article("a", 600)}, {finished("m1", kNow - 3 * kDay, ars(), che(), 3, 1)});
  const NewsParts* p = f.parts();
  CHECK(text(p->last.caption) == "Last: CHE");
  CHECK(text(p->last.value) == "3:1");
  CHECK(text(p->last.suffix) == "FT");
  CHECK(visible(p->last.suffix));

  // Away, 3:1 win: still 3:1.
  f.load({article("a", 600)}, {finished("m1", kNow - 3 * kDay, che(), ars(), 1, 3)});
  CHECK(text(p->last.caption) == "Last: CHE");
  CHECK(text(p->last.value) == "3:1");

  // Away, 0:2 loss reads 0:2.
  f.load({article("a", 600)}, {finished("m1", kNow - 3 * kDay, liv(), ars(), 2, 0)});
  CHECK(text(p->last.caption) == "Last: LIV");
  CHECK(text(p->last.value) == "0:2");

  // Home, 1:1 draw; the most recent finished match wins.
  f.load({article("a", 600)}, {finished("old", kNow - 9 * kDay, ars(), mnc(), 5, 0),
                               finished("new", kNow - kDay, ars(), che(), 1, 1)});
  CHECK(text(p->last.caption) == "Last: CHE");
  CHECK(text(p->last.value) == "1:1");
}

TEST_CASE("without a result or a fixture the tiles say so") {
  Fixture f;
  f.load({article("a", 600)}, {match("m", MatchState::Pre, kNow - kDay, mnc(), liv())});
  const NewsParts* p = f.parts();
  CHECK(text(p->last.caption) == "Last result");
  CHECK(text(p->last.value) == "No result yet");
  CHECK_FALSE(visible(p->last.suffix));
  CHECK(text(p->next.caption) == "Next fixture");
  CHECK(text(p->next.value) == "No fixture yet");
  // The text fits its tile.
  for (lv_obj_t* v : {p->last.value, p->next.value}) {
    CHECK(lv_obj_get_x(v) >= 0);
    CHECK(lv_obj_get_x(v) + lv_obj_get_width(v) <= lv_obj_get_width(lv_obj_get_parent(v)) - 6);
  }
  // Only matches of other teams: still nothing.
  f.load({article("a", 600)}, {finished("m", kNow - kDay, mnc(), liv(), 1, 0)});
  CHECK(text(p->last.value) == "No result yet");
  // No favourite chosen.
  f.store.set_favourite("");
  f.load({article("a", 600)}, {finished("m", kNow - kDay, ars(), che(), 1, 0)});
  CHECK(text(p->last.value) == "No result yet");
  CHECK(text(p->next.value) == "No fixture yet");
}

TEST_CASE("the next-fixture tile shows the local weekday and time") {
  Fixture f;
  f.store.set_tz_offset(kIndia);
  // Saturday 12:00 UTC is 17:30 in India.
  f.load({article("a", 600)},
         {match("n1", MatchState::Pre, kMidnight + 4 * kDay + 12 * 3600, ars(), liv())});
  const NewsParts* p = f.parts();
  CHECK(text(p->next.caption) == "Next: LIV");
  CHECK(text(p->next.value) == "Sat 17:30");

  // Today and tomorrow, local days (now is 22:30 local).
  f.load({article("a", 600)}, {match("n2", MatchState::Pre, kNow + 3600, che(), ars())});
  CHECK(text(p->next.caption) == "Next: CHE");
  CHECK(text(p->next.value) == "Today 23:30");
  f.load({article("a", 600)}, {match("n3", MatchState::Pre, kNow + 3 * 3600, ars(), che())});
  CHECK(text(p->next.value) == "Tomorrow 01:30");  // 20:00 UTC is 01:30 on Wednesday, local

  // The earliest future fixture, not a live or postponed one.
  f.store.set_tz_offset(0);
  f.load({article("a", 600)},
         {match("late", MatchState::Pre, kNow + 4 * kDay, ars(), che()),
          match("soon", MatchState::Pre, kNow + 2 * kDay + 2 * 3600, liv(), ars()),
          match("ppd", MatchState::Postponed, kNow + 600, ars(), mnc())});
  CHECK(text(p->next.caption) == "Next: LIV");
  CHECK(text(p->next.value) == "Thu 19:00");
  CHECK(lv_obj_get_width(p->next.value) <= lv_obj_get_width(p->next.card) - 12);

  // A fixture a week or more away gets a date instead of a weekday.
  f.load({article("a", 600)}, {match("far", MatchState::Pre, kNow + 9 * kDay, ars(), che())});
  CHECK(text(p->next.value) == "29 Oct 17:00");
  CHECK(lv_obj_get_width(p->next.value) <= lv_obj_get_width(p->next.card) - 12);
}

TEST_CASE("tile values fit the tile even when long") {
  Fixture f;
  f.load({article("a", 600)}, {match("n", MatchState::Pre, kNow + kDay + 3600, ars(), che())});
  const NewsParts* p = f.parts();
  CHECK(text(p->next.value) == "Tomorrow 18:00");
  lv_obj_update_layout(p->next.value);
  CHECK(lv_obj_get_width(p->next.value) <= lv_obj_get_width(p->next.card) - 12);
  CHECK(lv_obj_get_x(p->next.value) >= 0);

  // The widest case ("Tomorrow 20:00") steps down and tightens to fit.
  f.load({article("a", 600)}, {match("n", MatchState::Pre, kNow + kDay + 3 * 3600, ars(), che())});
  CHECK(text(p->next.value) == "Tomorrow 20:00");
  lv_obj_update_layout(p->next.value);
  CHECK(lv_obj_get_width(p->next.value) <= lv_obj_get_width(p->next.card) - 12);
}

TEST_CASE("empty states: connecting, and no headlines") {
  Fixture f;
  f.store.set_news({article("a", 600)});  // news alone does not end "Connecting"
  f.refresh();
  const NewsParts* p = f.parts();
  REQUIRE(p != nullptr);
  CHECK(visible(p->connecting));
  CHECK(text(p->connecting) == "Connecting...");
  CHECK_FALSE(visible(p->emptyCard));
  CHECK_FALSE(visible(p->list));
  CHECK(text(p->status.left) == "News");
  CHECK_FALSE(visible(p->status.staleLabel));
  f.refresh(kNow + 3600);
  CHECK_FALSE(visible(p->status.staleLabel));

  f.load({});
  CHECK_FALSE(visible(p->connecting));
  CHECK_FALSE(visible(p->list));
  CHECK(visible(p->emptyCard));
  CHECK(text(p->emptyTitle) == "No headlines right now");
  CHECK(text(p->emptyHint) == "Check back after the next match.");
  const int32_t cx = lv_obj_get_x(p->emptyCard) + lv_obj_get_width(p->emptyCard) / 2;
  CHECK(cx == 120);
  CHECK(lv_obj_get_width(p->emptyCard) <= 240 - 2 * 16);
  CHECK(news_rows(f.screen).empty());

  f.load({article("a", 600)});
  CHECK(visible(p->list));
  CHECK_FALSE(visible(p->emptyCard));
  CHECK_FALSE(visible(p->connecting));
}

TEST_CASE("stale data shows the same marker as the other screens") {
  Fixture f;
  f.load({article("a", 600)});
  const NewsParts* p = f.parts();
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

  f.load({article("a", 600)}, {}, kNow + 200);
  CHECK_FALSE(visible(p->status.staleLabel));
}

TEST_CASE("an unchanged refresh keeps every widget and requests no image again") {
  Fixture f;
  f.load(nine(), {finished("m", kNow - kDay, ars(), che(), 2, 0)});
  const uint32_t widgets = count_descendants(f.screen);
  const size_t requests = f.images.requested.size();
  CHECK(requests > 0);
  std::vector<lv_obj_t*> rows;
  for (const NewsRowParts& r : news_rows(f.screen)) rows.push_back(r.row);

  for (int i = 0; i < 3; ++i) f.refresh(kNow + i);
  CHECK(count_descendants(f.screen) == widgets);
  CHECK(f.images.requested.size() == requests);
  std::vector<lv_obj_t*> again;
  for (const NewsRowParts& r : news_rows(f.screen)) again.push_back(r.row);
  CHECK(again == rows);

  // Same data set again: still nothing new.
  f.store.set_news(nine());
  f.refresh();
  CHECK(count_descendants(f.screen) == widgets);
  CHECK(f.images.requested.size() == requests);
}

TEST_CASE("a new article becomes the lead, the old lead a row, and the sixth falls off") {
  Fixture f;
  std::vector<NewsItem> items = nine();
  items.resize(6);  // a0..a5; a5 is the newest
  f.load(items);
  CHECK(f.parts()->leadId == "a5");
  CHECK(news_rows(f.screen).size() == 5);
  const uint32_t widgets = count_descendants(f.screen);
  lv_obj_t* a3Row = nullptr;
  for (const NewsRowParts& r : news_rows(f.screen)) {
    if (r.id == "a3") a3Row = r.row;
  }
  REQUIRE(a3Row != nullptr);
  const size_t before = f.images.requested.size();

  items.push_back(article("fresh", 60));
  f.load(items);
  CHECK(f.parts()->leadId == "fresh");
  CHECK(text(f.parts()->leadHeadline) == "Headline fresh");
  std::vector<std::string> ids;
  for (const NewsRowParts& r : news_rows(f.screen)) ids.push_back(r.id);
  CHECK(ids == std::vector<std::string>{"a5", "a4", "a3", "a2", "a1"});  // a0 fell off
  CHECK(count_descendants(f.screen) == widgets);
  // Surviving rows keep their widgets.
  for (const NewsRowParts& r : news_rows(f.screen)) {
    if (r.id == "a3") CHECK(r.row == a3Row);
  }
  // Only the new article's pictures were asked for: the lead, and the old lead's new row.
  CHECK(f.images.requested.size() - before <= 3);
  // Rows sit in order on screen.
  const std::vector<NewsRowParts> rows = news_rows(f.screen);
  for (size_t i = 1; i < rows.size(); ++i) {
    CHECK(lv_obj_get_y(rows[i].row) > lv_obj_get_y(rows[i - 1].row));
  }
}

TEST_CASE("a headline or image change updates only that row") {
  Fixture f;
  f.load({article("a", 600), article("b", 3600), article("c", 7200)});
  const size_t before = f.images.requested.size();
  NewsItem b = article("b", 3600, "A better headline");
  f.load({article("a", 600), b, article("c", 7200)});
  CHECK(f.images.requested.size() == before);
  CHECK(text(news_rows(f.screen)[0].headline) == "A better headline");
  b.image = photo("other");
  f.load({article("a", 600), b, article("c", 7200)});
  CHECK(f.images.requested.size() == before + 1);
  CHECK(f.images.requested.back() == thumb("other"));
}

TEST_CASE("without a picture the lead is an accent gradient, never empty") {
  Fixture f;
  f.load({article("a", 600)});
  const NewsParts* p = f.parts();
  lv_obj_update_layout(p->lead);
  CHECK(visible(p->lead));
  CHECK(lv_obj_get_width(p->lead) == 240);
  CHECK(lv_obj_get_height(p->lead) == 158);
  CHECK(lv_image_get_src(p->leadImage) == nullptr);
  CHECK(lv_obj_get_style_bg_opa(p->lead, 0) == LV_OPA_COVER);
  const lv_grad_dsc_t* grad = lv_obj_get_style_bg_grad(p->lead, 0);
  REQUIRE(grad != nullptr);
  CHECK(grad->stops_count == 2);
  CHECK(u32(grad->stops[1].color) == u32(theme().brand));
  CHECK(u32(grad->stops[0].color) != u32(grad->stops[1].color));
  // It renders as colour, not black.
  const lv_color_t px = pixel_at(p->lead, 200, 60);
  CHECK(u32(px) != 0);
  CHECK(lv_color_to_u32(px) != u32(theme().turf));

  // An article without an image path behaves the same.
  NewsItem none = article("b", 300);
  none.image.clear();
  f.load({none});
  CHECK(lv_image_get_src(p->leadImage) == nullptr);
  CHECK(visible(p->lead));
  const size_t requests = f.images.requested.size();
  f.refresh();
  CHECK(f.images.requested.size() == requests);

  // The gradient follows the favourite's colour (two-argument form: brand = accent).
  theme_set_team(0x0A7B3E, 0x034694);
  f.load({article("c", 300)});
  grad = lv_obj_get_style_bg_grad(p->lead, 0);
  CHECK(rgb(grad->stops[1].color) == 0x0A7B3E);

  // It is a large surface: it uses the brand colour, not the accent.
  theme_set_team(0xDBA111, 0xEF0107, 0x034694);
  f.load({article("d", 300)});
  grad = lv_obj_get_style_bg_grad(p->lead, 0);
  CHECK(rgb(grad->stops[1].color) == 0x034694);
  CHECK(u32(grad->stops[0].color) == u32(darken(lv_color_hex(0x034694), 140)));

  // A very dark brand is toned up first, like the Live split (Spurs navy).
  theme_set_team(0xFFFFFF, 0xEF0107, 0x132257);
  f.load({article("e", 300)});
  grad = lv_obj_get_style_bg_grad(p->lead, 0);
  const lv_color_t toned = background_tone(lv_color_hex(0x132257));
  CHECK(u32(grad->stops[1].color) == u32(toned));
  CHECK(u32(grad->stops[0].color) == u32(darken(toned, 140)));
  theme_reset();
}

TEST_CASE("a delivered picture fills the lead and the thumbnails (cover)") {
  Fixture f;
  f.images.deliver = true;
  f.load({article("a", 600), article("b", 3600)});
  const NewsParts* p = f.parts();
  REQUIRE(lv_image_get_src(p->leadImage) != nullptr);
  const int32_t scale = lv_image_get_scale(p->leadImage);
  const int32_t sw = 240 * scale / 256;
  const int32_t sh = 135 * scale / 256;
  CHECK(sw >= 240);
  CHECK(sh >= 158);
  const int32_t x = lv_obj_get_x(p->leadImage);
  const int32_t y = lv_obj_get_y(p->leadImage);
  CHECK(x <= 0);
  CHECK(y <= 0);
  CHECK(x + sw >= 240);
  CHECK(y + sh >= 158);

  const NewsRowParts row = news_rows(f.screen)[0];
  lv_obj_t* img = lv_obj_get_child(row.thumb, 0);
  REQUIRE(lv_image_get_src(img) != nullptr);
  const lv_image_dsc_t* dsc = static_cast<const lv_image_dsc_t*>(lv_image_get_src(img));
  CHECK(dsc->header.w == 48);
  CHECK(dsc->header.h == 48);
  const int32_t ts = lv_image_get_scale(img);
  CHECK(48 * ts / 256 >= 40);
  CHECK(48 * ts / 256 <= 41);  // scaled down to the box, not blown up
  CHECK(lv_obj_get_x(img) <= 0);
  CHECK(lv_obj_get_y(img) <= 0);
  CHECK(lv_obj_get_width(row.thumb) == 40);
  CHECK(lv_obj_get_height(row.thumb) == 40);
  CHECK(lv_obj_get_style_radius(row.thumb, 0) > 0);
  // The picture is not stretched.
  CHECK(lv_image_get_scale_x(img) == lv_image_get_scale_y(img));
  CHECK(lv_image_get_scale_x(p->leadImage) == lv_image_get_scale_y(p->leadImage));
}

TEST_CASE("the lead asks for the 240x135 photo, each row for a 48x48 cover crop of its own") {
  Fixture f;
  f.load({article("a", 600), article("b", 3600), article("c", 7200)});
  const std::vector<std::string>& r = f.images.requested;
  CHECK(std::count(r.begin(), r.end(), photo("a")) == 1);  // the lead (newest)
  CHECK(std::count(r.begin(), r.end(), thumb("b")) == 1);
  CHECK(std::count(r.begin(), r.end(), thumb("c")) == 1);
  CHECK(std::count(r.begin(), r.end(), photo("b")) == 0);
  CHECK(std::count(r.begin(), r.end(), photo("c")) == 0);
  // A photo path without fit still gets a cover crop (never a letterboxed thumbnail).
  NewsItem plain = article("d", 900);
  plain.image = "/api/img?u=x%2Fd.png&w=240&h=135";
  f.load({article("a", 600), plain});
  CHECK(f.images.requested.back() == "/api/img?u=x%2Fd.png&w=48&h=48&fit=cover");
}

TEST_CASE("the screen scrolls vertically and its rows are not tappable") {
  Fixture f;
  f.load(nine(), {finished("m", kNow - kDay, ars(), che(), 2, 0)});
  const NewsParts* p = f.parts();
  CHECK(lv_obj_has_flag(p->list, LV_OBJ_FLAG_SCROLLABLE));
  CHECK(lv_obj_get_scroll_dir(p->list) == LV_DIR_VER);
  CHECK(lv_obj_get_scrollbar_mode(p->list) == LV_SCROLLBAR_MODE_OFF);
  CHECK(lv_obj_get_scroll_bottom(p->list) > 0);
  for (const NewsRowParts& r : news_rows(f.screen)) {
    CHECK_FALSE(lv_obj_has_flag(r.row, LV_OBJ_FLAG_CLICKABLE));
  }
  CHECK_FALSE(lv_obj_has_flag(p->lead, LV_OBJ_FLAG_CLICKABLE));

  // The scroll position survives a refresh.
  lv_obj_scroll_to_y(p->list, 120, LV_ANIM_OFF);
  test_advance(20);
  const int32_t y = lv_obj_get_scroll_y(p->list);
  CHECK(y > 0);
  f.store.set_news([&] {
    std::vector<NewsItem> v = nine();
    v[0].headline = "Changed headline";
    return v;
  }());
  f.refresh();
  CHECK(lv_obj_get_scroll_y(p->list) == y);
}

TEST_CASE("rows are cards in the panel colour with the headline and the time") {
  Fixture f;
  f.load({article("a", 600), article("b", 3600)}, {finished("m", kNow - kDay, ars(), che(), 3, 1)});
  const NewsRowParts r = news_rows(f.screen)[0];
  CHECK(rgb(lv_obj_get_style_bg_color(r.row, 0)) == 0x0F1D17);
  CHECK(lv_obj_get_style_bg_opa(r.row, 0) == LV_OPA_COVER);
  CHECK(lv_obj_get_style_text_font(r.headline, 0) == &hanken_12);
  CHECK(lv_obj_get_style_text_font(r.time, 0) == &hanken_10);
  CHECK(u32(lv_obj_get_style_text_color(r.time, 0)) == u32(theme().dim));
  CHECK(u32(lv_obj_get_style_text_color(r.headline, 0)) == u32(theme().chalk));
  // The tiles are panels too.
  const NewsParts* p = f.parts();
  CHECK(rgb(lv_obj_get_style_bg_color(p->last.card, 0)) == 0x0F1D17);
  CHECK(rgb(lv_obj_get_style_bg_color(p->next.card, 0)) == 0x0F1D17);
  CHECK(lv_obj_get_style_text_font(p->last.value, 0) == &bigshoulders_22);
}

TEST_CASE("news_parts and news_rows ignore other objects") {
  Fixture f;
  CHECK(news_parts(nullptr) == nullptr);
  CHECK(news_parts(lv_screen_active()) == nullptr);
  CHECK(news_rows(lv_screen_active()).empty());
  CHECK(news_rows(nullptr).empty());
  news_refresh(nullptr, kNow);
  news_refresh(lv_screen_active(), kNow);
}

// -------------------------------------------------------------------- app

namespace {

struct AppHost {
  Store store;
  FakeImages images;
  IdleSource source;
  MemoryPrefs prefs;
  AppDeps deps{store, images, source, nullptr, prefs};
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
  lv_obj_t* news() const {
    lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), 2);
    return lv_obj_get_child(tab, 0);
  }
};

}  // namespace

TEST_CASE("app: the News tab hosts the news screen and follows app_refresh") {
  AppHost h;
  REQUIRE(news_parts(h.news()) != nullptr);
  CHECK(text(news_parts(h.news())->connecting) == "Connecting...");

  h.store.set_matches({finished("m", kNow - kDay, ars(), che(), 3, 1)}, kNow);
  h.store.set_news({article("a", 600), article("b", 3600)});
  app_refresh(h.app, kNow);
  h.settle();
  const NewsParts* p = news_parts(h.news());
  CHECK(p->leadId == "a");
  CHECK(news_rows(h.news()).size() == 1);
  CHECK(text(p->last.value) == "3:1");

  // A refresh brings later news.
  h.store.set_news({article("c", 30), article("a", 600)});
  app_refresh(h.app, kNow);
  CHECK(p->leadId == "c");

  // The tab ends above the tab bar and the screen fits inside it.
  lv_tabview_set_active(h.app, 2, LV_ANIM_OFF);
  h.settle();
  lv_obj_t* tab = lv_obj_get_parent(h.news());
  CHECK(lv_obj_get_height(tab) == 320 - layout::kTabBarHeight);
  CHECK(lv_obj_get_height(h.news()) == lv_obj_get_height(tab));
  CHECK(lv_obj_get_height(p->list) <= lv_obj_get_height(tab));
  CHECK(lv_obj_get_y(p->list) + lv_obj_get_height(p->list) <= lv_obj_get_height(tab));
}

// -------------------------------------------------------------------- demo data

#include "demo_data.h"
#include "fixture_source.h"

TEST_CASE("demo data: six news articles of varied age, and results for the News tiles") {
  init_test_display();
  const std::vector<NewsItem> news = demo_news(kNow);
  REQUIRE(news.size() == 6);
  const int64_t ages[] = {25 * 60, 2 * 3600, 5 * 3600, kDay + 2 * 3600, 3 * kDay, 9 * kDay};
  const char* shown[] = {"25 min ago", "2h ago", "5h ago", "yesterday", "3 days ago", "9 days ago"};
  for (size_t i = 0; i < 6; ++i) {
    CHECK(kNow - news[i].published == ages[i]);
    CHECK(relative_time(kNow, news[i].published) == shown[i]);
    CHECK_FALSE(news[i].image.empty());
    CHECK_FALSE(news[i].headline.empty());
  }

  Store store;
  store.set_favourite(demo::kFavouriteId);
  store.set_matches(demo_matches(kNow), kNow);
  const MatchSummary* last = store.last_for(demo::kFavouriteId, kNow);
  REQUIRE(last != nullptr);
  CHECK(last->home.id == demo::kFavouriteId);
  CHECK(*last->homeScore == 3);
  CHECK(*last->awayScore == 1);
  CHECK(store.next_for(demo::kFavouriteId, kNow) != nullptr);
  CHECK(store.live_for(demo::kFavouriteId) != nullptr);
  deinit_test_display();
}
