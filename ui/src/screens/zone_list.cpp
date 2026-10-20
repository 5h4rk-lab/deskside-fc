#include "screens/zone_list.h"

#include <utility>

#include "fonts.h"
#include "format.h"
#include "screens/widgets.h"
#include "theme.h"
#include "timezone.h"

namespace {

using widgets::label;
using widgets::plain;

constexpr int32_t kRowHeight = 40;
constexpr int32_t kRowPad = 12;
constexpr int32_t kTickSlot = 18;  // tick width plus its gap

struct ZoneListState {
  std::function<void(std::string)> onPick;
  std::function<void()> onBack;
  ZoneListParts parts;
};

void on_back(lv_event_t* e) {
  auto* st = static_cast<ZoneListState*>(lv_event_get_user_data(e));
  const std::function<void()> f = st->onBack;
  if (f) f();
}

void on_row(lv_event_t* e) {
  auto* st = static_cast<ZoneListState*>(lv_event_get_user_data(e));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (const ZoneRowParts& r : st->parts.rows) {
    if (r.row != target) continue;
    const std::string id = r.id;  // the host may delete this list
    const std::function<void(std::string)> f = st->onPick;
    if (f) f(id);
    return;
  }
}

void on_delete(lv_event_t* e) { delete static_cast<ZoneListState*>(lv_event_get_user_data(e)); }

}  // namespace

lv_obj_t* zone_list_create(lv_obj_t* parent, const std::string& currentId, int64_t now,
                           std::function<void(std::string zoneId)> onPick,
                           std::function<void()> onBack) {
  auto* st = new ZoneListState();
  st->onPick = std::move(onPick);
  st->onBack = std::move(onBack);
  ZoneListParts& p = st->parts;
  const Palette& pal = theme();

  lv_obj_t* root = plain(parent);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, pal.turf, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_user_data(root, st);
  lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, st);
  lv_obj_update_layout(root);

  const widgets::PageHeader header = widgets::page_header_create(root, "Time zone");
  p.back = header.back;
  p.title = header.title;
  lv_obj_add_event_cb(p.back, on_back, LV_EVENT_CLICKED, st);

  p.list = widgets::scroll_column(root);
  lv_obj_set_pos(p.list, 0, widgets::kPageBodyY);
  lv_obj_set_size(p.list, LV_PCT(100), lv_obj_get_height(root) - widgets::kPageBodyY);

  lv_obj_t* current = nullptr;
  for (const Zone& z : zones()) {
    ZoneRowParts r;
    r.id = z.id;
    r.row = widgets::card(p.list, true);
    lv_obj_set_size(r.row, LV_PCT(100), kRowHeight);
    lv_obj_add_event_cb(r.row, on_row, LV_EVENT_CLICKED, st);

    r.tick = widgets::tick(r.row);
    lv_obj_align(r.tick, LV_ALIGN_RIGHT_MID, -kRowPad, 0);
    // Two lines, like the team picker: the place names, the offset under them.
    r.label = label(r.row, &hanken_14, pal.chalk);
    lv_label_set_long_mode(r.label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(r.label, 224 - 2 * kRowPad - kTickSlot, hanken_14.line_height);
    lv_label_set_text(r.label, z.label);
    lv_obj_set_pos(r.label, kRowPad, 3);
    r.offset = label(r.row, &hanken_10, pal.dim);
    lv_label_set_text(r.offset, utc_offset_text(offset_seconds(z, now)).c_str());
    lv_obj_set_pos(r.offset, kRowPad, 22);

    const bool selected = currentId == z.id;
    if (!selected) lv_obj_add_flag(r.tick, LV_OBJ_FLAG_HIDDEN);
    if (selected) {
      lv_obj_set_style_text_color(r.offset, pal.chalk, 0);
      current = r.row;
    }
    p.rows.push_back(std::move(r));
  }

  // Open with the current zone in view.
  if (current != nullptr) {
    lv_obj_update_layout(root);
    lv_obj_scroll_to_view(current, LV_ANIM_OFF);
  }
  return root;
}

const ZoneListParts* zone_list_parts(lv_obj_t* screen) {
  if (screen == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(screen); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(screen, i)) == on_delete) {
      return &static_cast<ZoneListState*>(lv_obj_get_user_data(screen))->parts;
    }
  }
  return nullptr;
}
