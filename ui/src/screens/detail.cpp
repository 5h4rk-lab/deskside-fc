#include "screens/detail.h"

#include <stdint.h>

#include <algorithm>
#include <utility>

#include "fonts.h"
#include "format.h"
#include "screens/live.h"
#include "theme.h"

// Geometry follows screen 2 of the mockup: status row (Back, clock), title
// row (score, formation chip), sub-tab row, then the sub-tab body. The detail
// normally fills the 240x276 area above the main tab bar.
namespace {

constexpr int32_t kSide = 10;      // left/right text inset
constexpr int32_t kStatusY = 3;
constexpr int32_t kTitleY = 13;
constexpr int32_t kSubY = 40;
constexpr int32_t kBodyY = 59;
constexpr int32_t kPitchInset = 8;
constexpr int32_t kPitchTop = 13;  // inside the Pitch tab, under the one-line blurb
constexpr int32_t kPitchBottom = 4;
constexpr int32_t kMarkerMax = 24;  // mockup size; smaller when a team has many rows
constexpr int32_t kMarkerMin = 14;
constexpr int32_t kRing = 2;        // 1.5 px in the mockup; LVGL borders are whole pixels
constexpr int32_t kPhotoRequest = 40;  // photo size asked from the helper
constexpr float kRowTolerance = 0.03f;  // y values closer than this share a row
constexpr int32_t kStripe = 24;    // mowing stripe height
constexpr int32_t kBoxHeight = 34;
constexpr int32_t kCircle = 54;
constexpr uint8_t kLineOpa = 179;  // rgba(241,239,228,.7)
constexpr uint32_t kTurfLight = 0x1F5A38;
constexpr uint32_t kTurfDark = 0x1B5233;
constexpr uint32_t kYellow = 0xF2C230;
constexpr uint32_t kRed = 0xE0302B;
constexpr uint32_t kSubIn = 0x4CC38A;
constexpr uint32_t kSubOut = 0xE0605A;
constexpr uint32_t kCardMs = 180;
constexpr int32_t kCardMargin = 4;  // name card inset from the pitch sides
constexpr uint32_t kTabMs = 220;

const char* const kTabNames[3] = {"Summary", "Pitch", "Stats"};

enum class Kind { Detail, Placeholder };

struct DetailState {
  Kind kind = Kind::Detail;
  ImageProvider* images = nullptr;
  std::function<void()> onBack;
  std::function<void()> onRetry;
  DetailParts parts;
  MatchDetail data;
  DetailContext ctx;
  lv_obj_t* root = nullptr;
  lv_obj_t* underline = nullptr;
  lv_obj_t* pages[3] = {nullptr, nullptr, nullptr};
  lv_obj_t* openMarker = nullptr;  // marker whose name card is shown
  std::vector<lv_obj_t*> photoMarkers;
  lv_color_t homeColor{};   // accents: event rows, stat bars, name card
  lv_color_t awayColor{};
  lv_color_t homeMarker{};  // primaries: the pitch markers (large team-colour discs)
  lv_color_t awayMarker{};
  int activeTab = 0;
};

// ---------------------------------------------------------------- equality

bool same(const Team& a, const Team& b) {
  return a.id == b.id && a.abbr == b.abbr && a.name == b.name && a.crest == b.crest &&
         a.color == b.color && a.primary == b.primary;
}

bool same(const MatchSummary& a, const MatchSummary& b) {
  return a.id == b.id && a.league == b.league && a.state == b.state && a.clock == b.clock &&
         a.kickoff == b.kickoff && same(a.home, b.home) && same(a.away, b.away) &&
         a.homeScore == b.homeScore && a.awayScore == b.awayScore;
}

bool same(const MatchEvent& a, const MatchEvent& b) {
  return a.minute == b.minute && a.type == b.type && a.team == b.team && a.player == b.player &&
         a.detail == b.detail;
}

bool same(const LineupPlayer& a, const LineupPlayer& b) {
  return a.name == b.name && a.number == b.number && a.x == b.x && a.y == b.y &&
         a.photo == b.photo;
}

bool same(const Lineup& a, const Lineup& b) {
  if (a.formation != b.formation || a.players.size() != b.players.size()) return false;
  for (size_t i = 0; i < a.players.size(); ++i) {
    if (!same(a.players[i], b.players[i])) return false;
  }
  return true;
}

bool same(const StatRow& a, const StatRow& b) {
  return a.label == b.label && a.home == b.home && a.away == b.away;
}

template <typename T>
bool same_list(const std::vector<T>& a, const std::vector<T>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!same(a[i], b[i])) return false;
  }
  return true;
}

// ---------------------------------------------------------------- helpers

void set_text(lv_obj_t* label, const std::string& text) {
  if (text != lv_label_get_text(label)) lv_label_set_text(label, text.c_str());
}

