#include "app.h"

#include <memory>
#include <utility>

#include "fonts.h"
#include "screens/detail.h"
#include "screens/live.h"
#include "screens/matches.h"
#include "theme.h"

namespace {

enum class Shown { None, Loading, Error, Detail };

// Everything the app needs after app_create(). Owned by the tab view.
struct AppState {
  Store* store = nullptr;
  ImageProvider* images = nullptr;
  DataSource* source = nullptr;
  std::function<void(const std::string&)> onOpenMatch;
  lv_obj_t* tabview = nullptr;
  lv_obj_t* live = nullptr;
  lv_obj_t* matches = nullptr;

  // Detail overlay: a sibling above the tab view, covering the tab content.
  lv_obj_t* overlay = nullptr;
  lv_obj_t* view = nullptr;  // detail or placeholder inside the overlay
  Shown shown = Shown::None;
  std::string matchId;
  uint32_t request = 0;  // id of the latest fetch; older results are only stored

  // Cleared when the tab view is deleted, for callbacks that outlive it.
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

AppState* app_state(lv_obj_t* appRoot) {
  return appRoot != nullptr ? static_cast<AppState*>(lv_obj_get_user_data(appRoot)) : nullptr;
}

DetailContext detail_context(const AppState* st) {
  DetailContext ctx;
  ctx.tzOffset = st->store->tz_offset();
  ctx.favouriteId = st->store->favourite();
  return ctx;
}

const MatchSummary* summary_for(const Store& store, const std::string& id) {
  for (const MatchSummary& m : store.matches()) {
    if (m.id == id) return &m;
  }
  return nullptr;
}

// The cached detail of the open match, with the (fresher) summary from the
// match list when there is one.
bool current_detail(const AppState* st, MatchDetail& out) {
  const MatchDetail* d = st->store->detail_for(st->matchId);
  if (d == nullptr) return false;
  out = *d;
  if (const MatchSummary* m = summary_for(*st->store, st->matchId)) out.match = *m;
  return true;
}

void close_detail(AppState* st, bool fromInside);
void fetch(AppState* st);

// Replaces the overlay's content. The old view may be running the event that
// led here (Try again), so it is hidden now and deleted on the next cycle.
void replace_view(AppState* st, lv_obj_t* view, Shown shown) {
  if (st->view != nullptr) {
    lv_obj_add_flag(st->view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(st->view);
  }
  st->view = view;
  st->shown = shown;
}

std::function<void()> back_action(AppState* st) {
  std::shared_ptr<bool> alive = st->alive;
  return [st, alive] {
    if (*alive) close_detail(st, true);
  };
}

void show_placeholder(AppState* st, DetailPlaceholder kind) {
  std::shared_ptr<bool> alive = st->alive;
  auto retry = [st, alive] {
    if (!*alive || st->overlay == nullptr) return;
    show_placeholder(st, DetailPlaceholder::Loading);
    fetch(st);
  };
  lv_obj_t* v = detail_create_placeholder(st->overlay, summary_for(*st->store, st->matchId), kind,
                                          detail_context(st), back_action(st), retry);
  replace_view(st, v, kind == DetailPlaceholder::Loading ? Shown::Loading : Shown::Error);
}

// Shows (or updates) the cached detail. False when the Store has none.
bool show_detail(AppState* st) {
  MatchDetail d;
  if (!current_detail(st, d)) return false;
  if (st->shown == Shown::Detail) {
    detail_update(st->view, d, detail_context(st));
    return true;
  }
  lv_obj_t* v = detail_create(st->overlay, d, *st->images, back_action(st), detail_context(st));
  replace_view(st, v, Shown::Detail);
  return true;
}

void fetch(AppState* st) {
  const uint32_t request = ++st->request;
  const std::string id = st->matchId;
  std::shared_ptr<bool> alive = st->alive;
  st->source->fetch_detail(id, [st, alive, request, id](bool ok, MatchDetail detail) {
    if (!*alive) return;
    if (ok) st->store->set_detail(std::move(detail));
    if (st->overlay == nullptr || st->matchId != id || st->request != request) return;
    if (ok && show_detail(st)) return;
    if (st->shown != Shown::Detail) show_placeholder(st, DetailPlaceholder::Error);
  });
}

void on_overlay_gesture(lv_event_t* e) {
  auto* st = static_cast<AppState*>(lv_event_get_user_data(e));
  const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  if (dir == LV_DIR_BOTTOM || dir == LV_DIR_RIGHT) close_detail(st, true);
}

// The overlay may be deleted with the screen after the tab view is gone.
struct OverlayLink {
  AppState* st;
  std::shared_ptr<bool> alive;
};

void on_overlay_delete(lv_event_t* e) {
  auto* link = static_cast<OverlayLink*>(lv_event_get_user_data(e));
  lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  if (*link->alive && link->st->overlay == obj) {
    link->st->overlay = nullptr;
    link->st->view = nullptr;
    link->st->shown = Shown::None;
  }
  delete link;
}

void close_detail(AppState* st, bool fromInside) {
  lv_obj_t* overlay = st->overlay;
  if (overlay == nullptr) return;
  st->overlay = nullptr;
  st->view = nullptr;
  st->shown = Shown::None;
  st->matchId.clear();
  if (fromInside) {
    // Called from an event of an object inside the overlay.
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(overlay);
  } else {
    lv_obj_delete(overlay);
  }
}

void on_app_delete(lv_event_t* e) {
  auto* st = static_cast<AppState*>(lv_event_get_user_data(e));
  *st->alive = false;
  delete st;
}

// Picking a main tab leaves the detail.
void on_tab_button(lv_event_t* e) {
  close_detail(static_cast<AppState*>(lv_event_get_user_data(e)), false);
}

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
        tab_icon(button), active ? theme().accent : theme().dim, 0);
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
  lv_obj_set_style_text_color(button, theme().dim, 0);
  lv_obj_set_style_text_color(button, theme().chalk, LV_STATE_CHECKED);

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
  lv_obj_set_style_text_color(title, theme().chalk, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -14);

