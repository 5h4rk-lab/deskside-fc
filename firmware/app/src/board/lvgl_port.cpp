// LVGL port for the Waveshare ESP32-S3-Touch-LCD-2.8.
//
// Display: two partial buffers of 240 x 40 RGB565 pixels in internal,
// DMA-capable RAM (allocated directly with heap_caps_malloc, not lv_malloc).
// LVGL renders one buffer while the other one is sent over SPI by DMA; the
// SPI completion interrupt calls lv_display_flush_ready(), which is a single
// store of a volatile flag and is safe in interrupt context.
#include "lvgl_port.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

#include "display_st7789.h"
#include "touch.h"

namespace {

constexpr int kBufRows = 40;
constexpr size_t kBufBytes = (size_t)kDisplayWidth * kBufRows * sizeof(uint16_t);
constexpr uint32_t kLvglTaskStackBytes = 12 * 1024;
constexpr UBaseType_t kLvglTaskPriority = 2;
constexpr BaseType_t kLvglTaskCore = 1;
constexpr uint32_t kMaxWaitMs = 5;
constexpr uint32_t kIndevPeriodMs = 20;
constexpr uint32_t kBusyCallUs = 1000;

bool s_started = false;
lv_display_t* s_disp = nullptr;
SemaphoreHandle_t s_lock = nullptr;

// Statistics. Written by the LVGL task and by the SPI interrupt, read by
// lvgl_port_take_stats(); one spinlock guards them.
portMUX_TYPE s_statsMux = portMUX_INITIALIZER_UNLOCKED;
LvglPortStats s_stats = {};
uint32_t s_frameFlushUs = 0;
volatile int64_t s_flushStartUs = 0;
volatile bool s_flushIsLast = false;

constexpr int kFatalPrints = 5;
constexpr uint32_t kFatalPrintGapMs = 2000;

const char* s_lastError = nullptr;

// Records the reason, prints it and returns false (for `return fail(...)`).
bool fail(const char* what) {
  s_lastError = what;
  Serial.printf("[port] ERROR: %s\n", what);
  return false;
}

// Bounded fatal behaviour: print the reason 5 times over about 10 s so a
// serial monitor attached late still sees it, then restart. A permanent fault
// therefore reboots no faster than about every 10 s plus the boot time (the
// backlight stays off meanwhile; GPIO7 stays held so the board does not power
// off and the USB port stays reachable for a re-flash).
[[noreturn]] void fatalRestart(const char* what) {
  for (int i = 0; i < kFatalPrints; ++i) {
    Serial.printf("[port] FATAL (%d/%d): %s; restarting\n", i + 1, kFatalPrints, what);
    delay(kFatalPrintGapMs);
  }
  esp_restart();
  for (;;) delay(1000);  // not reached
}

uint32_t lvglTickMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

// Runs in the SPI interrupt when the pixel block has been sent.
void onFlushDone(void*) {
  const uint32_t us = (uint32_t)(esp_timer_get_time() - s_flushStartUs);
  portENTER_CRITICAL_ISR(&s_statsMux);
  s_stats.flushes++;
  s_stats.flushUs += us;
  s_frameFlushUs += us;
  if (s_flushIsLast) {
    s_stats.frames++;
    if (s_frameFlushUs > s_stats.frameFlushMaxUs) s_stats.frameFlushMaxUs = s_frameFlushUs;
    s_frameFlushUs = 0;
  }
  portEXIT_CRITICAL_ISR(&s_statsMux);
  lv_display_flush_ready(s_disp);
}

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  s_flushStartUs = esp_timer_get_time();
  s_flushIsLast = lv_display_flush_is_last(disp);
  // Pixels are little-endian RGB565 as rendered: no byte swapping is needed.
  if (!display_flush_async(area->x1, area->y1, area->x2, area->y2, (const uint16_t*)px_map)) {
    // Could not queue (already printed): drop the block but keep LVGL running.
    lv_display_flush_ready(disp);
  }
}

