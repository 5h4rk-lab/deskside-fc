#include "app_host.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>

#include <atomic>
#include <memory>
#include <new>
#include <string>

#include "app.h"
#include "goal_detector.h"
#include "host_loop.h"
#include "power_policy.h"
#include "image_provider.h"
#include "screens/goal.h"
#include "format.h"
#include "settings.h"
#include "store.h"
#include "sound_synth.h"

#if APP_DEMO_DATA
#include "demo/demo_fixtures.gen.h"
#include "demo_data.h"
#include "mock_source.h"
#else
#include "http_image_provider.h"
#include "http_source.h"
#include "lvgl_image_decoder.h"
#include "platform/esp_http_transport.h"
#include "platform/time_sync.h"
#include "platform/timed_image_decoder.h"
#endif

namespace {

#if APP_DEMO_DATA
// The demo clock starts here (2026-10-20 20:00:00 UTC, a Tuesday evening: the
// demo matches are laid out around it) and advances with the monotonic clock.
constexpr int64_t kDemoEpoch = 1792526400;
#endif
constexpr uint32_t kTickMs = 10;
constexpr int kQueueLength = 8;

struct Command {
  AppCommand what;
  int arg;
  void (*fn)();        // AppCommand::Call only
  void (*fnArg)(int);  // AppCommand::CallArg only: called with arg
};

// The demo build and a device without a helper: every picture request is
// answered "not available", the UI keeps its placeholders.
class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

int64_t monotonicMs() { return esp_timer_get_time() / 1000; }

#if !APP_DEMO_DATA
// Without a valid helper URL and token: no request ever goes out and none is
// answered (the Poller retries after its lost-fetch time, which costs nothing),
// so the data status stays "Waiting for the first update...".
class NoHelperSource : public DataSource {
 public:
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)>) override {}
  void fetch_detail(const std::string&, std::function<void(bool, MatchDetail)>) override {}
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)>) override {}
  void fetch_teams(std::function<void(bool, std::vector<Team>)>) override {}
};

EspWifiService* s_readyWifi = nullptr;
#if APP_DIAG
std::atomic<bool> s_diagOffline{false};  // :diag-offline: refuse every request as if the router were off
#endif

// The transport's gate (LVGL task and HTTP worker): an address and a valid clock.
bool networkReady() {
#if APP_DIAG
  if (s_diagOffline.load()) return false;
#endif
  return s_readyWifi != nullptr && s_readyWifi->link_up() && epoch_now_if_valid() > 0;
}
#endif

#if APP_DIAG
bool s_powerDryRun = false;     // the next power-off skips the GPIO7 write (diagnostics only)
int64_t s_diagKeyUntilMs = -1;  // an injected key press lasts until then
#endif

// The board side of PowerController (LVGL task). Every step is logged with
// the time, so the order can be read off the serial console.
class BoardPowerHost : public PowerHost {
 public:
  BoardPowerHost(Backlight& backlight, EspWifiService& wifi) : backlight_(backlight), wifi_(wifi) {}
  void attach(lv_obj_t* app, I2sSoundOut* speaker) {
    app_ = app;
    speaker_ = speaker;
  }
  bool off() const { return off_; }

  void toggle_screen() override {
    if (!app_) return;
    const bool off = !app_screen_off(app_);
    app_set_screen_off(app_, off);
    log(off ? "short press: screen off (a touch or the key wakes it)" : "short press: screen on");
  }
  bool screen_on() override {
    if (!app_ || !app_screen_off(app_)) return false;
    app_set_screen_off(app_, false);
    return true;
  }
  void screen_off() override {
    if (app_) app_set_screen_off(app_, true);
    log("hold ended early: the screen it lit is off again");
  }
  void show_hold_hint(bool show) override {
    if (app_) app_show_power_hint(app_, show);
    log(show ? "held 1 s: 'Hold to power off'" : "hint gone");
  }
  void stop_sound() override {
    if (speaker_ && speaker_->ok()) speaker_->stop();
    log("power-off 1/7: speaker stopped");
  }
  void wait_sound_idle(uint32_t timeoutMs) override {
    const int64_t t0 = monotonicMs();
    const bool idle = !(speaker_ && speaker_->ok()) || speaker_->wait_idle(timeoutMs);
    Serial.printf("[power] %lld ms power-off 2/7: speaker %s after %lld ms (limit %lu ms)\n", (long long)monotonicMs(),
                  idle ? "silent" : "STILL BUSY", (long long)(monotonicMs() - t0), (unsigned long)timeoutMs);
  }
  void show_powering_off() override {
    if (app_) app_show_powering_off(app_);
    log("power-off 3/7: 'Powering off' shown (1.5 s)");
  }
  void save_prefs() override {
    if (app_) app_save_settings(app_);
    log("power-off 4/7: settings saved");
  }
  void stop_wifi() override {
    const bool ok = wifi_.stop_radio(1000);
    log(ok ? "power-off 5/7: Wi-Fi stopped" : "power-off 5/7: Wi-Fi did NOT confirm the stop within 1 s");
  }
  void cut_power() override {
#if APP_DIAG
    if (s_powerDryRun) {
      log("power-off 6/7: DRY RUN, GPIO7 stays HIGH (a real power-off drives it low here)");
      return;
    }
#endif
    log("power-off 6/7: GPIO7 low (on battery the board goes dark once the key is up)");
    Serial.flush();
    power_hold_cut();
  }
  void enter_off_state() override {
    off_ = true;
    backlight_.set_percent(0);
    log("power-off 7/7: still powered (USB, or the key still held): off state, backlight 0, a key press restarts");
  }
  void wake_from_off() override {
    log("key pressed in the off state: restarting");
    Serial.flush();
    delay(50);
    esp_restart();
  }

