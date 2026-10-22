#include "sound_voice.h"

namespace {

// (p / 100)^2 * kSoundFullGainQ15, rounded, for p = 0..100.
struct GainTable {
  uint16_t q15[101];
  constexpr GainTable() : q15() {
    for (int p = 0; p <= 100; ++p) {
      q15[p] = static_cast<uint16_t>((static_cast<uint32_t>(p * p) * kSoundFullGainQ15 + 5000) / 10000);
    }
  }
};

constexpr GainTable kGain;
static_assert(kGain.q15[100] == kSoundFullGainQ15, "100 percent is the full gain");
static_assert(kGain.q15[0] == 0, "0 percent is silence");

constexpr int32_t kUnity = 32768;  // envelope 1.0 in Q15

constexpr int32_t step_for(int frames) { return (kUnity + frames - 1) / frames; }

}  // namespace

uint16_t sound_gain_q15(int percent) {
  if (percent <= 0) return 0;
  if (percent >= 100) return kGain.q15[100];
  return kGain.q15[percent];
}

void SoundVoice::start(const int16_t* samples, size_t n, uint16_t gainQ15, SoundChannels channels) {
  if (samples == nullptr || n == 0) return;
  Clip c;
  c.samples = samples;
  c.n = n;
  c.gain = gainQ15;
  c.channels = channels;
  switch (state_) {
    case State::Idle:
      cur_ = c;
      hasPending_ = false;
      state_ = State::PreRoll;
      count_ = kPreRollFrames;
      break;
    case State::PreRoll:
      cur_ = c;  // nothing audible yet: the pre-roll goes on
      break;
    case State::Tail:
      begin(c);
      break;
    case State::Playing:
    case State::Fading:
      pending_ = c;
      hasPending_ = true;
      if (state_ != State::Fading || step_ < step_for(kSwitchFadeFrames)) step_ = step_for(kSwitchFadeFrames);
      state_ = State::Fading;
      break;
  }
}

void SoundVoice::stop() {
  hasPending_ = false;
  switch (state_) {
    case State::Idle:
    case State::Tail:
      break;
    case State::PreRoll:
      // Only silence has gone out: nothing to fade, nothing to flush.
      state_ = State::Idle;
      break;
    case State::Playing:
      state_ = State::Fading;
      step_ = step_for(kStopFadeFrames);
      break;
    case State::Fading:
      if (step_ < step_for(kStopFadeFrames)) step_ = step_for(kStopFadeFrames);
      break;
  }
}

void SoundVoice::begin(const Clip& c) {
  cur_ = c;
  pos_ = 0;
  env_ = 0;
  state_ = State::Playing;
}

int32_t SoundVoice::next_sample() {
  if (pos_ >= cur_.n) return 0;
  const int32_t s = cur_.samples[pos_++];
  return (s * static_cast<int32_t>(cur_.gain)) >> 15;
}

void SoundVoice::render(int16_t* out, size_t frames) {
  for (size_t i = 0; i < frames; ++i) {
    int32_t v = 0;
    switch (state_) {
      case State::Idle:
        break;
      case State::PreRoll:
        if (--count_ <= 0) begin(cur_);
        break;
      case State::Playing: {
        env_ += step_for(kRampFrames);
        if (env_ > kUnity) env_ = kUnity;
        // The last kRampFrames fade out too, whatever the clip does.
        const size_t left = cur_.n - pos_;  // this sample included
        const int32_t endCap = left <= static_cast<size_t>(kRampFrames)
                                   ? static_cast<int32_t>(left - 1) * step_for(kRampFrames)
                                   : kUnity;
        const int32_t env = env_ < endCap ? env_ : endCap;
        v = (next_sample() * env) >> 15;
        if (pos_ >= cur_.n) {
          state_ = State::Tail;
          count_ = tailFrames_;
        }
        break;
      }
      case State::Fading: {
        env_ -= step_;
        const int32_t s = next_sample();
        if (env_ <= 0 || pos_ >= cur_.n) {
          env_ = 0;
          if (hasPending_) {
            hasPending_ = false;
            begin(pending_);
          } else {
            state_ = State::Tail;
            count_ = tailFrames_;
          }
        } else {
          v = (s * env_) >> 15;
        }
        break;
      }
      case State::Tail:
        if (--count_ <= 0) state_ = State::Idle;
        break;
    }
    const int16_t s16 = static_cast<int16_t>(v);
    const uint8_t ch = static_cast<uint8_t>(cur_.channels);
    out[i * 2] = (ch & static_cast<uint8_t>(SoundChannels::Left)) ? s16 : 0;
    out[i * 2 + 1] = (ch & static_cast<uint8_t>(SoundChannels::Right)) ? s16 : 0;
  }
}
