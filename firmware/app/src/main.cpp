// Soccer display firmware. For now: boots, brings up the display, backlight
// and touch under LVGL and shows a driver test screen (build id, touch
// coordinates and crosshair, tap-counting button, colour bars, the two
// decoded pictures).
// Also decodes an embedded PNG and JPEG through the shared LvglImageDecoder
// (ui/src), shows them 1:1 and logs decode times and where the pixels live.
// The UI, Wi-Fi and the rest are added by the next tasks.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <stdio.h>
#include <string>

#include "board/backlight_ledc.h"
#include "board/display_st7789.h"
#include "board/lvgl_port.h"
#include "board/pins.h"
#include "board/test_assets.h"
#include "board/touch.h"
#include "lvgl_image_decoder.h"
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
  lv_obj_t* buttonLabel = nullptr;
  lv_obj_t* statusLabel = nullptr;
  lv_obj_t* cross = nullptr;
  uint32_t taps = 0;
  uint32_t badTouch = 0;  // touch points outside 0..239 x 0..319 (should stay 0)
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

// Every visible block of the test screen is registered here so that
// checkLayout() can verify, on the device, that nothing overlaps and nothing
// leaves the 240 x 320 panel (there is no way to see the screen from serial).
struct Tracked {
  const char* name;
  lv_obj_t* obj;
};
Tracked g_tracked[32];
size_t g_trackedCount = 0;
void track(const char* name, lv_obj_t* obj) {
  if (g_trackedCount < sizeof(g_tracked) / sizeof(g_tracked[0])) g_tracked[g_trackedCount++] = {name, obj};
}

void checkLayout(lv_obj_t* scr) {
  lv_obj_update_layout(scr);
  int problems = 0;
  for (size_t i = 0; i < g_trackedCount; ++i) {
    lv_area_t a;
    lv_obj_get_coords(g_tracked[i].obj, &a);
    if (a.x1 < 0 || a.y1 < 0 || a.x2 >= kDisplayWidth || a.y2 >= kDisplayHeight) {
      Serial.printf("[layout] WARNING: '%s' leaves the screen: x %d..%d y %d..%d\n", g_tracked[i].name, (int)a.x1,
                    (int)a.x2, (int)a.y1, (int)a.y2);
      ++problems;
    }
    for (size_t j = i + 1; j < g_trackedCount; ++j) {
      lv_area_t b;
      lv_obj_get_coords(g_tracked[j].obj, &b);
      if (a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2) {
        Serial.printf("[layout] WARNING: '%s' (x %d..%d y %d..%d) overlaps '%s' (x %d..%d y %d..%d)\n",
                      g_tracked[i].name, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2, g_tracked[j].name, (int)b.x1,
                      (int)b.x2, (int)b.y1, (int)b.y2);
        ++problems;
      }
    }
  }
  Serial.printf("[layout] %u blocks checked, %d problems\n", (unsigned)g_trackedCount, problems);
}

// Logs a layout bug on serial when `text` (the widest string a label will show)
// is wider than the label: it would be clipped or wrapped onto the next row.
void checkFits(const char* name, const char* text, const lv_font_t* font, int w) {
  lv_point_t size;
  lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (size.x > w) Serial.printf("[layout] WARNING: label '%s' needs %d px but has %d\n", name, (int)size.x, w);
}

// One-line label: fixed 18 px height, text clipped, never wraps, so it can
// never grow into the row below. `widest` is the longest text it will show.
lv_obj_t* makeLabel(lv_obj_t* parent, const char* name, int x, int y, int w, const char* widest,
                    lv_text_align_t align, int lines = 1) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_size(l, w, 18 * lines);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(l, align, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_label_set_text(l, widest);
  track(name, l);
  const lv_font_t* font = lv_obj_get_style_text_font(l, LV_PART_MAIN);
  // Check every line of the text separately.
  char line[96];
  const char* t = widest;
  while (*t) {
    size_t n = 0;
    while (t[n] && t[n] != '\n' && n < sizeof(line) - 1) ++n;
    memcpy(line, t, n);
    line[n] = 0;
    checkFits(name, line, font, w);
    t += n;
    if (*t == '\n') ++t;
  }
  return l;
}

