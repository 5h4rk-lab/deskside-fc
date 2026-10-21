#include "app_host.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>

#include <new>
#include <string>

#include "app.h"
#include "demo/demo_fixtures.gen.h"
#include "demo_data.h"
#include "host_loop.h"
#include "image_provider.h"
#include "mock_source.h"
#include "screens/goal.h"
#include "settings.h"
#include "store.h"

#if !APP_DEMO_DATA
#error "this task builds the demo-data app only (-DAPP_DEMO_DATA=1)"
#endif

namespace {

// The demo clock starts here (2026-10-20 20:00:00 UTC, a Tuesday evening: the
// demo matches are laid out around it) and advances with the monotonic clock.
constexpr int64_t kDemoEpoch = 1792526400;
constexpr uint32_t kTickMs = 10;
constexpr int kQueueLength = 8;

struct Command {
  AppCommand what;
  int arg;
  void (*fn)();  // AppCommand::Call only
};

// No pictures yet (they come with the helper): every request is answered
// "not available", the UI keeps its placeholders.
class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

struct Host {
  Host(Prefs& p, Backlight& b)
      : prefs(p),
        backlight(b),
        deps{store, images, source, nullptr, prefs, &backlight},
        loop(store, source, makeHooks()) {}

  HostLoop::Hooks makeHooks() {
    HostLoop::Hooks h;
    h.openMatchId = [this] { return app ? app_open_match_id(app) : std::string(); };
    h.onMatchesChanged = [this](int64_t now) {
      if (app) app_matches_changed(app, now);
    };
    h.refresh = [this](int64_t now) {
      if (app) app_refresh(app, now);
    };
    return h;
  }

  Prefs& prefs;
  Backlight& backlight;
  Store store;
  NoImages images;
  MockSource source;
  AppDeps deps;
  HostLoop loop;
  lv_obj_t* app = nullptr;
};

Host* s_host = nullptr;
QueueHandle_t s_queue = nullptr;

int64_t monotonicMs() { return esp_timer_get_time() / 1000; }
int64_t epochNow() { return kDemoEpoch + monotonicMs() / 1000; }

void loadDemo() {
  demo_load(s_host->source, epochNow(), demo_fixtures::kNewsJson, demo_fixtures::kTeamsJson);
}

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
        Serial.printf("[cmd] hide: no part %d (0 rays, 1 confetti, 2 headline, 3 cutout, 4 strip)\n", c.arg);
      }
      break;
    }
  }
}

// Runs in the LVGL task every kTickMs, inside lv_timer_handler().
void onTick(lv_timer_t*) {
  Command c;
  while (s_queue && xQueueReceive(s_queue, &c, 0) == pdTRUE) runCommand(c);
  s_host->loop.tick(monotonicMs(), epochNow());
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

bool app_host_start(Prefs& prefs, Backlight& backlight) {
  if (s_host) return true;
  heapStep("before the app");
  s_queue = xQueueCreate(kQueueLength, sizeof(Command));
  if (!s_queue) {
    Serial.println("[app] ERROR: cannot create the command queue (serial commands off)");
  }
  s_host = new (std::nothrow) Host(prefs, backlight);
  if (!s_host) {
    Serial.println("[app] ERROR: out of memory for the app state");
    return false;
  }
  heapStep("app state allocated");
  loadDemo();
  heapStep("demo data loaded");
  lv_obj_t* app = app_create(lv_screen_active(), s_host->deps);
  if (!app) {
    Serial.println("[app] ERROR: app_create failed (out of memory)");
    return false;
  }
  s_host->app = app;
  heapStep("app_create done");
  printSettings();
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
