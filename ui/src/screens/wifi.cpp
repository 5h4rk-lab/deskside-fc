#include "screens/wifi.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "fonts.h"
#include "screens/widgets.h"
#include "theme.h"

// Layout (240x320, the whole screen): "< Back" and the title (first setup:
// a welcome line under it), then a scrolling column: the status card, the
// saved networks, the nearby networks with "Scan again". The password page
// covers the list: "< Back", the SSID, the field with Show/Hide, the hint or
// error line, Connect, and the keyboard at the bottom.
namespace {

using widgets::label;
using widgets::plain;
using widgets::set_text;
using widgets::show;

constexpr int32_t kContentW = 240 - 2 * widgets::kListSide;  // 224: a card's width
constexpr int32_t kRowH = 44;
constexpr int32_t kConfirmRowH = 78;
constexpr int32_t kRowPad = 12;
constexpr int32_t kSmallButtonW = 58;
constexpr int32_t kSmallButtonH = 24;
constexpr int32_t kScanButtonW = 80;
constexpr int32_t kBarW = 3;
constexpr int32_t kBarGap = 2;
constexpr int32_t kBarsW = 4 * kBarW + 3 * kBarGap;  // 18
constexpr int32_t kBarsH = 14;
constexpr int32_t kLockW = 10;
constexpr int32_t kLockH = 13;
constexpr int32_t kSpinnerSize = 16;
constexpr int32_t kTickW = 16;
// Saved row: name, then on the right the tick and "Forget".
constexpr int32_t kSavedNameW = kContentW - 2 * kRowPad - kSmallButtonW - 8 - kTickW - 6;
// Nearby row: name, then the lock and the bars.
constexpr int32_t kNearbyNameW = kContentW - 2 * kRowPad - kBarsW - 8 - kLockW - 8;
constexpr int32_t kStatusTextW = kContentW - 2 * kRowPad - kBarsW - 8;
constexpr int32_t kHelpY = 50;
constexpr int32_t kFirstBodyY = 72;

// Password page.
constexpr int32_t kFieldX = widgets::kListSide;
constexpr int32_t kFieldY = 52;
constexpr int32_t kFieldH = 38;
constexpr int32_t kShowW = 52;
constexpr int32_t kNoteY = kFieldY + kFieldH + 6;  // hint or error
constexpr int32_t kButtonY = 128;  // under a two-line error
constexpr int32_t kButtonH = 36;
constexpr int32_t kKeyboardH = 150;
constexpr uint32_t kSpinMs = 1000;
constexpr uint32_t kSpinArc = 240;

struct WifiState {
  WifiService* wifi = nullptr;
  WifiMode mode = WifiMode::Normal;
  WifiCallbacks cb;
  lv_obj_t* root = nullptr;
  WifiParts parts{};
  lv_obj_t* savedBox = nullptr;
  lv_obj_t* nearbyBox = nullptr;
  lv_obj_t* messageLine = nullptr;
  std::vector<WifiSavedRow> savedRows;
  std::vector<WifiNearbyRow> nearbyRows;
  std::vector<std::string> shownSaved;
  std::vector<WifiNetwork> shownNearby;
  bool everRendered = false;

  // Scan.
  std::vector<WifiNetwork> scanned;
  bool scanning = false;
  bool scannedOnce = false;

  // The join in progress (one at a time). Results of older requests, or of
  // a request the user walked away from, are ignored.
  bool joining = false;
  bool joinFromPage = false;
  std::string joinSsid;
  uint32_t request = 0;
  // The latest attempt handed to the service and not answered yet. It stays
  // after the user walks away (its result is ignored, but the device is still
  // trying that network), so the status card keeps saying so.
  std::string inFlightSsid;
  uint32_t inFlightRequest = 0;
  std::string failedSsid;  // last list join that failed

  // Password page.
  bool pageOpen = false;
  WifiPasswordParts page{};
  bool pageError = false;
  bool pageJoined = false;
  lv_obj_t* connectTick = nullptr;  // in the button once joined

  // The "Connected" pause before moving on.
  lv_timer_t* hold = nullptr;

  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

WifiState* state_of(lv_obj_t* obj) { return static_cast<WifiState*>(lv_obj_get_user_data(obj)); }

size_t code_points(const char* s) {
  size_t n = 0;
  for (; *s != '\0'; ++s) {
    if ((static_cast<unsigned char>(*s) & 0xC0) != 0x80) ++n;
  }
  return n;
}

bool same_networks(const std::vector<WifiNetwork>& a, const std::vector<WifiNetwork>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].ssid != b[i].ssid || a[i].rssi != b[i].rssi || a[i].open != b[i].open) return false;
  }
  return true;
}

void call(const std::function<void()>& fn) {
  const std::function<void()> f = fn;  // the host may delete this screen
  if (f) f();
}

// Hidden now, deleted on the next cycle: it may be running the event that
// led here.
void discard(lv_obj_t* obj) {
  if (obj == nullptr) return;
  lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  lv_obj_delete_async(obj);
}

