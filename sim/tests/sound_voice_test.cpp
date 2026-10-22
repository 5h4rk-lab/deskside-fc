// The speaker's mixing logic (pure, shared with the board's I2S player): the
// volume curve, stereo frames and channel selection, pre-roll, ramps, the
// 20 ms stop fade, replacing a clip that plays, and the silent tail.
#include <doctest/doctest.h>

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "sound_synth.h"
#include "sound_voice.h"

namespace {

constexpr int kTail = 480;

// A clip of `n` samples all at `level`.
std::vector<int16_t> flat(size_t n, int16_t level) { return std::vector<int16_t>(n, level); }

// Renders `frames` stereo frames in blocks of `block`.
std::vector<int16_t> render(SoundVoice& v, size_t frames, size_t block = 240) {
  std::vector<int16_t> out(frames * 2, 12345);
  size_t done = 0;
  while (done < frames) {
    const size_t n = std::min(block, frames - done);
    v.render(&out[done * 2], n);
    done += n;
  }
  return out;
}

int16_t left(const std::vector<int16_t>& f, size_t i) { return f[i * 2]; }
int16_t right(const std::vector<int16_t>& f, size_t i) { return f[i * 2 + 1]; }

// Largest jump between neighbouring samples of one channel.
int max_step(const std::vector<int16_t>& f, int ch) {
  int m = 0;
  for (size_t i = 1; i < f.size() / 2; ++i) m = std::max(m, std::abs(f[i * 2 + ch] - f[(i - 1) * 2 + ch]));
  return m;
}

double db(int q15) { return 20.0 * log10(static_cast<double>(q15) / 32767.0); }

}  // namespace

TEST_CASE("sound voice: the volume curve is perceptual, gain = (percent/100)^2 in Q15") {
  CHECK(sound_gain_q15(0) == 0);
  CHECK(sound_gain_q15(100) == kSoundFullGainQ15);
  CHECK(sound_gain_q15(20) == 1311);
  CHECK(sound_gain_q15(40) == 5243);
  CHECK(sound_gain_q15(60) == 11796);
  CHECK(db(sound_gain_q15(20)) == doctest::Approx(-27.96).epsilon(0.002));
  CHECK(db(sound_gain_q15(40)) == doctest::Approx(-15.92).epsilon(0.002));
  CHECK(db(sound_gain_q15(100)) == doctest::Approx(0.0));
  for (int p = 1; p <= 100; ++p) CHECK(sound_gain_q15(p) > sound_gain_q15(p - 1));
  CHECK(sound_gain_q15(-5) == 0);
  CHECK(sound_gain_q15(150) == kSoundFullGainQ15);
}

TEST_CASE("sound voice: an idle voice renders silence and stays idle") {
  SoundVoice v(kTail);
  CHECK(v.idle());
  const std::vector<int16_t> f = render(v, 1000);
  for (int16_t s : f) CHECK(s == 0);
  CHECK(v.idle());
}

TEST_CASE("sound voice: a cold start pre-rolls silence, ramps in, copies the clip to both channels, then a silent tail") {
  const std::vector<int16_t> clip = flat(2000, 20000);
  SoundVoice v(kTail);
  v.start(clip.data(), clip.size(), sound_gain_q15(100), SoundChannels::Both);
  CHECK_FALSE(v.idle());
  const size_t total = SoundVoice::kPreRollFrames + clip.size() + kTail;
  const std::vector<int16_t> f = render(v, total + 100);
  for (size_t i = 0; i < static_cast<size_t>(SoundVoice::kPreRollFrames); ++i) REQUIRE(left(f, i) == 0);
  const size_t first = SoundVoice::kPreRollFrames;
  // The 2 ms ramp: rising, never above the clip.
  CHECK(left(f, first) < 2000);
  for (size_t i = first + 1; i < first + SoundVoice::kRampFrames; ++i) CHECK(left(f, i) >= left(f, i - 1));
  // Full level after the ramp: 20000 * 32767 / 32768.
  for (size_t i = first + SoundVoice::kRampFrames; i < first + clip.size() - SoundVoice::kRampFrames; ++i) {
    REQUIRE(left(f, i) == 19999);
  }
  // ... and the last 2 ms ramp down to nothing, whatever the clip does.
  for (size_t i = first + clip.size() - SoundVoice::kRampFrames; i < first + clip.size(); ++i) {
    REQUIRE(left(f, i) <= left(f, i - 1));
  }
  CHECK(left(f, first + clip.size() - 1) == 0);
  for (size_t i = 0; i < total; ++i) REQUIRE(left(f, i) == right(f, i));
  for (size_t i = first + clip.size(); i < total + 100; ++i) REQUIRE(left(f, i) == 0);
  CHECK(v.idle());
  CHECK(max_step(f, 0) <= 20000 / SoundVoice::kRampFrames + 1);
}

