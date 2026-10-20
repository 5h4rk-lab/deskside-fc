// Time zone list: every zone of zones() in its display order with the offset
// that applies at `now` (daylight saving included), the current one ticked.
// Opened from Settings, in the same overlay. Shared with the ESP32 firmware:
// no clock reads, no SDL, no stdio.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

// Builds the list as a child of `parent`, filling it. Tapping a row calls
// `onPick` with the zone id (not after a scroll gesture); Back calls `onBack`.
lv_obj_t* zone_list_create(lv_obj_t* parent, const std::string& currentId, int64_t now,
                           std::function<void(std::string zoneId)> onPick,
                           std::function<void()> onBack);

struct ZoneRowParts {
  std::string id;
  lv_obj_t* row = nullptr;
  lv_obj_t* label = nullptr;   // "New York, Toronto"
  lv_obj_t* offset = nullptr;  // "UTC-4"
  lv_obj_t* tick = nullptr;    // shown on the current zone only
};

struct ZoneListParts {
  lv_obj_t* back = nullptr;
  lv_obj_t* title = nullptr;  // "Time zone"
  lv_obj_t* list = nullptr;
  std::vector<ZoneRowParts> rows;  // in zones() order
};

// Parts of a list made by zone_list_create(), or nullptr for any other object.
const ZoneListParts* zone_list_parts(lv_obj_t* screen);
