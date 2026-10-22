// Soccer display firmware: boots the board, starts the shared UI with live data
// from the helper (HTTPS, after Wi-Fi and the SNTP clock; the `app_demo` env
// runs the demo data instead) and the persistent settings, and prints a compact
// status and memory report on the serial console.
//
// Serial commands. One-letter commands act at once, no Enter needed
// (echo -n g > /dev/ttyACM0); none of them erases or restarts anything:
//   g        goal celebration: demo build, the favourite scores in its live demo match;
//            live build, a test celebration for the favourite (the data is not touched)
//   w        full-time win: demo build, the live demo match ends with the favourite
//            ahead; live build, a test full-time celebration
//   d        demo build: reload the demo data; live build: fetch everything now
//   s0 s1 s2 show the Live, Matches or News tab
//   t        make the next team of the list the favourite (saved; shows persistence)
//   p        print the settings in use
//   k        NVS self-test (PASS/FAIL lines; also a counter that survives reboots and flashes)
//   f        toggle one-second performance lines ([perf]) instead of 5 s
//   m        print the memory report now
// Word commands start with ':' and run only when you press Enter (echo ':wifi' > /dev/ttyACM0).
// While you type one its letters are swallowed, so typing by hand can never run a letter
// command; a word left unfinished for 5 s is dropped, and the rest of its line is swallowed
// too (until Enter, or 5 s more without input), so the tail of a token typed with a pause
// never runs as letter commands:
//   :wifi            one line: connected network, signal, address, saved networks, memory
//   :wipe-wifi       forget every saved network and restart (first-run Wi-Fi setup shows again)
//   :erase-settings  erase the saved settings (not the Wi-Fi networks) and restart
//   :restart         restart the board
//   :set-url <url>   save the helper's address (https only, at most 200 characters);
//                    used from the next restart
//   :set-token <t>   save the helper's token (16 to 128 printable characters, no spaces);
//                    never echoed; used from the next restart
//   :show-config     the helper's address and the first 4 characters of its token
//   :net             one line about the data path and one about pictures ([img]; also in the 5 s status)
//   :sound <name> [v]  play one sound now at the saved volume (or v, 1..100), ignoring the sound
//                    settings: goal win kickoff fulltime low critical wifi
//   :sound tone l|r|both [v]  1 s of 440 Hz on the left channel only, the right only, or both
//                    (which speaker sounds tells whether the board drives two speakers)
//   :sound stop      stop what plays (20 ms fade);  :sound  alone prints the speaker's state
//   :power           one line: battery ADC and voltage, calibration, model, Battery setting, power key,
//                    power hold, USB data (also every 5 s with the status report)
//   :cal-battery <mV>  calibrate the battery reading: <mV> is a multimeter's reading of the pack now
//                    (3000..4400); stored in NVS batt.cal. :cal-battery clear  back to the factory
//                    formula;  :cal-battery  alone shows the state
//   :ota             the software update state: running and idle slots, image states, the trial
//                    record, the Settings row and the manifest address (read-only)
//   :help            list the commands
// The helper's address and token come from NVS (namespace "helper", keys helper.url and
// helper.token, written by :set-url / :set-token, see tools/provision-helper.sh), else
// from the optional build defaults HELPER_URL / HELPER_TOKEN (secrets.ini). A value that
// fails the checks of ui/src/helper_config.h is never used: then no request goes out.
// Only in the diagnostics builds (pio run -e app_diag, also app_debug; -DAPP_DIAG), never in the
// release `app` env, because they can inject a touch:
//   h j n    scripted swipe left / right / list scroll, b0..b2 tap a tab button, =xxxyyy tap at x,y
//            (a frame-time trace of the next 1.5 s is printed: [trace] lines); V starts a trace, v prints it
//   y        toggle repainting the whole screen (or celebration) every frame, to time a full repaint
//   x0..x5   hide/show a part of the goal celebration (0 rays, 1 confetti, 2 stamp box, 3 cutout,
//            4 strip, 5 baked stamp) to see what costs frame time
//   S        scan and print the networks in range (SSID, signal, open/secured)
//   W        join the most recently saved network with a deliberately wrong throw-away key (failure path)
//   Z        join a network that does not exist (the 15 s timeout path)
//   :diag-clock-break    stop SNTP and set the clock to 2023-06-01 (not valid): polling pauses
//   :diag-clock-restore  start SNTP again
//   :diag-burst [n]      n (default 100, at most 200) matches requests in a row, heap every 10 (leak check)
//   :diag-img-churn [n]  n (default 100, at most 300) distinct real pictures (team crests at seven sizes)
//                        through the picture provider, cache and heap every 20 (leak and budget check)
//   :diag-goal-photo     the test goal celebration with a scorer (fetches a 190 px player cutout)
//   :diag-slow-body      one GET to a public endpoint that trickles its body (no token): must end at ~8 s
//   :diag-slow-headers   one GET to a public endpoint that holds its headers 10 s (no token): must end at ~8 s
//   :diag-offline        toggle: refuse every request as if the router were off (stale marker test)
//   :diag-power-off      the switch-off sequence now as a DRY RUN: every step but the GPIO7 write
//                        (logged instead), then the off state; a key press (or :diag-key) restarts
//   :diag-key <ms>       a power key press of 30..20000 ms (OR'ed with the real key); makes any
//                        power-off a dry run: 200 toggles the screen, 1500 shows the hint, 3500 switches off
//   :diag-batt <mV>|off  the battery model gets <mV> (2000..4500) instead of the ADC (low-battery banner test)
//   :ota-url <url>       take the update manifest from <url> instead of the helper (plain http allowed:
//                        a local test server; no token is sent to it); until the next restart
//   :ota-check           check for an update now (as a tap on the Software update row)
//   :ota-install         install the offered update now (as a tap on "available · Install")
// A diagnostics image built with -DAPP_DIAG_CRASH_AT_BOOT=1 aborts 2 s after every start (the
// rollback test: it must never be confirmed, and the previous image comes back by itself).
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

