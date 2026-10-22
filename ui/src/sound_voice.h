// The speaker's mixing logic, pure (no clock, no RTOS, no I2S): the volume
// curve, and one voice that turns a mono clip into 16-bit stereo frames with
// a pre-roll of silence, short ramps, a 20 ms stop fade, a quick cross-over
// to a clip that replaces the one playing, and a silent tail after the end.
// The board's I2S player (firmware/app/src/platform/i2s_sound_out.cpp) runs
// it on its own task; the simulator tests it. Shared with the ESP32 firmware.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Gain at 100 percent volume, Q15 (32767 = 0 dB). Lower it here if the
// loudest setting distorts on the real speaker.
constexpr uint16_t kSoundFullGainQ15 = 32767;

// The volume curve of SoundOut::play: gain = (percent / 100)^2 of
// kSoundFullGainQ15, rounded (20 % = -28 dB, 40 % = -16 dB, 100 % = 0 dB).
// Percent is clamped to 0..100. A table lookup.
uint16_t sound_gain_q15(int percent);

// Which output channels a voice drives. Normal clips use Both (the mono clip
// is copied to left and right); Left and Right exist for the speaker test.
enum class SoundChannels : uint8_t { Left = 1, Right = 2, Both = 3 };

class SoundVoice {
 public:
  static constexpr int kRate = 16000;
  static constexpr int kPreRollFrames = 480;    // 30 ms of silence after the channel starts (the DAC locks on)
  static constexpr int kRampFrames = 32;        // 2 ms linear fade in at every start
  static constexpr int kStopFadeFrames = 320;   // 20 ms linear fade out on stop()
  static constexpr int kSwitchFadeFrames = 160; // 10 ms fade of a clip that a new one replaces

  // `tailFrames`: silence rendered after a clip ends (or after a stop fade)
  // before the voice reports idle. The host sets it to at least its DMA
  // queue length, so everything audible has left the DAC before it stops
  // the clock.
  explicit SoundVoice(int tailFrames) : tailFrames_(tailFrames > 0 ? tailFrames : 1) {}

  // Plays `n` mono samples (kept by the caller until the voice is idle or
  // another clip replaced it) at `gainQ15` on `channels`. From idle: after
  // the pre-roll. During the pre-roll: replaces the clip, the pre-roll goes
  // on. While a clip plays: that one fades over kSwitchFadeFrames, then this
  // one starts. During the tail: at once. An empty clip changes nothing.
  void start(const int16_t* samples, size_t n, uint16_t gainQ15, SoundChannels channels);
  // Fades out over kStopFadeFrames, then the tail; drops a clip waiting to
  // replace the current one. Nothing to do when idle.
  void stop();

  // Writes `frames` stereo frames (left, right interleaved) to `out`.
  void render(int16_t* out, size_t frames);

  // Nothing left to render: the host may stop its clock.
  bool idle() const { return state_ == State::Idle; }

 private:
  enum class State : uint8_t { Idle, PreRoll, Playing, Fading, Tail };

  struct Clip {
    const int16_t* samples = nullptr;
    size_t n = 0;
    uint16_t gain = 0;
    SoundChannels channels = SoundChannels::Both;
  };

  void begin(const Clip& c);  // starts `c` at its first sample with the ramp
  void clamp_to_end_ramp();   // env_ = the level last heard, when inside the end ramp
  int32_t next_sample();      // the current clip's next sample after gain, 0 past its end

  int tailFrames_;
  State state_ = State::Idle;
  Clip cur_;
  Clip pending_;
  bool hasPending_ = false;
  size_t pos_ = 0;
  int32_t env_ = 0;   // Q15, 32768 = 1
  int32_t step_ = 0;  // fade-out step per frame while Fading
  int count_ = 0;     // frames left of the pre-roll or the tail
};