 private:
  static void log(const char* what) { Serial.printf("[power] %lld ms %s\n", (long long)monotonicMs(), what); }

  Backlight& backlight_;
  EspWifiService& wifi_;
  lv_obj_t* app_ = nullptr;
  I2sSoundOut* speaker_ = nullptr;
  bool off_ = false;
};

struct Host {
  Host(Prefs& p, Backlight& b, EspWifiService& w, EspPowerService& pw, I2sSoundOut* sp, const HelperConfig& helper)
      : prefs(p),
        backlight(b),
        wifi(w),
        power(pw),
#if !APP_DEMO_DATA
        transport("data", EspHttpTransport::kJsonCapBytes, "application/json", &networkReady),
        http(helper.valid ? new (std::nothrow) HttpSource(transport, helper.url, helper.token) : nullptr),
        source(http ? static_cast<DataSource&>(*http) : static_cast<DataSource&>(none)),
        // Pictures come in bursts (a whole list of crests): one kept connection.
        imageTransport("images", EspHttpTransport::kImageCapBytes, "image/png, image/jpeg", &networkReady, true),
        decoder(lvglDecoder),
        httpImages(helper.valid ? new (std::nothrow) HttpImageProvider(
                                      imageTransport, decoder, helper.url, helper.token,
                                      HttpImageProvider::kDefaultCacheBytes, HttpImageProvider::kDeviceMaxNegative,
                                      HttpImageProvider::kDeviceMaxQueued)
                                : nullptr),
        images(httpImages ? static_cast<ImageProvider&>(*httpImages) : static_cast<ImageProvider&>(noImages)),
#endif
        deps{store, images, source, nullptr, prefs, &backlight, &wifi,
             // A join made on the Wi-Fi pages: fetch everything now (auto-joins are
             // noticed by the link watch in onTick, which does the same).
             [this] { loop.poller().request_all_now(); }},
        loop(store, source, makeHooks()),
        speaker(sp),
        powerHost(b, w),
        powerKey(powerHost, 0) {
    (void)helper;
    // A speaker that failed to start (logged once by begin()) gets no Sound rows.
    deps.sound = speaker && speaker->ok() ? speaker : nullptr;
    deps.power = &power;
  }

  HostLoop::Hooks makeHooks() {
    HostLoop::Hooks h;
#if !APP_DEMO_DATA
    // Data answers all at once; at most one picture per tick (its decode runs
    // here, on the LVGL task: a burst of pictures must not stall a frame).
    h.pollTransports = [this] {
      transport.poll();
      imageTransport.poll(1);
    };
    h.tickImages = [this](int64_t ms) {
      if (!httpImages) return;
      // Offline (no address or no valid clock yet): pictures wait instead of failing.
      httpImages->set_online(networkReady());
      httpImages->tick(ms);
    };
#endif
    h.openMatchId = [this] { return app ? app_open_match_id(app) : std::string(); };
    h.onMatchesChanged = [this](int64_t now) {
      if (!firstData) {
        firstData = true;
        Serial.printf("[data] first matches: %u matches at %lld ms after start (link up at %lld ms)\n",
                      (unsigned)store.matches().size(), (long long)(esp_timer_get_time() / 1000),
                      (long long)linkUpAtMs);
      }
      if (app) app_matches_changed(app, now);
    };
    h.refresh = [this](int64_t now) {
      if (app) app_refresh(app, now);
    };
    return h;
  }

  Prefs& prefs;
  Backlight& backlight;
  EspWifiService& wifi;
  EspPowerService& power;
  Store store;
  NoImages noImages;
#if APP_DEMO_DATA
  MockSource source;
  ImageProvider& images = noImages;
#else
  EspHttpTransport transport;  // before the source, which holds a reference to it
  std::unique_ptr<HttpSource> http;
  NoHelperSource none;
  DataSource& source;
  EspHttpTransport imageTransport;  // before the provider, which holds a reference to it
  LvglImageDecoder lvglDecoder;
  TimedImageDecoder decoder;
  std::unique_ptr<HttpImageProvider> httpImages;
  ImageProvider& images;
  bool linkWasUp = false;
  bool clockWasValid = false;
  int lastUntimedPercent = -1;
#endif
  bool firstData = false;
  int64_t linkUpAtMs = -1;
  AppDeps deps;
  HostLoop loop;
  I2sSoundOut* speaker;
  lv_obj_t* app = nullptr;
  BoardPowerHost powerHost;
  PowerController powerKey;
  int64_t keySampledAt = -1000000;
  bool keyLevel = false;
};