#include "helper_config.h"
#include "battery_cal.h"
#include "platform/esp_http_transport.h"
#include "platform/esp_ota.h"
#include "platform/esp_power_service.h"
#include "platform/esp_wifi_service.h"
#include "platform/i2s_sound_out.h"
#include "platform/nvs_prefs.h"
#include "platform/time_sync.h"
#include "serial_commands.h"
#include "sound_synth.h"
#include "version.h"

// Optional build defaults (secrets.ini); the device normally gets them over the
// serial console into NVS instead (tools/provision-helper.sh).
#ifndef HELPER_URL
#define HELPER_URL ""
#endif
#ifndef HELPER_TOKEN
#define HELPER_TOKEN ""
#endif
// Measurement only: 1 keeps mbedTLS in internal RAM (the IDF default).
#ifndef APP_TLS_INTERNAL
#define APP_TLS_INTERNAL 0
#endif

// An image installed over the air confirms itself only once it is healthy
// (platform/esp_ota.h): the Arduino core must not mark it valid at start.
extern "C" bool verifyRollbackLater() { return true; }

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
NvsPrefs* s_helperPrefs = nullptr;  // namespace "helper": helper.url, helper.token
EspWifiService* s_wifi = nullptr;
I2sSoundOut* s_speaker = nullptr;
EspPowerService* s_power = nullptr;
TaskHandle_t s_prepTask = nullptr;  // "snd_prep": deleted by the loop task once it has suspended itself
std::string s_urlInUse;             // the helper address the app runs with ("" when none)
bool s_helperInUse = false;

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
  if (!fast) {
    printMemory();
    app_host_post_call(app_host_print_net);
    app_host_post_call(app_host_print_power);
  }
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

// Runs in the LVGL task (posted by the :erase-settings command): the settings are written by
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

// ---- helper address and token ------------------------------------------------------

constexpr const char* kUrlKey = "helper.url";
constexpr const char* kTokenKey = "helper.token";

// Reads one value: NVS first, else the build default. `from` says which.
std::string readHelperValue(const char* key, const char* buildDefault, const char*& from) {
  std::string v;
  if (s_helperPrefs && s_helperPrefs->get(key, v)) {
    from = "saved";
    return v;
  }
  v = buildDefault;
  from = v.empty() ? "none" : "build default";
  return v;
}

