#include "screens/settings.h"

#include <stdint.h>

#include <algorithm>
#include <utility>

#include "fonts.h"
#include "format.h"
#include "screens/widgets.h"
#include "theme.h"
#include "timezone.h"
#include "version.h"

// Layout: the page header ("< Back", "Settings") over a scrolling column of
// panel cards, one per setting, and a dim About line at the end.
namespace {

using widgets::label;
using widgets::plain;
using widgets::set_text;
using widgets::show;

constexpr int32_t kCardPad = 10;
constexpr int32_t kCrestSize = 24;
// Team line: 240 - 2 * 8 (list margins) - 2 * kCardPad = 204 px of content.
constexpr int32_t kTeamTextX = kCrestSize + 8;
constexpr int32_t kTeamTextW = 204 - kTeamTextX - 18;  // 18: room for the chevron
constexpr int32_t kTeamLineH = 34;
constexpr int32_t kSwitchW = 40;
constexpr int32_t kSwitchH = 22;
constexpr int32_t kStepButton = 28;
constexpr int32_t kStepValueW = 54;
constexpr int32_t kStepLineH = 30;
constexpr int32_t kSliderExtraHit = 12;
constexpr int32_t kSoundRowH = 30;
constexpr int32_t kTestButtonH = 32;
constexpr lv_opa_t kVoltageOpa = 160;  // the voltage a step quieter than the status

enum Step { FromMinus, FromPlus, ToMinus, ToPlus, PctMinus, PctPlus, VolMinus, VolPlus };

struct SettingsState {
  Settings s;
  ImageProvider* images = nullptr;
  SettingsCallbacks cb;
  SettingsParts parts{};
  std::string shownCrest;
  bool everApplied = false;
};

SettingsState* state_of(lv_obj_t* screen) {
  return static_cast<SettingsState*>(lv_obj_get_user_data(screen));
}

std::string hour_text(int h) {
  std::string s;
  s += static_cast<char>('0' + h / 10);
  s += static_cast<char>('0' + h % 10);
  return s + ":00";
}

std::string percent_text(int p) { return std::to_string(p) + "%"; }

std::string zone_offset_line(const std::string& zoneId, int64_t now) {
  const int off = offset_seconds(zoneId, now);
  return utc_offset_text(off) + " \xC2\xB7 Local time " + hh_mm(now + off);
}

const char* zone_label(const std::string& zoneId) {
  const Zone* z = find_zone(zoneId);
  return z != nullptr ? z->label : zoneId.c_str();
}

// ---------------------------------------------------------------- events

void notify_changed(SettingsState* st) {
  // Copied first: the host may replace this screen while handling it.
  const std::function<void(const Settings&)> f = st->cb.onChanged;
  const Settings s = st->s;
  if (f) f(s);
}

void call(const std::function<void()>& fn) {
  const std::function<void()> f = fn;
  if (f) f();
}

void on_back(lv_event_t* e) { call(static_cast<SettingsState*>(lv_event_get_user_data(e))->cb.onBack); }
void on_team(lv_event_t* e) { call(static_cast<SettingsState*>(lv_event_get_user_data(e))->cb.onTeam); }
void on_zone(lv_event_t* e) { call(static_cast<SettingsState*>(lv_event_get_user_data(e))->cb.onZone); }
void on_wifi(lv_event_t* e) { call(static_cast<SettingsState*>(lv_event_get_user_data(e))->cb.onWifi); }

void on_test_sound(lv_event_t* e) {
  call(static_cast<SettingsState*>(lv_event_get_user_data(e))->cb.onTestSound);
}

void apply(SettingsState* st, int64_t now);

void on_slider(lv_event_t* e) {
  auto* st = static_cast<SettingsState*>(lv_event_get_user_data(e));
  const lv_event_code_t code = lv_event_get_code(e);
  const int v = lv_slider_get_value(st->parts.brightSlider);
  if (code == LV_EVENT_VALUE_CHANGED) {
    if (v == st->s.dayBrightness) return;
    st->s.dayBrightness = v;
    set_text(st->parts.brightValue, percent_text(v));
    const std::function<void(int)> f = st->cb.onBrightnessPreview;
    if (f) f(v);
  } else {  // released or press lost: the value is final
    notify_changed(st);
  }
}

void on_switch(lv_event_t* e) {
  auto* st = static_cast<SettingsState*>(lv_event_get_user_data(e));
  lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (sw == st->parts.nightSwitch) {
    if (on == st->s.nightDim) return;
    st->s.nightDim = on;
    show(st->parts.nightControls, on);
    if (st->parts.soundNightNeedsDim != nullptr) show(st->parts.soundNightNeedsDim, !on);
  } else if (sw == st->parts.soundNightSwitch) {
    if (on == st->s.soundMuteAtNight) return;
    st->s.soundMuteAtNight = on;
  } else {
    if (on == st->s.reducedMotion) return;
    st->s.reducedMotion = on;
  }
  notify_changed(st);
}

void on_sound_mode(lv_event_t* e) {
  auto* st = static_cast<SettingsState*>(lv_event_get_user_data(e));
  lv_obj_t* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  const auto mode = static_cast<SoundMode>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
  if (mode == st->s.soundMode) return;
  st->s.soundMode = mode;
  for (int i = 0; i < 3; ++i) {
    widgets::show(st->parts.soundTicks[i], static_cast<int>(mode) == i);
  }
  notify_changed(st);
}

void on_battery_choice(lv_event_t* e) {
  auto* st = static_cast<SettingsState*>(lv_event_get_user_data(e));
  lv_obj_t* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
  const auto choice = static_cast<BatteryChoice>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
  if (choice == st->s.battery) return;
  st->s.battery = choice;
  for (int i = 0; i < 3; ++i) widgets::show(st->parts.batteryChoiceTicks[i], static_cast<int>(choice) == i);
  notify_changed(st);
}

int volume_index(int percent) {
  for (int i = 0; i < 5; ++i) {
    if (kSoundVolumes[i] >= percent) return i;
  }
  return 4;
}

int wrap_hour(int h) { return ((h % 24) + 24) % 24; }

void on_step(lv_event_t* e) {
  auto* st = static_cast<SettingsState*>(lv_event_get_user_data(e));
  lv_obj_t* button = static_cast<lv_obj_t*>(lv_event_get_target(e));
  const auto step = static_cast<Step>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
  Settings& s = st->s;
  const Settings before = s;
  switch (step) {
    case FromMinus: s.nightStartHour = wrap_hour(s.nightStartHour - 1); break;
    case FromPlus: s.nightStartHour = wrap_hour(s.nightStartHour + 1); break;
    case ToMinus: s.nightEndHour = wrap_hour(s.nightEndHour - 1); break;
    case ToPlus: s.nightEndHour = wrap_hour(s.nightEndHour + 1); break;
    case PctMinus:
      s.nightPercent = s.nightPercent - kNightPercentStep < kNightPercentMin
                           ? kNightPercentMin
                           : s.nightPercent - kNightPercentStep;
      break;
    case PctPlus:
      s.nightPercent = s.nightPercent + kNightPercentStep > kNightPercentMax
                           ? kNightPercentMax
                           : s.nightPercent + kNightPercentStep;
      break;
    case VolMinus: s.soundVolume = kSoundVolumes[std::max(0, volume_index(s.soundVolume) - 1)]; break;
    case VolPlus: s.soundVolume = kSoundVolumes[std::min(4, volume_index(s.soundVolume) + 1)]; break;
  }
  if (s.nightStartHour == before.nightStartHour && s.nightEndHour == before.nightEndHour &&
      s.nightPercent == before.nightPercent && s.soundVolume == before.soundVolume) {
    return;
  }
  if (step == VolMinus || step == VolPlus) set_text(st->parts.volumeValue, percent_text(s.soundVolume));
  set_text(st->parts.nightFrom, hour_text(s.nightStartHour));
  set_text(st->parts.nightTo, hour_text(s.nightEndHour));
  set_text(st->parts.nightPercent, percent_text(s.nightPercent));
  notify_changed(st);
}

void on_delete(lv_event_t* e) { delete static_cast<SettingsState*>(lv_event_get_user_data(e)); }

// ---------------------------------------------------------------- widgets

lv_obj_t* text_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, const char* text) {
  lv_obj_t* l = label(parent, font, color);
  lv_label_set_text(l, text);
  return l;
}

