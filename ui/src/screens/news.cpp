#include "screens/news.h"

#include <string.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fonts.h"
#include "format.h"
#include "theme.h"

// Geometry follows screen 4 of the mockup (240x320). The screen lives in the
// News tab, which ends above the tab bar, so the scrolling list ends there too.
// The lead picture starts at the very top, behind the status row, as in the
// mockup; the status row gets a solid background once the list scrolls.
namespace {

using widgets::label;
using widgets::plain;
using widgets::set_text;
using widgets::show;
using widgets::StatusInfo;

constexpr int32_t kScreenWidth = 240;
constexpr int32_t kStatusHeight = 26;
constexpr int32_t kLeadHeight = 158;
constexpr int32_t kLeadTextLeft = 12;
constexpr int32_t kLeadTextWidth = 216;
constexpr int32_t kLeadTextBottom = 10;
constexpr int32_t kLeadMaxLines = 3;
constexpr int32_t kLeadLineSpace = -6;  // mockup line-height .95
constexpr int32_t kLeadTimeGap = 2;
constexpr int32_t kSideMargin = 10;
constexpr int32_t kContentWidth = kScreenWidth - 2 * kSideMargin;
constexpr int32_t kListGap = 6;
constexpr int32_t kListPadBottom = 10;
constexpr int32_t kTileGap = 6;
constexpr int32_t kTileWidth = (kContentWidth - kTileGap) / 2;
constexpr int32_t kTilePadX = 6;
constexpr int32_t kTilePadTop = 6;
constexpr int32_t kTilePadBottom = 8;
constexpr int32_t kTileRadius = 10;
constexpr int32_t kTileSectionPad = 2;  // lead -> tiles -> rows is 8 px, rows are 6 px apart
constexpr int32_t kSuffixGap = 4;
constexpr int32_t kSuffixWidth = 12 + kSuffixGap;  // room kept for "FT" next to a result
constexpr int32_t kRowHeight = 58;
constexpr int32_t kRowPad = 8;
constexpr int32_t kRowRadius = 12;
constexpr int32_t kThumbSize = 40;
constexpr int32_t kThumbRadius = 9;
constexpr int32_t kThumbGap = 10;
constexpr int32_t kRowTextX = kRowPad + kThumbSize + kThumbGap;
constexpr int32_t kRowTextWidth = kContentWidth - kRowTextX - kRowPad;
constexpr int32_t kRowTextTop = 7;
constexpr int32_t kRowLines = 2;
constexpr int32_t kRowTimeGap = 1;
constexpr int32_t kEmptyCardWidth = 196;
constexpr size_t kMaxArticles = 6;  // the lead and five rows
constexpr lv_opa_t kPlaceholderOpa = 24;

// The lead headline: big when it fits three lines, else the smaller face
// (real headlines can run past 70 characters).
const lv_font_t* const kLeadFont = &bigshoulders_22;
const lv_font_t* const kLeadSmallFont = &bigshoulders_16;
constexpr int32_t kLeadSmallLineSpace = -3;
const lv_font_t* const kValueFont = &bigshoulders_22;
const lv_font_t* const kValueSmallFont = &bigshoulders_16;

enum class Mode { Connecting, Empty, List };

// ---------------------------------------------------------------- view

struct ArticleView {
  std::string id;
  std::string headline;
  std::string time;
  std::string image;

  bool operator==(const ArticleView& o) const {
    return id == o.id && headline == o.headline && time == o.time && image == o.image;
  }
  bool operator!=(const ArticleView& o) const { return !(*this == o); }
};

struct TileView {
  std::string caption;
  std::string value;
  std::string suffix;  // "FT" or empty
  bool small = false;  // value in the smaller font
  bool tight = false;  // and with the letters closer together

  bool operator==(const TileView& o) const {
    return caption == o.caption && value == o.value && suffix == o.suffix && small == o.small &&
           tight == o.tight;
  }
  bool operator!=(const TileView& o) const { return !(*this == o); }
};

struct View {
  Mode mode = Mode::Connecting;
  StatusInfo status;
  uint32_t accent = 0;
  ArticleView lead;
  TileView last;
  TileView next;
  std::vector<ArticleView> rows;

