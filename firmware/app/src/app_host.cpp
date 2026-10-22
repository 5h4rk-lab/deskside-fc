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
#include "image_provider.h"
#include "screens/goal.h"
#include "settings.h"
#include "store.h"

#if APP_DEMO_DATA
#include "demo/demo_fixtures.gen.h"
#include "demo_data.h"
#include "mock_source.h"
#else
#include "http_source.h"
#include "platform/esp_http_transport.h"
#include "platform/time_sync.h"
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
  void (*fn)();  // AppCommand::Call only
};

// No pictures yet (Task 7): every request is answered "not available", the UI
// keeps its placeholders.
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

struct Host {
  Host(Prefs& p, Backlight& b, EspWifiService& w, const HelperConfig& helper)
      : prefs(p),
        backlight(b),
        wifi(w),
#if !APP_DEMO_DATA
        transport("data", EspHttpTransport::kJsonCapBytes, &networkReady),
        http(helper.valid ? new (std::nothrow) HttpSource(transport, helper.url, helper.token) : nullptr),
        source(http ? static_cast<DataSource&>(*http) : static_cast<DataSource&>(none)),
#endif
        deps{store, images, source, nullptr, prefs, &backlight, &wifi,
             // A join made on the Wi-Fi pages: fetch everything now (auto-joins are
             // noticed by the link watch in onTick, which does the same).
             [this] { loop.poller().request_all_now(); }},
        loop(store, source, makeHooks()) {
    (void)helper;
  }

  HostLoop::Hooks makeHooks() {
    HostLoop::Hooks h;
#if !APP_DEMO_DATA
    h.pollTransports = [this] { transport.poll(); };
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
  Store store;
  NoImages images;
#if APP_DEMO_DATA
  MockSource source;
#else
  EspHttpTransport transport;  // before the source, which holds a reference to it
  std::unique_ptr<HttpSource> http;
  NoHelperSource none;
  DataSource& source;
  bool linkWasUp = false;
  bool clockWasValid = false;
  int lastUntimedPercent = -1;
#endif
  bool firstData = false;
  int64_t linkUpAtMs = -1;
  AppDeps deps;
  HostLoop loop;
  lv_obj_t* app = nullptr;
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
  if (!s || s->dayBrightness == s_host->lastUntimedPercent) return;
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
  Serial.printf("[app] settings: favourite %s (id %s), zone %s, brightness %d%%, night dim %s %d-%d h at %d%%, reduced motion %s\n",
                s->favourite.name.c_str(), s->favourite.id.c_str(), s->zoneId.c_str(), s->dayBrightness,
                s->nightDim ? "on" : "off", s->nightStartHour, s->nightEndHour, s->nightPercent,
                s->reducedMotion ? "on" : "off");
}

void runCommand(const Command& c) {
  Host& h = *s_host;
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
    case AppCommand::Call:
      if (c.fn) c.fn();
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

// Runs in the LVGL task every kTickMs, inside lv_timer_handler().
void onTick(lv_timer_t*) {
  Command c;
  while (s_queue && xQueueReceive(s_queue, &c, 0) == pdTRUE) runCommand(c);
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

bool app_host_start(Prefs& prefs, Backlight& backlight, EspWifiService& wifi, const HelperConfig& helper) {
  if (s_host) return true;
  heapStep("before the app");
  s_queue = xQueueCreate(kQueueLength, sizeof(Command));
  if (!s_queue) {
    Serial.println("[app] ERROR: cannot create the command queue (serial commands off)");
  }
#if !APP_DEMO_DATA
  s_readyWifi = &wifi;
#endif
  s_host = new (std::nothrow) Host(prefs, backlight, wifi, helper);
  if (!s_host) {
    Serial.println("[app] ERROR: out of memory for the app state");
    return false;
  }
#if !APP_DEMO_DATA
  if (helper.valid && !s_host->http) Serial.println("[app] ERROR: out of memory for the data source");
  if (!s_host->transport.start()) Serial.println("[app] the data transport did not start: no live data");
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
  const Command c{command, arg, nullptr};
  if (xQueueSend(s_queue, &c, 0) != pdTRUE) Serial.println("[cmd] queue full, command dropped");
}

int64_t app_host_epoch() { return epochNow(); }

void app_host_post_call(void (*fn)()) {
  if (!s_queue || !fn) return;
  const Command c{AppCommand::Call, 0, fn};
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

void app_host_diag_offline_toggle() {
  const bool now = !s_diagOffline.load();
  s_diagOffline.store(now);
  Serial.printf("[net] diag: %s\n", now ? "OFFLINE (every request refused, as with the router off)" : "online again");
  if (!now && s_host) s_host->loop.poller().request_all_now();
}
#endif
