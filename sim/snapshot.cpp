#include "snapshot.h"

#include <cstdint>
#include <cstdio>
#include <vector>

// LVGL builds lodepng (LV_USE_LODEPNG) with its encoder; reuse it rather than
// writing our own. Declared here because lodepng.h's C++ section clashes with
// LVGL's extern "C" wrapper.
// The _file variant goes through lv_fs, which has no driver here, so encode to
// memory and write with stdio (simulator only).
extern "C" unsigned lodepng_encode24(unsigned char** out, size_t* outsize,
                                     const unsigned char* image, unsigned w, unsigned h);

namespace {

lv_color_t rgb565_at(const lv_draw_buf_t* buf, int x, int y) {
  const uint8_t* row = static_cast<const uint8_t*>(buf->data) + y * buf->header.stride;
  const uint16_t p = static_cast<uint16_t>(row[x * 2] | (row[x * 2 + 1] << 8));
  const uint8_t r5 = (p >> 11) & 0x1F;
  const uint8_t g6 = (p >> 5) & 0x3F;
  const uint8_t b5 = p & 0x1F;
  // Expand to 8 bits the same way a panel would show full intensity.
  return lv_color_make(static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
                       static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
                       static_cast<uint8_t>((b5 << 3) | (b5 >> 2)));
}

lv_draw_buf_t* take(lv_obj_t* obj) {
  lv_obj_update_layout(obj);
  return lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB565);
}

}  // namespace

bool snapshot_png(lv_obj_t* obj, const char* path) {
  if (obj == nullptr || path == nullptr) return false;
  lv_draw_buf_t* buf = take(obj);
  if (buf == nullptr) return false;
  const int w = buf->header.w;
  const int h = buf->header.h;
  std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const lv_color_t c = rgb565_at(buf, x, y);
      unsigned char* px = &rgb[(static_cast<size_t>(y) * w + x) * 3];
      px[0] = c.red;
      px[1] = c.green;
      px[2] = c.blue;
    }
  }
  lv_draw_buf_destroy(buf);
  unsigned char* png = nullptr;
  size_t size = 0;
  if (lodepng_encode24(&png, &size, rgb.data(), static_cast<unsigned>(w),
                       static_cast<unsigned>(h)) != 0) {
    lv_free(png);
    return false;
  }
  FILE* f = std::fopen(path, "wb");
  bool ok = f != nullptr && std::fwrite(png, 1, size, f) == size;
  if (f != nullptr && std::fclose(f) != 0) ok = false;
  lv_free(png);  // lodepng allocates through lv_malloc in LVGL builds
  return ok;
}

lv_color_t pixel_at(lv_obj_t* obj, int x, int y) {
  lv_color_t out = lv_color_black();
  if (obj == nullptr) return out;
  lv_draw_buf_t* buf = take(obj);
  if (buf == nullptr) return out;
  if (x >= 0 && y >= 0 && x < static_cast<int>(buf->header.w) &&
      y < static_cast<int>(buf->header.h)) {
    out = rgb565_at(buf, x, y);
  }
  lv_draw_buf_destroy(buf);
  return out;
}