  bool operator==(const View& o) const {
    return mode == o.mode && status == o.status && accent == o.accent && lead == o.lead &&
           last == o.last && next == o.next && rows == o.rows;
  }
  bool operator!=(const View& o) const { return !(*this == o); }
};

// A picture drawn scaled up to cover its box and centred, the overflow cut
// off by the box. The box clips its children.
struct CoverImage {
  lv_obj_t* img = nullptr;
  int32_t boxW = 0;
  int32_t boxH = 0;
  const void* src = nullptr;  // source the geometry was computed for
  int32_t srcW = 0;
  int32_t srcH = 0;
};

struct Row {
  std::string id;
  NewsRowParts parts;
  CoverImage cover;
  ArticleView shown;
  bool applied = false;
};

struct NewsState {
  Store* store = nullptr;
  ImageProvider* images = nullptr;
  NewsParts parts;
  lv_obj_t* header = nullptr;
  lv_obj_t* tilesRow = nullptr;
  CoverImage leadCover;
  lv_grad_dsc_t fallback;  // the lead's accent gradient; styles point at it
  std::vector<Row> rows;
  View shown;
  bool everApplied = false;
  bool leadApplied = false;
  uint32_t shownAccent = 0;
};

int32_t text_width(const std::string& s, const lv_font_t* font, int32_t letterSpace = 0) {
  lv_point_t size;
  lv_text_get_size(&size, s.c_str(), font, letterSpace, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}

ArticleView article_view(const NewsItem& n, int64_t now, int tz) {
  ArticleView a;
  a.id = n.id;
  a.headline = n.headline;
  a.time = relative_time(now, n.published, tz);
  a.image = n.image;
  return a;
}

// The value in the big font when it fits the tile, else in the small one, else
// the small one with tighter letters ("Tomorrow 20:00").
TileView tile_view(std::string caption, std::string value, std::string suffix) {
  TileView t;
  t.caption = std::move(caption);
  t.value = std::move(value);
  t.suffix = std::move(suffix);
  const int32_t room = kTileWidth - 2 * kTilePadX - (t.suffix.empty() ? 0 : kSuffixWidth);
  t.small = text_width(t.value, kValueFont) > room;
  t.tight = t.small && text_width(t.value, kValueSmallFont) > room;
  return t;
}

TileView last_tile(const Store& store, int64_t now) {
  const std::string& fav = store.favourite();
  const MatchSummary* m = fav.empty() ? nullptr : store.last_for(fav, now);
  if (m == nullptr) return tile_view("Last result", "No result yet", "");
  const bool home = m->home.id == fav;
  const Team& opponent = home ? m->away : m->home;
  const int mine = (home ? m->homeScore : m->awayScore).value_or(0);
  const int theirs = (home ? m->awayScore : m->homeScore).value_or(0);
  return tile_view("Last: " + widgets::team_abbr(opponent),
                   std::to_string(mine) + ":" + std::to_string(theirs), "FT");
}

TileView next_tile(const Store& store, int64_t now) {
  const std::string& fav = store.favourite();
  const MatchSummary* m = fav.empty() ? nullptr : store.next_for(fav, now);
  if (m == nullptr) return tile_view("Next fixture", "No fixture yet", "");
  const Team& opponent = m->home.id == fav ? m->away : m->home;
  const int64_t local = m->kickoff + store.tz_offset();
  const int64_t days = floor_div(local, 86400) - floor_div(now + store.tz_offset(), 86400);
  std::string day;
  if (days <= 0) {
    day = "Today";
  } else if (days == 1) {
    day = "Tomorrow";
  } else if (days < 7) {
    day = weekday(local);
  } else {
    day = day_month(local);
  }
  return tile_view("Next: " + widgets::team_abbr(opponent), day + " " + hh_mm(local), "");
}

View compute_view(const Store& store, int64_t now) {
  View v;
  v.status = widgets::status_info(store, now);
  v.accent = lv_color_to_u32(theme().accent);
  if (!store.has_data()) {
    v.mode = Mode::Connecting;
    return v;
  }
  // Newest first; equal times keep their input order. An id is shown once.
  std::vector<const NewsItem*> items;
  std::unordered_set<std::string> seen;
  for (const NewsItem& n : store.news()) {
    if (seen.insert(n.id).second) items.push_back(&n);
  }
  std::stable_sort(items.begin(), items.end(),
                   [](const NewsItem* a, const NewsItem* b) { return a->published > b->published; });
  if (items.empty()) {
    v.mode = Mode::Empty;
    return v;
  }
  v.mode = Mode::List;
  const int tz = store.tz_offset();
  v.lead = article_view(*items[0], now, tz);
  for (size_t i = 1; i < items.size() && i < kMaxArticles; ++i) {
    v.rows.push_back(article_view(*items[i], now, tz));
  }
  v.last = last_tile(store, now);
  v.next = next_tile(store, now);
  return v;
}

// ---------------------------------------------------------------- pictures

// Fits the picture now shown (if any) to the box: scaled up by the smaller
// factor that covers it, centred. Cheap when nothing changed.
void cover_sync(CoverImage& c) {
  const void* src = lv_image_get_src(c.img);
  if (src == nullptr) {
    c.src = nullptr;
    return;
  }
  if (lv_image_src_get_type(src) != LV_IMAGE_SRC_VARIABLE) return;
  const lv_image_dsc_t* dsc = static_cast<const lv_image_dsc_t*>(src);
  const int32_t w = static_cast<int32_t>(dsc->header.w);
  const int32_t h = static_cast<int32_t>(dsc->header.h);
  if (w <= 0 || h <= 0) return;
  if (c.src == src && c.srcW == w && c.srcH == h) return;
  const int32_t scaleX = (c.boxW * LV_SCALE_NONE + w - 1) / w;
  const int32_t scaleY = (c.boxH * LV_SCALE_NONE + h - 1) / h;
  const int32_t scale = std::max(scaleX, scaleY);
  const int32_t scaledW = (w * scale + LV_SCALE_NONE - 1) / LV_SCALE_NONE;
  const int32_t scaledH = (h * scale + LV_SCALE_NONE - 1) / LV_SCALE_NONE;
  lv_image_set_pivot(c.img, 0, 0);
  lv_image_set_scale(c.img, static_cast<uint32_t>(scale));
  lv_obj_set_pos(c.img, -(scaledW - c.boxW) / 2, -(scaledH - c.boxH) / 2);
  c.src = src;
  c.srcW = w;
  c.srcH = h;
}

// Drops the old picture first so another article never shows a stale one. An
// empty path leaves the box with its placeholder.
void picture_load(CoverImage& c, ImageProvider& images, const std::string& path) {
  if (lv_image_get_src(c.img) != nullptr) lv_image_set_src(c.img, nullptr);
  c.src = nullptr;
  if (!path.empty()) image_load_into(c.img, images, path);
  cover_sync(c);
}

CoverImage make_cover(lv_obj_t* box, int32_t w, int32_t h) {
  CoverImage c;
  c.img = lv_image_create(box);
  c.boxW = w;
  c.boxH = h;
  lv_obj_set_pos(c.img, 0, 0);
  return c;
}

// ---------------------------------------------------------------- widgets

void on_delete(lv_event_t* e) { delete static_cast<NewsState*>(lv_event_get_user_data(e)); }

// The state of a screen made by news_create(), or nullptr for any other object.
NewsState* news_state(lv_obj_t* screen) {
  if (screen == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(screen); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(screen, i)) == on_delete) {
      return static_cast<NewsState*>(lv_obj_get_user_data(screen));
    }
  }
  return nullptr;
}

