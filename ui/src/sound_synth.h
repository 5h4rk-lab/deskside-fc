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

// The clip for `event`, made on first use (Preview is the Goal clip). A host
// that cannot afford the work in the middle of a goal calls sound_prepare_all()
// once at start.
const PcmClip& sound_clip(SoundEvent event);
void sound_prepare_all();

// The clip made afresh, uncached (tests and the WAV dump tool). Preview is
// the Goal clip.
PcmClip sound_generate(SoundEvent event);
