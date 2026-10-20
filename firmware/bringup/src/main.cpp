// Bring-up test program for the Waveshare ESP32-S3-Touch-LCD-2.8 (V1 / V2).
//
// Order: [sys] chip + PSRAM, [display] backlight + colour phases + test card,
// [touch] controller detection + I2C scans, [wifi] scan (+ optional HTTPS GET),
// then the BRINGUP RESULT line and an endless touch-test loop with a 5 s
// heartbeat. Every step is logged on the native USB serial port at 115200.
//
// Serial commands: 'r' replays the boot log, 'p' reruns the colour phases,
// 's' prints the result line again.
// BOOT button: press it when the test card looks right to mark display=ok.
#include <Arduino.h>
#include <esp_system.h>

#include "board_pins.h"
#include "bringup_log.h"
#include "display_st7789.h"
#include "touch.h"
#include "wifi_scan.h"

#ifndef BRINGUP_ENV_NAME
#define BRINGUP_ENV_NAME "unknown"
#endif

namespace {

constexpr uint32_t kSerialWaitMs = 3000;
constexpr uint32_t kPhaseMs = 1500;
constexpr uint32_t kHeartbeatMs = 5000;
constexpr uint32_t kResultRepeatMs = 60000;
constexpr uint32_t kTouchPollMs = 20;
constexpr uint32_t kTouchLogMinMs = 100;   // rate limit for touch lines while dragging
constexpr int kReleasePolls = 3;           // no-touch polls before we call it a release
constexpr int kMarkerSize = 12;
constexpr int kCornerSlack = 4;            // extra pixels around the 40 x 40 corner square

struct Result {
  bool displayStarted = false;
  bool displayConfirmed = false;  // set by the BOOT button
  touch::Controller touch = touch::Controller::None;
  int wifiNetworks = 0;
} g_result;

struct TouchState {
  bool pressed = false;
  int idlePolls = 0;
  bool markerShown = false;
  int markerX = 0;
  int markerY = 0;
  uint32_t lastPoll = 0;
  uint32_t lastLog = 0;
  uint32_t lastInt = 0;
  uint32_t lastErr = 0;
} g_touch;

// ---- [sys] ------------------------------------------------------------------

void holdPower() {
  pinMode(pins::kPowerHold, OUTPUT);
  digitalWrite(pins::kPowerHold, HIGH);
}

void startSerial() {
  Serial.setTxBufferSize(16384);  // keeps early lines until the PC opens the port
  Serial.setTxTimeoutMs(10);      // never stall long when nobody is reading
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < kSerialWaitMs) delay(10);
}

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic (crash)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_BROWNOUT: return "brown-out";
    case ESP_RST_USB: return "usb";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "external pin";
    default: return "other";
  }
}

bool testPsram() {
  constexpr size_t kBytes = 1024 * 1024;
  constexpr size_t kWords = kBytes / sizeof(uint32_t);
  uint32_t *buf = static_cast<uint32_t *>(ps_malloc(kBytes));
  if (buf == nullptr) {
    logf("[sys] psram test: ps_malloc(1 MB) FAILED");
    return false;
  }
  const uint32_t t0 = millis();
  for (size_t i = 0; i < kWords; ++i) buf[i] = uint32_t(i * 2654435761u) ^ 0xA5A55A5Au;
  size_t errors = 0;
  for (size_t i = 0; i < kWords; ++i) {
    if (buf[i] != (uint32_t(i * 2654435761u) ^ 0xA5A55A5Au)) ++errors;
  }
  const uint32_t ms = millis() - t0;
  free(buf);
  logf("[sys] psram test: 1 MB write/read-back %s (%u bad words, %lu ms)", errors ? "FAILED" : "OK",
       (unsigned)errors, (unsigned long)ms);
  return errors == 0;
}

