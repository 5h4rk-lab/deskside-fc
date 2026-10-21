#include "goal.h"

#include <math.h>

#include <algorithm>
#include <optional>
#include <string>

#include "fonts.h"
#include "format.h"
#include "theme.h"
#include "widgets.h"

using namespace goal;

// ------------------------------------------------------------ pure helpers

std::optional<Scorer> latest_scorer(const MatchDetail& detail, Side side) {
  const MatchEvent* last = nullptr;
  int lastKey = 0;
  for (const MatchEvent& ev : detail.events) {
    if (ev.type != EventType::Goal || ev.team != side) continue;
    const int key = minute_sort_key(ev.minute);
    if (last == nullptr || key >= lastKey) {  // >=: equal minutes keep input order
      last = &ev;
      lastKey = key;
    }
  }
  if (last == nullptr) return std::nullopt;

  Scorer s;
  s.name = last->player;
  const Lineup& lineup = side == Side::Home ? detail.home : detail.away;
  for (const LineupPlayer& p : lineup.players) {
    if (p.name == s.name && !p.photo.empty()) {
      s.photo = p.photo;
      break;
    }
  }
  if (s.photo.empty()) s.photo = "/api/cutout?name=" + url_encode(s.name) + "&w=96&h=96";
  return s;
}

std::optional<Scorer> scorer_for_goal(const MatchDetail& detail, const GoalEvent& event) {
  // A detail fetched just after the scoreboard moved often predates the goal
  // (feed lag, helper cache): its last goal of that side is then the previous
  // one. Only trust it once it has as many goals as the side has scored.
  const int score = event.side == Side::Home ? event.homeScore : event.awayScore;
  int goals = 0;
  for (const MatchEvent& ev : detail.events) {
    if (ev.type == EventType::Goal && ev.team == event.side) ++goals;
  }
  if (goals < score) return std::nullopt;
  return latest_scorer(detail, event.side);
}

std::string goal_headline(const GoalEvent& event) { return event.fullTime ? "WIN!" : "GOAL!"; }

std::string goal_score_line(const GoalEvent& event) {
  const std::string score = event.homeAbbr + " " + std::to_string(event.homeScore) + " : " +
                            std::to_string(event.awayScore) + " " + event.awayAbbr;
  const char* kSep = "  \xC2\xB7  ";  // middle dot between two spaces each side
  if (event.fullTime) return std::string("Full time") + kSep + score;
  if (event.clock.empty()) return score;
  return event.clock + kSep + score;
}

std::string goal_waiting_line(const GoalEvent& event) {
  return (event.side == Side::Home ? event.homeAbbr : event.awayAbbr) + " scored";
}

lv_color_t goal_background(lv_color_t brand) {
  const lv_color_t c = background_tone(brand);
  return relative_luminance(c) > kLightAccent ? darken(c, kLightDarken) : c;
}

// ------------------------------------------------------------ overlay

