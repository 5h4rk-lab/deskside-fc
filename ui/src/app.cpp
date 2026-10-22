#include "app.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "dim_schedule.h"
#include "fonts.h"
#include "format.h"
#include "goal_detector.h"
#include "screens/battery_banner.h"
#include "screens/detail.h"
#include "screens/goal.h"
#include "screens/live.h"
#include "screens/matches.h"
#include "screens/news.h"
#include "screens/settings.h"
#include "screens/team_picker.h"
#include "screens/wifi.h"
#include "screens/zone_list.h"
#include "sound_out.h"
#include "sound_policy.h"
#include "sound_synth.h"
#include "theme.h"
#include "timezone.h"

namespace {

enum class Shown { None, Loading, Error, Detail };
enum class OverlayKind { Detail, Settings, Picker, Wifi, Banner };
enum class TeamsLoad { Idle, Loading, Failed };

constexpr uint32_t kOpponentHex = 0x034694;  // Chelsea blue, the default opponent colour
constexpr int kTabCount = 3;

// Everything the app needs after app_create(). Owned by the tab view.
struct AppState {
  Store* store = nullptr;
  ImageProvider* images = nullptr;
  DataSource* source = nullptr;
  std::function<void(const std::string&)> onOpenMatch;
  Prefs* prefs = nullptr;
  Backlight* backlight = nullptr;
  WifiService* wifi = nullptr;
  std::function<void(std::string)> openMatch;  // what the screens call to open a match
  lv_obj_t* tabview = nullptr;
  lv_obj_t* pages[kTabCount] = {nullptr, nullptr, nullptr};  // tab pages, kept across rebuilds
  lv_obj_t* live = nullptr;
  lv_obj_t* matches = nullptr;
  lv_obj_t* news = nullptr;

  Settings settings;
  Settings saved;           // as last written to the prefs
  int64_t now = 0;          // of the last app_refresh()
  int lastPercent = -1;     // last value given to the backlight
  bool previewing = false;  // the brightness slider is being dragged

  // Settings overlay (above the tab content) and the zone list inside it.
  lv_obj_t* settingsOverlay = nullptr;
  lv_obj_t* settingsView = nullptr;
  lv_obj_t* zoneView = nullptr;

  // Team picker overlay (the whole screen) and the team list fetch.
  lv_obj_t* pickerOverlay = nullptr;
  lv_obj_t* picker = nullptr;
  std::vector<std::string> pickerTeamIds;  // ids of the list the picker shows
  bool pickerFailed = false;
  TeamsLoad teamsLoad = TeamsLoad::Idle;
  uint32_t teamsRequest = 0;

  // Wi-Fi overlay (the whole screen: the password page needs the keyboard).
  lv_obj_t* wifiOverlay = nullptr;
  lv_obj_t* wifiView = nullptr;

  // Detail overlay: a sibling above the tab view, covering the tab content.
  lv_obj_t* overlay = nullptr;
  lv_obj_t* view = nullptr;  // detail or placeholder inside the overlay
  Shown shown = Shown::None;
  std::string matchId;
  uint32_t request = 0;  // id of the latest fetch; older results are only stored

  // Goal celebration: fed by app_matches_changed().
  GoalDetector detector;
  // The celebration still waiting for its scorer (the detail lagged behind
  // the score); app_refresh() looks at the Store's detail for it.
  std::optional<GoalEvent> awaitingScorer;
  std::function<void()> onWifiConnected;

  // Battery: the service (optional), the banner's state machine and the banner.
  PowerService* power = nullptr;
  BatteryAlert batteryAlert;
  lv_obj_t* banner = nullptr;

