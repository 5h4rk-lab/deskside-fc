// The synthesized clips: levels, fades, determinism, lengths and the budget.
#include <doctest/doctest.h>

#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "sound_synth.h"

namespace {

const SoundEvent kAll[] = {SoundEvent::Goal,       SoundEvent::WinFanfare,      SoundEvent::KickOff,
                           SoundEvent::FullTime,   SoundEvent::BatteryLow,      SoundEvent::BatteryCritical,
                           SoundEvent::WifiOk,     SoundEvent::Preview};

int peak_of(const PcmClip& c) {
  int p = 0;
  for (int16_t s : c.samples) p = std::max(p, std::abs(static_cast<int>(s)));
  return p;
}

double rms_of(const int16_t* a, size_t n) {
  double sum = 0;
  for (size_t i = 0; i < n; ++i) sum += static_cast<double>(a[i]) * a[i];
  return n ? sqrt(sum / static_cast<double>(n)) : 0;
}

double seconds(const PcmClip& c) { return static_cast<double>(c.samples.size()) / PcmClip::kRate; }

}  // namespace

TEST_CASE("sound clips: every clip has samples") {
  for (SoundEvent e : kAll) CHECK_FALSE(sound_clip(e).samples.empty());
}

TEST_CASE("sound clips: the peak stays at 90 percent of full scale or below, and the clip is not quiet") {
  for (SoundEvent e : kAll) {
    const PcmClip& c = sound_clip(e);
    CHECK(peak_of(c) <= kSoundPeakMax);
    CHECK(peak_of(c) >= 6000);  // soft beeps included
  }
  CHECK(kSoundPeakMax == 29491);
}

TEST_CASE("sound clips: no long stretch at the full-scale peak (nothing is clipped flat)") {
  for (SoundEvent e : kAll) {
    const PcmClip& c = sound_clip(e);
    const int p = peak_of(c);
    int run = 0;
    int longest = 0;
    for (int16_t s : c.samples) {
      run = std::abs(static_cast<int>(s)) >= p - 2 ? run + 1 : 0;
      longest = std::max(longest, run);
    }
    CHECK(longest < 8);
  }
}

TEST_CASE("sound clips: the first and last 8 samples are within 3 percent of full scale (fades)") {
  const int limit = 32767 * 3 / 100;
  for (SoundEvent e : kAll) {
    const PcmClip& c = sound_clip(e);
    REQUIRE(c.samples.size() > 16);
    for (size_t i = 0; i < 8; ++i) {
      CHECK(std::abs(static_cast<int>(c.samples[i])) <= limit);
      CHECK(std::abs(static_cast<int>(c.samples[c.samples.size() - 1 - i])) <= limit);
    }
  }
}

TEST_CASE("sound clips: the fade in takes 10 ms and the fade out 30 ms") {
  for (SoundEvent e : kAll) {
    const PcmClip& c = sound_clip(e);
    const size_t in = kSoundFadeInMs * PcmClip::kRate / 1000;
    const size_t out = kSoundFadeOutMs * PcmClip::kRate / 1000;
    // Louder after the fade than inside it (the clip does not start at full level).
    const double head = rms_of(c.samples.data(), in / 2);
    const double body = rms_of(c.samples.data() + in, in * 4);
    CHECK(head < body);
    // The last quarter millisecond is nearly silent.
    CHECK(rms_of(c.samples.data() + c.samples.size() - 4, 4) < 400);
    CHECK(c.samples.size() > in + out);
  }
}

TEST_CASE("sound clips: two generations give identical bytes, and the cache holds the same") {
  for (SoundEvent e : kAll) {
    const PcmClip a = sound_generate(e);
    const PcmClip b = sound_generate(e);
    REQUIRE(a.samples.size() == b.samples.size());
    CHECK(std::memcmp(a.samples.data(), b.samples.data(), a.samples.size() * sizeof(int16_t)) == 0);
    CHECK(sound_clip(e).samples == a.samples);
  }
}

TEST_CASE("sound clips: the sounds differ from each other (Preview is the Goal clip)") {
  std::set<std::vector<int16_t>> distinct;
  for (SoundEvent e : kAll) {
    if (e != SoundEvent::Preview) distinct.insert(sound_clip(e).samples);
  }
  CHECK(distinct.size() == 7);
}

