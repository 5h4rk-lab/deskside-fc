// LVGL heap hooks (lv_conf.h: LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM).
//
// Requests of kPsramThreshold bytes and above (decoded images, draw buffers,
// big strings) come from PSRAM; smaller ones come from internal RAM, which is
// scarce and also needed by Wi-Fi, LwIP and DMA buffers. When the preferred
// heap is full a small request may fall back to PSRAM (slower but alive); a
// large request never falls back to internal RAM, so a decoder asking for a
// big buffer can fail cleanly (nullptr) without starving the network stack.
//
// lv_malloc() and friends are only called from the LVGL task, but the ESP-IDF
// heap is thread-safe in any case.
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <lvgl.h>

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "lv_conf.h must select LV_STDLIB_CUSTOM: the firmware provides the LVGL heap hooks"
#endif

namespace {

constexpr size_t kPsramThreshold = 2048;
constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

}  // namespace

extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void*, size_t) { return nullptr; }  // not supported

void lv_mem_remove_pool(lv_mem_pool_t) {}

void* lv_malloc_core(size_t size) {
  if (size >= kPsramThreshold) return heap_caps_malloc(size, kPsramCaps);
  void* p = heap_caps_malloc(size, kInternalCaps);
  return p ? p : heap_caps_malloc(size, kPsramCaps);
}

void* lv_realloc_core(void* p, size_t new_size) {
  if (p == nullptr) return lv_malloc_core(new_size);
  // A block that already lives in PSRAM stays there; so does any block that
  // grows to the PSRAM threshold or beyond. heap_caps_realloc moves the data
  // when the block has to change heap.
  if (new_size >= kPsramThreshold || esp_ptr_external_ram(p)) {
    return heap_caps_realloc(p, new_size, kPsramCaps);
  }
  void* q = heap_caps_realloc(p, new_size, kInternalCaps);
  return q ? q : heap_caps_realloc(p, new_size, kPsramCaps);
}

void lv_free_core(void* p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon) {
  // Reports PSRAM plus internal RAM together; enough for logging trends.
  multi_heap_info_t in, ps;
  heap_caps_get_info(&in, kInternalCaps);
  heap_caps_get_info(&ps, kPsramCaps);
  mon->total_size = in.total_free_bytes + in.total_allocated_bytes + ps.total_free_bytes + ps.total_allocated_bytes;
  mon->free_size = in.total_free_bytes + ps.total_free_bytes;
  mon->free_cnt = in.free_blocks + ps.free_blocks;
  mon->free_biggest_size = in.largest_free_block > ps.largest_free_block ? in.largest_free_block : ps.largest_free_block;
  mon->used_cnt = in.allocated_blocks + ps.allocated_blocks;
  mon->max_used = mon->total_size - (in.minimum_free_bytes + ps.minimum_free_bytes);
  mon->used_pct = mon->total_size ? (uint8_t)(100 - (mon->free_size * 100) / mon->total_size) : 0;
  mon->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"