namespace {

constexpr int32_t kStampTop = 18;        // top of the headline box
constexpr int32_t kShadowOffset = 4;     // headline shadow, straight down
constexpr lv_opa_t kShadowOpa = 64;      // 25 percent
constexpr int32_t kStripHeight = 96;     // runs to the bottom edge
constexpr int32_t kNameBottom = 26;      // name baseline box, from the bottom
constexpr int32_t kLineBottom = 10;      // score line box, from the bottom
constexpr lv_opa_t kStripOpa = 166;      // 65 percent black at the bottom
constexpr int32_t kRayRadius = 320;      // reaches past every corner
constexpr float kRayCentreY = 0.715f;    // fraction of the height (mockup: 62 % of a 180 % box)
constexpr int32_t kTextWidth = 224;
constexpr uint8_t kSilhouetteDarken = 70;
constexpr int32_t kConfettiMargin = 12;  // pieces wrap this far outside the screen

const lv_color_t kAmber = lv_color_hex(0xFFC857);

struct ConfettiSpec {
  int16_t x;       // start x
  int16_t y;       // start y
  uint8_t size;    // px
  bool round;      // dot, else a tumbling square
  uint8_t colour;  // 0 chalk, 1 amber, 2 white
  uint8_t speed;   // fall speed, px/s
  uint8_t drift;   // sideways amplitude, px
  uint16_t sway;   // sideways period, ms
  uint16_t phase;  // degrees
};

// Hand-placed like the mockup's dots: spread over the sky, few over the face.
const ConfettiSpec kConfetti[kConfettiCount] = {
    {43, 70, 6, true, 2, 34, 8, 2600, 0},      {192, 45, 7, true, 1, 28, 6, 3100, 90},
    {158, 110, 4, true, 2, 40, 5, 2300, 200},   {24, 192, 6, false, 1, 31, 9, 2900, 40},
    {216, 176, 6, true, 2, 37, 7, 2700, 300},   {72, 22, 5, false, 1, 26, 6, 3300, 150},
    {118, 8, 5, false, 0, 44, 10, 2500, 250},   {226, 96, 5, false, 0, 30, 7, 3500, 60},
    {12, 128, 4, true, 0, 42, 6, 2200, 180},    {94, 150, 5, false, 2, 36, 8, 3000, 330},
    {176, 238, 4, true, 1, 33, 5, 2800, 120},   {58, 262, 5, false, 0, 39, 7, 2400, 270},
    {138, 58, 4, true, 1, 29, 9, 3200, 20},     {206, 286, 6, false, 1, 35, 6, 2600, 210},
};

struct State {
  GoalParts parts{};
  lv_color_t background;
  lv_timer_t* lifetime = nullptr;  // fires at the start of the fade (or the end)
  ImageProvider* images = nullptr;
  int32_t rayAngle = 0;            // 0.1 degree units
  bool moved = false;              // a motion frame has been shown
  uint32_t clockMs = 0;            // time since the start, from the motion animation
  uint32_t frameMs = 0;            // time of the shown rays and confetti (quantised)
  int32_t lastAnimValue = 0;
  bool reduced = false;
  bool dismissed = false;          // hidden, deletion scheduled
};

State* g_current = nullptr;      // the showing overlay
lv_obj_t* g_pending = nullptr;   // dismissed overlay waiting for its deletion
bool g_reduced = false;

State* state_of(lv_obj_t* overlay) { return static_cast<State*>(lv_obj_get_user_data(overlay)); }

void delete_pending(void* obj) {
  if (g_pending == obj) g_pending = nullptr;
  lv_obj_delete(static_cast<lv_obj_t*>(obj));
}

void on_overlay_delete(lv_event_t* e) {
  lv_obj_t* overlay = static_cast<lv_obj_t*>(lv_event_get_target(e));
  State* st = state_of(overlay);
  lv_async_call_cancel(delete_pending, overlay);
  if (g_pending == overlay) g_pending = nullptr;
  if (g_current == st) g_current = nullptr;
  if (st->lifetime != nullptr) lv_timer_delete(st->lifetime);
  delete st;
}

// Hides the overlay now and deletes it on the next cycle: this may run inside
// one of its own events or animations.
void dismiss(State* st) {
  if (st == nullptr || st->dismissed) return;  // for example a tap during the fade
  st->dismissed = true;
  if (g_current == st) g_current = nullptr;
  if (st->lifetime != nullptr) {
    lv_timer_delete(st->lifetime);
    st->lifetime = nullptr;
  }
  lv_obj_t* overlay = st->parts.overlay;
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  if (g_pending != nullptr && g_pending != overlay) {
    lv_async_call_cancel(delete_pending, g_pending);
    lv_obj_delete(g_pending);
  }
  g_pending = overlay;
  lv_async_call(delete_pending, overlay);
}

// Deletes a pending (already hidden) overlay right away.
void flush_pending() {
  if (g_pending == nullptr) return;
  lv_obj_t* obj = g_pending;
  g_pending = nullptr;
  lv_async_call_cancel(delete_pending, obj);
  lv_obj_delete(obj);
}

void on_tap(lv_event_t* e) { dismiss(static_cast<State*>(lv_event_get_user_data(e))); }

void on_fade_done(lv_anim_t* a) { dismiss(static_cast<State*>(lv_anim_get_user_data(a))); }

void set_opa(void* obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

void on_lifetime(lv_timer_t* t) {
  State* st = static_cast<State*>(lv_timer_get_user_data(t));
  st->lifetime = nullptr;  // one-shot: LVGL deletes it after this call
  if (st->reduced) {
    dismiss(st);
    return;
  }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, st->parts.overlay);
  lv_anim_set_exec_cb(&a, set_opa);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_duration(&a, kFadeMs);
  lv_anim_set_user_data(&a, st);
  lv_anim_set_completed_cb(&a, on_fade_done);
  lv_anim_start(&a);
}

// --- rays

void draw_rays(lv_event_t* e) {
  lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  State* st = static_cast<State*>(lv_event_get_user_data(e));
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const float cx = static_cast<float>(a.x1) + static_cast<float>(lv_area_get_width(&a)) / 2.0f;
  const float cy = static_cast<float>(a.y1) + static_cast<float>(lv_area_get_height(&a)) * kRayCentreY;

  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);
  dsc.bg_color = lv_color_white();
  dsc.bg_opa = kRayOpa;

