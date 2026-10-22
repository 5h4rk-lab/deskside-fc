// What can make a sound, and the PCM clip a sound is. Pure data: shared with
// the ESP32 firmware, no clock reads, no SDL, no stdio.
#pragma once

#include <stdint.h>

#include <vector>

enum class SoundEvent {
  Goal,             // the favourite scored
  WinFanfare,       // the favourite won at full time
  KickOff,          // the favourite's match went live
  FullTime,         // the favourite's match ended
  BatteryLow,       // the 15 percent banner
  BatteryCritical,  // the 5 percent banner
  WifiOk,           // a Wi-Fi join succeeded
  Preview,          // "Test sound" in Settings (plays the Goal clip)
};

constexpr int kSoundEventCount = 8;

// Mono, signed 16-bit, kRate samples per second, at full level: the volume is
// applied by whoever plays it.
struct PcmClip {
  std::vector<int16_t> samples;
  static constexpr int kRate = 16000;

  int duration_ms() const { return static_cast<int>(samples.size() * 1000 / kRate); }
};
