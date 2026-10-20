#include "screens/team_picker.h"

#include <algorithm>
#include <utility>

#include "fonts.h"
#include "format.h"
#include "screens/widgets.h"
#include "theme.h"

// Layout (240x320, the whole screen): "< Back" and the title, the search
// field, then the list. While the keyboard is up the title steps aside and
// the field moves up, so the list keeps room for two and a half rows above
// the keyboard.
namespace {

using widgets::label;
using widgets::plain;
using widgets::set_text;
using widgets::show;

constexpr int32_t kSide = 8;
constexpr int32_t kFieldH = 34;
constexpr int32_t kFieldY = 54;          // under the title
constexpr int32_t kFieldYKeyboard = 28;  // title hidden, right under "< Back"
constexpr int32_t kFieldGap = 6;
constexpr int32_t kKeyboardH = 150;
constexpr int32_t kRowHeight = 40;
constexpr int32_t kCrestSize = 24;
constexpr int32_t kRowPad = 8;
constexpr int32_t kCrestPreload = 120;  // px beyond the visible list whose crests load too

struct Row {
  TeamRowParts parts;
  bool crestRequested = false;
};

struct PickerState {
  std::vector<Team> teams;  // display order
  Team current;
  ImageProvider* images = nullptr;
  std::function<void(Team)> onPick;
  std::function<void()> onBack;
  std::function<void()> onRetry;
  bool failed = false;
  bool keyboard = false;
  std::string query;
  TeamPickerParts parts{};
  std::vector<Row> rows;
  lv_obj_t* root = nullptr;
};

PickerState* state_of(lv_obj_t* obj) { return static_cast<PickerState*>(lv_obj_get_user_data(obj)); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string s) {
  for (char& c : s) c = lower(c);
  return s;
}

int league_rank(const std::string& code) {
  const std::vector<std::string>& order = picker_leagues();
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == code) return static_cast<int>(i);
  }
  return static_cast<int>(order.size());
}

// ---------------------------------------------------------------- layout

void place(PickerState* st) {
  TeamPickerParts& p = st->parts;
  const int32_t h = lv_obj_get_height(st->root);
  const int32_t fieldY = st->keyboard ? kFieldYKeyboard : kFieldY;
  const int32_t listY = fieldY + kFieldH + kFieldGap;
  const int32_t listEnd = st->keyboard ? h - kKeyboardH - 2 : h;
  show(p.title, !st->keyboard);
  show(p.keyboard, st->keyboard);
  lv_obj_set_y(p.field, fieldY);
  lv_obj_set_pos(p.list, 0, listY);
  lv_obj_set_height(p.list, listEnd - listY);
  // Messages sit in the upper part of the list area.
  lv_obj_align(p.message, LV_ALIGN_TOP_MID, 0, listY + 24);
  lv_obj_align_to(p.retry, p.message, LV_ALIGN_OUT_BOTTOM_MID, 0, 14);
}

// Crests load as their rows come near the visible part of the list.
void load_crests(PickerState* st) {
  lv_obj_update_layout(st->root);
  lv_area_t view;
  lv_obj_get_coords(st->parts.list, &view);
  for (Row& r : st->rows) {
    if (lv_obj_has_flag(r.parts.row, LV_OBJ_FLAG_HIDDEN)) continue;
    if (!r.crestRequested) {
      lv_area_t a;
      lv_obj_get_coords(r.parts.row, &a);
      if (a.y2 < view.y1 - kCrestPreload || a.y1 > view.y2 + kCrestPreload) continue;
      r.crestRequested = true;
      widgets::crest_load(r.parts.crest, *st->images, sized(r.parts.team.crest, kCrestSize, kCrestSize));
    }
    widgets::crest_sync(r.parts.crest);
  }
}

void apply_filter(PickerState* st) {
  TeamPickerParts& p = st->parts;
  size_t shown = 0;
  for (Row& r : st->rows) {
    const bool match = team_matches(r.parts.team, st->query);
    show(r.parts.row, match);
    if (match) ++shown;
  }
  std::string message;
  bool retry = false;
  if (st->rows.empty()) {
    message = st->failed ? kTeamsFailed : kTeamsLoading;
    retry = st->failed;
  } else if (shown == 0) {
    message = std::string(kNoMatchPrefix) + st->query + "\"";
  }
  set_text(p.message, message);
  show(p.message, !message.empty());
  show(p.retry, retry);
  place(st);
  load_crests(st);
}

