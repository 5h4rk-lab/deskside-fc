#include "health.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <nvs.h>
#include <string.h>

#include "../app_diag.h"
#include "esp_ota.h"
#include "version.h"

namespace {

constexpr const char* kNamespace = "health";
constexpr const char* kRecordKey = "crash";  // 5 characters; nothing else in the namespace
constexpr uint32_t kRtcMagic = 0x5AFEB007u;
constexpr uint32_t kTickMs = 1000;

// How long this start has run, kept where a panic, a watchdog and a software
// restart do not clear it (RTC slow memory, not initialised at start).
struct RtcUptime {
  uint32_t magic;
  uint32_t uptimeMs;
  uint32_t check;
};
RTC_NOINIT_ATTR RtcUptime s_rtc;

uint32_t rtcCheck(uint32_t up) { return (up ^ 0xA5A5A5A5u) + kRtcMagic; }

HealthBoot s_boot;
CrashRecord s_stored;       // what was stored at this start (on_uptime's input)
bool s_cleared = false;     // the record was cleared after a stable run
bool s_leaving = false;     // safe mode's time ran: restarting
std::string s_coredump;     // the stored core dump's summary at start ("" when none)

int64_t uptimeMs() { return esp_timer_get_time() / 1000; }

ResetKind kindOf(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return ResetKind::PowerOn;
    case ESP_RST_SW: return ResetKind::Software;
    case ESP_RST_EXT: return ResetKind::External;
    case ESP_RST_USB: return ResetKind::External;
    case ESP_RST_JTAG: return ResetKind::External;
    case ESP_RST_DEEPSLEEP: return ResetKind::DeepSleep;
    case ESP_RST_PANIC: return ResetKind::Panic;
    case ESP_RST_TASK_WDT: return ResetKind::TaskWatchdog;
    case ESP_RST_INT_WDT: return ResetKind::IntWatchdog;
    case ESP_RST_WDT: return ResetKind::OtherWatchdog;
    case ESP_RST_CPU_LOCKUP: return ResetKind::OtherWatchdog;
    case ESP_RST_BROWNOUT: return ResetKind::Brownout;
    case ESP_RST_PWR_GLITCH: return ResetKind::Brownout;
    default: return ResetKind::Unknown;
  }
}

bool loadRecord(CrashRecord& out, bool& found) {
  found = false;
  nvs_handle_t h = 0;
  if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return true;  // no namespace yet: fresh
  char buf[160] = {};
  size_t len = sizeof(buf);
  const esp_err_t err = nvs_get_str(h, kRecordKey, buf, &len);
  nvs_close(h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return true;
  if (err != ESP_OK) return false;
  found = true;
  return decode_crash_record(buf, out);
}

bool storeRecord(const CrashRecord& r) {
  nvs_handle_t h = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_str(h, kRecordKey, encode_crash_record(r).c_str());
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

void readCoreDump() {
  if (esp_core_dump_image_check() != ESP_OK) return;  // none (or damaged)
  esp_core_dump_summary_t* sum = static_cast<esp_core_dump_summary_t*>(calloc(1, sizeof(esp_core_dump_summary_t)));
  if (sum == nullptr) return;
  char line[200];
  if (esp_core_dump_get_summary(sum) == ESP_OK) {
    char reason[64] = {};
    if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) != ESP_OK) strlcpy(reason, "?", sizeof(reason));
    int n = snprintf(line, sizeof(line), "task %.16s, PC 0x%08lx, reason \"%s\", backtrace", sum->exc_task,
                     (unsigned long)sum->exc_pc, reason);
    for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 6 && n > 0 && n < (int)sizeof(line) - 12; ++i) {
      n += snprintf(line + n, sizeof(line) - n, " 0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
    }
    s_coredump = line;
  } else {
    s_coredump = "present, summary unreadable";
  }
  free(sum);
  Serial.printf("[health] core dump in flash: %s\n", s_coredump.c_str());
#if !APP_DIAG
  // The release build: it may hold the helper token from RAM; the summary above is kept.
  const esp_err_t err = esp_core_dump_image_erase();
  Serial.printf("[health] core dump erased (release build): %s\n", esp_err_to_name(err));
#else
  Serial.println("[health] core dump kept (diagnostics build: espcoredump.py can read it)");
#endif
}

void printCounter(const char* prefix) {
  const CrashRecord& r = s_boot.verdict.record;
  Serial.printf("%s crashes counted %u of %d (window %d min of running) | safe mode %s", prefix,
                (unsigned)r.crashes.size(), CrashCounter::kSafeAfter, (int)(CrashCounter::kWindowMs / 60000),
                r.safe ? "ON" : "off");
  if (r.safe) {
    const int64_t left = CrashCounter::safe_duration_ms(r.episodes) - (r.axisMs + uptimeMs() - r.safeSinceMs);
    Serial.printf(" (episode %d, normal mode again in %lld min of running, or :safe-mode off)", r.episodes,
                  (long long)((left > 0 ? left : 0) + 59999) / 60000);
  } else if (r.episodes > 0) {
    Serial.printf(" (left safe mode; backoff %d until 10 min of stable running)", r.episodes);
  }
  Serial.println();
}

