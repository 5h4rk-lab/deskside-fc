#include "screens/live.h"

#include <string.h>

#include "fonts.h"
#include "format.h"
#include "screens/widgets.h"
#include "theme.h"

// Geometry follows screen 1 of the mockup (240x320, tab bar overlaid at the
// bottom). The screen lives in the Live tab, which is 276 px tall; everything
// keeps its mockup y position and the lower part is simply cut by the tab bar.
namespace {

using widgets::dot;
using widgets::label;
using widgets::plain;
using widgets::set_text;
using widgets::show;
using widgets::StatusInfo;
using widgets::StatusRow;

constexpr int32_t kVirtualHeight = 320;  // mockup screen height
constexpr int32_t kSplitCenterY = kVirtualHeight / 2;
// CSS linear-gradient(105deg): the split line leans 15 degrees from vertical,
// top end to the right. tan(15 deg) = 0.2679, kept as a fraction.
constexpr int32_t kSlopeNum = 2679;
constexpr int32_t kSlopeDen = 10000;
constexpr uint8_t kTeamOpa = 230;  // .live-bg opacity: .9

constexpr int32_t kCrestSize = 84;
constexpr int32_t kCrestTop = 26;
constexpr int32_t kCrestInset = 8;
constexpr int32_t kHaloRadius = 60;     // light disc behind a crest on a dark half
constexpr lv_opa_t kHaloOpa = 46;       // chalk at 18 % in the middle
constexpr int32_t kPillTop = 84;
constexpr int32_t kScoreCenterY = 136;  // middle of the digits (mockup: 97..174)
constexpr int32_t kScoreMaxWidth = 200;
constexpr int32_t kNamesTop = 184;
constexpr int32_t kNameWidth = 100;
constexpr int32_t kNamesInset = 14;
constexpr int32_t kStripTop = 214;
constexpr int32_t kStripInset = 10;
constexpr int32_t kMessageTop = 128;        // next-match text, under the crests
constexpr int32_t kStatusMessageTop = 120;  // "Connecting...", "No matches soon"
constexpr int32_t kLastCaptionTop = 4;      // "Last result", on the status row line, centred
constexpr int32_t kLastLineTop = 180;       // "vs CHE [FT]", under the score
constexpr int32_t kLastWhenTop = 216;       // "yesterday"

const lv_font_t* const kNameFont = &bigshoulders_16;

constexpr int32_t kGearSize = 32;     // tap target
constexpr int32_t kGearIconTop = 2;   // glyph centred on the status row text

enum class Mode { Connecting, NoMatches, Last, Next, Live };

// Everything the screen shows, computed from the Store. Applied to the
// widgets only when it differs from what is on screen.
struct View {
  Mode mode = Mode::Connecting;
  std::string league;
  StatusInfo status;
  std::string message;
  std::string countdown;
  std::string lastOpponent;  // "vs CHE"
  std::string lastWhen;      // "yesterday"
  std::string matchId;
  std::string homeCrest;
  std::string awayCrest;
  uint32_t homeColor = 0;  // primaries; the split is live_background_colors() of them
  uint32_t awayColor = 0;
  bool homeHalo = false;   // a light halo behind the crest on a dark half
  bool awayHalo = false;
  std::string pill;
  std::string scoreHome;
  std::string scoreAway;
  std::string homeName;
  std::string awayName;
  bool hasEvent = false;
  std::string eventCaption;
  std::string eventPlayer;
  std::string eventCutout;
  uint32_t eventColor = 0;