// ---------------------------------------------------------------- events

void set_keyboard(PickerState* st, bool on) {
  if (st->keyboard == on) return;
  st->keyboard = on;
  if (on) {
    lv_obj_add_state(st->parts.field, LV_STATE_FOCUSED);
  } else {
    lv_obj_remove_state(st->parts.field, LV_STATE_FOCUSED);
  }
  place(st);
  load_crests(st);
}

void on_field(lv_event_t* e) {
  auto* st = static_cast<PickerState*>(lv_event_get_user_data(e));
  switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED: set_keyboard(st, true); break;
    case LV_EVENT_READY: set_keyboard(st, false); break;  // Enter on the one-line field
    case LV_EVENT_VALUE_CHANGED: {
      const std::string q = lv_textarea_get_text(st->parts.field);
      if (q == st->query) return;
      st->query = q;
      apply_filter(st);
      break;
    }
    default: break;
  }
}

void on_keyboard(lv_event_t* e) {
  set_keyboard(static_cast<PickerState*>(lv_event_get_user_data(e)), false);
}

void on_list(lv_event_t* e) {
  auto* st = static_cast<PickerState*>(lv_event_get_user_data(e));
  if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
    set_keyboard(st, false);
  } else {
    load_crests(st);
  }
}

void on_row(lv_event_t* e) {
  auto* st = static_cast<PickerState*>(lv_event_get_user_data(e));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (const Row& r : st->rows) {
    if (r.parts.row != target) continue;
    const Team t = r.parts.team;  // the host may delete the picker
    const std::function<void(Team)> f = st->onPick;
    if (f) f(t);
    return;
  }
}

void on_back(lv_event_t* e) {
  const std::function<void()> f = static_cast<PickerState*>(lv_event_get_user_data(e))->onBack;
  if (f) f();
}

void on_retry(lv_event_t* e) {
  const std::function<void()> f = static_cast<PickerState*>(lv_event_get_user_data(e))->onRetry;
  if (f) f();
}

void on_delete(lv_event_t* e) { delete static_cast<PickerState*>(lv_event_get_user_data(e)); }

// ---------------------------------------------------------------- widgets

void style_field(lv_obj_t* ta) {
  const Palette& pal = theme();
  lv_obj_remove_style_all(ta);
  lv_obj_set_style_bg_color(ta, pal.panel, 0);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ta, widgets::kCardRadius, 0);
  lv_obj_set_style_border_color(ta, pal.line, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, mix(pal.line, pal.chalk, 70), LV_STATE_FOCUSED);
  lv_obj_set_style_pad_hor(ta, 12, 0);
  lv_obj_set_style_pad_top(ta, (kFieldH - 2 - hanken_14.line_height) / 2, 0);
  lv_obj_set_style_pad_bottom(ta, 0, 0);
  lv_obj_set_style_text_font(ta, &hanken_14, 0);
  lv_obj_set_style_text_color(ta, pal.chalk, 0);
  lv_obj_set_style_text_color(ta, pal.dim, LV_PART_TEXTAREA_PLACEHOLDER);
  // Cursor: a thin chalk bar, only while the field has the keyboard.
  lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
  lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
  lv_obj_set_style_border_color(ta, accent_mark(pal.accent), LV_PART_CURSOR);
  lv_obj_set_style_border_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
  lv_obj_set_style_border_opa(ta, LV_OPA_COVER,
                              static_cast<lv_style_selector_t>(LV_PART_CURSOR) | LV_STATE_FOCUSED);
  lv_obj_set_style_anim_duration(ta, 500, LV_PART_CURSOR);
}

