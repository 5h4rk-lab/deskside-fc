#include "sound_synth.h"

#include <math.h>

#include <algorithm>

// Everything is built in float at 16 kHz, then faded, scaled to its target
// peak and rounded. Only sinf/expf/cosf and integer noise: the same code
// gives the same sounds on the PC and on the board (to the last bit on one
// machine, which is what the tests pin).
namespace {

constexpr float kRate = static_cast<float>(PcmClip::kRate);
constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.28318531f;

// Clip lengths, chosen so that the whole set stays under 100000 samples
// (200 KB): 36000 + 22400 + 8000 + 16800 + 5280 + 7040 + 3200 = 98720.
constexpr int kGoalMs = 2250;
constexpr int kFanfareMs = 1400;
constexpr int kKickOffMs = 500;
constexpr int kFullTimeMs = 1050;

using Buf = std::vector<float>;

size_t samples_for(float ms) { return static_cast<size_t>(ms * kRate / 1000.0f + 0.5f); }

// xorshift32 with a fixed seed: the crowd and the breath are the same every time.
class Rng {
 public:
  explicit Rng(uint32_t seed) : s_(seed) {}
  float next() {  // -1 .. 1
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return static_cast<float>(static_cast<int32_t>(s_)) / 2147483648.0f;
  }

 private:
  uint32_t s_;
};

// 0 .. 1 over x = 0 .. 1, flat at both ends.
float smooth(float x) {
  if (x <= 0.0f) return 0.0f;
  if (x >= 1.0f) return 1.0f;
  return 0.5f - 0.5f * cosf(kPi * x);
}

// RBJ biquads.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;

