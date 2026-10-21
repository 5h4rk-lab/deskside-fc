#include "lvgl_image_decoder.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "src/draw/lv_image_decoder_private.h"
#include "src/libs/tjpgd/tjpgd.h"

namespace {

// All pixel memory comes from lv_malloc()/lv_free(), not from operator new:
// the code is built without exceptions on the firmware, where a failed `new`
// would abort, while lv_malloc() returns nullptr (and on the device places big
// buffers in PSRAM, see firmware/app/src/board/lvgl_alloc.cpp). Every
// allocation here is checked and a failure gives a nullptr image.
struct PixelBuffer {
  uint8_t* data = nullptr;
  size_t size = 0;

  PixelBuffer() = default;
  explicit PixelBuffer(size_t bytes) : data(static_cast<uint8_t*>(lv_malloc(bytes))), size(data ? bytes : 0) {}
  PixelBuffer(const PixelBuffer&) = delete;
  PixelBuffer& operator=(const PixelBuffer&) = delete;
  PixelBuffer(PixelBuffer&& o) noexcept : data(o.data), size(o.size) {
    o.data = nullptr;
    o.size = 0;
  }
  PixelBuffer& operator=(PixelBuffer&& o) noexcept {
    if (this != &o) {
      if (data) lv_free(data);
      data = o.data;
      size = o.size;
      o.data = nullptr;
      o.size = 0;
    }
    return *this;
  }
  ~PixelBuffer() {
    if (data) lv_free(data);
  }
};

// Image descriptor plus its pixel memory, freed together when the last
// ImagePtr goes away.
struct OwnedImage {
  lv_image_dsc_t dsc{};
  PixelBuffer pixels;
};

ImagePtr own(int32_t w, int32_t h, uint32_t stride, PixelBuffer pixels) {
  auto* owned = new (std::nothrow) OwnedImage();
  if (!owned) return nullptr;
  owned->pixels = std::move(pixels);
  owned->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  owned->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
  owned->dsc.header.w = w;
  owned->dsc.header.h = h;
  owned->dsc.header.stride = stride;
  owned->dsc.data_size = static_cast<uint32_t>(owned->pixels.size);
  owned->dsc.data = owned->pixels.data;
  // The shared_ptr control block (a few bytes) is the one allocation left that
  // cannot report failure; the deleter frees the image and its pixels.
  return ImagePtr(&owned->dsc, [owned](const lv_image_dsc_t*) { delete owned; });
}

bool within_cap(uint32_t w, uint32_t h) {
  return w > 0 && h > 0 && w <= static_cast<uint32_t>(kImageMaxDimension) &&
         h <= static_cast<uint32_t>(kImageMaxDimension);
}

uint32_t be32(const std::string& s, size_t at) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[at])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3]));
}

ImagePtr decode_png(const std::string& file) {
  // The IHDR chunk comes first: 8 bytes signature, 4 length, 4 "IHDR", then
  // width and height. Refuse a bad size before LVGL allocates for it.
  if (file.size() < 24 || std::memcmp(file.data() + 12, "IHDR", 4) != 0) return nullptr;
  if (!within_cap(be32(file, 16), be32(file, 20))) return nullptr;

  lv_image_dsc_t src{};
  src.header.magic = LV_IMAGE_HEADER_MAGIC;
  src.header.cf = LV_COLOR_FORMAT_RAW;
  src.data = reinterpret_cast<const uint8_t*>(file.data());
  src.data_size = static_cast<uint32_t>(file.size());

  lv_image_decoder_args_t args{};
  args.no_cache = true;  // the provider does the caching
  lv_image_decoder_dsc_t dsc;
  if (lv_image_decoder_open(&dsc, &src, &args) != LV_RESULT_OK) return nullptr;

  ImagePtr result;
  const lv_draw_buf_t* decoded = dsc.decoded;
  if (decoded && decoded->data && decoded->header.cf == LV_COLOR_FORMAT_ARGB8888 &&
      within_cap(static_cast<uint32_t>(decoded->header.w), static_cast<uint32_t>(decoded->header.h))) {
    const uint32_t stride = decoded->header.stride ? decoded->header.stride : decoded->header.w * 4;
    const size_t bytes = static_cast<size_t>(stride) * decoded->header.h;
    PixelBuffer pixels(bytes);
    if (pixels.data) {
      std::memcpy(pixels.data, decoded->data, bytes);
      result = own(decoded->header.w, decoded->header.h, stride, std::move(pixels));
    }
  }
  lv_image_decoder_close(&dsc);
  return result;
}

struct JpegContext {
  const uint8_t* data;
  size_t size;
  size_t pos;
  PixelBuffer pixels;  // ARGB8888 (B, G, R, A in memory)
  uint32_t width;
};

size_t jpeg_in(JDEC* jd, uint8_t* buf, size_t n) {
  auto* in = static_cast<JpegContext*>(jd->device);
  const size_t avail = in->size - in->pos;
  if (n > avail) n = avail;
  if (buf) std::memcpy(buf, in->data + in->pos, n);
  in->pos += n;
  return n;
}

int jpeg_out(JDEC* jd, void* bitmap, JRECT* rect) {
  auto* out = static_cast<JpegContext*>(jd->device);
  // LVGL's tjpgd emits 3 bytes per pixel in LV_COLOR_FORMAT_RGB888 order (B, G, R).
  const uint8_t* bgr = static_cast<const uint8_t*>(bitmap);
  for (uint32_t y = rect->top; y <= rect->bottom; ++y) {
    for (uint32_t x = rect->left; x <= rect->right; ++x) {
      uint8_t* p = out->pixels.data + (static_cast<size_t>(y) * out->width + x) * 4;
      p[0] = bgr[0];
      p[1] = bgr[1];
      p[2] = bgr[2];
      p[3] = 0xFF;
      bgr += 3;
    }
  }
  return 1;
}

ImagePtr decode_jpeg(const std::string& file) {
  // tjpgd's work area (tables and MCU buffer) is 4 KB: heap, not the stack of
  // whichever task decodes.
  constexpr size_t kWorkBytes = 4096;
  PixelBuffer work(kWorkBytes);
  if (!work.data) return nullptr;
  JpegContext ctx{reinterpret_cast<const uint8_t*>(file.data()), file.size(), 0, PixelBuffer(), 0};
  JDEC jd;
  if (jd_prepare(&jd, jpeg_in, work.data, work.size, &ctx) != JDR_OK) return nullptr;
  if (!within_cap(jd.width, jd.height)) return nullptr;
  ctx.width = jd.width;
  ctx.pixels = PixelBuffer(static_cast<size_t>(jd.width) * jd.height * 4);
  if (!ctx.pixels.data) return nullptr;
  std::memset(ctx.pixels.data, 0, ctx.pixels.size);
  if (jd_decomp(&jd, jpeg_out, 0) != JDR_OK) return nullptr;
  return own(jd.width, jd.height, jd.width * 4, std::move(ctx.pixels));
}

}  // namespace

ImagePtr LvglImageDecoder::decode(const std::string& bytes) {
  static const char kPngMagic[] = "\x89PNG\r\n\x1a\n";
  if (bytes.size() > 8 && std::memcmp(bytes.data(), kPngMagic, 8) == 0) return decode_png(bytes);
  if (bytes.size() > 3 && static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xD8 &&
      static_cast<uint8_t>(bytes[2]) == 0xFF) {
    return decode_jpeg(bytes);
  }
  return nullptr;
}