void style_keyboard(lv_obj_t* kb) {
  const Palette& pal = theme();
  const lv_color_t key = mix(pal.panel, pal.chalk, 22);
  const lv_color_t special = pal.line;
  lv_obj_set_style_bg_color(kb, pal.tabBar, 0);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(kb, 1, 0);
  lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(kb, pal.line, 0);
  lv_obj_set_style_radius(kb, 0, 0);
  lv_obj_set_style_pad_all(kb, 4, 0);
  lv_obj_set_style_pad_gap(kb, 4, 0);

  const lv_state_t kStates[] = {LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_CHECKED,
                                LV_STATE_CHECKED | LV_STATE_PRESSED, LV_STATE_FOCUSED,
                                LV_STATE_FOCUS_KEY, LV_STATE_CHECKED | LV_STATE_FOCUSED};
  for (lv_state_t s : kStates) {
    const lv_style_selector_t sel = static_cast<lv_style_selector_t>(LV_PART_ITEMS) | s;
    const bool pressed = (s & LV_STATE_PRESSED) != 0;
    const bool checked = (s & LV_STATE_CHECKED) != 0;
    const lv_color_t base = checked ? special : key;
    lv_obj_set_style_bg_color(kb, pressed ? mix(base, pal.chalk, 70) : base, sel);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, sel);
    lv_obj_set_style_text_color(kb, pal.chalk, sel);
    lv_obj_set_style_border_width(kb, 0, sel);
    lv_obj_set_style_outline_width(kb, 0, sel);
    lv_obj_set_style_shadow_width(kb, 0, sel);
  }
  lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
}

Row create_row(PickerState* st, const Team& team) {
  const Palette& pal = theme();
  Row r;
  TeamRowParts& p = r.parts;
  p.team = team;
  p.row = widgets::card(st->parts.list, true);
  lv_obj_set_size(p.row, LV_PCT(100), kRowHeight);
  lv_obj_add_event_cb(p.row, on_row, LV_EVENT_CLICKED, st);

  p.crest = widgets::crest_image(p.row, kCrestSize);
  lv_obj_align(p.crest, LV_ALIGN_LEFT_MID, kRowPad, 0);
  const int32_t textX = kRowPad + kCrestSize + 8;
  p.name = label(p.row, &hanken_14, pal.chalk);
  lv_label_set_long_mode(p.name, LV_LABEL_LONG_DOT);
  lv_obj_set_size(p.name, 224 - textX - 30, hanken_14.line_height);  // one line
  lv_label_set_text(p.name, team.name.c_str());
  lv_obj_set_pos(p.name, textX, 3);
  p.league = label(p.row, &hanken_10, pal.dim);
  lv_label_set_text(p.league, league_name(team.league));
  lv_obj_set_pos(p.league, textX, 22);
  p.tick = widgets::tick(p.row);
  lv_obj_align(p.tick, LV_ALIGN_RIGHT_MID, -10, 0);
  if (team.id != st->current.id) lv_obj_add_flag(p.tick, LV_OBJ_FLAG_HIDDEN);
  return r;
}

void build_rows(PickerState* st) {
  for (Row& r : st->rows) lv_obj_delete(r.parts.row);
  st->rows.clear();
  st->rows.reserve(st->teams.size());
  for (const Team& t : st->teams) st->rows.push_back(create_row(st, t));
}

PickerState* picker_state(lv_obj_t* picker) {
  if (picker == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(picker); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(picker, i)) == on_delete) return state_of(picker);
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- public

const std::vector<std::string>& picker_leagues() {
  static const std::vector<std::string> kLeagues = {"eng.1", "esp.1", "ita.1", "ger.1", "fra.1"};
  return kLeagues;
}

bool team_matches(const Team& team, const std::string& query) {
  if (query.empty()) return true;
  const std::string q = lowered(query);
  return lowered(team.name).find(q) != std::string::npos ||
         lowered(team.abbr).find(q) != std::string::npos;
}

std::vector<Team> filter_teams(const std::vector<Team>& teams, const std::string& query) {
  std::vector<Team> out;
  for (const Team& t : teams) {
    if (team_matches(t, query)) out.push_back(t);
  }
  std::stable_sort(out.begin(), out.end(), [](const Team& a, const Team& b) {
    const int ra = league_rank(a.league);
    const int rb = league_rank(b.league);
    if (ra != rb) return ra < rb;
    if (a.league != b.league) return a.league < b.league;  // unknown leagues, by code
    return lowered(a.name) < lowered(b.name);
  });
  return out;
}

