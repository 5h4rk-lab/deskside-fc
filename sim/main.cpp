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
//   --screen goal    the goal celebration (goal-win: the full-time win),
//                    reached through the real path: the favourite scores (or
//                    wins) in the demo data and the app detects it
//   --at-ms N        with --snapshot and a goal screen: render after N ms of
//                    animation time (default 900)
//   --reduced-motion static goal celebration
//   --demo-goal      the favourite scores 6 s after start (no keyboard needed)
// Keys in the window: g = the favourite scores in its live match, w = that
// match ends with the favourite ahead (full-time win).
#include <SDL2/SDL.h>
#include <lvgl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "data_source.h"
#include "demo_data.h"
#include "mock_image_provider.h"
#include "mock_source.h"
#include "screens/detail.h"
#include "screens/goal.h"
#include "snapshot.h"
#include "store.h"

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 320;
constexpr int kDefaultScale = 2;
constexpr int64_t kPollSeconds = 30;
constexpr uint32_t kDefaultAtMs = 900;
constexpr uint32_t kDemoGoalMs = 6000;
constexpr uint32_t kSnapshotStepMs = 10;

struct Options {
  int scale = kDefaultScale;
  std::string assets = SIM_ASSETS_DIR;
  bool fixtures = false;
  std::string snapshot;
  std::string screen = "live";
  std::string match = demo::kLiveMatchId;
  uint32_t atMs = kDefaultAtMs;
  bool reducedMotion = false;
  bool demoGoal = false;
};

bool goal_screen(const std::string& screen) { return screen == "goal" || screen == "goal-win"; }

