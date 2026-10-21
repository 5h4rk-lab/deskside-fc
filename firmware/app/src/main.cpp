// Soccer display firmware: boots the board, starts the shared UI with demo data
// and the persistent settings, and prints a compact status and memory report on
// the serial console.
//
// Serial commands (one letter, no Enter needed; echo -n g > /dev/ttyACM0):
//   g        the favourite scores in its live demo match (goal celebration)
//   w        the live demo match ends with the favourite ahead (win)
//   d        reload the demo data (after a win the live match is gone)
//   s0 s1 s2 show the Live, Matches or News tab
//   t        make the next team of the list the favourite (saved; shows persistence)
//   p        print the settings in use
//   e        erase the saved settings and restart (defaults: Arsenal, UTC, full brightness)
//   k        NVS self-test (PASS/FAIL lines; also a counter that survives reboots and flashes)
//   f        toggle one-second performance lines ([perf]) instead of 5 s
//   m        print the memory report now
//   r        restart the board
// Whole-word commands (type the word and Enter: echo wifi > /dev/ttyACM0):
//   wifi       one line: connected network, signal, address, number of saved networks, memory
//   wipe-wifi  forget every saved network and restart (the first-run Wi-Fi setup shows again)
// Only in the diagnostics builds (pio run -e app_diag, also app_debug; -DAPP_DIAG), never in the
// release `app` env, because they can inject a touch:
//   h j n    scripted swipe left / right / list scroll, b0..b2 tap a tab button, =xxxyyy tap at x,y
//            (a frame-time trace of the next 1.5 s is printed: [trace] lines); V starts a trace, v prints it
//   y        toggle repainting the whole screen (or celebration) every frame, to time a full repaint
//   x0..x5   hide/show a part of the goal celebration (0 rays, 1 confetti, 2 stamp box, 3 cutout,
//            4 strip, 5 baked stamp) to see what costs frame time
//   S        scan and print the networks in range (SSID, signal, open/secured)
//   W        join TP-Link_D066 with a deliberately wrong throw-away passphrase (failure path)
//   Z        join a network that does not exist (the 15 s timeout path)
// The commands are read here (the Arduino loop task) and handed to the LVGL
// task through a queue (the settings erase and the NVS self-test too), so the
// console never slows the screen and nothing but the LVGL task touches the UI
// or the settings.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <stdio.h>

#include "app_diag.h"
#include "app_host.h"
#include "board/backlight_ledc.h"
#include "board/display_st7789.h"
#include "board/lvgl_port.h"
#include "board/pins.h"
#include <string>
#include <vector>

#include "platform/esp_wifi_service.h"
#include "platform/nvs_prefs.h"
#include "serial_commands.h"
#include "version.h"

// The app is built inside setup(); give the Arduino loop task room for it.
SET_LOOP_TASK_STACK_SIZE(12 * 1024);  // measured use about 6.4 KB (Task 4)

namespace {

constexpr uint32_t kSerialWaitMs = 1000;
constexpr uint32_t kStatusMs = 5000;
constexpr uint32_t kFastStatusMs = 1000;
constexpr uint32_t kLoopStepMs = 20;

// Power hold: GPIO7 must be HIGH or the board powers off on battery. The
// output latch is set to 1 BEFORE the output driver is enabled, so the pin
// never dips low (pinMode(OUTPUT) followed by digitalWrite(HIGH) can).
void powerHoldHigh() {
  gpio_set_level((gpio_num_t)pins::kPowerHold, 1);
  gpio_set_direction((gpio_num_t)pins::kPowerHold, GPIO_MODE_INPUT_OUTPUT);  // output + readable
}

// Runs as a static constructor, i.e. before app_main() and initArduino(): the
// first application code to execute after the ESP-IDF startup.
__attribute__((constructor(101))) void earlyPowerHold() { powerHoldHigh(); }

// The UI asks for a brightness while it is being built, before the first frame
// has been drawn; the panel RAM holds garbage until then. This keeps the
// request and passes it on only once open() is called (after the first frame).
class GatedBacklight : public Backlight {
 public:
  explicit GatedBacklight(Backlight& real) : real_(real) {}
  void set_percent(uint8_t percent) override {
    percent_ = percent;
    if (open_) real_.set_percent(percent);
  }
  // Called from the Arduino loop task while the LVGL task may be calling
  // set_percent(): both run under the LVGL lock.
  void open() {
    const bool locked = lvgl_lock(1000);
    if (!locked) Serial.println("[main] WARNING: LVGL lock timeout while opening the backlight");
    open_ = true;
    real_.set_percent(percent_);
    if (locked) lvgl_unlock();
  }
  uint8_t percent() const { return percent_; }