lv_obj_t* team_picker_create(lv_obj_t* parent, const std::vector<Team>& teams, const Team& current,
                             ImageProvider& images, std::function<void(Team)> onPick,
                             std::function<void()> onBack) {
  auto* st = new PickerState();
  st->current = current;
  st->images = &images;
  st->onPick = std::move(onPick);
  st->onBack = std::move(onBack);
  TeamPickerParts& p = st->parts;
  const Palette& pal = theme();

  lv_obj_t* root = plain(parent);
  st->root = root;
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, pal.turf, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_user_data(root, st);
  lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, st);
  lv_obj_update_layout(root);

  const widgets::PageHeader header = widgets::page_header_create(root, kPickerTitle);
  p.back = header.back;
  p.title = header.title;
  lv_obj_add_event_cb(p.back, on_back, LV_EVENT_CLICKED, st);

  p.field = lv_textarea_create(root);
  style_field(p.field);
  lv_textarea_set_one_line(p.field, true);
  lv_textarea_set_placeholder_text(p.field, kSearchPlaceholder);
  lv_obj_set_size(p.field, 240 - 2 * kSide, kFieldH);
  lv_obj_set_x(p.field, kSide);
  lv_obj_remove_flag(p.field, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(p.field, on_field, LV_EVENT_CLICKED, st);
  lv_obj_add_event_cb(p.field, on_field, LV_EVENT_READY, st);
  lv_obj_add_event_cb(p.field, on_field, LV_EVENT_VALUE_CHANGED, st);

  p.list = widgets::scroll_column(root);
  lv_obj_set_width(p.list, LV_PCT(100));
  lv_obj_add_event_cb(p.list, on_list, LV_EVENT_SCROLL_BEGIN, st);
  lv_obj_add_event_cb(p.list, on_list, LV_EVENT_SCROLL_END, st);

  p.message = label(root, &hanken_14, pal.chalk);
  lv_obj_set_width(p.message, 200);
  lv_label_set_long_mode(p.message, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(p.message, LV_TEXT_ALIGN_CENTER, 0);

  p.retry = plain(root);
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
  lv_label_set_text(label(p.retry, &hanken_12, pal.amber), "Try again");

  p.keyboard = lv_keyboard_create(root);
  style_keyboard(p.keyboard);
  lv_keyboard_set_mode(p.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(p.keyboard, p.field);
  lv_obj_set_size(p.keyboard, LV_PCT(100), kKeyboardH);
  lv_obj_align(p.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_event_cb(p.keyboard, on_keyboard, LV_EVENT_READY, st);
  lv_obj_add_event_cb(p.keyboard, on_keyboard, LV_EVENT_CANCEL, st);

  st->teams = filter_teams(teams, "");
  build_rows(st);
  apply_filter(st);
  return root;
}

void team_picker_set_teams(lv_obj_t* picker, const std::vector<Team>& teams) {
  PickerState* st = picker_state(picker);
  if (st == nullptr) return;
  st->teams = filter_teams(teams, "");
  build_rows(st);
  apply_filter(st);
}

void team_picker_set_failed(lv_obj_t* picker, bool failed, std::function<void()> onRetry) {
  PickerState* st = picker_state(picker);
  if (st == nullptr) return;
  st->failed = failed;
  st->onRetry = std::move(onRetry);
  apply_filter(st);
}

void team_picker_set_query(lv_obj_t* picker, const std::string& query) {
  PickerState* st = picker_state(picker);
  if (st == nullptr) return;
  lv_textarea_set_text(st->parts.field, query.c_str());  // sends VALUE_CHANGED
  if (st->query != query) {
    st->query = query;
    apply_filter(st);
  }
}

void team_picker_refresh(lv_obj_t* picker) {
  PickerState* st = picker_state(picker);
  if (st != nullptr) load_crests(st);
}

void team_picker_show_keyboard(lv_obj_t* picker, bool show) {
  PickerState* st = picker_state(picker);
  if (st != nullptr) set_keyboard(st, show);
}

const TeamPickerParts* team_picker_parts(lv_obj_t* picker) {
  PickerState* st = picker_state(picker);
  return st != nullptr ? &st->parts : nullptr;
}

std::vector<TeamRowParts> team_picker_rows(lv_obj_t* picker) {
  std::vector<TeamRowParts> out;
  PickerState* st = picker_state(picker);
  if (st == nullptr) return out;
  for (const Row& r : st->rows) {
    if (!lv_obj_has_flag(r.parts.row, LV_OBJ_FLAG_HIDDEN)) out.push_back(r.parts);
  }
  return out;
}
