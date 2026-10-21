// Soccer display firmware, app skeleton: boots, prints the build id and proves
// that LVGL runs with its heap hooks. The display, touch, Wi-Fi and the UI
// are added by the next tasks.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "version.h"

namespace {

constexpr int kPowerHoldPin = 7;  // must be HIGH early or the board powers off on battery
constexpr uint32_t kSerialWaitMs = 3000;
constexpr uint32_t kHeartbeatMs = 5000;

// 64-bit monotonic time, never wraps; LVGL takes the low 32 bits.
uint32_t lvglTickMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

void printBuildLine() {
  Serial.printf("soccer-display app %s built %s %s\n", kAppVersion, __DATE__, __TIME__);
}

// One small and one large LVGL allocation: confirms where the hooks put them.
void lvglAllocSelfTest() {
  void* small = lv_malloc(256);
  void* large = lv_malloc(64 * 1024);
  Serial.printf("lvgl %d.%d.%d alloc: 256 B %s, 64 KB %s\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
                LVGL_VERSION_PATCH, small ? (esp_ptr_external_ram(small) ? "psram" : "internal") : "FAILED",
                large ? (esp_ptr_external_ram(large) ? "psram" : "internal") : "FAILED");
  lv_free(small);
  lv_free(large);
  Serial.printf("heap: internal free %u B, psram free %u B\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

}  // namespace

void setup() {
  pinMode(kPowerHoldPin, OUTPUT);
  digitalWrite(kPowerHoldPin, HIGH);

  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < kSerialWaitMs) delay(10);

  printBuildLine();
  lv_tick_set_cb(lvglTickMs);
  lv_init();
  lvglAllocSelfTest();
}

void loop() {
  delay(kHeartbeatMs);
  printBuildLine();
}
