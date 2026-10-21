// LVGL's heap in the simulator: plain malloc with a byte counter and an
// allocation-failure switch, so tests can prove that big buffers (the
// celebration's ray pictures and stamp bitmap, the Live background bitmap) are
// freed on every path and that the app degrades, not crashes, when one cannot
// be had. The firmware has its own hooks (firmware/app/src/board/lvgl_alloc.cpp).
#pragma once

#include <stddef.h>

namespace sim_alloc {

// Bytes and blocks LVGL holds right now (requested sizes, not malloc's rounding).
size_t live_bytes();
size_t live_blocks();

// Makes lv_malloc() return nullptr for requests of min_size..max_size bytes
// (inclusive) after letting `skip` such requests through. Stays on until
// clear_failures().
void fail_range(size_t min_size, size_t max_size, int skip = 0);
void clear_failures();

// Requests refused since the last fail_range().
int failures();

}  // namespace sim_alloc
