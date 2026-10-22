// Writes every sound to a 16 kHz mono 16-bit WAV file, for checking levels
// and spectrograms off the board: dump_wav OUTDIR (default: the current
// directory). Not part of the simulator or the firmware.
#include <stdint.h>
#include <stdio.h>

#include <string>

#include "sound_synth.h"

namespace {

void put32(FILE* f, uint32_t v) {
  for (int i = 0; i < 4; ++i) fputc(static_cast<int>((v >> (8 * i)) & 0xFF), f);
}
void put16(FILE* f, uint16_t v) {
  fputc(v & 0xFF, f);
  fputc(v >> 8, f);
}

bool write_wav(const std::string& path, const PcmClip& clip) {
  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const uint32_t bytes = static_cast<uint32_t>(clip.samples.size() * 2);
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);  // PCM
  put16(f, 1);  // mono
  put32(f, PcmClip::kRate);
  put32(f, PcmClip::kRate * 2);
  put16(f, 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (int16_t s : clip.samples) put16(f, static_cast<uint16_t>(s));
  return fclose(f) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? std::string(argv[1]) + "/" : "";
  const struct {
    SoundEvent event;
    const char* name;
  } sounds[] = {{SoundEvent::Goal, "goal"},
                {SoundEvent::WinFanfare, "win-fanfare"},
                {SoundEvent::KickOff, "kick-off"},
                {SoundEvent::FullTime, "full-time"},
                {SoundEvent::BatteryLow, "battery-low"},
                {SoundEvent::BatteryCritical, "battery-critical"},
                {SoundEvent::WifiOk, "wifi-ok"}};
  for (const auto& s : sounds) {
    const std::string path = dir + s.name + ".wav";
    if (!write_wav(path, sound_clip(s.event))) {
      fprintf(stderr, "could not write %s\n", path.c_str());
      return 1;
    }
    printf("%s (%d ms)\n", path.c_str(), sound_clip(s.event).duration_ms());
  }
  return 0;
}
