// Desktop simulator: runs the shared UI code in an SDL window.
// Usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]
//            [--screen VIEW] [--match ID]
//   --scale N        window is 240*N x 320*N, LVGL stays 240x320 (default 2)
//   --assets DIR     image assets (default: the repo's sim/assets)
//   --fixtures       load the contract fixtures instead of the demo data
//   --snapshot PATH  render once without a window, write a PNG and exit
//   --screen VIEW    live (default), live-last (the Live tab when the
//                    favourite has only a past match: its last result),
//                    matches (the Matches tab), news (the News tab), or the match
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
//   --screen settings|zone|picker|picker-search
//                    Settings, the time zone list, the team picker, or the
//                    picker with "ars" typed and the keyboard up
//   --prefs FILE     settings file (default ~/.config/soccer-display-sim/prefs.txt,
//                    created on demand); "memory" keeps them for this run only.
//                    Snapshots use memory unless --prefs is given.
//   --reset-prefs    start with default settings (deletes the settings file)
//   --tz ZONE        time zone id (for example Europe/London), saved like a
//                    choice in Settings. Without a saved zone the window
//                    picks the zone matching the computer's current offset.
//   --now EPOCH      pretend the clock started at EPOCH (seconds, UTC)
//   --wifi none|demo Wi-Fi: none (no Wi-Fi row, no setup) or a mock with demo
//                    networks (default; results arrive after a short delay)
//   --wifi-saved none
//                    start with no saved network: the guided setup opens
//   --screen wifi|wifi-first|wifi-password|wifi-failed|wifi-connecting
//                    the network list, the first-start setup, the password
//                    page (a few characters typed), after a wrong password,
//                    and while joining
//   --helper URL     fetch real data from the helper at URL (also the
//                    HELPER_URL environment variable) instead of the demo.
//                    Use https for real use. Only scheme://host is printed.
//   --token T        the helper's access token (also HELPER_TOKEN); never
//                    printed. Prefer HELPER_TOKEN: argv is visible to other
//                    local users in /proc. With --helper, crests, photos and cutouts are
//                    downloaded through the helper too (a second connection,
//                    so pictures never queue behind data polls); without it
//                    they come from --assets. Control characters in the URL or
//                    token are refused at startup.
//   --offline-test   the demo source is down most of the time (only every
//                    5th match poll succeeds), to see the stale marker and the
//                    Settings data line
// Data is fetched by the Poller (ui/src/poller.h) in every mode: every 25 s
// while a match is live, every 3 min otherwise, backing off after failures.
// Keys in the window (demo data only): g = the favourite scores in its live
// match, w = that match ends with the favourite ahead (full-time win).
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
#include "curl_transport.h"
#include "data_source.h"
#include "demo_data.h"
#include "file_prefs.h"
#include "http_source.h"
#include "mock_image_provider.h"
#include "mock_source.h"
#include "mock_wifi.h"
#include "poller.h"
#include "prefs.h"
#include "screens/detail.h"
#include "screens/goal.h"
#include "screens/team_picker.h"
#include "screens/wifi.h"
#include "sim_backlight.h"
#include "http_image_provider.h"
#include "lvgl_image_decoder.h"
#include "snapshot.h"
#include "store.h"
#include "timezone.h"

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 320;
constexpr int kDefaultScale = 2;
constexpr int kOfflineTestCycle = 5;      // --offline-test: 1 good poll in 5
constexpr int64_t kHelperSnapshotMs = 10000;  // headless with --helper: wait for data
constexpr int64_t kImageSnapshotMs = 8000;    // ... and then for the pictures
constexpr unsigned kImageWorkers = 2;         // simultaneous picture downloads
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
  std::string prefs;  // empty: the default file (memory for snapshots)
  bool resetPrefs = false;
  std::string tz;
  bool hasNow = false;
  int64_t now = 0;
  bool wifi = true;
  bool wifiSaved = true;
  std::string helper;  // empty: the demo / fixtures
  std::string token;
  bool offlineTest = false;
};

bool goal_screen(const std::string& screen) { return screen == "goal" || screen == "goal-win"; }
bool wifi_screen(const std::string& screen) { return screen.rfind("wifi", 0) == 0; }

