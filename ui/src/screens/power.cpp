#include "screens/power.h"

#include "fonts.h"
#include "power_policy.h"
#include "screens/widgets.h"
#include "theme.h"

namespace {

constexpr int32_t kHintW = 200;
constexpr int32_t kHintH = 64;
constexpr int32_t kBarW = 160;
constexpr int32_t kBarH = 6;

void set_bar(void* bar, int32_t v) { lv_bar_set_value(static_cast<lv_obj_t*>(bar), v, LV_ANIM_OFF); }

}  // namespace

lv_obj_t* power_hint_create(lv_obj_t* parent, lv_obj_t** barOut) {
  const Palette& pal = theme();
  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, kHintW, kHintH);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_bg_color(card, pal.panel, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, pal.line, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);  // touches go on to the screen

  lv_obj_t* text = widgets::label(card, &hanken_14, pal.chalk);
  lv_label_set_text(text, power_key::kHoldHintText);
  lv_obj_align(text, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t* bar = lv_bar_create(card);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, kBarW, kBarH);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, pal.line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, pal.chalk, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 0, LV_ANIM_OFF);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, bar);
  lv_anim_set_exec_cb(&a, set_bar);
  lv_anim_set_values(&a, 0, 100);
  lv_anim_set_duration(&a, static_cast<uint32_t>(power_key::kLongPressMs - power_key::kHintAfterMs));
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_start(&a);  // deleted with the bar

  if (barOut != nullptr) *barOut = bar;
  return card;
}

lv_obj_t* powering_off_create(lv_obj_t* parent) {
  const Palette& pal = theme();
  lv_obj_t* v = lv_obj_create(parent);
  lv_obj_remove_style_all(v);
  lv_obj_set_pos(v, 0, 0);
  lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(v, pal.turf, 0);
  lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
  lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* icon = widgets::label(v, &lv_font_montserrat_14, pal.dim);
  lv_label_set_text(icon, LV_SYMBOL_POWER);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -24);

  lv_obj_t* text = widgets::label(v, &bigshoulders_28, pal.chalk);
  lv_label_set_text(text, power_key::kPoweringOffText);
  lv_obj_align(text, LV_ALIGN_CENTER, 0, 8);
  return v;
}
