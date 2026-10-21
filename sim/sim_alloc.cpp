#include "sim_alloc.h"

#include <lvgl.h>
#include <atomic>
#include <stdlib.h>
#include <string.h>

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "sim/lv_conf.h must select LV_STDLIB_CUSTOM: the simulator provides the LVGL heap hooks (sim_alloc.cpp)"
#endif

namespace {

// Every block starts with its size, padded to keep 16-byte alignment.
constexpr size_t kHeader = 16;

std::atomic<size_t> g_bytes{0};  // the simulator's image workers may allocate off the main thread
std::atomic<size_t> g_blocks{0};
bool g_failOn = false;
size_t g_failMin = 0;
size_t g_failMax = 0;
int g_failSkip = 0;
int g_failures = 0;

bool should_fail(size_t size) {
  if (!g_failOn || size < g_failMin || size > g_failMax) return false;
  if (g_failSkip > 0) {
    --g_failSkip;
    return false;
  }
  ++g_failures;
  return true;
}

size_t size_of(void* p) { return *reinterpret_cast<size_t*>(static_cast<char*>(p) - kHeader); }

}  // namespace

namespace sim_alloc {

size_t live_bytes() { return g_bytes.load(); }
size_t live_blocks() { return g_blocks.load(); }

void fail_range(size_t min_size, size_t max_size, int skip) {
  g_failOn = true;
  g_failMin = min_size;
  g_failMax = max_size;
  g_failSkip = skip;
  g_failures = 0;
}

void clear_failures() { g_failOn = false; }

int failures() { return g_failures; }

}  // namespace sim_alloc

extern "C" {

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void*, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}

void* lv_malloc_core(size_t size) {
  if (should_fail(size)) return nullptr;
  char* raw = static_cast<char*>(malloc(size + kHeader));
  if (raw == nullptr) return nullptr;
  *reinterpret_cast<size_t*>(raw) = size;
  g_bytes += size;
  ++g_blocks;
  return raw + kHeader;
}

void lv_free_core(void* p) {
  if (p == nullptr) return;
  g_bytes -= size_of(p);
  --g_blocks;
  free(static_cast<char*>(p) - kHeader);
}

void* lv_realloc_core(void* p, size_t new_size) {
  if (p == nullptr) return lv_malloc_core(new_size);
  if (should_fail(new_size)) return nullptr;
  const size_t old = size_of(p);
  char* raw = static_cast<char*>(realloc(static_cast<char*>(p) - kHeader, new_size + kHeader));
  if (raw == nullptr) return nullptr;
  *reinterpret_cast<size_t*>(raw) = new_size;
  g_bytes += new_size;
  g_bytes -= old;
  return raw + kHeader;
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon) {
  memset(mon, 0, sizeof(*mon));
  mon->total_size = g_bytes.load();
  mon->used_cnt = static_cast<uint32_t>(g_blocks.load());
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"