void show(lv_obj_t* obj, bool visible) {
  if (obj == nullptr) return;
  if (visible == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
    if (visible) {
      lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text = "") {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

lv_obj_t* dot(lv_obj_t* parent, int32_t size, lv_color_t color) {
  lv_obj_t* d = plain(parent);
  lv_obj_set_size(d, size, size);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, color, 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  return d;
}

// Rounded panel in the mockup's card colour (#0F1D17).
lv_obj_t* panel(lv_obj_t* parent) {
  lv_obj_t* p = plain(parent);
  lv_obj_set_style_bg_color(p, theme().panel, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p, 10, 0);
  return p;
}

// Readable text colour on a team colour: chalk on dark shirts, turf on light ones.
lv_color_t text_on(lv_color_t bg) {
  const int luma = (bg.red * 299 + bg.green * 587 + bg.blue * 114) / 1000;
  return luma > 150 ? theme().turf : theme().chalk;
}

DetailState* state_of(lv_obj_t* screen) {
  return static_cast<DetailState*>(lv_obj_get_user_data(screen));
}

lv_color_t side_color(const DetailState* st, Side side) {
  return side == Side::Home ? st->homeColor : st->awayColor;
}

void compute_colors(DetailState* st) {
  auto c = split_colors(lv_color_hex(st->data.match.home.color),
                        lv_color_hex(st->data.match.away.color));
  st->homeColor = c.first;
  st->awayColor = c.second;
  auto m = split_colors(lv_color_hex(st->data.match.home.primary),
                        lv_color_hex(st->data.match.away.primary));
  st->homeMarker = m.first;
  st->awayMarker = m.second;
}

// ---------------------------------------------------------------- header

std::string title_text(const MatchSummary& m) {
  const bool scored = (m.state == MatchState::Live || m.state == MatchState::Post) &&
                      m.homeScore.has_value() && m.awayScore.has_value();
  if (scored) {
    return m.home.abbr + " " + std::to_string(*m.homeScore) + ":" + std::to_string(*m.awayScore) +
           " " + m.away.abbr;
  }
  return m.home.abbr + " v " + m.away.abbr;
}

std::string status_text(const MatchSummary& m, int tzOffset) {
  switch (m.state) {
    case MatchState::Live: return m.clock.empty() ? "Live" : m.clock;
    case MatchState::Post: return "FT";
    case MatchState::Postponed: return "Postponed";
    case MatchState::Pre: {
      const int64_t local = m.kickoff + tzOffset;
      return std::string(weekday(local)) + " " + hh_mm(local);
    }
  }
  return "";
}

// The chip shows the favourite's formation when the favourite plays, else home's.
Side chip_side(const MatchDetail& d, const DetailContext& ctx) {
  return (!ctx.favouriteId.empty() && d.match.away.id == ctx.favouriteId) ? Side::Away
                                                                          : Side::Home;
}

void on_back(lv_event_t* e) {
  auto* st = static_cast<DetailState*>(lv_event_get_user_data(e));
  if (st->onBack) st->onBack();
}

void build_header(DetailState* st) {
  const Palette& pal = theme();
  DetailParts& p = st->parts;

  // Back: a generous tap target around the small text.
  p.back = plain(st->root);
  lv_obj_add_flag(p.back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(p.back, 0, 0);
  lv_obj_set_size(p.back, 72, 20);
  lv_obj_add_event_cb(p.back, on_back, LV_EVENT_CLICKED, st);
  lv_obj_t* backText = label(p.back, &hanken_10, pal.chalk, "< Back");
  lv_obj_set_pos(backText, kSide, kStatusY);

  // Clock / kick-off, right-aligned, with the live dot after it.
  lv_obj_t* right = plain(st->root);
  lv_obj_set_size(right, 140, 13);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right, 5, 0);
  lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -kSide, kStatusY);
  p.status = label(right, &hanken_10, pal.dim);
  p.liveDot = dot(right, 5, pal.amber);

  p.title = label(st->root, &bigshoulders_22, pal.chalk);
  lv_obj_set_pos(p.title, kSide, kTitleY);
  lv_obj_set_width(p.title, 160);
  lv_label_set_long_mode(p.title, LV_LABEL_LONG_DOT);

  p.chip = plain(st->root);
  lv_obj_set_size(p.chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_border_color(p.chip, pal.line, 0);
  lv_obj_set_style_border_width(p.chip, 1, 0);
  lv_obj_set_style_radius(p.chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(p.chip, 7, 0);
  lv_obj_set_style_pad_ver(p.chip, 1, 0);
  lv_obj_align(p.chip, LV_ALIGN_TOP_RIGHT, -kSide, kTitleY + 8);
  p.chipLabel = label(p.chip, &hanken_10, pal.dim);
}

void apply_header(DetailState* st, const MatchSummary* m, const std::string& formation) {
  DetailParts& p = st->parts;
  set_text(p.title, m != nullptr ? title_text(*m) : "");
  set_text(p.status, m != nullptr ? status_text(*m, st->ctx.tzOffset) : "");
  show(p.liveDot, m != nullptr && m->state == MatchState::Live);
  set_text(p.chipLabel, formation);
  show(p.chip, !formation.empty());
}

// ---------------------------------------------------------------- sub-tabs

void move_underline(DetailState* st, bool animate);

void sync_tabs(DetailState* st, bool animate) {
  if (st->parts.tabview == nullptr) return;
  st->activeTab = static_cast<int>(lv_tabview_get_tab_active(st->parts.tabview));
  for (int i = 0; i < 3; ++i) {
    lv_obj_set_style_text_color(st->parts.tabLabels[i],
                                i == st->activeTab ? theme().chalk : theme().dim, 0);
  }
  move_underline(st, animate);
}

void anim_x(void* obj, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(obj), v); }
void anim_w(void* obj, int32_t v) { lv_obj_set_width(static_cast<lv_obj_t*>(obj), v); }

void move_underline(DetailState* st, bool animate) {
  lv_obj_t* target = st->parts.tabLabels[st->activeTab];
  lv_obj_update_layout(st->root);
  lv_area_t t;
  lv_area_t r;
  lv_obj_get_coords(target, &t);
  lv_obj_get_coords(st->root, &r);
  const int32_t x = t.x1 - r.x1;
  const int32_t w = lv_area_get_width(&t);
  lv_obj_t* u = st->underline;
  lv_anim_delete(u, anim_x);
  lv_anim_delete(u, anim_w);
  if (!animate) {
    lv_obj_set_x(u, x);
    lv_obj_set_width(u, w);
    return;
  }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, u);
  lv_anim_set_duration(&a, kTabMs);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_set_exec_cb(&a, anim_x);
  lv_anim_set_values(&a, lv_obj_get_x(u), x);
  lv_anim_start(&a);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, anim_w);
  lv_anim_set_values(&a, lv_obj_get_width(u), w);
  lv_anim_start(&a);
}

void hide_card(DetailState* st);

void on_tab_changed(lv_event_t* e) {
  auto* st = static_cast<DetailState*>(lv_event_get_user_data(e));
  hide_card(st);
  sync_tabs(st, true);
}

void on_tab_label(lv_event_t* e) {
  auto* st = static_cast<DetailState*>(lv_event_get_user_data(e));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  for (int i = 0; i < 3; ++i) {
    if (st->parts.tabLabels[i] == target && st->parts.tabview != nullptr) {
      hide_card(st);
      lv_tabview_set_active(st->parts.tabview, static_cast<uint32_t>(i), LV_ANIM_ON);
      sync_tabs(st, true);
    }
  }
}

void build_tab_row(DetailState* st, bool interactive) {
  lv_obj_t* row = plain(st->root);
  lv_obj_set_pos(row, kSide, kSubY);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 14, 0);
  for (int i = 0; i < 3; ++i) {
    lv_obj_t* l = label(row, &hanken_12, theme().dim, kTabNames[i]);
    st->parts.tabLabels[i] = l;
    if (interactive) {
      lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(l, 7);
      lv_obj_add_event_cb(l, on_tab_label, LV_EVENT_CLICKED, st);
    }
  }
  st->underline = plain(st->root);
  lv_obj_set_size(st->underline, 0, 2);
  lv_obj_set_y(st->underline, kSubY + lv_font_get_line_height(&hanken_12) + 1);
  lv_obj_set_style_bg_color(st->underline, theme().accent, 0);
  lv_obj_set_style_bg_opa(st->underline, interactive ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(st->underline, 1, 0);
}

// A sub-tab page with no padding or theme styling.
void style_page(lv_obj_t* page, bool scroll) {
  lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
  // No rubber band: a swipe down at the top reaches the overlay as "back".
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_ELASTIC);
  if (scroll) {
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
  } else {
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  }
}

