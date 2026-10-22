#include "i2s_sound_out.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "board/pins.h"

namespace {

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

const char* channelName(SoundChannels c) {
  switch (c) {
    case SoundChannels::Left: return "left only";
    case SoundChannels::Right: return "right only";
    case SoundChannels::Both: return "both";
  }
  return "?";
}

}  // namespace

void I2sSoundOut::fail(const char* what, int err) {
  // Logged once: after this ok_ stays false and nothing runs again.
  Serial.printf("[sound] ERROR: %s failed (%d): the speaker stays silent\n", what, err);
  if (tx_) {
    i2s_del_channel(tx_);
    tx_ = nullptr;
  }
  if (queue_) {
    vQueueDelete(queue_);
    queue_ = nullptr;
  }
}

bool I2sSoundOut::begin() {
  if (ok_ || queue_) return ok_;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.dma_desc_num = kDmaBuffers;
  cc.dma_frame_num = kDmaFrames;
  cc.auto_clear = true;  // a buffer that ran out is sent as zeros, never as old samples
  esp_err_t err = i2s_new_channel(&cc, &tx_, nullptr);
  if (err != ESP_OK) {
    tx_ = nullptr;
    fail("i2s_new_channel", err);
    return false;
  }
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = static_cast<gpio_num_t>(pins::kI2sBclk),
              .ws = static_cast<gpio_num_t>(pins::kI2sLrc),
              .dout = static_cast<gpio_num_t>(pins::kI2sDout),
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  // 16-bit samples in 32-bit slots: BCK = 64 fs = 1.024 MHz for the DAC's PLL.
  // WS must span the whole slot (50 % duty): the default ws_width is the data
  // width (16), which with 32-bit slots would give a 25 % duty LRCK. The rest
  // of the default is Philips as the DAC expects it: one-BCK delay (bit_shift),
  // WS low = left (ws_pol false), both slots, data left-aligned in its slot
  // (MSB first, the 16 low bits zero).
  sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  sc.slot_cfg.ws_width = 32;
  err = i2s_channel_init_std_mode(tx_, &sc);
  if (err != ESP_OK) {
    fail("i2s_channel_init_std_mode", err);
    return false;
  }
  // Underruns: the driver's send-queue overflow fires only when every DMA
  // buffer has gone out with no refill (the whole ring ran dry), not for a
  // refill that was merely late. It is an ISR callback (IRAM, and
  // this object lives in internal RAM: a static in setup()).
  i2s_event_callbacks_t cbs = {};
  cbs.on_send_q_ovf = &I2sSoundOut::on_queue_overflow;
  err = i2s_channel_register_event_callback(tx_, &cbs, this);
  if (err != ESP_OK) Serial.printf("[sound] underrun counter off (%d)\n", (int)err);
  i2s_chan_info_t info = {};
  if (i2s_channel_get_info(tx_, &info) == ESP_OK) dmaBytes_ = info.total_dma_buf_size;
  queue_ = xQueueCreate(kQueueDepth, sizeof(Command));
  if (!queue_) {
    fail("xQueueCreate", 0);
    return false;
  }
  ok_ = true;  // before the task starts: it reads ok_
  // The stack lives in PSRAM (internal RAM is what TLS runs short of). Allowed
  // because this task never touches flash (which turns the cache off); while
  // another task writes flash, the IDF parks this core in internal RAM.
  if (xTaskCreatePinnedToCoreWithCaps(&I2sSoundOut::task_main, "sound", kTaskStackBytes, this, kTaskPriority, &task_,
                                      kTaskCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ok_ = false;
    task_ = nullptr;
    fail("xTaskCreatePinnedToCore", 0);
    return false;
  }
  Serial.printf("[sound] ready: I2S %lu Hz, 16-bit in 32-bit slots, WS %lu BCK high/low (50 %% duty, BCK %lu Hz), "
                "%lu x %lu frames DMA (%u B), player task core %d prio %u\n",
                (unsigned long)kRate, (unsigned long)sc.slot_cfg.ws_width, (unsigned long)info.bclk_hz,
                (unsigned long)kDmaBuffers,
                (unsigned long)kDmaFrames, (unsigned)dmaBytes_, (int)kTaskCore, (unsigned)kTaskPriority);
  return true;
}

void I2sSoundOut::send(const Command& c) {
  if (!ok_) return;
  pending_.fetch_add(1);
  if (xQueueSend(queue_, &c, 0) == pdTRUE) return;
  // Full: drop the oldest, the newest wins.
  Command old;
  if (xQueueReceive(queue_, &old, 0) == pdTRUE) {
    pending_.fetch_sub(1);
    dropped_.fetch_add(1);
  }
  if (xQueueSend(queue_, &c, 0) != pdTRUE) {
    pending_.fetch_sub(1);
    dropped_.fetch_add(1);
  }
}

void I2sSoundOut::play(const PcmClip& clip, uint8_t volumePercent) {
  play_on(clip, volumePercent, SoundChannels::Both);
}

void I2sSoundOut::play_on(const PcmClip& clip, uint8_t volumePercent, SoundChannels channels) {
  if (!ok_ || clip.samples.empty()) return;
  send(Command{Kind::Play, channels, volumePercent, clip.samples.data(), clip.samples.size()});
}

void I2sSoundOut::stop() {
  if (!ok_) return;
  send(Command{Kind::Stop, SoundChannels::Both, 0, nullptr, 0});
}

bool I2sSoundOut::busy() const { return ok_ && (pending_.load() > 0 || active_.load()); }

bool I2sSoundOut::wait_idle(uint32_t timeoutMs) {
  const uint32_t start = nowMs();
  while (busy()) {
    if (nowMs() - start >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

bool IRAM_ATTR I2sSoundOut::on_queue_overflow(i2s_chan_handle_t, i2s_event_data_t*, void* self) {
  static_cast<I2sSoundOut*>(self)->underruns_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void I2sSoundOut::task_main(void* self) { static_cast<I2sSoundOut*>(self)->run(); }

void I2sSoundOut::apply(const Command& c) {
  if (c.kind == Kind::Stop) {
    stops_.fetch_add(1);
    voice_.stop();
    Serial.printf("[sound] stop (20 ms fade)\n");
    return;
  }
  const bool wasIdle = voice_.idle();
  const uint32_t clipMs = static_cast<uint32_t>(c.n * 1000 / kRate);
  plays_.fetch_add(1);
  if (!wasIdle) replaced_.fetch_add(1);
  lastClipMs_.store(clipMs);
  const uint16_t gain = sound_gain_q15(c.volume);
  voice_.start(c.samples, c.n, gain, c.channels);
  Serial.printf("[sound] play %lu ms clip at %u%% (gain %u/32767) on %s%s\n", (unsigned long)clipMs,
                (unsigned)c.volume, (unsigned)gain, channelName(c.channels),
                wasIdle ? "" : ", replacing the clip that plays");
}

void I2sSoundOut::run() {
  bool loggedWriteError = false;
  bool loggedDisableError = false;
  for (;;) {
    // First wind down (the voice may have gone idle in the last render), then
    // sleep until a command comes; while something plays, only look.
    if (voice_.idle()) {
      if (enabled_) {
        // Everything audible left the DAC during the tail: only zeros are
        // queued now. Stopping the clock silences the DAC.
        const esp_err_t derr = i2s_channel_disable(tx_);
        if (derr != ESP_OK && !loggedDisableError) {
          loggedDisableError = true;
          Serial.printf("[sound] ERROR: i2s_channel_disable failed (%d) (logged once)\n", (int)derr);
        }
        enabled_ = false;
        const uint32_t ranMs = nowMs() - enabledAtMs_;
        lastRunMs_.store(ranMs);
        Serial.printf("[sound] done: channel on for %lu ms (last clip %lu ms + %d ms pre-roll + %d ms tail), "
                      "underruns %lu in total, stack free %u B\n",
                      (unsigned long)ranMs, (unsigned long)lastClipMs_.load(),
                      SoundVoice::kPreRollFrames * 1000 / SoundVoice::kRate, kTailFrames * 1000 / SoundVoice::kRate,
                      (unsigned long)underruns_.load(), (unsigned)stack_free_bytes());
      }
      if (pending_.load() == 0) active_.store(false);
    }
    Command c;
    TickType_t wait = voice_.idle() ? portMAX_DELAY : 0;
    while (xQueueReceive(queue_, &c, wait) == pdTRUE) {
      if (c.kind == Kind::Play) active_.store(true);  // before pending_ drops: busy() never blinks off
      apply(c);
      pending_.fetch_sub(1);
      wait = 0;
    }
    if (voice_.idle()) continue;  // a stop during the pre-roll: wound down above
    if (!enabled_) {
      // The DMA buffers hold zeros (auto_clear), and the voice starts with a
      // pre-roll of silence while the DAC locks onto the new clock.
      const esp_err_t err = i2s_channel_enable(tx_);
      if (err != ESP_OK) {
        Serial.printf("[sound] ERROR: i2s_channel_enable failed (%d): clip dropped\n", (int)err);
        voice_.stop();
        while (!voice_.idle()) voice_.render(frames_, kDmaFrames);
        writeErrors_.fetch_add(1);
        continue;
      }
      enabled_ = true;
      enabledAtMs_ = nowMs();
      runs_.fetch_add(1);
    }
    voice_.render(frames_, kDmaFrames);
    size_t written = 0;
    const esp_err_t err = i2s_channel_write(tx_, frames_, sizeof(frames_), &written, 200);
    if (err != ESP_OK || written != sizeof(frames_)) {
      writeErrors_.fetch_add(1);
      if (!loggedWriteError) {
        loggedWriteError = true;
        Serial.printf("[sound] ERROR: i2s_channel_write: %d, %u of %u bytes (logged once)\n", (int)err,
                      (unsigned)written, (unsigned)sizeof(frames_));
      }
    }
  }
}

I2sSoundOut::Stats I2sSoundOut::stats() const {
  Stats s;
  s.plays = plays_.load();
  s.replaced = replaced_.load();
  s.stops = stops_.load();
  s.dropped = dropped_.load();
  s.writeErrors = writeErrors_.load();
  s.underruns = underruns_.load();
  s.runs = runs_.load();
  s.lastRunMs = lastRunMs_.load();
  s.lastClipMs = lastClipMs_.load();
  s.dmaBytes = dmaBytes_;
  return s;
}

size_t I2sSoundOut::stack_free_bytes() const {
  return task_ ? uxTaskGetStackHighWaterMark(task_) * sizeof(StackType_t) : 0;
}

void I2sSoundOut::print_status() const {
  if (!ok_) {
    Serial.println("[sound] speaker off (init failed at start)");
    return;
  }
  const Stats s = stats();
  Serial.printf("[sound] %s | plays %lu (replacing %lu), stops %lu, dropped %lu, write errors %lu, underruns %lu | "
                "channel runs %lu, last %lu ms for a %lu ms clip | stack free %u B, DMA %lu B\n",
                busy() ? "playing" : "idle", (unsigned long)s.plays, (unsigned long)s.replaced, (unsigned long)s.stops,
                (unsigned long)s.dropped, (unsigned long)s.writeErrors, (unsigned long)s.underruns, (unsigned long)s.runs,
                (unsigned long)s.lastRunMs, (unsigned long)s.lastClipMs, (unsigned)stack_free_bytes(),
                (unsigned long)s.dmaBytes);
}