Host* s_host = nullptr;
QueueHandle_t s_queue = nullptr;

int64_t epochNow() {
#if APP_DEMO_DATA
  return kDemoEpoch + monotonicMs() / 1000;
#else
  return epoch_now_if_valid();
#endif
}

#if APP_DEMO_DATA
void loadDemo() {
  demo_load(s_host->source, epochNow(), demo_fixtures::kNewsJson, demo_fixtures::kTeamsJson);
}
#else
// A celebration for the favourite without touching the data (serial g / w in
// the real-data build), to check the animation while polling runs.
void testCelebration(bool fullTime) {
  const Settings* s = s_host->app ? app_settings(s_host->app) : nullptr;
  GoalEvent e;
  e.matchId = "serial-test";
  e.favourite = true;
  e.fullTime = fullTime;
  e.side = Side::Home;
  e.clock = fullTime ? "FT" : "67'";
  e.homeScore = 2;
  e.awayScore = 1;
  e.homeAbbr = s && !s->favourite.abbr.empty() ? s->favourite.abbr : "FAV";
  e.awayAbbr = "OPP";
  goal_celebrate(e, s_host->images);
  Serial.printf("[cmd] test %s celebration for %s (data untouched)\n", fullTime ? "full-time" : "goal",
                e.homeAbbr.c_str());
}

// Before the clock is valid app_refresh() does not run, so nothing drives the
// backlight: give it the saved day brightness (no night dimming without a
// clock). Once refreshes run, app_refresh() takes over.
void untimedBacklight() {
  const Settings* s = app_settings(s_host->app);
  if (!s || app_screen_off(s_host->app)) {
    s_host->lastUntimedPercent = -1;  // set again when the screen comes back on
    return;
  }
  if (s->dayBrightness == s_host->lastUntimedPercent) return;
  s_host->lastUntimedPercent = s->dayBrightness;
  s_host->backlight.set_percent(static_cast<uint8_t>(s->dayBrightness));
}

// Wi-Fi link and clock transitions (LVGL task, every tick).
void watchNetwork(int64_t epoch) {
  Host& h = *s_host;
  const bool up = h.wifi.link_up();
  if (up && !h.linkWasUp) {
    if (h.linkUpAtMs < 0) h.linkUpAtMs = monotonicMs();
    time_sync_start();  // once; needs the network stack, which is up now
    h.loop.poller().request_all_now();
    Serial.printf("[net] Wi-Fi link up: %s\n",
                  epoch > 0 ? "fetching everything now" : "waiting for the clock before any request");
  } else if (!up && h.linkWasUp) {
    Serial.println("[net] Wi-Fi link down: requests are refused until it returns");
  }
  h.linkWasUp = up;
  if (epoch > 0 && !h.clockWasValid) {
    const time_t t = static_cast<time_t>(epoch);
    struct tm utc;
    gmtime_r(&t, &utc);
    Serial.printf("[time] clock valid: %04d-%02d-%02d %02d:%02d:%02d UTC (after %lld ms)%s\n", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, (long long)monotonicMs(),
                  h.http ? "" : "; no valid helper URL/token, so no requests (:show-config)");
  } else if (epoch <= 0 && h.clockWasValid) {
    Serial.println("[time] clock no longer valid: polling and refreshes pause");
  }
  h.clockWasValid = epoch > 0;
}
#endif

// Names and numbers only: nothing secret is stored in the settings.
void printSettings() {
  const Settings* s = s_host && s_host->app ? app_settings(s_host->app) : nullptr;
  if (!s) return;
  static const char* const kBattery[] = {"Auto", "Installed", "None"};
  Serial.printf("[app] settings: favourite %s (id %s), zone %s, brightness %d%%, night dim %s %d-%d h at %d%%, reduced motion %s, battery %s\n",
                s->favourite.name.c_str(), s->favourite.id.c_str(), s->zoneId.c_str(), s->dayBrightness,
                s->nightDim ? "on" : "off", s->nightStartHour, s->nightEndHour, s->nightPercent,
                s->reducedMotion ? "on" : "off", kBattery[static_cast<int>(s->battery) % 3]);
}