// A white-bordered square at each corner and a ring in the centre: the
// targets for the four-corners-and-centre touch check.
void makeTarget(lv_obj_t* parent, int cx, int cy, int size) {
  lv_obj_t* t = lv_obj_create(parent);
  track("corner target", t);
  lv_obj_remove_style_all(t);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(t, size, size);
  lv_obj_set_pos(t, cx - size / 2, cy - size / 2);
  lv_obj_set_style_border_width(t, 2, 0);
  lv_obj_set_style_border_color(t, lv_color_white(), 0);
  lv_obj_set_style_border_opa(t, LV_OPA_COVER, 0);
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
  if (down && moved && (p.x < 0 || p.x >= kDisplayWidth || p.y < 0 || p.y >= kDisplayHeight)) {
    ++g_ui.badTouch;
    if (g_ui.badTouch <= 20) {
      Serial.printf("[touch] OUT OF RANGE x=%d y=%d (count %lu)\n", (int)p.x, (int)p.y, (unsigned long)g_ui.badTouch);
    }
  }
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

// Where the picture's pixels live: "PSRAM", "internal" or "?" (not RAM).
const char* placement(const void* p) {
  if (esp_ptr_external_ram(p)) return "PSRAM";
  if (esp_ptr_internal(p)) return "internal";
  return "?";
}

// ---- decoded test pictures -----------------------------------------------------

struct DecodedPicture {
  ImagePtr image;
  uint32_t firstUs = 0;  // first decode (cold caches), including the descriptor
  uint32_t avgUs = 0;    // average of the repeat decodes
  int32_t psramDelta = 0, internalDelta = 0;  // free-bytes change caused by the first decode
  size_t pixelBytes = 0;
  const char* where = "?";
};
DecodedPicture g_png, g_jpeg;
constexpr int kRepeats = 5;

struct HeapSnap {
  size_t intFree, psFree, intLargest, psLargest;
};
HeapSnap snapHeap() {
  return {heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)};
}

// Decodes `bytes` once for display, then kRepeats more times for an average
// (the repeats are freed again). Logs heap before and after. LVGL lock held.
void decodeAndLog(const char* name, const unsigned char* bytes, size_t n, DecodedPicture& out) {
  static LvglImageDecoder decoder;
  const std::string file(reinterpret_cast<const char*>(bytes), n);

  const HeapSnap before = snapHeap();
  const int64_t t0 = esp_timer_get_time();
  out.image = decoder.decode(file);
  out.firstUs = (uint32_t)(esp_timer_get_time() - t0);
  const HeapSnap after = snapHeap();

  if (!out.image) {
    Serial.printf("[img] %s: DECODE FAILED (%u bytes in)\n", name, (unsigned)n);
    return;
  }
  out.psramDelta = (int)after.psFree - (int)before.psFree;
  out.internalDelta = (int)after.intFree - (int)before.intFree;
  out.pixelBytes = out.image->data_size;
  out.where = placement(out.image->data);
  Serial.printf("[img] %s: %u bytes -> %ldx%ld cf %u stride %u, %u B of pixels at %p in %s, first decode %lu us\n", name,
                (unsigned)n, (long)out.image->header.w, (long)out.image->header.h, (unsigned)out.image->header.cf,
                (unsigned)out.image->header.stride, (unsigned)out.pixelBytes, (const void*)out.image->data, out.where,
                (unsigned long)out.firstUs);
  Serial.printf(
      "[img] %s heap delta: internal free %d (largest %u -> %u), psram free %d (largest %u -> %u)\n", name,
      (int)after.intFree - (int)before.intFree, (unsigned)before.intLargest, (unsigned)after.intLargest,
      (int)after.psFree - (int)before.psFree, (unsigned)before.psLargest, (unsigned)after.psLargest);

  uint64_t total = 0;
  int ok = 0;
  for (int i = 0; i < kRepeats; ++i) {
    const int64_t r0 = esp_timer_get_time();
    ImagePtr again = decoder.decode(file);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - r0);
    if (again) {
      total += us;
      ++ok;
    }
  }
  out.avgUs = ok ? (uint32_t)(total / ok) : 0;
  Serial.printf("[img] %s: %d repeat decodes avg %lu us\n", name, ok, (unsigned long)out.avgUs);
  const HeapSnap end = snapHeap();
  Serial.printf("[img] %s heap after repeats: internal free %u psram free %u (delta vs after first decode: %d / %d)\n",
                name, (unsigned)end.intFree, (unsigned)end.psFree, (int)end.intFree - (int)after.intFree,
                (int)end.psFree - (int)after.psFree);
}