void logSystem() {
  logf("[sys] ---- Waveshare ESP32-S3-Touch-LCD-2.8 bring-up ----");
  logf("[sys] env=%s spi_hz=%lu build=%s %s", BRINGUP_ENV_NAME, (unsigned long)display::spiHz(), __DATE__,
       __TIME__);
  logf("[sys] reset reason: %s", resetReasonName(esp_reset_reason()));
  logf("[sys] chip=%s rev=%u cores=%u cpu=%lu MHz", ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
       (unsigned)ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
  logf("[sys] flash=%lu bytes at %lu Hz", (unsigned long)ESP.getFlashChipSize(),
       (unsigned long)ESP.getFlashChipSpeed());
  logf("[sys] psram=%lu bytes (free %lu), heap free=%lu", (unsigned long)ESP.getPsramSize(),
       (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getFreeHeap());
  logf("[sys] power hold GPIO%d driven HIGH (needed on battery)", pins::kPowerHold);
  testPsram();
}

// ---- [display] ----------------------------------------------------------------

struct Phase {
  uint16_t color;
  const char *expect;
};

const Phase kPhases[] = {
    {display::kRed, "solid RED"},     {display::kGreen, "solid GREEN"}, {display::kBlue, "solid BLUE"},
    {display::kWhite, "solid WHITE"}, {display::kBlack, "solid BLACK"},
};

void showTestCard() {
  logf("[display] expect: TEST CARD, portrait, USB-C at the bottom edge:");
  logf("[display] expect:   top third 8 columns L->R red green blue yellow cyan magenta white black");
  logf("[display] expect:   middle third gray ramp, black at left -> white at right");
  logf("[display] expect:   bottom third black panel, white 1 px frame, diagonal top-left -> bottom-right,");
  logf("[display] expect:   white 40x40 square in the panel's top-left corner (touch it to rerun phases)");
  if (g_result.displayStarted) display::drawTestCard();
}

void runDisplayPhases() {
  for (const Phase &p : kPhases) {
    logf("[display] expect: %s", p.expect);
    if (g_result.displayStarted) display::fill(p.color);
    delay(kPhaseMs);
  }
  showTestCard();
  g_touch.markerShown = false;
}

// ---- result line ----------------------------------------------------------------

void printResult() {
  logf("BRINGUP RESULT display=%s touch=%s wifi=%d networks", g_result.displayConfirmed ? "ok" : "?",
       touch::name(g_result.touch), g_result.wifiNetworks);
}

// ---- touch test mode ---------------------------------------------------------------

void moveMarker(int x, int y) {
  if (!g_result.displayStarted) return;
  if (g_touch.markerShown) {
    display::restoreTestCardRect(g_touch.markerX - kMarkerSize / 2, g_touch.markerY - kMarkerSize / 2,
                                 kMarkerSize, kMarkerSize);
  }
  display::drawTouchMarker(x, y, kMarkerSize);
  g_touch.markerShown = true;
  g_touch.markerX = x;
  g_touch.markerY = y;
}

void pollTouch() {
  if (g_result.touch == touch::Controller::None) return;
  const uint32_t now = millis();
  if (now - g_touch.lastPoll < kTouchPollMs) return;
  g_touch.lastPoll = now;

  touch::Point p{};
  const int n = touch::read(&p);
  if (n <= 0) {
    if (g_touch.pressed && ++g_touch.idlePolls >= kReleasePolls) {
      g_touch.pressed = false;
      logf("[touch] release");
    }
    return;
  }
  g_touch.idlePolls = 0;
  const bool newPress = !g_touch.pressed;
  const bool moved = p.x != g_touch.markerX || p.y != g_touch.markerY;
  g_touch.pressed = true;

  if (newPress || (moved && now - g_touch.lastLog >= kTouchLogMinMs)) {
    logf("[touch] raw x=%u y=%u n=%d", p.x, p.y, n);
    g_touch.lastLog = now;
  }
  if (newPress && display::inCornerSquare(p.x, p.y, kCornerSlack)) {
    logf("[touch] corner square touched: rerunning colour phases");
    runDisplayPhases();
    g_touch.pressed = false;
    return;
  }
  if (newPress || moved) moveMarker(p.x, p.y);
}

void logTouchStats() {
  if (g_result.touch == touch::Controller::None) return;
  const uint32_t ints = touch::interruptCount();
  const uint32_t errs = touch::i2cErrorCount();
  if (ints != g_touch.lastInt || errs != g_touch.lastErr) {
    logf("[touch] stats int_edges=%lu i2c_errors=%lu", (unsigned long)ints, (unsigned long)errs);
    g_touch.lastInt = ints;
    g_touch.lastErr = errs;
  }
}

// ---- operator input -------------------------------------------------------------------

void handleBootButton() {
  static bool wasDown = false;
  static uint32_t downSince = 0;
  const bool down = digitalRead(pins::kBootButton) == LOW;
  if (down && !wasDown) downSince = millis();
  if (!down && wasDown && millis() - downSince > 30) {
    g_result.displayConfirmed = true;
    logf("[display] BOOT pressed: test card confirmed by the user");
    printResult();
  }
  wasDown = down;
}

void handleSerialInput() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'r': bringupLogReplay(); break;
      case 'p': runDisplayPhases(); break;
      case 's': printResult(); break;
      default: break;
    }
  }
}

}  // namespace

void setup() {
  holdPower();  // first: on battery the board switches off unless GPIO7 is high
  startSerial();
  bringupLogBegin();
  logSystem();

  display::backlightBegin();
  g_result.displayStarted = display::begin();
  runDisplayPhases();

  g_result.touch = touch::begin();
  touch::scanMainBus();

  g_result.wifiNetworks = wifi_test::run();

  pinMode(pins::kBootButton, INPUT_PULLUP);
  printResult();
  logf("[sys] touch-test mode. Press BOOT if the test card looks right. Serial: r=replay p=phases s=result");
  bringupLogFreeze();
}

void loop() {
  static uint32_t lastBeat = 0;
  static uint32_t lastResult = 0;
  const uint32_t now = millis();

  pollTouch();
  handleBootButton();
  handleSerialInput();

  if (now - lastBeat >= kHeartbeatMs) {
    lastBeat = now;
    logf("[sys] alive %lu s heap=%lu psram=%lu", (unsigned long)(now / 1000), (unsigned long)ESP.getFreeHeap(),
         (unsigned long)ESP.getFreePsram());
    logTouchStats();
  }
  if (now - lastResult >= kResultRepeatMs) {
    lastResult = now;
    printResult();
  }
  delay(2);
}
