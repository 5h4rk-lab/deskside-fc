// ImageDecoder for the simulator: PNG through LVGL's decoder, JPEG through
// tjpgd called directly (LVGL 9.2's built-in JPEG decoder cannot read from
// memory without MEMFS). Needs LVGL initialised. See image_decoder.h for the
// contract.
#pragma once

#include <string>

#include "image_decoder.h"

class LvglImageDecoder : public ImageDecoder {
 public:
  ImagePtr decode(const std::string& bytes) override;
};
