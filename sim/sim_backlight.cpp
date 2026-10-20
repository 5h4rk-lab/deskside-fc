#include "sim_backlight.h"

lv_opa_t sim_backlight_opa(uint8_t percent) {
  if (percent > 100) percent = 100;
  return static_cast<lv_opa_t>((100 - percent) * 255 / 100 * 85 / 100);
}

SimBacklight::SimBacklight() {
  shade_ = lv_obj_create(lv_layer_sys());
  lv_obj_remove_style_all(shade_);
  lv_obj_set_size(shade_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(shade_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(shade_, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(shade_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(shade_, LV_OBJ_FLAG_SCROLLABLE);
}

void SimBacklight::set_percent(uint8_t percent) {
  percent_ = percent > 100 ? 100 : percent;
  lv_obj_set_style_bg_opa(shade_, sim_backlight_opa(percent_), 0);
}
