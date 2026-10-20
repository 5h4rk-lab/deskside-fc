// Desktop simulator: runs the shared UI code in an SDL window.
// Usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]
//            [--screen VIEW] [--match ID]
//   --scale N        window is 240*N x 320*N, LVGL stays 240x320 (default 2)
//   --assets DIR     image assets (default: the repo's sim/assets)
//   --fixtures       load the contract fixtures instead of the demo data
//   --snapshot PATH  render once without a window, write a PNG and exit
//   --screen VIEW    live (default), matches (the Matches tab), news (the News
//                    tab), or the match
//                    detail: detail (its default sub-tab), summary, pitch or
//                    stats
//   --match ID       match the detail opens (default: the live demo match;
//                    demo-rma-bar is a pre-match with no lineups yet)
#include <SDL2/SDL.h>
#include <lvgl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include "app.h"
#include "data_source.h"
#include "demo_data.h"
#include "mock_image_provider.h"
#include "mock_source.h"
#include "screens/detail.h"
#include "snapshot.h"
#include "store.h"

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 320;
constexpr int kDefaultScale = 2;
constexpr int64_t kPollSeconds = 30;

struct Options {
  int scale = kDefaultScale;
  std::string assets = SIM_ASSETS_DIR;
  bool fixtures = false;
  std::string snapshot;
  std::string screen = "live";
  std::string match = demo::kLiveMatchId;
};

[[noreturn]] void usage(int code) {
  std::printf(
      "usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]\n"
      "           [--screen live|matches|news|detail|summary|pitch|stats] [--match ID]\n");
  std::exit(code);
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const bool hasValue = i + 1 < argc;
    if (std::strcmp(argv[i], "--scale") == 0 && hasValue) {
      o.scale = std::atoi(argv[++i]);
      if (o.scale < 1 || o.scale > 8) {
        std::fprintf(stderr, "invalid --scale '%s' (1-8)\n", argv[i]);
        std::exit(2);
      }
    } else if (std::strcmp(argv[i], "--assets") == 0 && hasValue) {
      o.assets = argv[++i];
    } else if (std::strcmp(argv[i], "--snapshot") == 0 && hasValue) {
      o.snapshot = argv[++i];
    } else if (std::strcmp(argv[i], "--screen") == 0 && hasValue) {
      o.screen = argv[++i];
      const char* const kScreens[] = {"live", "matches", "news", "detail", "summary", "pitch", "stats"};
      bool known = false;
      for (const char* k : kScreens) known = known || o.screen == k;
      if (!known) {
        std::fprintf(stderr, "invalid --screen '%s'\n", o.screen.c_str());
        usage(2);
      }
    } else if (std::strcmp(argv[i], "--match") == 0 && hasValue) {
      o.match = argv[++i];
    } else if (std::strcmp(argv[i], "--fixtures") == 0) {
      o.fixtures = true;
    } else if (std::strcmp(argv[i], "--help") == 0) {
      usage(0);
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", argv[i]);
      usage(2);
    }
  }
  return o;
}

uint32_t sdl_ticks() { return SDL_GetTicks(); }

uint32_t steady_ms() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}

// Offset of the host's local time from UTC, in seconds.
int local_tz_offset(time_t now) {
  struct tm local = *std::localtime(&now);
  struct tm utc = *std::gmtime(&now);
  utc.tm_isdst = local.tm_isdst;
  return static_cast<int>(std::difftime(std::mktime(&local), std::mktime(&utc)));
}

// DataSource over the demo data. Callbacks run synchronously.
class DemoSource : public DataSource {
 public:
  void set_now(int64_t now) { now_ = now; }

  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    cb(true, demo_matches(now_));
  }
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override {
    MatchDetail d = demo_detail(id);
    if (d.match.home.abbr.empty()) {
      cb(false, {});  // not a demo match
      return;
    }
    // Same kick-off times as the match list.
    for (const MatchSummary& m : demo_matches(now_)) {
      if (m.id == id) d.match = m;
    }
    cb(true, std::move(d));
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override {
    cb(true, demo_news(now_));
  }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override { cb(true, {}); }

 private:
  int64_t now_ = 0;
};