TEST_CASE("sound voice: the gain scales every sample") {
  const std::vector<int16_t> clip = flat(1000, 30000);
  SoundVoice v(kTail);
  v.start(clip.data(), clip.size(), sound_gain_q15(40), SoundChannels::Both);
  const std::vector<int16_t> f = render(v, 1000);
  const size_t mid = SoundVoice::kPreRollFrames + 200;
  CHECK(left(f, mid) == (30000 * 5243) >> 15);
}

TEST_CASE("sound voice: one channel only leaves the other silent (the left/right speaker test)") {
  const std::vector<int16_t> clip = flat(1000, 10000);
  for (SoundChannels ch : {SoundChannels::Left, SoundChannels::Right}) {
    SoundVoice v(kTail);
    v.start(clip.data(), clip.size(), kSoundFullGainQ15, ch);
    const std::vector<int16_t> f = render(v, 3000);
    int loud = 0;
    for (size_t i = 0; i < 3000; ++i) {
      const int16_t on = ch == SoundChannels::Left ? left(f, i) : right(f, i);
      const int16_t off = ch == SoundChannels::Left ? right(f, i) : left(f, i);
      REQUIRE(off == 0);
      if (on != 0) ++loud;
    }
    CHECK(loud == 999);  // the last frame is the end of the 2 ms ramp: 0
  }
}

TEST_CASE("sound voice: stop() fades out within 20 ms, then the tail, then idle") {
  const std::vector<int16_t> clip = flat(16000, 20000);
  SoundVoice v(kTail);
  v.start(clip.data(), clip.size(), kSoundFullGainQ15, SoundChannels::Both);
  render(v, SoundVoice::kPreRollFrames + 1000);
  v.stop();
  CHECK_FALSE(v.idle());
  const std::vector<int16_t> f = render(v, SoundVoice::kStopFadeFrames + kTail + 10);
  CHECK(left(f, 0) > 19000);
  for (size_t i = 1; i < static_cast<size_t>(SoundVoice::kStopFadeFrames); ++i) REQUIRE(left(f, i) <= left(f, i - 1));
  for (size_t i = SoundVoice::kStopFadeFrames; i < f.size() / 2; ++i) REQUIRE(left(f, i) == 0);
  CHECK(v.idle());
  CHECK(max_step(f, 0) <= 20000 / SoundVoice::kStopFadeFrames + 2);
  CHECK(SoundVoice::kStopFadeFrames == 320);  // 20 ms at 16 kHz
}

TEST_CASE("sound voice: stop() during the pre-roll or the tail, or when idle, is quiet and ends idle") {
  const std::vector<int16_t> clip = flat(100, 20000);
  SoundVoice v(kTail);
  v.stop();
  CHECK(v.idle());
  v.start(clip.data(), clip.size(), kSoundFullGainQ15, SoundChannels::Both);
  render(v, 10);
  v.stop();
  std::vector<int16_t> f = render(v, 2000);
  for (int16_t s : f) REQUIRE(s == 0);
  CHECK(v.idle());
  v.start(clip.data(), clip.size(), kSoundFullGainQ15, SoundChannels::Both);
  render(v, SoundVoice::kPreRollFrames + 100 + 5);  // into the tail
  v.stop();
  f = render(v, 2000);
  for (int16_t s : f) REQUIRE(s == 0);
  CHECK(v.idle());
}