  void lowpass(float fc, float q) {
    const float w = kTwoPi * fc / kRate;
    const float alpha = sinf(w) / (2.0f * q);
    const float c = cosf(w);
    const float a0 = 1.0f + alpha;
    b0 = (1.0f - c) * 0.5f / a0;
    b1 = (1.0f - c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
  }
  void bandpass(float fc, float q) {  // 0 dB at the centre
    const float w = kTwoPi * fc / kRate;
    const float alpha = sinf(w) / (2.0f * q);
    const float c = cosf(w);
    const float a0 = 1.0f + alpha;
    b0 = alpha / a0;
    b1 = 0.0f;
    b2 = -alpha / a0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
  }
  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

float polyblep(float t, float dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

// Band-limited saw and square from one phase.
struct Osc {
  float phase = 0.0f;
  float saw = 0.0f;
  float square = 0.0f;

  void tick(float hz) {
    const float dt = hz / kRate;
    saw = 2.0f * phase - 1.0f - polyblep(phase, dt);
    const float half = phase + 0.5f >= 1.0f ? phase - 0.5f : phase + 0.5f;
    square = (phase < 0.5f ? 1.0f : -1.0f) + polyblep(phase, dt) - polyblep(half, dt);
    phase += dt;
    if (phase >= 1.0f) phase -= 1.0f;
  }
};

// The clip's edges and level: a raised-cosine fade in and out (no clicks),
// then the whole clip scaled so its loudest sample is `peak`.
PcmClip finish(Buf& b, int peak) {
  const size_t n = b.size();
  const size_t in = samples_for(kSoundFadeInMs);
  const size_t out = samples_for(kSoundFadeOutMs);
  for (size_t i = 0; i < in && i < n; ++i) b[i] *= smooth(static_cast<float>(i) / static_cast<float>(in));
  for (size_t i = 0; i < out && i < n; ++i) {
    b[n - 1 - i] *= smooth(static_cast<float>(i) / static_cast<float>(out));
  }
  float top = 1e-6f;
  for (float v : b) top = std::max(top, fabsf(v));
  const float scale = static_cast<float>(peak) / top;
  PcmClip clip;
  clip.samples.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const float v = std::min(static_cast<float>(kSoundPeakMax), std::max(-static_cast<float>(kSoundPeakMax), b[i] * scale));
    clip.samples[i] = static_cast<int16_t>(lrintf(v));
  }
  return clip;
}

// ---------------------------------------------------------------- Goal

// An air horn over a crowd that roars up. The horn: two tones a major third
// apart (Ab4 and C5), each doubled and detuned by 0.4 percent for the beat,
// saw and square mixed and low-passed at 2.4 kHz so it is loud but not harsh.
// It overshoots in pitch for the first 40 ms (the blast), sags 3.5 percent
// over the clip and falls 7 percent as the air runs out in the last 450 ms.
// The crowd is noise through a band-pass that opens upwards, with slow
// swells in level like many voices.
PcmClip make_goal() {
  const size_t n = samples_for(kGoalMs);
  const float dur = kGoalMs / 1000.0f;
  Buf b(n, 0.0f);
  Rng rng(0x600DF00Du);

  Osc osc[4];
  Biquad tone;
  tone.lowpass(2400.0f, 0.8f);
  Biquad crowd;
  Biquad crowdSoft;
  crowdSoft.lowpass(2800.0f, 0.7f);
  const float f1 = 415.3f;
  const float f2 = 523.25f;
  const float ratios[4] = {1.004f, 1.0f / 1.004f, 1.004f, 1.0f / 1.004f};
  const float bases[4] = {f1, f1, f2, f2};

  for (size_t i = 0; i < n; ++i) {
    const float t = static_cast<float>(i) / kRate;
    // Horn.
    float bend = 1.0f - 0.035f * (t / dur) + 0.04f * expf(-t / 0.04f);
    bend *= 1.0f - 0.07f * smooth((t - (dur - 0.45f)) / 0.45f);
    float env = 1.0f - expf(-t / 0.012f);
    env *= 1.0f - smooth((t - (dur - 0.35f)) / 0.35f);
    const float flutter = 1.0f + 0.05f * sinf(kTwoPi * 9.0f * t);
    float horn = 0.0f;
    for (int k = 0; k < 4; ++k) {
      osc[k].tick(bases[k] * ratios[k] * bend);
      horn += 0.55f * osc[k].saw + 0.45f * osc[k].square;
    }
    // Driven into a soft clip first: the blast is dense, as a real horn is.
    horn = tone.process(tanhf(horn * 0.55f)) * env * flutter;

    // Crowd.
    if ((i & 31) == 0) crowd.bandpass(550.0f + 1000.0f * smooth(t / 1.6f), 0.9f);
    const float swell = 0.06f + 0.94f * powf(smooth(t / 1.7f), 1.3f);
    const float voices = 0.8f + 0.12f * sinf(kTwoPi * 4.3f * t + 1.1f) + 0.08f * sinf(kTwoPi * 7.1f * t);
    const float tail = 1.0f - smooth((t - (dur - 0.4f)) / 0.4f);
    const float noise = crowdSoft.process(crowd.process(rng.next())) * 1.7f;
    b[i] = horn + noise * swell * voices * tail;
  }
  // A soft limiter on the whole mix packs the horn and the swell into the
  // same peak, so the clip is loud without being any higher than the rest.
  float top = 1e-6f;
  for (float v : b) top = std::max(top, fabsf(v));
  for (float& v : b) v = tanhf(1.8f * v / top);
  return finish(b, 29000);
}

// ---------------------------------------------------------------- whistles

// One pea-whistle blast at `startMs`: a 2.7 kHz tone with a 30 Hz vibrato
// (the pea rattling: frequency and level both warble), a touch of second
// harmonic and some breath. It scoops up from 6 percent flat in 40 ms.
void whistle(Buf& b, float startMs, float ms, float level, Rng& rng) {
  const size_t from = samples_for(startMs);
  const size_t n = samples_for(ms);
  float phase = 0.0f;
  float prev = 0.0f;
  for (size_t i = 0; i < n && from + i < b.size(); ++i) {
    const float t = static_cast<float>(i) / kRate;
    const float trill = kTwoPi * 30.0f * t;
    const float f = 2700.0f * (1.0f - 0.06f * expf(-t / 0.04f)) + 90.0f * sinf(trill);
    phase += kTwoPi * f / kRate;
    if (phase > kTwoPi) phase -= kTwoPi;
    const float warble = 1.0f - 0.3f * (0.5f + 0.5f * sinf(trill + 1.2f));
    float env = smooth(t / 0.02f) * (1.0f - smooth((t - (ms / 1000.0f - 0.05f)) / 0.05f));
    const float noise = rng.next();
    const float breath = (noise - prev) * 0.07f;  // high-passed
    prev = noise;
    b[from + i] += level * env * (warble * (sinf(phase) + 0.2f * sinf(2.0f * phase)) + breath);
  }
}

PcmClip make_kick_off() {
  Buf b(samples_for(kKickOffMs), 0.0f);
  Rng rng(0x0F0F1u);
  whistle(b, 0.0f, static_cast<float>(kKickOffMs), 1.0f, rng);
  return finish(b, 21000);
}

// Short, short, long.
PcmClip make_full_time() {
  Buf b(samples_for(kFullTimeMs), 0.0f);
  Rng rng(0x0F0F2u);
  whistle(b, 0.0f, 250.0f, 1.0f, rng);
  whistle(b, 350.0f, 250.0f, 1.0f, rng);
  whistle(b, 700.0f, 350.0f, 1.0f, rng);
  return finish(b, 21000);
}

// ---------------------------------------------------------------- fanfare

// A brass note: two saws 0.3 percent apart, a low-pass that opens with the
// blare (the attack overshoots, then settles) and a little vibrato once it is
// held.
void brass(Buf& b, float startMs, float ms, float hz, float level, float releaseMs) {
  const size_t from = samples_for(startMs);
  const size_t n = samples_for(ms);
  Osc a;
  Osc c;
  float y = 0.0f;
  for (size_t i = 0; i < n && from + i < b.size(); ++i) {
    const float t = static_cast<float>(i) / kRate;
    const float vib = 1.0f + 0.004f * smooth((t - 0.2f) / 0.2f) * sinf(kTwoPi * 5.5f * t);
    a.tick(hz * 1.003f * vib);
    c.tick(hz / 1.003f * vib);
    float env = smooth(t / 0.03f) * (1.0f + 0.15f * expf(-t / 0.08f));
    env *= 1.0f - smooth((t - (ms - releaseMs) / 1000.0f) / (releaseMs / 1000.0f));
    const float cutoff = 700.0f + 3000.0f * std::min(1.0f, env);
    const float k = 1.0f - expf(-kTwoPi * cutoff / kRate);
    y += k * (0.5f * (a.saw + c.saw) - y);
    b[from + i] += level * env * y;
  }
}

// G4 C5 E5, then C major held: the classic "ta-ta-ta TAAA".
PcmClip make_fanfare() {
  Buf b(samples_for(kFanfareMs), 0.0f);
  brass(b, 0.0f, 150.0f, 392.00f, 0.7f, 30.0f);
  brass(b, 170.0f, 150.0f, 523.25f, 0.7f, 30.0f);
  brass(b, 340.0f, 150.0f, 659.25f, 0.7f, 30.0f);
  const float chord[4] = {523.25f, 659.25f, 783.99f, 1046.50f};
  const float level[4] = {0.55f, 0.45f, 0.45f, 0.35f};
  for (int k = 0; k < 4; ++k) brass(b, 520.0f, 880.0f, chord[k], level[k], 260.0f);
  return finish(b, 28000);
}

// ---------------------------------------------------------------- beeps

// A soft sine beep of `ms` with its own 6 ms edges.
void beep(Buf& b, float startMs, float ms, float hz, float level) {
  const size_t from = samples_for(startMs);
  const size_t n = samples_for(ms);
  for (size_t i = 0; i < n && from + i < b.size(); ++i) {
    const float t = static_cast<float>(i) / kRate;
    const float edge = smooth(t / 0.006f) * (1.0f - smooth((t - (ms / 1000.0f - 0.006f)) / 0.006f));
    const float w = kTwoPi * hz * t;
    b[from + i] += level * edge * (sinf(w) + 0.12f * sinf(2.0f * w));
  }
}

PcmClip make_battery_low() {
  Buf b(samples_for(330.0f), 0.0f);
  beep(b, 0.0f, 120.0f, 740.0f, 1.0f);
  beep(b, 210.0f, 120.0f, 740.0f, 1.0f);
  return finish(b, 10000);
}

PcmClip make_battery_critical() {
  Buf b(samples_for(440.0f), 0.0f);
  for (int k = 0; k < 3; ++k) beep(b, 170.0f * static_cast<float>(k), 100.0f, 880.0f, 1.0f);
  return finish(b, 11000);
}

// Two bell-like notes, G5 then D6: rising, short, friendly.
PcmClip make_wifi_ok() {
  Buf b(samples_for(200.0f), 0.0f);
  const float hz[2] = {783.99f, 1174.66f};
  for (int k = 0; k < 2; ++k) {
    const size_t from = samples_for(100.0f * static_cast<float>(k));
    for (size_t i = 0; from + i < b.size(); ++i) {
      const float t = static_cast<float>(i) / kRate;
      const float env = smooth(t / 0.004f) * expf(-t / 0.045f);
      const float w = kTwoPi * hz[k] * t;
      b[from + i] += env * (sinf(w) + 0.3f * sinf(2.0f * w));
    }
  }
  return finish(b, 12000);
}

template <SoundEvent E>
const PcmClip& cached() {
  static const PcmClip clip = sound_generate(E);
  return clip;
}

}  // namespace

PcmClip sound_generate(SoundEvent event) {
  switch (event) {
    case SoundEvent::Goal:
    case SoundEvent::Preview: return make_goal();
    case SoundEvent::WinFanfare: return make_fanfare();
    case SoundEvent::KickOff: return make_kick_off();
    case SoundEvent::FullTime: return make_full_time();
    case SoundEvent::BatteryLow: return make_battery_low();
    case SoundEvent::BatteryCritical: return make_battery_critical();
    case SoundEvent::WifiOk: return make_wifi_ok();
  }
  return PcmClip();
}

const PcmClip& sound_clip(SoundEvent event) {
  switch (event) {
    case SoundEvent::Goal:
    case SoundEvent::Preview: return cached<SoundEvent::Goal>();
    case SoundEvent::WinFanfare: return cached<SoundEvent::WinFanfare>();
    case SoundEvent::KickOff: return cached<SoundEvent::KickOff>();
    case SoundEvent::FullTime: return cached<SoundEvent::FullTime>();
    case SoundEvent::BatteryLow: return cached<SoundEvent::BatteryLow>();
    case SoundEvent::BatteryCritical: return cached<SoundEvent::BatteryCritical>();
    case SoundEvent::WifiOk: return cached<SoundEvent::WifiOk>();
  }
  return cached<SoundEvent::Goal>();
}

void sound_prepare_all() {
  for (int e = 0; e < kSoundEventCount; ++e) sound_clip(static_cast<SoundEvent>(e));
}