[[noreturn]] void restartNow(const char* why) {
  Serial.printf("[health] restarting: %s\n", why);
  Serial.flush();
  delay(100);
  esp_restart();
  for (;;) delay(1000);
}

}  // namespace

// ---- watchdog -------------------------------------------------------------------

void health_wdt_setup() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = kWdtTimeoutS * 1000;
  cfg.idle_core_mask = 1u << 0;  // as the prebuilt config: core 0's idle task (core 1's is not watched)
  cfg.trigger_panic = true;      // panic -> core dump -> reboot (reset reason "task watchdog")
  const esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  Serial.printf("[health] task watchdog %lu s, panic on, idle task core 0 watched: %s\n", (unsigned long)kWdtTimeoutS,
                esp_err_to_name(err));
}

void health_wdt_subscribe(const char* name) {
  const esp_err_t err = esp_task_wdt_add(nullptr);
  if (err != ESP_OK) Serial.printf("[health] WARNING: %s not watched (%s)\n", name, esp_err_to_name(err));
}

void health_wdt_feed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

void health_wdt_unsubscribe() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_delete(nullptr);
}

constexpr uint64_t kCoverFeedUs = 2000 * 1000;  // the cover timer feeds this often

WdtUser::~WdtUser() {
  if (timer_ != nullptr) {
    esp_timer_stop(static_cast<esp_timer_handle_t>(timer_));
    esp_timer_delete(static_cast<esp_timer_handle_t>(timer_));
  }
  if (user_ != nullptr) esp_task_wdt_delete_user(static_cast<esp_task_wdt_user_handle_t>(user_));
}

bool WdtUser::begin(const char* name) {
  esp_task_wdt_user_handle_t u = nullptr;
  const esp_err_t err = esp_task_wdt_add_user(name, &u);
  if (err != ESP_OK) {
    Serial.printf("[health] WARNING: %s not watched (%s)\n", name, esp_err_to_name(err));
    return false;
  }
  user_ = u;
  esp_timer_create_args_t args = {};
  args.callback = &WdtUser::on_timer;
  args.arg = this;
  args.name = "wdt_cover";
  esp_timer_handle_t t = nullptr;
  if (esp_timer_create(&args, &t) == ESP_OK && esp_timer_start_periodic(t, kCoverFeedUs) == ESP_OK) {
    timer_ = t;
  } else {
    Serial.printf("[health] WARNING: %s: no cover timer (a slow connect may reset the board)\n", name);
  }
  return true;
}

void WdtUser::feed() {
  if (user_ != nullptr) esp_task_wdt_reset_user(static_cast<esp_task_wdt_user_handle_t>(user_));
}

void WdtUser::cover(uint32_t maxMs) {
  feed();
  coverUntilUs_ = esp_timer_get_time() + static_cast<int64_t>(maxMs) * 1000;
}

void WdtUser::uncover() {
  coverUntilUs_ = 0;
  feed();
}

// esp_timer task, every 2 s: feeds only while a cover runs and has time left.
void WdtUser::on_timer(void* arg) {
  auto* self = static_cast<WdtUser*>(arg);
  const int64_t until = self->coverUntilUs_;
  if (until != 0 && esp_timer_get_time() < until) self->feed();
}

// ---- reset reason, crash counter, safe mode ------------------------------------

HealthBoot health_boot() {
  const esp_reset_reason_t raw = esp_reset_reason();
  s_boot.espReason = static_cast<int>(raw);
  s_boot.kind = kindOf(raw);
  std::optional<int64_t> previous;
  if (s_boot.kind != ResetKind::PowerOn && s_rtc.magic == kRtcMagic && s_rtc.check == rtcCheck(s_rtc.uptimeMs)) {
    previous = s_rtc.uptimeMs;
  }
  s_boot.previousRunMs = previous ? *previous : -1;
  s_rtc.uptimeMs = 0;
  s_rtc.check = rtcCheck(0);
  s_rtc.magic = kRtcMagic;

  CrashRecord stored;
  bool found = false;
  if (!loadRecord(stored, found)) {
    Serial.println("[health] WARNING: the crash record was unreadable: starting a fresh one");
    stored = CrashRecord();
  }
  s_boot.verdict = CrashCounter::on_boot(stored, s_boot.kind, previous);
  const BootVerdict& v = s_boot.verdict;
  if (!(v.record == stored) || !found) {
    if (!storeRecord(v.record)) Serial.println("[health] WARNING: could not store the crash record");
  }
  s_stored = v.record;

  Serial.printf("[health] reset reason: %s (esp_reset_reason %d)%s | previous run %s%lld s\n",
                reset_kind_name(s_boot.kind), s_boot.espReason, v.counted ? ", COUNTED as a crash" : "",
                previous ? "" : "unknown, taken as ", (long long)(previous ? *previous / 1000 : 0));
  if (s_boot.kind == ResetKind::Brownout) {
    Serial.println("[health] the last reset was a BROWNOUT: the supply voltage dipped (weak battery, cable or charger)");
  }
  printCounter("[health]");
  readCoreDump();

  if (v.enteredSafe) {
    Serial.printf("[health] SAFE MODE: %d abnormal resets within %d min of running: no data, no pictures, no update "
                  "checks; Settings and Wi-Fi work\n",
                  CrashCounter::kSafeAfter, (int)(CrashCounter::kWindowMs / 60000));
    std::string older;
    if (v.rollBack && ota_older_image_available(older)) {
      Serial.printf("[health] the running image %s is newer than %s in the other slot: rolling the update back\n",
                    kAppVersion, older.c_str());
      if (storeRecord(CrashCounter::fresh_record()) && ota_switch_to_older_image()) {
        restartNow("into the older image after repeated crashes");
      }
      Serial.println("[health] the rollback did not happen: staying in safe mode on this image");
      storeRecord(v.record);
    } else if (v.rollBack) {
      Serial.println("[health] no older valid image in the other slot: no rollback");
    }
  } else if (v.leftSafe) {
    Serial.println("[health] safe mode ended (its time had run): normal start");
  }
  return s_boot;
}