[[noreturn]] void usage(int code) {
  std::printf(
      "usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]\n"
      "           [--screen live|matches|news|detail|summary|pitch|stats|goal|goal-win]\n"
      "           [--match ID] [--at-ms N] [--reduced-motion] [--demo-goal]\n"
      "keys: g = favourite scores, w = favourite wins at full time\n");
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
      const char* const kScreens[] = {"live",  "matches", "news", "detail",  "summary",
                                      "pitch", "stats",   "goal", "goal-win"};
      bool known = false;
      for (const char* k : kScreens) known = known || o.screen == k;
      if (!known) {
        std::fprintf(stderr, "invalid --screen '%s'\n", o.screen.c_str());
        usage(2);
      }
    } else if (std::strcmp(argv[i], "--match") == 0 && hasValue) {
      o.match = argv[++i];
    } else if (std::strcmp(argv[i], "--at-ms") == 0 && hasValue) {
      const int ms = std::atoi(argv[++i]);
      if (ms < 0 || ms > 60000) {
        std::fprintf(stderr, "invalid --at-ms '%s' (0-60000)\n", argv[i]);
        std::exit(2);
      }
      o.atMs = static_cast<uint32_t>(ms);
    } else if (std::strcmp(argv[i], "--reduced-motion") == 0) {
      o.reducedMotion = true;
    } else if (std::strcmp(argv[i], "--demo-goal") == 0) {
      o.demoGoal = true;
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

// Animation time for goal snapshots: advanced by hand, not by the clock.
uint32_t g_fakeMs = 0;
uint32_t fake_ms() { return g_fakeMs; }

// A MockSource holding the demo data, so the demo can be changed while it
// runs (goals, full time). Kick-off and news times are relative to `now`.
void load_demo(MockSource& source, int64_t now) {
  const std::vector<MatchSummary> matches = demo_matches(now);
  source.set_matches(matches);
  source.set_news(demo_news(now));
  for (const MatchSummary& m : matches) {
    MatchDetail d = demo_detail(m.id);
    if (d.match.home.abbr.empty()) continue;  // no detail for this demo match
    d.match = m;
    source.set_detail(std::move(d));
  }
}

// Fills the store from the demo data or the contract fixtures. `onMatches`
// runs after every successful Store::set_matches().
class Feed {
 public:
  Feed(Store& store, bool fixtures, int64_t now)
      : store_(store), source_(fixtures ? CONTRACT_FIXTURES_DIR : "/nonexistent-fixtures") {
    if (!fixtures) load_demo(source_, now);
  }

  MockSource& source() { return source_; }
  void set_on_matches(std::function<void(int64_t)> cb) { onMatches_ = std::move(cb); }

  void poll(int64_t now) {
    bool updated = false;
    source_.fetch_matches([&](bool ok, std::vector<MatchSummary> matches) {
      if (ok) {
        store_.set_matches(std::move(matches), now);
        updated = true;
      } else {
        store_.mark_failure();
      }
    });
    if (updated && onMatches_) onMatches_(now);
    source_.fetch_teams([&](bool ok, std::vector<Team> teams) {
      if (ok) store_.set_teams(std::move(teams));
    });
    source_.fetch_news([&](bool ok, std::vector<NewsItem> news) {
      if (ok) store_.set_news(std::move(news));
    });
    if (const MatchSummary* live = store_.live_for(store_.favourite())) {
      source_.fetch_detail(live->id, [&](bool ok, MatchDetail detail) {
        if (ok) store_.set_detail(std::move(detail));
      });
    }
  }

 private:
  Store& store_;
  MockSource source_;
  std::function<void(int64_t)> onMatches_;
};

// The favourite's live match in the source, or false.
bool favourite_live(MockSource& source, const std::string& fav, MatchSummary& out, Side& side) {
  bool found = false;
  source.fetch_matches([&](bool ok, std::vector<MatchSummary> matches) {
    if (!ok) return;
    for (const MatchSummary& m : matches) {
      if (m.state != MatchState::Live || (m.home.id != fav && m.away.id != fav)) continue;
      out = m;
      side = m.home.id == fav ? Side::Home : Side::Away;
      found = true;
      return;
    }
  });
  return found;
}

// Keeps the source's detail of `m` in step with its summary, adding `event`
// when given.
void update_detail(MockSource& source, const MatchSummary& m, const MatchEvent* event) {
  MatchDetail d;
  source.fetch_detail(m.id, [&](bool ok, MatchDetail detail) {
    if (ok) d = std::move(detail);
  });
  d.match = m;
  if (event != nullptr) d.events.push_back(*event);
  source.set_detail(std::move(d));
}

// The favourite scores in its live match: the score goes up and the detail
// gets a goal by one of the team's players with a photo, in turn.
bool demo_goal(MockSource& source, const std::string& fav) {
  MatchSummary m;
  Side side = Side::Home;
  if (!favourite_live(source, fav, m, side)) return false;
  source.advance_score(m.id, side);
  favourite_live(source, fav, m, side);

  MatchDetail d;
  source.fetch_detail(m.id, [&](bool ok, MatchDetail detail) {
    if (ok) d = std::move(detail);
  });
  const Lineup& lineup = side == Side::Home ? d.home : d.away;
  std::vector<std::string> candidates;
  for (const LineupPlayer& p : lineup.players) {
    if (!p.photo.empty()) candidates.push_back(p.name);
  }
  static size_t turn = 0;
  const std::string scorer = candidates.empty() ? "Bukayo Saka" : candidates[turn++ % candidates.size()];
  const MatchEvent goal{m.clock.empty() ? "90'" : m.clock, EventType::Goal, side, scorer, ""};
  update_detail(source, m, &goal);
  return true;
}

// The favourite's live match ends with the favourite ahead.
bool demo_win(MockSource& source, const std::string& fav) {
  MatchSummary m;
  Side side = Side::Home;
  if (!favourite_live(source, fav, m, side)) return false;
  const int favScore = (side == Side::Home ? m.homeScore : m.awayScore).value_or(0);
  const int oppScore = (side == Side::Home ? m.awayScore : m.homeScore).value_or(0);
  if (favScore <= oppScore) {
    std::printf("the favourite is not ahead: press g first\n");
    return false;
  }
  std::vector<MatchSummary> matches;
  source.fetch_matches([&](bool, std::vector<MatchSummary> ms) { matches = std::move(ms); });
  for (MatchSummary& x : matches) {
    if (x.id != m.id) continue;
    x.state = MatchState::Post;
    x.clock = "FT";
    m = x;
  }
  source.set_matches(std::move(matches));
  update_detail(source, m, nullptr);
  return true;
}

// Keys pressed in the SDL window, collected by an event watch (LVGL's own SDL
// driver consumes the event queue) and handled in the main loop.
struct Keys {
  bool goal = false;
  bool win = false;
};

int watch_keys(void* userdata, SDL_Event* e) {
  if (e->type == SDL_KEYDOWN && e->key.repeat == 0) {
    auto* keys = static_cast<Keys*>(userdata);
    if (e->key.keysym.sym == SDLK_g) keys->goal = true;
    if (e->key.keysym.sym == SDLK_w) keys->win = true;
  }
  return 1;
}

void discard_flush(lv_display_t* disp, const lv_area_t*, uint8_t*) {
  lv_display_flush_ready(disp);
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  const bool headless = !opt.snapshot.empty();

  lv_init();
  if (headless && goal_screen(opt.screen)) {
    lv_tick_set_cb(fake_ms);
  } else {
    lv_tick_set_cb(headless ? steady_ms : sdl_ticks);
  }
  goal_set_reduced_motion(opt.reducedMotion);

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
  Feed feed(store, opt.fixtures, start);

  AppDeps deps{store, images, feed.source(), [](const std::string& id) {
                 std::printf("open match %s\n", id.c_str());
                 std::fflush(stdout);
               }};
  lv_obj_t* app = app_create(lv_screen_active(), deps);
  feed.set_on_matches([app](int64_t now) { app_matches_changed(app, now); });
  feed.poll(start);
  app_refresh(app, start);

  // Runs a demo change through the whole path: source, poll, detector,
  // celebration.
  auto demo_change = [&](bool win) {
    const int64_t now = std::time(nullptr);
    const bool ok = win ? demo_win(feed.source(), store.favourite())
                        : demo_goal(feed.source(), store.favourite());
    if (!ok) {
      std::printf("no live match of the favourite\n");
      std::fflush(stdout);
      return;
    }
    feed.poll(now);
    app_refresh(app, now);
  };

  if (opt.screen == "matches") {
    lv_tabview_set_active(app, 1, LV_ANIM_OFF);
    app_refresh(app, start);
  } else if (opt.screen == "news") {
    lv_tabview_set_active(app, 2, LV_ANIM_OFF);
    app_refresh(app, start);
  } else if (goal_screen(opt.screen)) {
    demo_change(opt.screen == "goal-win");
  } else if (opt.screen != "live") {
    app_open_match(app, opt.match);
    lv_obj_t* view = app_detail_view(app);
    if (opt.screen == "summary") detail_select_tab(view, DetailTab::Summary, false);
    if (opt.screen == "pitch") detail_select_tab(view, DetailTab::Pitch, false);
    if (opt.screen == "stats") detail_select_tab(view, DetailTab::Stats, false);
  }

  if (headless) {
    lv_obj_t* target = lv_screen_active();
    if (goal_screen(opt.screen)) {
      lv_timer_handler();
      for (uint32_t t = 0; t < opt.atMs; t += kSnapshotStepMs) {
        g_fakeMs += std::min(kSnapshotStepMs, opt.atMs - t);
        lv_timer_handler();
      }
      if (const GoalParts* p = goal_parts()) target = p->overlay;
    } else {
      for (int i = 0; i < 10; ++i) lv_timer_handler();
    }
    const bool ok = snapshot_png(target, opt.snapshot.c_str());
    if (!ok) std::fprintf(stderr, "could not write %s\n", opt.snapshot.c_str());
    lv_deinit();
    return ok ? 0 : 1;
  }

  Keys keys;
  SDL_AddEventWatch(watch_keys, &keys);
  bool demoGoalDone = !opt.demoGoal;
  int64_t lastRefresh = start;
  int64_t lastPoll = start;
  for (;;) {
    lv_timer_handler();
    if (keys.goal || (!demoGoalDone && SDL_GetTicks() >= kDemoGoalMs)) {
      keys.goal = false;
      demoGoalDone = true;
      demo_change(false);
    }
    if (keys.win) {
      keys.win = false;
      demo_change(true);
    }
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