  // Sound: the speaker (optional), the gate's memory, LVGL's tick extended to
  // 64 bits for it, and the fanfare waiting for the whistles to end.
  SoundOut* sound = nullptr;
  SoundGate soundGate;
  uint32_t lastTick = 0;
  int64_t monoMs = 0;
  lv_timer_t* fanfareTimer = nullptr;

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

// An overlay may be deleted with the screen after the tab view is gone.
struct OverlayLink {
  AppState* st;
  std::shared_ptr<bool> alive;
  OverlayKind kind;
};

void on_overlay_delete(lv_event_t* e) {
  auto* link = static_cast<OverlayLink*>(lv_event_get_user_data(e));
  lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  AppState* st = link->st;
  if (*link->alive) {
    if (link->kind == OverlayKind::Detail && st->overlay == obj) {
      st->overlay = nullptr;
      st->view = nullptr;
      st->shown = Shown::None;
    } else if (link->kind == OverlayKind::Settings && st->settingsOverlay == obj) {
      st->settingsOverlay = nullptr;
      st->settingsView = nullptr;
      st->zoneView = nullptr;
    } else if (link->kind == OverlayKind::Picker && st->pickerOverlay == obj) {
      st->pickerOverlay = nullptr;
      st->picker = nullptr;
    } else if (link->kind == OverlayKind::Wifi && st->wifiOverlay == obj) {
      st->wifiOverlay = nullptr;
      st->wifiView = nullptr;
    } else if (link->kind == OverlayKind::Banner && st->banner == obj) {
      st->banner = nullptr;
    }
  }
  delete link;
}

// A screen-level overlay above the tab view, covering the tab content (the
// tab bar stays visible) or the whole screen. It swallows taps meant for what
// is underneath, and swipes inside it end here (no gesture bubbling to the
// tab view).
lv_obj_t* make_overlay(AppState* st, OverlayKind kind, bool coverTabBar) {
  lv_obj_t* screen = lv_obj_get_parent(st->tabview);
  lv_obj_update_layout(screen);
  lv_obj_t* o = lv_obj_create(screen);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, 0, 0);
  lv_obj_set_size(o, lv_obj_get_width(screen),
                  lv_obj_get_height(screen) - (coverTabBar ? 0 : layout::kTabBarHeight));
  lv_obj_set_style_bg_color(o, theme().turf, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(o, on_overlay_delete, LV_EVENT_DELETE, new OverlayLink{st, st->alive, kind});
  if (st->banner != nullptr) lv_obj_move_foreground(st->banner);  // the warning stays on top
  return o;
}

// Deletes an overlay (or a view inside one). From an event of an object
// inside it, it is hidden now and deleted on the next cycle.
void discard(lv_obj_t* obj, bool fromInside) {
  if (obj == nullptr) return;
  if (fromInside) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(obj);
  } else {
    lv_obj_delete(obj);
  }
}

void close_detail(AppState* st, bool fromInside) {
  lv_obj_t* overlay = st->overlay;
  if (overlay == nullptr) return;
  st->overlay = nullptr;
  st->view = nullptr;
  st->shown = Shown::None;
  st->matchId.clear();
  discard(overlay, fromInside);
}

void drive_backlight(AppState* st, int64_t now);

// A brightness drag that never got its release (Settings closed or rebuilt
// under the finger): drop the unsaved value and give the backlight back to
// the schedule.
void end_preview(AppState* st) {
  if (!st->previewing) return;
  st->previewing = false;
  st->settings.dayBrightness = st->saved.dayBrightness;
  drive_backlight(st, st->now);
}

void close_settings(AppState* st, bool fromInside) {
  end_preview(st);
  lv_obj_t* overlay = st->settingsOverlay;
  if (overlay == nullptr) return;
  st->settingsOverlay = nullptr;
  st->settingsView = nullptr;
  st->zoneView = nullptr;
  discard(overlay, fromInside);
}

void close_zones(AppState* st, bool fromInside) {
  lv_obj_t* view = st->zoneView;
  st->zoneView = nullptr;
  discard(view, fromInside);
}

void close_picker(AppState* st, bool fromInside) {
  lv_obj_t* overlay = st->pickerOverlay;
  if (overlay == nullptr) return;
  st->pickerOverlay = nullptr;
  st->picker = nullptr;
  discard(overlay, fromInside);
}

void close_wifi(AppState* st, bool fromInside) {
  lv_obj_t* overlay = st->wifiOverlay;
  if (overlay == nullptr) return;
  st->wifiOverlay = nullptr;
  st->wifiView = nullptr;
  discard(overlay, fromInside);
}

// ---------------------------------------------------------------- backlight

// Day brightness, or the night level inside the night window unless the
// screen was touched recently. Only changes reach the backlight.
void drive_backlight(AppState* st, int64_t now) {
  if (st->backlight == nullptr || st->previewing) return;
  const Settings& s = st->settings;
  int percent = s.dayBrightness;
  if (s.nightDim) {
    const bool touched = lv_display_get_inactive_time(nullptr) < kTouchWakeMs;
    const int night = std::min(s.nightPercent, s.dayBrightness);
    const DimSchedule schedule(s.nightStartHour, s.nightEndHour, static_cast<uint8_t>(night),
                               static_cast<uint8_t>(s.dayBrightness));
    percent = schedule.brightness_at(local_hour(now, st->store->tz_offset()), touched);
  }
  if (percent == st->lastPercent) return;
  st->lastPercent = percent;
  st->backlight->set_percent(static_cast<uint8_t>(percent));
}

// The brightness slider is being dragged: show the value at once.
void preview_brightness(AppState* st, int percent) {
  // In use at once (refreshes keep it), saved only when the finger lifts.
  st->settings.dayBrightness = percent;
  st->previewing = true;
  if (st->backlight == nullptr || percent == st->lastPercent) return;
  st->lastPercent = percent;
  st->backlight->set_percent(static_cast<uint8_t>(percent));
}

// ---------------------------------------------------------------- settings

void open_settings(AppState* st);
void open_zones(AppState* st);
void open_picker(AppState* st);
void open_wifi(AppState* st, WifiMode mode);

// The Settings Wi-Fi row's state line.
void sync_settings_wifi(AppState* st) {
  if (st->settingsView == nullptr || st->wifi == nullptr) return;
  settings_set_wifi_status(st->settingsView, wifi_status_text(st->wifi->status()));
}

// The About area's data line: how fetching is going.
void sync_settings_data(AppState* st) {
  if (st->settingsView == nullptr) return;
  settings_set_data_status(st->settingsView, st->store->data_status_text(),
                           st->store->last_error() != FetchError::None);
}

// The About area's battery line (hosts with a battery only).
void sync_settings_battery(AppState* st) {
  if (st->settingsView == nullptr || st->power == nullptr) return;
  settings_set_battery(st->settingsView, st->store->power());
}

// ---------------------------------------------------------------- sound

// LVGL's millisecond tick as a 64-bit counter that survives its 49 day wrap.
int64_t monotonic_ms(AppState* st) {
  const uint32_t tick = lv_tick_get();
  st->monoMs += static_cast<uint32_t>(tick - st->lastTick);
  st->lastTick = tick;
  return st->monoMs;
}

// Night dimming is active at `now`: it is switched on and the local hour is
// inside the night window (touching the screen lifts the dimming, not this).
bool night_dim_active(const AppState* st, int64_t now) {
  const Settings& s = st->settings;
  if (!s.nightDim) return false;
  const DimSchedule schedule(s.nightStartHour, s.nightEndHour, 0, 100);
  return schedule.in_night(local_hour(now, st->store->tz_offset()));
}

// The one place the UI makes a sound: the settings and quiet hours decide
// whether `event` is wanted, the gate whether it fits with what played
// lately, then the speaker plays the clip at the chosen volume. False when
// nothing was started (no speaker, muted, a repeat).
bool app_sound(AppState* st, SoundEvent event, int64_t now) {
  if (st->sound == nullptr) return false;
  if (!sound_should_play(st->settings, event, night_dim_active(st, now))) return false;
  if (!st->soundGate.allow(event, monotonic_ms(st))) return false;
  st->sound->play(sound_clip(event), static_cast<uint8_t>(st->settings.soundVolume));
  return true;
}

void on_fanfare_due(lv_timer_t* t) {
  auto* st = static_cast<AppState*>(lv_timer_get_user_data(t));
  st->fanfareTimer = nullptr;  // a one-shot timer: LVGL deletes it after this call
  app_sound(st, SoundEvent::WinFanfare, st->now);
}

void cancel_fanfare(AppState* st) {
  if (st->fanfareTimer == nullptr) return;
  lv_timer_delete(st->fanfareTimer);
  st->fanfareTimer = nullptr;
}

// The favourite won at full time: the whistles, then the fanfare when they
// are over (at once when the whistles are not wanted).
void win_sound(AppState* st, int64_t now) {
  cancel_fanfare(st);
  if (app_sound(st, SoundEvent::FullTime, now)) {
    st->fanfareTimer = lv_timer_create(on_fanfare_due, static_cast<uint32_t>(sound_clip_duration_ms(SoundEvent::FullTime)), st);
    lv_timer_set_repeat_count(st->fanfareTimer, 1);
  } else {
    app_sound(st, SoundEvent::WinFanfare, now);
  }
}

// ---------------------------------------------------------------- battery

void show_banner(AppState* st, const Alert& alert) {
  if (st->banner != nullptr) {
    lv_obj_t* old = st->banner;
    st->banner = nullptr;
    lv_obj_delete(old);
  }
  lv_obj_t* screen = lv_obj_get_parent(st->tabview);
  st->banner = battery_banner_create(screen, alert);
  lv_obj_add_event_cb(st->banner, on_overlay_delete, LV_EVENT_DELETE,
                      new OverlayLink{st, st->alive, OverlayKind::Banner});
  // Once for each appearance of the banner (15 percent, then 5 percent).
  app_sound(st, alert.level == Alert::Level::Critical ? SoundEvent::BatteryCritical : SoundEvent::BatteryLow,
            st->now);
}

// Reads the battery once: into the Store (the screens' status rows follow it
// in their own refresh, touching only what changed), the Settings line and
// the low-battery banner.
void sync_power(AppState* st, int64_t now) {
  if (st->power == nullptr) return;
  const PowerStatus p = st->power->status();
  st->store->set_power(p);
  sync_settings_battery(st);
  const std::optional<Alert> alert = st->batteryAlert.update(p, goal_active(), now);
  if (alert) show_banner(st, *alert);
}

void save(AppState* st) {
  save_settings(*st->prefs, st->settings);
  st->saved = st->settings;
}

// The fields the Settings screen edits.
bool same_display(const Settings& a, const Settings& b) {
  return a.dayBrightness == b.dayBrightness && a.nightDim == b.nightDim &&
         a.nightStartHour == b.nightStartHour && a.nightEndHour == b.nightEndHour &&
         a.nightPercent == b.nightPercent && a.reducedMotion == b.reducedMotion &&
         a.soundMode == b.soundMode && a.soundVolume == b.soundVolume &&
         a.soundMuteAtNight == b.soundMuteAtNight;
}

// A change made on the Settings screen: take the fields it edits, apply them
// at once and save them when they differ from what is saved (a tap on the
// slider that leaves it where it was writes nothing).
void settings_changed(AppState* st, const Settings& from) {
  Settings& s = st->settings;
  s.dayBrightness = from.dayBrightness;
  s.nightDim = from.nightDim;
  s.nightStartHour = from.nightStartHour;
  s.nightEndHour = from.nightEndHour;
  s.nightPercent = from.nightPercent;
  s.reducedMotion = from.reducedMotion;
  s.soundMode = from.soundMode;
  s.soundVolume = from.soundVolume;
  s.soundMuteAtNight = from.soundMuteAtNight;
  if (!same_display(s, st->saved)) save(st);
  goal_set_reduced_motion(s.reducedMotion);
  st->previewing = false;
  drive_backlight(st, st->now);
}

SettingsCallbacks settings_callbacks(AppState* st) {
  std::shared_ptr<bool> alive = st->alive;
  SettingsCallbacks cb;
  cb.onBack = [st, alive] {
    if (*alive) close_settings(st, true);
  };
  cb.onTeam = [st, alive] {
    if (*alive) open_picker(st);
  };
  cb.onZone = [st, alive] {
    if (*alive) open_zones(st);
  };
  if (st->wifi != nullptr) {
    cb.onWifi = [st, alive] {
      if (*alive) open_wifi(st, WifiMode::Normal);
    };
  }
  if (st->sound != nullptr) {
    cb.onTestSound = [st, alive] {
      if (*alive) app_sound(st, SoundEvent::Preview, st->now);
    };
  }
  cb.onBrightnessPreview = [st, alive](int percent) {
    if (*alive) preview_brightness(st, percent);
  };
  cb.onChanged = [st, alive](const Settings& s) {
    if (*alive) settings_changed(st, s);
  };
  return cb;
}

void open_settings(AppState* st) {
  if (st->settingsOverlay != nullptr) return;
  close_detail(st, false);
  st->settingsOverlay = make_overlay(st, OverlayKind::Settings, false);
  st->settingsView = settings_create(st->settingsOverlay, st->settings, *st->images,
                                     settings_callbacks(st), st->now);
  sync_settings_wifi(st);
  sync_settings_data(st);
  sync_settings_battery(st);
}

// ---------------------------------------------------------------- wifi

// A join made on the Wi-Fi pages succeeded. First setup ends there.
void wifi_joined(AppState* st, WifiMode mode) {
  sync_settings_wifi(st);
  if (mode != WifiMode::FirstSetup) return;
  close_wifi(st, true);  // called from inside the Wi-Fi screen
  app_refresh(st->tabview, st->now);
}

void open_wifi(AppState* st, WifiMode mode) {
  if (st->wifi == nullptr || st->wifiOverlay != nullptr) return;
  std::shared_ptr<bool> alive = st->alive;
  WifiCallbacks cb;
  cb.onBack = [st, alive] {
    if (*alive) close_wifi(st, true);
  };
  cb.onConnected = [st, alive, mode] {
    if (*alive) wifi_joined(st, mode);
  };
  // From the join result itself: also after Back on the page or with the
  // overlay closed, but not once the app is gone.
  cb.onJoinSucceeded = [st, alive] {
    if (!*alive) return;
    app_sound(st, SoundEvent::WifiOk, st->now);
    const std::function<void()> notify = st->onWifiConnected;  // the host may delete the app
    if (notify) notify();
  };
  st->wifiOverlay = make_overlay(st, OverlayKind::Wifi, true);
  st->wifiView = wifi_create(st->wifiOverlay, *st->wifi, mode, cb);
}

void pick_zone(AppState* st, const std::string& zoneId) {
  st->settings.zoneId = zoneId;
  save(st);
  st->store->set_tz_offset(offset_seconds(zoneId, st->now));
  close_zones(st, true);  // called from a row of the list
  app_refresh(st->tabview, st->now);
}

void open_zones(AppState* st) {
  open_settings(st);
  if (st->zoneView != nullptr) return;
  std::shared_ptr<bool> alive = st->alive;
  st->zoneView = zone_list_create(
      st->settingsOverlay, st->settings.zoneId, st->now,
      [st, alive](std::string id) {
        if (*alive) pick_zone(st, id);
      },
      [st, alive] {
        if (*alive) close_zones(st, true);
      });
}

// ---------------------------------------------------------------- teams

void fetch_teams(AppState* st);

std::vector<std::string> team_ids(const std::vector<Team>& teams) {
  std::vector<std::string> ids;
  ids.reserve(teams.size());
  for (const Team& t : teams) ids.push_back(t.id);
  return ids;
}

// Brings an open picker in line with the Store's team list and the fetch.
void sync_picker(AppState* st) {
  if (st->picker == nullptr) return;
  std::vector<std::string> ids = team_ids(st->store->teams());
  if (ids != st->pickerTeamIds) {
    team_picker_set_teams(st->picker, st->store->teams());
    st->pickerTeamIds = std::move(ids);
  }
  const bool failed = st->teamsLoad == TeamsLoad::Failed && st->store->teams().empty();
  if (failed != st->pickerFailed) {
    st->pickerFailed = failed;
    std::shared_ptr<bool> alive = st->alive;
    team_picker_set_failed(st->picker, failed, [st, alive] {
      if (*alive) fetch_teams(st);
    });
  }
  team_picker_refresh(st->picker);
}

void fetch_teams(AppState* st) {
  st->teamsLoad = TeamsLoad::Loading;
  const uint32_t request = ++st->teamsRequest;
  sync_picker(st);
  std::shared_ptr<bool> alive = st->alive;
  // The result may arrive at once or much later (picker closed, app gone).
  st->source->fetch_teams([st, alive, request](bool ok, std::vector<Team> teams) {
    if (!*alive || request != st->teamsRequest) return;
    if (ok && !teams.empty()) {
      st->store->set_teams(std::move(teams));
      st->teamsLoad = TeamsLoad::Idle;
    } else {
      st->teamsLoad = TeamsLoad::Failed;
    }
    sync_picker(st);
  });
}

void open_picker(AppState* st) {
  if (st->pickerOverlay != nullptr) return;
  std::shared_ptr<bool> alive = st->alive;
  st->pickerOverlay = make_overlay(st, OverlayKind::Picker, true);
  st->picker = team_picker_create(
      st->pickerOverlay, st->store->teams(), st->settings.favourite, *st->images,
      [st, alive](Team team) {
        if (*alive) app_apply_team(st->tabview, team);
      },
      [st, alive] {
        if (*alive) close_picker(st, true);
      });
  st->pickerTeamIds = team_ids(st->store->teams());
  st->pickerFailed = false;
  if (st->store->teams().empty() && st->teamsLoad != TeamsLoad::Loading) {
    fetch_teams(st);
  } else {
    sync_picker(st);
  }
}

void on_app_delete(lv_event_t* e) {
  auto* st = static_cast<AppState*>(lv_event_get_user_data(e));
  *st->alive = false;
  cancel_fanfare(st);
  delete st;
}

// Picking a main tab leaves the detail and Settings.
void on_tab_button(lv_event_t* e) {
  auto* st = static_cast<AppState*>(lv_event_get_user_data(e));
  close_detail(st, false);
  close_settings(st, false);
}

struct TabSpec {
  const char* label;
  const char* icon;  // LVGL symbol, rendered with the built-in symbol font
};

const TabSpec kTabs[] = {
    {"Live", LV_SYMBOL_PLAY},
    {"Matches", LV_SYMBOL_LIST},
    {"News", LV_SYMBOL_FILE},
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

// (Re)creates the three tab screens inside their pages, so everything that
// captured the accent colour at build time follows the current theme. The
// selected tab stays.
void build_tabs(AppState* st) {
  const uint32_t active = lv_tabview_get_tab_active(st->tabview);
  for (lv_obj_t* page : st->pages) lv_obj_clean(page);
  st->live = live_create(st->pages[0], *st->store, *st->images, st->openMatch);
  st->matches = matches_create(st->pages[1], *st->store, *st->images, st->openMatch);
  st->news = news_create(st->pages[2], *st->store, *st->images);
  std::shared_ptr<bool> alive = st->alive;
  live_set_on_settings(st->live, [st, alive] {
    if (*alive) open_settings(st);
  });
  lv_tabview_set_active(st->tabview, active, LV_ANIM_OFF);
}

// A celebration still without its scorer: the detail watch keeps refreshing
// the Store's detail, so look there. Forgotten once the overlay is gone.
void check_scorer(AppState* st) {
  if (!st->awaitingScorer) return;
  const GoalParts* parts = goal_parts();
  if (parts == nullptr || parts->matchId != st->awaitingScorer->matchId) {
    st->awaitingScorer.reset();
    return;
  }
  const MatchDetail* d = st->store->detail_for(st->awaitingScorer->matchId);
  if (d == nullptr) return;
  const std::optional<Scorer> scorer = scorer_for_goal(*d, *st->awaitingScorer);
  if (!scorer) return;
  const std::string matchId = st->awaitingScorer->matchId;
  st->awaitingScorer.reset();
  goal_set_scorer(matchId, scorer->name, scorer->photo);
}

}  // namespace

lv_obj_t* app_create(lv_obj_t* root, const AppDeps& deps) {
  auto* st = new AppState();
  st->store = &deps.store;
  st->images = &deps.images;
  st->source = &deps.source;
  st->onOpenMatch = deps.onOpenMatch;
  st->prefs = &deps.prefs;
  st->backlight = deps.backlight;
  st->wifi = deps.wifi;
  st->onWifiConnected = deps.onWifiConnected;
  st->power = deps.power;
  st->sound = deps.sound;
  st->lastTick = lv_tick_get();

  // Saved settings first: the screens capture the accent colour as they are built.
  st->settings = load_settings(deps.prefs);
  st->saved = st->settings;
  deps.store.set_favourite(st->settings.favourite.id);
  theme_set_team(st->settings.favourite.color, kOpponentHex, st->settings.favourite.primary);
  goal_set_reduced_motion(st->settings.reducedMotion);

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

  st->openMatch = [st](std::string id) {
    app_open_match(st->tabview, id);
    if (st->onOpenMatch) st->onOpenMatch(id);
  };
  for (int i = 0; i < kTabCount; ++i) {
    lv_obj_t* tab = lv_tabview_add_tab(tabview, kTabs[i].label);
    // Full-bleed screens that manage their own scrolling.
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    st->pages[i] = tab;
  }
  st->tabview = tabview;
  lv_obj_set_user_data(tabview, st);
  lv_obj_add_event_cb(tabview, on_app_delete, LV_EVENT_DELETE, st);
  build_tabs(st);

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

  // First start (nothing saved, not connected): the guided Wi-Fi setup.
  if (st->wifi != nullptr && st->wifi->saved().empty() && !st->wifi->status().connected) {
    open_wifi(st, WifiMode::FirstSetup);
  }
  return tabview;
}

static void sync_favourite_from_teams(AppState* st);

void app_refresh(lv_obj_t* appRoot, int64_t now) {
  AppState* st = app_state(appRoot);
  if (st == nullptr) return;
  st->now = now;
  sync_favourite_from_teams(st);
  // Every screen's local times follow the zone, daylight saving included.
  st->store->set_tz_offset(offset_seconds(st->settings.zoneId, now));
  drive_backlight(st, now);
  sync_power(st, now);
  check_scorer(st);
  if (st->live != nullptr) live_refresh(st->live, now);
  if (st->matches != nullptr) matches_refresh(st->matches, now);
  if (st->news != nullptr) news_refresh(st->news, now);
  // The open detail follows the Store: newer events, published lineups, the
  // clock from the match list. Unchanged data touches nothing.
  if (st->overlay != nullptr) show_detail(st);
  if (st->settingsView != nullptr) settings_refresh(st->settingsView, st->settings, now);
  sync_settings_wifi(st);
  sync_settings_data(st);
  if (st->wifiView != nullptr) wifi_refresh(st->wifiView);
  sync_picker(st);
}

const Settings* app_settings(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  return st != nullptr ? &st->settings : nullptr;
}

void app_open_settings(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) open_settings(st);
}