// Empty sub-tab: a quiet card with the message and an optional hint line.
// Returns the message label.
lv_obj_t* empty_message(lv_obj_t* page, const char* text, const std::string& hint) {
  lv_obj_t* card = panel(page);
  lv_obj_set_pos(card, kPitchInset, 2);
  lv_obj_set_size(card, 240 - 2 * kPitchInset, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_ver(card, 16, 0);
  lv_obj_set_style_pad_hor(card, 14, 0);
  lv_obj_set_style_pad_row(card, 4, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t* l = label(card, &hanken_14, theme().chalk, text);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  if (!hint.empty()) {
    lv_obj_t* h = label(card, &hanken_10, theme().dim, hint.c_str());
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  }
  return l;
}

std::string kickoff_hint(const DetailState* st) {
  const MatchSummary& m = st->data.match;
  if (m.state != MatchState::Pre) return "";
  const int64_t local = m.kickoff + st->ctx.tzOffset;
  return "Kick-off " + std::string(weekday(local)) + " " + hh_mm(local);
}

// ---------------------------------------------------------------- summary

void draw_event_icon(lv_event_t* e) {
  lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  const auto type = static_cast<EventType>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int32_t cx = (a.x1 + a.x2) / 2;
  const int32_t cy = (a.y1 + a.y2) / 2;

  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  r.bg_opa = LV_OPA_COVER;
  switch (type) {
    case EventType::Goal: {
      // Ball: chalk disc with a dark centre patch and a thin dark rim.
      r.bg_color = theme().chalk;
      r.radius = LV_RADIUS_CIRCLE;
      r.border_color = theme().turf;
      r.border_width = 1;
      r.border_opa = 120;
      lv_area_t ball = {cx - 6, cy - 6, cx + 5, cy + 5};
      lv_draw_rect(layer, &r, &ball);
      lv_draw_rect_dsc_t patch;
      lv_draw_rect_dsc_init(&patch);
      patch.bg_color = theme().turf;
      patch.bg_opa = LV_OPA_COVER;
      patch.radius = 1;
      lv_area_t centre = {cx - 2, cy - 2, cx + 1, cy + 1};
      lv_draw_rect(layer, &patch, &centre);
      break;
    }
    case EventType::Yellow:
    case EventType::Red: {
      r.bg_color = lv_color_hex(type == EventType::Yellow ? kYellow : kRed);
      r.radius = 2;
      lv_area_t card = {cx - 4, cy - 6, cx + 4, cy + 5};
      lv_draw_rect(layer, &r, &card);
      break;
    }
    case EventType::Sub: {
      // Two arrows: in (up, green) and out (down, red).
      lv_draw_triangle_dsc_t t;
      lv_draw_triangle_dsc_init(&t);
      t.bg_opa = LV_OPA_COVER;
      t.bg_color = lv_color_hex(kSubIn);
      t.p[0] = {static_cast<lv_value_precise_t>(cx - 4), static_cast<lv_value_precise_t>(cy - 6)};
      t.p[1] = {static_cast<lv_value_precise_t>(cx - 8), static_cast<lv_value_precise_t>(cy)};
      t.p[2] = {static_cast<lv_value_precise_t>(cx), static_cast<lv_value_precise_t>(cy)};
      lv_draw_triangle(layer, &t);
      lv_area_t stemIn = {cx - 5, cy, cx - 3, cy + 5};
      r.bg_color = t.bg_color;
      lv_draw_rect(layer, &r, &stemIn);
      t.bg_color = lv_color_hex(kSubOut);
      t.p[0] = {static_cast<lv_value_precise_t>(cx + 4), static_cast<lv_value_precise_t>(cy + 6)};
      t.p[1] = {static_cast<lv_value_precise_t>(cx), static_cast<lv_value_precise_t>(cy)};
      t.p[2] = {static_cast<lv_value_precise_t>(cx + 8), static_cast<lv_value_precise_t>(cy)};
      lv_draw_triangle(layer, &t);
      lv_area_t stemOut = {cx + 3, cy - 5, cx + 5, cy};
      r.bg_color = t.bg_color;
      lv_draw_rect(layer, &r, &stemOut);
      break;
    }
  }
}

void build_summary(DetailState* st) {
  lv_obj_t* page = st->pages[static_cast<int>(DetailTab::Summary)];
  DetailParts& p = st->parts;
  lv_obj_clean(page);
  p.eventMinutes.clear();
  p.eventPlayers.clear();

  std::vector<const MatchEvent*> events;
  for (const MatchEvent& ev : st->data.events) events.push_back(&ev);
  std::stable_sort(events.begin(), events.end(), [](const MatchEvent* a, const MatchEvent* b) {
    return minute_sort_key(a->minute) < minute_sort_key(b->minute);
  });

  p.summaryEmpty = nullptr;
  if (events.empty()) {
    std::string hint = kickoff_hint(st);
    if (st->data.match.state == MatchState::Live) {
      hint = "Goals, cards and substitutions show up here";
    }
    p.summaryEmpty = empty_message(page, "No events yet", hint);
    return;
  }

  lv_obj_t* list = plain(page);
  lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(list, kPitchInset, 0);
  lv_obj_set_style_pad_top(list, 2, 0);
  lv_obj_set_style_pad_bottom(list, 8, 0);
  lv_obj_set_style_pad_row(list, 5, 0);

  const MatchSummary& m = st->data.match;
  for (const MatchEvent* ev : events) {
    const bool home = ev->team == Side::Home;
    // Home rows carry the team edge on the left, away rows on the right,
    // echoing ARS left / CHE right in the header.
    lv_obj_t* row = panel(list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(row, side_color(st, ev->team), 0);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_side(row, home ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_pad_ver(row, 5, 0);
    // The 3 px edge counts as border: keep the columns aligned across sides.
    lv_obj_set_style_pad_left(row, home ? 7 : 10, 0);
    lv_obj_set_style_pad_right(row, home ? 10 : 7, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* minute = label(row, &bigshoulders_16, theme().chalk, ev->minute.c_str());
    lv_obj_set_width(minute, 40);
    lv_label_set_long_mode(minute, LV_LABEL_LONG_CLIP);
    p.eventMinutes.push_back(minute);

    lv_obj_t* icon = plain(row);
    lv_obj_set_size(icon, 16, 14);
    lv_obj_add_event_cb(icon, draw_event_icon, LV_EVENT_DRAW_MAIN,
                        reinterpret_cast<void*>(static_cast<intptr_t>(ev->type)));

    lv_obj_t* texts = plain(row);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* name = label(texts, &hanken_12, theme().chalk, ev->player.c_str());
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    p.eventPlayers.push_back(name);
    if (!ev->detail.empty()) {
      lv_obj_t* more = label(texts, &hanken_10, theme().dim, ev->detail.c_str());
      lv_obj_set_width(more, LV_PCT(100));
      lv_label_set_long_mode(more, LV_LABEL_LONG_DOT);
    }

    label(row, &hanken_10, theme().dim, (home ? m.home.abbr : m.away.abbr).c_str());
  }
}

// ---------------------------------------------------------------- stats

void build_stats(DetailState* st) {
  lv_obj_t* page = st->pages[static_cast<int>(DetailTab::Stats)];
  DetailParts& p = st->parts;
  lv_obj_clean(page);
  p.statRows.clear();

  p.statsEmpty = nullptr;
  if (st->data.stats.empty()) {
    const bool pre = st->data.match.state == MatchState::Pre;
    p.statsEmpty = empty_message(page, "Stats are not available for this match",
                                 pre ? "They usually appear once the match starts" : "");
    return;
  }

  lv_obj_t* list = plain(page);
  lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(list, kPitchInset, 0);
  lv_obj_set_style_pad_top(list, 2, 0);
  lv_obj_set_style_pad_bottom(list, 8, 0);
  lv_obj_set_style_pad_row(list, 5, 0);

  constexpr int32_t kRowWidth = 240 - 2 * kPitchInset;
  constexpr int32_t kRowPad = 10;
  constexpr int32_t kBarWidth = kRowWidth - 2 * kRowPad;
  for (const StatRow& s : st->data.stats) {
    lv_obj_t* row = panel(list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(row, kRowPad, 0);
    lv_obj_set_style_pad_top(row, 5, 0);
    lv_obj_set_style_pad_bottom(row, 7, 0);
    p.statRows.push_back(row);

    lv_obj_t* homeValue = label(row, &bigshoulders_16, theme().chalk, s.home.c_str());
    lv_obj_align(homeValue, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t* name = label(row, &hanken_10, theme().dim, s.label.c_str());
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_t* awayValue = label(row, &bigshoulders_16, theme().chalk, s.away.c_str());
    lv_obj_align(awayValue, LV_ALIGN_TOP_RIGHT, 0, 0);

    // Two-sided bar: each side's share of the total, split by a small gap.
    double h = 0;
    double a = 0;
    const int32_t barY = lv_font_get_line_height(&bigshoulders_16) + 2;
    lv_obj_t* spacer = plain(row);  // keeps the row height the same with or without a bar
    lv_obj_set_size(spacer, 1, barY + 3);
    if (!stat_number(s.home, h) || !stat_number(s.away, a)) continue;
    constexpr int32_t kGap = 3;
    int32_t homeW = (kBarWidth - kGap) / 2;
    if (h + a > 0) homeW = static_cast<int32_t>((kBarWidth - kGap) * h / (h + a) + 0.5);
    const int32_t awayW = kBarWidth - kGap - homeW;
    const bool zero = h + a <= 0;
    if (homeW > 0) {
      lv_obj_t* hb = plain(row);
      lv_obj_set_pos(hb, 0, barY);
      lv_obj_set_size(hb, homeW, 3);
      lv_obj_set_style_radius(hb, 2, 0);
      lv_obj_set_style_bg_color(hb, zero ? theme().line : st->homeColor, 0);
      lv_obj_set_style_bg_opa(hb, LV_OPA_COVER, 0);
    }
    if (awayW > 0) {
      lv_obj_t* ab = plain(row);
      lv_obj_set_pos(ab, kBarWidth - awayW, barY);
      lv_obj_set_size(ab, awayW, 3);
      lv_obj_set_style_radius(ab, 2, 0);
      lv_obj_set_style_bg_color(ab, zero ? theme().line : st->awayColor, 0);
      lv_obj_set_style_bg_opa(ab, LV_OPA_COVER, 0);
    }
  }
}

// ---------------------------------------------------------------- pitch

// Pitch markings over mowing stripes. With non-null user data it draws a
// "ghost" pitch instead: faint lines, no stripes (lineups not out yet).
void draw_field(lv_event_t* e) {
  const bool ghost = lv_event_get_user_data(e) != nullptr;
  const lv_opa_t lineOpa = ghost ? 46 : kLineOpa;
  lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int32_t w = lv_area_get_width(&a);
  const int32_t h = lv_area_get_height(&a);

  lv_draw_rect_dsc_t stripe;
  lv_draw_rect_dsc_init(&stripe);
  stripe.bg_color = lv_color_hex(kTurfLight);
  stripe.bg_opa = LV_OPA_COVER;
  for (int32_t y = 0; !ghost && y < h; y += 2 * kStripe) {
    lv_area_t band = {a.x1, a.y1 + y, a.x2, LV_MIN(a.y1 + y + kStripe - 1, a.y2)};
    lv_draw_rect(layer, &stripe, &band);
  }

  lv_draw_rect_dsc_t line;
  lv_draw_rect_dsc_init(&line);
  line.bg_color = theme().chalk;
  line.bg_opa = lineOpa;
  const int32_t midY = a.y1 + h / 2;
  lv_area_t half = {a.x1, midY - 1, a.x2, midY};
  lv_draw_rect(layer, &line, &half);

  lv_draw_rect_dsc_t outline;
  lv_draw_rect_dsc_init(&outline);
  outline.bg_opa = LV_OPA_TRANSP;
  outline.border_color = theme().chalk;
  outline.border_opa = lineOpa;
  outline.border_width = 2;

  const int32_t cx = a.x1 + w / 2;
  lv_area_t circle = {cx - kCircle / 2, midY - kCircle / 2, cx + kCircle / 2 - 1,
                      midY + kCircle / 2 - 1};
  outline.radius = LV_RADIUS_CIRCLE;
  lv_draw_rect(layer, &outline, &circle);

  outline.radius = 0;
  const int32_t bx1 = a.x1 + w / 4;
  const int32_t bx2 = a.x1 + w * 3 / 4 - 1;
  outline.border_side = static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT |
                                                      LV_BORDER_SIDE_BOTTOM);
  lv_area_t top = {bx1, a.y1, bx2, a.y1 + kBoxHeight - 1};
  lv_draw_rect(layer, &outline, &top);
  outline.border_side = static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT |
                                                      LV_BORDER_SIDE_TOP);
  lv_area_t bottom = {bx1, a.y2 - kBoxHeight + 1, bx2, a.y2};
  lv_draw_rect(layer, &outline, &bottom);

  // Touch line and goal lines, following the pitch's rounded corners.
  outline.border_side = LV_BORDER_SIDE_FULL;
  outline.radius = 10;
  lv_draw_rect(layer, &outline, &a);
}

void on_marker_delete(lv_event_t* e) { delete static_cast<MarkerInfo*>(lv_event_get_user_data(e)); }

void hide_card(DetailState* st) {
  st->openMarker = nullptr;
  if (st->parts.nameCard == nullptr) return;
  lv_anim_delete(st->parts.nameCard, nullptr);
  show(st->parts.nameCard, false);
}

void card_opa(void* obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}
void card_y(void* obj, int32_t v) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), v, 0);
}

void show_card(DetailState* st, lv_obj_t* marker) {
  const MarkerInfo* info = detail_marker(marker);
  DetailParts& p = st->parts;
  st->openMarker = marker;
  set_text(p.nameCardNumber, std::to_string(info->number));
  set_text(p.nameCardName, info->name);
  lv_obj_set_style_border_color(p.nameCard, side_color(st, info->side), 0);
  show(p.nameCard, true);

  // The card never gets wider than the pitch: long names end in dots.
  lv_obj_t* page = lv_obj_get_parent(p.nameCard);
  lv_area_t pt;
  lv_obj_get_coords(p.pitch, &pt);
  const int32_t pitchW = lv_area_get_width(&pt);
  lv_obj_set_width(p.nameCardName, LV_SIZE_CONTENT);
  lv_obj_update_layout(p.nameCard);
  const int32_t chrome = lv_obj_get_width(p.nameCard) - lv_obj_get_width(p.nameCardName);
  const int32_t maxName = pitchW - 2 * kCardMargin - chrome;
  if (lv_obj_get_width(p.nameCardName) > maxName) {
    lv_obj_set_width(p.nameCardName, LV_MAX(maxName, 20));
    lv_obj_update_layout(p.nameCard);
  }

  // Above the marker, or below it when there is no room; inside the pitch.
  lv_area_t mk;
  lv_area_t pg;
  lv_obj_get_coords(marker, &mk);
  lv_obj_get_coords(page, &pg);
  const int32_t cw = lv_obj_get_width(p.nameCard);
  const int32_t ch = lv_obj_get_height(p.nameCard);
  const int32_t minX = pt.x1 - pg.x1 + kCardMargin;
  const int32_t maxX = LV_MAX(minX, pt.x2 + 1 - pg.x1 - kCardMargin - cw);
  int32_t x = (mk.x1 + mk.x2) / 2 - pg.x1 - cw / 2;
  x = LV_CLAMP(minX, x, maxX);
  int32_t y = mk.y1 - pg.y1 - ch - 4;
  if (y < kPitchTop) y = mk.y2 - pg.y1 + 5;
  lv_obj_set_pos(p.nameCard, x, y);

  lv_anim_delete(p.nameCard, nullptr);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, p.nameCard);
  lv_anim_set_duration(&a, kCardMs);
  lv_anim_set_exec_cb(&a, card_opa);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
  lv_anim_set_exec_cb(&a, card_y);
  lv_anim_set_values(&a, y < mk.y1 - pg.y1 ? 6 : -6, 0);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_start(&a);
}

void on_marker(lv_event_t* e) {
  auto* st = static_cast<DetailState*>(lv_event_get_user_data(e));
  lv_obj_t* marker = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  if (st->openMarker == marker) {
    hide_card(st);
  } else {
    show_card(st, marker);
  }
}

void on_dismiss(lv_event_t* e) { hide_card(static_cast<DetailState*>(lv_event_get_user_data(e))); }

// The shirt number gives way to the photo once one is shown.
bool sync_photos(DetailState* st) {
  bool changed = false;
  for (lv_obj_t* m : st->photoMarkers) {
    const MarkerInfo* info = detail_marker(m);
    const bool hasPhoto = lv_image_get_src(info->photo) != nullptr;
    if (hasPhoto == lv_obj_has_flag(info->numberLabel, LV_OBJ_FLAG_HIDDEN)) continue;
    show(info->numberLabel, !hasPhoto);
    changed = true;
  }
  return changed;
}

// Row of each player (0 = nearest its own goal) and the number of rows.
// Players whose y values are within kRowTolerance of the previous one in y
// order share a row. Only the order of the rows matters, not their spacing.
std::vector<int> lineup_rows(const Lineup& lineup, int& rows) {
  const size_t n = lineup.players.size();
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return lineup.players[a].y < lineup.players[b].y;
  });
  std::vector<int> row(n, 0);
  rows = 0;
  float last = 0.0f;
  for (size_t k = 0; k < n; ++k) {
    const float y = lineup.players[order[k]].y;
    if (k == 0 || y - last > kRowTolerance) ++rows;
    row[order[k]] = rows - 1;
    last = y;
  }
  return row;
}

