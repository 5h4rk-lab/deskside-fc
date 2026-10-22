// The sounds, synthesized in code (no audio files): 16 kHz mono PCM.
// Deterministic: the same bytes on every call and every run (the crowd noise
// comes from a fixed-seed integer generator). Shared with the ESP32 firmware.
#pragma once

#include "sound_event.h"

// Peak of every clip: 90 percent of full scale at most. Each clip has a
// 10 ms fade in and a 30 ms fade out, so it starts and ends without a click.
constexpr int kSoundPeakMax = 29491;
constexpr int kSoundFadeInMs = 10;
constexpr int kSoundFadeOutMs = 30;

// Clip lengths, chosen so that the whole set stays under 100000 samples
// (200 KB): 36000 + 22400 + 8000 + 16800 + 5280 + 7040 + 3200 = 98720.
constexpr int kSoundGoalMs = 2250;
constexpr int kSoundFanfareMs = 1400;
constexpr int kSoundKickOffMs = 500;
constexpr int kSoundFullTimeMs = 1050;
constexpr int kSoundBatteryLowMs = 330;
constexpr int kSoundBatteryCriticalMs = 440;
constexpr int kSoundWifiOkMs = 200;

// How long the clip for `event` lasts, without making it (the gate and the
// app's timers use this, so they never start a synthesis).
constexpr int sound_clip_duration_ms(SoundEvent event) {
  switch (event) {
    case SoundEvent::Goal:
    case SoundEvent::Preview: return kSoundGoalMs;
    case SoundEvent::WinFanfare: return kSoundFanfareMs;
    case SoundEvent::KickOff: return kSoundKickOffMs;
    case SoundEvent::FullTime: return kSoundFullTimeMs;
    case SoundEvent::BatteryLow: return kSoundBatteryLowMs;
    case SoundEvent::BatteryCritical: return kSoundBatteryCriticalMs;
    case SoundEvent::WifiOk: return kSoundWifiOkMs;
  }
  return 0;
}

// The clip for `event`, made on first use (Preview is the Goal clip). A host
// that cannot afford the work in the middle of a goal calls sound_prepare_all()
// once at start.
const PcmClip& sound_clip(SoundEvent event);
void sound_prepare_all();

// The clip made afresh, uncached (tests and the WAV dump tool). Preview is
// the Goal clip.
PcmClip sound_generate(SoundEvent event);

// A plain sine of `hz` for `ms` with 10 ms raised-cosine edges, peaking at
// `peak` (the speaker's left/right test). Uncached.
PcmClip sound_test_tone(int hz, int ms, int peak);

// Counters for tests and the board's boot log: sound_clip() calls, and clips
// actually synthesized (sound_generate() calls, cached or not). Any thread.
int sound_clip_lookups();
int sound_generate_calls();
