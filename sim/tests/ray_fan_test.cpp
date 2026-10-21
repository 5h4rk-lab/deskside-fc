#include <doctest/doctest.h>
#include <math.h>
#include <stdint.h>

#include <vector>

#include "ray_fan.h"

namespace {

constexpr float kPi = 3.14159265358979f;

// Angular distance in radians from the pixel centre's direction to the nearest
// edge of a wedge, and whether the centre lies in a wedge.
struct Probe {
  bool inside;
  float edgeDistance;  // in pixels, perpendicular to the nearest edge
};

Probe probe(float px, float py, float cx, float cy, float angle, int count) {
  const float dx = px - cx;
  const float dy = py - cy;
  const float r = sqrtf(dx * dx + dy * dy);
  const float period = 2.0f * kPi / static_cast<float>(count);
  float theta = atan2f(dy, dx) - angle;
  theta = fmodf(theta, period);
  if (theta < 0.0f) theta += period;
  const float half = period / 2.0f;
  Probe p;
  p.inside = theta < half;
  const float d = fminf(fminf(theta, fabsf(theta - half)), period - theta);
  p.edgeDistance = r * sinf(d);
  return p;
}

struct Case {
  float cx, cy, angle;
  int count;
};

}  // namespace

TEST_CASE("ray fan: matches a per-pixel angle test away from the edges") {
  const int w = 240;
  const int h = 320;
  const uint8_t opa = 31;
  const Case cases[] = {
      {120.0f, 228.8f, 0.0f, 24},   {120.0f, 228.8f, 0.5f, 24},  {120.0f, 228.8f, 2.9f, 24},
      {120.0f, 228.8f, 6.0f, 24},   {120.0f, 228.8f, -1.3f, 24}, {120.0f, 228.8f, 13.0f, 24},
      {120.0f, 100.0f, 0.3f, 24},   {-30.0f, -40.0f, 0.7f, 12}, {300.0f, 400.0f, 1.1f, 7},
      {120.5f, 160.0f, 0.0f, 24},   {120.0f, 160.25f, 0.01f, 24},
  };
  for (const Case& c : cases) {
    CAPTURE(c.cx);
    CAPTURE(c.cy);
    CAPTURE(c.angle);
    CAPTURE(c.count);
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h, 0xAA);
    ray_fan_render(buf.data(), w, h, w, c.cx, c.cy, c.angle, c.count, opa);
    int checked = 0;
    int wrong = 0;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const Probe p = probe(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, c.cx, c.cy, c.angle, c.count);
        if (p.edgeDistance < 1.6f) continue;  // an edge pixel: any coverage
        ++checked;
        const uint8_t expected = p.inside ? opa : 0;
        if (buf[static_cast<size_t>(y) * w + x] != expected) ++wrong;
      }
    }
    CHECK(checked > w * h / 2);
    CHECK(wrong == 0);
  }
}

TEST_CASE("ray fan: edge pixels carry partial coverage, never more than opa") {
  const int w = 240;
  const int h = 320;
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h, 0);
  ray_fan_render(buf.data(), w, h, w, 120.0f, 228.8f, 0.4f, 24, 31);
  int partial = 0;
  int maxV = 0;
  for (uint8_t v : buf) {
    if (v > 0 && v < 31) ++partial;
    if (v > maxV) maxV = v;
  }
  CHECK(maxV == 31);
  CHECK(partial > 500);
}

TEST_CASE("ray fan: wedges cover half of the area around the apex") {
  const int w = 240;
  const int h = 320;
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h, 0);
  ray_fan_render(buf.data(), w, h, w, 120.0f, 160.0f, 0.0f, 24, 200);
  double sum = 0;
  for (uint8_t v : buf) sum += v;
  const double share = sum / (200.0 * w * h);
  CHECK(share > 0.47);
  CHECK(share < 0.53);
}

TEST_CASE("ray fan: a stride wider than the bitmap leaves the padding alone") {
  const int w = 100;
  const int h = 60;
  const uint32_t stride = 128;
  std::vector<uint8_t> buf(static_cast<size_t>(stride) * h, 0x55);
  ray_fan_render(buf.data(), w, h, stride, 50.0f, 40.0f, 0.2f, 24, 31);
  for (int y = 0; y < h; ++y) {
    for (uint32_t x = w; x < stride; ++x) CHECK(buf[static_cast<size_t>(y) * stride + x] == 0x55);
  }
}

TEST_CASE("ray fan: bad arguments change nothing") {
  std::vector<uint8_t> buf(100 * 100, 0x77);
  ray_fan_render(nullptr, 100, 100, 100, 50, 50, 0, 24, 31);
  ray_fan_render(buf.data(), 100, 100, 99, 50, 50, 0, 24, 31);       // stride below width
  ray_fan_render(buf.data(), kRayFanMaxWidth + 1, 1, kRayFanMaxWidth + 1, 0, 0, 0, 24, 31);
  ray_fan_render(buf.data(), 100, 100, 100, 50, 50, 0, 0, 31);       // no wedges
  ray_fan_render(buf.data(), 100, 100, 100, 50, 50, 0, 65, 31);      // too many
  ray_fan_render(buf.data(), 0, 100, 100, 50, 50, 0, 24, 31);
  for (uint8_t v : buf) CHECK(v == 0x77);
}

TEST_CASE("ray fan: the RGB565 renderer is the A8 one seen through the palette") {
  const int w = 240;
  const int h = 320;
  const uint8_t opa = 31;
  uint16_t lut[opa + 1];
  for (int i = 0; i <= opa; ++i) lut[i] = static_cast<uint16_t>(0x1000 + i * 37);
  const Case cases[] = {{120.0f, 228.8f, 0.0f, 24}, {120.0f, 228.8f, 2.9f, 24}, {120.0f, 100.0f, 0.3f, 24},
                        {-30.0f, -40.0f, 0.7f, 12}, {300.0f, 400.0f, 1.1f, 7},  {120.0f, 160.25f, 0.01f, 24}};
  for (int offset = 0; offset < 2; ++offset) {  // an odd start address exercises the unaligned path
    for (const Case& c : cases) {
      std::vector<uint8_t> a8(static_cast<size_t>(w) * h, 0);
      ray_fan_render(a8.data(), w, h, w, c.cx, c.cy, c.angle, c.count, opa);
      std::vector<uint16_t> raw(static_cast<size_t>(w) * h + 4, 0xFFFF);
      uint16_t* px = raw.data() + offset;
      ray_fan_render_rgb565(px, w, h, w, c.cx, c.cy, c.angle, c.count, opa, lut);
      int wrong = 0;
      for (size_t i = 0; i < a8.size(); ++i) {
        if (px[i] != lut[a8[i]]) ++wrong;
      }
      CHECK(wrong == 0);
      // Nothing written outside the bitmap.
      CHECK(raw[a8.size() + offset] == 0xFFFF);
      if (offset == 1) CHECK(raw[0] == 0xFFFF);
    }
  }
}

TEST_CASE("ray fan: RGB565 bad arguments change nothing") {
  std::vector<uint16_t> buf(100 * 100, 0x7777);
  uint16_t lut[32] = {};
  ray_fan_render_rgb565(nullptr, 100, 100, 100, 50, 50, 0, 24, 31, lut);
  ray_fan_render_rgb565(buf.data(), 100, 100, 100, 50, 50, 0, 24, 31, nullptr);
  ray_fan_render_rgb565(buf.data(), 100, 100, 99, 50, 50, 0, 24, 31, lut);
  for (uint16_t v : buf) CHECK(v == 0x7777);
}
