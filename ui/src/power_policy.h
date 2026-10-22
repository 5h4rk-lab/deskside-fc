// The power key and switching off, as pure logic (no clock reads, no GPIO):
// the firmware samples the key and passes the time in; the simulator tests
// every timing. Shared with the ESP32 firmware.
//
// PowerKeyPolicy turns raw key samples into events. PowerController uses it
// and drives the host through PowerHost: a short press turns the screen off
// or on, a hold shows "Hold to power off" and after 3 s switches off in a
// fixed order, then (still powered: on USB, or the key still held) waits in
// the "off" state for a press that wakes the board.
#pragma once

#include <stdint.h>

namespace power_key {

constexpr int64_t kDebounceMs = 30;        // a change must hold this long to count
constexpr int64_t kShortPressMaxMs = 600;  // released before this: a short press
constexpr int64_t kHintAfterMs = 1000;     // held this long: "Hold to power off"
constexpr int64_t kLongPressMs = 3000;     // held this long: switch off
constexpr int64_t kBootIgnoreMs = 2000;    // presses that begin this soon after boot are ignored
constexpr int64_t kPollMs = 10;            // how often the host samples the key while on
constexpr int64_t kOffPollMs = 10000;      // in the off state, with the key up
constexpr uint32_t kSoundIdleWaitMs = 300;   // wait for the speaker to fall silent
constexpr int64_t kPoweringOffShowMs = 1500;  // the "Powering off" screen before the power is cut

constexpr const char* kHoldHintText = "Hold to power off";
constexpr const char* kPoweringOffText = "Powering off";

}  // namespace power_key

enum class PowerKeyEvent { None, ShortPress, LongPressArmed, LongPress };

// Debounced key with press timing. update() takes every sample (pressed =
// true while the key is down) with a monotonic time in ms and returns at most
// one event per call:
// - ShortPress when a press shorter than kShortPressMaxMs is released;
// - LongPressArmed once when a press is seen down kHintAfterMs after it began
//   (the hint shows from then until the release or the LongPress);
// - LongPress once when it is seen down kLongPressMs after it began (nothing
//   more until it is released; a press from kShortPressMaxMs up to there
//   does nothing else).
// Durations run from the first sample of the change (bounces shorter than
// kDebounceMs are ignored). A press that begins before bootMs +
// kBootIgnoreMs, or is already down at the first sample (the key that
// switched the board on), is ignored until it has been released.
class PowerKeyPolicy {
 public:
  explicit PowerKeyPolicy(int64_t bootMs = 0) : bootMs_(bootMs) {}
  PowerKeyEvent update(bool pressed, int64_t nowMs);

  bool pressed() const { return down_; }           // debounced
  bool settling() const { return raw_ != down_; }  // a change is waiting for its debounce
  bool hint_visible() const { return hint_; }
  // Forget the current press: nothing more comes from it, and a key that is
  // down now must be released before it counts again.
  void require_release();

 private:
  int64_t bootMs_;
  bool started_ = false;
  bool raw_ = false;
  int64_t rawSince_ = 0;
  bool down_ = false;
  int64_t downAt_ = 0;
  bool ignored_ = false;  // this press does not count
  bool ignoreNext_ = false;  // require_release() while a press was still settling
  bool armed_ = false;    // LongPressArmed given for this press
  bool long_ = false;     // LongPress given for this press
  bool hint_ = false;
};

// What PowerController asks of the host. Switching off calls, in this order:
// stop_sound, wait_sound_idle(300), show_powering_off, (1.5 s later)
// save_prefs, stop_wifi, cut_power, enter_off_state.
class PowerHost {
 public:
  virtual ~PowerHost() = default;
  // A short press: screen off (backlight 0, touch still wakes it) or back on.
  virtual void toggle_screen() = 0;
  // Before the hint shows: light the screen if it is off (does nothing if on).
  virtual void screen_on() = 0;
  virtual void show_hold_hint(bool show) = 0;
  virtual void stop_sound() = 0;
  virtual void wait_sound_idle(uint32_t timeoutMs) = 0;
  virtual void show_powering_off() = 0;
  virtual void save_prefs() = 0;
  virtual void stop_wifi() = 0;
  // Power hold low (GPIO7): on battery the board goes dark once the key is up.
  virtual void cut_power() = 0;
  // Still running (USB keeps the board powered): backlight 0, nothing drawn.
  virtual void enter_off_state() = 0;
  // A press in the off state: start again.
  virtual void wake_from_off() = 0;
};

class PowerController {
 public:
  enum class Mode { On, PoweringOff, Off };

  PowerController(PowerHost& host, int64_t bootMs) : host_(host), key_(bootMs) {}

  // Every sample of the key (see poll_interval_ms).
  void update(bool pressed, int64_t nowMs);
  // Starts switching off now (what a long press does; also a diagnostics
  // command). Ignored unless the mode is On.
  void power_off();

  Mode mode() const { return mode_; }
  // How soon the host should sample the key again: kPollMs, or kOffPollMs in
  // the off state while the key is up and settled (the host also samples at
  // once when the key's level changes).
  int64_t poll_interval_ms() const;
  bool hint_visible() const { return key_.hint_visible(); }

 private:
  void finish_power_off();
  // The key in PoweringOff and Off (debounced); true for a press that follows a release.
  bool off_key(bool pressed, int64_t nowMs);

  PowerHost& host_;
  PowerKeyPolicy key_;
  Mode mode_ = Mode::On;
  bool hintShown_ = false;
  bool shownAtKnown_ = false;  // the Powering off screen's 1.5 s have started
  int64_t shownAt_ = 0;
  // Off state: the key, debounced, and whether it has been up since.
  bool offRaw_ = false;
  int64_t offRawSince_ = 0;
  bool offDown_ = false;
  bool offReleased_ = false;
  bool offStarted_ = false;
};