 private:
  Backlight& real_;
  uint8_t percent_ = 0;
  volatile bool open_ = false;
};

uint32_t s_bootMs = 0;          // millis() when setup() began
uint32_t s_firstFrameMs = 0;    // time from setup() to the first frame leaving the SPI bus
uint32_t s_serialWaitedMs = 0;  // time spent waiting for a serial monitor
bool s_fast = false;            // [perf] line every second
uint32_t s_loopLagMaxMs = 0;    // worst overshoot of the loop's own 20 ms step since the last report
bool s_appOk = false;
NvsPrefs* s_prefs = nullptr;
EspWifiService* s_wifi = nullptr;

void printMemory() {
  lv_mem_monitor_t mon = {};
  if (lvgl_lock(100)) {
    lv_mem_monitor(&mon);
    lvgl_unlock();
  }
  Serial.printf(
      "[mem] internal free %u (min %u, largest %u) psram free %u (min %u, largest %u) | lvgl used %u of %u B, "
      "free %u, frag %u%% | stack free lvgl %u loop %u B | boot: first frame at %lu ms (%lu ms serial wait)\n",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)(mon.total_size - mon.free_size),
      (unsigned)mon.total_size, (unsigned)mon.free_size, (unsigned)mon.frag_pct, (unsigned)lvgl_port_stack_free_bytes(),
      (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)), (unsigned long)s_firstFrameMs,
      (unsigned long)s_serialWaitedMs);
  if (s_wifi) Serial.printf("[mem] wifi worker stack free %u B\n", (unsigned)s_wifi->worker_stack_free_bytes());
}

// Numbers for `elapsedMs` of running: `frames` counts completed screen
// refreshes, `cpu` is the share of time spent inside lv_timer_handler() (render,
// flush wait and the host loop timer), `busy` the average of the calls that
// rendered something (took more than 1 ms), `loop_lag_max` how late the Arduino
// loop task ran (it has a lower priority than LVGL: a large value would mean
// LVGL starves it).
void printStatus(uint32_t elapsedMs, bool fast) {
  LvglPortStats s;
  lvgl_port_take_stats(s);
  const uint32_t ms = elapsedMs ? elapsedMs : 1;
  const uint32_t cpuTenths = (uint32_t)((uint64_t)s.handlerUs * 1000 / ((uint64_t)ms * 1000));  // 0.1 %
  const uint32_t waitTenths = (uint32_t)((uint64_t)s.waitUs * 1000 / ((uint64_t)ms * 1000));
  const uint32_t busyAvgUs = s.busyCalls ? s.busyUs / s.busyCalls : 0;
  const uint32_t handlerAvgUs = s.handlerCalls ? s.handlerUs / s.handlerCalls : 0;
  const uint32_t fpsTenths = (uint32_t)((uint64_t)s.frames * 10000 / ms);
  const uint32_t frameFlushAvgUs = s.frames ? s.flushUs / s.frames : 0;
  Serial.printf(
      "[%s] up %lu s | frames %lu (%lu.%lu/s) flush/frame avg %lu us max %lu us | lv_timer_handler %lu calls avg %lu us, "
      "busy %lu calls avg %lu.%lu ms, max %lu.%lu ms, cpu %lu.%lu%% (flush wait %lu.%lu%%, longest %lu us) | chunk gap avg %lu us max %lu us, pre %lu us post %lu us | display_err %lu loop_lag_max %lu ms\n",
      fast ? "perf" : "status", (unsigned long)(millis() / 1000), (unsigned long)s.frames,
      (unsigned long)(fpsTenths / 10), (unsigned long)(fpsTenths % 10), (unsigned long)frameFlushAvgUs,
      (unsigned long)s.frameFlushMaxUs, (unsigned long)s.handlerCalls, (unsigned long)handlerAvgUs,
      (unsigned long)s.busyCalls, (unsigned long)(busyAvgUs / 1000), (unsigned long)(busyAvgUs % 1000 / 100),
      (unsigned long)(s.handlerMaxUs / 1000), (unsigned long)(s.handlerMaxUs % 1000 / 100),
      (unsigned long)(cpuTenths / 10), (unsigned long)(cpuTenths % 10), (unsigned long)(waitTenths / 10),
      (unsigned long)(waitTenths % 10), (unsigned long)s.waitMaxUs,
      (unsigned long)(s.chunkGapCount ? s.chunkGapUs / s.chunkGapCount : 0), (unsigned long)s.chunkGapMaxUs,
      (unsigned long)(s.preCount ? s.preUs / s.preCount : 0), (unsigned long)(s.preCount ? s.postUs / s.preCount : 0),
      (unsigned long)s.displayErrors,
      (unsigned long)s_loopLagMaxMs);
  s_loopLagMaxMs = 0;
  if (!fast) printMemory();
}