// A ">" at the right edge of a card's content area, outside any flex layout.
lv_obj_t* chevron(lv_obj_t* parent, int32_t x) {
  lv_obj_t* c = text_label(parent, &lv_font_montserrat_14, theme().dim, LV_SYMBOL_RIGHT);
  lv_obj_add_flag(c, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(c, LV_ALIGN_RIGHT_MID, x, 0);
  return c;
}

// A card laid out as a column with the standard padding.
lv_obj_t* column_card(lv_obj_t* list, bool clickable) {
  lv_obj_t* c = widgets::card(list, clickable);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(c, kCardPad, 0);
  lv_obj_set_style_pad_row(c, 4, 0);
  return c;
}

// A full-width line inside a column card, for absolutely placed children.
lv_obj_t* line(lv_obj_t* parent, int32_t height) {
  lv_obj_t* l = plain(parent);
  lv_obj_set_size(l, LV_PCT(100), height);
  return l;
}

lv_obj_t* make_switch(lv_obj_t* parent, SettingsState* st) {
  const Palette& pal = theme();
  lv_obj_t* sw = lv_switch_create(parent);
  lv_obj_remove_style_all(sw);
  lv_obj_set_size(sw, kSwitchW, kSwitchH);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, pal.line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sw, accent_fill(pal.accent),
                            static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                          static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_bg_color(sw, pal.chalk, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sw, -3, LV_PART_KNOB);
  lv_obj_set_ext_click_area(sw, 8);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(sw, on_switch, LV_EVENT_VALUE_CHANGED, st);
  return sw;
}

lv_obj_t* step_button(lv_obj_t* parent, SettingsState* st, Step step, const char* symbol) {
  const Palette& pal = theme();
  lv_obj_t* b = plain(parent);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(b, kStepButton, kStepButton);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_bg_color(b, pal.line, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, mix(pal.line, pal.chalk, 50), LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(b, 2);
  lv_obj_t* l = text_label(b, &lv_font_montserrat_14, pal.chalk, symbol);
  lv_obj_center(l);
  lv_obj_set_user_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(step)));
  lv_obj_add_event_cb(b, on_step, LV_EVENT_CLICKED, st);
  return b;
}