  constexpr float kPi = 3.14159265f;
  const float period = 2.0f * kPi / static_cast<float>(kRayCount);
  const float width = period / 2.0f;  // wedges and gaps of equal width
  // Offset by half a period so the still frame leaves the top corners plain.
  const float base = static_cast<float>(st->rayAngle) * kPi / 1800.0f + width;
  const float r = static_cast<float>(kRayRadius);
  for (int k = 0; k < kRayCount; ++k) {
    const float a0 = base + static_cast<float>(k) * period;
    const float a1 = a0 + width;
    dsc.p[0].x = static_cast<lv_value_precise_t>(cx);
    dsc.p[0].y = static_cast<lv_value_precise_t>(cy);
    dsc.p[1].x = static_cast<lv_value_precise_t>(cx + r * cosf(a0));
    dsc.p[1].y = static_cast<lv_value_precise_t>(cy + r * sinf(a0));
    dsc.p[2].x = static_cast<lv_value_precise_t>(cx + r * cosf(a1));
    dsc.p[2].y = static_cast<lv_value_precise_t>(cy + r * sinf(a1));
    lv_draw_triangle(layer, &dsc);
  }
}

// --- confetti

void place_confetti(State* st) {
  const int32_t h = lv_obj_get_height(st->parts.overlay);
  const int32_t span = h + 2 * kConfettiMargin;
  const float t = static_cast<float>(st->frameMs);
  constexpr float kTwoPi = 6.2831853f;
  for (size_t i = 0; i < st->parts.confetti.size(); ++i) {
    const ConfettiSpec& c = kConfetti[i];
    lv_obj_t* piece = st->parts.confetti[i];
    const int32_t fall = static_cast<int32_t>(t * static_cast<float>(c.speed) / 1000.0f);
    const int32_t y = (c.y + kConfettiMargin + fall) % span - kConfettiMargin;
    const float swing = kTwoPi * t / static_cast<float>(c.sway) +
                        static_cast<float>(c.phase) * kTwoPi / 360.0f;
    const int32_t x = c.x + static_cast<int32_t>(static_cast<float>(c.drift) * sinf(swing));
    if (!c.round) {
      // A square seen tumbling: its width follows the turn.
      const float turn = fabsf(cosf(swing * 1.7f));
      const int32_t w = std::max<int32_t>(2, static_cast<int32_t>(c.size * turn + 0.5f));
      lv_obj_set_width(piece, w);
      lv_obj_set_pos(piece, x + (c.size - w) / 2, y);
    } else {
      lv_obj_set_pos(piece, x, y);
    }
  }
}

