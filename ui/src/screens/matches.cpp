#include "screens/matches.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "fonts.h"
#include "format.h"
#include "theme.h"

// Geometry follows screen 3 of the mockup (240x320). The screen lives in the
// Matches tab, which ends above the tab bar, so the list ends there too.
namespace {

using widgets::dot;
using widgets::label;
using widgets::plain;
using widgets::set_text;
using widgets::show;
using widgets::StatusInfo;

constexpr int32_t kStatusHeight = 26;  // mockup: list starts at y = 26
constexpr int32_t kSideMargin = 8;
constexpr int32_t kRowHeight = 42;
constexpr int32_t kRowPad = 8;
constexpr int32_t kRowGap = 5;
constexpr int32_t kRowRadius = 10;
constexpr int32_t kCrestSize = 24;
constexpr int32_t kCrestGap = 6;
constexpr int32_t kAbbrWidth = 52;
constexpr int32_t kCentreWidth = 54;
constexpr int32_t kEdgeWidth = 3;
constexpr int32_t kEdgeInset = 6;  // keeps the bar clear of the rounded corners
constexpr int32_t kFavDotSize = 5;
constexpr int32_t kFavDotTop = 2;  // above the crest (which starts at y 9), 2 px clear
constexpr int32_t kHeaderPadLeft = 4;  // plus the list's 8 px: the mockup's 12
constexpr int32_t kHeaderPadTop = 4;   // on top of the row gap
constexpr int32_t kListPadBottom = 10;
constexpr int32_t kEmptyCardWidth = 196;
constexpr uint32_t kLiveRowColor = 0x1A2410;  // mockup .m.lv

const lv_font_t* const kCentreFont = &bigshoulders_22;
const lv_font_t* const kCentreSmallFont = &bigshoulders_16;

enum class Mode { Connecting, Empty, List };

// What one match row shows.
struct RowView {
  std::string homeAbbr;
  std::string awayAbbr;
  std::string homeCrest;
  std::string awayCrest;
  MatchState state = MatchState::Pre;
  std::string centre;
  std::string sub;
  bool smallCentre = false;
  bool fav = false;
  bool favHome = false;  // the favourite plays at home: the dot sits over the home crest

  bool operator==(const RowView& o) const {
    return homeAbbr == o.homeAbbr && awayAbbr == o.awayAbbr && homeCrest == o.homeCrest &&
           awayCrest == o.awayCrest && state == o.state && centre == o.centre && sub == o.sub &&
           smallCentre == o.smallCentre && fav == o.fav && favHome == o.favHome;
  }
  bool operator!=(const RowView& o) const { return !(*this == o); }
};

// One entry of the list. `key` identifies the widget across refreshes: the
// day for a header, the match id for a row.
struct ItemView {
  std::string key;
  bool header = false;
  std::string text;  // header text
  std::string matchId;
  RowView row;
};

struct View {
  Mode mode = Mode::Connecting;
  StatusInfo status;
  std::vector<ItemView> items;