  bool operator==(const View& o) const {
    return mode == o.mode && league == o.league && status == o.status && message == o.message && countdown == o.countdown &&
           lastOpponent == o.lastOpponent && lastWhen == o.lastWhen &&
           matchId == o.matchId && homeCrest == o.homeCrest && awayCrest == o.awayCrest &&
           homeColor == o.homeColor && awayColor == o.awayColor && homeHalo == o.homeHalo &&
           awayHalo == o.awayHalo && pill == o.pill &&
           scoreHome == o.scoreHome && scoreAway == o.scoreAway && homeName == o.homeName &&
           awayName == o.awayName && hasEvent == o.hasEvent && eventCaption == o.eventCaption &&
           eventPlayer == o.eventPlayer && eventCutout == o.eventCutout &&
           eventColor == o.eventColor;
  }
  bool operator!=(const View& o) const { return !(*this == o); }
};

struct LiveState {
  Store* store = nullptr;
  ImageProvider* images = nullptr;
  std::function<void(std::string)> onOpen;
  std::function<void()> onSettings;
  LiveParts parts{};
  View shown;
  StatusRow status;
  bool everApplied = false;
  bool splitVisible = false;
  lv_color_t splitHome{};
  lv_color_t splitAway{};
};

// ---------------------------------------------------------------- helpers

const char* event_name(EventType t) {
  switch (t) {
    case EventType::Goal: return "Goal";
    case EventType::Yellow: return "Yellow card";
    case EventType::Red: return "Red card";
    case EventType::Sub: return "Substitution";
  }
  return "";
}

int distance_sq(lv_color_t a, lv_color_t b) {
  const int dr = a.red - b.red;
  const int dg = a.green - b.green;
  const int db = a.blue - b.blue;
  return dr * dr + dg * dg + db * db;
}

bool too_close(lv_color_t a, lv_color_t b) {
  return distance_sq(a, b) < kSplitMinDistance * kSplitMinDistance;
}

LiveState* state_of(lv_obj_t* screen) {
  return static_cast<LiveState*>(lv_obj_get_user_data(screen));
}

// ---------------------------------------------------------------- widgets

lv_obj_t* crest(lv_obj_t* parent, int32_t x) {
  lv_obj_t* img = widgets::crest_image(parent, kCrestSize);
  lv_obj_set_pos(img, x, kCrestTop);
  return img;
}

// The halo's gradient: chalk at about 18 % out to roughly the crest's radius
// (so it shows through the crest's transparent corners and just around it),
// fading to nothing at the halo's edge. Shared by every halo; styles keep a pointer to it.
const lv_grad_dsc_t* halo_grad() {
  static lv_grad_dsc_t g = [] {
    lv_grad_dsc_t d;
    memset(&d, 0, sizeof(d));
#if LV_USE_DRAW_SW_COMPLEX_GRADIENTS
    lv_grad_radial_init(&d, LV_GRAD_CENTER, LV_GRAD_CENTER, LV_GRAD_RIGHT, LV_GRAD_CENTER,
                        LV_GRAD_EXTEND_PAD);
#endif
    d.stops_count = 2;
    d.stops[0].color = theme().chalk;
    d.stops[0].opa = kHaloOpa;
    d.stops[0].frac = 140;  // flat out to about the crest's edge, then fading
    d.stops[1].color = theme().chalk;
    d.stops[1].opa = LV_OPA_TRANSP;
    d.stops[1].frac = 255;
    return d;
  }();
  return &g;
}

// A soft lit disc centred on the crest at `crestX` (one object, drawn below
// the crest). Hidden until a dark half needs it.
lv_obj_t* halo(lv_obj_t* parent, int32_t crestX) {
  lv_obj_t* o = plain(parent);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(o, kHaloRadius * 2, kHaloRadius * 2);
  lv_obj_set_pos(o, crestX + kCrestSize / 2 - kHaloRadius, kCrestTop + kCrestSize / 2 - kHaloRadius);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, theme().chalk, 0);
#if LV_USE_DRAW_SW_COMPLEX_GRADIENTS
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_grad(o, halo_grad(), 0);
#else
  lv_obj_set_style_bg_opa(o, kHaloOpa / 2, 0);  // flat fallback: a faint disc
#endif
  lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  return o;
}

void load_image(LiveState* st, lv_obj_t* img, const std::string& path) {
  widgets::crest_load(img, *st->images, path);
}