// One animation drives the steady motion: rays and confetti.
void on_motion(void* var, int32_t v) {
  State* st = state_of(static_cast<lv_obj_t*>(var));
  if (st == nullptr) return;
  const int32_t delta = v >= st->lastAnimValue ? v - st->lastAnimValue
                                               : v + static_cast<int32_t>(kRayTurnMs) - st->lastAnimValue;
  st->lastAnimValue = v;
  st->clockMs += static_cast<uint32_t>(delta);
  // Full rate while the entrance plays; then the rays and confetti move in
  // kIdleFrameMs steps and the screen is repainted only when they do.
  const uint32_t frame =
      st->clockMs < kEntranceMs ? st->clockMs : st->clockMs - st->clockMs % kIdleFrameMs;
  if (frame == st->frameMs && st->moved) return;
  st->moved = true;
  st->frameMs = frame;
  st->rayAngle = static_cast<int32_t>(static_cast<int64_t>(frame % kRayTurnMs) * 3600 / kRayTurnMs);
  lv_obj_invalidate(st->parts.rays);
  place_confetti(st);
}

// --- stamp and cutout

void set_scale(void* obj, int32_t v) {
  lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), v, 0);
}

void set_y(void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); }

void start_anim(void* var, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms,
                uint32_t delay, lv_anim_path_cb_t path) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, var);
  lv_anim_set_exec_cb(&a, exec);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_delay(&a, delay);
  lv_anim_set_path_cb(&a, path);
  lv_anim_start(&a);
}

// While the cutout has no picture, a plain head-and-shoulders silhouette.
void draw_silhouette(lv_event_t* e) {
  lv_obj_t* img = static_cast<lv_obj_t*>(lv_event_get_target(e));
  if (lv_image_get_src(img) != nullptr) return;
  State* st = static_cast<State*>(lv_event_get_user_data(e));
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(img, &a);
  const int32_t cx = (a.x1 + a.x2) / 2;

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = darken(st->background, kSilhouetteDarken);
  dsc.bg_opa = LV_OPA_COVER;

  dsc.radius = LV_RADIUS_CIRCLE;
  const int32_t head = 58;
  lv_area_t headArea = {cx - head / 2, a.y1 + 40, cx + head / 2 - 1, a.y1 + 40 + head - 1};
  lv_draw_rect(layer, &dsc, &headArea);

  dsc.radius = 46;
  lv_area_t body = {cx - 84, a.y1 + 110, cx + 83, a.y2 + 60};
  lv_draw_rect(layer, &dsc, &body);
}

// One centred line of text; whatever does not fit is clipped.
lv_obj_t* text_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t colour) {
  lv_obj_t* l = widgets::label(parent, font, colour);
  lv_obj_set_size(l, kTextWidth, lv_font_get_line_height(font));
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  return l;
}

// Sets the scorer line, stepping down to a smaller font for long names.
void set_name(lv_obj_t* label, const std::string& text) {
  const lv_font_t* font = &bigshoulders_28;
  if (lv_text_get_width(text.c_str(), static_cast<uint32_t>(text.size()), font, 0) > kTextWidth) {
    font = &bigshoulders_22;
  }
  if (lv_obj_get_style_text_font(label, 0) != font) lv_obj_set_style_text_font(label, font, 0);
  widgets::set_text(label, text);
}

