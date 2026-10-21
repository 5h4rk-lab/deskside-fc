// ImageDecoder shared by the simulator and the firmware: PNG through LVGL's
// decoder, JPEG through tjpgd called directly (LVGL 9.2's built-in JPEG decoder
// cannot read from memory without MEMFS). Needs LVGL initialised, and runs in
// the LVGL task (it calls LVGL). Uses lv_malloc for the pixels, so it builds
// without exceptions. See image_decoder.h for the contract.
#pragma once

#include <string>

#include "image_decoder.h"

class LvglImageDecoder : public ImageDecoder {
 public:
  ImagePtr decode(const std::string& bytes) override;
};
