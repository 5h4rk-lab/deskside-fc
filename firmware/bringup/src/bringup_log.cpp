#include "bringup_log.h"

#include <Arduino.h>
#include <stdarg.h>

namespace {

constexpr size_t kBootLogBytes = 32 * 1024;
char *s_bootLog = nullptr;  // PSRAM if available, else internal heap
size_t s_bootLogLen = 0;
bool s_recording = true;

void record(const char *line, size_t len) {
  if (!s_recording || s_bootLog == nullptr) return;
  if (s_bootLogLen + len + 2 > kBootLogBytes) return;  // full: keep the start
  memcpy(s_bootLog + s_bootLogLen, line, len);
  s_bootLogLen += len;
  s_bootLog[s_bootLogLen++] = '\n';
  s_bootLog[s_bootLogLen] = '\0';
}

}  // namespace

void bringupLogBegin() {
  s_bootLog = static_cast<char *>(ps_malloc(kBootLogBytes));
  if (s_bootLog == nullptr) s_bootLog = static_cast<char *>(malloc(kBootLogBytes));
  if (s_bootLog != nullptr) s_bootLog[0] = '\0';
}

void logf(const char *fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (n < 0) return;
  size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
  Serial.println(line);
  record(line, len);
}

void bringupLogFreeze() { s_recording = false; }

void bringupLogReplay() {
  Serial.println("[sys] ---- replay of the boot log ----");
  if (s_bootLog != nullptr) Serial.print(s_bootLog);
  Serial.println("[sys] ---- end of replay ----");
}