void build(State* st, const GoalEvent& event) {
  GoalParts& p = st->parts;
  lv_obj_t* layer = lv_layer_top();
  const int32_t w = lv_display_get_horizontal_resolution(lv_obj_get_display(layer));
  const int32_t h = lv_display_get_vertical_resolution(lv_obj_get_display(layer));
  const lv_color_t chalk = theme().chalk;

  p.overlay = widgets::plain(layer);
  lv_obj_set_size(p.overlay, w, h);
  lv_obj_set_pos(p.overlay, 0, 0);
  lv_obj_set_style_bg_color(p.overlay, st->background, 0);
  lv_obj_set_style_bg_opa(p.overlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(p.overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(p.overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_user_data(p.overlay, st);
  lv_obj_add_event_cb(p.overlay, on_tap, LV_EVENT_CLICKED, st);
  lv_obj_add_event_cb(p.overlay, on_overlay_delete, LV_EVENT_DELETE, nullptr);

  p.rays = widgets::plain(p.overlay);
  lv_obj_set_size(p.rays, w, h);
  lv_obj_add_event_cb(p.rays, draw_rays, LV_EVENT_DRAW_MAIN, st);

  if (!st->reduced) {
    for (int i = 0; i < kConfettiCount; ++i) {
      const ConfettiSpec& c = kConfetti[i];
      const lv_color_t colour = c.colour == 0 ? chalk : c.colour == 1 ? kAmber : lv_color_white();
      lv_obj_t* piece = c.round ? widgets::dot(p.overlay, c.size, colour) : widgets::plain(p.overlay);
      if (!c.round) {
        lv_obj_set_size(piece, c.size, c.size);
        lv_obj_set_style_bg_color(piece, colour, 0);
        lv_obj_set_style_bg_opa(piece, LV_OPA_COVER, 0);
      }
      lv_obj_set_pos(piece, c.x, c.y);
      p.confetti.push_back(piece);
    }
  }

  // Headline and its shadow share one transformed box: one layer per frame.
  p.stamp = widgets::plain(p.overlay);
  lv_obj_set_size(p.stamp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  p.headlineShadow = widgets::label(p.stamp, &bigshoulders_72, lv_color_black());
  lv_obj_set_style_text_opa(p.headlineShadow, kShadowOpa, 0);
  lv_obj_set_pos(p.headlineShadow, 0, kShadowOffset);
  p.headline = widgets::label(p.stamp, &bigshoulders_72, chalk);
  lv_obj_set_pos(p.headline, 0, 0);
  const std::string headline = goal_headline(event);
  lv_label_set_text(p.headline, headline.c_str());
  lv_label_set_text(p.headlineShadow, headline.c_str());
  lv_obj_align(p.stamp, LV_ALIGN_TOP_MID, 0, kStampTop);
  lv_obj_set_style_transform_pivot_x(p.stamp, LV_PCT(50), 0);
  lv_obj_set_style_transform_pivot_y(p.stamp, LV_PCT(50), 0);
  lv_obj_set_style_transform_rotation(p.stamp, kStampRotation, 0);

  p.cutout = lv_image_create(p.overlay);
  lv_obj_set_size(p.cutout, kCutoutSize, kCutoutSize);
  lv_image_set_inner_align(p.cutout, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_x(p.cutout, (w - kCutoutSize) / 2);
  lv_obj_set_y(p.cutout, h - kCutoutSize);
  lv_obj_add_event_cb(p.cutout, draw_silhouette, LV_EVENT_DRAW_MAIN, st);

  p.strip = widgets::plain(p.overlay);
  lv_obj_set_size(p.strip, w, kStripHeight);
  lv_obj_set_pos(p.strip, 0, h - kStripHeight);
  lv_obj_set_style_bg_color(p.strip, lv_color_black(), 0);
  lv_obj_set_style_bg_grad_color(p.strip, lv_color_black(), 0);
  lv_obj_set_style_bg_grad_dir(p.strip, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_main_opa(p.strip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_grad_opa(p.strip, kStripOpa, 0);
  lv_obj_set_style_bg_opa(p.strip, LV_OPA_COVER, 0);

  p.name = text_label(p.strip, &bigshoulders_28, chalk);
  set_name(p.name, goal_waiting_line(event));
  lv_obj_align(p.name, LV_ALIGN_BOTTOM_MID, 0, -kNameBottom);
  p.scoreLine = text_label(p.strip, &hanken_12, chalk);
  lv_obj_set_style_text_opa(p.scoreLine, 217, 0);  // 85 percent
  lv_label_set_text(p.scoreLine, goal_score_line(event).c_str());
  lv_obj_align(p.scoreLine, LV_ALIGN_BOTTOM_MID, 0, -kLineBottom);
  if (event.fullTime) {
    lv_obj_add_flag(p.name, LV_OBJ_FLAG_HIDDEN);
    // No scorer line: the result takes its place, in the display face.
    lv_obj_set_style_text_font(p.scoreLine, &bigshoulders_22, 0);
    lv_obj_set_style_text_opa(p.scoreLine, LV_OPA_COVER, 0);
    lv_obj_set_height(p.scoreLine, lv_font_get_line_height(&bigshoulders_22));
    lv_obj_align(p.scoreLine, LV_ALIGN_BOTTOM_MID, 0, -kNameBottom + 4);
  }

  p.matchId = event.matchId;
  p.fullTime = event.fullTime;

  const uint32_t life = event.fullTime ? kWinMs : kGoalMs;
  st->lifetime = lv_timer_create(on_lifetime, st->reduced ? life : life - kFadeMs, st);
  lv_timer_set_repeat_count(st->lifetime, 1);

  if (st->reduced) return;

  // Stamp: big and invisible, then slammed down to size with an overshoot.
  lv_obj_set_style_transform_scale(p.stamp, kStampScaleStart, 0);
  lv_obj_set_style_opa(p.stamp, LV_OPA_TRANSP, 0);
  start_anim(p.stamp, set_scale, kStampScaleStart, LV_SCALE_NONE, kStampMs, 0,
             lv_anim_path_overshoot);
  start_anim(p.stamp, set_opa, LV_OPA_TRANSP, LV_OPA_COVER, kStampFadeInMs, 0, lv_anim_path_linear);

  // Cutout rises from below the screen.
  lv_obj_set_y(p.cutout, h);
  start_anim(p.cutout, set_y, h, h - kCutoutSize, kRiseMs, kRiseDelayMs, lv_anim_path_ease_out);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, p.overlay);
  lv_anim_set_exec_cb(&a, on_motion);
  lv_anim_set_values(&a, 0, static_cast<int32_t>(kRayTurnMs) - 1);
  lv_anim_set_duration(&a, kRayTurnMs - 1);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
}

}  // namespace

void goal_celebrate(const GoalEvent& event, ImageProvider& images) {
  // Only the favourite's goals and wins are celebrated; anything else leaves
  // a showing overlay alone.
  if (!event.favourite) return;
  if (g_current != nullptr) {
    lv_obj_t* old = g_current->parts.overlay;
    g_current = nullptr;
    lv_obj_delete(old);
  }
  flush_pending();

  auto* st = new State();
  st->images = &images;
  st->reduced = g_reduced;
  st->background = goal_background(theme().brand);  // a large surface: the brand colour
  build(st, event);
  g_current = st;
}

bool goal_active() { return g_current != nullptr; }

void goal_set_scorer(const std::string& matchId, const std::string& name,
                     const std::string& photoPath) {
  State* st = g_current;
  if (st == nullptr || st->parts.matchId != matchId) return;
  if (!st->parts.fullTime && !name.empty()) {
    set_name(st->parts.name, to_upper(name));
  }
  if (!photoPath.empty()) {
    image_load_into(st->parts.cutout, *st->images, sized(photoPath, kCutoutRequest, kCutoutRequest));
  }
}

void goal_dismiss() { dismiss(g_current); }

void goal_set_reduced_motion(bool reduced) { g_reduced = reduced; }

bool goal_reduced_motion() { return g_reduced; }

const GoalParts* goal_parts() { return g_current != nullptr ? &g_current->parts : nullptr; }