// "Caption      [-] value [+]"
lv_obj_t* stepper(lv_obj_t* parent, SettingsState* st, const char* caption, Step minus, Step plus,
                  lv_obj_t*& minusOut, lv_obj_t*& valueOut, lv_obj_t*& plusOut) {
  const Palette& pal = theme();
  lv_obj_t* l = line(parent, kStepLineH);
  lv_obj_t* cap = text_label(l, &hanken_12, pal.dim, caption);
  lv_obj_align(cap, LV_ALIGN_LEFT_MID, 0, 0);
  plusOut = step_button(l, st, plus, LV_SYMBOL_PLUS);
  lv_obj_align(plusOut, LV_ALIGN_RIGHT_MID, 0, 0);
  valueOut = label(l, &bigshoulders_22, pal.chalk);
  lv_obj_set_width(valueOut, kStepValueW);
  lv_obj_set_style_text_align(valueOut, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(valueOut, LV_ALIGN_RIGHT_MID, -kStepButton, 0);
  minusOut = step_button(l, st, minus, LV_SYMBOL_MINUS);
  lv_obj_align(minusOut, LV_ALIGN_RIGHT_MID, -kStepButton - kStepValueW, 0);
  return l;
}

lv_obj_t* help_text(lv_obj_t* parent, const char* text) {
  lv_obj_t* l = text_label(parent, &hanken_10, theme().dim, text);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

// One row of a single-choice list: the text on the left, a tick on the right
// when chosen. `value` goes into the row's user data for `cb`.
lv_obj_t* choice_row(lv_obj_t* parent, SettingsState* st, const char* text, int value, lv_event_cb_t cb,
                     lv_obj_t*& tickOut) {
  const Palette& pal = theme();
  lv_obj_t* row = line(parent, kSoundRowH);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_bg_color(row, pal.line, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(value)));
  lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, st);
  lv_obj_align(text_label(row, &hanken_12, pal.chalk, text), LV_ALIGN_LEFT_MID, 0, 0);
  tickOut = widgets::tick(row);
  lv_obj_align(tickOut, LV_ALIGN_RIGHT_MID, 0, 0);
  return row;
}

