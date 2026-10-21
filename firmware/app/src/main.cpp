// Soccer display firmware. For now: boots, brings up the display, backlight
// and touch under LVGL and shows a driver test screen (build id, slider,
// switch, tap-counting button, touch crosshair, colour and gradient bars).
// The UI, Wi-Fi and the rest are added by the next tasks.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <stdio.h>

#include "board/backlight_ledc.h"
#include "board/display_st7789.h"
#include "board/lvgl_port.h"
#include "board/pins.h"
#include "board/touch.h"
#include "version.h"

namespace {

constexpr uint32_t kSerialWaitMs = 3000;
constexpr uint32_t kStatusMs = 5000;

// Power hold: GPIO7 must be HIGH or the board powers off on battery. The
// output latch is set to 1 BEFORE the output driver is enabled, so the pin
// never dips low (pinMode(OUTPUT) followed by digitalWrite(HIGH) can).
void powerHoldHigh() {
  gpio_set_level((gpio_num_t)pins::kPowerHold, 1);
  gpio_set_direction((gpio_num_t)pins::kPowerHold, GPIO_MODE_INPUT_OUTPUT);  // output + readable
}

// Runs as a static constructor, i.e. before app_main() and initArduino(): the
// first application code to execute after the ESP-IDF startup.
__attribute__((constructor(101))) void earlyPowerHold() { powerHoldHigh(); }

LedcBacklight* s_backlight = nullptr;

// ---- test screen ---------------------------------------------------------------

struct TestUi {
  lv_obj_t* touchLabel = nullptr;
  lv_obj_t* sliderLabel = nullptr;
  lv_obj_t* switchLabel = nullptr;
  lv_obj_t* buttonLabel = nullptr;
  lv_obj_t* statusLabel = nullptr;
  lv_obj_t* cross = nullptr;
  uint32_t taps = 0;
  lv_point_t lastPoint = {0, 0};
  bool wasDown = false;
};
TestUi g_ui;

lv_obj_t* makeBox(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t color) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, color, 0);
  return o;
}

lv_obj_t* makeLabel(lv_obj_t* parent, int x, int y, int w, const char* text, lv_text_align_t align) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, align, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, text);
  return l;
}

// A white-bordered square at each corner and a ring in the centre: the
// targets for the four-corners-and-centre touch check.
void makeTarget(lv_obj_t* parent, int cx, int cy, int size) {
  lv_obj_t* t = lv_obj_create(parent);
  lv_obj_remove_style_all(t);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(t, size, size);
  lv_obj_set_pos(t, cx - size / 2, cy - size / 2);
  lv_obj_set_style_border_width(t, 2, 0);
  lv_obj_set_style_border_color(t, lv_color_white(), 0);
  lv_obj_set_style_border_opa(t, LV_OPA_COVER, 0);
}

void onSlider(lv_event_t* e) {
  lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
  lv_label_set_text_fmt(g_ui.sliderLabel, "slider %d", (int)lv_slider_get_value(s));
}

void onSwitch(lv_event_t* e) {
  lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
  lv_label_set_text_fmt(g_ui.switchLabel, "switch %s", lv_obj_has_state(s, LV_STATE_CHECKED) ? "ON" : "off");
}

void onButton(lv_event_t*) {
  ++g_ui.taps;
  lv_label_set_text_fmt(g_ui.buttonLabel, "Tap me: %lu", (unsigned long)g_ui.taps);
}

// Follows the finger with a large crosshair on the top layer and prints the
// coordinates. Runs in the LVGL task (an lv_timer).
void onTouchTimer(lv_timer_t*) {
  lv_indev_t* indev = lv_indev_get_next(nullptr);
  if (!indev) return;
  const bool down = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  const bool moved = down && (p.x != g_ui.lastPoint.x || p.y != g_ui.lastPoint.y);
  const bool pressChanged = down != g_ui.wasDown;

  // Only touch the crosshair when something changes: setting a flag that is
  // already set still invalidates the area and would redraw 30 times a second.
  const bool hidden = lv_obj_has_flag(g_ui.cross, LV_OBJ_FLAG_HIDDEN);
  if (down) {
    if (hidden) lv_obj_remove_flag(g_ui.cross, LV_OBJ_FLAG_HIDDEN);
    if (hidden || moved) lv_obj_set_pos(g_ui.cross, p.x - 40, p.y - 40);
  } else if (!hidden) {
    lv_obj_add_flag(g_ui.cross, LV_OBJ_FLAG_HIDDEN);
  }

  if (pressChanged || moved) {
    if (down) {
      lv_label_set_text_fmt(g_ui.touchLabel, "touch DOWN x=%d y=%d", (int)p.x, (int)p.y);
      g_ui.lastPoint = p;
    } else {
      lv_label_set_text_fmt(g_ui.touchLabel, "released x=%d y=%d", (int)g_ui.lastPoint.x, (int)g_ui.lastPoint.y);
    }
    g_ui.wasDown = down;
  }
}