// The status row sits on the lead picture; once the list scrolls it needs its
// own background.
void on_scroll(lv_event_t* e) {
  auto* st = static_cast<NewsState*>(lv_event_get_user_data(e));
  const lv_opa_t want = lv_obj_get_scroll_y(st->parts.list) > 0 ? LV_OPA_COVER : LV_OPA_TRANSP;
  if (lv_obj_get_style_bg_opa(st->header, 0) != want) lv_obj_set_style_bg_opa(st->header, want, 0);
}

lv_grad_dsc_t scrim(lv_grad_dir_t dir, uint8_t frac0, lv_opa_t opa0, uint8_t frac1, lv_opa_t opa1) {
  lv_grad_dsc_t g;
  memset(&g, 0, sizeof(g));
  g.dir = dir;
  g.stops_count = 2;
  g.stops[0].color = theme().turf;
  g.stops[0].opa = opa0;
  g.stops[0].frac = frac0;
  g.stops[1].color = theme().turf;
  g.stops[1].opa = opa1;
  g.stops[1].frac = frac1;
  return g;
}

// A full-size child of `parent` painting `grad` (the pointer must outlive it).
lv_obj_t* shade(lv_obj_t* parent, const lv_grad_dsc_t* grad, int32_t w, int32_t h) {
  lv_obj_t* o = plain(parent);
  lv_obj_set_pos(o, 0, 0);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, theme().turf, 0);
  lv_obj_set_style_bg_grad(o, grad, 0);
  return o;
}

