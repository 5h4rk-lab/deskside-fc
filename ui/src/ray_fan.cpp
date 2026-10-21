#include "ray_fan.h"

#include <math.h>
#include <string.h>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kInf = 1e9f;
constexpr int kMaxWedges = 64;

struct Wedge {
  float t0;    // start angle, [0, 2 pi)
  float t1;    // end angle, t0 + half a period (may pass 2 pi)
  float cot0;  // cos / sin of the two edges: x = cx + dy * cot
  float cot1;
};

// The row writers. run(): pixels [x0, x1) are inside; px(): one edge pixel
// with a coverage index 0..opa (later writes win, which only matters within a
// pixel or two of the apex, where wedges touch).
struct A8Row {
  uint8_t* row;
  uint8_t opa;
  void clear(int32_t w) { memset(row, 0, static_cast<size_t>(w)); }
  void run(int32_t x0, int32_t x1) { memset(row + x0, opa, static_cast<size_t>(x1 - x0)); }
  void px(int32_t x, uint32_t idx) { row[x] = static_cast<uint8_t>(idx); }
};

struct Rgb565Row {
  uint16_t* row;
  const uint16_t* lut;
  uint8_t opa;
  void clear(int32_t w) {
    const uint16_t c = lut[0];
    int32_t x = 0;
    if ((reinterpret_cast<uintptr_t>(row) & 3u) != 0 && w > 0) row[x++] = c;
    const uint32_t c2 = (static_cast<uint32_t>(c) << 16) | c;
    uint32_t* p = reinterpret_cast<uint32_t*>(row + x);
    for (int32_t n = (w - x) / 2; n > 0; --n) *p++ = c2;
    if (((w - x) & 1) != 0) row[w - 1] = c;
  }
  void run(int32_t x0, int32_t x1) {
    const uint16_t c = lut[opa];
    if (x1 - x0 < 4) {
      for (int32_t x = x0; x < x1; ++x) row[x] = c;
      return;
    }
    int32_t x = x0;
    if ((reinterpret_cast<uintptr_t>(row + x) & 3u) != 0) row[x++] = c;
    const uint32_t c2 = (static_cast<uint32_t>(c) << 16) | c;
    uint32_t* p = reinterpret_cast<uint32_t*>(row + x);
    for (int32_t n = (x1 - x) / 2; n > 0; --n) *p++ = c2;
    if (((x1 - x) & 1) != 0) row[x1 - 1] = c;
  }
  void px(int32_t x, uint32_t idx) { row[x] = lut[idx]; }
};

inline int32_t floor_i(float v) { return static_cast<int32_t>(v); }  // v >= 0
inline int32_t ceil_i(float v) {
  const int32_t i = static_cast<int32_t>(v);
  return static_cast<float>(i) < v ? i + 1 : i;
}

inline float coverage(float xl, float xr, int32_t i) {
  const float a = xl > static_cast<float>(i) ? xl : static_cast<float>(i);
  const float b = xr < static_cast<float>(i + 1) ? xr : static_cast<float>(i + 1);
  return b > a ? b - a : 0.0f;
}

// One wedge piece on one pixel row, sampled on two lines: [xl0, xr0] and
// [xl1, xr1] (empty when xr <= xl).
template <typename Row>
void paint_piece(Row& pr, int32_t w, float xl0, float xr0, float xl1, float xr1, uint8_t opa) {
  const float fw = static_cast<float>(w);
  if (xl0 < 0.0f) xl0 = 0.0f;
  if (xl1 < 0.0f) xl1 = 0.0f;
  if (xr0 > fw) xr0 = fw;
  if (xr1 > fw) xr1 = fw;
  const bool e0 = xr0 <= xl0;
  const bool e1 = xr1 <= xl1;
  if (e0 && e1) return;
  float lo;
  float hi;
  int32_t f0 = 0;  // fully covered pixels [f0, f1) on both lines
  int32_t f1 = 0;
  if (e0) {
    lo = xl1;
    hi = xr1;
  } else if (e1) {
    lo = xl0;
    hi = xr0;
  } else {
    lo = xl0 < xl1 ? xl0 : xl1;
    hi = xr0 > xr1 ? xr0 : xr1;
    f0 = ceil_i(xl0 > xl1 ? xl0 : xl1);
    f1 = floor_i(xr0 < xr1 ? xr0 : xr1);
  }
  int32_t i = floor_i(lo);
  const int32_t end = ceil_i(hi);  // exclusive
  const float scale = static_cast<float>(opa) * 0.5f;
  while (i < end) {
    if (i >= f0 && i < f1) {
      pr.run(f0, f1);
      i = f1;
      continue;
    }
    float c = 0.0f;
    if (!e0) c += coverage(xl0, xr0, i);
    if (!e1) c += coverage(xl1, xr1, i);
    pr.px(i, static_cast<uint32_t>(c * scale + 0.5f));
    ++i;
  }
}

