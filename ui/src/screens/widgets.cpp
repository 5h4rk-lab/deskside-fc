#include "screens/widgets.h"

#include "fonts.h"
#include "format.h"
#include "theme.h"

namespace widgets {

namespace {

constexpr lv_opa_t kPlaceholderOpa = 24;
constexpr size_t kAbbrMaxLength = 4;
constexpr size_t kFallbackLength = 3;

size_t code_points(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++n;
  }
  return n;
}

// First `count` code points of `s`.
std::string prefix(const std::string& s, size_t count) {
  size_t n = 0;
  size_t i = 0;
  for (; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;  // continuation byte
    if (n == count) break;
    ++n;
  }
  return s.substr(0, i);
}

}  // namespace

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, "");
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

void set_text(lv_obj_t* label, const std::string& text) {
  if (text != lv_label_get_text(label)) lv_label_set_text(label, text.c_str());
}

void show(lv_obj_t* obj, bool visible) {
  if (visible == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
    if (visible) {
      lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

std::string team_abbr(const Team& t) {
  const size_t len = code_points(t.abbr);
  if (len > 0 && len <= kAbbrMaxLength) return t.abbr;
  const std::string fallback = to_upper(prefix(t.name, kFallbackLength));
  return fallback.empty() ? t.abbr.substr(0, 3) : fallback;
}

lv_obj_t* crest_image(lv_obj_t* parent, int32_t size) {
  lv_obj_t* img = lv_image_create(parent);
  lv_obj_set_size(img, size, size);
  // Helper images are square at the requested size; stretch only matters for
  // the simulator's larger local assets.
  lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
  lv_obj_set_style_radius(img, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(img, theme().chalk, 0);
  lv_obj_set_style_bg_opa(img, kPlaceholderOpa, 0);
  return img;
}

void crest_sync(lv_obj_t* img) {
  const lv_opa_t want = lv_image_get_src(img) != nullptr ? static_cast<lv_opa_t>(LV_OPA_TRANSP) : kPlaceholderOpa;
  if (lv_obj_get_style_bg_opa(img, 0) != want) lv_obj_set_style_bg_opa(img, want, 0);
}

void crest_load(lv_obj_t* img, ImageProvider& images, const std::string& path) {
  if (lv_image_get_src(img) != nullptr) lv_image_set_src(img, nullptr);
  if (!path.empty()) image_load_into(img, images, path);
  crest_sync(img);
}

void digit_metrics(const lv_font_t* font, int32_t& top, int32_t& height) {
  lv_font_glyph_dsc_t g;
  if (lv_font_get_glyph_dsc(font, &g, '0', 0)) {
    height = g.box_h;
    top = font->line_height - font->base_line - g.box_h - g.ofs_y;
  } else {
    height = font->line_height;
    top = 0;
  }
}

PageHeader page_header_create(lv_obj_t* parent, const char* title) {
  const Palette& pal = theme();
  PageHeader h;
  h.back = plain(parent);
  lv_obj_add_flag(h.back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(h.back, 0, 0);
  lv_obj_set_size(h.back, 80, 28);
  lv_obj_t* text = label(h.back, &hanken_10, pal.chalk);
  lv_label_set_text(text, "< Back");
  lv_obj_set_pos(text, kPageSide, 4);
  h.title = label(parent, &bigshoulders_28, pal.chalk);
  lv_label_set_text(h.title, title);
  lv_obj_set_pos(h.title, kPageSide, kPageTitleY);
  return h;
}

lv_obj_t* card(lv_obj_t* parent, bool clickable) {
  const Palette& pal = theme();
  lv_obj_t* c = plain(parent);
  lv_obj_set_style_bg_color(c, pal.panel, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, kCardRadius, 0);
  if (clickable) {
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, mix(pal.panel, pal.chalk, 18), LV_STATE_PRESSED);
  }
  return c;
}

lv_obj_t* scroll_column(lv_obj_t* parent) {
  lv_obj_t* list = plain(parent);
  lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_left(list, kListSide, 0);
  lv_obj_set_style_pad_right(list, kListSide, 0);
  lv_obj_set_style_pad_bottom(list, 10, 0);
  lv_obj_set_style_pad_row(list, kListGap, 0);
  return list;
}

lv_obj_t* tick(lv_obj_t* parent) {
  lv_obj_t* t = label(parent, &lv_font_montserrat_14, accent_mark(theme().accent));
  lv_label_set_text(t, LV_SYMBOL_OK);
  return t;
}

void style_text_field(lv_obj_t* ta, int32_t height) {
  const Palette& pal = theme();
  lv_obj_remove_style_all(ta);
  lv_obj_set_style_bg_color(ta, pal.panel, 0);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ta, kCardRadius, 0);
  lv_obj_set_style_border_color(ta, pal.line, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, mix(pal.line, pal.chalk, 70), LV_STATE_FOCUSED);
  lv_obj_set_style_pad_hor(ta, 12, 0);
  lv_obj_set_style_pad_top(ta, (height - 2 - hanken_14.line_height) / 2, 0);
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

StatusInfo status_info(const Store& store, int64_t now) {
  StatusInfo s;
  s.clock = hh_mm(now + store.tz_offset());
  s.connecting = !store.has_data();
  if (!s.connecting) {
    s.stale = store.stale(now);
    if (s.stale) s.staleText = stale_text(now - store.last_success());
  }
  return s;
}

StatusRow status_row_create(lv_obj_t* parent) {
  const Palette& pal = theme();
  StatusRow r;
  r.left = label(parent, &hanken_10, pal.dim);
  lv_obj_set_pos(r.left, 10, 4);
  // Fixed-size box with right-aligned children: a content-sized flex row does
  // not always re-measure when a label's text changes.
  lv_obj_t* right = plain(parent);
  lv_obj_set_size(right, 150, 13);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right, 4, 0);
  lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -10, 4);
  r.staleDot = dot(right, 5, pal.amber);
  r.staleLabel = label(right, &hanken_10, pal.amber);
  lv_obj_set_style_pad_right(r.staleLabel, 3, 0);
  r.clock = label(right, &hanken_10, pal.dim);
  // The mockup's "09:42 ●": a quiet dot while data is fresh; when stale the
  // amber dot before the age takes its place.
  r.freshDot = dot(right, 4, pal.dim);
  for (lv_obj_t* o : {r.staleDot, r.staleLabel, r.freshDot}) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  return r;
}

void status_row_apply(const StatusRow& row, const std::string& left, const StatusInfo& info) {
  set_text(row.left, left);
  set_text(row.clock, info.clock);
  show(row.staleDot, info.stale);
  show(row.staleLabel, info.stale);
  show(row.freshDot, !info.stale && !info.connecting);
  set_text(row.staleLabel, info.staleText);
}

}  // namespace widgets
