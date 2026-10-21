#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "http_image_provider.h"
#include "image_decoder.h"
#include "lvgl_image_decoder.h"
#include "test_display.h"
#include "test_images.h"

namespace {

std::string bytes_of(const uint8_t* data, size_t n) {
  return std::string(reinterpret_cast<const char*>(data), n);
}

// Valid 512x1 and 513x1 RGBA PNGs (solid colour): the cap is 512 per side.
constexpr uint8_t kPng512x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xa8, 0xbb, 0x16,
    0x76, 0x00, 0x00, 0x00, 0x1b, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10, 0x78, 0xdf, 0xf0,
    0x7f, 0x14, 0x8f, 0xe2, 0x51, 0x3c, 0x8a, 0x47, 0xf1, 0x28, 0x1e, 0xc5, 0x23, 0x0b, 0x03, 0x00,
    0x53, 0x20, 0xfc, 0x3d, 0x68, 0xd8, 0x10, 0x77, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82};
constexpr uint8_t kPng513x1[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x47, 0x79, 0x7d,
    0x48, 0x00, 0x00, 0x00, 0x1b, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10, 0x78, 0xdf, 0xf0,
    0x7f, 0x14, 0x8f, 0xe2, 0x51, 0x3c, 0x8a, 0x47, 0xf1, 0x28, 0x1e, 0xc5, 0x23, 0x0f, 0x03, 0x00,
    0x49, 0x5c, 0xfe, 0xbb, 0xc8, 0x35, 0x56, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82};

// Rewrites the SOF0 frame size of a JPEG (height then width, big endian).
std::string with_jpeg_size(const uint8_t* data, size_t n, uint16_t w, uint16_t h) {
  std::string s = bytes_of(data, n);
  for (size_t i = 0; i + 8 < s.size(); ++i) {
    if (static_cast<uint8_t>(s[i]) == 0xFF && static_cast<uint8_t>(s[i + 1]) == 0xC0) {
      s[i + 5] = static_cast<char>(h >> 8);
      s[i + 6] = static_cast<char>(h & 0xFF);
      s[i + 7] = static_cast<char>(w >> 8);
      s[i + 8] = static_cast<char>(w & 0xFF);
      return s;
    }
  }
  FAIL("no SOF0 marker in the test JPEG");
  return s;
}

uint32_t crc32_of(const std::string& s) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char c : s) {
    crc ^= c;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void put_be32(std::string& s, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) s.push_back(static_cast<char>((v >> shift) & 0xFF));
}

void put_chunk(std::string& out, const char* type, const std::string& data) {
  put_be32(out, static_cast<uint32_t>(data.size()));
  std::string body = std::string(type, 4) + data;
  out += body;
  put_be32(out, crc32_of(body));
}

// A valid w x h RGBA PNG of one flat colour, written with stored (uncompressed)
// deflate blocks so the test needs no encoder. The file is large (about w*h*4
// bytes); decoding it gives a w*h*4 byte ARGB8888 image.
std::string flat_png(uint32_t w, uint32_t h, uint8_t shade) {
  std::string raw;
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back('\0');  // filter: none
    for (uint32_t x = 0; x < w; ++x) {
      raw.push_back(static_cast<char>(shade));
      raw.push_back(static_cast<char>(x & 0xFF));
      raw.push_back(static_cast<char>(y & 0xFF));
      raw.push_back(static_cast<char>(0xFF));
    }
  }
  std::string z = "\x78\x01";
  for (size_t pos = 0; pos < raw.size();) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z.push_back(last ? '\1' : '\0');
    z.push_back(static_cast<char>(n & 0xFF));
    z.push_back(static_cast<char>(n >> 8));
    z.push_back(static_cast<char>(~n & 0xFF));
    z.push_back(static_cast<char>((~n >> 8) & 0xFF));
    z.append(raw, pos, n);
    pos += n;
  }
  uint32_t a = 1, b = 0;
  for (unsigned char c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put_be32(z, (b << 16) | a);
  std::string ihdr;
  put_be32(ihdr, w);
  put_be32(ihdr, h);
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);
  std::string png = "\x89PNG\r\n\x1a\n";
  put_chunk(png, "IHDR", ihdr);
  put_chunk(png, "IDAT", z);
  put_chunk(png, "IEND", "");
  return png;
}

}  // namespace