// The serial :sound command: one clip (or the test tone) now, at the saved
// volume unless the command named one; the settings and the gate are not asked.
void playSoundCommand(int arg) {
  Host& h = *s_host;
  if (!h.speaker || !h.speaker->ok()) {
    Serial.println("[cmd] sound: no speaker (init failed at start, see [sound] above)");
    return;
  }
  if (arg == kSoundArgStop) {
    h.speaker->stop();
    Serial.println("[cmd] sound: stop");
    return;
  }
  const Settings* s = h.app ? app_settings(h.app) : nullptr;
  int volume = (arg >> 8) & 0xFF;
  if (volume == 0) volume = s ? s->soundVolume : 40;
  const int what = arg & 0x0F;
  if (what == kSoundArgTone) {
    // 440 Hz for 1 s on one channel: which speaker sounds tells left from right.
    static const PcmClip tone = sound_test_tone(440, 1000, 12000);  // PSRAM, made on first use, kept
    const SoundChannels ch = static_cast<SoundChannels>((arg >> 4) & 0x3);
    h.speaker->play_on(tone, static_cast<uint8_t>(volume), ch);
    Serial.printf("[cmd] sound: test tone 440 Hz 1 s on %s at %d%%\n",
                  ch == SoundChannels::Left ? "the LEFT channel only"
                                            : (ch == SoundChannels::Right ? "the RIGHT channel only" : "both channels"),
                  volume);
    return;
  }
  if (what >= kSoundEventCount) return;
  const SoundEvent e = static_cast<SoundEvent>(what);
  const uint32_t t0 = static_cast<uint32_t>(esp_timer_get_time());
  const PcmClip& clip = sound_clip(e);
  const uint32_t lookupUs = static_cast<uint32_t>(esp_timer_get_time()) - t0;
  h.speaker->play(clip, static_cast<uint8_t>(volume));
  Serial.printf("[cmd] sound: event %d, %d ms clip (expected %d ms) at %d%%, clip ready in %lu us\n", what,
                clip.duration_ms(), sound_clip_duration_ms(e), volume, (unsigned long)lookupUs);
}