void create_lead(NewsState* st) {
  NewsParts& p = st->parts;
  const Palette& pal = theme();
  p.lead = plain(p.list);
  lv_obj_set_size(p.lead, kScreenWidth, kLeadHeight);
  lv_obj_set_style_bg_opa(p.lead, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(p.lead, pal.accent, 0);

  st->leadCover = make_cover(p.lead, kScreenWidth, kLeadHeight);
  p.leadImage = st->leadCover.img;

  // The mockup's :after layers: legibility from the left, fade into the page
  // at the bottom, and a little darkness behind the status row.
  static const lv_grad_dsc_t kFromLeft = scrim(LV_GRAD_DIR_HOR, 0, 191, 153, LV_OPA_TRANSP);
  static const lv_grad_dsc_t kToPage = scrim(LV_GRAD_DIR_VER, 128, LV_OPA_TRANSP, 255, LV_OPA_COVER);
  static const lv_grad_dsc_t kUnderStatus = scrim(LV_GRAD_DIR_VER, 0, 150, 255, LV_OPA_TRANSP);
  shade(p.lead, &kFromLeft, kScreenWidth, kLeadHeight);
  shade(p.lead, &kToPage, kScreenWidth, kLeadHeight);
  shade(p.lead, &kUnderStatus, kScreenWidth, kStatusHeight);

  const int32_t textHeight = kLeadMaxLines * kLeadFont->line_height +
                             (kLeadMaxLines - 1) * kLeadLineSpace + kLeadTimeGap +
                             hanken_10.line_height;
  lv_obj_t* box = plain(p.lead);
  lv_obj_set_size(box, kLeadTextWidth, textHeight);
  lv_obj_set_pos(box, kLeadTextLeft, kLeadHeight - kLeadTextBottom - textHeight);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(box, kLeadTimeGap, 0);
  p.leadTime = label(box, &hanken_10, pal.chalk);
  lv_obj_set_style_text_opa(p.leadTime, 210, 0);
  p.leadHeadline = label(box, kLeadFont, pal.chalk);
  lv_obj_set_style_text_line_space(p.leadHeadline, kLeadLineSpace, 0);
  lv_label_set_long_mode(p.leadHeadline, LV_LABEL_LONG_DOT);
  lv_obj_set_size(p.leadHeadline, kLeadTextWidth, kLeadFont->line_height);
}

NewsTileParts create_tile(lv_obj_t* parent, int32_t height) {
  const Palette& pal = theme();
  NewsTileParts t;
  t.card = plain(parent);
  lv_obj_set_size(t.card, kTileWidth, height);
  lv_obj_set_style_bg_color(t.card, pal.panel, 0);
  lv_obj_set_style_bg_opa(t.card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t.card, kTileRadius, 0);
  t.caption = label(t.card, &hanken_10, pal.dim);
  lv_obj_set_pos(t.caption, kTilePadX, kTilePadTop);
  t.value = label(t.card, kValueFont, pal.chalk);
  t.suffix = label(t.card, &hanken_10, pal.dim);
  lv_obj_add_flag(t.suffix, LV_OBJ_FLAG_HIDDEN);
  return t;
}

// Digit top and height of the big value font, relative to its label.
void value_metrics(int32_t& top, int32_t& height) {
  widgets::digit_metrics(kValueFont, top, height);
}

int32_t tile_height() {
  int32_t top = 0;
  int32_t height = 0;
  value_metrics(top, height);
  return kTilePadTop + hanken_10.line_height + 3 + height + kTilePadBottom;
}

// Vertical centre of the value's digits inside the tile.
int32_t value_centre() {
  int32_t top = 0;
  int32_t height = 0;
  value_metrics(top, height);
  return kTilePadTop + hanken_10.line_height + 3 + height / 2;
}

void apply_tile(const NewsTileParts& t, const TileView& v) {
  const lv_font_t* font = v.small ? kValueSmallFont : kValueFont;
  set_text(t.caption, v.caption);
  if (lv_obj_get_style_text_font(t.value, 0) != font) lv_obj_set_style_text_font(t.value, font, 0);
  lv_obj_set_style_text_letter_space(t.value, v.tight ? -1 : 0, 0);
  set_text(t.value, v.value);
  int32_t top = 0;
  int32_t height = 0;
  widgets::digit_metrics(font, top, height);
  lv_obj_set_pos(t.value, kTilePadX, value_centre() - top - height / 2);
  show(t.suffix, !v.suffix.empty());
  set_text(t.suffix, v.suffix);
  // "FT" sits on the value's baseline, just after it.
  lv_obj_set_pos(t.suffix, kTilePadX + text_width(v.value, font, v.tight ? -1 : 0) + kSuffixGap,
                 value_centre() + height / 2 - hanken_10.line_height + 3);
}

Row create_row(NewsState* st) {
  const Palette& pal = theme();
  Row r;
  NewsRowParts& p = r.parts;
  p.row = plain(st->parts.list);
  lv_obj_set_size(p.row, kContentWidth, kRowHeight);
  lv_obj_set_style_bg_color(p.row, pal.panel, 0);
  lv_obj_set_style_bg_opa(p.row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.row, kRowRadius, 0);

  p.thumb = plain(p.row);
  lv_obj_set_size(p.thumb, kThumbSize, kThumbSize);
  lv_obj_set_pos(p.thumb, kRowPad, (kRowHeight - kThumbSize) / 2);
  lv_obj_set_style_radius(p.thumb, kThumbRadius, 0);
  lv_obj_set_style_clip_corner(p.thumb, true, 0);
  lv_obj_set_style_bg_color(p.thumb, pal.chalk, 0);
  lv_obj_set_style_bg_opa(p.thumb, kPlaceholderOpa, 0);
  r.cover = make_cover(p.thumb, kThumbSize, kThumbSize);

  // Fixed height of exactly two lines: with LV_LABEL_LONG_DOT a label that
  // sizes itself to its content would wrap instead of ending in dots.
  p.headline = label(p.row, &hanken_12, pal.chalk);
  lv_label_set_long_mode(p.headline, LV_LABEL_LONG_DOT);
  lv_obj_set_size(p.headline, kRowTextWidth, kRowLines * hanken_12.line_height);
  lv_obj_set_pos(p.headline, kRowTextX, kRowTextTop);
  p.time = label(p.row, &hanken_10, pal.dim);
  lv_obj_set_pos(p.time, kRowTextX, kRowTextTop + kRowLines * hanken_12.line_height + kRowTimeGap);
  return r;
}

void apply_row(NewsState* st, Row& r, const ArticleView& v) {
  const bool first = !r.applied;
  // Not set_text(): a label in LV_LABEL_LONG_DOT mode edits its text in place
  // to end in dots, so comparing against it would always differ.
  if (first || v.headline != r.shown.headline) lv_label_set_text(r.parts.headline, v.headline.c_str());
  set_text(r.parts.time, v.time);
  if (first || v.image != r.shown.image) picture_load(r.cover, *st->images, v.image);
  r.shown = v;
  r.applied = true;
}

// Brings the rows in line with `v`: rows keep their widgets while their
// article id is still shown, new ones are created, others deleted, and the
// order is set (the lead and the tiles come first in the list).
void reconcile(NewsState* st, const View& v) {
  std::unordered_set<std::string> wanted;
  for (const ArticleView& a : v.rows) wanted.insert(a.id);

  std::unordered_map<std::string, size_t> have;
  std::vector<Row> kept;
  for (Row& r : st->rows) {
    if (wanted.count(r.id) != 0) {
      have[r.id] = kept.size();
      kept.push_back(std::move(r));
    } else {
      lv_obj_delete(r.parts.row);
    }
  }

  constexpr int32_t kFirstRowIndex = 2;  // after the lead and the tiles
  std::vector<Row> next;
  next.reserve(v.rows.size());
  for (size_t i = 0; i < v.rows.size(); ++i) {
    const ArticleView& a = v.rows[i];
    Row r;
    auto found = have.find(a.id);
    if (found != have.end()) {
      r = std::move(kept[found->second]);
    } else {
      r = create_row(st);
      r.id = a.id;
      r.parts.id = a.id;
    }
    const int32_t index = kFirstRowIndex + static_cast<int32_t>(i);
    if (lv_obj_get_index(r.parts.row) != index) lv_obj_move_to_index(r.parts.row, index);
    apply_row(st, r, a);
    next.push_back(std::move(r));
  }
  st->rows = std::move(next);
}

// Lines `text` needs in the lead at the given face.
int32_t lead_lines(const std::string& text, const lv_font_t* font, int32_t lineSpace) {
  const int32_t pitch = font->line_height + lineSpace;
  lv_point_t size;
  lv_text_get_size(&size, text.c_str(), font, 0, lineSpace, kLeadTextWidth, LV_TEXT_FLAG_NONE);
  // n lines are n * line_height + (n - 1) * line_space tall.
  return std::max<int32_t>(1, (size.y + lineSpace + pitch / 2) / pitch);
}

// Headline of the lead: the big face when it needs at most three lines, else
// the small one; at most three lines either way, with the label sized to
// exactly that so LV_LABEL_LONG_DOT cuts the rest.
void apply_lead_headline(NewsParts& p, const std::string& text, bool changed) {
  const bool big = lead_lines(text, kLeadFont, kLeadLineSpace) <= kLeadMaxLines;
  const lv_font_t* font = big ? kLeadFont : kLeadSmallFont;
  const int32_t space = big ? kLeadLineSpace : kLeadSmallLineSpace;
  if (lv_obj_get_style_text_font(p.leadHeadline, 0) != font) {
    lv_obj_set_style_text_font(p.leadHeadline, font, 0);
  }
  lv_obj_set_style_text_line_space(p.leadHeadline, space, 0);
  const int32_t lines = std::min<int32_t>(lead_lines(text, font, space), kLeadMaxLines);
  const int32_t height = lines * font->line_height + (lines - 1) * space;
  if (lv_obj_get_height(p.leadHeadline) != height) lv_obj_set_height(p.leadHeadline, height);
  if (changed) lv_label_set_text(p.leadHeadline, text.c_str());  // see apply_row()
}

void apply_lead(NewsState* st, const View& v) {
  NewsParts& p = st->parts;
  const bool first = !st->leadApplied;
  if (first || v.accent != st->shownAccent) {
    // Fallback while the picture is missing or loading: dark to the favourite's colour.
    memset(&st->fallback, 0, sizeof(st->fallback));
    st->fallback.dir = LV_GRAD_DIR_HOR;
    st->fallback.stops_count = 2;
    st->fallback.stops[0].color = darken(theme().accent, 140);
    st->fallback.stops[0].opa = LV_OPA_COVER;
    st->fallback.stops[0].frac = 0;
    st->fallback.stops[1].color = theme().accent;
    st->fallback.stops[1].opa = LV_OPA_COVER;
    st->fallback.stops[1].frac = 200;
    lv_obj_set_style_bg_color(p.lead, theme().accent, 0);
    lv_obj_set_style_bg_grad(p.lead, &st->fallback, 0);
    st->shownAccent = v.accent;
  }
  apply_lead_headline(p, v.lead.headline, first || v.lead.headline != st->shown.lead.headline);
  set_text(p.leadTime, v.lead.time);
  if (first || v.lead.image != st->shown.lead.image || v.lead.id != st->shown.lead.id) {
    picture_load(st->leadCover, *st->images, v.lead.image);
  }
  p.leadId = v.lead.id;
  st->leadApplied = true;
}

void apply(NewsState* st, const View& v) {
  NewsParts& p = st->parts;
  widgets::status_row_apply(p.status, "News", v.status);
  show(p.connecting, v.mode == Mode::Connecting);
  show(p.emptyCard, v.mode == Mode::Empty);
  show(p.list, v.mode == Mode::List);
  if (v.mode == Mode::List) {
    apply_lead(st, v);
    apply_tile(p.last, v.last);
    apply_tile(p.next, v.next);
  }
  reconcile(st, v);
  st->shown = v;
  st->everApplied = true;
}

}  // namespace