// Fills the store from the demo data or the contract fixtures.
class Feed {
 public:
  Feed(Store& store, bool fixtures) : store_(store) {
    if (fixtures) {
      source_.reset(new MockSource(CONTRACT_FIXTURES_DIR));
    } else {
      demo_ = new DemoSource();
      source_.reset(demo_);
    }
  }

  DataSource& source() { return *source_; }

  void poll(int64_t now) {
    if (demo_ != nullptr) demo_->set_now(now);
    source_->fetch_matches([&](bool ok, std::vector<MatchSummary> matches) {
      if (ok) {
        store_.set_matches(std::move(matches), now);
      } else {
        store_.mark_failure();
      }
    });
    source_->fetch_teams([&](bool ok, std::vector<Team> teams) {
      if (ok) store_.set_teams(std::move(teams));
    });
    source_->fetch_news([&](bool ok, std::vector<NewsItem> news) {
      if (ok) store_.set_news(std::move(news));
    });
    if (const MatchSummary* live = store_.live_for(store_.favourite())) {
      source_->fetch_detail(live->id, [&](bool ok, MatchDetail detail) {
        if (ok) store_.set_detail(std::move(detail));
      });
    }
  }

 private:
  Store& store_;
  std::unique_ptr<DataSource> source_;
  DemoSource* demo_ = nullptr;  // owned by source_ in demo mode
};

void discard_flush(lv_display_t* disp, const lv_area_t*, uint8_t*) {
  lv_display_flush_ready(disp);
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  const bool headless = !opt.snapshot.empty();

  lv_init();
  lv_tick_set_cb(headless ? steady_ms : sdl_ticks);

  lv_display_t* disp = nullptr;
  if (headless) {
    static uint8_t buf[kWidth * 40 * 2];
    disp = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, discard_flush);
  } else {
    disp = lv_sdl_window_create(kWidth, kHeight);
    if (!disp) {
      std::fprintf(stderr, "could not create SDL window\n");
      return 1;
    }
    lv_sdl_window_set_title(disp, "Soccer display simulator");
    lv_sdl_window_set_zoom(disp, static_cast<float>(opt.scale));
    lv_sdl_mouse_create();
  }

  Store store;
  store.set_favourite(demo::kFavouriteId);
  const time_t start = std::time(nullptr);
  store.set_tz_offset(local_tz_offset(start));
  MockImageProvider images(opt.assets);
  Feed feed(store, opt.fixtures);
  feed.poll(start);

  AppDeps deps{store, images, feed.source(), [](const std::string& id) {
                 std::printf("open match %s\n", id.c_str());
                 std::fflush(stdout);
               }};
  lv_obj_t* app = app_create(lv_screen_active(), deps);
  app_refresh(app, start);

  if (opt.screen == "matches") {
    lv_tabview_set_active(app, 1, LV_ANIM_OFF);
    app_refresh(app, start);
  } else if (opt.screen == "news") {
    lv_tabview_set_active(app, 2, LV_ANIM_OFF);
    app_refresh(app, start);
  } else if (opt.screen != "live") {
    app_open_match(app, opt.match);
    lv_obj_t* view = app_detail_view(app);
    if (opt.screen == "summary") detail_select_tab(view, DetailTab::Summary, false);
    if (opt.screen == "pitch") detail_select_tab(view, DetailTab::Pitch, false);
    if (opt.screen == "stats") detail_select_tab(view, DetailTab::Stats, false);
  }

  if (headless) {
    for (int i = 0; i < 10; ++i) lv_timer_handler();
    const bool ok = snapshot_png(lv_screen_active(), opt.snapshot.c_str());
    if (!ok) std::fprintf(stderr, "could not write %s\n", opt.snapshot.c_str());
    lv_deinit();
    return ok ? 0 : 1;
  }

  int64_t lastRefresh = start;
  int64_t lastPoll = start;
  for (;;) {
    lv_timer_handler();
    const int64_t now = std::time(nullptr);
    if (now - lastPoll >= kPollSeconds) {
      feed.poll(now);
      lastPoll = now;
    }
    if (now != lastRefresh) {
      app_refresh(app, now);
      lastRefresh = now;
    }
    SDL_Delay(5);
  }
}