TEST_CASE("LvglImageDecoder keeps decoding with a full image cache and a screen alive") {
  // HttpImageProvider::kDefaultCacheBytes (3 MB) of decoded images stay alive
  // next to the widgets and the decoder's own temporary buffers. Decoded
  // pixels must not run out of memory before that (an earlier version took
  // them from LVGL's 4 MB pool): hold 5 MiB of 512 x 512 images, then decode
  // more and build a screen.
  init_test_display();
  {
    LvglImageDecoder decoder;
    std::vector<ImagePtr> held;
    for (int i = 0; i < 5; ++i) {
      ImagePtr img = decoder.decode(flat_png(512, 512, static_cast<uint8_t>(i * 40)));
      REQUIRE(img);
      CHECK(img->header.w == 512);
      CHECK(img->header.h == 512);
      CHECK(img->data_size == 512u * 512u * 4u);
      held.push_back(img);
    }
    CHECK(held.size() * 512 * 512 * 4 > 3u * HttpImageProvider::kDefaultCacheBytes / 2);
    // Everything held is still intact (pixel 0 is R = shade, see flat_png; ARGB8888 is B,G,R,A in memory).
    for (size_t i = 0; i < held.size(); ++i) CHECK(held[i]->data[2] == static_cast<uint8_t>(i * 40));

    // A screen on top: widgets allocate from the same LVGL heap.
    lv_obj_t* screen = lv_screen_active();
    for (int i = 0; i < 40; ++i) {
      lv_obj_t* label = lv_label_create(screen);
      lv_label_set_text_fmt(label, "label %d with some text to lay out", i);
    }
    lv_obj_update_layout(screen);

    // And more decodes still work.
    CHECK(decoder.decode(bytes_of(test_images::kPng4x4, sizeof(test_images::kPng4x4))));
    CHECK(decoder.decode(bytes_of(test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8))));
    CHECK(decoder.decode(flat_png(512, 512, 7)));
    held.clear();
  }
  deinit_test_display();
}

TEST_CASE("LvglImageDecoder decodes a PNG and owns the pixels") {
  init_test_display();
  {
    LvglImageDecoder decoder;
    ImageDecoder& d = decoder;  // through the interface
    std::string bytes = bytes_of(test_images::kPng4x4, sizeof(test_images::kPng4x4));
    ImagePtr img = d.decode(bytes);
    REQUIRE(img);
    CHECK(img->header.w == 4);
    CHECK(img->header.h == 4);
    CHECK(img->header.cf == LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(img->data != nullptr);
    CHECK(img->data_size >= 64u);
    // Pixel 1 is (r=16, g=239, b=128, a=255); ARGB8888 is B,G,R,A in memory.
    CHECK(img->data[4 + 0] == 128);
    CHECK(img->data[4 + 1] == 239);
    CHECK(img->data[4 + 2] == 16);
    CHECK(img->data[4 + 3] == 255);
    CHECK(img->data[3] < 255);  // alpha kept

    // The pixels do not point into the input: free it and read again.
    bytes.assign(bytes.size(), '\0');
    bytes.clear();
    bytes.shrink_to_fit();
    CHECK(img->data[4 + 1] == 239);
  }
  deinit_test_display();
}

TEST_CASE("LvglImageDecoder decodes a JPEG") {
  init_test_display();
  {
    LvglImageDecoder decoder;
    ImagePtr img = decoder.decode(bytes_of(test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8)));
    REQUIRE(img);
    CHECK(img->header.w == 16);
    CHECK(img->header.h == 8);
    CHECK(img->header.cf == LV_COLOR_FORMAT_ARGB8888);
    CHECK(img->data[2] > 170);  // flat red, lossy
    CHECK(img->data[3] == 255);
  }
  deinit_test_display();
}