const HealthBoot& health_boot_info() { return s_boot; }

bool health_safe_mode() { return s_boot.verdict.safeMode; }

void health_tick() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (last != 0 && now - last < kTickMs) return;
  last = now;
  const int64_t up = uptimeMs();
  s_rtc.uptimeMs = static_cast<uint32_t>(up);
  s_rtc.check = rtcCheck(s_rtc.uptimeMs);
  if (s_cleared || s_leaving) return;
  switch (CrashCounter::on_uptime(s_stored, up)) {
    case CrashCounter::Tick::Nothing:
      break;
    case CrashCounter::Tick::ClearRecord:
      s_cleared = true;
      if (storeRecord(CrashCounter::fresh_record())) {
        s_boot.verdict.record = CrashCounter::fresh_record();
        Serial.printf("[health] %lld min of stable running: crash counter cleared\n", (long long)(up / 60000));
      } else {
        Serial.println("[health] WARNING: could not clear the crash record");
      }
      break;
    case CrashCounter::Tick::LeaveSafeMode:
      s_leaving = true;
      storeRecord(CrashCounter::left_safe(s_stored));
      restartNow("safe mode's time has run, back to normal mode");
  }
}

void health_safe_mode_off() {
  if (!s_boot.verdict.safeMode) {
    // Not in safe mode: still clear what is counted (harmless; the user asked).
    s_boot.verdict.record = CrashCounter::fresh_record();
    s_stored = s_boot.verdict.record;
    const bool ok = storeRecord(s_boot.verdict.record);
    Serial.printf("[cmd] safe-mode: not in safe mode; crash counter cleared%s\n", ok ? "" : " (NOT stored)");
    return;
  }
  if (!storeRecord(CrashCounter::left_safe(s_stored))) {
    Serial.println("[cmd] safe-mode off: could not store the record; staying in safe mode");
    return;
  }
  restartNow("safe mode off (:safe-mode off)");
}

void health_print_safe_mode() { printCounter("[cmd] safe-mode:"); }

void health_print_status() {
  const int64_t up = uptimeMs();
  Serial.printf("[status] up %lld s (%lldd %02lld:%02lld:%02lld) | reset reason: %s (%d) | previous run %s\n",
                (long long)(up / 1000), (long long)(up / 86400000), (long long)(up / 3600000 % 24),
                (long long)(up / 60000 % 60), (long long)(up / 1000 % 60), reset_kind_name(s_boot.kind),
                s_boot.espReason,
                s_boot.previousRunMs < 0 ? "unknown" : (std::to_string(s_boot.previousRunMs / 1000) + " s").c_str());
  printCounter("[status]");
  Serial.printf("[status] core dump at start: %s | task watchdog %lu s on lvgl, wifi_worker, http workers\n",
                s_coredump.empty() ? "none" : s_coredump.c_str(), (unsigned long)kWdtTimeoutS);
}

#if APP_DIAG
namespace {
void stallTask(void*) {
  health_wdt_subscribe("wdt_stall");
  Serial.printf("[diag] wdt_stall: subscribed, never feeding: expect a task watchdog reset in %lu s\n",
                (unsigned long)kWdtTimeoutS);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));  // blocks politely, never feeds
}
}  // namespace

void health_diag_wdt_stall() {
  if (xTaskCreatePinnedToCore(stallTask, "wdt_stall", 3072, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("[diag] wdt_stall: cannot create the task");
  }
}

void health_diag_panic() {
  Serial.println("[diag] panic: abort() now");
  Serial.flush();
  abort();
}
#endif
