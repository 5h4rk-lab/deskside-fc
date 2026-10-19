// Headless LVGL display for tests: a dummy lv_display with a draw buffer and a
// flush callback that discards the pixels. No SDL window is ever created.
#pragma once

#include <lvgl.h>

namespace test_display {

constexpr int32_t kWidth = 240;
constexpr int32_t kHeight = 320;

inline void flush_cb(lv_display_t* disp, const lv_area_t*, uint8_t*) {
  lv_display_flush_ready(disp);
}

inline uint32_t& fake_ms() {
  static uint32_t ms = 0;
  return ms;
}

inline uint32_t tick_cb() { return fake_ms(); }

}  // namespace test_display

// Initialises LVGL (once per call; pair with deinit_test_display()) and returns
// a 240x320 display that is also the default display.
inline lv_display_t* init_test_display() {
  using namespace test_display;
  static uint8_t buf[kWidth * 40 * 2];  // partial buffer, RGB565
  lv_init();
  fake_ms() = 0;
  lv_tick_set_cb(tick_cb);
  lv_display_t* disp = lv_display_create(kWidth, kHeight);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, buf, nullptr, sizeof(buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  return disp;
}

inline void deinit_test_display() { lv_deinit(); }

// Advances fake time and lets LVGL run timers, layout and rendering.
inline void test_advance(uint32_t ms) {
  test_display::fake_ms() += ms;
  lv_timer_handler();
}