// ---------------------------------------------------------------- widgets

// The field font: Hanken with Montserrat behind it for the password bullet.
const lv_font_t* field_font() {
  static lv_font_t font;
  static bool made = false;
  if (!made) {
    font = hanken_14;
    font.fallback = &lv_font_montserrat_14;
    made = true;
  }
  return &font;
}

lv_obj_t* text_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text) {
  lv_obj_t* l = label(parent, font, color);
  lv_label_set_text(l, text);
  return l;
}

// A one-line label of fixed size ending in "..." when the text is too long.
lv_obj_t* line_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, int32_t width) {
  lv_obj_t* l = label(parent, font, color);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_size(l, width, font->line_height);
  return l;
}

// A small outlined pill holding its text: the label is the button. A warning
// pill (the confirming Forget) is amber.
lv_obj_t* pill(lv_obj_t* parent, const char* text, bool warning, int32_t width) {
  const Palette& pal = theme();
  lv_obj_t* b = text_label(parent, &hanken_12, warning ? pal.amber : pal.chalk, text);
  lv_obj_set_size(b, width, kSmallButtonH);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(b, (kSmallButtonH - 2 - hanken_12.line_height) / 2, 0);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, warning ? pal.amber : mix(pal.line, pal.chalk, 60), 0);
  lv_obj_set_style_bg_color(b, pal.panel, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, mix(pal.panel, pal.chalk, 40), LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, 6);
  return b;
}

lv_obj_t* spinner(lv_obj_t* parent) {
  const Palette& pal = theme();
  lv_obj_t* s = lv_spinner_create(parent);
  lv_obj_set_size(s, kSpinnerSize, kSpinnerSize);
  lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s, 2, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s, pal.line, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s, pal.chalk, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(s, true, LV_PART_INDICATOR);
  lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  lv_anim_delete(s, nullptr);  // runs only while shown
  return s;
}

void show_spinner(lv_obj_t* s, bool on) {
  if (on == !lv_obj_has_flag(s, LV_OBJ_FLAG_HIDDEN)) return;
  show(s, on);
  if (on) {
    lv_spinner_set_anim_params(s, kSpinMs, kSpinArc);
  } else {
    lv_anim_delete(s, nullptr);
  }
}