void app_close_settings(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) close_settings(st, false);
}

lv_obj_t* app_settings_view(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  return st != nullptr ? st->settingsView : nullptr;
}

void app_open_zones(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) open_zones(st);
}

lv_obj_t* app_zone_view(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  return st != nullptr ? st->zoneView : nullptr;
}

void app_open_team_picker(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) open_picker(st);
}

lv_obj_t* app_team_picker_view(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  return st != nullptr ? st->picker : nullptr;
}

void app_open_wifi(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) open_wifi(st, WifiMode::Normal);
}

void app_close_wifi(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) close_wifi(st, false);
}

lv_obj_t* app_wifi_view(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  return st != nullptr ? st->wifiView : nullptr;
}

// Applies the favourite's colours to the theme and rebuilds what is drawn in
// them: the tabs, the tab icons and an open Settings screen.
static void recolour(AppState* st) {
  const Team& team = st->settings.favourite;
  theme_set_team(team.color, kOpponentHex, team.primary);
  build_tabs(st);
  refresh_icons(st->tabview);
  if (st->settingsView != nullptr) {
    // Rebuilt in the new colour, under the zone list if that is open.
    discard(st->settingsView, true);
    st->settingsView = settings_create(st->settingsOverlay, st->settings, *st->images,
                                       settings_callbacks(st), st->now);
    lv_obj_move_to_index(st->settingsView, 0);
    sync_settings_wifi(st);
    sync_settings_data(st);
    sync_settings_battery(st);
  }
}