void pulse_exec(void* obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

// Diagonal split plus the mockup's .live-shade: a vignette around the score
// and a fade into the turf colour towards the bottom.
void draw_background(lv_event_t* e) {
  auto* st = static_cast<LiveState*>(lv_event_get_user_data(e));
  if (!st->splitVisible) return;
  lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const lv_color_t turf = theme().turf;

  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.bg_color = mix(turf, st->splitHome, kTeamOpa);
  rect.bg_opa = LV_OPA_COVER;
  lv_draw_rect(layer, &rect, &a);

  // Away half: one triangle whose only visible edge is the split line, so
  // there is no seam inside the away colour.
  auto line_x = [&](int32_t y) {
    return a.x1 + 120 + (kSplitCenterY - y) * kSlopeNum / kSlopeDen;
  };
  constexpr int32_t kFar = 2000;
  lv_draw_triangle_dsc_t tri;
  lv_draw_triangle_dsc_init(&tri);
  tri.bg_color = mix(turf, st->splitAway, kTeamOpa);
  tri.bg_opa = LV_OPA_COVER;
  tri.p[0].x = line_x(-kFar);
  tri.p[0].y = a.y1 - kFar;
  tri.p[1].x = line_x(kFar);
  tri.p[1].y = a.y1 + kFar;
  tri.p[2].x = a.x1 + kFar * 2;
  tri.p[2].y = a.y1 + kSplitCenterY;
  lv_draw_triangle(layer, &tri);

  // Vignette: radial-gradient(120% 70% at 50% 38%, clear -> turf .78 at 70%).
  // LVGL gradients are circular; 180 px sits between the ellipse's radii.
  lv_draw_rect_dsc_t shade;
  lv_draw_rect_dsc_init(&shade);
#if LV_USE_DRAW_SW_COMPLEX_GRADIENTS
  shade.bg_opa = LV_OPA_COVER;
  lv_grad_radial_init(&shade.bg_grad, 120, 122, 120 + 180, 122, LV_GRAD_EXTEND_PAD);
  shade.bg_grad.stops[0].color = turf;
  shade.bg_grad.stops[0].opa = LV_OPA_TRANSP;
  shade.bg_grad.stops[0].frac = 0;
  shade.bg_grad.stops[1].color = turf;
  shade.bg_grad.stops[1].opa = 199;  // .78
  shade.bg_grad.stops[1].frac = 255;
  shade.bg_grad.stops_count = 2;
#else
  shade.bg_color = turf;
  shade.bg_opa = 100;
#endif
  lv_area_t full = a;
  full.y2 = a.y1 + kVirtualHeight - 1;
  lv_draw_rect(layer, &shade, &full);

  // linear-gradient(transparent 55%, turf 92%).
  lv_draw_rect_dsc_t fade;
  lv_draw_rect_dsc_init(&fade);
  fade.bg_opa = LV_OPA_COVER;
  fade.bg_grad.dir = LV_GRAD_DIR_VER;
  fade.bg_grad.stops[0].color = turf;
  fade.bg_grad.stops[0].opa = LV_OPA_TRANSP;
  fade.bg_grad.stops[0].frac = 0;
  fade.bg_grad.stops[1].color = turf;
  fade.bg_grad.stops[1].opa = LV_OPA_COVER;
  fade.bg_grad.stops[1].frac = 255;
  fade.bg_grad.stops_count = 2;
  lv_area_t band = a;
  band.y1 = a.y1 + kVirtualHeight * 55 / 100;
  band.y2 = a.y1 + kVirtualHeight * 92 / 100;
  lv_draw_rect(layer, &fade, &band);
  lv_draw_rect_dsc_t solid;
  lv_draw_rect_dsc_init(&solid);
  solid.bg_color = turf;
  solid.bg_opa = LV_OPA_COVER;
  lv_area_t rest = a;
  rest.y1 = band.y2 + 1;
  if (rest.y1 <= rest.y2) lv_draw_rect(layer, &solid, &rest);
}

void on_score_clicked(lv_event_t* e) {
  auto* st = static_cast<LiveState*>(lv_event_get_user_data(e));
  const bool opens = st->shown.mode == Mode::Live || st->shown.mode == Mode::Last;
  if (opens && st->onOpen) st->onOpen(st->shown.matchId);
}

void on_gear_clicked(lv_event_t* e) {
  auto* st = static_cast<LiveState*>(lv_event_get_user_data(e));
  const std::function<void()> f = st->onSettings;  // the host may rebuild this screen
  if (f) f();
}

void on_delete(lv_event_t* e) { delete static_cast<LiveState*>(lv_event_get_user_data(e)); }

void set_score_font(LiveParts& p, const lv_font_t* font) {
  if (lv_obj_get_style_text_font(p.scoreHome, 0) == font) return;
  for (lv_obj_t* l : {p.scoreHome, p.scoreSep, p.scoreAway}) {
    lv_obj_set_style_text_font(l, font, 0);
  }
  int32_t top = 0;
  int32_t height = 0;
  widgets::digit_metrics(font, top, height);
  lv_obj_align(p.score, LV_ALIGN_TOP_MID, 0, kScoreCenterY - height / 2 - top);
}

int32_t text_width(const std::string& s, const lv_font_t* font, int32_t letterSpace) {
  lv_point_t size;
  lv_text_get_size(&size, s.c_str(), font, letterSpace, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}

const lv_font_t* score_font(const View& v) {
  // Big Shoulders is condensed: even "10:10" is about 160 px wide at 96 px,
  // so the width rule alone never triggers. Two-digit scores also step down,
  // which keeps the digits clear of the crests either side.
  if (v.scoreHome.size() > 1 || v.scoreAway.size() > 1) return &bigshoulders_56;
  const int32_t w = text_width(v.scoreHome + ":" + v.scoreAway, &bigshoulders_96, -2);
  return w > kScoreMaxWidth ? &bigshoulders_56 : &bigshoulders_96;
}

// ---------------------------------------------------------------- view

// Halos behind the crests whose half is dark (see crest_needs_halo).
void set_halos(View& v) {
  const auto bg = live_background_colors(v.homeColor, v.awayColor);
  v.homeHalo = crest_needs_halo(bg.first);
  v.awayHalo = crest_needs_halo(bg.second);
}

View compute_view(const Store& store, int64_t now) {
  View v;
  const int tz = store.tz_offset();
  v.status = widgets::status_info(store, now);
  if (!store.has_data()) {
    v.mode = Mode::Connecting;
    v.message = "Connecting...";
    return v;
  }
  const std::string& fav = store.favourite();
  if (const MatchSummary* m = store.live_for(fav)) {
    v.mode = Mode::Live;
    v.league = league_name(m->league);
    v.matchId = m->id;
    v.homeCrest = sized(m->home.crest, kCrestSize, kCrestSize);
    v.awayCrest = sized(m->away.crest, kCrestSize, kCrestSize);
    v.homeColor = m->home.primary;
    v.awayColor = m->away.primary;
    v.pill = m->clock.empty() ? "LIVE" : m->clock;
    v.scoreHome = std::to_string(m->homeScore.value_or(0));
    v.scoreAway = std::to_string(m->awayScore.value_or(0));
    v.homeName = to_upper(m->home.name);
    v.awayName = to_upper(m->away.name);
    if (const MatchDetail* d = store.detail_for(m->id)) {
      if (!d->events.empty()) {
        const MatchEvent& ev = d->events.back();
        v.hasEvent = true;
        v.eventCaption = ev.minute + "  " + event_name(ev.type);
        v.eventPlayer = ev.player;
        v.eventCutout = "/api/cutout?name=" + url_encode(ev.player) + "&w=40&h=40";
        v.eventColor = ev.team == Side::Home ? m->home.color : m->away.color;
      }
    }
    set_halos(v);
    return v;
  }
  if (const MatchSummary* m = store.next_for(fav, now)) {
    v.mode = Mode::Next;
    v.league = league_name(m->league);
    v.matchId = m->id;
    v.homeCrest = sized(m->home.crest, kCrestSize, kCrestSize);
    v.awayCrest = sized(m->away.crest, kCrestSize, kCrestSize);
    v.homeColor = m->home.primary;
    v.awayColor = m->away.primary;
    const Team& opponent = m->home.id == fav ? m->away : m->home;
    const int64_t local = m->kickoff + tz;
    v.message = "Next: " + opponent.abbr + " " + weekday(local) + " " + hh_mm(local);
    v.countdown = countdown_text(m->kickoff - now);
    set_halos(v);
    return v;
  }
  if (const MatchSummary* m = store.last_for(fav, now)) {
    v.mode = Mode::Last;
    v.matchId = m->id;
    // Favourite first, whether it played at home or away: its crest and
    // colour on the left with its goals, the opponent on the right with
    // theirs, so each number sits on its own team's side. (The "home"/"away"
    // fields of the view mean left/right here.)
    const bool home = m->home.id == fav;
    const Team& us = home ? m->home : m->away;
    const Team& them = home ? m->away : m->home;
    v.homeCrest = sized(us.crest, kCrestSize, kCrestSize);
    v.awayCrest = sized(them.crest, kCrestSize, kCrestSize);
    v.homeColor = us.primary;
    v.awayColor = them.primary;
    v.scoreHome = std::to_string((home ? m->homeScore : m->awayScore).value_or(0));
    v.scoreAway = std::to_string((home ? m->awayScore : m->homeScore).value_or(0));
    v.lastOpponent = "vs " + them.abbr;
    v.lastWhen = relative_time(now, m->kickoff, tz);
    set_halos(v);
    return v;
  }
  v.mode = Mode::NoMatches;
  v.message = "No matches soon";
  return v;
}

void apply(LiveState* st, const View& v) {
  LiveParts& p = st->parts;
  const View& old = st->shown;
  const bool first = !st->everApplied;
  const bool live = v.mode == Mode::Live;
  const bool next = v.mode == Mode::Next;
  const bool last = v.mode == Mode::Last;

  widgets::status_row_apply(st->status, v.league, v.status);

  // Background split.
  const bool split = live || next || last;
  if (split != st->splitVisible || (split && (first || v.homeColor != old.homeColor ||
                                              v.awayColor != old.awayColor))) {
    st->splitVisible = split;
    const auto colors = live_background_colors(v.homeColor, v.awayColor);
    st->splitHome = colors.first;
    st->splitAway = colors.second;
    lv_obj_invalidate(p.background);
  }

  show(p.homeHalo, split && v.homeHalo);
  show(p.awayHalo, split && v.awayHalo);

  // Crests are shared by the live and next layouts.
  show(p.homeCrest, split);
  show(p.awayCrest, split);
  if (first || v.homeCrest != old.homeCrest) load_image(st, p.homeCrest, v.homeCrest);
  if (first || v.awayCrest != old.awayCrest) load_image(st, p.awayCrest, v.awayCrest);

  show(p.message, !live && !last);
  set_text(p.message, v.message);
  if (v.mode != old.mode || first) {
    if (next) {
      lv_obj_align(p.message, LV_ALIGN_TOP_MID, 0, kMessageTop);
    } else {
      lv_obj_align(p.message, LV_ALIGN_TOP_MID, 0, kStatusMessageTop);
    }
  }
  show(p.countdown, next);
  set_text(p.countdown, v.countdown);

  show(p.scoreArea, live || last);
  show(p.pill, live);
  show(p.score, live || last);
  show(p.homeName, live);
  show(p.awayName, live);
  set_text(p.pillLabel, v.pill);
  if (live || last) set_score_font(p, score_font(v));
  // "Last result" shares the status row's line: centred when the right group
  // (stale marker, clock, battery) leaves room, else in the left text's slot
  // (empty for a last result), else it gives way.
  bool captionFits = false;
  if (last) {
    const int32_t start = widgets::status_row_right_start(st->status);
    for (const bool centred : {true, false}) {
      if (centred) {
        lv_obj_align(p.lastCaption, LV_ALIGN_TOP_MID, 0, kLastCaptionTop);
      } else {
        lv_obj_align(p.lastCaption, LV_ALIGN_TOP_LEFT, widgets::kStatusSide, kLastCaptionTop);
      }
      lv_obj_update_layout(p.lastCaption);
      lv_area_t cap;
      lv_obj_get_coords(p.lastCaption, &cap);
      if (cap.x2 + widgets::kStatusGap < start) {
        captionFits = true;
        break;
      }
    }
  }
  show(p.lastCaption, captionFits);
  show(p.lastLine, last);
  show(p.lastWhen, last);
  set_text(p.lastOpponent, v.lastOpponent);
  set_text(p.lastWhen, v.lastWhen);
  set_text(p.scoreHome, v.scoreHome);
  set_text(p.scoreAway, v.scoreAway);
  set_text(p.homeName, v.homeName);
  set_text(p.awayName, v.awayName);

  show(p.eventStrip, live && v.hasEvent);
  set_text(p.eventCaption, v.eventCaption);
  set_text(p.eventPlayer, v.eventPlayer);
  if (first || v.eventColor != old.eventColor) {
    lv_obj_set_style_bg_color(p.eventCutout, lv_color_hex(v.eventColor), 0);
  }
  if (first || v.eventCutout != old.eventCutout) {
    if (lv_image_get_src(p.eventCutout) != nullptr) lv_image_set_src(p.eventCutout, nullptr);
    if (!v.eventCutout.empty()) image_load_into(p.eventCutout, *st->images, v.eventCutout);
  }

  st->shown = v;
  st->everApplied = true;
}

}  // namespace

// ---------------------------------------------------------------- public

std::pair<lv_color_t, lv_color_t> split_colors(lv_color_t home, lv_color_t away) {
  if (!too_close(home, away)) return {home, away};
  const lv_color_t opponent = theme().opponent;
  if (!too_close(home, opponent)) return {home, opponent};
  const lv_color_t darker = darken(home, 140);
  if (!too_close(home, darker)) return {home, darker};
  // Home is nearly black: darkening cannot separate it, so lighten instead.
  return {home, mix(home, theme().chalk, 140)};
}

std::pair<lv_color_t, lv_color_t> live_background_colors(uint32_t homePrimary, uint32_t awayPrimary) {
  // Tone first, then separate, and use the result as is: toning the clash
  // fallback again would pull it back towards home.
  return split_colors(background_tone(lv_color_hex(homePrimary)),
                      background_tone(lv_color_hex(awayPrimary)));
}

bool crest_needs_halo(lv_color_t side) { return relative_luminance(side) < kHaloMaxLuminance; }

lv_obj_t* live_create(lv_obj_t* parent, Store& store, ImageProvider& images,
                      std::function<void(std::string matchId)> onOpen) {
  auto* st = new LiveState();
  st->store = &store;
  st->images = &images;
  st->onOpen = std::move(onOpen);
  LiveParts& p = st->parts;
  const Palette& pal = theme();

  lv_obj_t* root = plain(parent);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, pal.turf, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_user_data(root, st);
  lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, st);

  p.background = plain(root);
  lv_obj_set_size(p.background, LV_PCT(100), LV_PCT(100));
  lv_obj_add_event_cb(p.background, draw_background, LV_EVENT_DRAW_MAIN, st);

  // Status row: league left, stale marker and clock right.
  st->status = widgets::status_row_create(root);
  p.statusLeague = st->status.left;
  p.statusClock = st->status.clock;
  p.staleDot = st->status.staleDot;
  p.staleLabel = st->status.staleLabel;
  p.status = st->status;

  // Halos first, so they draw below the crests.
  p.homeHalo = halo(root, kCrestInset);
  p.awayHalo = halo(root, 240 - kCrestInset - kCrestSize);
  p.homeCrest = crest(root, kCrestInset);
  p.awayCrest = crest(root, 240 - kCrestInset - kCrestSize);

  // Next-match / status message.
  p.message = label(root, &bigshoulders_28, pal.chalk);
  lv_obj_set_style_text_align(p.message, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(p.message, LV_ALIGN_TOP_MID, 0, kStatusMessageTop);
  lv_label_set_text(p.message, "Connecting...");
  p.countdown = label(root, &hanken_12, pal.chalk);
  lv_obj_set_style_text_opa(p.countdown, 190, 0);  // dim text is lost on team colours
  lv_obj_align(p.countdown, LV_ALIGN_TOP_MID, 0, kMessageTop + 38);

  // Score.
  p.score = plain(root);
  lv_obj_set_size(p.score, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(p.score, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.score, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  p.scoreHome = label(p.score, &bigshoulders_96, pal.chalk);
  p.scoreSep = label(p.score, &bigshoulders_96, pal.chalk);
  lv_obj_set_style_text_opa(p.scoreSep, 140, 0);  // opacity .55: the split shows through
  lv_label_set_text(p.scoreSep, ":");
  lv_obj_set_style_pad_hor(p.scoreSep, 2, 0);
  p.scoreAway = label(p.score, &bigshoulders_96, pal.chalk);
  for (lv_obj_t* l : {p.scoreHome, p.scoreSep, p.scoreAway}) {
    lv_obj_set_style_text_letter_space(l, -2, 0);  // letter-spacing -.02em
  }
  lv_obj_set_style_text_font(p.scoreHome, &bigshoulders_56, 0);  // forces the first layout
  set_score_font(p, &bigshoulders_96);

  // Minute pill, above the score.
  p.pill = plain(root);
  lv_obj_set_size(p.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(p.pill, pal.amber, 0);
  lv_obj_set_style_bg_opa(p.pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p.pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(p.pill, 9, 0);
  lv_obj_set_style_pad_ver(p.pill, 0, 0);
  lv_obj_set_style_pad_column(p.pill, 5, 0);
  lv_obj_set_flex_flow(p.pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(p.pill, LV_ALIGN_TOP_MID, 0, kPillTop);
  p.pillDot = dot(p.pill, 6, lv_color_hex(0xBB0000));
  p.pillLabel = label(p.pill, &bigshoulders_16, lv_color_hex(0x241A00));

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, p.pillDot);
  lv_anim_set_exec_cb(&a, pulse_exec);
  lv_anim_set_values(&a, LV_OPA_COVER, 51);  // 1 -> .2
  lv_anim_set_duration(&a, 600);
  lv_anim_set_playback_duration(&a, 600);  // 1.2 s per pulse
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);

  // Team names under the crests.
  p.homeName = label(root, kNameFont, pal.chalk);
  p.awayName = label(root, kNameFont, pal.chalk);
  for (lv_obj_t* l : {p.homeName, p.awayName}) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, kNameWidth);
    lv_obj_set_style_text_letter_space(l, 0, 0);
  }
  lv_obj_set_pos(p.homeName, kNamesInset, kNamesTop);
  lv_obj_set_style_text_align(p.homeName, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_pos(p.awayName, 240 - kNamesInset - kNameWidth, kNamesTop);
  lv_obj_set_style_text_align(p.awayName, LV_TEXT_ALIGN_RIGHT, 0);

  // Tap target over crests, pill, score and names.
  p.scoreArea = plain(root);
  lv_obj_add_flag(p.scoreArea, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(p.scoreArea, 0, kCrestTop);
  lv_obj_set_size(p.scoreArea, 240, kStripTop - kCrestTop - 2);
  lv_obj_add_event_cb(p.scoreArea, on_score_clicked, LV_EVENT_CLICKED, st);

  // Glass strip with the latest event.
  p.eventStrip = plain(root);
  lv_obj_set_pos(p.eventStrip, kStripInset, kStripTop);
  lv_obj_set_size(p.eventStrip, 240 - 2 * kStripInset, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(p.eventStrip, pal.chalk, 0);
  lv_obj_set_style_bg_opa(p.eventStrip, 20, 0);  // rgba(241,239,228,.08)
  lv_obj_set_style_border_color(p.eventStrip, pal.chalk, 0);
  lv_obj_set_style_border_opa(p.eventStrip, 31, 0);  // .12
  lv_obj_set_style_border_width(p.eventStrip, 1, 0);
  lv_obj_set_style_radius(p.eventStrip, 14, 0);
  lv_obj_set_style_pad_top(p.eventStrip, 6, 0);
  lv_obj_set_style_pad_bottom(p.eventStrip, 6, 0);
  lv_obj_set_style_pad_left(p.eventStrip, 6, 0);
  lv_obj_set_style_pad_right(p.eventStrip, 10, 0);
  lv_obj_set_style_pad_column(p.eventStrip, 10, 0);
  lv_obj_set_flex_flow(p.eventStrip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.eventStrip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  p.eventCutout = lv_image_create(p.eventStrip);
  lv_obj_set_size(p.eventCutout, 40, 40);
  lv_image_set_inner_align(p.eventCutout, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_style_radius(p.eventCutout, 10, 0);
  lv_obj_set_style_clip_corner(p.eventCutout, true, 0);
  lv_obj_set_style_bg_opa(p.eventCutout, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(p.eventCutout, pal.accent, 0);

  lv_obj_t* texts = plain(p.eventStrip);
  lv_obj_set_flex_grow(texts, 1);
  lv_obj_set_height(texts, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
  p.eventCaption = label(texts, &hanken_10, pal.dim);
  p.eventPlayer = label(texts, &hanken_14, pal.chalk);
  for (lv_obj_t* l : {p.eventCaption, p.eventPlayer}) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
  }

  // Last result card, favourite on the left: the caption centred on the
  // status row line, the score where the live score goes, then "vs CHE [FT]"
  // and how long ago.
  p.lastCaption = label(root, &hanken_10, pal.dim);
  lv_label_set_text(p.lastCaption, "Last result");
  lv_obj_align(p.lastCaption, LV_ALIGN_TOP_MID, 0, kLastCaptionTop);
  p.lastLine = plain(root);
  lv_obj_set_size(p.lastLine, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(p.lastLine, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.lastLine, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(p.lastLine, 8, 0);
  lv_obj_align(p.lastLine, LV_ALIGN_TOP_MID, 0, kLastLineTop);
  p.lastOpponent = label(p.lastLine, &bigshoulders_28, pal.chalk);
  lv_obj_t* ft = plain(p.lastLine);
  lv_obj_set_size(ft, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_border_color(ft, pal.chalk, 0);
  lv_obj_set_style_border_opa(ft, 110, 0);
  lv_obj_set_style_border_width(ft, 1, 0);
  lv_obj_set_style_radius(ft, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(ft, 7, 0);
  lv_obj_set_style_pad_ver(ft, 1, 0);
  p.lastFt = label(ft, &hanken_10, pal.chalk);
  lv_label_set_text(p.lastFt, "FT");
  p.lastWhen = label(root, &hanken_12, pal.chalk);
  lv_obj_set_style_text_opa(p.lastWhen, 190, 0);  // as the countdown: dim is lost on team colours
  lv_obj_align(p.lastWhen, LV_ALIGN_TOP_MID, 0, kLastWhenTop);

  // Settings gear at the top right, above everything else; shown once the
  // host listens (live_set_on_settings).
  p.gear = plain(root);
  lv_obj_add_flag(p.gear, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(p.gear, kGearSize, kGearSize);
  lv_obj_align(p.gear, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(p.gear, pal.chalk, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(p.gear, 30, LV_STATE_PRESSED);
  lv_obj_set_style_radius(p.gear, LV_RADIUS_CIRCLE, 0);
  lv_obj_t* gearIcon = label(p.gear, &lv_font_montserrat_14, pal.chalk);
  lv_obj_set_style_text_opa(gearIcon, 200, 0);
  lv_label_set_text(gearIcon, LV_SYMBOL_SETTINGS);
  lv_obj_align(gearIcon, LV_ALIGN_TOP_MID, 0, kGearIconTop);
  lv_obj_add_event_cb(p.gear, on_gear_clicked, LV_EVENT_CLICKED, st);
  lv_obj_add_flag(p.gear, LV_OBJ_FLAG_HIDDEN);

  // Initial state: Connecting, nothing else visible until the first refresh.
  for (lv_obj_t* o : {p.homeCrest, p.awayCrest, p.countdown, p.scoreArea, p.pill, p.score,
                      p.homeName, p.awayName, p.eventStrip, p.lastCaption, p.lastLine, p.lastWhen}) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  return root;
}

void live_set_on_settings(lv_obj_t* screen, std::function<void()> onSettings) {
  if (live_parts(screen) == nullptr) return;
  LiveState* st = state_of(screen);
  const bool on = static_cast<bool>(onSettings);
  st->onSettings = std::move(onSettings);
  show(st->parts.gear, on);  // in the room the status row leaves on every screen
}

void live_refresh(lv_obj_t* screen, int64_t now) {
  LiveState* st = state_of(screen);
  if (st == nullptr) return;
  widgets::crest_sync(st->parts.homeCrest);
  widgets::crest_sync(st->parts.awayCrest);
  const View v = compute_view(*st->store, now);
  if (st->everApplied && v == st->shown) return;
  apply(st, v);
}

const LiveParts* live_parts(lv_obj_t* screen) {
  if (screen == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(screen); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(screen, i)) == on_delete) {
      return &state_of(screen)->parts;
    }
  }
  return nullptr;
}