  lv_obj_t* hint = lv_label_create(tab);
  lv_label_set_text(hint, spec.hint);
  lv_obj_set_style_text_font(hint, &hanken_12, 0);
  lv_obj_set_style_text_color(hint, theme().dim, 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 18);
}

}  // namespace

lv_obj_t* app_create(lv_obj_t* root, const AppDeps& deps) {
  auto* st = new AppState();
  st->store = &deps.store;
  st->images = &deps.images;
  st->source = &deps.source;
  st->onOpenMatch = deps.onOpenMatch;

  lv_obj_set_style_bg_color(root, theme().turf, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  lv_obj_t* tabview = lv_tabview_create(root);
  lv_tabview_set_tab_bar_position(tabview, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(tabview, layout::kTabBarHeight);
  lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(tabview, theme().turf, 0);
  lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(tabview, 0, 0);

  lv_obj_t* content = lv_tabview_get_content(tabview);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

  lv_obj_t* bar = lv_tabview_get_tab_bar(tabview);
  lv_obj_set_style_bg_color(bar, theme().tabBar, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(bar, theme().line, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);

  auto open_match = [st](std::string id) {
    app_open_match(st->tabview, id);
    if (st->onOpenMatch) st->onOpenMatch(id);
  };
  lv_obj_t* live = nullptr;
  lv_obj_t* matches = nullptr;
  for (const TabSpec& spec : kTabs) {
    lv_obj_t* tab = lv_tabview_add_tab(tabview, spec.label);
    if (&spec == &kTabs[0] || &spec == &kTabs[1]) {
      // Full-bleed screens that manage their own scrolling.
      lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(tab, 0, 0);
      lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);
      lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
      if (&spec == &kTabs[0]) {
        live = live_create(tab, deps.store, deps.images, open_match);
      } else {
        matches = matches_create(tab, deps.store, deps.images, open_match);
      }
    } else {
      build_tab(tab, spec);
    }
  }
  st->tabview = tabview;
  st->live = live;
  st->matches = matches;
  lv_obj_set_user_data(tabview, st);
  lv_obj_add_event_cb(tabview, on_app_delete, LV_EVENT_DELETE, st);

  const uint32_t n = lv_obj_get_child_count(bar);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* button = lv_obj_get_child(bar, static_cast<int32_t>(i));
    style_tab_button(button, tabview);
    lv_label_set_text(tab_icon(button), kTabs[i].icon);
    lv_obj_add_event_cb(button, on_tab_button, LV_EVENT_CLICKED, st);
  }
  lv_obj_add_event_cb(tabview, on_tab_changed, LV_EVENT_VALUE_CHANGED, tabview);

  lv_tabview_set_active(tabview, 0, LV_ANIM_OFF);
  refresh_icons(tabview);
  return tabview;
}

void app_refresh(lv_obj_t* appRoot, int64_t now) {
  AppState* st = app_state(appRoot);
  if (st == nullptr) return;
  if (st->live != nullptr) live_refresh(st->live, now);
  if (st->matches != nullptr) matches_refresh(st->matches, now);
  // The open detail follows the Store: newer events, published lineups, the
  // clock from the match list. Unchanged data touches nothing.
  if (st->overlay != nullptr) show_detail(st);
}

void app_open_match(lv_obj_t* appRoot, const std::string& matchId) {
  AppState* st = app_state(appRoot);
  if (st == nullptr) return;
  close_detail(st, false);

  lv_obj_t* screen = lv_obj_get_parent(appRoot);
  lv_obj_update_layout(screen);
  st->overlay = lv_obj_create(screen);
  lv_obj_remove_style_all(st->overlay);
  lv_obj_set_pos(st->overlay, 0, 0);
  lv_obj_set_size(st->overlay, lv_obj_get_width(screen),
                  lv_obj_get_height(screen) - layout::kTabBarHeight);
  lv_obj_set_style_bg_color(st->overlay, theme().turf, 0);
  lv_obj_set_style_bg_opa(st->overlay, LV_OPA_COVER, 0);
  lv_obj_remove_flag(st->overlay, LV_OBJ_FLAG_SCROLLABLE);
  // Swallows taps meant for the tab content underneath; swipes inside end
  // here (no gesture bubbling past the overlay).
  lv_obj_add_flag(st->overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(st->overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(st->overlay, on_overlay_gesture, LV_EVENT_GESTURE, st);
  lv_obj_add_event_cb(st->overlay, on_overlay_delete, LV_EVENT_DELETE,
                      new OverlayLink{st, st->alive});
  st->matchId = matchId;
  st->shown = Shown::None;
  st->view = nullptr;

  if (!show_detail(st)) show_placeholder(st, DetailPlaceholder::Loading);
  fetch(st);
}

void app_close_detail(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) close_detail(st, false);
}

lv_obj_t* app_detail_view(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  return st != nullptr && st->overlay != nullptr ? st->view : nullptr;
}
