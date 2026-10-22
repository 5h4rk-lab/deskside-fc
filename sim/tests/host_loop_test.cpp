// HostLoop: the order of the host's calls, the once-a-second refresh, the
// detail watch, and no work before the epoch is valid. Fakes only.
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <vector>

#include "data_source.h"
#include "host_loop.h"
#include "model.h"
#include "store.h"

namespace {

constexpr int64_t kEpoch = 1792526400;

MatchSummary match(const std::string& id, MatchState state, const std::string& homeId = "359") {
  MatchSummary m;
  m.id = id;
  m.league = "eng.1";
  m.state = state;
  m.home.id = homeId;
  m.away.id = "363";
  if (state != MatchState::Pre) {
    m.homeScore = 1;
    m.awayScore = 0;
  }
  return m;
}

// Answers at once (like MockSource) and records every call, in a log shared
// with the hooks so the order across both can be checked.
class LoggingSource : public DataSource {
 public:
  explicit LoggingSource(std::vector<std::string>& log) : log_(log) {}
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    log_.push_back("fetch matches");
    cb(true, matches);
  }
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override {
    log_.push_back("fetch detail " + id);
    MatchDetail d;
    d.match.id = id;
    cb(true, d);
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override {
    log_.push_back("fetch news");
    cb(true, {});
  }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override {
    log_.push_back("fetch teams");
    cb(true, {});
  }

  std::vector<MatchSummary> matches;

 private:
  std::vector<std::string>& log_;
};

struct Fixture {
  std::vector<std::string> log;
  Store store;
  LoggingSource source{log};
  std::string openId;
  std::vector<int64_t> refreshes;
  HostLoop::Hooks hooks() {
    HostLoop::Hooks h;
    h.pollTransports = [this] { log.push_back("poll"); };
    h.tickImages = [this](int64_t ms) { log.push_back("images " + std::to_string(ms)); };
    h.openMatchId = [this] { return openId; };
    h.onMatchesChanged = [this](int64_t) { log.push_back("changed"); };
    h.everySecond = [this](int64_t) { log.push_back("second"); };
    h.refresh = [this](int64_t now) {
      log.push_back("refresh");
      refreshes.push_back(now);
    };
    return h;
  }
  HostLoop loop{store, source, hooks()};

  int count(const std::string& what) const {
    int n = 0;
    for (const std::string& l : log) n += l == what ? 1 : 0;
    return n;
  }
};

}  // namespace

TEST_CASE("host loop: one tick runs the hooks in the simulator's order") {
  Fixture f;
  f.source.matches = {match("m1", MatchState::Pre)};
  f.loop.tick(1000, kEpoch);
  const std::vector<std::string> expected{"poll",         "images 1000",  "fetch matches", "changed",
                                          "fetch teams",  "fetch news",   "second",        "refresh"};
  CHECK(f.log == expected);
}

TEST_CASE("host loop: refreshes once per epoch second, not on every tick") {
  Fixture f;
  f.loop.tick(0, kEpoch);
  REQUIRE(f.refreshes.size() == 1);
  f.loop.tick(100, kEpoch);
  f.loop.tick(900, kEpoch);
  CHECK(f.refreshes.size() == 1);
  f.loop.tick(1000, kEpoch + 1);
  CHECK(f.refreshes.size() == 2);
  CHECK(f.refreshes.back() == kEpoch + 1);
  CHECK(f.count("second") == 2);
}

TEST_CASE("host loop: a matches change refreshes within the same second") {
  Fixture f;
  f.source.matches = {match("m1", MatchState::Live)};
  f.loop.tick(0, kEpoch);
  f.log.clear();
  f.loop.poller().request_matches_now();
  f.loop.tick(200, kEpoch);
  CHECK(f.log == std::vector<std::string>{"poll", "images 200", "fetch matches", "changed", "refresh"});
  CHECK(f.count("second") == 0);  // not a new second
  f.loop.tick(300, kEpoch);
  CHECK(f.refreshes.size() == 2);  // and nothing again on the next tick
}