void wipeString(std::string& s) {
  if (!s.empty()) memset(&s[0], 0, s.size());
  s.clear();
}

HelperConfig loadHelperConfig() {
  HelperConfig c;
  const char* urlFrom = "";
  const char* keyFrom = "";  // where the token came from
  c.url = readHelperValue(kUrlKey, HELPER_URL, urlFrom);
  c.token = readHelperValue(kTokenKey, HELPER_TOKEN, keyFrom);
  const bool urlOk = valid_helper_url(c.url);
  const bool keyOk = valid_token(c.token);
  const char* keyState = keyOk ? "ok" : (c.token.empty() ? "missing" : "INVALID");
  c.valid = urlOk && keyOk;
  // The URL is not a secret; of the token only whether it is usable (log
  // statements never name the token itself: sim/tests/helper_secrets_static_test.cpp).
  Serial.printf("[helper] url %s (%s, %s), token %s (%s): %s\n", urlOk ? c.url.c_str() : "-", urlFrom,
                urlOk ? "ok" : (c.url.empty() ? "missing" : "INVALID"), keyState, keyFrom,
                c.valid ? "live data on" : "no requests will be made (:set-url / :set-token, then :restart)");
  if (!c.valid) {
    c.url.clear();
    wipeString(c.token);
  }
  return c;
}

// :set-url / :set-token / :show-config run in the loop task (their own NVS
// namespace; the NVS API is thread-safe). Values are never echoed: the URL is
// printed only after it passed the checks, the token never.
void setHelperUrl(const std::string& value) {
  if (!valid_helper_url(value)) {
    Serial.printf("[cmd] URL refused: it must start with https://, name a host, have no user, query, spaces or control characters, and be at most %u characters\n",
                  (unsigned)kHelperUrlMaxLen);
    return;
  }
  if (!s_helperPrefs) return;
  s_helperPrefs->set(kUrlKey, value);
  std::string check;
  const bool ok = s_helperPrefs->get(kUrlKey, check) && check == value;
  Serial.printf("[cmd] helper URL %s: %s%s\n", ok ? "saved" : "NOT saved", value.c_str(),
                ok ? " (used after :restart)" : "");
}

void setHelperToken(std::string& value) {
  if (!valid_token(value)) {
    const unsigned minLen = kTokenMinLen;
    const unsigned maxLen = kTokenMaxLen;
    Serial.printf("[cmd] token refused: it must be %u to %u printable characters without spaces (nothing saved)\n",
                  minLen, maxLen);
    wipeString(value);
    return;
  }
  if (!s_helperPrefs) return;
  s_helperPrefs->set(kTokenKey, value);
  std::string check;
  const bool ok = s_helperPrefs->get(kTokenKey, check) && check == value;
  const size_t n = value.size();
  wipeString(check);
  wipeString(value);
  Serial.printf("[cmd] helper token %s (%u characters)%s\n", ok ? "saved" : "NOT saved", (unsigned)n,
                ok ? " (used after :restart)" : "");
}

void showHelperConfig() {
  const char* urlFrom = "";
  const char* keyFrom = "";
  std::string url = readHelperValue(kUrlKey, HELPER_URL, urlFrom);
  std::string token = readHelperValue(kTokenKey, HELPER_TOKEN, keyFrom);
  const bool urlOk = valid_helper_url(url);
  Serial.printf("[cmd] helper url %s (%s%s), token %s (%s) | in use since start: %s\n",
                urlOk ? url.c_str() : (url.empty() ? "(none)" : "(invalid)"), urlFrom, urlOk || url.empty() ? "" : ", not used",
                masked_token(token).c_str(), keyFrom, s_helperInUse ? s_urlInUse.c_str() : "nothing (no requests)");
  wipeString(token);
}

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
    default:
      break;  // line ends and anything else are ignored
  }
}

// ---- speaker ----------------------------------------------------------------------