// ---- NVS self-test --------------------------------------------------------------------

// Exercises NvsPrefs on the real partition with its own keys (selftest.*). The
// counter selftest.boots grows with every run: it proves the values survive a
// restart and a USB re-flash (the partition is not part of the uploaded image).
void nvsSelfTest() {
  if (!s_prefs) return;
  NvsPrefs& p = *s_prefs;
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    Serial.printf("[nvs-test] %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  Serial.printf("[nvs-test] persistent: %s\n", p.persistent() ? "yes" : "NO (RAM fallback)");
  std::string out;

  const int before = pref_int(p, "selftest.boots", 0);
  set_pref_int(p, "selftest.boots", before + 1);
  Serial.printf("[nvs-test] selftest.boots was %d, now %d\n", before, pref_int(p, "selftest.boots", -1));
  check(pref_int(p, "selftest.boots", -1) == before + 1, "counter written and read back");

  check(!p.get("selftest.missing", out), "a missing key reads false");
  p.set("selftest.empty", "");
  check(p.get("selftest.empty", out) && out.empty(), "an empty value round-trips");
  const std::string utf8 = "Atl\xC3\xA9tico Madrid";
  p.set("selftest.utf8", utf8);
  check(p.get("selftest.utf8", out) && out == utf8, "UTF-8 round-trips");
  const std::string big(kPrefsMaxValueLen, 'a');
  p.set("selftest.big", big);
  check(p.get("selftest.big", out) && out == big, "a 1000-byte value round-trips");
  p.set("selftest.big", std::string(kPrefsMaxValueLen + 1, 'b'));
  check(p.get("selftest.big", out) && out == big, "a 1001-byte value is rejected, the old value stays");
  p.set("123456789012345", "x");
  check(p.get("123456789012345", out) && out == "x", "a 15-character key works");
  p.remove("123456789012345");
  check(!p.get("123456789012345", out), "remove deletes it");
  p.set("1234567890123456", "x");
  check(!p.get("1234567890123456", out), "a 16-character key is rejected");
  p.set("Bad Key", "x");
  check(!p.get("Bad Key", out), "a key with a space or capitals is rejected");
  p.set("selftest.nul", std::string("a\0b", 3));
  check(!p.get("selftest.nul", out), "a value with a NUL byte is rejected");

  // A corrupted entry: the right key with the wrong type (a blob).
  nvs_handle_t raw = 0;
  if (nvs_open("app", NVS_READWRITE, &raw) == ESP_OK) {
    const uint8_t junk[3] = {1, 2, 3};
    nvs_set_blob(raw, "selftest.blob", junk, sizeof(junk));
    nvs_commit(raw);
    nvs_close(raw);
    out = "untouched";
    check(!p.get("selftest.blob", out) && out == "untouched", "a wrongly typed (corrupted) entry reads false");
    if (nvs_open("app", NVS_READWRITE, &raw) == ESP_OK) {
      nvs_erase_key(raw, "selftest.blob");
      nvs_commit(raw);
      nvs_close(raw);
    }
  } else {
    check(false, "could not open the namespace for the corruption test");
  }

  for (const char* k : {"selftest.empty", "selftest.utf8", "selftest.big"}) p.remove(k);
  Serial.printf("[nvs-test] %s (%d failures)\n", fails == 0 ? "ALL PASSED" : "FAILED", fails);
}

// Runs in the LVGL task (posted by the 'e' command): the settings are written by
// that task too, so the erase cannot interleave with a write.
void eraseSettingsAndRestart() {
  if (s_prefs && s_prefs->clear()) {
    Serial.println("[cmd] settings erased, restarting");
    Serial.flush();
    delay(100);
    esp_restart();
  }
  Serial.println("[cmd] erase FAILED: the settings were not cleared (no restart)");
}

// Wi-Fi word commands run in the LVGL task (the service belongs to it).
void wifiPrintState() {
  if (s_wifi) s_wifi->print_state();
}

void wifiWipeAndRestart() {
  if (!s_wifi) return;
  s_wifi->wipe();
  Serial.println("[cmd] saved Wi-Fi networks wiped, restarting to show the first-run setup");
  Serial.flush();
  delay(100);
  esp_restart();
}

#if APP_DIAG
void wifiDiagScan() {
  if (s_wifi) s_wifi->diag_print_scan();
}
void wifiDiagWrongPassword() {
  if (s_wifi) s_wifi->diag_wrong_password_test();
}
void wifiDiagTimeout() {
  if (s_wifi) s_wifi->diag_timeout_test();
}
#endif

// ---- serial commands ---------------------------------------------------------------

bool s_wantTabDigit = false;
#if APP_DIAG
bool s_wantHideDigit = false;
bool s_wantButtonDigit = false;
int s_tapDigits = -1;  // after '=': collecting the 6 digits xxxyyy of a scripted tap
int s_tapValue = 0;
const char* s_dumpLabel = nullptr;
uint32_t s_dumpAtMs = 0;

// Scripted finger for measurements (touch is ignored while it runs); the trace
// of the frames that follow is printed 1.5 s later.
void scriptedGesture(const char* label, int x0, int y0, int x1, int y1, uint32_t durMs) {
  lvgl_port_inject_drag(x0, y0, x1, y1, durMs);
  s_dumpLabel = label;
  s_dumpAtMs = millis() + durMs + 1500;
  Serial.printf("[cmd] gesture: %s\n", label);
}
#endif  // APP_DIAG

void handleSerialByte(int c) {
  if (s_wantTabDigit) {
    s_wantTabDigit = false;
    if (c >= '0' && c <= '9') {
      app_host_post(AppCommand::Tab, c - '0');
      return;
    }
    // Not a digit: treat it as a fresh command below.
  }
#if APP_DIAG
  if (s_tapDigits >= 0) {
    if (c >= '0' && c <= '9') {
      s_tapValue = s_tapValue * 10 + (c - '0');
      if (++s_tapDigits == 6) {
        char label[40];
        snprintf(label, sizeof(label), "tap at %d,%d", s_tapValue / 1000, s_tapValue % 1000);
        // The label must outlive the gesture's trace dump: keep it in a static buffer.
        static char kept[40];
        memcpy(kept, label, sizeof(kept));
        scriptedGesture(kept, s_tapValue / 1000, s_tapValue % 1000, s_tapValue / 1000, s_tapValue % 1000, 60);
        s_tapDigits = -1;
      }
      return;
    }
    s_tapDigits = -1;  // not a digit: a fresh command
  }
  if (s_wantButtonDigit) {
    s_wantButtonDigit = false;
    if (c >= '0' && c <= '2') {
      static const char* const names[] = {"tap Live button", "tap Matches button", "tap News button"};
      scriptedGesture(names[c - '0'], 40 + 80 * (c - '0'), 304, 40 + 80 * (c - '0'), 304, 60);
      return;
    }
  }
  if (s_wantHideDigit) {
    s_wantHideDigit = false;
    if (c >= '0' && c <= '9') {
      app_host_post(AppCommand::Hide, c - '0');
      return;
    }
  }
#endif  // APP_DIAG
  switch (c) {
    case 'g':
      app_host_post(AppCommand::Goal);
      break;
    case 'w':
      app_host_post(AppCommand::Win);
      break;
    case 'd':
      app_host_post(AppCommand::ReloadDemo);
      break;
    case 's':
      s_wantTabDigit = true;
      break;
#if APP_DIAG
    case 'x':
      s_wantHideDigit = true;
      break;
    case 'y':
      app_host_post(AppCommand::Churn);
      break;
    case 'h':
      scriptedGesture("swipe left", 200, 150, 40, 150, 200);
      break;
    case 'j':
      scriptedGesture("swipe right", 40, 150, 200, 150, 200);
      break;
    case 'n':
      scriptedGesture("scroll up", 120, 250, 120, 110, 300);
      break;
    case 'b':
      s_wantButtonDigit = true;
      break;
    case '=':
      s_tapDigits = 0;
      s_tapValue = 0;
      break;
    case 'V':
      lvgl_port_trace_start();
      Serial.println("[cmd] trace started (v prints it)");
      break;
    case 'v':
      lvgl_port_trace_dump("now");
      break;
    case 'S':
      app_host_post_call(wifiDiagScan);
      break;
    case 'W':
      app_host_post_call(wifiDiagWrongPassword);
      break;
    case 'Z':
      app_host_post_call(wifiDiagTimeout);
      break;
#endif  // APP_DIAG
    case 't':
      app_host_post(AppCommand::NextTeam);
      break;
    case 'p':
      app_host_post(AppCommand::Settings);
      break;
    case 'e':
      app_host_post_call(eraseSettingsAndRestart);  // runs in the LVGL task, like the settings writes
      break;
    case 'k':
      app_host_post_call(nvsSelfTest);
      break;
    case 'f':
      s_fast = !s_fast;
      Serial.printf("[cmd] performance lines every %s\n", s_fast ? "second" : "5 s (with the memory report)");
      break;
    case 'm':
      printMemory();
      break;
    case 'r':
      Serial.println("[cmd] restarting");
      Serial.flush();
      delay(100);
      esp_restart();
      break;
    default:
      break;  // line ends and anything else are ignored
  }
}

// Whole-word commands and the one-letter commands share the console; see serial_commands.h.
SerialCommandSplitter s_splitter({"wifi", "wipe-wifi"});

void handleSerialItem(const SerialCommandSplitter::Item& it) {
  if (!it.isWord) {
    handleSerialByte(static_cast<unsigned char>(it.byte));
  } else if (it.word == "wifi") {
    app_host_post_call(wifiPrintState);
  } else if (it.word == "wipe-wifi") {
    app_host_post_call(wifiWipeAndRestart);
  }
}

void pollSerial() {
  std::vector<SerialCommandSplitter::Item> items;
  while (Serial.available() > 0) s_splitter.feed(static_cast<char>(Serial.read()), millis(), items);
  s_splitter.poll(millis(), items);
  for (const SerialCommandSplitter::Item& it : items) handleSerialItem(it);
}

}  // namespace