  bool operator==(const View& o) const {
    if (mode != o.mode || status != o.status || items.size() != o.items.size()) return false;
    for (size_t i = 0; i < items.size(); ++i) {
      const ItemView& a = items[i];
      const ItemView& b = o.items[i];
      if (a.key != b.key || a.header != b.header || a.text != b.text || a.matchId != b.matchId ||
          a.row != b.row) {
        return false;
      }
    }
    return true;
  }
  bool operator!=(const View& o) const { return !(*this == o); }
};

struct Item {
  std::string key;
  bool header = false;
  lv_obj_t* obj = nullptr;  // header label or row
  MatchesRowParts parts{};
  RowView shown;
  bool applied = false;
};

// Everything the list (not the status row) is computed from besides the
// Store's match list: when none of it changed, the rows cannot have changed.
// Computing them takes about 17 ms for 62 matches on the board, too much for
// every one-second refresh.
struct ItemsKey {
  uint32_t version = 0;
  std::string favourite;
  int tz = 0;
  int64_t today = 0;
  bool operator==(const ItemsKey& o) const {
    return version == o.version && favourite == o.favourite && tz == o.tz && today == o.today;
  }
  bool operator!=(const ItemsKey& o) const { return !(*this == o); }
};

ItemsKey items_key(const Store& store, int64_t now) {
  ItemsKey k;
  k.version = store.matches_version();
  k.favourite = store.favourite();
  k.tz = store.tz_offset();
  k.today = floor_div(now + k.tz, 86400);
  return k;
}

struct MatchesState {
  Store* store = nullptr;
  ImageProvider* images = nullptr;
  std::function<void(std::string)> onOpen;
  MatchesParts parts{};
  std::vector<Item> items;
  View shown;
  ItemsKey shownKey;  // what `shown`'s items were computed from
  bool everApplied = false;
  // A long list is built in steps (kMatchesFirstItems, then
  // kMatchesItemsPerStep every kMatchesStepMs): `target` is the view being
  // built, `builder` the timer that goes on with it.
  View target;
  lv_timer_t* builder = nullptr;
};

MatchesState* state_of(lv_obj_t* screen) {
  return static_cast<MatchesState*>(lv_obj_get_user_data(screen));
}

// ---------------------------------------------------------------- view

int32_t text_width(const std::string& s, const lv_font_t* font) {
  lv_point_t size;
  lv_text_get_size(&size, s.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}

RowView row_view(const MatchSummary& m, const Store& store, int tz) {
  RowView r;
  r.homeAbbr = widgets::team_abbr(m.home);
  r.awayAbbr = widgets::team_abbr(m.away);
  r.homeCrest = sized(m.home.crest, kCrestSize, kCrestSize);
  r.awayCrest = sized(m.away.crest, kCrestSize, kCrestSize);
  r.state = m.state;
  const std::string& fav = store.favourite();
  r.fav = !fav.empty() && (m.home.id == fav || m.away.id == fav);
  r.favHome = r.fav && m.home.id == fav;
  const std::string score =
      std::to_string(m.homeScore.value_or(0)) + ":" + std::to_string(m.awayScore.value_or(0));
  switch (m.state) {
    case MatchState::Pre:
      r.centre = hh_mm(m.kickoff + tz);
      r.sub = league_name(m.league);
      break;
    case MatchState::Live:
      r.centre = score;
      r.sub = m.clock.empty() ? "LIVE" : m.clock;
      break;
    case MatchState::Post:
      r.centre = score;
      r.sub = "FT";
      break;
    case MatchState::Postponed:
      r.centre = "PPD";
      break;
  }
  r.smallCentre = text_width(r.centre, kCentreFont) > kCentreWidth;
  return r;
}

bool by_kickoff(const MatchSummary* a, const MatchSummary* b) {
  if (a->kickoff != b->kickoff) return a->kickoff < b->kickoff;
  return a->id < b->id;
}

View compute_view(const Store& store, int64_t now) {
  View v;
  v.status = widgets::status_info(store, now);
  if (!store.has_data()) {
    v.mode = Mode::Connecting;
    return v;
  }
  const int tz = store.tz_offset();
  const int64_t nowLocal = now + tz;
  const int64_t today = floor_div(nowLocal, 86400);

  std::vector<const MatchSummary*> live;
  std::vector<const MatchSummary*> upcoming;
  std::vector<const MatchSummary*> past;
  std::unordered_set<std::string> seen;
  for (const MatchSummary& m : store.matches()) {
    if (!seen.insert(m.id).second) continue;  // an id is shown once
    if (m.state == MatchState::Live) {
      live.push_back(&m);
    } else if (floor_div(m.kickoff + tz, 86400) >= today) {
      upcoming.push_back(&m);
    } else {
      past.push_back(&m);
    }
  }
  if (live.empty() && upcoming.empty() && past.empty()) {
    v.mode = Mode::Empty;
    return v;
  }
  v.mode = Mode::List;

  auto add_header = [&](const std::string& key, const std::string& text) {
    ItemView it;
    it.key = "h:" + key;
    it.header = true;
    it.text = text;
    v.items.push_back(std::move(it));
  };
  auto add_row = [&](const MatchSummary& m) {
    ItemView it;
    it.key = "r:" + m.id;
    it.matchId = m.id;
    it.row = row_view(m, store, tz);
    v.items.push_back(std::move(it));
  };

  if (!live.empty()) {
    std::sort(live.begin(), live.end(), by_kickoff);
    add_header("live", "LIVE NOW");
    for (const MatchSummary* m : live) add_row(*m);
  }
  auto add_days = [&](std::vector<const MatchSummary*>& group) {
    int64_t current = 0;
    bool open = false;
    for (const MatchSummary* m : group) {
      const int64_t day = floor_div(m->kickoff + tz, 86400);
      if (!open || day != current) {
        add_header(std::to_string(day), day_header(m->kickoff + tz, nowLocal));
        current = day;
        open = true;
      }
      add_row(*m);
    }
  };
  std::sort(upcoming.begin(), upcoming.end(), by_kickoff);
  add_days(upcoming);
  // Past days: the most recent day first, and within a day the latest kickoff
  // first.
  std::sort(past.begin(), past.end(), [](const MatchSummary* a, const MatchSummary* b) {
    return by_kickoff(b, a);
  });
  add_days(past);
  return v;
}

// ---------------------------------------------------------------- widgets

void on_delete(lv_event_t* e) {
  auto* st = static_cast<MatchesState*>(lv_event_get_user_data(e));
  if (st->builder != nullptr) lv_timer_delete(st->builder);
  delete st;
}

void on_row_clicked(lv_event_t* e) {
  auto* st = static_cast<MatchesState*>(lv_event_get_user_data(e));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (const Item& it : st->items) {
    if (it.obj == target && !it.header) {
      if (st->onOpen) {
        const std::string id = it.parts.matchId;  // onOpen may refresh this screen
        st->onOpen(id);
      }
      return;
    }
  }
}

lv_color_t row_color(bool live) {
  return live ? lv_color_hex(kLiveRowColor) : theme().panel;
}

Item create_header(MatchesState* st) {
  Item it;
  it.header = true;
  lv_obj_t* l = label(st->parts.list, &bigshoulders_16, theme().dim);
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_pad_left(l, kHeaderPadLeft, 0);
  lv_obj_set_style_pad_top(l, kHeaderPadTop, 0);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  it.obj = l;
  return it;
}

Item create_row(MatchesState* st) {
  const Palette& pal = theme();
  Item it;
  MatchesRowParts& p = it.parts;
  lv_obj_t* row = plain(st->parts.list);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(row, LV_PCT(100), kRowHeight);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, kRowRadius, 0);
  lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, st);
  p.row = row;