template <typename Row>
void render(int32_t w, int32_t h, float cx, float cy, float angle, int count, uint8_t opa, Row (*rowAt)(void*, int32_t),
            void* ctx) {
  const float period = kTwoPi / static_cast<float>(count);
  Wedge wedges[kMaxWedges];
  for (int k = 0; k < count; ++k) {
    float t0 = fmodf(angle + static_cast<float>(k) * period, kTwoPi);
    if (t0 < 0.0f) t0 += kTwoPi;
    const float t1 = t0 + period * 0.5f;
    wedges[k] = {t0, t1, cosf(t0) / sinf(t0), cosf(t1) / sinf(t1)};
  }

  for (int32_t y = 0; y < h; ++y) {
    Row pr = rowAt(ctx, y);
    pr.clear(w);
    float dy0 = static_cast<float>(y) + 0.25f - cy;
    float dy1 = static_cast<float>(y) + 0.75f - cy;
    if (fabsf(dy0) < 1e-3f) dy0 = dy0 < 0.0f ? -1e-3f : 1e-3f;
    if (fabsf(dy1) < 1e-3f) dy1 = dy1 < 0.0f ? -1e-3f : 1e-3f;
    // A row whose two sample lines lie on different sides of the apex is
    // handled line by line (at most one row per fan).
    const bool split = (dy0 > 0.0f) != (dy1 > 0.0f);
    for (int k = 0; k < count; ++k) {
      const Wedge& e = wedges[k];
      if (!split) {
        const float dy = dy0;  // same sign for both lines
        if (dy > 0.0f) {
          // Angles in (0, pi): x falls as the angle grows.
          if (e.t0 < kPi) {
            const float r0 = e.t0 <= 1e-6f ? kInf : cx + dy0 * e.cot0;
            const float r1 = e.t0 <= 1e-6f ? kInf : cx + dy1 * e.cot0;
            const float l0 = e.t1 >= kPi ? -kInf : cx + dy0 * e.cot1;
            const float l1 = e.t1 >= kPi ? -kInf : cx + dy1 * e.cot1;
            paint_piece(pr, w, l0, r0, l1, r1, opa);
          }
          if (e.t1 > kTwoPi) {  // the part past 2 pi starts again at angle 0
            paint_piece(pr, w, cx + dy0 * e.cot1, kInf, cx + dy1 * e.cot1, kInf, opa);
          }
        } else if (e.t1 > kPi) {
          // Angles in (pi, 2 pi): x grows with the angle.
          const float l0 = e.t0 <= kPi ? -kInf : cx + dy0 * e.cot0;
          const float l1 = e.t0 <= kPi ? -kInf : cx + dy1 * e.cot0;
          const float r0 = e.t1 >= kTwoPi ? kInf : cx + dy0 * e.cot1;
          const float r1 = e.t1 >= kTwoPi ? kInf : cx + dy1 * e.cot1;
          paint_piece(pr, w, l0, r0, l1, r1, opa);
        }
      } else {
        for (int s = 0; s < 2; ++s) {
          const float dy = s == 0 ? dy0 : dy1;
          float xl = 0.0f;
          float xr = 0.0f;  // empty
          float xl2 = 0.0f;
          float xr2 = 0.0f;  // the wrapped piece, if any
          if (dy > 0.0f) {
            if (e.t0 < kPi) {
              xr = e.t0 <= 1e-6f ? kInf : cx + dy * e.cot0;
              xl = e.t1 >= kPi ? -kInf : cx + dy * e.cot1;
            }
            if (e.t1 > kTwoPi) {
              xl2 = cx + dy * e.cot1;
              xr2 = kInf;
            }
          } else if (e.t1 > kPi) {
            xl = e.t0 <= kPi ? -kInf : cx + dy * e.cot0;
            xr = e.t1 >= kTwoPi ? kInf : cx + dy * e.cot1;
          }
          // One line only: the other is empty.
          paint_piece(pr, w, xl, xr, 0.0f, 0.0f, opa);
          if (xr2 > xl2) paint_piece(pr, w, xl2, xr2, 0.0f, 0.0f, opa);
        }
      }
    }
  }
}

struct A8Ctx {
  uint8_t* buf;
  uint32_t stride;
  uint8_t opa;
};
A8Row a8_row(void* c, int32_t y) {
  A8Ctx* x = static_cast<A8Ctx*>(c);
  return A8Row{x->buf + static_cast<size_t>(y) * x->stride, x->opa};
}

struct Rgb565Ctx {
  uint16_t* buf;
  uint32_t stride;
  const uint16_t* lut;
  uint8_t opa;
};
Rgb565Row rgb565_row(void* c, int32_t y) {
  Rgb565Ctx* x = static_cast<Rgb565Ctx*>(c);
  return Rgb565Row{x->buf + static_cast<size_t>(y) * x->stride, x->lut, x->opa};
}

bool valid(const void* buf, int32_t w, int32_t h, uint32_t stride, int count) {
  return buf != nullptr && w > 0 && h > 0 && w <= kRayFanMaxWidth && stride >= static_cast<uint32_t>(w) && count >= 1 &&
         count <= kMaxWedges;
}

}  // namespace

void ray_fan_render(uint8_t* buf, int32_t w, int32_t h, uint32_t stride, float cx, float cy, float angle, int count,
                    uint8_t opa) {
  if (!valid(buf, w, h, stride, count)) return;
  A8Ctx ctx{buf, stride, opa};
  render<A8Row>(w, h, cx, cy, angle, count, opa, a8_row, &ctx);
}

void ray_fan_render_rgb565(uint16_t* buf, int32_t w, int32_t h, uint32_t stridePixels, float cx, float cy,
                           float angle, int count, uint8_t opa, const uint16_t* lut) {
  if (!valid(buf, w, h, stridePixels, count) || lut == nullptr) return;
  Rgb565Ctx ctx{buf, stridePixels, lut, opa};
  render<Rgb565Row>(w, h, cx, cy, angle, count, opa, rgb565_row, &ctx);
}
