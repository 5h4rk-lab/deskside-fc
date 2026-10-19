// Desktop simulator: runs the shared UI code in an SDL window.
// Usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]
//   --scale N        window is 240*N x 320*N, LVGL stays 240x320 (default 2)
//   --assets DIR     image assets (default: the repo's sim/assets)
//   --fixtures       load the contract fixtures instead of the demo data
//   --snapshot PATH  render once without a window, write a PNG and exit
#include <SDL2/SDL.h>
#include <lvgl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "app.h"
#include "demo_data.h"
#include "mock_image_provider.h"
#include "mock_source.h"
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
};

[[noreturn]] void usage(int code) {
  std::printf("usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]\n");
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

// Fills the store from the demo data or the contract fixtures.
class Feed {
 public:
  Feed(Store& store, bool fixtures)
      : store_(store), source_(fixtures ? new MockSource(CONTRACT_FIXTURES_DIR) : nullptr) {}
  ~Feed() { delete source_; }

  void poll(int64_t now) {
    if (source_ == nullptr) {
      store_.set_matches(demo_matches(now), now);
      store_.set_detail(demo_detail(demo::kLiveMatchId));
      return;
    }
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
  MockSource* source_;
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

  AppDeps deps{store, images, [](const std::string& id) {
                 std::printf("open match %s\n", id.c_str());
                 std::fflush(stdout);
               }};
  lv_obj_t* app = app_create(lv_screen_active(), deps);
  app_refresh(app, start);

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