// ---------------------------------------------------------------- public

lv_obj_t* news_create(lv_obj_t* parent, Store& store, ImageProvider& images) {
  auto* st = new NewsState();
  st->store = &store;
  st->images = &images;
  NewsParts& p = st->parts;
  const Palette& pal = theme();

  lv_obj_t* root = plain(parent);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_user_data(root, st);
  lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, st);

  // The list fills the whole tab (it ends above the tab bar) and scrolls
  // vertically, momentum on, no scrollbar. It stays clickable so a drag that
  // starts anywhere on it scrolls.
  p.list = plain(root);
  lv_obj_add_flag(p.list, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(p.list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(p.list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(p.list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_pos(p.list, 0, 0);
  lv_obj_set_size(p.list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(p.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(p.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(p.list, kListGap, 0);
  lv_obj_set_style_pad_bottom(p.list, kListPadBottom, 0);
  lv_obj_add_flag(p.list, LV_OBJ_FLAG_HIDDEN);

  create_lead(st);

  const int32_t tileH = tile_height();
  st->tilesRow = plain(p.list);
  lv_obj_set_size(st->tilesRow, kContentWidth, tileH + 2 * kTileSectionPad);
  p.last = create_tile(st->tilesRow, tileH);
  lv_obj_set_pos(p.last.card, 0, kTileSectionPad);
  p.next = create_tile(st->tilesRow, tileH);
  lv_obj_set_pos(p.next.card, kTileWidth + kTileGap, kTileSectionPad);

  // Status row on top of the list (transparent over the picture).
  st->header = plain(root);
  lv_obj_set_pos(st->header, 0, 0);
  lv_obj_set_size(st->header, LV_PCT(100), kStatusHeight);
  lv_obj_set_style_bg_color(st->header, pal.turf, 0);
  lv_obj_set_style_bg_opa(st->header, LV_OPA_TRANSP, 0);
  p.status = widgets::status_row_create(st->header);
  lv_obj_add_event_cb(p.list, on_scroll, LV_EVENT_SCROLL, st);

  p.connecting = label(root, &bigshoulders_28, pal.chalk);
  lv_obj_add_flag(p.connecting, LV_OBJ_FLAG_FLOATING);
  lv_label_set_text(p.connecting, kNewsConnecting);
  lv_obj_align(p.connecting, LV_ALIGN_CENTER, 0, -10);

  p.emptyCard = plain(root);
  lv_obj_add_flag(p.emptyCard, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(p.emptyCard, kEmptyCardWidth, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(p.emptyCard, pal.panel, 0);
  lv_obj_set_style_bg_opa(p.emptyCard, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.emptyCard, kRowRadius, 0);
  lv_obj_set_style_pad_all(p.emptyCard, 14, 0);
  lv_obj_set_style_pad_row(p.emptyCard, 6, 0);
  lv_obj_set_flex_flow(p.emptyCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(p.emptyCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  p.emptyTitle = label(p.emptyCard, &bigshoulders_22, pal.chalk);
  p.emptyHint = label(p.emptyCard, &hanken_12, pal.dim);
  for (lv_obj_t* l : {p.emptyTitle, p.emptyHint}) {
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  }
  lv_label_set_text(p.emptyTitle, kNewsEmptyTitle);
  lv_label_set_text(p.emptyHint, kNewsEmptyHint);
  lv_obj_align(p.emptyCard, LV_ALIGN_CENTER, 0, -10);
  lv_obj_add_flag(p.emptyCard, LV_OBJ_FLAG_HIDDEN);

  // Until the first refresh the screen reads "Connecting...".
  widgets::status_row_apply(p.status, "News", StatusInfo{});
  return root;
}

void news_refresh(lv_obj_t* screen, int64_t now) {
  NewsState* st = news_state(screen);
  if (st == nullptr) return;
  cover_sync(st->leadCover);
  for (Row& r : st->rows) cover_sync(r.cover);
  const View v = compute_view(*st->store, now);
  if (st->everApplied && v == st->shown) return;
  apply(st, v);
}

const NewsParts* news_parts(lv_obj_t* screen) {
  NewsState* st = news_state(screen);
  return st != nullptr ? &st->parts : nullptr;
}

std::vector<NewsRowParts> news_rows(lv_obj_t* screen) {
  std::vector<NewsRowParts> out;
  const NewsState* st = news_state(screen);
  if (st == nullptr) return out;
  for (const Row& r : st->rows) out.push_back(r.parts);
  return out;
}
