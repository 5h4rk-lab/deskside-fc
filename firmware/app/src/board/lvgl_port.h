// LVGL glue: display (SPI DMA flush), pointer input, tick and the LVGL task.
//
// Threading: lvgl_port_start() runs lv_init() and creates the one task that
// calls lv_timer_handler(). Every LVGL and ui/ call happens in that task. Code
// that must touch LVGL from another task (rare; the setup code of the test
// screen is one) takes lvgl_lock() first.
#pragma once

#include <stdint.h>

// Initialises LVGL, the display and the touch controller, then starts the LVGL
// task (core 1, 12 KB stack). A failure of the display, a draw buffer or the
// task is fatal: it is printed on the serial console every 2 s and this
// function never returns. A missing touch controller is not fatal (printed,
// the screen just does not react). Prints `[display] ok [touch] <name>`.
void lvgl_port_start();

// Takes the LVGL lock (recursive). Returns false on timeout. Always pair a
// successful call with lvgl_unlock(). Never call from an interrupt.
bool lvgl_lock(uint32_t timeoutMs);
void lvgl_unlock();

// Counters since the previous call (the call resets them). Safe from any task.
struct LvglPortStats {
  uint32_t frames;           // completed screen refreshes (last flush finished)
  uint32_t flushes;          // display blocks sent
  uint32_t flushUs;          // sum of block times, queued -> sent (SPI DMA)
  uint32_t frameFlushMaxUs;  // longest sum of block times within one frame
  uint32_t handlerCalls;     // lv_timer_handler() calls
  uint32_t handlerUs;        // total time inside lv_timer_handler()
  uint32_t busyCalls;        // calls that took more than 1 ms (rendered something)
  uint32_t busyUs;           // total time of those calls
  uint32_t handlerMaxUs;     // longest single call
  uint32_t displayErrors;    // SPI transfers that failed to queue or finish, since boot
};
void lvgl_port_take_stats(LvglPortStats& out);
