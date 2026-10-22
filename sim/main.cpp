// Desktop simulator: runs the shared UI code in an SDL window.
// Usage: sim [--scale N] [--assets DIR] [--fixtures] [--snapshot PATH]
//            [--screen VIEW] [--match ID]
//   --scale N        window is 240*N x 320*N, LVGL stays 240x320 (default 2)
//   --assets DIR     image assets (default: the repo's sim/assets)
//   --fixtures       load the contract fixtures instead of the demo data
//   --snapshot PATH  render once without a window, write a PNG and exit
//   --screen VIEW    live (default), live-last (the Live tab when the
//                    favourite has only a past match: its last result;
//                    live-last-away: that result as a 0:2 away defeat),
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
//   --screen settings|settings-about|settings-sound|settings-battery|zone|picker|picker-search
//                    Settings (settings-sound: scrolled to the Sound card; settings-battery: scrolled to the
//                    Battery card; settings-about: scrolled to the About area
//                    with the Battery and data lines), the time zone list, the team picker, or the
//                    picker with "ars" typed and the keyboard up
//   --prefs FILE     settings file (default ~/.config/soccer-display-sim/prefs.txt,
//                    created on demand); "memory" keeps them for this run only.
//                    Snapshots use memory unless --prefs is given.
//   --reset-prefs    start with default settings (deletes the settings file)
//   --fav ABBR       make the team with this abbreviation (from the demo team
//                    list, e.g. CHE or TOT) the favourite before start, saved
//                    like a choice in the team picker
//   --live-vs H,A    demo data: the live match is H v A (abbreviations from the
//                    demo team list, e.g. CHE,EVE); H is the favourite unless
//                    --fav is given
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
//   --battery P[,charging]|none
//                    the battery: P percent (0-100) on battery or charging, or
//                    none (no battery: no icon, "No battery detected" in
//                    Settings). Default: 78 percent on battery
//   --battery-mv MV  a pretend ADC reading MV millivolts (not with --battery),
//                    fed once a second through the real BatteryModel
//                    (smoothing, percent, charging inferred from a rise); + and
//                    - in the window raise or lower it by 50 mV (a charger
//                    plugged or pulled)
//   --banner low|critical|none
//                    with a snapshot: force the low-battery banner, or remove
//                    one the battery state raised at start (to see the icon)
//   --stale-min N    with a snapshot: render N minutes after the last update,
//                    as if the helper had gone quiet (the stale marker shows)
// Data is fetched by the Poller (ui/src/poller.h) in every mode: every 25 s
// while a match is live, every 3 min otherwise, backing off after failures.
// Keys in the window (demo data only): g = the favourite scores in its live
// match, w = that match ends with the favourite ahead (full-time win),
// b = the next demo battery state (78 %, 40 %, 18 % low, 8 % critical,
// 55 % charging, 100 % charging, no battery), printed in the terminal.
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
#include "fixture_source.h"
#include "file_prefs.h"
#include "http_source.h"
#include "mock_image_provider.h"
#include "mock_power.h"
#include "mock_source.h"
#include "mock_wifi.h"
#include "host_loop.h"
#include "poller.h"
#include "prefs.h"
#include "settings.h"
#include "screens/detail.h"
#include "screens/goal.h"
#include "screens/settings.h"
#include "screens/team_picker.h"
#include "screens/wifi.h"
#include "sim_backlight.h"
#include "http_image_provider.h"
#include "lvgl_image_decoder.h"
#include "snapshot.h"
#include "sound_out.h"
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
  std::string fav;  // --fav ABBR: favourite from the demo teams
  std::string liveHome;  // --live-vs H,A
  std::string liveAway;
  std::string tz;
  bool hasNow = false;
  int64_t now = 0;
  bool wifi = true;
  bool wifiSaved = true;
  std::string helper;  // empty: the demo / fixtures
  std::string token;
  bool offlineTest = false;
  PowerStatus battery = MockPower().status();
  bool batteryGiven = false;  // --battery was given
  int batteryMv = 0;  // --battery-mv: 0 is off
  std::string banner;  // --banner: "", low, critical or none
  int staleMin = 0;    // --stale-min
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
      "           [--screen live|live-last|live-last-away|matches|news|detail|summary|pitch|stats|goal|goal-win]\n"
      "           [--match ID] [--at-ms N] [--reduced-motion] [--demo-goal]\n"
      "           [--screen settings|settings-about|settings-sound|settings-battery|zone|picker|picker-search]\n"
      "           [--prefs FILE|memory] [--reset-prefs] [--fav ABBR] [--live-vs H,A]\n"
      "           [--tz ZONE] [--now EPOCH]\n"
      "           [--wifi none|demo] [--wifi-saved none]\n"
      "           [--screen wifi|wifi-first|wifi-password|wifi-failed|wifi-connecting]\n"
      "           [--helper URL --token T] [--offline-test]\n"
      "           [--battery P[,charging]|none] [--battery-mv MV] [--banner low|critical|none]\n"
      "           [--stale-min N]\n"
      "env: HELPER_URL, HELPER_TOKEN (when the flags are absent)\n"
      "keys: g = favourite scores, w = favourite wins at full time (demo data),\n"
      "      b = next demo battery state, + / - = 50 mV up / down (--battery-mv)\n");
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
      const char* const kScreens[] = {"live",     "live-last", "live-last-away", "matches",  "news",   "detail",
                                      "summary",  "pitch",    "stats",  "goal",
                                      "goal-win", "settings", "settings-about", "settings-sound", "settings-battery", "zone",   "picker",
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
    } else if (std::strcmp(argv[i], "--fav") == 0 && hasValue) {
      o.fav = argv[++i];
    } else if (std::strcmp(argv[i], "--live-vs") == 0 && hasValue) {
      const std::string v = argv[++i];
      const size_t comma = v.find(',');
      if (comma == std::string::npos || comma == 0 || comma + 1 == v.size()) {
        std::fprintf(stderr, "invalid --live-vs '%s' (HOME,AWAY abbreviations)\n", v.c_str());
        std::exit(2);
      }
      o.liveHome = v.substr(0, comma);
      o.liveAway = v.substr(comma + 1);
    } else if (std::strcmp(argv[i], "--fixtures") == 0) {
      o.fixtures = true;
    } else if (std::strcmp(argv[i], "--helper") == 0 && hasValue) {
      o.helper = argv[++i];
    } else if (std::strcmp(argv[i], "--token") == 0 && hasValue) {
      o.token = argv[++i];
    } else if (std::strcmp(argv[i], "--offline-test") == 0) {
      o.offlineTest = true;
    } else if (std::strcmp(argv[i], "--battery") == 0 && hasValue) {
      o.batteryGiven = true;
      if (!MockPower::parse(argv[++i], o.battery)) {
        std::fprintf(stderr, "invalid --battery '%s' (PERCENT, PERCENT,charging or none)\n", argv[i]);
        std::exit(2);
      }
    } else if (std::strcmp(argv[i], "--battery-mv") == 0 && hasValue) {
      o.batteryMv = std::atoi(argv[++i]);
      if (o.batteryMv < 1 || o.batteryMv > 5000) {
        std::fprintf(stderr, "invalid --battery-mv '%s' (1-5000)\n", argv[i]);
        std::exit(2);
      }
    } else if (std::strcmp(argv[i], "--banner") == 0 && hasValue) {
      o.banner = argv[++i];
      if (o.banner != "low" && o.banner != "critical" && o.banner != "none") {
        std::fprintf(stderr, "invalid --banner '%s' (low, critical or none)\n", o.banner.c_str());
        std::exit(2);
      }
    } else if (std::strcmp(argv[i], "--stale-min") == 0 && hasValue) {
      o.staleMin = std::atoi(argv[++i]);
      if (o.staleMin < 1 || o.staleMin > 100000) {
        std::fprintf(stderr, "invalid --stale-min '%s' (1-100000)\n", argv[i]);
        std::exit(2);
      }
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
  if (o.batteryGiven && o.batteryMv > 0) {
    std::fprintf(stderr, "--battery and --battery-mv cannot be used together (scripted or raw millivolts)\n");
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
// tab falls back to the last result. `awayLoss`: those become 2-0 defeats
// away from home (live-last-away).
void keep_only_past_of_favourite(MockSource& source, const std::string& fav, bool awayLoss) {
  std::vector<MatchSummary> kept;
  source.fetch_matches([&](bool ok, std::vector<MatchSummary> ms) {
    if (!ok) return;
    for (MatchSummary& m : ms) {
      const bool favourite = m.home.id == fav || m.away.id == fav;
      if (favourite && m.state != MatchState::Post) continue;
      if (favourite && awayLoss) {
        if (m.home.id == fav) std::swap(m.home, m.away);
        m.homeScore = 2;
        m.awayScore = 0;
      }
      kept.push_back(std::move(m));
    }
  });
  source.set_matches(std::move(kept));
}

// --live-vs: the demo live match becomes home v away (demo team list).
bool set_live_teams(MockSource& source, const std::string& home, const std::string& away) {
  const std::vector<Team> teams = demo_teams();
  auto find = [&](const std::string& abbr, Team& out) {
    for (const Team& t : teams) {
      if (t.abbr == abbr) {
        out = t;
        out.league.clear();
        return true;
      }
    }
    return false;
  };
  Team h;
  Team a;
  if (!find(home, h) || !find(away, a)) return false;
  std::vector<MatchSummary> all;
  source.fetch_matches([&](bool ok, std::vector<MatchSummary> ms) {
    if (ok) all = std::move(ms);
  });
  for (MatchSummary& m : all) {
    if (m.id != demo::kLiveMatchId) continue;
    m.home = h;
    m.away = a;
  }
  source.set_matches(std::move(all));
  return true;
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
  demo_load(source, now, fixture_text("news-normalised.json"), fixture_text("teams-normalised.json"));
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

// Keys pressed in the SDL window, collected by an event watch (LVGL's own SDL
// driver consumes the event queue) and handled in the main loop.
struct Keys {
  bool goal = false;
  bool win = false;
  bool battery = false;
  int millivolts = 0;  // + / -: steps of 50 mV
};

int watch_keys(void* userdata, SDL_Event* e) {
  if (e->type == SDL_KEYDOWN && e->key.repeat == 0) {
    auto* keys = static_cast<Keys*>(userdata);
    if (e->key.keysym.sym == SDLK_g) keys->goal = true;
    if (e->key.keysym.sym == SDLK_w) keys->win = true;
    if (e->key.keysym.sym == SDLK_b) keys->battery = true;
    const SDL_Keycode k = e->key.keysym.sym;
    if (k == SDLK_PLUS || k == SDLK_EQUALS || k == SDLK_KP_PLUS) keys->millivolts += 50;
    if (k == SDLK_MINUS || k == SDLK_KP_MINUS) keys->millivolts -= 50;
  }
  return 1;
}

// The simulator has no speaker (the sounds are heard on the board): this takes
// the clips so the Sound rows show in Settings, and says what would play.
class SimSound : public SoundOut {
 public:
  explicit SimSound(bool verbose) : verbose_(verbose) {}
  void play(const PcmClip& clip, uint8_t volumePercent) override {
    if (verbose_) {
      std::printf("sound: %d ms at %d%% (no speaker in the simulator)\n", clip.duration_ms(), volumePercent);
      std::fflush(stdout);
    }
  }
  void stop() override {}
  bool busy() const override { return false; }

 private:
  bool verbose_;
};

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

  const std::string favAbbr = !opt.fav.empty() ? opt.fav : opt.liveHome;
  if (!favAbbr.empty()) {
    bool found = false;
    for (const Team& t : demo_teams()) {
      if (t.abbr != favAbbr) continue;
      Settings s = load_settings(*prefs);
      s.favourite = t;
      save_settings(*prefs, s);
      found = true;
      break;
    }
    if (!found) {
      std::fprintf(stderr, "unknown team '%s' (an abbreviation from the demo teams, e.g. CHE)\n",
                   favAbbr.c_str());
      return 2;
    }
  }

  Store store;
  SimBacklight backlight;

  // Data: the real helper (--helper), or the demo data / contract fixtures in
  // a MockSource (optionally flaky). The Poller drives all of them.
  FixtureSource mock(opt.fixtures ? CONTRACT_FIXTURES_DIR : "/nonexistent-fixtures");
  if (!opt.fixtures) load_demo(mock, start);
  if (!opt.liveHome.empty() && !opt.fixtures && !set_live_teams(mock, opt.liveHome, opt.liveAway)) {
    std::fprintf(stderr, "unknown --live-vs team (abbreviations from the demo teams)\n");
    return 2;
  }
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

  // Battery: scripted (--battery, the b key) or raw millivolts through the
  // real model (--battery-mv), sampled once a second like the firmware's ADC.
  MockPower mockPower;
  mockPower.set(opt.battery);
  ModelPower modelPower(opt.batteryMv);
  PowerService* power = &mockPower;
  if (opt.batteryMv > 0) {
    power = &modelPower;
    modelPower.sample(start);
  }

  SimSound simSound(!headless);
  lv_obj_t* app = nullptr;
  PowerStatus lastModelStatus = modelPower.status();
  // The shared host loop: the same sequence the firmware runs. The data path
  // is delivered, scheduled and refreshed from it; in the simulator the
  // battery model is sampled once a second just before the refresh.
  HostLoop::Hooks hooks;
  hooks.pollTransports = [&] {
    if (transport) transport->poll();
    if (imageTransport) imageTransport->poll();
  };
  hooks.tickImages = [&](int64_t ms) {
    if (httpImages) httpImages->tick(ms);
  };
  hooks.openMatchId = [&] { return app_open_match_id(app); };
  hooks.onMatchesChanged = [&](int64_t now) { app_matches_changed(app, now); };
  hooks.everySecond = [&](int64_t now) {
    if (opt.batteryMv <= 0) return;
    modelPower.sample(now);
    const PowerStatus s = modelPower.status();
    if (s != lastModelStatus) {  // the model's view changed: say so
      std::printf("ADC %d mV -> %s\n", modelPower.millivolts(), MockPower::describe(s).c_str());
      std::fflush(stdout);
      lastModelStatus = s;
    }
  };
  hooks.refresh = [&](int64_t now) { app_refresh(app, now); };
  HostLoop host(store, *source, hooks);
  Poller& poller = host.poller();
  // One step of the data path: deliver finished requests, then schedule.
  auto pump = [&](int64_t now) { host.pump(monotonic_ms(), now); };

  AppDeps deps{store, *images, *source,
               [](const std::string& id) {
                 std::printf("open match %s\n", id.c_str());
                 std::fflush(stdout);
               },
               *prefs, &backlight, opt.wifi ? &wifi : nullptr,
               // Connected at last: fetch everything now, not at the next due time.
               [&poller] { poller.request_all_now(); }, power, &simSound};
  app = app_create(lv_screen_active(), deps);
  // A session-only override of the saved setting.
  if (opt.reducedMotion) goal_set_reduced_motion(true);
  if ((opt.screen == "live-last" || opt.screen == "live-last-away") && demoData) {
    keep_only_past_of_favourite(mock, store.favourite(), opt.screen == "live-last-away");
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
  host.note_refreshed(start);

  // Runs a demo change through the whole path: source, poll, detector,
  // celebration.
  auto demo_change = [&](bool win) {
    const int64_t now = clock_now();
    if (!demoData) {
      std::printf("the demo keys work on the demo data only (not with --helper)\n");
      std::fflush(stdout);
      return;
    }
    bool ok = false;
    if (win) {
      const DemoWin r = demo_win(mock, store.favourite());
      if (r == DemoWin::NotAhead) std::printf("the favourite is not ahead: press g first\n");
      ok = r == DemoWin::Ok;
    } else {
      ok = demo_goal(mock, store.favourite());
    }
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
  } else if (opt.screen == "settings" || opt.screen == "settings-about" || opt.screen == "settings-sound" ||
             opt.screen == "settings-battery") {
    app_open_settings(app);
    if (opt.screen == "settings-battery") {  // scrolled to the Battery card
      const SettingsParts* sp = settings_parts(app_settings_view(app));
      lv_obj_update_layout(lv_screen_active());
      if (sp != nullptr && sp->batteryChoiceRows[0] != nullptr) {
        lv_obj_scroll_to_y(sp->list, lv_obj_get_y(lv_obj_get_parent(sp->batteryChoiceRows[0])) - 60, LV_ANIM_OFF);
      }
    }
    if (opt.screen == "settings-sound") {  // scrolled to the Sound card
      const SettingsParts* sp = settings_parts(app_settings_view(app));
      lv_obj_update_layout(lv_screen_active());
      if (sp != nullptr && sp->soundRows[0] != nullptr) {
        lv_obj_scroll_to_y(sp->list, lv_obj_get_y(lv_obj_get_parent(sp->soundRows[0])), LV_ANIM_OFF);
      }
    }
    if (opt.screen == "settings-about") {  // scrolled to the end: the About area
      const SettingsParts* sp = settings_parts(app_settings_view(app));
      lv_obj_update_layout(lv_screen_active());
      if (sp != nullptr) lv_obj_scroll_to_y(sp->list, LV_COORD_MAX, LV_ANIM_OFF);
    }
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
  } else if (opt.screen != "live" && opt.screen.rfind("live-last", 0) != 0) {
    app_open_match(app, opt.match);
    lv_obj_t* view = app_detail_view(app);
    if (opt.screen == "summary") detail_select_tab(view, DetailTab::Summary, false);
    if (opt.screen == "pitch") detail_select_tab(view, DetailTab::Pitch, false);
    if (opt.screen == "stats") detail_select_tab(view, DetailTab::Stats, false);
  }

  if (headless && opt.staleMin > 0) {
    // The helper went quiet: no more polls, the clock moves on.
    app_refresh(app, start + static_cast<int64_t>(opt.staleMin) * 60);
  }

  if (headless && !opt.banner.empty()) {
    if (opt.banner == "none") {
      if (lv_obj_t* b = app_battery_banner(app)) lv_obj_delete(b);
    } else if (opt.banner == "low") {
      app_show_battery_banner(app, Alert{Alert::Level::Low, battery::kAlertLowText, battery::kAlertLowMs});
    } else {
      app_show_battery_banner(
          app, Alert{Alert::Level::Critical, battery::kAlertCriticalText, battery::kAlertCriticalMs});
    }
  }

  if (headless && httpImages) {
    // A snapshot of real data: wait (a bounded time) for the pictures the
    // screen asked for. More data may arrive meanwhile and ask for more.
    const int64_t until = monotonic_ms() + kImageSnapshotMs;
    while (monotonic_ms() < until && httpImages->pending() > 0) {
      pump(start);
      if (host.take_matches_changed()) app_refresh(app, start);
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
    if (keys.battery) {
      keys.battery = false;
      if (opt.batteryMv > 0) {
        std::printf("the b key cycles the scripted battery (not with --battery-mv)\n");
      } else {
        std::printf("%s\n", MockPower::describe(mockPower.next_demo()).c_str());
        app_refresh(app, now);
      }
      std::fflush(stdout);
    }
    if (keys.millivolts != 0) {
      if (opt.batteryMv > 0) {
        modelPower.set_millivolts(std::max(0, modelPower.millivolts() + keys.millivolts));
        std::printf("ADC now reads %d mV\n", modelPower.millivolts());
      } else {
        std::printf("+ and - work with --battery-mv only\n");
      }
      keys.millivolts = 0;
      std::fflush(stdout);
    }
    host.tick(monotonic_ms(), now);
    SDL_Delay(5);
  }
}