void setup() {
  s_bootMs = millis();
  powerHoldHigh();  // again, harmless: the latch is already 1

  // Logging must never hold the screen back: with nobody reading the console
  // (the board runs from a charger, or the monitor is closed) the default
  // transmit buffer fills and every write then waits for its timeout, which
  // stretched the first start-up to 10 s. A bigger buffer holds the boot
  // lines, and a short timeout drops the rest instead of waiting.
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(5);
  while (!Serial && millis() - s_bootMs < kSerialWaitMs) delay(10);
  s_serialWaitedMs = millis() - s_bootMs;
  Serial.printf("soccer-display app %s built %s %s\n", kAppVersion, __DATE__, __TIME__);
  Serial.printf("reset reason %d, power hold GPIO%d level %d, CPU %lu MHz, flash %lu MHz\n", (int)esp_reset_reason(),
                pins::kPowerHold, gpio_get_level((gpio_num_t)pins::kPowerHold), (unsigned long)getCpuFrequencyMhz(),
                (unsigned long)(ESP.getFlashChipSpeed() / 1000000));

  // The backlight stays off until the first frame has been drawn.
  static LedcBacklight ledc;
  if (!ledc.ok()) Serial.println("[backlight] ERROR: backlight unavailable");
  static GatedBacklight backlight(ledc);

  Serial.printf("[heap] %-22s internal free %6u\n", "at setup", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  // Settings first: a broken NVS must never keep the screen from starting.
  static NvsPrefs prefs;
  s_prefs = &prefs;
  // The saved Wi-Fi list must be known before the UI is built (it opens the
  // first-run setup when the list is empty). The radio starts after the first frame.
  static EspWifiService wifi;
  s_wifi = &wifi;
  wifi.load();
  Serial.printf("[heap] %-22s internal free %6u\n", "after NVS", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  lvgl_port_start();
  Serial.printf("[heap] %-22s internal free %6u\n", "after the LVGL port", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (!lvgl_lock(1000)) {
    Serial.println("[main] FATAL: cannot take the LVGL lock to build the UI");
    for (;;) delay(2000);
  }
  s_appOk = app_host_start(prefs, backlight, wifi);
  lvgl_unlock();
  if (!s_appOk) Serial.println("[main] the UI did not start (see above); the screen stays dark");

  // Wait (bounded) for the first frame to leave the SPI bus, then light the panel.
  uint32_t waited = 0;
  LvglPortStats first = {};
  while (waited < 1000) {
    delay(5);
    waited += 5;
    LvglPortStats s;
    lvgl_port_take_stats(s);
    first.frames += s.frames;
    first.flushes += s.flushes;
    first.flushUs += s.flushUs;
    if (s.frameFlushMaxUs > first.frameFlushMaxUs) first.frameFlushMaxUs = s.frameFlushMaxUs;
    if (first.frames > 0) break;
  }
  s_firstFrameMs = millis() - s_bootMs;
  if (s_appOk) backlight.open();
  if (s_appOk && !wifi.start()) Serial.println("[main] the Wi-Fi service did not start (see above)");
  Serial.printf(
      "[boot] first frame after %lu ms (%lu ms of it waiting for a serial monitor); frames %lu, %lu blocks, %lu us "
      "flush, backlight %u%%; demo epoch %lld\n",
      (unsigned long)s_firstFrameMs, (unsigned long)s_serialWaitedMs, (unsigned long)first.frames,
      (unsigned long)first.flushes, (unsigned long)first.flushUs, (unsigned)backlight.percent(),
      (long long)app_host_epoch());
  if (first.frames == 0) Serial.println("[boot] WARNING: no frame completed within 1 s");
  printMemory();
}

void loop() {
  static uint32_t lastReport = millis();
  static uint32_t lastStep = millis();

  pollSerial();

  delay(kLoopStepMs);
  const uint32_t now = millis();
  const uint32_t step = now - lastStep;
  lastStep = now;
  if (step > kLoopStepMs && step - kLoopStepMs > s_loopLagMaxMs) s_loopLagMaxMs = step - kLoopStepMs;

#if APP_DIAG
  if (s_dumpLabel && (int32_t)(now - s_dumpAtMs) >= 0) {
    lvgl_port_trace_dump(s_dumpLabel);
    s_dumpLabel = nullptr;
  }
#endif

  const uint32_t period = s_fast ? kFastStatusMs : kStatusMs;
  if (now - lastReport >= period) {
    printStatus(now - lastReport, s_fast);
    lastReport = now;
  }
}