void buildTestScreen() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Corner and centre targets (drawn first, everything else sits above).
  makeTarget(scr, 14, 14, 28);
  makeTarget(scr, kDisplayWidth - 14, 14, 28);
  makeTarget(scr, 14, kDisplayHeight - 14, 28);
  makeTarget(scr, kDisplayWidth - 14, kDisplayHeight - 14, 28);
  makeTarget(scr, kDisplayWidth / 2, kDisplayHeight / 2, 24);

  char build[96];
  snprintf(build, sizeof(build), "%s %s\nbuilt %s %s", kAppName, kAppVersion, __DATE__, __TIME__);
  makeLabel(scr, 34, 4, kDisplayWidth - 68, build, LV_TEXT_ALIGN_CENTER);
  g_ui.touchLabel = makeLabel(scr, 34, 44, kDisplayWidth - 68, "touch: none yet", LV_TEXT_ALIGN_CENTER);

  lv_obj_t* slider = lv_slider_create(scr);
  lv_obj_set_pos(slider, 30, 76);
  lv_obj_set_size(slider, kDisplayWidth - 60, 16);
  lv_slider_set_value(slider, 40, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, onSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  g_ui.sliderLabel = makeLabel(scr, 30, 98, kDisplayWidth - 60, "slider 40", LV_TEXT_ALIGN_CENTER);

  lv_obj_t* sw = lv_switch_create(scr);
  lv_obj_set_pos(sw, 50, 124);
  lv_obj_add_event_cb(sw, onSwitch, LV_EVENT_VALUE_CHANGED, nullptr);
  g_ui.switchLabel = makeLabel(scr, 118, 130, 90, "switch off", LV_TEXT_ALIGN_LEFT);

  lv_obj_t* btn = lv_button_create(scr);
  lv_obj_set_pos(btn, 40, 182);
  lv_obj_set_size(btn, kDisplayWidth - 80, 48);
  lv_obj_add_event_cb(btn, onButton, LV_EVENT_CLICKED, nullptr);
  g_ui.buttonLabel = lv_label_create(btn);
  lv_label_set_text(g_ui.buttonLabel, "Tap me: 0");
  lv_obj_center(g_ui.buttonLabel);

  // Colour bars (wrong colours or a wrong byte order show up here at once):
  // red, green, blue, white, black.
  const lv_color_t bars[5] = {lv_color_make(255, 0, 0), lv_color_make(0, 255, 0), lv_color_make(0, 0, 255),
                              lv_color_make(255, 255, 255), lv_color_make(0, 0, 0)};
  for (int i = 0; i < 5; ++i) makeBox(scr, 20 + i * 40, 238, 40, 14, bars[i]);

  // Two gradients (blue to red; black to white) to see banding and tearing.
  lv_obj_t* g1 = makeBox(scr, 20, 256, 200, 14, lv_color_make(0, 0, 255));
  lv_obj_set_style_bg_grad_color(g1, lv_color_make(255, 0, 0), 0);
  lv_obj_set_style_bg_grad_dir(g1, LV_GRAD_DIR_HOR, 0);
  lv_obj_t* g2 = makeBox(scr, 20, 274, 200, 14, lv_color_make(0, 0, 0));
  lv_obj_set_style_bg_grad_color(g2, lv_color_make(255, 255, 255), 0);
  lv_obj_set_style_bg_grad_dir(g2, LV_GRAD_DIR_HOR, 0);

  // One line only: a label that wraps would put its second line below row 319
  // (it was cut off in the first version), so clip instead and keep the text short.
  g_ui.statusLabel = makeLabel(scr, 30, 296, kDisplayWidth - 60, "starting...", LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(g_ui.statusLabel, LV_LABEL_LONG_CLIP);
  lv_obj_set_height(g_ui.statusLabel, 18);
  lv_obj_set_style_text_color(g_ui.statusLabel, lv_color_make(150, 200, 255), 0);

  // Crosshair on the top layer: 80 x 80, a ring plus two bars, never clickable.
  lv_obj_t* cross = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(cross);
  lv_obj_remove_flag(cross, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(cross, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(cross, 80, 80);
  lv_obj_add_flag(cross, LV_OBJ_FLAG_HIDDEN);
  makeBox(cross, 0, 38, 80, 4, lv_color_make(255, 60, 60));
  makeBox(cross, 38, 0, 4, 80, lv_color_make(255, 60, 60));
  lv_obj_t* ring = lv_obj_create(cross);
  lv_obj_remove_style_all(ring);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(ring, 40, 40);
  lv_obj_set_pos(ring, 20, 20);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 3, 0);
  lv_obj_set_style_border_color(ring, lv_color_make(255, 230, 0), 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
  g_ui.cross = cross;

  lv_timer_create(onTouchTimer, 20, nullptr);
#if defined(APP_BENCH) && APP_BENCH
  // Benchmark build only: redraw the whole screen 5 times a second so the
  // status line shows full-frame times and any SPI trouble shows on screen.
  lv_timer_create([](lv_timer_t*) { lv_obj_invalidate(lv_screen_active()); }, 200, nullptr);
#endif
}

// ---- status ----------------------------------------------------------------------

void printStatus(uint32_t elapsedMs) {
  LvglPortStats s;
  lvgl_port_take_stats(s);
  const uint32_t secs = elapsedMs ? elapsedMs : 1;
  const uint32_t frameAvgUs = s.frames ? s.flushUs / s.frames : 0;
  const uint32_t blockAvgUs = s.flushes ? s.flushUs / s.flushes : 0;
  const uint32_t handlerAvgUs = s.handlerCalls ? s.handlerUs / s.handlerCalls : 0;
  const uint32_t busyAvgUs = s.busyCalls ? s.busyUs / s.busyCalls : 0;
  Serial.printf(
      "[status] up %lu s | frames %lu (%lu.%lu/s) flush/frame avg %lu us max %lu us, %lu us/block (%lu blocks) | "
      "lv_timer_handler %lu calls avg %lu us, busy %lu calls avg %lu us, max %lu us | display_err %lu touch int %lu "
      "i2c_err %lu\n",
      (unsigned long)(millis() / 1000), (unsigned long)s.frames, (unsigned long)(s.frames * 1000 / secs),
      (unsigned long)((s.frames * 10000 / secs) % 10), (unsigned long)frameAvgUs, (unsigned long)s.frameFlushMaxUs,
      (unsigned long)blockAvgUs, (unsigned long)s.flushes, (unsigned long)s.handlerCalls,
      (unsigned long)handlerAvgUs, (unsigned long)s.busyCalls, (unsigned long)busyAvgUs, (unsigned long)s.handlerMaxUs,
      (unsigned long)s.displayErrors, (unsigned long)touch_interrupt_count(), (unsigned long)touch_i2c_error_count());

  lv_mem_monitor_t mon;
  if (lvgl_lock(100)) {
    lv_mem_monitor(&mon);
    char text[64];
    snprintf(text, sizeof(text), "flush %lu.%lu busy %lu.%lu ms", (unsigned long)(frameAvgUs / 1000),
             (unsigned long)(frameAvgUs % 1000 / 100), (unsigned long)(busyAvgUs / 1000),
             (unsigned long)(busyAvgUs % 1000 / 100));
    lv_label_set_text(g_ui.statusLabel, text);
    lvgl_unlock();
  } else {
    mon = {};
    Serial.println("[status] lvgl_lock timed out");
  }
  Serial.printf(
      "[mem] internal free %u (min %u, largest %u) psram free %u (min %u, largest %u) | lvgl used %u of %u B, "
      "free %u, frag %u%%\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)(mon.total_size - mon.free_size),
      (unsigned)mon.total_size, (unsigned)mon.free_size, (unsigned)mon.frag_pct);
}

}  // namespace

void setup() {
  powerHoldHigh();  // again, harmless: the latch is already 1

  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < kSerialWaitMs) delay(10);
  Serial.printf("soccer-display app %s built %s %s\n", kAppVersion, __DATE__, __TIME__);
  Serial.printf("power hold GPIO%d level %d\n", pins::kPowerHold, gpio_get_level((gpio_num_t)pins::kPowerHold));

  // The backlight stays off until the first frame has been drawn.
  static LedcBacklight backlight;
  s_backlight = &backlight;
  if (!backlight.ok()) Serial.println("[backlight] ERROR: backlight unavailable");

  lvgl_port_start();

  if (!lvgl_lock(1000)) {
    Serial.println("[main] FATAL: cannot take the LVGL lock to build the test screen");
    for (;;) delay(2000);
  }
  buildTestScreen();
  lvgl_unlock();

  delay(150);  // the LVGL task draws the first frame
  {
    // Whole-screen first frame: the worst case for the SPI link.
    LvglPortStats first;
    lvgl_port_take_stats(first);
    Serial.printf("[first frame] frames %lu, %lu blocks, %lu us flush total (max frame %lu us), display_err %lu\n",
                  (unsigned long)first.frames, (unsigned long)first.flushes, (unsigned long)first.flushUs,
                  (unsigned long)first.frameFlushMaxUs, (unsigned long)first.displayErrors);
  }
  s_backlight->set_percent(100);
  Serial.println("[main] test screen up, backlight 100%");
}

void loop() {
  static uint32_t last = millis();
  delay(kStatusMs);
  const uint32_t now = millis();
  printStatus(now - last);
  last = now;
}
