#include "screens/battery_banner.h"

#include "fonts.h"
#include "screens/widgets.h"
#include "theme.h"

namespace {

struct BannerState {
  BatteryBannerParts parts;
  lv_timer_t* timer = nullptr;  // the lifetime; gone once it has fired
};

void on_delete(lv_event_t* e) {
  auto* st = static_cast<BannerState*>(lv_event_get_user_data(e));
  if (st->timer != nullptr) lv_timer_delete(st->timer);
  delete st;
}

void on_lifetime(lv_timer_t* t) {
  auto* banner = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
  auto* st = static_cast<BannerState*>(lv_obj_get_user_data(banner));
  st->timer = nullptr;  // a one-shot timer: LVGL deletes it after this call
  lv_obj_delete(banner);
}

void on_tap(lv_event_t* e) {
  auto* banner = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  // Inside the banner's own event: hidden now, deleted on the next cycle.
  lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(banner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_delete_async(banner);
}

}  // namespace

lv_obj_t* battery_banner_create(lv_obj_t* parent, const Alert& alert) {
  const Palette& pal = theme();
  const bool critical = alert.level == Alert::Level::Critical;
  const lv_color_t colour = critical ? pal.warn : pal.amber;

  auto* st = new BannerState();
  st->parts.level = alert.level;

  lv_obj_t* b = widgets::plain(parent);
  lv_obj_set_pos(b, 0, 0);
  lv_obj_set_size(b, LV_PCT(100), kBatteryBannerHeight);
  lv_obj_set_style_bg_color(b, pal.tabBar, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  // A hairline in the alert colour along the bottom edge.
  lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, colour, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_user_data(b, st);
  lv_obj_add_event_cb(b, on_delete, LV_EVENT_DELETE, st);
  lv_obj_add_event_cb(b, on_tap, LV_EVENT_CLICKED, nullptr);

  // A warning sign and the text, centred together above the hairline.
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(b, 6, 0);
  lv_obj_set_style_pad_bottom(b, 1, 0);
  st->parts.icon = widgets::label(b, &lv_font_montserrat_14, colour);
  lv_label_set_text(st->parts.icon, LV_SYMBOL_WARNING);
  st->parts.text = widgets::label(b, &hanken_10, colour);
  lv_label_set_text(st->parts.text, alert.text);

  st->timer = lv_timer_create(on_lifetime, alert.durationMs, b);
  lv_timer_set_repeat_count(st->timer, 1);
  return b;
}

const BatteryBannerParts* battery_banner_parts(lv_obj_t* banner) {
  if (banner == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(banner); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(banner, i)) == on_delete) {
      return &static_cast<BannerState*>(lv_obj_get_user_data(banner))->parts;
    }
  }
  return nullptr;
}