TEST_CASE("LvglImageDecoder returns nullptr for anything undecodable") {
  init_test_display();
  {
    LvglImageDecoder decoder;
    CHECK(decoder.decode("") == nullptr);
    CHECK(decoder.decode("x") == nullptr);
    CHECK(decoder.decode("this is not an image at all") == nullptr);
    CHECK(decoder.decode("<html>502 Bad Gateway</html>") == nullptr);
    CHECK(decoder.decode(std::string("\x89PNG", 4)) == nullptr);       // magic only
    CHECK(decoder.decode(std::string("\xFF\xD8\xFF", 3)) == nullptr);  // magic only
    CHECK(decoder.decode(std::string("GIF89a\x01\x00\x01\x00", 10)) == nullptr);  // unsupported type
    // Truncated.
    CHECK(decoder.decode(bytes_of(test_images::kPng4x4, 20)) == nullptr);
    CHECK(decoder.decode(bytes_of(test_images::kPng4x4, sizeof(test_images::kPng4x4) - 30)) == nullptr);
    CHECK(decoder.decode(bytes_of(test_images::kJpeg16x8, 40)) == nullptr);
    CHECK(decoder.decode(bytes_of(test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8) / 2)) == nullptr);
    // Binary noise after a valid magic.
    std::string noise = "\x89PNG\r\n\x1a\n";
    for (int i = 0; i < 200; ++i) noise.push_back(static_cast<char>(i * 37));
    CHECK(decoder.decode(noise) == nullptr);
    std::string jnoise = "\xFF\xD8\xFF";
    for (int i = 0; i < 200; ++i) jnoise.push_back(static_cast<char>(i * 91));
    CHECK(decoder.decode(jnoise) == nullptr);
  }
  deinit_test_display();
}

TEST_CASE("LvglImageDecoder rejects zero and oversized dimensions") {
  init_test_display();
  {
    LvglImageDecoder decoder;
    CHECK(kImageMaxDimension == 512);
    ImagePtr edge = decoder.decode(bytes_of(kPng512x1, sizeof(kPng512x1)));
    REQUIRE(edge);
    CHECK(edge->header.w == 512);
    CHECK(edge->header.h == 1);
    CHECK(decoder.decode(bytes_of(kPng513x1, sizeof(kPng513x1))) == nullptr);

    // A PNG header that claims a huge size must be refused before any
    // allocation (the body is not even valid).
    std::string huge = bytes_of(test_images::kPng4x4, sizeof(test_images::kPng4x4));
    huge[16] = '\x7F';  // width 0x7F000004
    CHECK(decoder.decode(huge) == nullptr);
    std::string zero = bytes_of(test_images::kPng4x4, sizeof(test_images::kPng4x4));
    zero[19] = '\0';  // width 0
    CHECK(decoder.decode(zero) == nullptr);

    const std::string ok = with_jpeg_size(test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8), 16, 8);
    CHECK(decoder.decode(ok) != nullptr);
    CHECK(decoder.decode(with_jpeg_size(test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8), 513, 8)) ==
          nullptr);
    CHECK(decoder.decode(with_jpeg_size(test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8), 16, 4000)) ==
          nullptr);
    CHECK(decoder.decode(with_jpeg_size(test_images::kJpeg16x8, sizeof(test_images::kJpeg16x8), 0, 8)) ==
          nullptr);
  }
  deinit_test_display();
}

TEST_CASE("LvglImageDecoder images are independent and outlive the decoder") {
  init_test_display();
  {
    ImagePtr a;
    ImagePtr b;
    {
      LvglImageDecoder decoder;
      const std::string bytes = bytes_of(test_images::kPng4x4, sizeof(test_images::kPng4x4));
      a = decoder.decode(bytes);
      b = decoder.decode(bytes);
    }
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a.get() != b.get());
    CHECK(a->data != b->data);
    CHECK(a->data[4 + 1] == 239);
    CHECK(b->data[4 + 1] == 239);
  }
  deinit_test_display();
}
