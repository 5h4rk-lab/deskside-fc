// Off-screen rendering helpers for tests and screenshots (simulator only).
#pragma once

#include <lvgl.h>

// Renders `obj` (and its children) into an RGB565 buffer, as the panel would
// show it, and writes it as an RGB PNG. Returns false when the snapshot or the
// file write fails.
bool snapshot_png(lv_obj_t* obj, const char* path);

// Colour of the pixel at (x, y) relative to `obj`'s top-left corner, rendered
// the same way as snapshot_png. Out-of-range coordinates or a failed snapshot
// give black.
lv_color_t pixel_at(lv_obj_t* obj, int x, int y);