// :sound [name [volume]] (loop task): parsed here, played in the LVGL task
// (it owns the saved volume). Bad input is answered with the list, not echoed.
void soundCommand(const std::string& value) {
  static const char* const kUsage =
      "[cmd] sound: :sound goal|win|kickoff|fulltime|low|critical|wifi [1-100], :sound tone l|r|both [1-100], "
      ":sound stop, :sound (state)";
  std::string what;
  std::string rest;
  split_word(value, what, rest);
  if (what.empty()) {
    if (s_speaker) s_speaker->print_status();
    Serial.println(kUsage);
    return;
  }
  if (what == "stop") {
    if (!rest.empty()) {
      Serial.println(kUsage);
      return;
    }
    app_host_post(AppCommand::Sound, kSoundArgStop);
    return;
  }
  int sel = -1;
  SoundChannels ch = SoundChannels::Both;
  if (what == "tone") {
    std::string side;
    std::string after;
    split_word(rest, side, after);
    rest = after;
    if (side == "l" || side == "left") {
      ch = SoundChannels::Left;
    } else if (side == "r" || side == "right") {
      ch = SoundChannels::Right;
    } else if (side != "both") {
      Serial.println(kUsage);
      return;
    }
    sel = kSoundArgTone;
  } else {
    static const struct {
      const char* name;
      SoundEvent event;
    } kNames[] = {{"goal", SoundEvent::Goal},        {"win", SoundEvent::WinFanfare},
                  {"kickoff", SoundEvent::KickOff},  {"fulltime", SoundEvent::FullTime},
                  {"low", SoundEvent::BatteryLow},   {"critical", SoundEvent::BatteryCritical},
                  {"wifi", SoundEvent::WifiOk}};
    for (const auto& n : kNames) {
      if (what == n.name) sel = static_cast<int>(n.event);
    }
  }
  int volume = 0;  // the saved one
  std::string number;
  std::string extra;
  split_word(rest, number, extra);
  if (!extra.empty()) sel = -1;  // nothing may follow the volume
  if (!number.empty()) {
    if (number.size() > 3 || number.find_first_not_of("0123456789") != std::string::npos) {
      sel = -1;
    } else {
      volume = atoi(number.c_str());
      if (volume < 1 || volume > 100) sel = -1;
    }
  }
  if (sel < 0) {
    Serial.println(kUsage);
    return;
  }
  app_host_post(AppCommand::Sound, sound_arg(sel, ch, volume));
}

// Makes every clip once, after the first frame, on a low-priority task on the
// display core (below LVGL: it only takes time the screen does not need), so
// no goal ever waits for the synthesis. The float work buffers and the clips
// come from PSRAM (C++ new: platform/psram_new.cpp), and so does this task's
// stack. It takes about 1.6 s: a clip the UI needs within that time after the
// first frame (a Wi-Fi join chime, a battery alert) makes the LVGL task wait
// until that clip is made.
void soundPrepareTask(void*) {
  const size_t internal0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t internalMin0 = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const size_t psram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t psramMin0 = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
  const int made0 = sound_generate_calls();
  const int64_t t0 = esp_timer_get_time();
  sound_prepare_all();
  const int64_t us = esp_timer_get_time() - t0;
  Serial.printf(
      "[sound] clips ready: %d synthesized in %lld ms | internal free %u -> %u (lowest %u -> %u) | psram free %u -> %u "
      "(clips %d B), lowest %u -> %u | prepare stack free %u B\n",
      sound_generate_calls() - made0, (long long)(us / 1000), (unsigned)internal0,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)internalMin0,
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)psram0,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (int)psram0 - (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)psramMin0,
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
      (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
  // A task with a caps-allocated stack is best deleted by another task: the
  // loop task (same core) does it once this one is suspended.
  vTaskSuspend(nullptr);
}

// ---- battery calibration ------------------------------------------------------------