void runCommand(const Command& c) {
  Host& h = *s_host;
  // Switching off or off: nothing may celebrate or make a sound any more.
  if (h.powerKey.mode() != PowerController::Mode::On &&
      (c.what == AppCommand::Goal || c.what == AppCommand::Win || c.what == AppCommand::Sound ||
       c.what == AppCommand::ReloadDemo)) {
    Serial.println("[cmd] ignored: the board is switching off or off");
    return;
  }
  switch (c.what) {
#if APP_DEMO_DATA
    case AppCommand::Goal:
      if (demo_goal(h.source, h.store.favourite())) {
        h.loop.poller().request_matches_now();
        Serial.println("[cmd] goal: the favourite scores in the demo match");
      } else {
        Serial.println("[cmd] goal: the favourite has no live match (d reloads the demo data)");
      }
      break;
    case AppCommand::Win: {
      const DemoWin r = demo_win(h.source, h.store.favourite());
      if (r == DemoWin::Ok) {
        h.loop.poller().request_matches_now();
        Serial.println("[cmd] win: full time, the favourite wins");
      } else if (r == DemoWin::NotAhead) {
        Serial.println("[cmd] win: the favourite is not ahead (press g first)");
      } else {
        Serial.println("[cmd] win: the favourite has no live match (d reloads the demo data)");
      }
      break;
    }
    case AppCommand::ReloadDemo:
      loadDemo();
      h.loop.poller().request_all_now();
      Serial.println("[cmd] demo data reloaded");
      break;
#else
    case AppCommand::Goal:
      testCelebration(false);
      break;
    case AppCommand::Win:
      testCelebration(true);
      break;
    case AppCommand::ReloadDemo:
      h.loop.poller().request_all_now();
      Serial.println("[cmd] fetching everything now");
      break;
#endif
    case AppCommand::Tab:
      if (c.arg >= 0 && c.arg <= 2 && h.app) {
        lv_tabview_set_active(h.app, static_cast<uint32_t>(c.arg), LV_ANIM_OFF);
        Serial.printf("[cmd] tab %d\n", c.arg);
      } else {
        Serial.printf("[cmd] tab %d: there are tabs 0 to 2\n", c.arg);
      }
      break;
    case AppCommand::NextTeam: {
      const std::vector<Team>& teams = h.store.teams();
      if (teams.empty() || !h.app) {
        Serial.println("[cmd] team: no team list");
        break;
      }
      size_t next = 0;
      for (size_t i = 0; i < teams.size(); ++i) {
        if (teams[i].id == h.store.favourite()) next = (i + 1) % teams.size();
      }
      app_apply_team(h.app, teams[next]);
      printSettings();
      break;
    }
    case AppCommand::Settings:
      printSettings();
      break;
    case AppCommand::Sound:
      playSoundCommand(c.arg);
      break;
    case AppCommand::Call:
      if (c.fn) c.fn();
      break;
    case AppCommand::CallArg:
      if (c.fnArg) c.fnArg(c.arg);
      break;
#if APP_DIAG
    case AppCommand::Churn: {
      static lv_timer_t* churn = nullptr;
      if (churn) {
        lv_timer_delete(churn);
        churn = nullptr;
      } else {
        churn = lv_timer_create([](lv_timer_t*) {
          const GoalParts* gp = goal_parts();
          lv_obj_invalidate(gp ? gp->overlay : lv_screen_active());
        }, 5, nullptr);
      }
      Serial.printf("[cmd] churn %s\n", churn ? "on (overlay repainted every frame)" : "off");
      break;
    }
    case AppCommand::Hide: {
      const GoalParts* p = goal_parts();
      if (!p) {
        Serial.println("[cmd] hide: no goal celebration is showing");
        break;
      }
      lv_obj_t* part = nullptr;
      switch (c.arg) {
        case 0: part = p->rays; break;
        case 1: part = p->confetti.empty() ? nullptr : p->confetti[0]; break;
        case 2: part = p->stamp; break;
        case 3: part = p->cutout; break;
        case 4: part = p->strip; break;
        case 5: part = p->stampImage; break;
        default: break;
      }
      if (c.arg == 1) {
        // All confetti pieces together.
        const bool hide = !p->confetti.empty() && !lv_obj_has_flag(p->confetti[0], LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* piece : p->confetti) {
          if (hide) lv_obj_add_flag(piece, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(piece, LV_OBJ_FLAG_HIDDEN);
        }
        Serial.printf("[cmd] hide: confetti %s\n", hide ? "hidden" : "shown");
      } else if (part) {
        const bool hide = !lv_obj_has_flag(part, LV_OBJ_FLAG_HIDDEN);
        if (hide) lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(part, LV_OBJ_FLAG_HIDDEN);
        Serial.printf("[cmd] hide: part %d %s\n", c.arg, hide ? "hidden" : "shown");
      } else {
        Serial.printf("[cmd] hide: no part %d (0 rays, 1 confetti, 2 stamp box, 3 cutout, 4 strip, 5 baked stamp)\n", c.arg);
      }
      break;
    }
#endif
  }
}

// The power key, every tick: one pin read; the controller gets a sample when
// it asked for one (every 10 ms, or every 10 s in the off state) or the level
// changed.
void powerKeyTick() {
  Host& h = *s_host;
  const int64_t now = monotonicMs();
  bool level = power_key_down();
#if APP_DIAG
  if (now < s_diagKeyUntilMs) level = true;
#endif
  if (now - h.keySampledAt >= h.powerKey.poll_interval_ms() || level != h.keyLevel) {
    h.powerKey.update(level, now);
    h.keySampledAt = now;
  }
  h.keyLevel = level;
}

// Runs in the LVGL task every kTickMs, inside lv_timer_handler().
void onTick(lv_timer_t*) {
  Command c;
  while (s_queue && xQueueReceive(s_queue, &c, 0) == pdTRUE) runCommand(c);
  powerKeyTick();
  // Switching off or off: no data, no refresh, nothing new to draw.
  if (s_host->powerKey.mode() != PowerController::Mode::On) return;
  s_host->wifi.pump();  // driver events and timers of the Wi-Fi service; its callbacks run here
  const int64_t epoch = epochNow();
#if !APP_DEMO_DATA
  watchNetwork(epoch);
  if (epoch <= 0) untimedBacklight();
#endif
  s_host->loop.tick(monotonicMs(), epoch);
}

}  // namespace

