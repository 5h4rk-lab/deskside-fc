// C++ `new` and `delete` for the whole firmware: blocks come from PSRAM first
// and from internal RAM only when PSRAM is full.
//
// Why: internal RAM is the scarce resource (Wi-Fi, lwIP, the display's DMA
// buffers and task stacks need it), while the app's own C++ objects (the
// Store's matches, news and teams with their many small strings, std::function
// closures, HttpSource queues) do not. Measured on the board with live data:
// internal free 128 KB before the first fetch and 58 KB (largest block 23 KB)
// after it while every small object stayed internal; see the Task 6 report.
// malloc() itself is untouched (the core already sends blocks over 4 KB to
// PSRAM), so C components (lwIP, esp_http_client, drivers) keep their choices,
// and anything that must be internal asks for it with heap_caps_*.
//
// Nothing in this firmware touches C++ heap objects from an interrupt or while
// the flash cache is disabled (the only code that runs then is the IDF's own
// IRAM code), which is the one thing PSRAM cannot serve.
#include <esp_heap_caps.h>
#include <stdlib.h>

#include <new>

namespace {

void* allocate(size_t size) {
  if (size == 0) size = 1;
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

// Built with -fno-exceptions: a plain `new` that cannot be served stops here
// with a clear reason instead of returning null to code that never checks.
void* allocate_or_abort(size_t size) {
  void* p = allocate(size);
  if (p == nullptr) abort();
  return p;
}

}  // namespace

void* operator new(size_t size) { return allocate_or_abort(size); }
void* operator new[](size_t size) { return allocate_or_abort(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return allocate(size); }

void operator delete(void* p) noexcept { heap_caps_free(p); }
void operator delete[](void* p) noexcept { heap_caps_free(p); }
void operator delete(void* p, size_t) noexcept { heap_caps_free(p); }
void operator delete[](void* p, size_t) noexcept { heap_caps_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { heap_caps_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { heap_caps_free(p); }