void showPicture(lv_obj_t* parent, const DecodedPicture& pic, int x, int y) {
  // Grey outline 1 px outside the 96 x 96 picture so its bounds are visible.
  track("picture", makeBox(parent, x - 2, y - 2, test_assets::kSize + 4, test_assets::kSize + 4, lv_color_make(90, 90, 90)));
  if (!pic.image) {
    lv_obj_t* l = makeLabel(parent, "missing", x, y + 38, test_assets::kSize, "no picture", LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_color(l, lv_color_make(255, 90, 90), 0);
    return;
  }
  lv_obj_t* im = lv_image_create(parent);
  lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
  lv_image_set_src(im, pic.image.get());
  lv_obj_set_pos(im, x, y);
}

void buildTestScreen() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Layout, 240 x 320 (every row is its own band, nothing overlaps):
  //   y   2..38   build id (2 lines)        y  40..58   touch line
  //   y  62..160  PNG and JPEG, 96 x 96 each at 1:1, side by side
  //   y 164..182  captions                  y 184..220  decode times, placement
  //   y 224..260  button                    y 266..278  colour bars
  //   y 296..314  status line               corner squares 28 x 28 at the edges
  // Corner targets (drawn first, everything else sits above).
  makeTarget(scr, 14, 14, 28);
  makeTarget(scr, kDisplayWidth - 14, 14, 28);
  makeTarget(scr, 14, kDisplayHeight - 14, 28);
  makeTarget(scr, kDisplayWidth - 14, kDisplayHeight - 14, 28);

  char build[96];
  snprintf(build, sizeof(build), "%s %s\nbuilt %s %s", kAppName, kAppVersion, __DATE__, __TIME__);
  makeLabel(scr, "build", 30, 2, kDisplayWidth - 60, build, LV_TEXT_ALIGN_CENTER, 2);
  g_ui.touchLabel = makeLabel(scr, "touch", 10, 40, kDisplayWidth - 20, "touch DOWN x=-999 y=-999", LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(g_ui.touchLabel, "touch: none yet");

  // The pictures: 1:1 so every pixel is visible.
  showPicture(scr, g_png, 16, 64);
  showPicture(scr, g_jpeg, 128, 64);
  makeLabel(scr, "cap png", 14, 164, 100, "PNG", LV_TEXT_ALIGN_CENTER);
  makeLabel(scr, "cap jpeg", 126, 164, 100, "JPEG", LV_TEXT_ALIGN_CENTER);

  char line[64];
  lv_obj_t* l;
  snprintf(line, sizeof(line), "PNG %lu.%lu ms  JPEG %lu.%lu ms", (unsigned long)(g_png.avgUs / 1000),
           (unsigned long)(g_png.avgUs % 1000 / 100), (unsigned long)(g_jpeg.avgUs / 1000),
           (unsigned long)(g_jpeg.avgUs % 1000 / 100));
  l = makeLabel(scr, "times", 10, 184, kDisplayWidth - 20, "PNG 000.0 ms  JPEG 000.0 ms", LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(l, line);
  snprintf(line, sizeof(line), "pixels in: %s / %s", g_png.where, g_jpeg.where);
  l = makeLabel(scr, "where", 10, 202, kDisplayWidth - 20, "pixels in: internal / internal", LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(l, line);
  const bool bothOk = g_png.image && g_jpeg.image;
  lv_obj_set_style_text_color(l, bothOk ? lv_color_make(120, 255, 120) : lv_color_make(255, 90, 90), 0);

  lv_obj_t* btn = lv_button_create(scr);
  lv_obj_set_pos(btn, 40, 224);
  lv_obj_set_size(btn, kDisplayWidth - 80, 36);
  track("button", btn);
  lv_obj_add_event_cb(btn, onButton, LV_EVENT_CLICKED, nullptr);
  g_ui.buttonLabel = lv_label_create(btn);
  lv_label_set_text(g_ui.buttonLabel, "Tap me: 0");
  lv_obj_center(g_ui.buttonLabel);

  // Colour bars: red, green, blue, white, black.
  const lv_color_t bars[5] = {lv_color_make(255, 0, 0), lv_color_make(0, 255, 0), lv_color_make(0, 0, 255),
                              lv_color_make(255, 255, 255), lv_color_make(0, 0, 0)};
  for (int i = 0; i < 5; ++i) track("colour bar", makeBox(scr, 20 + i * 40, 266, 40, 12, bars[i]));

  // Status line: one row, clipped (see makeLabel).
  g_ui.statusLabel = makeLabel(scr, "status", 30, 296, kDisplayWidth - 60, "flush 00.0 busy 00.0 ms", LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(g_ui.statusLabel, "starting...");
  lv_obj_set_style_text_color(g_ui.statusLabel, lv_color_make(150, 200, 255), 0);
  checkLayout(scr);

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
      "i2c_err %lu recoveries %lu%s\n",
      (unsigned long)(millis() / 1000), (unsigned long)s.frames, (unsigned long)(s.frames * 1000 / secs),
      (unsigned long)((s.frames * 10000 / secs) % 10), (unsigned long)frameAvgUs, (unsigned long)s.frameFlushMaxUs,
      (unsigned long)blockAvgUs, (unsigned long)s.flushes, (unsigned long)s.handlerCalls,
      (unsigned long)handlerAvgUs, (unsigned long)s.busyCalls, (unsigned long)busyAvgUs, (unsigned long)s.handlerMaxUs,
      (unsigned long)s.displayErrors, (unsigned long)touch_interrupt_count(), (unsigned long)touch_i2c_error_count(), (unsigned long)touch_recovery_count(),
      touch_in_backoff() ? " BACKOFF" : "");

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
  // The decode lines at boot scroll away (the monitor attaches late), so the
  // result is repeated here with every report.
  Serial.printf(
      "[img] png first %lu us avg %lu us, %u B in %s (psram %+ld, internal %+ld B); jpeg first %lu us avg %lu us, %u B in %s "
      "(psram %+ld, internal %+ld B)\n",
      (unsigned long)g_png.firstUs, (unsigned long)g_png.avgUs, (unsigned)g_png.pixelBytes, g_png.where,
      (long)g_png.psramDelta, (long)g_png.internalDelta, (unsigned long)g_jpeg.firstUs, (unsigned long)g_jpeg.avgUs,
      (unsigned)g_jpeg.pixelBytes, g_jpeg.where, (long)g_jpeg.psramDelta, (long)g_jpeg.internalDelta);
  Serial.printf(
      "[mem] internal free %u (min %u, largest %u) psram free %u (min %u, largest %u) | lvgl used %u of %u B, "
      "free %u, frag %u%% | pictures png %s jpeg %s | touch_out_of_range %lu\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)(mon.total_size - mon.free_size),
      (unsigned)mon.total_size, (unsigned)mon.free_size, (unsigned)mon.frag_pct, g_png.where, g_jpeg.where,
      (unsigned long)g_ui.badTouch);
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
  {
    // Decode the embedded pictures first (the decoder calls LVGL, so it runs
    // under the lock like everything else), then build the screen around them.
    const HeapSnap before = snapHeap();
    Serial.printf("[img] before: internal free %u (largest %u) psram free %u (largest %u)\n",
                  (unsigned)before.intFree, (unsigned)before.intLargest, (unsigned)before.psFree,
                  (unsigned)before.psLargest);
    decodeAndLog("png", test_assets::kPng, sizeof(test_assets::kPng), g_png);
    decodeAndLog("jpeg", test_assets::kJpeg, sizeof(test_assets::kJpeg), g_jpeg);
    const HeapSnap after = snapHeap();
    Serial.printf("[img] after both: internal free %u (largest %u) psram free %u (largest %u); total delta internal %d psram %d\n",
                  (unsigned)after.intFree, (unsigned)after.intLargest, (unsigned)after.psFree,
                  (unsigned)after.psLargest, (int)after.intFree - (int)before.intFree,
                  (int)after.psFree - (int)before.psFree);
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