void style_slider(lv_obj_t* slider) {
  const Palette& pal = theme();
  lv_obj_remove_style_all(slider);
  lv_obj_set_style_bg_color(slider, pal.line, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, accent_fill(pal.accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, pal.chalk, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
  lv_obj_set_ext_click_area(slider, kSliderExtraHit);
}

void build(SettingsState* st, lv_obj_t* root) {
  const Palette& pal = theme();
  SettingsParts& p = st->parts;

  const widgets::PageHeader header = widgets::page_header_create(root, "Settings");
  p.back = header.back;
  p.title = header.title;
  lv_obj_add_event_cb(p.back, on_back, LV_EVENT_CLICKED, st);

  p.list = widgets::scroll_column(root);
  lv_obj_set_pos(p.list, 0, widgets::kPageBodyY);
  lv_obj_set_size(p.list, LV_PCT(100), lv_obj_get_height(root) - widgets::kPageBodyY);

  // Favourite team.
  p.teamRow = column_card(p.list, true);
  lv_obj_set_style_pad_row(p.teamRow, 6, 0);
  lv_obj_add_event_cb(p.teamRow, on_team, LV_EVENT_CLICKED, st);
  text_label(p.teamRow, &hanken_10, pal.dim, "Favourite team");
  // Crest, then the name over the league (as in the picker): a long name
  // ends in "..." before the chevron and never meets the league text.
  lv_obj_t* teamLine = line(p.teamRow, kTeamLineH);
  p.teamCrest = widgets::crest_image(teamLine, kCrestSize);
  lv_obj_align(p.teamCrest, LV_ALIGN_LEFT_MID, 0, 0);
  p.teamName = label(teamLine, &hanken_14, pal.chalk);
  lv_label_set_long_mode(p.teamName, LV_LABEL_LONG_DOT);
  lv_obj_set_size(p.teamName, kTeamTextW, hanken_14.line_height);
  lv_obj_set_pos(p.teamName, kTeamTextX, 0);
  p.teamLeague = label(teamLine, &hanken_10, pal.dim);
  lv_label_set_long_mode(p.teamLeague, LV_LABEL_LONG_DOT);
  lv_obj_set_size(p.teamLeague, kTeamTextW, hanken_10.line_height);
  lv_obj_set_pos(p.teamLeague, kTeamTextX, hanken_14.line_height);
  chevron(p.teamRow, 0);

  // Time zone.
  p.zoneRow = column_card(p.list, true);
  lv_obj_add_event_cb(p.zoneRow, on_zone, LV_EVENT_CLICKED, st);
  text_label(p.zoneRow, &hanken_10, pal.dim, "Time zone");
  p.zoneLabel = label(p.zoneRow, &hanken_14, pal.chalk);
  lv_label_set_long_mode(p.zoneLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_size(p.zoneLabel, 180, hanken_14.line_height);
  p.zoneOffset = label(p.zoneRow, &hanken_10, pal.dim);
  chevron(p.zoneRow, 0);

  // Brightness.
  lv_obj_t* bright = column_card(p.list, false);
  lv_obj_set_style_pad_row(bright, 8, 0);
  lv_obj_t* brightHead = line(bright, 24);
  lv_obj_align(text_label(brightHead, &hanken_14, pal.chalk, "Brightness"), LV_ALIGN_LEFT_MID, 0, 0);
  p.brightValue = label(brightHead, &bigshoulders_22, pal.chalk);
  lv_obj_align(p.brightValue, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_t* sliderLine = line(bright, 20);
  p.brightSlider = lv_slider_create(sliderLine);
  style_slider(p.brightSlider);
  lv_slider_set_range(p.brightSlider, kBrightnessMin, kBrightnessMax);
  lv_obj_set_size(p.brightSlider, LV_PCT(100), 6);
  lv_obj_align(p.brightSlider, LV_ALIGN_CENTER, 0, 0);
  // The knob overhangs the track ends: keep it inside the card.
  lv_obj_set_style_pad_hor(sliderLine, 9, 0);
  lv_obj_add_event_cb(p.brightSlider, on_slider, LV_EVENT_VALUE_CHANGED, st);
  lv_obj_add_event_cb(p.brightSlider, on_slider, LV_EVENT_RELEASED, st);
  lv_obj_add_event_cb(p.brightSlider, on_slider, LV_EVENT_PRESS_LOST, st);

  // Night dimming.
  lv_obj_t* night = column_card(p.list, false);
  lv_obj_t* nightHead = line(night, kSwitchH + 2);
  lv_obj_align(text_label(nightHead, &hanken_14, pal.chalk, "Night dimming"), LV_ALIGN_LEFT_MID, 0, 0);
  p.nightSwitch = make_switch(nightHead, st);
  p.nightControls = plain(night);
  lv_obj_set_size(p.nightControls, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(p.nightControls, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(p.nightControls, 4, 0);
  lv_obj_set_style_pad_top(p.nightControls, 4, 0);
  stepper(p.nightControls, st, "From", FromMinus, FromPlus, p.nightFromMinus, p.nightFrom, p.nightFromPlus);
  stepper(p.nightControls, st, "Until", ToMinus, ToPlus, p.nightToMinus, p.nightTo, p.nightToPlus);
  stepper(p.nightControls, st, "Night brightness", PctMinus, PctPlus, p.nightPercentMinus,
          p.nightPercent, p.nightPercentPlus);
  p.nightHelp = help_text(night, kNightHelp);

  // Reduced motion.
  lv_obj_t* motion = column_card(p.list, false);
  lv_obj_t* motionHead = line(motion, kSwitchH + 2);
  lv_obj_align(text_label(motionHead, &hanken_14, pal.chalk, "Reduced motion"), LV_ALIGN_LEFT_MID, 0, 0);
  p.motionSwitch = make_switch(motionHead, st);
  p.motionHelp = help_text(motion, kMotionHelp);

  // Sound, when the host has a speaker: what plays, how loud, quiet hours.
  if (st->cb.onTestSound) {
    lv_obj_t* sound = column_card(p.list, false);
    lv_obj_set_style_pad_row(sound, 2, 0);
    text_label(sound, &hanken_14, pal.chalk, "Sound");
    for (int i = 0; i < 3; ++i) {
      p.soundRows[i] = choice_row(sound, st, kSoundModeText[i], i, on_sound_mode, p.soundTicks[i]);
    }
    p.soundHelp = help_text(sound, kSoundHelp);

    lv_obj_t* volume = column_card(p.list, false);
    stepper(volume, st, "Volume", VolMinus, VolPlus, p.volumeMinus, p.volumeValue, p.volumePlus);
    p.testSound = plain(volume);
    lv_obj_add_flag(p.testSound, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(p.testSound, LV_PCT(100), kTestButtonH);
    lv_obj_set_style_radius(p.testSound, 8, 0);
    lv_obj_set_style_bg_color(p.testSound, pal.line, 0);
    lv_obj_set_style_bg_opa(p.testSound, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p.testSound, mix(pal.line, pal.chalk, 50), LV_STATE_PRESSED);
    lv_obj_add_event_cb(p.testSound, on_test_sound, LV_EVENT_CLICKED, st);
    lv_obj_center(text_label(p.testSound, &hanken_14, pal.chalk, "Test sound"));

    lv_obj_t* quiet = column_card(p.list, false);
    lv_obj_t* quietHead = line(quiet, kSwitchH + 2);
    lv_obj_align(text_label(quietHead, &hanken_14, pal.chalk, "Mute at night"), LV_ALIGN_LEFT_MID, 0, 0);
    p.soundNightSwitch = make_switch(quietHead, st);
    p.soundNightHelp = help_text(quiet, kSoundNightHelp);
    p.soundNightNeedsDim = help_text(quiet, kSoundNightNeedsDim);
  }

  // Battery, when the host has battery hardware: whether one is connected.
  if (st->cb.battery) {
    lv_obj_t* battery = column_card(p.list, false);
    lv_obj_set_style_pad_row(battery, 2, 0);
    text_label(battery, &hanken_14, pal.chalk, "Battery");
    for (int i = 0; i < 3; ++i) {
      p.batteryChoiceRows[i] =
          choice_row(battery, st, kBatteryChoiceText[i], i, on_battery_choice, p.batteryChoiceTicks[i]);
    }
    p.batteryChoiceHelp = help_text(battery, kBatteryHelp);
  }

  // Wi-Fi, when the host supports it.
  if (st->cb.onWifi) {
    // "Wi-Fi" over the connection state, like the time zone row.
    p.wifiRow = column_card(p.list, true);
    lv_obj_set_style_pad_row(p.wifiRow, 2, 0);
    lv_obj_add_event_cb(p.wifiRow, on_wifi, LV_EVENT_CLICKED, st);
    text_label(p.wifiRow, &hanken_14, pal.chalk, "Wi-Fi");
    p.wifiStatus = label(p.wifiRow, &hanken_10, pal.dim);
    lv_label_set_long_mode(p.wifiStatus, LV_LABEL_LONG_DOT);
    lv_obj_set_size(p.wifiStatus, 180, hanken_10.line_height);
    chevron(p.wifiRow, 0);
  }

  // About.
  p.about = label(p.list, &hanken_10, pal.dim);
  lv_obj_set_width(p.about, LV_PCT(100));
  lv_obj_set_style_text_align(p.about, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(p.about, 6, 0);
  lv_label_set_text(p.about, (std::string(kAppName) + " " + kAppVersion).c_str());
  // Battery: one centred line, the status then the voltage a step quieter.
  p.batteryLine = plain(p.list);
  lv_obj_set_size(p.batteryLine, LV_PCT(100), hanken_10.line_height);
  lv_obj_set_flex_flow(p.batteryLine, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.batteryLine, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(p.batteryLine, 6, 0);
  p.batteryStatus = label(p.batteryLine, &hanken_10, pal.dim);
  p.batteryVoltage = label(p.batteryLine, &hanken_10, pal.dim);
  lv_obj_set_style_text_opa(p.batteryVoltage, kVoltageOpa, 0);
  lv_obj_add_flag(p.batteryLine, LV_OBJ_FLAG_HIDDEN);
  p.dataStatus = label(p.list, &hanken_10, pal.dim);
  lv_obj_set_width(p.dataStatus, LV_PCT(100));
  lv_label_set_long_mode(p.dataStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(p.dataStatus, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(p.dataStatus, "");
}

void set_checked(lv_obj_t* obj, bool on) {
  if (lv_obj_has_state(obj, LV_STATE_CHECKED) == on) return;
  if (on) {
    lv_obj_add_state(obj, LV_STATE_CHECKED);
  } else {
    lv_obj_remove_state(obj, LV_STATE_CHECKED);
  }
}

void apply(SettingsState* st, int64_t now) {
  const Settings& s = st->s;
  SettingsParts& p = st->parts;
  set_text(p.teamName, s.favourite.name);
  set_text(p.teamLeague, league_name(s.favourite.league));
  const std::string crest = sized(s.favourite.crest, kCrestSize, kCrestSize);
  if (!st->everApplied || crest != st->shownCrest) {
    widgets::crest_load(p.teamCrest, *st->images, crest);
    st->shownCrest = crest;
  }
  widgets::crest_sync(p.teamCrest);
  set_text(p.zoneLabel, zone_label(s.zoneId));
  set_text(p.zoneOffset, zone_offset_line(s.zoneId, now));
  set_text(p.brightValue, percent_text(s.dayBrightness));
  // Never fight the finger: the slider is left alone while it is dragged.
  if (!lv_obj_has_state(p.brightSlider, LV_STATE_PRESSED) &&
      lv_slider_get_value(p.brightSlider) != s.dayBrightness) {
    lv_slider_set_value(p.brightSlider, s.dayBrightness, LV_ANIM_OFF);
  }
  set_checked(p.nightSwitch, s.nightDim);
  show(p.nightControls, s.nightDim);
  set_text(p.nightFrom, hour_text(s.nightStartHour));
  set_text(p.nightTo, hour_text(s.nightEndHour));
  set_text(p.nightPercent, percent_text(s.nightPercent));
  set_checked(p.motionSwitch, s.reducedMotion);
  if (p.testSound != nullptr) {
    for (int i = 0; i < 3; ++i) show(p.soundTicks[i], static_cast<int>(s.soundMode) == i);
    set_text(p.volumeValue, percent_text(s.soundVolume));
    set_checked(p.soundNightSwitch, s.soundMuteAtNight);
    show(p.soundNightNeedsDim, !s.nightDim);
  }
  if (p.batteryChoiceHelp != nullptr) {
    for (int i = 0; i < 3; ++i) show(p.batteryChoiceTicks[i], static_cast<int>(s.battery) == i);
  }
  st->everApplied = true;
}

SettingsState* settings_state(lv_obj_t* screen) {
  if (screen == nullptr) return nullptr;
  for (uint32_t i = 0; i < lv_obj_get_event_count(screen); ++i) {
    if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(screen, i)) == on_delete) return state_of(screen);
  }
  return nullptr;
}

}  // namespace

lv_obj_t* settings_create(lv_obj_t* parent, const Settings& settings, ImageProvider& images,
                          const SettingsCallbacks& callbacks, int64_t now) {
  auto* st = new SettingsState();
  st->s = settings;
  st->images = &images;
  st->cb = callbacks;

  lv_obj_t* root = plain(parent);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(root, theme().turf, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_user_data(root, st);
  lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, st);
  lv_obj_update_layout(root);

  build(st, root);
  apply(st, now);
  return root;
}

void settings_refresh(lv_obj_t* screen, const Settings& settings, int64_t now) {
  SettingsState* st = settings_state(screen);
  if (st == nullptr) return;
  // While the slider is held its value is the one in progress: the label and
  // the slider stay as the finger left them.
  const bool dragging = lv_obj_has_state(st->parts.brightSlider, LV_STATE_PRESSED);
  const int inProgress = st->s.dayBrightness;
  st->s = settings;
  if (dragging) st->s.dayBrightness = inProgress;
  apply(st, now);
}

void settings_set_wifi_status(lv_obj_t* screen, const std::string& text) {
  SettingsState* st = settings_state(screen);
  if (st != nullptr && st->parts.wifiStatus != nullptr) set_text(st->parts.wifiStatus, text);
}

void settings_set_data_status(lv_obj_t* screen, const std::string& text, bool problem) {
  SettingsState* st = settings_state(screen);
  if (st == nullptr) return;
  lv_obj_t* l = st->parts.dataStatus;
  set_text(l, text);
  const lv_color_t color = problem ? theme().amber : theme().dim;
  if (lv_color_to_u32(lv_obj_get_style_text_color(l, 0)) != lv_color_to_u32(color)) {
    lv_obj_set_style_text_color(l, color, 0);
  }
}

void settings_set_battery(lv_obj_t* screen, const PowerStatus& power) {
  SettingsState* st = settings_state(screen);
  if (st == nullptr) return;
  const SettingsParts& p = st->parts;
  show(p.batteryLine, true);
  set_text(p.batteryStatus, battery_status_text(power));
  const std::string volts = battery_voltage_text(power);
  set_text(p.batteryVoltage, volts);
  show(p.batteryVoltage, !volts.empty());
  const bool low = power.present && !power.charging && power.percent <= battery::kLowPercent;
  const lv_color_t color = low ? theme().amber : theme().dim;
  if (lv_color_to_u32(lv_obj_get_style_text_color(p.batteryStatus, 0)) != lv_color_to_u32(color)) {
    lv_obj_set_style_text_color(p.batteryStatus, color, 0);
  }
}

const Settings* settings_current(lv_obj_t* screen) {
  SettingsState* st = settings_state(screen);
  return st != nullptr ? &st->s : nullptr;
}

const SettingsParts* settings_parts(lv_obj_t* screen) {
  SettingsState* st = settings_state(screen);
  return st != nullptr ? &st->parts : nullptr;
}