TEST_CASE("sound voice: a new clip replaces one that plays after a 10 ms fade, without a new pre-roll") {
  const std::vector<int16_t> first = flat(16000, 20000);
  const std::vector<int16_t> second = flat(1000, -10000);
  SoundVoice v(kTail);
  v.start(first.data(), first.size(), kSoundFullGainQ15, SoundChannels::Both);
  render(v, SoundVoice::kPreRollFrames + 500);
  v.start(second.data(), second.size(), kSoundFullGainQ15, SoundChannels::Both);
  const std::vector<int16_t> f = render(v, SoundVoice::kSwitchFadeFrames + second.size() + kTail + 10);
  // The old clip fades to nothing (positive samples) ...
  for (size_t i = 0; i < static_cast<size_t>(SoundVoice::kSwitchFadeFrames); ++i) REQUIRE(left(f, i) >= 0);
  CHECK(left(f, SoundVoice::kSwitchFadeFrames - 1) < 200);
  // ... then the new one ramps in at once.
  const size_t start = SoundVoice::kSwitchFadeFrames;
  for (size_t i = start; i < start + second.size(); ++i) REQUIRE(left(f, i) <= 0);
  CHECK(left(f, start + SoundVoice::kRampFrames + 10) == (-10000 * 32767) >> 15);
  for (size_t i = start + second.size(); i < f.size() / 2; ++i) REQUIRE(left(f, i) == 0);
  CHECK(v.idle());
  // Steps: the old clip's fade, or the new clip's 2 ms ramp (real clips add their own 10 ms fade in).
  CHECK(max_step(f, 0) <= std::max(20000 / SoundVoice::kSwitchFadeFrames, 10000 / SoundVoice::kRampFrames) + 2);
}

TEST_CASE("sound voice: a clip started during the silent tail plays at once (the channel is still running)") {
  const std::vector<int16_t> clip = flat(100, 8000);
  SoundVoice v(kTail);
  v.start(clip.data(), clip.size(), kSoundFullGainQ15, SoundChannels::Both);
  render(v, SoundVoice::kPreRollFrames + 100 + 50);
  v.start(clip.data(), clip.size(), kSoundFullGainQ15, SoundChannels::Both);
  const std::vector<int16_t> f = render(v, 200);
  CHECK(left(f, SoundVoice::kRampFrames + 5) == (8000 * 32767) >> 15);
}

TEST_CASE("sound voice: a clip started during the pre-roll replaces it and keeps the pre-roll") {
  const std::vector<int16_t> a = flat(100, 8000);
  const std::vector<int16_t> b = flat(100, -8000);
  SoundVoice v(kTail);
  v.start(a.data(), a.size(), kSoundFullGainQ15, SoundChannels::Both);
  render(v, 100);
  v.start(b.data(), b.size(), kSoundFullGainQ15, SoundChannels::Both);
  const std::vector<int16_t> f = render(v, SoundVoice::kPreRollFrames);
  for (size_t i = 0; i < SoundVoice::kPreRollFrames - 100u; ++i) REQUIRE(left(f, i) == 0);
  CHECK(left(f, SoundVoice::kPreRollFrames - 100 + SoundVoice::kRampFrames + 5) < 0);
}

TEST_CASE("sound voice: an empty clip changes nothing") {
  SoundVoice v(kTail);
  v.start(nullptr, 0, kSoundFullGainQ15, SoundChannels::Both);
  CHECK(v.idle());
}

TEST_CASE("sound voice: the real Goal clip played through the voice keeps its samples (gain 100 percent)") {
  const PcmClip& c = sound_clip(SoundEvent::Goal);
  SoundVoice v(kTail);
  v.start(c.samples.data(), c.samples.size(), kSoundFullGainQ15, SoundChannels::Both);
  const std::vector<int16_t> f = render(v, SoundVoice::kPreRollFrames + c.samples.size() + kTail);
  for (size_t i = SoundVoice::kRampFrames; i < c.samples.size() - SoundVoice::kRampFrames; ++i) {
    REQUIRE(std::abs(left(f, SoundVoice::kPreRollFrames + i) - c.samples[i]) <= 1);
  }
  CHECK(v.idle());
}