// The demo networks. The saved ones join with their stored password; typing
// "homehome1" joins HomeNet-5G and "netgear123" joins NETGEAR-4F.
void load_demo_wifi(MockWifi& wifi, bool saved) {
  wifi.set_networks({
      {"HomeNet-5G", -48, false},
      {"Lab-Guest", -62, false},
      {"Cafe Open Wi-Fi", -70, true},
      {"NETGEAR-4F", -74, false},
      {"Stadium-Fan-Zone-WiFi-Network-32", -66, false},
      {"Neighbour", -88, false},
      {"NETGEAR-4F", -81, false},  // a second access point: listed once
      {"", -60, false},            // a hidden network: not listed
  });
  wifi.set_password("HomeNet-5G", "homehome1");
  wifi.set_password("Lab-Guest", "lab-guest-24");
  wifi.set_password("NETGEAR-4F", "netgear123");
  if (saved) {
    wifi.set_saved({"HomeNet-5G", "Lab-Guest"});
    wifi.auto_join();
  }
}

[[noreturn]] void usage(int code) {
  std::printf(
      "usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]\n"
      "           [--screen live|live-last|matches|news|detail|summary|pitch|stats|goal|goal-win]\n"
      "           [--match ID] [--at-ms N] [--reduced-motion] [--demo-goal]\n"
      "           [--screen settings|zone|picker|picker-search]\n"
      "           [--prefs FILE|memory] [--reset-prefs] [--tz ZONE] [--now EPOCH]\n"
      "           [--wifi none|demo] [--wifi-saved none]\n"
      "           [--screen wifi|wifi-first|wifi-password|wifi-failed|wifi-connecting]\n"
      "           [--helper URL --token T] [--offline-test]\n"
      "env: HELPER_URL, HELPER_TOKEN (when the flags are absent)\n"
      "keys: g = favourite scores, w = favourite wins at full time (demo data)\n");
  std::exit(code);
}

bool has_control_chars(const std::string& s) {
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F) return true;
  }
  return false;
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
      const char* const kScreens[] = {"live",     "live-last", "matches",  "news",   "detail",
                                      "summary",  "pitch",    "stats",  "goal",
                                      "goal-win", "settings", "zone",   "picker",
                                      "picker-search", "wifi", "wifi-first", "wifi-password",
                                      "wifi-failed", "wifi-connecting"};
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
    } else if (std::strcmp(argv[i], "--prefs") == 0 && hasValue) {
      o.prefs = argv[++i];
    } else if (std::strcmp(argv[i], "--reset-prefs") == 0) {
      o.resetPrefs = true;
    } else if (std::strcmp(argv[i], "--tz") == 0 && hasValue) {
      o.tz = argv[++i];
      if (find_zone(o.tz) == nullptr) {
        std::fprintf(stderr, "unknown --tz '%s'; known zones:\n", o.tz.c_str());
        for (const Zone& z : zones()) std::fprintf(stderr, "  %s\n", z.id);
        std::exit(2);
      }
    } else if (std::strcmp(argv[i], "--now") == 0 && hasValue) {
      o.now = std::strtoll(argv[++i], nullptr, 10);
      o.hasNow = o.now > 0;
      if (!o.hasNow) {
        std::fprintf(stderr, "invalid --now '%s'\n", argv[i]);
        std::exit(2);
      }
    } else if (std::strcmp(argv[i], "--wifi") == 0 && hasValue) {
      const std::string v = argv[++i];
      if (v != "none" && v != "demo") {
        std::fprintf(stderr, "invalid --wifi '%s' (none or demo)\n", v.c_str());
        std::exit(2);
      }
      o.wifi = v == "demo";
    } else if (std::strcmp(argv[i], "--wifi-saved") == 0 && hasValue) {
      const std::string v = argv[++i];
      if (v != "none") {
        std::fprintf(stderr, "invalid --wifi-saved '%s' (none)\n", v.c_str());
        std::exit(2);
      }
      o.wifiSaved = false;
    } else if (std::strcmp(argv[i], "--fixtures") == 0) {
      o.fixtures = true;
    } else if (std::strcmp(argv[i], "--helper") == 0 && hasValue) {
      o.helper = argv[++i];
    } else if (std::strcmp(argv[i], "--token") == 0 && hasValue) {
      o.token = argv[++i];
    } else if (std::strcmp(argv[i], "--offline-test") == 0) {
      o.offlineTest = true;
    } else if (std::strcmp(argv[i], "--help") == 0) {
      usage(0);
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", argv[i]);
      usage(2);
    }
  }
  if (o.helper.empty()) {
    if (const char* env = std::getenv("HELPER_URL")) o.helper = env;
  }
  if (o.token.empty()) {
    if (const char* env = std::getenv("HELPER_TOKEN")) o.token = env;
  }
  if (!o.helper.empty() && o.helper.rfind("http://", 0) != 0 && o.helper.rfind("https://", 0) != 0) {
    std::fprintf(stderr, "invalid --helper '%s' (an http:// or https:// URL)\n", o.helper.c_str());
    std::exit(2);
  }
  if (!o.helper.empty() && o.token.empty()) {
    std::fprintf(stderr, "--helper needs a token: --token T or HELPER_TOKEN\n");
    std::exit(2);
  }
  // A CR or LF in either would let it add headers to every request.
  if (has_control_chars(o.helper)) {
    std::fprintf(stderr, "invalid --helper: control characters are not allowed in the URL\n");
    std::exit(2);
  }
  if (has_control_chars(o.token)) {
    std::fprintf(stderr, "invalid token: control characters are not allowed (check HELPER_TOKEN)\n");
    std::exit(2);
  }
  if (!o.helper.empty() && o.offlineTest) {
    std::fprintf(stderr, "--offline-test works on the demo data only (not with --helper)\n");
    std::exit(2);
  }
  return o;
}