TEST_CASE("sound clips: lengths") {
  const PcmClip& goal = sound_clip(SoundEvent::Goal);
  CHECK(seconds(goal) >= 2.0);
  CHECK(seconds(goal) <= 3.0);
  const PcmClip& kick = sound_clip(SoundEvent::KickOff);
  CHECK(seconds(kick) >= 0.4);
  CHECK(seconds(kick) <= 0.8);
  CHECK(seconds(sound_clip(SoundEvent::WinFanfare)) >= 1.2);
  CHECK(seconds(sound_clip(SoundEvent::WinFanfare)) <= 2.2);
  CHECK(seconds(sound_clip(SoundEvent::FullTime)) >= 0.9);
  CHECK(seconds(sound_clip(SoundEvent::FullTime)) <= 1.6);
  CHECK(seconds(sound_clip(SoundEvent::BatteryLow)) < seconds(sound_clip(SoundEvent::BatteryCritical)));
  CHECK(seconds(sound_clip(SoundEvent::WifiOk)) <= 0.4);
  CHECK(sound_clip(SoundEvent::Goal).duration_ms() == static_cast<int>(goal.samples.size() / 16));
}

TEST_CASE("sound clips: Preview is the Goal clip") {
  CHECK(&sound_clip(SoundEvent::Preview) == &sound_clip(SoundEvent::Goal));
}

TEST_CASE("sound clips: all of them together fit the budget (100000 samples, 200 KB)") {
  std::set<const PcmClip*> unique;
  for (SoundEvent e : kAll) unique.insert(&sound_clip(e));
  size_t total = 0;
  for (const PcmClip* c : unique) total += c->samples.size();
  CHECK(total <= 100000);
  CHECK(total * sizeof(int16_t) <= 200u * 1024u);
}

TEST_CASE("sound clips: sound_prepare_all() changes nothing") {
  const std::vector<int16_t> before = sound_clip(SoundEvent::Goal).samples;
  sound_prepare_all();
  CHECK(sound_clip(SoundEvent::Goal).samples == before);
}

TEST_CASE("sound clips: loudness floor (RMS) per clip") {
  // Soft beeps are quiet on purpose; everything else is clearly audible.
  for (SoundEvent e : kAll) {
    const PcmClip& c = sound_clip(e);
    const double rms = rms_of(c.samples.data(), c.samples.size());
    const bool soft = e == SoundEvent::BatteryLow || e == SoundEvent::BatteryCritical || e == SoundEvent::WifiOk;
    CHECK(rms > (soft ? 1500.0 : 5000.0));
  }
}

TEST_CASE("sound clips: Goal has no silent gap longer than 200 ms and a crowd swell that rises") {
  const PcmClip& c = sound_clip(SoundEvent::Goal);
  const size_t win = PcmClip::kRate / 50;  // 20 ms windows
  size_t silent = 0;
  size_t longest = 0;
  for (size_t i = win; i + win <= c.samples.size() - 480; i += win) {  // inside the fades
    if (rms_of(c.samples.data() + i, win) < 800) {
      ++silent;
      longest = std::max(longest, silent);
    } else {
      silent = 0;
    }
  }
  CHECK(longest * win * 1000 / PcmClip::kRate <= 200);
  // The first 300 ms are quieter than the middle (the swell builds up).
  const double early = rms_of(c.samples.data() + 160, 4000);
  const double mid = rms_of(c.samples.data() + c.samples.size() / 2, 4000);
  CHECK(early < mid);
}

TEST_CASE("sound clips: whistles sit around 2.7 kHz with a fast trill") {
  // Zero crossings per second of the loudest stretch estimate the pitch.
  const PcmClip& c = sound_clip(SoundEvent::KickOff);
  const size_t from = c.samples.size() / 4;
  const size_t n = c.samples.size() / 2;
  int crossings = 0;
  for (size_t i = from + 1; i < from + n; ++i) {
    if ((c.samples[i - 1] < 0) != (c.samples[i] < 0)) ++crossings;
  }
  const double hz = crossings / 2.0 / (static_cast<double>(n) / PcmClip::kRate);
  CHECK(hz > 2300);
  CHECK(hz < 3300);
}

TEST_CASE("sound clips: the duration table matches the clips (the gate and timers use the table)") {
  for (SoundEvent e : kAll) CHECK(sound_clip_duration_ms(e) == sound_generate(e).duration_ms());
  static_assert(sound_clip_duration_ms(SoundEvent::Goal) == 2250, "compile-time");
  CHECK(sound_clip_duration_ms(SoundEvent::Preview) == sound_clip_duration_ms(SoundEvent::Goal));
}

TEST_CASE("sound clips: the speaker test tone is a clean 440 Hz sine of the asked length with soft edges") {
  const PcmClip t = sound_test_tone(440, 1000, 12000);
  CHECK(t.samples.size() == 16000u);
  CHECK(peak_of(t) <= 12000);
  CHECK(peak_of(t) >= 11900);
  CHECK(std::abs(t.samples.front()) <= 50);
  CHECK(std::abs(t.samples.back()) <= 50);
  int crossings = 0;
  for (size_t i = 1; i < t.samples.size(); ++i) {
    if ((t.samples[i - 1] < 0) != (t.samples[i] < 0)) ++crossings;
  }
  CHECK(crossings >= 870);  // 440 Hz: 880 sign changes per second
  CHECK(crossings <= 890);
}