// Marker layout for one team: `size` px circles, rows `spacing` px apart
// (top of the box, from its own goal line).
struct RowLayout {
  int32_t size = kMarkerMax;
  int32_t spacing = 0;
};

// Rows spread evenly over the team's half: the first row touches its goal
// line, the last one the halfway line, and adjacent rows are at least one
// marker apart. `size` is shared by both teams so markers look alike.
RowLayout row_layout(int rows, int32_t size, int32_t half) {
  RowLayout r;
  r.size = size;
  r.spacing = rows > 1 ? (half - size) / (rows - 1) : 0;
  return r;
}

lv_obj_t* add_marker(DetailState* st, lv_obj_t* pitch, const LineupPlayer& pl, Side side,
                     int row, const RowLayout& layout, int32_t w, int32_t h) {
  // Home defends the bottom goal, away the top one. The away team faces the
  // viewer, so its left (small x) is the screen's right.
  const int32_t size = layout.size;
  const float x = LV_CLAMP(0.0f, pl.x, 1.0f);
  const float across = side == Side::Home ? x : 1.0f - x;
  const int32_t cx = static_cast<int32_t>(across * static_cast<float>(w) + 0.5f);
  const int32_t left = LV_CLAMP(1, cx - size / 2, w - size - 1);
  const int32_t fromGoal = row * layout.spacing;
  const int32_t top = side == Side::Home ? h - fromGoal - size : fromGoal;

  const lv_color_t color = side == Side::Home ? st->homeMarker : st->awayMarker;
  lv_obj_t* m = plain(pitch);
  lv_obj_set_pos(m, left, top);
  lv_obj_set_size(m, size, size);
  lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(m, color, 0);
  lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(m, theme().chalk, 0);
  lv_obj_set_style_border_width(m, kRing, 0);
  lv_obj_set_style_border_post(m, true, 0);  // the ring stays above the photo
  lv_obj_set_style_shadow_color(m, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(m, 5, 0);
  lv_obj_set_style_shadow_offset_y(m, 2, 0);
  lv_obj_set_style_shadow_opa(m, 102, 0);
  lv_obj_set_style_clip_corner(m, true, 0);
  lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(m, 3);

  auto* info = new MarkerInfo();
  info->side = side;
  info->number = pl.number;
  info->name = pl.name;
  lv_obj_set_user_data(m, info);
  lv_obj_add_event_cb(m, on_marker_delete, LV_EVENT_DELETE, info);
  lv_obj_add_event_cb(m, on_marker, LV_EVENT_CLICKED, st);

  // Big Shoulders 16 is the smallest generated size; its digits still fit a
  // 20 px marker (the label box overflows the ring and is clipped by it).
  info->numberLabel = label(m, &bigshoulders_16, text_on(color), std::to_string(pl.number).c_str());
  lv_obj_center(info->numberLabel);

  if (!pl.photo.empty()) {
    // Cropped by the round marker, head and shoulders at the top: the photo
    // is drawn 5/3 of the marker size.
    const int32_t photo = size * 5 / 3;
    info->photo = lv_image_create(m);
    lv_obj_remove_flag(info->photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(info->photo, photo, photo);
    // Child positions start inside the ring: undo that to centre on the marker.
    lv_obj_set_pos(info->photo, (size - photo) / 2 - kRing, -kRing);
    image_stretch_when_loaded(info->photo);
    st->photoMarkers.push_back(m);
    image_load_into(info->photo, *st->images, sized(pl.photo, kPhotoRequest, kPhotoRequest));
  }
  return m;
}

// Small "CHE 4-2-3-1" tag in the corner by a team's goal line.
lv_obj_t* formation_tag(lv_obj_t* pitch, const std::string& abbr, const std::string& formation,
                        bool top) {
  if (formation.empty()) return nullptr;
  lv_obj_t* tag = label(pitch, &hanken_10, theme().chalk, (abbr + " " + formation).c_str());
  lv_obj_set_style_bg_color(tag, theme().turf, 0);
  lv_obj_set_style_bg_opa(tag, 150, 0);
  lv_obj_set_style_radius(tag, 4, 0);
  lv_obj_set_style_pad_hor(tag, 4, 0);
  lv_obj_align(tag, top ? LV_ALIGN_TOP_LEFT : LV_ALIGN_BOTTOM_LEFT, 4, top ? 3 : -3);
  return tag;
}

void build_pitch(DetailState* st) {
  lv_obj_t* page = st->pages[static_cast<int>(DetailTab::Pitch)];
  DetailParts& p = st->parts;
  const MatchDetail& d = st->data;
  hide_card(st);
  lv_obj_clean(page);
  st->photoMarkers.clear();
  p.halfNote = nullptr;

  const bool homeOut = !d.home.players.empty();
  const bool awayOut = !d.away.players.empty();

  p.homeFormation = nullptr;
  p.awayFormation = nullptr;

  // One subtitle line: the chip team's formation in words. Both formations
  // are tagged inside the pitch, by each team's goal.
  const Side chip = chip_side(d, st->ctx);
  const std::string blurb =
      formation_blurb(chip == Side::Home ? d.home.formation : d.away.formation);
  const std::string& chipAbbr = chip == Side::Home ? d.match.home.abbr : d.match.away.abbr;
  p.blurb = label(page, &hanken_10, theme().dim,
                  blurb.empty() ? "" : (chipAbbr + ": " + blurb).c_str());
  lv_obj_set_pos(p.blurb, kSide, -1);

  lv_obj_update_layout(st->root);
  const int32_t w = 240 - 2 * kPitchInset;
  const int32_t h = LV_MAX(lv_obj_get_content_height(page) - kPitchTop - kPitchBottom, 120);

  p.pitch = plain(page);
  lv_obj_set_pos(p.pitch, kPitchInset, kPitchTop);
  lv_obj_set_size(p.pitch, w, h);
  lv_obj_set_style_radius(p.pitch, 10, 0);
  lv_obj_set_style_clip_corner(p.pitch, true, 0);
  lv_obj_set_style_bg_color(p.pitch, lv_color_hex(kTurfDark), 0);
  lv_obj_set_style_bg_opa(p.pitch, LV_OPA_COVER, 0);
  lv_obj_add_flag(p.pitch, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(p.pitch, on_dismiss, LV_EVENT_CLICKED, st);

  lv_obj_t* field = plain(p.pitch);
  lv_obj_set_size(field, w, h);
  lv_obj_add_event_cb(field, draw_field, LV_EVENT_DRAW_MAIN, nullptr);

  // Neither lineup out: a faded pitch behind the message instead of players.
  p.pitchMessage = label(page, &hanken_12, theme().chalk,
                         "Lineups appear about an hour before kickoff");
  lv_obj_set_width(p.pitchMessage, 170);
  lv_label_set_long_mode(p.pitchMessage, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(p.pitchMessage, LV_TEXT_ALIGN_CENTER, 0);
  if (!homeOut && !awayOut) {
    show(p.pitch, false);
    lv_obj_t* ghost = panel(page);
    lv_obj_set_pos(ghost, kPitchInset, kPitchTop);
    lv_obj_set_size(ghost, w, h);
    lv_obj_t* lines = plain(ghost);
    lv_obj_set_size(lines, w, h);
    lv_obj_add_event_cb(lines, draw_field, LV_EVENT_DRAW_MAIN, ghost);
    lv_obj_move_foreground(p.pitchMessage);
    lv_obj_align(p.pitchMessage, LV_ALIGN_TOP_MID, 0, kPitchTop + h / 2 - 18);
  } else {
    show(p.pitchMessage, false);
    int homeRows = 0;
    int awayRows = 0;
    const std::vector<int> homeRow = lineup_rows(d.home, homeRows);
    const std::vector<int> awayRow = lineup_rows(d.away, awayRows);
    const int32_t half = h / 2;
    const int32_t size =
        LV_CLAMP(kMarkerMin, half / LV_MAX(LV_MAX(homeRows, awayRows), 1), kMarkerMax);
    const RowLayout homeLayout = row_layout(homeRows, size, half);
    const RowLayout awayLayout = row_layout(awayRows, size, half);
    for (size_t i = 0; i < d.home.players.size(); ++i) {
      add_marker(st, p.pitch, d.home.players[i], Side::Home, homeRow[i], homeLayout, w, h);
    }
    for (size_t i = 0; i < d.away.players.size(); ++i) {
      add_marker(st, p.pitch, d.away.players[i], Side::Away, awayRow[i], awayLayout, w, h);
    }
    if (homeOut) {
      p.homeFormation = formation_tag(p.pitch, d.match.home.abbr, d.home.formation, false);
    }
    if (awayOut) {
      p.awayFormation = formation_tag(p.pitch, d.match.away.abbr, d.away.formation, true);
    }
    if (!homeOut || !awayOut) {
      const std::string& abbr = homeOut ? d.match.away.abbr : d.match.home.abbr;
      p.halfNote = label(p.pitch, &hanken_10, theme().chalk,
                         (abbr + " lineup not published yet").c_str());
      lv_obj_set_style_bg_color(p.halfNote, theme().turf, 0);
      lv_obj_set_style_bg_opa(p.halfNote, 180, 0);
      lv_obj_set_style_radius(p.halfNote, 6, 0);
      lv_obj_set_style_pad_hor(p.halfNote, 7, 0);
      lv_obj_set_style_pad_ver(p.halfNote, 2, 0);
      lv_obj_align(p.halfNote, LV_ALIGN_CENTER, 0, homeOut ? -h / 4 : h / 4);
    }
  }

  // Name card, above the pitch in the page.
  p.nameCard = plain(page);
  lv_obj_set_size(p.nameCard, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(p.nameCard, theme().panel, 0);
  lv_obj_set_style_bg_opa(p.nameCard, 245, 0);
  lv_obj_set_style_radius(p.nameCard, 8, 0);
  lv_obj_set_style_border_width(p.nameCard, 3, 0);
  lv_obj_set_style_border_side(p.nameCard, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_shadow_color(p.nameCard, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(p.nameCard, 12, 0);
  lv_obj_set_style_shadow_offset_y(p.nameCard, 3, 0);
  lv_obj_set_style_shadow_opa(p.nameCard, 140, 0);
  lv_obj_set_style_pad_left(p.nameCard, 9, 0);
  lv_obj_set_style_pad_right(p.nameCard, 10, 0);
  lv_obj_set_style_pad_ver(p.nameCard, 5, 0);
  lv_obj_set_style_pad_column(p.nameCard, 7, 0);
  lv_obj_set_flex_flow(p.nameCard, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.nameCard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(p.nameCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(p.nameCard, on_dismiss, LV_EVENT_CLICKED, st);
  p.nameCardNumber = label(p.nameCard, &bigshoulders_16, theme().amber);
  p.nameCardName = label(p.nameCard, &hanken_12, theme().chalk);
  lv_label_set_long_mode(p.nameCardName, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(p.nameCard, LV_OBJ_FLAG_HIDDEN);

  sync_photos(st);
}

// ---------------------------------------------------------------- lifecycle

std::string chip_formation(const DetailState* st) {
  const MatchDetail& d = st->data;
  return chip_side(d, st->ctx) == Side::Home ? d.home.formation : d.away.formation;
}

void on_delete(lv_event_t* e) { delete static_cast<DetailState*>(lv_event_get_user_data(e)); }

DetailState* make_root(lv_obj_t* parent, Kind kind) {
  auto* st = new DetailState();
  st->kind = kind;
  st->root = plain(parent);
  lv_obj_set_size(st->root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(st->root, theme().turf, 0);
  lv_obj_set_style_bg_opa(st->root, LV_OPA_COVER, 0);
  lv_obj_set_user_data(st->root, st);
  lv_obj_add_event_cb(st->root, on_delete, LV_EVENT_DELETE, st);
  return st;
}

void on_retry(lv_event_t* e) {
  auto* st = static_cast<DetailState*>(lv_event_get_user_data(e));
  if (st->onRetry) st->onRetry();
}

}  // namespace

// ---------------------------------------------------------------- public

lv_obj_t* detail_create(lv_obj_t* parent, const MatchDetail& detail, ImageProvider& images,
                        std::function<void()> onBack, const DetailContext& ctx) {
  DetailState* st = make_root(parent, Kind::Detail);
  st->images = &images;
  st->onBack = std::move(onBack);
  st->data = detail;
  st->ctx = ctx;
  compute_colors(st);

  build_header(st);
  build_tab_row(st, true);

  lv_obj_t* tv = lv_tabview_create(st->root);
  st->parts.tabview = tv;
  lv_obj_set_pos(tv, 0, kBodyY);
  lv_obj_update_layout(st->root);
  lv_obj_set_size(tv, LV_PCT(100), LV_MAX(lv_obj_get_height(st->root) - kBodyY, 120));
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(tv, 0, 0);
  lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
  lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* content = lv_tabview_get_content(tv);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  // No rubber band at the ends: a swipe right on the first sub-tab reaches
  // the overlay as a back gesture.
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
  for (int i = 0; i < 3; ++i) {
    st->pages[i] = lv_tabview_add_tab(tv, kTabNames[i]);
    style_page(st->pages[i], i != static_cast<int>(DetailTab::Pitch));
  }
  lv_obj_add_event_cb(tv, on_tab_changed, LV_EVENT_VALUE_CHANGED, st);

  apply_header(st, &st->data.match, chip_formation(st));
  build_summary(st);
  build_pitch(st);
  build_stats(st);

  const bool lineups = !detail.home.players.empty() || !detail.away.players.empty();
  const DetailTab first =
      detail.match.state == MatchState::Live && lineups ? DetailTab::Pitch : DetailTab::Summary;
  lv_tabview_set_active(tv, static_cast<uint32_t>(first), LV_ANIM_OFF);
  sync_tabs(st, false);
  return st->root;
}

bool detail_update(lv_obj_t* screen, const MatchDetail& detail, const DetailContext& ctx) {
  const DetailParts* parts = detail_parts(screen);
  if (parts == nullptr) return false;
  DetailState* st = state_of(screen);
  if (st->kind != Kind::Detail) return false;

  MatchDetail& old = st->data;
  const bool teams = !same(old.match.home, detail.match.home) ||
                     !same(old.match.away, detail.match.away);
  const bool header = teams || !same(old.match, detail.match) ||
                      st->ctx.tzOffset != ctx.tzOffset || st->ctx.favouriteId != ctx.favouriteId;
  const bool events = teams || !same_list(old.events, detail.events);
  const bool lineups = teams || !same(old.home, detail.home) || !same(old.away, detail.away) ||
                       st->ctx.favouriteId != ctx.favouriteId;
  const bool stats = teams || !same_list(old.stats, detail.stats);

  bool changed = header || events || lineups || stats;
  if (changed) {
    st->data = detail;
    st->ctx = ctx;
    compute_colors(st);
  }
  if (header || lineups) apply_header(st, &st->data.match, chip_formation(st));
  if (events) build_summary(st);
  if (lineups) build_pitch(st);
  if (stats) build_stats(st);
  if (sync_photos(st)) changed = true;
  return changed;
}

lv_obj_t* detail_create_placeholder(lv_obj_t* parent, const MatchSummary* match,
                                    DetailPlaceholder kind, const DetailContext& ctx,
                                    std::function<void()> onBack, std::function<void()> onRetry) {
  DetailState* st = make_root(parent, Kind::Placeholder);
  st->onBack = std::move(onBack);
  st->onRetry = std::move(onRetry);
  st->ctx = ctx;
  if (match != nullptr) st->data.match = *match;
  DetailParts& p = st->parts;
  const Palette& pal = theme();

  build_header(st);
  build_tab_row(st, false);
  apply_header(st, match, "");
  if (match == nullptr) {
    // Unknown match: a bar where the score will be.
    lv_obj_t* bar = panel(st->root);
    lv_obj_set_pos(bar, kSide, kTitleY + 5);
    lv_obj_set_size(bar, 96, 16);
    lv_obj_set_style_radius(bar, 4, 0);
  }

  const bool loading = kind == DetailPlaceholder::Loading;
  const char* text =
      loading ? "Loading match..." : "Could not load this match. Check the connection.";
  p.message = label(st->root, &hanken_12, loading ? pal.dim : pal.chalk, text);
  lv_obj_set_width(p.message, 200);
  lv_label_set_long_mode(p.message, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(p.message, LV_TEXT_ALIGN_CENTER, 0);

  p.retry = plain(st->root);
  lv_obj_set_size(p.retry, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(p.retry, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(p.retry, pal.panel, 0);
  lv_obj_set_style_bg_opa(p.retry, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(p.retry, pal.amber, 0);
  lv_obj_set_style_border_width(p.retry, 1, 0);
  lv_obj_set_style_pad_hor(p.retry, 18, 0);
  lv_obj_set_style_pad_ver(p.retry, 7, 0);
  lv_obj_add_flag(p.retry, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(p.retry, 6);
  lv_obj_add_event_cb(p.retry, on_retry, LV_EVENT_CLICKED, st);
  label(p.retry, &hanken_12, pal.amber, "Try again");

  if (loading) {
    // Skeleton of the Summary rows, so the frame reads as "coming".
    show(p.retry, false);
    lv_obj_align(p.message, LV_ALIGN_TOP_MID, 0, kBodyY + 4);
    const int32_t widths[] = {224, 224, 224, 224};
    const uint8_t fades[] = {255, 190, 125, 60};
    for (int i = 0; i < 4; ++i) {
      lv_obj_t* row = panel(st->root);
      lv_obj_set_pos(row, kPitchInset, kBodyY + 26 + i * 40);
      lv_obj_set_size(row, widths[i], 34);
      lv_obj_set_style_opa(row, fades[i], 0);
      lv_obj_t* a = plain(row);
      lv_obj_set_style_bg_color(a, pal.line, 0);
      lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(a, 3, 0);
      lv_obj_set_pos(a, 12, 12);
      lv_obj_set_size(a, 22, 10);
      lv_obj_t* b = plain(row);
      lv_obj_set_style_bg_color(b, pal.line, 0);
      lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(b, 3, 0);
      lv_obj_set_pos(b, 46, 12);
      lv_obj_set_size(b, 90 + (i % 2) * 30, 10);
    }
  } else {
    lv_obj_align(p.message, LV_ALIGN_TOP_MID, 0, kBodyY + 52);
    lv_obj_align_to(p.retry, p.message, LV_ALIGN_OUT_BOTTOM_MID, 0, 14);
  }
  return st->root;
}

void detail_select_tab(lv_obj_t* screen, DetailTab tab, bool animate) {
  if (detail_parts(screen) == nullptr) return;
  DetailState* st = state_of(screen);
  if (st->parts.tabview == nullptr) return;
  hide_card(st);
  lv_tabview_set_active(st->parts.tabview, static_cast<uint32_t>(tab),
                        animate ? LV_ANIM_ON : LV_ANIM_OFF);
  sync_tabs(st, animate);
}

DetailTab detail_active_tab(lv_obj_t* screen) {
  if (detail_parts(screen) == nullptr) return DetailTab::Summary;
  const DetailState* st = state_of(screen);
  if (st->parts.tabview == nullptr) return DetailTab::Summary;
  return static_cast<DetailTab>(lv_tabview_get_tab_active(st->parts.tabview));
}

const DetailParts* detail_parts(lv_obj_t* screen) {
  if (screen == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(screen); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(screen, i)) == on_delete) {
      return &state_of(screen)->parts;
    }
  }
  return nullptr;
}

const MarkerInfo* detail_marker(lv_obj_t* obj) {
  if (obj == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
    lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
    if (lv_event_dsc_get_cb(dsc) == on_marker_delete) {
      return static_cast<const MarkerInfo*>(lv_event_dsc_get_user_data(dsc));
    }
  }
  return nullptr;
}