// :cal-battery [mV|clear] (loop task, the ADC's only user). The NVS write runs in
// the LVGL task, which owns the settings namespace.
void calBatteryCommand(const std::string& value) {
  if (!s_power) return;
  const CalCommand c = parse_cal_command(value);
  switch (c.kind) {
    case CalCommand::Kind::Invalid:
      Serial.printf("[cmd] cal-battery: :cal-battery <mV> (a multimeter's reading of the pack, %d to %d), "
                    ":cal-battery clear, :cal-battery (state)\n",
                    battery_cal::kMinMv, battery_cal::kMaxMv);
      return;
    case CalCommand::Kind::Show: {
      const int factory = s_power->read_factory_mv();
      const std::optional<int> ppm = s_power->calibration();
      Serial.printf("[cmd] cal-battery: factory reading %d mV now; %s", factory,
                    ppm ? "calibrated, scale " : "not calibrated (factory formula)\n");
      if (ppm) Serial.printf("%d ppm -> %d mV\n", *ppm, apply_battery_cal(factory, *ppm));
      return;
    }
    case CalCommand::Kind::Clear:
      app_host_post_call([] {
        clear_battery_cal(*s_prefs);
        s_power->set_calibration(std::nullopt);
        Serial.println("[cmd] cal-battery: cleared, the factory formula applies again");
      });
      return;
    case CalCommand::Kind::Set: {
      const int factory = s_power->read_factory_mv();
      const std::optional<int> ppm = battery_cal_ppm(c.mv, factory);
      if (!ppm) {
        Serial.printf("[cmd] cal-battery: refused, %d mV is more than 20 %% away from the reading (%d mV); "
                      "nothing saved\n", c.mv, factory);
        return;
      }
      Serial.printf("[cmd] cal-battery: reading %d mV, multimeter %d mV -> scale %d ppm\n", factory, c.mv, *ppm);
      app_host_post_call_arg(
          [](int value) {
            save_battery_cal(*s_prefs, value);
            const std::optional<int> back = load_battery_cal(*s_prefs);
            s_power->set_calibration(back);
            Serial.printf("[cmd] cal-battery: %s\n", back && *back == value ? "saved (batt.cal), in use now"
                                                                           : "NOT saved; the factory formula stays");
          },
          *ppm);
      return;
    }
  }
}

#if APP_DIAG
// Digits only, inside [lo, hi]; -1 otherwise.
int diagNumber(const std::string& v, int lo, int hi) {
  if (v.empty() || v.size() > 5 || v.find_first_not_of("0123456789") != std::string::npos) return -1;
  const int n = atoi(v.c_str());
  return n >= lo && n <= hi ? n : -1;
}
int s_diagKeyMs = 0;
#endif

// Whole-word commands and the one-letter commands share the console; see serial_commands.h.
SerialCommandSplitter s_splitter;

// A restart while an update is on trial brings the previous image back: say so.
void warnIfTrialPending() {
  if (app_host_trial_pending()) {
    Serial.println("[cmd] WARNING: the update just installed is not confirmed yet (first data or 60 s): this restart rolls it back");
  }
}

void restartNow() {
  warnIfTrialPending();
  Serial.println("[cmd] restarting");
  Serial.flush();
  delay(100);
  esp_restart();
}