  const int32_t crestY = (kRowHeight - kCrestSize) / 2;
  const int32_t abbrY = (kRowHeight - hanken_14.line_height) / 2;
  p.homeCrest = widgets::crest_image(row, kCrestSize);
  lv_obj_set_pos(p.homeCrest, kRowPad, crestY);
  p.homeAbbr = label(row, &hanken_14, pal.chalk);
  lv_obj_set_pos(p.homeAbbr, kRowPad + kCrestSize + kCrestGap, abbrY);
  lv_obj_set_width(p.homeAbbr, kAbbrWidth);

  p.awayCrest = widgets::crest_image(row, kCrestSize);
  lv_obj_align(p.awayCrest, LV_ALIGN_TOP_RIGHT, -kRowPad, crestY);
  p.awayAbbr = label(row, &hanken_14, pal.chalk);
  lv_obj_set_width(p.awayAbbr, kAbbrWidth);
  lv_obj_set_style_text_align(p.awayAbbr, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(p.awayAbbr, LV_ALIGN_TOP_RIGHT, -(kRowPad + kCrestSize + kCrestGap), abbrY);

  p.centre = label(row, kCentreFont, pal.chalk);
  lv_obj_set_style_text_align(p.centre, LV_TEXT_ALIGN_CENTER, 0);
  p.sub = label(row, &hanken_10, pal.dim);
  lv_obj_set_style_text_align(p.sub, LV_TEXT_ALIGN_CENTER, 0);

  p.edge = plain(row);
  lv_obj_set_pos(p.edge, 0, kEdgeInset);
  lv_obj_set_size(p.edge, kEdgeWidth, kRowHeight - 2 * kEdgeInset);
  lv_obj_set_style_radius(p.edge, 2, 0);
  lv_obj_set_style_bg_color(p.edge, pal.amber, 0);
  lv_obj_set_style_bg_opa(p.edge, LV_OPA_COVER, 0);
  lv_obj_add_flag(p.edge, LV_OBJ_FLAG_HIDDEN);

  p.favDot = dot(row, kFavDotSize, pal.accent);
  lv_obj_add_flag(p.favDot, LV_OBJ_FLAG_HIDDEN);

  it.obj = row;
  return it;
}

// Centre text and sub text are stacked and centred as one block, or the
// centre text alone is centred when there is no sub text.
void place_centre(const MatchesRowParts& p, const lv_font_t* font, bool hasSub) {
  int32_t top = 0;
  int32_t height = 0;
  widgets::digit_metrics(font, top, height);
  const int32_t subHeight = 8;  // x-height-ish: Hanken 10 without ascender room
  const int32_t gap = 3;
  const int32_t block = hasSub ? height + gap + subHeight : height;
  const int32_t blockTop = (kRowHeight - block) / 2;
  lv_obj_align(p.centre, LV_ALIGN_TOP_MID, 0, blockTop - top);
  lv_obj_align(p.sub, LV_ALIGN_TOP_MID, 0, blockTop + height + gap - 2);
}

void apply_row(MatchesState* st, Item& it, const RowView& v) {
  MatchesRowParts& p = it.parts;
  const RowView& old = it.shown;
  const bool first = !it.applied;
  const bool live = v.state == MatchState::Live;
  const Palette& pal = theme();

  set_text(p.homeAbbr, v.homeAbbr);
  set_text(p.awayAbbr, v.awayAbbr);
  if (first || v.homeCrest != old.homeCrest) widgets::crest_load(p.homeCrest, *st->images, v.homeCrest);
  if (first || v.awayCrest != old.awayCrest) widgets::crest_load(p.awayCrest, *st->images, v.awayCrest);

  const lv_font_t* font = v.smallCentre ? kCentreSmallFont : kCentreFont;
  const bool hasSub = !v.sub.empty();
  if (first || v.smallCentre != old.smallCentre || hasSub != !old.sub.empty()) {
    lv_obj_set_style_text_font(p.centre, font, 0);
    place_centre(p, font, hasSub);
  }
  set_text(p.centre, v.centre);
  set_text(p.sub, v.sub);

  if (first || v.state != old.state) {
    const bool ppd = v.state == MatchState::Postponed;
    lv_obj_set_style_text_color(p.centre, live ? pal.amber : ppd ? pal.dim : pal.chalk, 0);
    lv_obj_set_style_text_color(p.sub, live ? pal.amber : pal.dim, 0);
    lv_obj_set_style_bg_color(p.row, row_color(live), 0);
    lv_obj_set_style_bg_color(p.row, mix(row_color(live), pal.chalk, 18), LV_STATE_PRESSED);
    show(p.edge, live);
  }
  if (v.fav && (first || v.favHome != old.favHome || !old.fav)) {
    // Centred over the favourite's own crest, in the row's top padding.
    const int32_t x = kRowPad + (kCrestSize - kFavDotSize) / 2;
    lv_obj_align(p.favDot, v.favHome ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, v.favHome ? x : -x,
                 kFavDotTop);
  }
  show(p.favDot, v.fav);
  it.shown = v;
  it.applied = true;
}

// Brings the list's children in line with `v`: rows and headers keep their
// widgets while their key is still wanted, new ones are created (at most
// `maxNew` of them: the rest wait for the next step), others deleted, and the
// order is set. True when every item of `v` has its widget.
bool reconcile(MatchesState* st, const View& v, size_t maxNew) {
  std::unordered_set<std::string> wanted;
  for (const ItemView& iv : v.items) wanted.insert(iv.key);

  std::unordered_map<std::string, size_t> have;
  std::vector<Item> kept;
  for (Item& it : st->items) {
    if (wanted.count(it.key) != 0) {
      have[it.key] = kept.size();
      kept.push_back(std::move(it));
    } else {
      lv_obj_delete(it.obj);
    }
  }

  std::vector<Item> next;
  next.reserve(v.items.size());
  size_t created = 0;
  bool complete = true;
  for (size_t i = 0; i < v.items.size(); ++i) {
    const ItemView& iv = v.items[i];
    Item it;
    auto found = have.find(iv.key);
    if (found != have.end()) {
      it = std::move(kept[found->second]);
    } else if (created >= maxNew) {
      complete = false;  // built in a later step
      continue;
    } else {
      it = iv.header ? create_header(st) : create_row(st);
      it.key = iv.key;
      ++created;
    }
    const int32_t pos = static_cast<int32_t>(next.size());
    if (lv_obj_get_index(it.obj) != pos) lv_obj_move_to_index(it.obj, pos);
    if (iv.header) {
      set_text(it.obj, iv.text);
    } else {
      it.parts.matchId = iv.matchId;
      apply_row(st, it, iv.row);
    }
    next.push_back(std::move(it));
  }
  st->items = std::move(next);
  return complete;
}

void on_build_step(lv_timer_t* t);

// Shows `v` (in steps when it needs many new widgets).
void apply(MatchesState* st, const View& v, size_t maxNew) {
  MatchesParts& p = st->parts;
  widgets::status_row_apply(p.status, "Matches", v.status);
  show(p.connecting, v.mode == Mode::Connecting);
  show(p.emptyCard, v.mode == Mode::Empty);
  show(p.list, v.mode == Mode::List);
  const bool complete = reconcile(st, v, maxNew);
  st->everApplied = true;
  if (complete) {
    st->shown = v;
    if (st->builder != nullptr) {
      lv_timer_delete(st->builder);
      st->builder = nullptr;
    }
    return;
  }
  // Not all there yet: `shown` keeps what was complete before (so the next
  // refresh never takes the partial list for the target) and a timer goes on.
  st->shown.status = v.status;
  st->target = v;
  if (st->builder == nullptr) st->builder = lv_timer_create(on_build_step, kMatchesStepMs, st);
}

void on_build_step(lv_timer_t* t) {
  auto* st = static_cast<MatchesState*>(lv_timer_get_user_data(t));
  View v = st->target;           // apply() may replace st->target
  v.status = st->shown.status;   // the status row as the last refresh left it
  apply(st, v, kMatchesItemsPerStep);
}

}  // namespace

