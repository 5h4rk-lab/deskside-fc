#include "app.h"

#include "fonts.h"
#include "theme.h"

namespace {

struct TabSpec {
  const char* label;
  const char* icon;  // LVGL symbol, rendered with the built-in symbol font
  const char* hint;
};

const TabSpec kTabs[] = {
    {"Live", LV_SYMBOL_PLAY, "no live match"},
    {"Matches", LV_SYMBOL_LIST, "no matches yet"},
    {"News", LV_SYMBOL_FILE, "no news yet"},
};

// Accent follows the favourite team's colour; later tasks make this settable.
lv_color_t accent_color() { return lv_color_hex(theme::kDefaultAccent); }

// Child 0 of a tab button is the text label (created by the tabview), child 1
// is the icon label added below.
lv_obj_t* tab_icon(lv_obj_t* button) { return lv_obj_get_child(button, 1); }

// The active tab shows an accent icon, inactive tabs a dim one. Text colour
// follows the button's checked state through the style defined in style_tab_button.
void refresh_icons(lv_obj_t* tabview) {
  lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
  const uint32_t n = lv_obj_get_child_count(bar);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* button = lv_obj_get_child(bar, static_cast<int32_t>(i));
    const bool active = lv_obj_has_state(button, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(
        tab_icon(button), active ? accent_color() : lv_color_hex(theme::kDim), 0);
  }
}

void on_tab_changed(lv_event_t* e) {
  refresh_icons(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

void style_tab_button(lv_obj_t* button, lv_obj_t* tabview) {
  // The default theme styles the checked/pressed/focused tab button (blue
  // fill, bottom border). LVGL prefers the style matching the most specific
  // state, so the plain-state override alone is not enough: flatten each one.
  const lv_state_t kStates[] = {
      LV_STATE_DEFAULT,
      LV_STATE_CHECKED,
      LV_STATE_PRESSED,
      LV_STATE_CHECKED | LV_STATE_PRESSED,
      LV_STATE_FOCUSED,
      LV_STATE_CHECKED | LV_STATE_FOCUSED,
      LV_STATE_FOCUS_KEY,
      LV_STATE_CHECKED | LV_STATE_FOCUS_KEY,
  };
  for (lv_state_t st : kStates) {
    const lv_style_selector_t sel = static_cast<lv_style_selector_t>(LV_PART_MAIN) | st;
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, sel);
    lv_obj_set_style_border_width(button, 0, sel);
    lv_obj_set_style_outline_width(button, 0, sel);
    lv_obj_set_style_shadow_width(button, 0, sel);
  }
  lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(button, &hanken_10, 0);
  lv_obj_set_style_text_color(button, lv_color_hex(theme::kDim), 0);
  lv_obj_set_style_text_color(button, lv_color_hex(theme::kChalk), LV_STATE_CHECKED);

  lv_obj_t* label = lv_obj_get_child(button, 0);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);

  lv_obj_t* icon = lv_label_create(button);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 6);

  // The tabview has already updated the checked state when this runs (our
  // handler is added after its own), so the icon colour is current.
  lv_obj_add_event_cb(button, on_tab_changed, LV_EVENT_CLICKED, tabview);
}

void build_tab(lv_obj_t* tab, const TabSpec& spec) {
  lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(tab, 0, 0);
  lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* title = lv_label_create(tab);
  lv_label_set_text(title, spec.label);
  lv_obj_set_style_text_font(title, &bigshoulders_40, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(theme::kChalk), 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -14);

  lv_obj_t* hint = lv_label_create(tab);
  lv_label_set_text(hint, spec.hint);
  lv_obj_set_style_text_font(hint, &hanken_12, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(theme::kDim), 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 18);
}

}  // namespace

void app_create(lv_obj_t* root) {
  lv_obj_set_style_bg_color(root, lv_color_hex(theme::kTurf), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  lv_obj_t* tabview = lv_tabview_create(root);
  lv_tabview_set_tab_bar_position(tabview, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(tabview, theme::kTabBarHeight);
  lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(tabview, lv_color_hex(theme::kTurf), 0);
  lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(tabview, 0, 0);

  lv_obj_t* content = lv_tabview_get_content(tabview);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
  lv_obj_set_style_bg_color(bar, lv_color_hex(theme::kTabBar), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(bar, lv_color_hex(theme::kLine), 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);

  for (const TabSpec& spec : kTabs) {
    lv_obj_t* tab = lv_tabview_add_tab(tabview, spec.label);
    build_tab(tab, spec);
  }

  const uint32_t n = lv_obj_get_child_count(bar);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* button = lv_obj_get_child(bar, static_cast<int32_t>(i));
    style_tab_button(button, tabview);
    lv_label_set_text(tab_icon(button), kTabs[i].icon);
  }
  lv_obj_add_event_cb(tabview, on_tab_changed, LV_EVENT_VALUE_CHANGED, tabview);

  lv_tabview_set_active(tabview, 0, LV_ANIM_OFF);
  refresh_icons(tabview);
}
