// Image source abstraction. The UI asks for helper-relative paths (for example
// "/api/img?u=...&w=48&h=48"); the simulator serves local files, the firmware
// downloads from the helper. Free of SDL/filesystem/stdio so it also builds
// into the ESP32 firmware.
#pragma once

#include <lvgl.h>

#include <functional>
#include <memory>
#include <string>

// Shared ownership: a cache may evict an image a widget still displays.
using ImagePtr = std::shared_ptr<const lv_image_dsc_t>;

class ImageProvider {
 public:
  virtual ~ImageProvider() = default;

  // Calls `done` exactly once, either before request() returns or later.
  // nullptr means "not available"; the UI keeps its placeholder.
  virtual void request(const std::string& path, std::function<void(ImagePtr)> done) = 0;
};

// Requests `path` and, when delivered, sets it as the source of the lv_image
// widget `img`. The widget keeps the image alive until it is deleted. A
// callback arriving after the widget was deleted does nothing. Calling it
// again for the same widget supersedes the earlier request (the image already
// shown stays valid until the new one arrives).
void image_load_into(lv_obj_t* img, ImageProvider& provider, const std::string& path);

// Marks `img` (an lv_image whose size was set with lv_obj_set_size) to be
// stretched to its size, but only once a picture of another size arrives
// through image_load_into(). Same picture as LV_IMAGE_ALIGN_STRETCH, without
// its cost at creation: lv_image_set_inner_align() lays out the whole screen
// (LVGL 9.2 lv_image_set_pivot), so setting it on every row of a long list
// while building it made the build quadratic (96 picker rows: 1 s on the
// board). The helper sends pictures at the requested size, so on the device
// nothing is stretched at all.
void image_stretch_when_loaded(lv_obj_t* img);