void touchReadCb(lv_indev_t*, lv_indev_data_t* data) {
  TouchPoint p;
  const bool pressed = touch_poll(p);
  data->point.x = p.x;
  data->point.y = p.y;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void lvglTask(void*) {
  for (;;) {
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    const int64_t t0 = esp_timer_get_time();
    const uint32_t nextMs = lv_timer_handler();
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    xSemaphoreGiveRecursive(s_lock);

    portENTER_CRITICAL(&s_statsMux);
    s_stats.handlerCalls++;
    s_stats.handlerUs += us;
    if (us > kBusyCallUs) {
      s_stats.busyCalls++;
      s_stats.busyUs += us;
    }
    if (us > s_stats.handlerMaxUs) s_stats.handlerMaxUs = us;
    portEXIT_CRITICAL(&s_statsMux);

    uint32_t waitMs = nextMs < kMaxWaitMs ? nextMs : kMaxWaitMs;
    vTaskDelay(waitMs ? pdMS_TO_TICKS(waitMs) : 1);
  }
}

}  // namespace

bool lvgl_port_try_start() {
  if (s_started) return s_lastError == nullptr;
  s_started = true;

  s_lock = xSemaphoreCreateRecursiveMutex();
  if (!s_lock) return fail("cannot create the LVGL mutex");

  lv_init();
  lv_tick_set_cb(lvglTickMs);

  // Draw buffers: internal DMA-capable RAM, null-checked.
  void* buf1 = heap_caps_malloc(kBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void* buf2 = heap_caps_malloc(kBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buf1 || !buf2) return fail("not enough internal DMA RAM for the two 240 x 40 draw buffers");

  s_disp = lv_display_create(kDisplayWidth, kDisplayHeight);
  if (!s_disp) return fail("lv_display_create failed (out of memory)");
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(s_disp, buf1, buf2, (uint32_t)kBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(s_disp, flushCb);

  if (!display_begin(onFlushDone, nullptr)) return fail("display init failed (see [display] lines above)");

  const TouchController controller = touch_begin();
  Serial.printf("[display] ok [touch] %s\n", touch_controller_name(controller));
  Serial.printf("[port] SPI %lu Hz, 2 x %u B draw buffers (internal DMA), indev %lu ms, LVGL task core %d stack %lu B\n",
                (unsigned long)kDisplaySpiHz, (unsigned)kBufBytes, (unsigned long)kIndevPeriodMs,
                (int)kLvglTaskCore, (unsigned long)kLvglTaskStackBytes);
  if (controller == TouchController::None) {
    Serial.println("[touch] WARNING: no touch controller found (nothing at 0x1A with a CST328 signature, nothing at 0x58)");
  }

  lv_indev_t* indev = lv_indev_create();
  if (!indev) return fail("lv_indev_create failed (out of memory)");
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchReadCb);
  lv_timer_t* readTimer = lv_indev_get_read_timer(indev);
  if (readTimer) lv_timer_set_period(readTimer, kIndevPeriodMs);

  if (xTaskCreatePinnedToCore(lvglTask, "lvgl", kLvglTaskStackBytes, nullptr, kLvglTaskPriority, nullptr,
                              kLvglTaskCore) != pdPASS) {
    return fail("cannot create the LVGL task");
  }
  return true;
}

const char* lvgl_port_last_error() { return s_lastError; }

void lvgl_port_start() {
  if (!lvgl_port_try_start()) fatalRestart(s_lastError ? s_lastError : "unknown error");
}

bool lvgl_lock(uint32_t timeoutMs) {
  if (!s_lock) return false;
  return xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void lvgl_unlock() {
  if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

void lvgl_port_take_stats(LvglPortStats& out) {
  portENTER_CRITICAL(&s_statsMux);
  out = s_stats;
  s_stats = {};
  portEXIT_CRITICAL(&s_statsMux);
  out.displayErrors = display_error_count();
}