void handleSerialItem(SerialCommandSplitter::Item& it) {
  if (!it.isWord) {
    handleSerialByte(static_cast<unsigned char>(it.byte));
    return;
  }
  std::string name;
  std::string value;
  split_word(it.word, name, value);
  wipeString(it.word);  // may hold a token
#if APP_DIAG && !APP_DEMO_DATA
  if (name == "diag-burst") {
    // Optional count, 1 to 200 (default 100).
    static int s_burstCount = 100;
    const int n = value.empty() ? 100 : atoi(value.c_str());
    s_burstCount = n >= 1 && n <= 200 ? n : 100;
    app_host_post_call([] { app_host_diag_burst(s_burstCount); });
    return;
  }
  if (name == "diag-img-churn") {
    // Optional count, 1 to 300 (default 100).
    static int s_churnCount = 100;
    const int n = value.empty() ? 100 : atoi(value.c_str());
    s_churnCount = n >= 1 && n <= 300 ? n : 100;
    app_host_post_call([] { app_host_diag_img_churn(s_churnCount); });
    return;
  }
#endif
#if APP_DIAG
  if (name == "diag-key") {
    s_diagKeyMs = diagNumber(value, 30, 20000);
    if (s_diagKeyMs < 0) {
      Serial.println("[cmd] diag-key: :diag-key <ms>, 30 to 20000");
    } else {
      app_host_post_call([] { app_host_diag_key(s_diagKeyMs); });
    }
    return;
  }
#if !APP_DEMO_DATA
  if (name == "ota-url") {
    if (value.empty() || value.size() > 200 || value.find_first_of(" \t\r\n") != std::string::npos ||
        (value.compare(0, 7, "http://") != 0 && value.compare(0, 8, "https://") != 0)) {
      Serial.println("[cmd] ota-url: :ota-url http://<host>:<port>/<manifest>.json (or https://)");
    } else {
      app_host_diag_ota_url(value);
    }
    return;
  }
#endif
  if (name == "diag-batt") {
    const int mv = value == "off" ? 0 : diagNumber(value, 2000, 4500);
    if (mv < 0 || !s_power) {
      Serial.println("[cmd] diag-batt: :diag-batt <mV> (2000 to 4500) or :diag-batt off");
    } else {
      s_power->diag_inject_mv(mv);
      Serial.printf("[cmd] diag-batt: %s (the model restarts)\n", mv ? "battery model fed the injected value" : "the ADC again");
      if (mv) Serial.printf("[cmd] diag-batt: %d mV\n", mv);
    }
    return;
  }
#endif
  if (name == "set-token") {
    setHelperToken(value);
  } else if (name == "cal-battery") {
    calBatteryCommand(value);
  } else if (name == "set-url") {
    setHelperUrl(value);
  } else if (name == "sound") {
    soundCommand(value);
  } else if (!value.empty()) {
    // Only the settings words take a value. Neither the value nor the name is
    // echoed: "set-tokenXYZ..." typed without its space would be a token.
    Serial.println("[cmd] not a command that takes a value (nothing is echoed; :help lists them)");
  } else if (name == "show-config") {
    showHelperConfig();
  } else if (name == "net") {
    app_host_post_call(app_host_print_net);
  } else if (name == "power") {
    app_host_post_call(app_host_print_power);
  } else if (name == "ota") {
    app_host_post_call(app_host_print_ota);
#if APP_DIAG && !APP_DEMO_DATA
  } else if (name == "ota-check") {
    app_host_post_call(app_host_diag_ota_check);
  } else if (name == "ota-install") {
    app_host_post_call(app_host_diag_ota_install);
#endif
#if APP_DIAG
  } else if (name == "diag-power-off") {
    app_host_post_call(app_host_diag_power_off);
#endif
#if APP_DIAG
  } else if (name == "diag-clock-break") {
    time_sync_diag_break();
  } else if (name == "diag-clock-restore") {
    time_sync_diag_restore();
#if !APP_DEMO_DATA
  } else if (name == "diag-slow-body") {
    app_host_post_call([] { app_host_diag_slow(false); });
  } else if (name == "diag-slow-headers") {
    app_host_post_call([] { app_host_diag_slow(true); });
  } else if (name == "diag-goal-photo") {
    app_host_post_call(app_host_diag_goal_photo);
  } else if (name == "diag-offline") {
    app_host_post_call(app_host_diag_offline_toggle);
#endif
#endif
  } else if (name == "wifi") {
    app_host_post_call(wifiPrintState);
  } else if (name == "wipe-wifi") {
    warnIfTrialPending();
    app_host_post_call(wifiWipeAndRestart);
  } else if (name == "erase-settings") {
    warnIfTrialPending();
    app_host_post_call(eraseSettingsAndRestart);  // runs in the LVGL task, like the settings writes
  } else if (name == "restart") {
    restartNow();
  } else if (name == "help") {
    Serial.println("[cmd] letters: g w d s0 s1 s2 t p k f m | words (Enter): :wifi :wipe-wifi :erase-settings :restart "
                   ":set-url <url> :set-token <token> :show-config :net :ota :sound [goal|win|kickoff|fulltime|low|critical|"
                   "wifi|tone l|tone r|tone both|stop] [volume] :power :cal-battery [<mV>|clear] :help | a word unfinished for 5 s is dropped with the "
                   "rest of its line");
  } else {
    // Not echoed: a mistyped word may hold a secret.
    Serial.println("[cmd] unknown command (not echoed; :help lists them)");
  }
}