// A favourite saved by an older build (no fav.primary) or before the helper
// changed a colour, crest or name: once the team list holds the favourite,
// its entry wins. Saved when anything differs; rebuilt only when a colour does.
static void sync_favourite_from_teams(AppState* st) {
  Team& fav = st->settings.favourite;
  for (const Team& t : st->store->teams()) {
    if (t.id != fav.id) continue;
    const bool colours = t.color != fav.color || t.primary != fav.primary;
    const bool other = t.name != fav.name || t.abbr != fav.abbr || t.crest != fav.crest ||
                       (!t.league.empty() && t.league != fav.league);
    if (!colours && !other) return;
    const std::string league = t.league.empty() ? fav.league : t.league;
    fav = t;
    fav.league = league;
    save(st);
    if (colours) recolour(st);
    return;
  }
}

void app_apply_team(lv_obj_t* appRoot, const Team& team) {
  AppState* st = app_state(appRoot);
  if (st == nullptr) return;
  end_preview(st);
  st->settings.favourite = team;
  save(st);
  st->store->set_favourite(team.id);

  // Usually called from a row of the picker: it goes on the next cycle.
  close_picker(st, true);
  recolour(st);
  app_refresh(appRoot, st->now);
}


void app_open_match(lv_obj_t* appRoot, const std::string& matchId) {
  AppState* st = app_state(appRoot);
  if (st == nullptr) return;
  close_detail(st, false);
  close_settings(st, false);
  close_picker(st, false);

  st->overlay = make_overlay(st, OverlayKind::Detail, false);
  lv_obj_add_event_cb(st->overlay, on_overlay_gesture, LV_EVENT_GESTURE, st);
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

std::string app_open_match_id(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  return st != nullptr && st->overlay != nullptr ? st->matchId : std::string();
}

void app_matches_changed(lv_obj_t* appRoot, int64_t now) {
  AppState* st = app_state(appRoot);
  if (st == nullptr) return;
  const std::optional<GoalEvent> event =
      st->detector.update(st->store->matches(), st->store->favourite());
  if (!event) return;
  if (event->favourite) {
    goal_celebrate(*event, *st->images);
    st->awaitingScorer = *event;
    // Same guard as the celebration: the favourite's goal (or win), never a
    // first sight, an opponent's goal or another team's match.
    if (event->fullTime) {
      win_sound(st, now);
    } else {
      cancel_fanfare(st);  // a goal after the final whistle is not expected; the horn wins
      app_sound(st, SoundEvent::Goal, now);
    }
  }

  // The detail names the scorer and feeds the Live strip, for either team. A
  // detail that predates the goal names nobody; app_refresh() keeps looking
  // at later details (the poller's detail watch) while the overlay shows.
  std::shared_ptr<bool> alive = st->alive;
  const GoalEvent ev = *event;
  st->source->fetch_detail(ev.matchId, [st, alive, now](bool ok, MatchDetail detail) {
    if (!*alive || !ok) return;
    st->store->set_detail(std::move(detail));
    app_refresh(st->tabview, now);  // names the scorer when this detail has the goal
  });
}

lv_obj_t* app_battery_banner(lv_obj_t* appRoot) {
  AppState* st = app_state(appRoot);
  if (st == nullptr || st->banner == nullptr) return nullptr;
  // A tapped banner is hidden at once and deleted on the next cycle.
  return lv_obj_has_flag(st->banner, LV_OBJ_FLAG_HIDDEN) ? nullptr : st->banner;
}

void app_show_battery_banner(lv_obj_t* appRoot, const Alert& alert) {
  AppState* st = app_state(appRoot);
  if (st != nullptr) show_banner(st, alert);
}
