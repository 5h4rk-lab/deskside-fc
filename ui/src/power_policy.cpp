#include "power_policy.h"

using namespace power_key;

// ---------------------------------------------------------------- PowerKeyPolicy

PowerKeyEvent PowerKeyPolicy::update(bool pressed, int64_t now) {
  if (!started_) {
    // The first sample: a key already down is the one that switched the board on.
    started_ = true;
    raw_ = pressed;
    rawSince_ = now;
    down_ = pressed;
    downAt_ = now;
    ignored_ = pressed;
    return PowerKeyEvent::None;
  }
  if (pressed != raw_) {
    raw_ = pressed;
    rawSince_ = now;
  }
  if (raw_ != down_ && now - rawSince_ >= kDebounceMs) {
    down_ = raw_;
    if (down_) {
      downAt_ = rawSince_;
      ignored_ = ignoreNext_ || downAt_ < bootMs_ + kBootIgnoreMs;
      ignoreNext_ = false;
      armed_ = false;
      long_ = false;
    } else {
      const int64_t held = rawSince_ - downAt_;
      const bool counted = !ignored_ && !armed_ && !long_;
      hint_ = false;
      ignored_ = false;
      return counted && held < kShortPressMaxMs ? PowerKeyEvent::ShortPress : PowerKeyEvent::None;
    }
  }
  if (down_ && raw_ && !ignored_) {
    // Only while the key is still seen down: a release that is settling
    // neither arms nor fires (it would be a release just before the mark).
    const int64_t held = now - downAt_;
    if (!armed_ && held >= kHintAfterMs) {
      armed_ = true;
      hint_ = true;
      return PowerKeyEvent::LongPressArmed;
    }
    if (armed_ && !long_ && held >= kLongPressMs) {
      long_ = true;
      hint_ = false;
      return PowerKeyEvent::LongPress;
    }
  }
  return PowerKeyEvent::None;
}

void PowerKeyPolicy::require_release() {
  hint_ = false;
  if (down_) ignored_ = true;
  if (raw_ && !down_) ignoreNext_ = true;
}

// ---------------------------------------------------------------- PowerController

void PowerController::update(bool pressed, int64_t now) {
  switch (mode_) {
    case Mode::On: {
      const PowerKeyEvent e = key_.update(pressed, now);
      if (e == PowerKeyEvent::ShortPress) {
        host_.toggle_screen();
      } else if (e == PowerKeyEvent::LongPressArmed) {
        litForHint_ = host_.screen_on();
        host_.show_hold_hint(true);
        hintShown_ = true;
      } else if (e == PowerKeyEvent::LongPress) {
        if (hintShown_) host_.show_hold_hint(false);
        hintShown_ = false;
        litForHint_ = false;
        power_off();
        off_key(pressed, now);
        return;
      }
      if (hintShown_ && !key_.hint_visible()) {
        // Released before 3 s: the hint goes, and a screen it lit goes dark again.
        host_.show_hold_hint(false);
        hintShown_ = false;
        if (litForHint_) host_.screen_off();
        litForHint_ = false;
      }
      return;
    }
    case Mode::PoweringOff:
      off_key(pressed, now);
      // The screen's time starts at the first sample after it was asked for:
      // waiting for the speaker may have taken a while.
      if (!shownAtKnown_) {
        shownAtKnown_ = true;
        shownAt_ = now;
      }
      if (now - shownAt_ >= kPoweringOffShowMs) finish_power_off();
      return;
    case Mode::Off:
      if (off_key(pressed, now)) host_.wake_from_off();
      return;
  }
}

bool PowerController::off_key(bool pressed, int64_t now) {
  if (!offStarted_) {
    offStarted_ = true;
    offRaw_ = pressed;
    offRawSince_ = now;
    offDown_ = pressed;
    offReleased_ = !pressed;  // the key of the long press must come up first
    return false;
  }
  if (pressed != offRaw_) {
    offRaw_ = pressed;
    offRawSince_ = now;
  }
  if (offRaw_ == offDown_ || now - offRawSince_ < kDebounceMs) return false;
  offDown_ = offRaw_;
  if (!offDown_) {
    offReleased_ = true;
    return false;
  }
  if (!offReleased_ || mode_ != Mode::Off) return false;
  offReleased_ = false;  // one wake per press
  return true;
}

void PowerController::power_off() {
  if (mode_ != Mode::On) return;
  mode_ = Mode::PoweringOff;
  key_.require_release();
  host_.stop_sound();
  host_.wait_sound_idle(kSoundIdleWaitMs);
  host_.show_powering_off();
}

void PowerController::finish_power_off() {
  host_.save_prefs();
  host_.stop_wifi();
  host_.cut_power();
  mode_ = Mode::Off;
  host_.enter_off_state();
}

int64_t PowerController::poll_interval_ms() const {
  if (mode_ == Mode::Off && !offRaw_ && !offDown_) return kOffPollMs;
  return kPollMs;
}