void pollSerial() {
  std::vector<SerialCommandSplitter::Item> items;
  while (Serial.available() > 0) s_splitter.feed(static_cast<char>(Serial.read()), millis(), items);
  s_splitter.poll(millis());
  for (SerialCommandSplitter::Item& it : items) handleSerialItem(it);
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
  // Before anything else that could fail: an update on trial that keeps
  // failing to start is rolled back here (this call then restarts).
  const OtaBoot otaBoot = ota_boot();
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
  static NvsPrefs helperPrefs("helper");
  s_helperPrefs = &helperPrefs;
  // mbedTLS (HTTPS, and the Wi-Fi supplicant's crypto) allocates in PSRAM from
  // here on: internal RAM is too short for TLS record buffers next to Wi-Fi.
#if !APP_TLS_INTERNAL
  tls_use_psram();
#endif
  // The saved Wi-Fi list must be known before the UI is built (it opens the
  // first-run setup when the list is empty). The radio starts after the first frame.
  static EspWifiService wifi;
  s_wifi = &wifi;
  wifi.load();
  Serial.printf("[heap] %-22s internal free %6u\n", "after NVS", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  lvgl_port_start();
  Serial.printf("[heap] %-22s internal free %6u\n", "after the LVGL port", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  // The speaker: a failed start leaves it silent (logged) and the app runs on.
  static I2sSoundOut speaker;
  speaker.begin();
  s_speaker = &speaker;

  // The battery reading and the power key, before the UI asks for them.
  static EspPowerService power;
  power.begin(prefs);
  s_power = &power;
  Serial.printf("[heap] %-22s internal free %6u\n", "after the speaker", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (!lvgl_lock(1000)) {
    Serial.println("[main] FATAL: cannot take the LVGL lock to build the UI");
    for (;;) delay(2000);
  }
  {
    HelperConfig helper = loadHelperConfig();
    s_helperInUse = helper.valid;
    s_urlInUse = helper.url;
    s_appOk = app_host_start(prefs, backlight, wifi, power, &speaker, helper, otaBoot);
    wipeString(helper.token);
  }
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
  // (The update trial's health check reads the frame count and wifi.started() itself.)
  if (s_appOk && !wifi.start()) Serial.println("[main] the Wi-Fi service did not start (see above)");
  if (xTaskCreatePinnedToCoreWithCaps(soundPrepareTask, "snd_prep", 8192, nullptr, 1, &s_prepTask, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    s_prepTask = nullptr;
    Serial.println("[sound] could not start the clip preparation: clips are made on first use");
  }
  Serial.printf(
      "[boot] first frame after %lu ms (%lu ms of it waiting for a serial monitor); frames %lu, %lu blocks, %lu us "
      "flush, backlight %u%%; epoch %lld (0: clock not set yet)\n",
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
  if (s_prepTask && eTaskGetState(s_prepTask) == eSuspended) {
    vTaskDeleteWithCaps(s_prepTask);
    s_prepTask = nullptr;
    Serial.println("[sound] clip preparation task deleted");
  }

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

  // The battery, once a second (16 ADC readings, about 1 ms).
  static uint32_t lastPowerSample = millis();
  if (s_power && now - lastPowerSample >= EspPowerService::kSampleMs) {
    lastPowerSample = now;
    s_power->sample();
  }

#if APP_DIAG && defined(APP_DIAG_CRASH_AT_BOOT) && APP_DIAG_CRASH_AT_BOOT
  // The rollback test image: never healthy.
  if (millis() - s_bootMs > 2000) {
    Serial.println("[diag] APP_DIAG_CRASH_AT_BOOT: aborting on purpose (rollback test)");
    Serial.flush();
    abort();
  }
#endif

  const uint32_t period = s_fast ? kFastStatusMs : kStatusMs;
  if (now - lastReport >= period) {
    printStatus(now - lastReport, s_fast);
    lastReport = now;
  }
}