// "scheme://host[:port]" of a URL, without userinfo, path or query (which may
// carry secrets), for printing.
std::string printable_origin(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "(invalid URL)";
  const size_t start = scheme + 3;
  size_t end = url.find_first_of("/?#", start);
  if (end == std::string::npos) end = url.size();
  std::string authority = url.substr(start, end - start);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  return url.substr(0, start) + authority;
}

// --screen live-last: the favourite keeps only its past matches, so the Live
// tab falls back to the last result.
void keep_only_past_of_favourite(MockSource& source, const std::string& fav) {
  std::vector<MatchSummary> kept;
  source.fetch_matches([&](bool ok, std::vector<MatchSummary> ms) {
    if (!ok) return;
    for (MatchSummary& m : ms) {
      const bool favourite = m.home.id == fav || m.away.id == fav;
      if (!favourite || m.state == MatchState::Post) kept.push_back(std::move(m));
    }
  });
  source.set_matches(std::move(kept));
}

// Milliseconds since start on a monotonic clock (for the Poller).
int64_t monotonic_ms() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration_cast<milliseconds>(steady_clock::now() - start).count();
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
  source.set_teams(demo_teams());
  for (const MatchSummary& m : matches) {
    MatchDetail d = demo_detail(m.id);
    if (d.match.home.abbr.empty()) continue;  // no detail for this demo match
    d.match = m;
    source.set_detail(std::move(d));
  }
}

