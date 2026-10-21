// The goal celebration's light rays as a bitmap.
//
// `count` wedges fan out from an apex; each is half a period wide, the gap
// after it the other half. Drawing 24 triangles per frame cost about 120 ms on
// the ESP32; filling a bitmap with straight spans costs a few ms and is then
// drawn as one image. Pure: no LVGL, no allocation.
#pragma once

#include <stdint.h>

// Widest bitmap the renderers accept (they keep one row of work on the stack).
constexpr int32_t kRayFanMaxWidth = 512;

// Fills the A8 bitmap `buf` (`w` x `h`, `stride` bytes per row): 0 outside the
// wedges, `opa` inside, a proportional value on the edges (anti-aliased by 2
// sample rows and exact horizontal coverage). Wedge k spans the angles
// [angle + k * P, angle + k * P + P / 2] with P = 2 pi / count, in radians, 0
// pointing right and positive angles turning clockwise on screen (y grows
// downwards). `cx`, `cy` is the apex in pixel units of the bitmap (pixel i
// covers [i, i + 1)). Does nothing for a null buffer, w > kRayFanMaxWidth, a
// stride below w or count outside 1..64.
void ray_fan_render(uint8_t* buf, int32_t w, int32_t h, uint32_t stride, float cx, float cy, float angle, int count,
                    uint8_t opa);

// The same fan as ready-made RGB565 pixels: pixel = lut[coverage], with the
// coverage ray_fan_render() would write for `opa` (0..opa), so `lut` needs
// opa + 1 entries; lut[0] is the colour outside the wedges. `stridePixels` is
// the row length in pixels. Used when the rays lie over one flat colour: it
// turns the per-frame blend of a mask over the background into a copy.
void ray_fan_render_rgb565(uint16_t* buf, int32_t w, int32_t h, uint32_t stridePixels, float cx, float cy,
                           float angle, int count, uint8_t opa, const uint16_t* lut);