// ---------------------------------------------------------------- public

lv_obj_t* matches_create(lv_obj_t* parent, Store& store, ImageProvider& images,
                         std::function<void(std::string matchId)> onOpen) {
  auto* st = new MatchesState();
  st->store = &store;
  st->images = &images;
  st->onOpen = std::move(onOpen);
  MatchesParts& p = st->parts;
  const Palette& pal = theme();

  lv_obj_t* root = plain(parent);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_user_data(root, st);
  lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, st);

  lv_obj_t* header = plain(root);
  lv_obj_set_size(header, LV_PCT(100), kStatusHeight);
  p.status = widgets::status_row_create(header);

  // The list scrolls vertically (momentum on, no scrollbar) and ends where
  // the tab ends, above the tab bar. It stays clickable so a drag that
  // starts between rows scrolls too.
  p.list = plain(root);
  lv_obj_add_flag(p.list, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(p.list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(p.list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(p.list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_width(p.list, LV_PCT(100));
  lv_obj_set_flex_grow(p.list, 1);
  lv_obj_set_flex_flow(p.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_left(p.list, kSideMargin, 0);
  lv_obj_set_style_pad_right(p.list, kSideMargin, 0);
  lv_obj_set_style_pad_bottom(p.list, kListPadBottom, 0);
  lv_obj_set_style_pad_row(p.list, kRowGap, 0);
  lv_obj_add_flag(p.list, LV_OBJ_FLAG_HIDDEN);

  // Empty states float over the list area.
  p.connecting = label(root, &bigshoulders_28, pal.chalk);
  lv_obj_add_flag(p.connecting, LV_OBJ_FLAG_FLOATING);
  lv_label_set_text(p.connecting, kMatchesConnecting);
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
  lv_label_set_text(p.emptyTitle, kMatchesEmptyTitle);
  lv_label_set_text(p.emptyHint, kMatchesEmptyHint);
  lv_obj_align(p.emptyCard, LV_ALIGN_CENTER, 0, -10);
  lv_obj_add_flag(p.emptyCard, LV_OBJ_FLAG_HIDDEN);

  // Until the first refresh the screen reads "Connecting...".
  widgets::status_row_apply(p.status, "Matches", StatusInfo{});
  return root;
}

void matches_refresh(lv_obj_t* screen, int64_t now) {
  MatchesState* st = screen != nullptr ? state_of(screen) : nullptr;
  if (st == nullptr) return;
  for (const Item& it : st->items) {
    if (!it.header) {
      widgets::crest_sync(it.parts.homeCrest);
      widgets::crest_sync(it.parts.awayCrest);
    }
  }
  const ItemsKey key = items_key(*st->store, now);
  if (st->everApplied && key == st->shownKey) {
    // The rows are as they were (or still being built for the same list):
    // only the status row (clock, stale marker, battery) may have changed.
    const StatusInfo status = widgets::status_info(*st->store, now);
    if (status != st->shown.status) {
      widgets::status_row_apply(st->parts.status, "Matches", status);
      st->shown.status = status;
    }
    return;
  }
  st->shownKey = key;
  const View v = compute_view(*st->store, now);
  if (st->everApplied && v == st->shown && st->builder == nullptr) return;
  apply(st, v, kMatchesFirstItems);
}

bool matches_complete(lv_obj_t* screen) {
  MatchesState* st = screen != nullptr ? state_of(screen) : nullptr;
  return st != nullptr && st->builder == nullptr;
}

namespace {

MatchesState* matches_state(lv_obj_t* screen) {
  if (screen == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(screen); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(screen, i)) == on_delete) return state_of(screen);
  }
  return nullptr;
}

}  // namespace

const MatchesParts* matches_parts(lv_obj_t* screen) {
  MatchesState* st = matches_state(screen);
  return st != nullptr ? &st->parts : nullptr;
}

std::vector<MatchesItem> matches_items(lv_obj_t* screen) {
  std::vector<MatchesItem> out;
  const MatchesState* st = matches_state(screen);
  if (st == nullptr) return out;
  for (const Item& it : st->items) {
    MatchesItem m;
    m.header = it.header;
    if (it.header) {
      m.headerLabel = it.obj;
    } else {
      m.row = it.parts;
    }
    out.push_back(std::move(m));
  }
  return out;
}
