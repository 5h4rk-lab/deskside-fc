// ImageDecoder wrapper that times every decode (LVGL task, where the shared
// LvglImageDecoder must run) for the [img] status line. Firmware glue: the
// decoding itself is ui/src/lvgl_image_decoder.
#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <stdint.h>

#include <string>

#include "app_diag.h"
#include "image_decoder.h"

#if APP_DIAG
extern volatile bool g_lvglDiagFailLarge;  // board/lvgl_alloc.cpp
#endif

class TimedImageDecoder : public ImageDecoder {
 public:
  struct Stats {
    uint32_t ok = 0;
    uint32_t failed = 0;
    uint32_t lastUs = 0;
    uint32_t maxUs = 0;
    uint64_t totalUs = 0;
    uint16_t lastW = 0;
    uint16_t lastH = 0;
    size_t lastInBytes = 0;
    bool lastPng = false;
  };

  explicit TimedImageDecoder(ImageDecoder& inner) : inner_(inner) {}

  ImagePtr decode(const std::string& bytes) override {
    const int64_t t0 = esp_timer_get_time();
#if APP_DIAG
    // :diag-img-fail: this decode runs with every LVGL allocation of 1 KB or
    // more failing (as with PSRAM full): the decoder must answer nullptr.
    const bool inject = diagFailLeft_ > 0;
    if (inject) {
      --diagFailLeft_;
      g_lvglDiagFailLarge = true;
    }
    ImagePtr image = inner_.decode(bytes);
    g_lvglDiagFailLarge = false;
    if (inject) Serial.printf("[diag] img-fail: decode under failing allocations -> %s\n", image ? "an image (!)" : "nullptr (placeholder)");
#else
    ImagePtr image = inner_.decode(bytes);
#endif
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    stats_.lastUs = us;
    if (us > stats_.maxUs) stats_.maxUs = us;
    stats_.totalUs += us;
    stats_.lastInBytes = bytes.size();
    stats_.lastPng = bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 0x89 && bytes[1] == 'P';
    if (image) {
      ++stats_.ok;
      stats_.lastW = static_cast<uint16_t>(image->header.w);
      stats_.lastH = static_cast<uint16_t>(image->header.h);
    } else {
      ++stats_.failed;
      stats_.lastW = stats_.lastH = 0;
    }
#if APP_DIAG
    // One line per picture in the diagnostics builds (sizes only, no URL).
    Serial.printf("[img] decoded %s %ux%u from %u B in %lu.%lu ms%s\n", stats_.lastPng ? "PNG" : "JPEG",
                  (unsigned)stats_.lastW, (unsigned)stats_.lastH, (unsigned)bytes.size(), (unsigned long)(us / 1000),
                  (unsigned long)(us % 1000 / 100), image ? "" : " (FAILED)");
#endif
    return image;
  }

  const Stats& stats() const { return stats_; }
#if APP_DIAG
  // The next `n` decodes run with failing allocations (LVGL task).
  void diag_fail_next(int n) { diagFailLeft_ = n; }
#endif

 private:
  ImageDecoder& inner_;
  Stats stats_;
#if APP_DIAG
  int diagFailLeft_ = 0;
#endif
};
