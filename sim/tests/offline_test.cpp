// Offline behaviour end to end: Poller + source + Store + app. A failing
// source keeps the last score on the Live screen with the stale marker, and
// Settings says why in plain English.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "fake_transport.h"
#include "http_source.h"
#include "fixture_source.h"
#include "mock_source.h"
#include "poller.h"
#include "prefs.h"
#include "screens/goal.h"
#include "screens/live.h"
#include "screens/settings.h"
#include "store.h"
#include "test_display.h"
#include "theme.h"

namespace {

constexpr int64_t kT0 = 1792526400 - 3 * 3600;  // Tue 2026-10-20 17:00 UTC

class NoImages : public ImageProvider {
 public:
  void request(const std::string&, std::function<void(ImagePtr)> done) override { done(nullptr); }
};

std::string text(lv_obj_t* label) { return lv_label_get_text(label); }
uint32_t u32(lv_color_t c) { return lv_color_to_u32(c); }

bool visible(lv_obj_t* obj) {
  for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

Team team(const char* id, const char* abbr, const char* name, uint32_t color) {
  Team t;
  t.id = id;
  t.abbr = abbr;
  t.name = name;
  t.color = color;
  t.primary = color;
  return t;
}

MatchSummary arsenal_live() {
  MatchSummary m;
  m.id = "m-live";
  m.league = "eng.1";
  m.state = MatchState::Live;
  m.clock = "67'";
  m.kickoff = kT0 - 67 * 60;
  m.home = team("359", "ARS", "Arsenal", 0xEF0107);
  m.away = team("363", "CHE", "Chelsea", 0x034694);
  m.homeScore = 2;
  m.awayScore = 1;
  return m;
}

// The app (favourite Arsenal by default) fed by a Poller over `source`.
struct Host {
  Store store;
  NoImages images;
  MemoryPrefs prefs;
  DataSource& source;
  lv_obj_t* app = nullptr;
  Poller poller;

  explicit Host(DataSource& src)
      : source(src), poller(store, src, [this](int64_t now) { app_matches_changed(app, now); }) {
    init_test_display();
    theme_reset();
    goal_set_reduced_motion(true);
    AppDeps deps{store, images, source, nullptr, prefs, nullptr, nullptr};
    app = app_create(lv_screen_active(), deps);
  }
  ~Host() {
    theme_reset();
    goal_set_reduced_motion(false);
    deinit_test_display();
  }

  // Ticks the poller at `seconds` after kT0 and redraws.
  void at(int64_t seconds) {
    poller.tick(seconds * 1000, kT0 + seconds);
    app_refresh(app, kT0 + seconds);
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }

  const LiveParts* live() const {
    lv_obj_t* tab = lv_obj_get_child(lv_tabview_get_content(app), 0);
    return live_parts(lv_obj_get_child(tab, 0));
  }
  const SettingsParts* settings() const { return settings_parts(app_settings_view(app)); }
};

std::string score(const LiveParts* p) {
  return text(p->scoreHome) + text(p->scoreSep) + text(p->scoreAway);
}

}  // namespace

TEST_CASE("offline: the Live screen keeps the last score and shows the stale marker") {
  MockSource source;
  source.set_matches({arsenal_live()});
  Host h(source);
  h.at(0);
  const LiveParts* p = h.live();
  REQUIRE(p != nullptr);
  REQUIRE(h.store.has_data());
  CHECK(score(p) == "2:1");
  CHECK(text(p->pillLabel) == "67'");
  CHECK_FALSE(visible(p->staleDot));
  CHECK_FALSE(visible(p->staleLabel));

  // The helper goes away.
  source.set_fail(true);
  for (int64_t s = 1; s <= 200; ++s) CHECK_NOTHROW(h.at(s));
  CHECK(h.store.failures() >= 3);
  CHECK(h.store.last_error() == FetchError::Offline);
  CHECK(h.store.matches().size() == 1);
  CHECK_FALSE(h.store.stale(kT0 + 90));
  CHECK(h.store.stale(kT0 + 91));

  // Still the last score, flagged as old.
  CHECK(score(p) == "2:1");
  CHECK(visible(p->scoreArea));
  CHECK(visible(p->staleDot));
  CHECK(visible(p->staleLabel));
  CHECK(text(p->staleLabel) == "Updated 3 min ago");
  CHECK(u32(lv_obj_get_style_bg_color(p->staleDot, 0)) == u32(theme().amber));

  // Settings says why, in amber.
  app_open_settings(h.app);
  h.at(201);
  REQUIRE(h.settings() != nullptr);
  CHECK(text(h.settings()->dataStatus) == "Cannot reach the helper");
  CHECK(u32(lv_obj_get_style_text_color(h.settings()->dataStatus, 0)) == u32(theme().amber));

  // Back online: the next poll clears the marker and the status line.
  source.set_fail(false);
  h.poller.request_matches_now();
  h.at(202);
  CHECK_FALSE(visible(p->staleDot));
  CHECK(text(h.settings()->dataStatus) == "Live data is up to date");
  CHECK(u32(lv_obj_get_style_text_color(h.settings()->dataStatus, 0)) == u32(theme().dim));
}

TEST_CASE("offline: a 401 from the helper shows 'Helper rejected the token' in Settings") {
  FakeTransport transport;
  HttpSource source(transport, "http://helper.test", "wrong-token");
  Host h(source);
  app_open_settings(h.app);
  h.at(0);
  REQUIRE(h.settings() != nullptr);
  CHECK(text(h.settings()->dataStatus) == "Waiting for the first update...");
  CHECK(u32(lv_obj_get_style_text_color(h.settings()->dataStatus, 0)) == u32(theme().dim));

  // matches, teams and news, one at a time: all refused.
  for (int i = 0; i < 3; ++i) transport.respond(401, "{\"error\":\"unauthorized\"}");
  CHECK(transport.flush() == 3);
  h.at(1);
  CHECK(h.store.last_error() == FetchError::Auth);
  CHECK_FALSE(h.store.has_data());
  CHECK(text(h.settings()->dataStatus) == "Helper rejected the token");
  CHECK(u32(lv_obj_get_style_text_color(h.settings()->dataStatus, 0)) == u32(theme().amber));
  // Settings opened later shows it too.
  app_close_settings(h.app);
  app_open_settings(h.app);
  CHECK(text(h.settings()->dataStatus) == "Helper rejected the token");
}

TEST_CASE("app_open_match_id names the open detail, for the poller's detail watch") {
  FixtureSource source(CONTRACT_FIXTURES_DIR);
  Host h(source);
  h.at(0);
  CHECK(app_open_match_id(h.app).empty());
  app_open_match(h.app, "401879257");
  CHECK(app_open_match_id(h.app) == "401879257");
  h.poller.set_detail_watch(app_open_match_id(h.app));
  app_close_detail(h.app);
  CHECK(app_open_match_id(h.app).empty());
  CHECK(app_open_match_id(nullptr).empty());
}
