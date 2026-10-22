#include "sound_policy.h"

#include "sound_synth.h"

namespace {

// Starts a clip over whatever plays, and is not cut short by a lesser event.
bool loud(SoundEvent e) {
  return e == SoundEvent::Goal || e == SoundEvent::WinFanfare || e == SoundEvent::Preview;
}

bool match_alert(SoundEvent e) { return e == SoundEvent::KickOff || e == SoundEvent::FullTime; }

}  // namespace

bool sound_should_play(const Settings& settings, SoundEvent event, bool nightDimActive) {
  if (event == SoundEvent::Preview) return true;
  if (settings.soundMode == SoundMode::Off) return false;
  if (settings.soundMuteAtNight && nightDimActive) return false;
  if (match_alert(event)) return settings.soundMode == SoundMode::GoalsAndAlerts;
  return true;
}

bool SoundGate::allow(SoundEvent event, int64_t nowMs) {
  const int i = static_cast<int>(event);
  if (event != SoundEvent::Preview && seen_[i] && nowMs - at_[i] < kSoundRepeatMs) return false;
  if (!loud(event) && playingLoud_ && nowMs < playingUntil_) return false;
  seen_[i] = true;
  at_[i] = nowMs;
  playingLoud_ = loud(event);
  playingUntil_ = nowMs + sound_clip(event).duration_ms();
  return true;
}
