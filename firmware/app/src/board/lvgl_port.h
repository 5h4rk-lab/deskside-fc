// LVGL glue: display (SPI DMA flush), pointer input, tick and the LVGL task.
//
// Threading: lvgl_port_start() runs lv_init() and creates the one task that
// calls lv_timer_handler(). Every LVGL and ui/ call happens in that task. Code
// that must touch LVGL from another task (rare; the setup code of the test
// screen is one) takes lvgl_lock() first.
#pragma once

#include <stdint.h>

// Initialises LVGL, the display and the touch controller, then starts the LVGL
// task (core 1, 16 KB stack). Prints `[display] ok [touch] <name>`.
// A failure of the display, a draw buffer or the task is fatal here: the reason
// is printed 5 times over about 10 s, then the board restarts (so a permanent
// fault reboots every ~10 s plus boot time, never faster). A missing touch
// controller is not fatal (printed; the screen just does not react).
void lvgl_port_start();

// Same start-up, but returns false instead of restarting, for code that can
// carry on without a screen (headless features). On false the port is
// half-initialised and unusable: do not call lvgl_* again, retry is not
// supported; lvgl_port_last_error() says why (static string, also printed).
bool lvgl_port_try_start();
const char* lvgl_port_last_error();

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
  uint32_t waitUs;           // time the renderer waited for the previous block (inside the handler)
  uint32_t waitMaxUs;        // longest single wait
  uint32_t chunkGapCount;    // gaps measured between consecutive blocks of one frame
  uint32_t chunkGapUs;       // their total (render + wait for the previous block)
  uint32_t chunkGapMaxUs;
  uint32_t preUs;            // handler start -> first block of a frame, summed
  uint32_t postUs;           // last block of a frame -> handler end, summed
  uint32_t preCount;
  uint32_t displayErrors;    // SPI transfers that failed to queue or finish, since boot
};
void lvgl_port_take_stats(LvglPortStats& out);

// Smallest free stack the LVGL task ever had, in bytes (0 before it runs).
uint32_t lvgl_port_stack_free_bytes();

// ---- diagnostics (serial test hooks) ----------------------------------------
// A scripted finger: from (x0,y0) to (x1,y1) over durMs, then released. The
// position follows the clock, so a slow frame makes the finger jump rather than
// stretch the gesture, as a real finger would. While a script runs the real
// touch controller is ignored. Safe from any task.
void lvgl_port_inject_drag(int x0, int y0, int x1, int y1, uint32_t durMs);

// Records lv_timer_handler() calls longer than 1 ms from the moment a script
// starts (up to 256 calls) so a gesture's frame times can be printed afterwards.
// Prints one summary line and one line of "t+<ms>:<call ms>" entries.
void lvgl_port_trace_dump(const char* label);

// Starts the same recording without a scripted finger (up to 256 calls).
void lvgl_port_trace_start();