TEST_CASE("host loop: no Poller tick and no refresh while the epoch is invalid") {
  Fixture f;
  f.source.matches = {match("m1", MatchState::Live)};
  for (int64_t bad : {int64_t{0}, int64_t{-1}, int64_t{-1792526400}}) {
    f.loop.tick(1000, bad);
    f.loop.tick(60000, bad);
  }
  CHECK(f.count("fetch matches") == 0);
  CHECK(f.count("fetch news") == 0);
  CHECK(f.count("fetch teams") == 0);
  CHECK(f.count("refresh") == 0);
  CHECK(f.count("second") == 0);
  CHECK(f.store.matches().empty());
  // Requests already in flight and the pictures still get their time.
  CHECK(f.count("poll") == 6);
  CHECK(f.count("images 1000") == 3);
  // The first valid epoch starts everything, and refreshes at once.
  f.loop.tick(61000, kEpoch);
  CHECK(f.count("fetch matches") == 1);
  CHECK(f.refreshes == std::vector<int64_t>{kEpoch});
}

TEST_CASE("host loop: pump never refreshes") {
  Fixture f;
  f.source.matches = {match("m1", MatchState::Pre)};
  f.loop.pump(0, kEpoch);
  CHECK(f.count("refresh") == 0);
  CHECK(f.count("fetch matches") == 1);
  CHECK(f.loop.take_matches_changed());
  CHECK_FALSE(f.loop.take_matches_changed());
}

TEST_CASE("host loop: note_refreshed holds the refresh until the second changes") {
  Fixture f;
  f.loop.note_refreshed(kEpoch);
  f.loop.tick(0, kEpoch);  // the poll's matches success refreshes
  const int afterFirst = f.count("refresh");
  f.log.clear();
  f.loop.tick(10, kEpoch);
  CHECK(f.count("refresh") == 0);
  f.loop.tick(20, kEpoch + 1);
  CHECK(f.count("refresh") == 1);
  CHECK(afterFirst == 1);  // only because the matches arrived
}

TEST_CASE("host loop: the detail watch follows the open match, else the favourite's live match") {
  Fixture f;
  f.store.set_favourite("359");
  f.source.matches = {match("other", MatchState::Live, "111"), match("fav", MatchState::Live, "359")};
  f.loop.tick(0, kEpoch);  // matches arrive; the watch is set at the next tick
  f.log.clear();
  f.loop.tick(1000, kEpoch + 1);
  CHECK(f.count("fetch detail fav") == 1);  // the favourite's live match

  f.openId = "other";
  f.log.clear();
  f.loop.tick(2000, kEpoch + 2);
  CHECK(f.count("fetch detail other") == 1);  // the open match wins at once

  f.openId.clear();
  f.log.clear();
  f.loop.tick(20000, kEpoch + 20);  // past the 15 s live detail delay
  CHECK(f.count("fetch detail fav") == 1);  // closed: back to the favourite
  CHECK(f.count("fetch detail other") == 0);
}

TEST_CASE("host loop: no open match and no live favourite watches nothing") {
  Fixture f;
  f.store.set_favourite("359");
  f.source.matches = {match("m1", MatchState::Pre, "359")};
  f.loop.tick(0, kEpoch);
  f.loop.tick(1000, kEpoch + 1);
  for (const std::string& l : f.log) CHECK(l.rfind("fetch detail", 0) != 0);
}

TEST_CASE("host loop: works with no optional hooks") {
  Store store;
  std::vector<std::string> log;
  LoggingSource source(log);
  source.matches = {match("m1", MatchState::Pre)};
  HostLoop::Hooks hooks;  // everything empty
  HostLoop loop(store, source, hooks);
  loop.tick(0, kEpoch);
  loop.tick(1000, kEpoch + 1);
  CHECK(store.matches().size() == 1);
}

TEST_CASE("host loop: paused fetches (a software update) still refresh and deliver") {
  Fixture f;
  f.source.matches = {match("m1", MatchState::Live)};
  f.loop.tick(0, kEpoch);
  REQUIRE(f.count("fetch matches") == 1);
  f.loop.set_fetches_paused(true);
  CHECK(f.loop.fetches_paused());
  f.loop.poller().request_all_now();
  for (int i = 1; i <= 10; ++i) f.loop.tick(i * 60000, kEpoch + i * 60);
  // No new fetch of any kind while paused; the screens still refresh every
  // second and requests in flight are still delivered.
  CHECK(f.count("fetch matches") == 1);
  CHECK(f.count("fetch news") == 1);
  CHECK(f.count("fetch teams") == 1);
  CHECK(f.count("refresh") == 11);
  CHECK(f.count("poll") == 11);
  // Resumed: the request made while paused runs at the next tick.
  f.loop.set_fetches_paused(false);
  f.loop.tick(11 * 60000, kEpoch + 11 * 60);
  CHECK(f.count("fetch matches") == 2);
}
