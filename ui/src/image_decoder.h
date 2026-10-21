// Turns the bytes of a downloaded image into something LVGL can draw. The
// simulator implements it with LVGL's PNG decoder and tjpgd
// (ui/src/lvgl_image_decoder.h); the firmware with its own decoders. Shared with
// the firmware: no stdio, no filesystem.
//
// Contract of decode():
// - Sniffs the format from the magic bytes: PNG ("\x89PNG") or JPEG
//   ("\xFF\xD8\xFF"). Nothing else is accepted.
// - Returns an ImagePtr that owns its pixel memory, so the caller can free
//   `bytes` right away. The image outlives the decoder.
// - Returns nullptr for anything it cannot decode: empty, truncated or corrupt
//   data, an unsupported format, zero width or height, or a side over
//   kImageMaxDimension (512). The size is checked before the pixels are
//   allocated.
// - Never throws.
#pragma once

#include <string>

#include "image_provider.h"

// The largest width or height an ImageDecoder accepts.
constexpr int kImageMaxDimension = 512;

class ImageDecoder {
 public:
  virtual ~ImageDecoder() = default;
  virtual ImagePtr decode(const std::string& bytes) = 0;
};