// Internal heap in use between start-up steps (serial), to see which one costs.
static void heapStep(const char* what) {
  static size_t last = 0;
  const size_t now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  Serial.printf("[heap] %-22s internal free %6u (%+d) psram free %7u\n", what, (unsigned)now,
                last ? (int)now - (int)last : 0, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  last = now;
}

bool app_host_start(Prefs& prefs, Backlight& backlight, EspWifiService& wifi, EspPowerService& power,
                    I2sSoundOut* speaker, const HelperConfig& helper) {
  if (s_host) return true;
  heapStep("before the app");
  s_queue = xQueueCreate(kQueueLength, sizeof(Command));
  if (!s_queue) {
    Serial.println("[app] ERROR: cannot create the command queue (serial commands off)");
  }
#if !APP_DEMO_DATA
  s_readyWifi = &wifi;
#endif
  s_host = new (std::nothrow) Host(prefs, backlight, wifi, power, speaker, helper);
  if (!s_host) {
    Serial.println("[app] ERROR: out of memory for the app state");
    return false;
  }
#if !APP_DEMO_DATA
  if (helper.valid && !s_host->http) Serial.println("[app] ERROR: out of memory for the data source");
  if (!s_host->transport.start()) Serial.println("[app] the data transport did not start: no live data");
  if (helper.valid && !s_host->httpImages) Serial.println("[app] ERROR: out of memory for the picture provider");
  if (s_host->httpImages) {
    s_host->httpImages->set_entry_overhead_bytes(HttpImageProvider::kDeviceEntryOverheadBytes);
  }
  s_host->imageTransport.set_min_internal_free(EspHttpTransport::kImagesMinInternalFree);
  if (s_host->httpImages && !s_host->imageTransport.start()) {
    Serial.println("[app] the picture transport did not start: placeholders only");
  }
#endif
  heapStep("app state allocated");
#if APP_DEMO_DATA
  loadDemo();
  heapStep("demo data loaded");
#endif
  lv_obj_t* app = app_create(lv_screen_active(), s_host->deps);
  if (!app) {
    Serial.println("[app] ERROR: app_create failed (out of memory)");
    return false;
  }
  s_host->app = app;
  s_host->powerHost.attach(app, s_host->speaker);
  heapStep("app_create done");
  printSettings();
#if APP_DEMO_DATA
  // First paint with data: the poller's first ticks fill the Store at once
  // (the demo source answers synchronously), twice so the favourite's live
  // detail follows the matches; then one refresh and the loop takes over.
  const int64_t now = epochNow();
  s_host->loop.pump(monotonicMs(), now);
  s_host->loop.pump(monotonicMs(), now);
  heapStep("first pumps done");
  app_refresh(app, now);
  heapStep("first refresh done");
  s_host->loop.note_refreshed(now);
#else
  // No clock yet: the screens show "Connecting..." until SNTP has set it; the
  // first valid tick fetches everything and refreshes.
  untimedBacklight();
#endif
  if (!lv_timer_create(onTick, kTickMs, nullptr)) {
    Serial.println("[app] ERROR: cannot create the host loop timer (out of memory)");
    return false;
  }
  return true;
}

void app_host_post(AppCommand command, int arg) {
  if (!s_queue) return;
  const Command c{command, arg, nullptr, nullptr};
  if (xQueueSend(s_queue, &c, 0) != pdTRUE) Serial.println("[cmd] queue full, command dropped");
}

int64_t app_host_epoch() { return epochNow(); }

void app_host_print_power() {
  if (!s_host) return;
  const Settings* s = s_host->app ? app_settings(s_host->app) : nullptr;
  s_host->power.print_status(s ? s->battery : BatteryChoice::Auto);
  const bool dark = s_host->powerHost.off() || (s_host->app && app_screen_off(s_host->app));
  Serial.printf("[power] screen %s, mode %s, %s\n", dark ? "OFF" : "on",
                s_host->powerKey.mode() == PowerController::Mode::On
                    ? "on"
                    : (s_host->powerKey.mode() == PowerController::Mode::PoweringOff ? "powering off" : "OFF STATE"),
                s_host->app && app_battery_banner(s_host->app) ? "low-battery banner SHOWING" : "no banner");
}

#if APP_DIAG
void app_host_diag_power_off() {
  if (!s_host) return;
  s_powerDryRun = true;
  Serial.printf("[diag] %lld ms power-off DRY RUN started (GPIO7 is not touched)\n", (long long)monotonicMs());
  s_host->powerKey.power_off();
}

void app_host_diag_key(int ms) {
  if (!s_host) return;
  s_powerDryRun = true;  // an injected long press must never cut the power
  s_diagKeyUntilMs = monotonicMs() + ms;
  Serial.printf("[diag] %lld ms power key held for %d ms (injected; any power-off is a DRY RUN)\n",
                (long long)monotonicMs(), ms);
}
#endif

void app_host_post_call(void (*fn)()) {
  if (!s_queue || !fn) return;
  const Command c{AppCommand::Call, 0, fn, nullptr};
  if (xQueueSend(s_queue, &c, 0) != pdTRUE) Serial.println("[cmd] queue full, command dropped");
}

void app_host_post_call_arg(void (*fn)(int), int arg) {
  if (!s_queue || !fn) return;
  const Command c{AppCommand::CallArg, arg, nullptr, fn};
  if (xQueueSend(s_queue, &c, 0) != pdTRUE) Serial.println("[cmd] queue full, command dropped");
}

void app_host_print_net() {
  if (!s_host) return;
  const Host& h = *s_host;
  const int64_t epoch = epochNow();
#if APP_DEMO_DATA
  Serial.printf("[net] demo data, demo clock %lld | data: %s\n", (long long)epoch, h.store.data_status_text().c_str());
#else
  const EspHttpTransport::Stats s = h.transport.stats();
  auto kb = [](size_t v) { return v == SIZE_MAX ? -1 : (int)(v / 1024); };
  Serial.printf(
      "[net] link %s, clock %s (syncs %lu) | http ok %lu failed %lu refused %lu, last %d %u B in %lu ms (handshake "
      "%lu ms), in flight %u | internal free in TLS last %d KB, min %d KB, largest min %d KB, psram min %d KB | "
      "worker stack free %u B | data: %s%s\n",
      h.wifi.link_up() ? "up" : "down", epoch > 0 ? "valid" : "NOT valid", (unsigned long)time_sync_count(),
      (unsigned long)s.ok, (unsigned long)s.failed, (unsigned long)s.refused, s.lastStatus, (unsigned)s.lastBytes,
      (unsigned long)s.lastMs, (unsigned long)s.lastHandshakeMs, (unsigned)h.transport.outstanding(),
      s.internalFreeInTls ? (int)(s.internalFreeInTls / 1024) : -1, kb(s.internalFreeInTlsMin),
      kb(s.internalLargestInTlsMin), kb(s.psramFreeInTlsMin), (unsigned)h.transport.worker_stack_free_bytes(),
      h.store.data_status_text().c_str(), epoch > 0 && h.store.stale(epoch) ? " (STALE marker on)" : "");
  if (!h.httpImages) {
    Serial.println("[img] pictures off (no helper configured)");
    return;
  }
  const HttpImageProvider& p = *h.httpImages;
  const EspHttpTransport::Stats is = h.imageTransport.stats();
  const TimedImageDecoder::Stats& d = h.decoder.stats();
  const uint32_t decodes = d.ok + d.failed;
  const uint32_t avgUs = decodes ? (uint32_t)(d.totalUs / decodes) : 0;
  Serial.printf(
      "[img] cache %u KB of %u KB, %u pictures | waiting %u, in flight %u, remembered failures %u%s%s | decoded %lu "
      "(failed %lu), last %lu.%lu ms, avg %lu.%lu ms, max %lu.%lu ms | http ok %lu failed %lu refused %lu, last %d "
      "%u B in %lu ms (handshake %lu ms), kept connection used %lu times, reopened %lu, heap guard waits %lu refusals %lu | internal free in TLS min %d KB "
      "| worker stack free %u B\n",
      (unsigned)(p.cached_bytes() / 1024), (unsigned)(HttpImageProvider::kDefaultCacheBytes / 1024),
      (unsigned)p.cached_images(), (unsigned)(p.pending() - p.in_flight()), (unsigned)p.in_flight(),
      (unsigned)p.negative_entries(), p.online() ? "" : ", OFFLINE (waiting)",
      p.auth_rejected() ? ", TOKEN REJECTED" : "", (unsigned long)d.ok, (unsigned long)d.failed,
      (unsigned long)(d.lastUs / 1000), (unsigned long)(d.lastUs % 1000 / 100), (unsigned long)(avgUs / 1000),
      (unsigned long)(avgUs % 1000 / 100), (unsigned long)(d.maxUs / 1000), (unsigned long)(d.maxUs % 1000 / 100),
      (unsigned long)is.ok, (unsigned long)is.failed, (unsigned long)is.refused, is.lastStatus,
      (unsigned)is.lastBytes, (unsigned long)is.lastMs, (unsigned long)is.lastHandshakeMs,
      (unsigned long)h.imageTransport.reuses(), (unsigned long)h.imageTransport.reconnects(), (unsigned long)h.imageTransport.heap_waits(),
      (unsigned long)h.imageTransport.heap_refusals(), kb(is.internalFreeInTlsMin),
      (unsigned)h.imageTransport.worker_stack_free_bytes());
#endif
}

#if APP_DIAG && !APP_DEMO_DATA
namespace {
int s_burstLeft = 0;
int s_burstDone = 0;
int s_burstOk = 0;
int s_burstTotal = 0;
size_t s_burstInternal0 = 0, s_burstPsram0 = 0;

void heapLine(const char* what) {
  Serial.printf("[burst] %-10s %3d/%d ok %3d | internal free %u (largest %u, min %u) psram free %u\n", what,
                s_burstDone, s_burstTotal, s_burstOk, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void burstNext() {
  if (s_burstLeft <= 0 || !s_host || !s_host->http) return;
  --s_burstLeft;
  // Through the real source and parser (the same path as a poll), one at a time.
  s_host->http->fetch_matches([](bool ok, std::vector<MatchSummary>) {
    ++s_burstDone;
    if (ok) ++s_burstOk;
    if (s_burstDone % 10 == 0) heapLine("progress");
    if (s_burstLeft > 0) {
      burstNext();
    } else {
      heapLine("end");
      Serial.printf("[burst] change over %d requests: internal %+d B, psram %+d B\n", s_burstTotal,
                    (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (int)s_burstInternal0,
                    (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (int)s_burstPsram0);
    }
  });
}
}  // namespace

void app_host_diag_burst(int n) {
  if (!s_host || !s_host->http) {
    Serial.println("[burst] no helper configured");
    return;
  }
  if (s_burstLeft > 0) {
    Serial.println("[burst] already running");
    return;
  }
  s_burstLeft = n;
  s_burstTotal = n;
  s_burstDone = 0;
  s_burstOk = 0;
  s_burstInternal0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s_burstPsram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  heapLine("start");
  burstNext();
}

void app_host_diag_slow(bool headers) {
  if (!s_host) return;
  // Public test endpoints (valid certificates, no token sent): one trickles a
  // 30-byte body over 30 s, the other holds the headers back for 10 s. Both
  // must end as a transport error after about 8000 ms (the total deadline).
  const char* url = headers ? "https://httpbin.org/delay/10"
                            : "https://httpbin.org/drip?duration=30&numbytes=30&delay=0";
  const int64_t start = monotonicMs();
  Serial.printf("[slow] GET %s with an 8000 ms total budget\n", headers ? "/delay/10 (headers after 10 s)"
                                                                       : "/drip (30 bytes over 30 s)");
  s_host->transport.get(url, std::string(), 8000, [start](HttpResult r) {
    Serial.printf("[slow] answer after %lld ms: status %d, transport error %s, %u bytes\n",
                  (long long)(monotonicMs() - start), r.status, r.transportError ? "yes" : "no",
                  (unsigned)r.body.size());
  });
}

namespace {
// :diag-img-churn: distinct real pictures (team crests at several sizes), two
// at a time through the real provider, heap and cache every 20.
int s_churnLeft = 0, s_churnDone = 0, s_churnOk = 0, s_churnTotal = 0;
size_t s_churnNext = 0;
size_t s_churnInternal0 = 0, s_churnPsram0 = 0;
int64_t s_churnStartMs = 0;

void churnLine(const char* what) {
  const HttpImageProvider& p = *s_host->httpImages;
  Serial.printf("[churn] %-8s %3d/%d ok %3d after %lld ms | cache %u KB, %u pictures | internal free %u (min %u) "
                "psram free %u (min %u)\n",
                what, s_churnDone, s_churnTotal, s_churnOk, (long long)(monotonicMs() - s_churnStartMs),
                (unsigned)(p.cached_bytes() / 1024), (unsigned)p.cached_images(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

// The n-th distinct picture: crest of team n % teams at one of seven sizes the
// screens never ask for (so every one is a real download, not a cache hit).
std::string churnPath(size_t n) {
  static const int kSizes[] = {32, 40, 56, 64, 72, 96, 120};
  const std::vector<Team>& teams = s_host->store.teams();
  const Team& t = teams[n % teams.size()];
  const int size = kSizes[(n / teams.size()) % (sizeof(kSizes) / sizeof(kSizes[0]))];
  return sized(t.crest, size, size);
}

void churnNext() {
  if (s_churnLeft <= 0 || !s_host || !s_host->httpImages) return;
  --s_churnLeft;
  const std::string path = churnPath(s_churnNext++);
  s_host->httpImages->request(path, [](ImagePtr img) {
    ++s_churnDone;
    if (img) ++s_churnOk;
    if (s_churnDone % 20 == 0) churnLine("progress");
    if (s_churnLeft > 0) {
      // Not from inside the callback: a picture answered at once (a remembered
      // failure) would otherwise recurse once per picture.
      lv_async_call([](void*) { churnNext(); }, nullptr);
    } else if (s_churnDone == s_churnTotal) {
      churnLine("end");
      Serial.printf("[churn] change over %d pictures: internal %+d B, psram %+d B (the cache holds what fits in "
                    "its budget)\n",
                    s_churnTotal, (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (int)s_churnInternal0,
                    (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (int)s_churnPsram0);
    }
  });
}
}  // namespace

void app_host_diag_img_churn(int n) {
  if (!s_host || !s_host->httpImages) {
    Serial.println("[churn] no helper configured");
    return;
  }
  if (s_host->store.teams().empty()) {
    Serial.println("[churn] no team list yet (wait for the first data)");
    return;
  }
  if (s_churnLeft > 0) {
    Serial.println("[churn] already running");
    return;
  }
  s_churnLeft = n;
  s_churnTotal = n;
  s_churnDone = 0;
  s_churnOk = 0;
  s_churnStartMs = monotonicMs();
  s_churnInternal0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s_churnPsram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  churnLine("start");
  // Two chains, like the provider's two requests in flight.
  churnNext();
  churnNext();
}

void app_host_diag_goal_photo() {
  if (!s_host) return;
  testCelebration(false);
  // A real player's cutout (190 px, the largest picture the UI asks for), to
  // time its download and decode while the celebration runs.
  goal_set_scorer("serial-test", "Ollie Watkins", "/api/cutout?name=Ollie%20Watkins&w=96&h=96");
  Serial.println("[cmd] test goal with a scorer photo (/api/cutout, 190 px)");
}

void app_host_diag_offline_toggle() {
  const bool now = !s_diagOffline.load();
  s_diagOffline.store(now);
  Serial.printf("[net] diag: %s\n", now ? "OFFLINE (every request refused, as with the router off)" : "online again");
  if (!now && s_host) s_host->loop.poller().request_all_now();
}
#endif
