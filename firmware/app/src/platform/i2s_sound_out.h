// The board's speaker: SoundOut over the ESP-IDF i2s_std driver, into the
// PCM5101A DAC and the APA2068 stereo amplifier (see the audio section of
// docs/superpowers/specs/hardware-waveshare-esp32-s3-touch-lcd-2.8.md).
//
// I2S: BCLK GPIO48, LRC/WS GPIO38, DOUT GPIO47, no MCLK (the DAC's SCK pin is
// grounded: it makes its clock from BCK with its PLL). 16 kHz, 16-bit samples,
// stereo frames (Philips format). Each 16-bit sample is sent in a 32-bit slot,
// so BCK is 64 x 16 kHz = 1.024 MHz: the PCM510x PLL needs BCK of about
// 1 MHz or more, which 32 x 16 kHz (512 kHz) is not. DMA: 6 buffers of 240
// frames (90 ms queued at most).
//
// Threads: play(), stop() and busy() are safe from any task and never block.
// Commands go through a queue of depth 2 to the player task (core 0, priority
// 3: below the Wi-Fi driver (23), lwIP (18) and on a par with the Wi-Fi
// worker, above the HTTP workers (2), so a TLS handshake cannot starve the
// DMA; it sleeps in i2s_channel_write between blocks, a few microseconds of
// work per 15 ms block). When the queue is full the oldest command is
// dropped: the newest always wins (a play replaces whatever plays anyway,
// and a stop after a play still stops).
//
// The mixing (volume, channel, pre-roll, ramps, the 20 ms stop fade, the
// cross-over when a clip replaces another, the silent tail) is SoundVoice
// (ui/src/sound_voice.h, sim-tested). The I2S channel runs only while a clip
// (or its pre-roll/tail) plays and is disabled otherwise: no clock to the DAC
// and no idle hiss from the DAC. The amplifier has no enable pin wired to the
// ESP32 (MUTE pulled low, SD pulled high on the board), so whatever hiss the
// amplifier makes by itself the firmware cannot switch off.
//
// Memory: the DMA buffers (5760 B) are internal RAM (DMA needs it); the
// player's 4 KB stack is in PSRAM; the clips are in PSRAM (sound_synth).
//
// A failed init makes every call a silent no-op (logged once).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "sound_out.h"
#include "sound_voice.h"

class I2sSoundOut : public SoundOut {
 public:
  static constexpr uint32_t kRate = 16000;
  static constexpr uint32_t kDmaBuffers = 6;
  static constexpr uint32_t kDmaFrames = 240;    // per buffer and per write: 15 ms
  static constexpr int kTailFrames = static_cast<int>((kDmaBuffers + 1) * kDmaFrames);  // 105 ms of zeros
  static constexpr uint32_t kTaskStackBytes = 4096;
  static constexpr UBaseType_t kTaskPriority = 3;
  static constexpr BaseType_t kTaskCore = 0;
  static constexpr int kQueueDepth = 2;

  I2sSoundOut() = default;
  I2sSoundOut(const I2sSoundOut&) = delete;
  I2sSoundOut& operator=(const I2sSoundOut&) = delete;

  // Creates the I2S channel (not started), the queue and the player task.
  // False (logged once) when any of it fails; then every call does nothing.
  bool begin();
  bool ok() const { return ok_; }

  // SoundOut
  void play(const PcmClip& clip, uint8_t volumePercent) override;
  void stop() override;
  bool busy() const override;

  // play() on one channel only (the left/right speaker test), or both.
  void play_on(const PcmClip& clip, uint8_t volumePercent, SoundChannels channels);
  // Waits until nothing plays (after stop(): the fade and the queued DMA,
  // about 0.15 s). True when idle within `timeoutMs`. Not from the player task.
  bool wait_idle(uint32_t timeoutMs);

  struct Stats {
    uint32_t plays = 0;        // play commands the player took
    uint32_t replaced = 0;     // plays that arrived while another clip played
    uint32_t stops = 0;        // stop commands the player took
    uint32_t dropped = 0;      // commands dropped because the queue was full
    uint32_t writeErrors = 0;  // i2s_channel_write failures or short writes
    uint32_t underruns = 0;    // DMA buffers the driver had to send with nothing new in them
                               // (its send queue overflowed: the player was late); counted
                               // only while the channel runs
    uint32_t runs = 0;         // times the channel was started
    uint32_t lastRunMs = 0;    // last channel-on time, enable to disable
    uint32_t lastClipMs = 0;   // length of the last clip started
    uint32_t dmaBytes = 0;     // DMA buffers in internal RAM (from the driver)
  };
  Stats stats() const;
  size_t stack_free_bytes() const;
  // One line ([sound]): state, counters, stack, DMA memory.
  void print_status() const;

 private:
  enum class Kind : uint8_t { Play, Stop };
  struct Command {
    Kind kind;
    SoundChannels channels;
    uint8_t volume;
    const int16_t* samples;
    size_t n;
  };

  static void task_main(void* self);
  static bool on_queue_overflow(i2s_chan_handle_t, i2s_event_data_t*, void* self);
  void run();
  void send(const Command& c);
  void apply(const Command& c);
  void fail(const char* what, int err);

  bool ok_ = false;
  i2s_chan_handle_t tx_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SoundVoice voice_{kTailFrames};
  bool enabled_ = false;  // player task only
  uint32_t enabledAtMs_ = 0;
  std::atomic<int> pending_{0};      // commands sent and not yet taken
  std::atomic<bool> active_{false};  // the voice has something to play
  mutable std::atomic<uint32_t> plays_{0}, replaced_{0}, stops_{0}, dropped_{0}, writeErrors_{0}, runs_{0},
      lastRunMs_{0}, lastClipMs_{0}, underruns_{0};
  uint32_t dmaBytes_ = 0;
  int16_t frames_[kDmaFrames * 2] = {};
};