// --offline-test: the demo source, down most of the time. Only every
// kOfflineTestCycle-th matches fetch (the first included) succeeds; while it is
// down every kind of fetch fails as if the helper could not be reached.
class FlakySource : public DataSource {
 public:
  explicit FlakySource(MockSource& inner) : inner_(inner) {}
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    inner_.set_fail(polls_++ % kOfflineTestCycle != 0, FetchError::Offline);
    inner_.fetch_matches(std::move(cb));
  }
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override {
    inner_.fetch_detail(id, std::move(cb));
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override {
    inner_.fetch_news(std::move(cb));
  }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override {
    inner_.fetch_teams(std::move(cb));
  }
  FetchError last_error() const override { return inner_.last_error(); }

 private:
  MockSource& inner_;
  int polls_ = 0;
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
  if (headless && (goal_screen(opt.screen) || wifi_screen(opt.screen))) {
    lv_tick_set_cb(fake_ms);
  } else {
    lv_tick_set_cb(headless ? steady_ms : sdl_ticks);
  }

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

  // The simulated clock: the real one, shifted when --now is given.
  const int64_t realStart = std::time(nullptr);
  const int64_t clockShift = opt.hasNow ? opt.now - realStart : 0;
  auto clock_now = [clockShift] { return static_cast<int64_t>(std::time(nullptr)) + clockShift; };
  const int64_t start = clock_now();

  // Settings: a file that survives restarts, or memory (snapshots, --prefs memory).
  std::unique_ptr<Prefs> prefs;
  const bool memoryPrefs = opt.prefs == "memory" || (headless && opt.prefs.empty());
  if (memoryPrefs) {
    prefs = std::make_unique<MemoryPrefs>();
  } else {
    const std::string path = opt.prefs.empty() ? FilePrefs::default_path() : opt.prefs;
    if (opt.resetPrefs) std::remove(path.c_str());
    prefs = std::make_unique<FilePrefs>(path);
  }
  std::string savedZone;
  if (!opt.tz.empty()) {
    prefs->set("zone", opt.tz);
  } else if (!headless && !prefs->get("zone", savedZone)) {
    // First run in the window: the zone that matches this computer right now.
    const int local = local_tz_offset(static_cast<time_t>(start));
    for (const Zone& z : zones()) {
      if (offset_seconds(z, start) == local) {
        prefs->set("zone", z.id);
        break;
      }
    }
  }

  Store store;
  SimBacklight backlight;

  // Data: the real helper (--helper), or the demo data / contract fixtures in
  // a MockSource (optionally flaky). The Poller drives all of them.
  MockSource mock(opt.fixtures ? CONTRACT_FIXTURES_DIR : "/nonexistent-fixtures");
  if (!opt.fixtures) load_demo(mock, start);
  FlakySource flaky(mock);
  std::unique_ptr<CurlTransport> transport;
  std::unique_ptr<HttpSource> http;
  DataSource* source = &mock;
  if (!opt.helper.empty()) {
    transport = std::make_unique<CurlTransport>();
    http = std::make_unique<HttpSource>(*transport, opt.helper, opt.token);
    source = http.get();
    // Never the token, nor userinfo or a query string from the URL.
    std::printf("fetching from the helper at %s\n", printable_origin(opt.helper).c_str());
    std::fflush(stdout);
  } else if (opt.offlineTest) {
    source = &flaky;
  }
  const bool demoData = opt.helper.empty();

  // Pictures: downloaded through the helper over their own connections (they
  // must not queue behind data polls), or read from --assets. Declared after
  // the data transport and before the app, so the provider is destroyed before
  // its transport.
  LvglImageDecoder decoder;
  std::unique_ptr<CurlTransport> imageTransport;
  std::unique_ptr<ImageProvider> images;
  HttpImageProvider* httpImages = nullptr;
  if (!opt.helper.empty()) {
    imageTransport = std::make_unique<CurlTransport>(kImageWorkers);
    auto provider = std::make_unique<HttpImageProvider>(*imageTransport, decoder, opt.helper, opt.token);
    httpImages = provider.get();
    httpImages->tick(monotonic_ms());
    images = std::move(provider);
  } else {
    images = std::make_unique<MockImageProvider>(decoder, opt.assets);
  }

  // Wi-Fi: the demo networks. In the window results take a moment (spinners
  // show); snapshots are synchronous, except the joining one which never
  // completes. wifi-first starts with nothing saved.
  MockWifi wifi;
  load_demo_wifi(wifi, opt.wifiSaved && opt.screen != "wifi-first");
  wifi.set_async(!headless || opt.screen == "wifi-connecting");

  lv_obj_t* app = nullptr;
  bool matchesChanged = false;
  Poller poller(store, *source, [&app, &matchesChanged](int64_t now) {
    app_matches_changed(app, now);
    matchesChanged = true;
  });
  // The open detail, else the favourite's live match (its latest event feeds
  // the Live tab's strip).
  auto detail_watch = [&]() {
    std::string id = app_open_match_id(app);
    if (id.empty()) {
      if (const MatchSummary* m = store.live_for(store.favourite())) id = m->id;
    }
    return id;
  };
  // One step of the data path: deliver finished requests, then schedule.
  auto pump = [&](int64_t now) {
    if (transport) transport->poll();
    if (imageTransport) imageTransport->poll();
    if (httpImages) httpImages->tick(monotonic_ms());
    poller.set_detail_watch(detail_watch());
    poller.tick(monotonic_ms(), now);
  };

  AppDeps deps{store, *images, *source,
               [](const std::string& id) {
                 std::printf("open match %s\n", id.c_str());
                 std::fflush(stdout);
               },
               *prefs, &backlight, opt.wifi ? &wifi : nullptr,
               // Connected at last: fetch everything now, not at the next due time.
               [&poller] { poller.request_all_now(); }};
  app = app_create(lv_screen_active(), deps);
  // A session-only override of the saved setting.
  if (opt.reducedMotion) goal_set_reduced_motion(true);
  if (opt.screen == "live-last" && demoData) {
    keep_only_past_of_favourite(mock, store.favourite());
  }
  pump(start);  // matches, teams, news (at once with the demo source)
  pump(start);  // the favourite's live detail, now that the matches are in
  if (headless && transport) {
    // A snapshot of real data: wait (a bounded time) for the answers.
    const int64_t until = monotonic_ms() + kHelperSnapshotMs;
    while (monotonic_ms() < until &&
           !(store.has_data() && !store.news().empty()) && store.last_error() == FetchError::None) {
      pump(start);
      SDL_Delay(10);
    }
  }
  app_refresh(app, start);

  // Runs a demo change through the whole path: source, poll, detector,
  // celebration.
  auto demo_change = [&](bool win) {
    const int64_t now = clock_now();
    if (!demoData) {
      std::printf("the demo keys work on the demo data only (not with --helper)\n");
      std::fflush(stdout);
      return;
    }
    const bool ok = win ? demo_win(mock, store.favourite()) : demo_goal(mock, store.favourite());
    if (!ok) {
      std::printf("no live match of the favourite\n");
      std::fflush(stdout);
      return;
    }
    poller.request_matches_now();
    pump(now);
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
  } else if (opt.screen == "settings") {
    app_open_settings(app);
  } else if (opt.screen == "zone") {
    app_open_zones(app);
  } else if (opt.screen == "picker" || opt.screen == "picker-search") {
    app_open_settings(app);
    app_open_team_picker(app);
    if (opt.screen == "picker-search") {
      lv_obj_t* picker = app_team_picker_view(app);
      team_picker_set_query(picker, "ars");
      team_picker_show_keyboard(picker, true);
    }
  } else if (wifi_screen(opt.screen)) {
    if (opt.screen != "wifi-first") {
      app_open_settings(app);
      app_open_wifi(app);
    }
    lv_obj_t* view = app_wifi_view(app);
    if (opt.screen == "wifi-connecting") wifi.poll(MockWifi::kDefaultScanMs);  // the list is in
    if (view != nullptr && opt.screen != "wifi" && opt.screen != "wifi-first") {
      wifi_open_password(view, "NETGEAR-4F");
      const WifiPasswordParts* p = wifi_password_parts(view);
      if (p != nullptr) {
        // Too short to join, a wrong password, the right one.
        const char* typed = opt.screen == "wifi-password" ? "netg"
                            : opt.screen == "wifi-failed" ? "netgear12"
                                                          : "netgear123";
        lv_textarea_set_text(p->field, typed);
        if (opt.screen != "wifi-password") lv_obj_send_event(p->connect, LV_EVENT_CLICKED, nullptr);
      }
    }
  } else if (opt.screen != "live" && opt.screen != "live-last") {
    app_open_match(app, opt.match);
    lv_obj_t* view = app_detail_view(app);
    if (opt.screen == "summary") detail_select_tab(view, DetailTab::Summary, false);
    if (opt.screen == "pitch") detail_select_tab(view, DetailTab::Pitch, false);
    if (opt.screen == "stats") detail_select_tab(view, DetailTab::Stats, false);
  }

  if (headless && httpImages) {
    // A snapshot of real data: wait (a bounded time) for the pictures the
    // screen asked for. More data may arrive meanwhile and ask for more.
    const int64_t until = monotonic_ms() + kImageSnapshotMs;
    while (monotonic_ms() < until && httpImages->pending() > 0) {
      pump(start);
      if (matchesChanged) {
        app_refresh(app, start);
        matchesChanged = false;
      }
      SDL_Delay(10);
    }
    pump(start);
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
    } else if (wifi_screen(opt.screen)) {
      // Fixed animation time: the spinner is always drawn at the same angle,
      // and the last typed character is already masked.
      for (uint32_t t = 0; t < 2000; t += kSnapshotStepMs) {
        g_fakeMs += kSnapshotStepMs;
        lv_timer_handler();
      }
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
  uint32_t lastWifiMs = SDL_GetTicks();
  for (;;) {
    lv_timer_handler();
    const uint32_t ms = SDL_GetTicks();
    wifi.poll(static_cast<int64_t>(ms - lastWifiMs));
    lastWifiMs = ms;
    if (keys.goal || (!demoGoalDone && SDL_GetTicks() >= kDemoGoalMs)) {
      keys.goal = false;
      demoGoalDone = true;
      demo_change(false);
    }
    if (keys.win) {
      keys.win = false;
      demo_change(true);
    }
    const int64_t now = clock_now();
    pump(now);
    if (now != lastRefresh || matchesChanged) {
      app_refresh(app, now);
      lastRefresh = now;
      matchesChanged = false;
    }
    SDL_Delay(5);
  }
}