// Four bars of rising height, the lit ones in chalk.
lv_obj_t* signal_bars(lv_obj_t* parent) {
  lv_obj_t* box = plain(parent);
  lv_obj_set_size(box, kBarsW, kBarsH);
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* bar = plain(box);
    const int32_t h = 5 + i * 3;
    lv_obj_set_size(bar, kBarW, h);
    lv_obj_set_pos(bar, i * (kBarW + kBarGap), kBarsH - h);
    lv_obj_set_style_radius(bar, 1, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  }
  lv_obj_set_user_data(box, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
  return box;
}

void set_bars(lv_obj_t* box, int level) {
  if (reinterpret_cast<intptr_t>(lv_obj_get_user_data(box)) == level) return;
  lv_obj_set_user_data(box, reinterpret_cast<void*>(static_cast<intptr_t>(level)));
  const Palette& pal = theme();
  for (int i = 0; i < 4; ++i) {
    lv_obj_set_style_bg_color(lv_obj_get_child(box, i), i < level ? pal.chalk : pal.line, 0);
  }
}

// A padlock: a rounded shackle over a solid body.
lv_obj_t* lock_icon(lv_obj_t* parent) {
  const lv_color_t c = theme().dim;
  lv_obj_t* box = plain(parent);
  lv_obj_set_size(box, kLockW, kLockH);
  lv_obj_t* shackle = plain(box);
  lv_obj_set_size(shackle, 8, 10);
  lv_obj_set_pos(shackle, 1, 0);
  lv_obj_set_style_border_width(shackle, 2, 0);
  lv_obj_set_style_border_color(shackle, c, 0);
  lv_obj_set_style_border_opa(shackle, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(shackle, 4, 0);
  lv_obj_t* body = plain(box);
  lv_obj_set_size(body, kLockW, 7);
  lv_obj_set_pos(body, 0, kLockH - 7);
  lv_obj_set_style_radius(body, 2, 0);
  lv_obj_set_style_bg_color(body, c, 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  return box;
}

// ---------------------------------------------------------------- rendering

void refresh(WifiState* st);
void render_status(WifiState* st);
void start_join(WifiState* st, const std::string& ssid, bool saved);
void open_page(WifiState* st, const std::string& ssid);

void on_saved_row(lv_event_t* e);
void on_forget(lv_event_t* e);
void on_confirm_cancel(lv_event_t* e);
void on_confirm_forget(lv_event_t* e);
void on_nearby_row(lv_event_t* e);

WifiSavedRow make_saved_row(WifiState* st, const std::string& ssid) {
  const Palette& pal = theme();
  WifiSavedRow r;
  r.ssid = ssid;
  r.row = widgets::card(st->savedBox, true);
  lv_obj_set_size(r.row, LV_PCT(100), kRowH);
  lv_obj_add_event_cb(r.row, on_saved_row, LV_EVENT_CLICKED, st);

  r.name = line_label(r.row, &hanken_14, pal.chalk, kSavedNameW);
  lv_label_set_text(r.name, ssid.c_str());
  lv_obj_set_pos(r.name, kRowPad, 6);
  r.tag = text_label(r.row, &hanken_10, pal.dim, kWifiSavedTag);
  lv_obj_set_pos(r.tag, kRowPad, 24);
  r.forget = pill(r.row, kWifiForget, false, kSmallButtonW);
  lv_obj_align(r.forget, LV_ALIGN_RIGHT_MID, -kRowPad + 2, 0);
  lv_obj_add_event_cb(r.forget, on_forget, LV_EVENT_CLICKED, st);
  r.tick = widgets::tick(r.row);
  lv_obj_align(r.tick, LV_ALIGN_RIGHT_MID, -(kRowPad - 2 + kSmallButtonW + 8), 0);
  lv_obj_add_flag(r.tick, LV_OBJ_FLAG_HIDDEN);

  // The inline question: "Forget <ssid>?" over Cancel and Forget.
  r.confirm = plain(r.row);
  lv_obj_set_size(r.confirm, LV_PCT(100), kConfirmRowH);
  lv_obj_add_flag(r.confirm, LV_OBJ_FLAG_HIDDEN);
  r.confirmText = line_label(r.confirm, &hanken_14, pal.chalk, kContentW - 2 * kRowPad);
  lv_label_set_text(r.confirmText, (std::string(kWifiForget) + " " + ssid + "?").c_str());
  lv_obj_set_pos(r.confirmText, kRowPad, 10);
  r.confirmForget = pill(r.confirm, kWifiForget, true, kSmallButtonW + 8);
  lv_obj_align(r.confirmForget, LV_ALIGN_BOTTOM_RIGHT, -kRowPad + 2, -12);
  lv_obj_add_event_cb(r.confirmForget, on_confirm_forget, LV_EVENT_CLICKED, st);
  r.confirmCancel = pill(r.confirm, kWifiCancel, false, kSmallButtonW + 8);
  lv_obj_align_to(r.confirmCancel, r.confirmForget, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  lv_obj_add_event_cb(r.confirmCancel, on_confirm_cancel, LV_EVENT_CLICKED, st);
  return r;
}

void set_confirm(WifiSavedRow& r, bool on) {
  show(r.confirm, on);
  show(r.name, !on);
  show(r.tag, !on);
  show(r.forget, !on);
  if (on) show(r.tick, false);
  lv_obj_set_height(r.row, on ? kConfirmRowH : kRowH);
}

WifiNearbyRow make_nearby_row(WifiState* st, const WifiNetwork& n) {
  const Palette& pal = theme();
  WifiNearbyRow r;
  r.network = n;
  r.row = widgets::card(st->nearbyBox, true);
  lv_obj_set_size(r.row, LV_PCT(100), kRowH);
  lv_obj_add_event_cb(r.row, on_nearby_row, LV_EVENT_CLICKED, st);
  r.name = line_label(r.row, &hanken_14, pal.chalk, kNearbyNameW);
  lv_label_set_text(r.name, n.ssid.c_str());
  lv_obj_align(r.name, LV_ALIGN_LEFT_MID, kRowPad, 0);
  r.bars = signal_bars(r.row);
  lv_obj_align(r.bars, LV_ALIGN_RIGHT_MID, -kRowPad, -1);
  set_bars(r.bars, wifi_signal_bars(n.rssi));
  r.lock = lock_icon(r.row);
  lv_obj_align(r.lock, LV_ALIGN_RIGHT_MID, -(kRowPad + kBarsW + 8), -1);
  if (n.open) lv_obj_add_flag(r.lock, LV_OBJ_FLAG_HIDDEN);
  return r;
}

void render_status(WifiState* st) {
  const Palette& pal = theme();
  WifiParts& p = st->parts;
  const WifiStatus s = st->wifi->status();
  std::string main;
  std::string detail;
  bool failed = false;
  const bool trying = !st->inFlightSsid.empty();
  if (trying) {
    main = std::string(kWifiConnectingPrefix) + st->inFlightSsid + "...";
  } else {
    main = wifi_status_text(s);
    if (!st->failedSsid.empty()) {
      detail = std::string(kWifiJoinFailedPrefix) + st->failedSsid + ".";
      failed = true;
    } else if (s.connected) {
      detail = s.ip;
    }
  }
  set_text(p.statusText, main);
  set_text(p.statusDetail, detail);
  lv_obj_set_style_text_color(p.statusDetail, failed ? pal.amber : pal.dim, 0);
  show(p.statusDetail, !detail.empty());
  show_spinner(p.statusSpinner, trying);
  show(p.statusBars, !trying && s.connected);
  if (s.connected) set_bars(p.statusBars, wifi_signal_bars(s.rssi));
}

void render_saved(WifiState* st, const std::vector<std::string>& saved, const WifiStatus& s) {
  if (!st->everRendered || saved != st->shownSaved) {
    for (WifiSavedRow& r : st->savedRows) discard(r.row);
    st->savedRows.clear();
    for (const std::string& ssid : saved) st->savedRows.push_back(make_saved_row(st, ssid));
    st->shownSaved = saved;
  }
  for (WifiSavedRow& r : st->savedRows) {
    const bool confirming = !lv_obj_has_flag(r.confirm, LV_OBJ_FLAG_HIDDEN);
    show(r.tick, !confirming && s.connected && s.ssid == r.ssid);
  }
  show(st->parts.savedTitle, !saved.empty());
  show(st->savedBox, !saved.empty());
}

void render_nearby(WifiState* st, const std::vector<std::string>& saved) {
  WifiParts& p = st->parts;
  const std::vector<WifiNetwork> nearby =
      st->scanning ? std::vector<WifiNetwork>{} : wifi_nearby(st->scanned, saved);
  if (!st->everRendered || !same_networks(nearby, st->shownNearby)) {
    for (WifiNearbyRow& r : st->nearbyRows) discard(r.row);
    st->nearbyRows.clear();
    for (const WifiNetwork& n : nearby) st->nearbyRows.push_back(make_nearby_row(st, n));
    st->shownNearby = nearby;
  }
  std::string message;
  if (st->scanning) {
    message = kWifiScanning;
  } else if (st->scannedOnce && nearby.empty()) {
    message = kWifiNoNetworks;
  }
  set_text(p.message, message);
  show(st->messageLine, !message.empty());
  show(p.message, !message.empty());
  show_spinner(p.messageSpinner, st->scanning);
  show(st->nearbyBox, !nearby.empty());
}

void refresh(WifiState* st) {
  const std::vector<std::string> saved = st->wifi->saved();
  const WifiStatus s = st->wifi->status();
  render_status(st);
  render_saved(st, saved, s);
  render_nearby(st, saved);
  st->everRendered = true;
}

// ---------------------------------------------------------------- scan

void start_scan(WifiState* st) {
  if (st->scanning) return;
  st->scanning = true;
  refresh(st);
  std::shared_ptr<bool> alive = st->alive;
  st->wifi->scan([st, alive](std::vector<WifiNetwork> found) {
    if (!*alive) return;
    st->scanning = false;
    st->scannedOnce = true;
    st->scanned = std::move(found);
    refresh(st);
  });
}

// ---------------------------------------------------------------- password page

void update_page(WifiState* st) {
  if (!st->pageOpen) return;
  const Palette& pal = theme();
  WifiPasswordParts& p = st->page;
  const size_t len = code_points(lv_textarea_get_text(p.field));
  const bool busy = st->joining || st->pageJoined;
  const bool ready = !busy && len >= static_cast<size_t>(kWifiMinPassword);

  if (ready) {
    lv_obj_remove_state(p.connect, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(p.connect, LV_STATE_DISABLED);
  }
  const char* text = st->joining ? kWifiConnectingButton : st->pageJoined ? kWifiConnectedButton : kWifiConnect;
  set_text(p.connectLabel, text);
  // Ready: a chalk button (whatever the team colour, never mistaken for an
  // error); otherwise a quiet panel.
  lv_obj_set_style_text_color(p.connectLabel, ready ? pal.turf : busy ? pal.chalk : pal.dim, 0);
  lv_obj_set_style_bg_color(p.connect, ready ? pal.chalk : pal.panel, 0);
  show_spinner(p.connectSpinner, st->joining);
  show(st->connectTick, st->pageJoined);

  show(p.error, st->pageError && !busy);
  show(p.hint, !st->pageError && !busy && len < static_cast<size_t>(kWifiMinPassword));

  for (lv_obj_t* o : {p.field, p.keyboard, p.show}) {
    if (busy) {
      lv_obj_add_state(o, LV_STATE_DISABLED);
    } else {
      lv_obj_remove_state(o, LV_STATE_DISABLED);
    }
  }
  // No typing while a join is running.
  lv_obj_t* target = busy ? nullptr : p.field;
  if (lv_keyboard_get_textarea(p.keyboard) != target) lv_keyboard_set_textarea(p.keyboard, target);
  if (busy) {
    lv_obj_remove_state(p.field, LV_STATE_FOCUSED);
  } else {
    lv_obj_add_state(p.field, LV_STATE_FOCUSED);
  }
}

void stop_hold(WifiState* st) {
  if (st->hold == nullptr) return;
  lv_timer_delete(st->hold);
  st->hold = nullptr;
}

void close_page(WifiState* st) {
  if (!st->pageOpen) return;
  if (st->joining && st->joinFromPage) {
    // Walk away from the join: its result is ignored. The device is still
    // trying, so the status card says so until the service answers; a new
    // join replaces the attempt (see WifiService).
    ++st->request;
    st->joining = false;
  }
  if (st->pageJoined) stop_hold(st);
  lv_obj_t* page = st->page.page;
  lv_obj_t* field = st->page.field;
  st->pageOpen = false;
  st->page = WifiPasswordParts{};
  st->pageError = false;
  st->pageJoined = false;
  lv_textarea_set_text(field, "");  // the password leaves with the page
  discard(page);
  refresh(st);
}

// Hands attempt `request` for `ssid` to the service (the caller then calls it).
void sent(WifiState* st, uint32_t request, const std::string& ssid) {
  st->inFlightRequest = request;
  st->inFlightSsid = ssid;
}

// Every attempt's answer arrives exactly once (a replaced one as a failure).
// Only the latest one ends the "Connecting to" state.
void returned(WifiState* st, uint32_t request) {
  if (request == st->inFlightRequest) st->inFlightSsid.clear();
}

void page_connect(WifiState* st) {
  if (!st->pageOpen || st->joining || st->pageJoined || st->hold != nullptr) return;
  std::string password = lv_textarea_get_text(st->page.field);
  if (code_points(password.c_str()) < static_cast<size_t>(kWifiMinPassword)) return;
  st->joining = true;
  st->joinFromPage = true;
  st->joinSsid = st->page.ssid;
  st->failedSsid.clear();
  st->pageError = false;
  const uint32_t request = ++st->request;
  sent(st, request, st->joinSsid);
  update_page(st);
  render_status(st);
  std::shared_ptr<bool> alive = st->alive;
  st->wifi->connect(st->joinSsid, password, [st, alive, request](bool ok) {
    if (!*alive) return;
    returned(st, request);
    if (request != st->request || !st->pageOpen) {
      refresh(st);  // walked away: only the status and lists follow the device
      return;
    }
    st->joining = false;
    if (ok) {
      st->pageJoined = true;
      st->hold = lv_timer_create(
          [](lv_timer_t* t) {
            auto* s = static_cast<WifiState*>(lv_timer_get_user_data(t));
            stop_hold(s);
            const bool first = s->mode == WifiMode::FirstSetup;
            if (!first) close_page(s);
            call(s->cb.onConnected);  // last: the host may close everything
          },
          kWifiConnectedHoldMs, st);
    } else {
      st->pageError = true;
    }
    update_page(st);
    refresh(st);
  });
  std::fill(password.begin(), password.end(), '\0');
}

void on_page_back(lv_event_t* e) { close_page(static_cast<WifiState*>(lv_event_get_user_data(e))); }

void on_page_connect(lv_event_t* e) { page_connect(static_cast<WifiState*>(lv_event_get_user_data(e))); }

void on_field(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  if (!st->pageOpen) return;  // being cleared on close
  switch (lv_event_get_code(e)) {
    case LV_EVENT_VALUE_CHANGED:
      st->pageError = false;
      update_page(st);
      break;
    case LV_EVENT_CLICKED:
      if (!st->joining && !st->pageJoined) show(st->page.keyboard, true);
      break;
    case LV_EVENT_READY:  // Enter on the one-line field
      page_connect(st);
      break;
    default: break;
  }
}

void on_keyboard(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  if (!st->pageOpen) return;
  if (lv_event_get_code(e) == LV_EVENT_READY) {
    page_connect(st);
  } else {  // the close key
    show(st->page.keyboard, false);
  }
}

void on_show(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  if (!st->pageOpen || st->joining || st->pageJoined) return;
  const bool hidden = lv_textarea_get_password_mode(st->page.field);
  lv_textarea_set_password_mode(st->page.field, !hidden);
  set_text(st->page.showLabel, hidden ? kWifiHide : kWifiShow);
}

void open_page(WifiState* st, const std::string& ssid) {
  if (st->pageOpen) return;
  const Palette& pal = theme();
  st->pageOpen = true;
  st->pageError = false;
  st->pageJoined = false;
  WifiPasswordParts& p = st->page;
  p = WifiPasswordParts{};
  p.ssid = ssid;

  p.page = plain(st->root);
  lv_obj_set_size(p.page, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(p.page, pal.turf, 0);
  lv_obj_set_style_bg_opa(p.page, LV_OPA_COVER, 0);
  lv_obj_add_flag(p.page, LV_OBJ_FLAG_CLICKABLE);  // taps stay on the page
  lv_obj_update_layout(p.page);

  const widgets::PageHeader header = widgets::page_header_create(p.page, "");
  p.back = header.back;
  p.title = header.title;
  lv_label_set_long_mode(p.title, LV_LABEL_LONG_DOT);
  lv_obj_set_size(p.title, 240 - 2 * widgets::kPageSide, bigshoulders_28.line_height);
  lv_label_set_text(p.title, ssid.c_str());
  lv_obj_add_event_cb(p.back, on_page_back, LV_EVENT_CLICKED, st);

  p.field = lv_textarea_create(p.page);
  widgets::style_text_field(p.field, kFieldH);
  lv_obj_set_style_text_font(p.field, field_font(), 0);
  lv_obj_set_style_pad_right(p.field, kShowW, 0);
  lv_textarea_set_one_line(p.field, true);
  lv_textarea_set_password_mode(p.field, true);
  lv_textarea_set_password_show_time(p.field, 0);  // mask each character at once
  lv_textarea_set_max_length(p.field, kWifiMaxPassword);
  lv_textarea_set_placeholder_text(p.field, kWifiPasswordPlaceholder);
  lv_obj_set_size(p.field, kContentW, kFieldH);
  lv_obj_set_pos(p.field, kFieldX, kFieldY);
  lv_obj_remove_flag(p.field, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_set_style_opa(p.field, LV_OPA_60, LV_STATE_DISABLED);
  lv_obj_add_event_cb(p.field, on_field, LV_EVENT_VALUE_CHANGED, st);
  lv_obj_add_event_cb(p.field, on_field, LV_EVENT_CLICKED, st);
  lv_obj_add_event_cb(p.field, on_field, LV_EVENT_READY, st);

  // Show / Hide, over the right end of the field.
  p.show = plain(p.page);
  lv_obj_add_flag(p.show, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(p.show, kShowW, kFieldH);
  lv_obj_set_pos(p.show, kFieldX + kContentW - kShowW, kFieldY);
  lv_obj_set_style_opa(p.show, LV_OPA_60, LV_STATE_DISABLED);
  lv_obj_add_event_cb(p.show, on_show, LV_EVENT_CLICKED, st);
  p.showLabel = text_label(p.show, &hanken_12, pal.dim, kWifiShow);
  lv_obj_center(p.showLabel);

  p.hint = text_label(p.page, &hanken_10, pal.dim, kWifiPasswordHint);
  lv_obj_set_pos(p.hint, widgets::kPageSide, kNoteY);
  p.error = text_label(p.page, &hanken_12, pal.amber, kWifiWrongPassword);
  lv_label_set_long_mode(p.error, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(p.error, 240 - 2 * widgets::kPageSide);
  lv_obj_set_pos(p.error, widgets::kPageSide, kNoteY - 1);

  p.connect = plain(p.page);
  lv_obj_add_flag(p.connect, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(p.connect, kContentW, kButtonH);
  lv_obj_set_pos(p.connect, kFieldX, kButtonY);
  lv_obj_set_style_radius(p.connect, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(p.connect, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_opa(p.connect, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_set_flex_flow(p.connect, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.connect, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(p.connect, 8, 0);
  lv_obj_add_event_cb(p.connect, on_page_connect, LV_EVENT_CLICKED, st);
  p.connectSpinner = spinner(p.connect);
  st->connectTick = widgets::tick(p.connect);
  p.connectLabel = text_label(p.connect, &hanken_14, pal.chalk, kWifiConnect);

  p.keyboard = lv_keyboard_create(p.page);
  widgets::style_keyboard(p.keyboard);
  lv_keyboard_set_mode(p.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(p.keyboard, LV_PCT(100), kKeyboardH);
  lv_obj_align(p.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_opa(p.keyboard, LV_OPA_60, LV_STATE_DISABLED);
  lv_keyboard_set_textarea(p.keyboard, p.field);
  lv_obj_add_event_cb(p.keyboard, on_keyboard, LV_EVENT_READY, st);
  lv_obj_add_event_cb(p.keyboard, on_keyboard, LV_EVENT_CANCEL, st);

  update_page(st);
}

// ---------------------------------------------------------------- list events

bool busy(const WifiState* st) { return st->joining || st->hold != nullptr || st->pageOpen; }

void start_join(WifiState* st, const std::string& ssid, bool saved) {
  if (busy(st)) return;
  st->joining = true;
  st->joinFromPage = false;
  st->joinSsid = ssid;
  st->failedSsid.clear();
  const uint32_t request = ++st->request;
  sent(st, request, ssid);
  render_status(st);
  std::shared_ptr<bool> alive = st->alive;
  auto done = [st, alive, request, ssid](bool ok) {
    if (!*alive) return;
    returned(st, request);
    if (request != st->request) {
      refresh(st);
      return;
    }
    st->joining = false;
    if (!ok) st->failedSsid = ssid;
    refresh(st);
    if (!ok) return;
    if (st->mode == WifiMode::FirstSetup) {
      // Let "Connected to ..." show before the host moves on.
      st->hold = lv_timer_create(
          [](lv_timer_t* t) {
            auto* s = static_cast<WifiState*>(lv_timer_get_user_data(t));
            stop_hold(s);
            call(s->cb.onConnected);
          },
          kWifiConnectedHoldMs, st);
    } else {
      call(st->cb.onConnected);
    }
  };
  if (saved) {
    st->wifi->connect_saved(ssid, done);
  } else {
    st->wifi->connect(ssid, "", done);
  }
}

WifiSavedRow* saved_row_of(WifiState* st, lv_obj_t* obj, lv_obj_t* WifiSavedRow::*member) {
  for (WifiSavedRow& r : st->savedRows) {
    if (r.*member == obj) return &r;
  }
  return nullptr;
}

void on_saved_row(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  WifiSavedRow* r = saved_row_of(st, static_cast<lv_obj_t*>(lv_event_get_target(e)), &WifiSavedRow::row);
  if (r == nullptr || !lv_obj_has_flag(r->confirm, LV_OBJ_FLAG_HIDDEN)) return;
  const WifiStatus s = st->wifi->status();
  if (s.connected && s.ssid == r->ssid) return;  // already joined
  const std::string ssid = r->ssid;
  start_join(st, ssid, true);
}

void on_forget(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  WifiSavedRow* r = saved_row_of(st, static_cast<lv_obj_t*>(lv_event_get_target(e)), &WifiSavedRow::forget);
  if (r == nullptr) return;
  // One question at a time.
  for (WifiSavedRow& other : st->savedRows) {
    if (&other != r && !lv_obj_has_flag(other.confirm, LV_OBJ_FLAG_HIDDEN)) set_confirm(other, false);
  }
  set_confirm(*r, true);
  refresh(st);
}

void on_confirm_cancel(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  WifiSavedRow* r =
      saved_row_of(st, static_cast<lv_obj_t*>(lv_event_get_target(e)), &WifiSavedRow::confirmCancel);
  if (r == nullptr) return;
  set_confirm(*r, false);
  refresh(st);
}

void on_confirm_forget(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  WifiSavedRow* r =
      saved_row_of(st, static_cast<lv_obj_t*>(lv_event_get_target(e)), &WifiSavedRow::confirmForget);
  if (r == nullptr) return;
  const std::string ssid = r->ssid;  // the row goes with the refresh
  st->wifi->forget(ssid);
  if (st->failedSsid == ssid) st->failedSsid.clear();
  refresh(st);
}

void on_nearby_row(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
  for (const WifiNearbyRow& r : st->nearbyRows) {
    if (r.row != target) continue;
    if (busy(st)) return;
    const WifiNetwork n = r.network;  // the rows may be rebuilt
    if (n.open) {
      start_join(st, n.ssid, false);
    } else {
      open_page(st, n.ssid);
    }
    return;
  }
}

void on_back(lv_event_t* e) { call(static_cast<WifiState*>(lv_event_get_user_data(e))->cb.onBack); }

void on_scan(lv_event_t* e) { start_scan(static_cast<WifiState*>(lv_event_get_user_data(e))); }

void on_delete(lv_event_t* e) {
  auto* st = static_cast<WifiState*>(lv_event_get_user_data(e));
  *st->alive = false;
  stop_hold(st);
  // Children (the page and its field) are still there: clear the password.
  if (st->pageOpen) {
    lv_obj_t* field = st->page.field;
    st->pageOpen = false;
    lv_textarea_set_text(field, "");
  }
  delete st;
}

WifiState* wifi_state(lv_obj_t* screen) {
  if (screen == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(screen); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(screen, i)) == on_delete) return state_of(screen);
  }
  return nullptr;
}

lv_obj_t* section_title(lv_obj_t* parent, const char* text) {
  lv_obj_t* l = text_label(parent, &hanken_10, theme().dim, text);
  lv_obj_set_style_pad_top(l, 8, 0);
  lv_obj_set_style_pad_left(l, 2, 0);
  return l;
}

// A plain full-width column for rows inside the list.
lv_obj_t* row_box(lv_obj_t* parent) {
  lv_obj_t* b = plain(parent);
  lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(b, widgets::kListGap, 0);
  return b;
}

void build(WifiState* st) {
  const Palette& pal = theme();
  WifiParts& p = st->parts;
  lv_obj_t* root = st->root;
  const bool first = st->mode == WifiMode::FirstSetup;

  const widgets::PageHeader header = widgets::page_header_create(root, first ? kWifiFirstTitle : kWifiTitle);
  p.back = header.back;
  p.title = header.title;
  lv_obj_add_event_cb(p.back, on_back, LV_EVENT_CLICKED, st);
  p.help = text_label(root, &hanken_12, pal.dim, kWifiFirstHelp);
  lv_obj_set_pos(p.help, widgets::kPageSide, kHelpY);
  show(p.help, first);

  const int32_t bodyY = first ? kFirstBodyY : widgets::kPageBodyY;
  p.list = widgets::scroll_column(root);
  lv_obj_set_pos(p.list, 0, bodyY);
  lv_obj_set_size(p.list, LV_PCT(100), lv_obj_get_height(root) - bodyY);

  // Status card: the state, the IP (or why a join failed), the signal.
  p.statusCard = widgets::card(p.list, false);
  lv_obj_set_size(p.statusCard, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(p.statusCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(p.statusCard, kRowPad, 0);
  lv_obj_set_style_pad_ver(p.statusCard, 11, 0);
  lv_obj_set_style_pad_row(p.statusCard, 2, 0);
  p.statusText = line_label(p.statusCard, &hanken_14, pal.chalk, kStatusTextW);
  p.statusDetail = line_label(p.statusCard, &hanken_10, pal.dim, kStatusTextW);
  p.statusBars = signal_bars(p.statusCard);
  lv_obj_add_flag(p.statusBars, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(p.statusBars, LV_ALIGN_RIGHT_MID, 0, 0);
  p.statusSpinner = spinner(p.statusCard);
  lv_obj_add_flag(p.statusSpinner, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(p.statusSpinner, LV_ALIGN_RIGHT_MID, 0, 0);

  p.savedTitle = section_title(p.list, kWifiSavedTitle);
  st->savedBox = row_box(p.list);

  // "Nearby networks" with "Scan again" on the same line.
  lv_obj_t* head = plain(p.list);
  lv_obj_set_size(head, LV_PCT(100), kSmallButtonH + 10);
  p.nearbyTitle = text_label(head, &hanken_10, pal.dim, kWifiNearbyTitle);
  lv_obj_align(p.nearbyTitle, LV_ALIGN_BOTTOM_LEFT, 2, -4);
  p.scanAgain = pill(head, kWifiScanAgain, false, kScanButtonW);
  lv_obj_align(p.scanAgain, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(p.scanAgain, on_scan, LV_EVENT_CLICKED, st);

  st->messageLine = plain(p.list);
  lv_obj_set_size(st->messageLine, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(st->messageLine, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(st->messageLine, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(st->messageLine, 8, 0);
  lv_obj_set_style_pad_hor(st->messageLine, 4, 0);
  lv_obj_set_style_pad_ver(st->messageLine, 6, 0);
  p.messageSpinner = spinner(st->messageLine);
  p.message = label(st->messageLine, &hanken_12, pal.chalk);
  lv_label_set_long_mode(p.message, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(p.message, kContentW - 2 * 4 - kSpinnerSize - 8);

  st->nearbyBox = row_box(p.list);
}

}  // namespace

// ---------------------------------------------------------------- public

std::string wifi_status_text(const WifiStatus& status) {
  return status.connected ? std::string(kWifiConnectedPrefix) + status.ssid : kWifiNotConnected;
}

int wifi_signal_bars(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

std::vector<WifiNetwork> wifi_nearby(const std::vector<WifiNetwork>& scanned,
                                     const std::vector<std::string>& saved) {
  std::vector<WifiNetwork> out;
  for (const WifiNetwork& n : scanned) {
    if (n.ssid.empty()) continue;
    if (std::find(saved.begin(), saved.end(), n.ssid) != saved.end()) continue;
    auto it = std::find_if(out.begin(), out.end(), [&](const WifiNetwork& o) { return o.ssid == n.ssid; });
    if (it == out.end()) {
      out.push_back(n);
    } else if (n.rssi > it->rssi) {
      *it = n;
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });
  return out;
}

lv_obj_t* wifi_create(lv_obj_t* parent, WifiService& wifi, WifiMode mode,
                      const WifiCallbacks& callbacks) {
  auto* st = new WifiState();
  st->wifi = &wifi;
  st->mode = mode;
  st->cb = callbacks;

  lv_obj_t* root = plain(parent);
  st->root = root;
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, theme().turf, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_user_data(root, st);
  lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, st);
  lv_obj_update_layout(root);

  build(st);
  refresh(st);
  start_scan(st);
  return root;
}

void wifi_refresh(lv_obj_t* screen) {
  WifiState* st = wifi_state(screen);
  if (st != nullptr) refresh(st);
}

void wifi_scan(lv_obj_t* screen) {
  WifiState* st = wifi_state(screen);
  if (st != nullptr) start_scan(st);
}

void wifi_open_password(lv_obj_t* screen, const std::string& ssid) {
  WifiState* st = wifi_state(screen);
  if (st != nullptr && !st->joining && st->hold == nullptr) open_page(st, ssid);
}

const WifiParts* wifi_parts(lv_obj_t* screen) {
  WifiState* st = wifi_state(screen);
  return st != nullptr ? &st->parts : nullptr;
}

std::vector<WifiSavedRow> wifi_saved_rows(lv_obj_t* screen) {
  WifiState* st = wifi_state(screen);
  return st != nullptr ? st->savedRows : std::vector<WifiSavedRow>{};
}

std::vector<WifiNearbyRow> wifi_nearby_rows(lv_obj_t* screen) {
  WifiState* st = wifi_state(screen);
  return st != nullptr ? st->nearbyRows : std::vector<WifiNearbyRow>{};
}

const WifiPasswordParts* wifi_password_parts(lv_obj_t* screen) {
  WifiState* st = wifi_state(screen);
  return st != nullptr && st->pageOpen ? &st->page : nullptr;
}

int wifi_bars_level(lv_obj_t* bars) {
  return bars != nullptr ? static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(bars))) : 0;
}
