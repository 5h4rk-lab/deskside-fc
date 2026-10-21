// Poller: when fetches happen and what their results do to the Store. Time is
// manual (monotonic ms and epoch seconds given to tick()).
#include <doctest/doctest.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "data_source.h"
#include "mock_source.h"
#include "model.h"
#include "poller.h"
#include "store.h"

namespace {

constexpr int64_t kEpoch = 1792526400;

MatchSummary match(const std::string& id, MatchState state, int home = 0, int away = 0) {
  MatchSummary m;
  m.id = id;
  m.league = "eng.1";
  m.state = state;
  m.home.id = "359";
  m.away.id = "363";
  if (state != MatchState::Pre) {
    m.homeScore = home;
    m.awayScore = away;
  }
  return m;
}

// Records every fetch and keeps its callback until the test answers it.
class ScriptedSource : public DataSource {
 public:
  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override {
    calls.push_back("matches");
    matches.push_back(std::move(cb));
  }
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override {
    calls.push_back("detail:" + id);
    details.push_back(std::move(cb));
  }
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override {
    calls.push_back("news");
    news.push_back(std::move(cb));
  }
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override {
    calls.push_back("teams");
    teams.push_back(std::move(cb));
  }
  FetchError last_error() const override { return error; }

  // Answers the oldest pending matches fetch.
  void answer_matches(bool ok, std::vector<MatchSummary> data = {}, FetchError err = FetchError::None) {
    REQUIRE(!matches.empty());
    auto cb = std::move(matches.front());
    matches.erase(matches.begin());
    error = ok ? FetchError::None : err;
    cb(ok, ok ? std::move(data) : std::vector<MatchSummary>{});
  }
  void answer_news(bool ok, FetchError err = FetchError::None) {
    REQUIRE(!news.empty());
    auto cb = std::move(news.front());
    news.erase(news.begin());
    error = ok ? FetchError::None : err;
    NewsItem n;
    n.id = "n1";
    cb(ok, ok ? std::vector<NewsItem>{n} : std::vector<NewsItem>{});
  }
  void answer_teams(bool ok, FetchError err = FetchError::None) {
    REQUIRE(!teams.empty());
    auto cb = std::move(teams.front());
    teams.erase(teams.begin());
    error = ok ? FetchError::None : err;
    Team t;
    t.id = "359";
    cb(ok, ok ? std::vector<Team>{t} : std::vector<Team>{});
  }
  void answer_detail(bool ok, const std::string& id) {
    REQUIRE(!details.empty());
    auto cb = std::move(details.front());
    details.erase(details.begin());
    MatchDetail d;
    d.match.id = id;
    error = ok ? FetchError::None : FetchError::Server;
    cb(ok, ok ? d : MatchDetail{});
  }

  int count(const std::string& kind) const {
    int n = 0;
    for (const std::string& c : calls) n += c == kind ? 1 : 0;
    return n;
  }

  std::vector<std::string> calls;
  std::vector<std::function<void(bool, std::vector<MatchSummary>)>> matches;
  std::vector<std::function<void(bool, MatchDetail)>> details;
  std::vector<std::function<void(bool, std::vector<NewsItem>)>> news;
  std::vector<std::function<void(bool, std::vector<Team>)>> teams;
  FetchError error = FetchError::None;
};

struct Fixture {
  Store store;
  ScriptedSource source;
  std::vector<int64_t> changed;
  Poller poller{store, source, [this](int64_t now) { changed.push_back(now); }};

  // Monotonic ms since the start; the epoch follows it.
  void at(int64_t ms) { poller.tick(ms, kEpoch + ms / 1000); }

  // First tick, answered: matches (live or not), teams, news.
  void start(bool live) {
    at(0);
    source.answer_matches(true, {match("m1", live ? MatchState::Live : MatchState::Pre)});
    source.answer_teams(true);
    source.answer_news(true);
    source.calls.clear();
  }
};

}  // namespace

TEST_CASE("poller: the first tick fetches matches, teams and news, in that order") {
  Fixture f;
  f.at(0);
  CHECK(f.source.calls == std::vector<std::string>{"matches", "teams", "news"});
  f.at(10);
  f.at(1000);
  CHECK(f.source.calls.size() == 3);  // nothing more while they are pending
}

TEST_CASE("poller: a matches success fills the Store and reports the change") {
  Fixture f;
  f.at(5000);
  f.source.answer_matches(true, {match("m1", MatchState::Live, 1, 0)});
  REQUIRE(f.store.matches().size() == 1);
  CHECK(f.store.has_data());
  CHECK(f.store.last_success() == kEpoch + 5);
  CHECK(f.store.last_error() == FetchError::None);
  CHECK(f.changed == std::vector<int64_t>{kEpoch + 5});
  f.source.answer_teams(true);
  f.source.answer_news(true);
  CHECK(f.store.teams().size() == 1);
  CHECK(f.store.news().size() == 1);
}

TEST_CASE("poller: matches again after 25 s with a live match") {
  Fixture f;
  f.start(true);
  f.at(24999);
  CHECK(f.source.calls.empty());
  f.at(25000);
  CHECK(f.source.calls == std::vector<std::string>{"matches"});
}

TEST_CASE("poller: matches again after 180 s without a live match") {
  Fixture f;
  f.start(false);
  f.at(25000);
  f.at(179999);
  CHECK(f.source.calls.empty());
  f.at(180000);
  CHECK(f.source.calls == std::vector<std::string>{"matches"});
}

TEST_CASE("poller: the delay runs from the start of the last attempt, not its answer") {
  Fixture f;
  f.at(0);
  f.at(20000);  // the answer comes 20 s late
  f.source.answer_matches(true, {match("m1", MatchState::Live)});
  f.source.calls.clear();
  f.at(24999);
  CHECK(f.source.count("matches") == 0);
  f.at(25000);
  CHECK(f.source.count("matches") == 1);
}

TEST_CASE("poller: a failure keeps the old matches, counts, sets the error and backs off") {
  Fixture f;
  f.start(true);
  REQUIRE(f.changed.size() == 1);

  f.at(25000);
  f.source.answer_matches(false, {}, FetchError::Offline);
  REQUIRE(f.store.matches().size() == 1);  // kept
  CHECK(f.store.matches()[0].id == "m1");
  CHECK(f.store.failures() == 1);
  CHECK(f.store.last_error() == FetchError::Offline);
  CHECK(f.store.data_status_text() == "Cannot reach the helper");
  CHECK(f.changed.size() == 1);  // not reported
  CHECK(f.store.last_success() == kEpoch);

  // One failure: the base delay.
  f.at(49999);
  CHECK(f.source.count("matches") == 1);
  f.at(50000);
  CHECK(f.source.count("matches") == 2);
  f.source.answer_matches(false, {}, FetchError::Server);
  CHECK(f.store.failures() == 2);
  CHECK(f.store.last_error() == FetchError::Server);

  // Two failures with a live match: 50 s.
  f.at(99999);
  CHECK(f.source.count("matches") == 2);
  f.at(100000);
  CHECK(f.source.count("matches") == 3);

  // A success resets the back-off and the error.
  f.source.answer_matches(true, {match("m1", MatchState::Live, 2, 0)});
  CHECK(f.store.failures() == 0);
  CHECK(f.store.last_error() == FetchError::None);
  CHECK(f.changed.size() == 2);
  f.at(125000);
  CHECK(f.source.count("matches") == 4);
}

TEST_CASE("poller: offline for long enough makes the Store stale; nothing throws") {
  Fixture f;
  f.start(true);
  int64_t ms = 0;
  for (int i = 0; i < 6; ++i) {
    ms += 300000;
    CHECK_NOTHROW(f.at(ms));
    if (!f.source.matches.empty()) f.source.answer_matches(false, {}, FetchError::Offline);
  }
  CHECK(f.store.has_data());
  CHECK(f.store.matches().size() == 1);
  CHECK_FALSE(f.store.stale(kEpoch + 90));
  CHECK(f.store.stale(kEpoch + 91));
  CHECK(f.store.stale(kEpoch + ms / 1000));
}

TEST_CASE("poller: a pending fetch is not duplicated; a lost one is retried after 30 s") {
  Fixture f;
  f.start(true);
  f.at(25000);
  CHECK(f.source.count("matches") == 1);
  f.at(50000);
  f.at(54999);
  CHECK(f.source.count("matches") == 1);  // still waiting for the answer
  f.at(55000);                             // 30 s without an answer: lost
  CHECK(f.source.count("matches") == 2);

  // The late answer of the lost fetch is ignored.
  f.source.answer_matches(true, {match("late", MatchState::Live)});
  CHECK(f.store.matches()[0].id == "m1");
  CHECK(f.changed.size() == 1);
  // The retry's answer counts.
  f.source.answer_matches(true, {match("fresh", MatchState::Live)});
  CHECK(f.store.matches()[0].id == "fresh");
  CHECK(f.changed.size() == 2);
}

TEST_CASE("poller: request_matches_now fetches at the next tick, even during back-off") {
  Fixture f;
  f.start(false);
  f.at(1000);
  CHECK(f.source.calls.empty());
  f.poller.request_matches_now();
  f.at(1001);
  CHECK(f.source.calls == std::vector<std::string>{"matches"});
  // Asking again while the fetch is pending does not duplicate it...
  f.poller.request_matches_now();
  f.at(1002);
  CHECK(f.source.count("matches") == 1);
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  // ...but is remembered: the fetch in flight may predate the reason for asking.
  f.at(2000);
  CHECK(f.source.count("matches") == 2);
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  f.at(3000);
  CHECK(f.source.count("matches") == 2);  // asked once, fetched once more
}

TEST_CASE("poller: a forced request made while a fetch is pending survives its failure") {
  Fixture f;
  f.start(false);
  f.poller.request_matches_now();
  f.at(1000);
  REQUIRE(f.source.count("matches") == 1);
  f.poller.request_matches_now();  // e.g. Wi-Fi just connected
  f.at(1500);
  CHECK(f.source.count("matches") == 1);
  f.source.answer_matches(false, {}, FetchError::Offline);  // back-off would say 180 s
  f.at(1501);
  CHECK(f.source.count("matches") == 2);
}

TEST_CASE("poller: a forced request made while a fetch is pending survives its loss") {
  Fixture f;
  f.start(false);
  f.poller.request_matches_now();
  f.at(1000);
  f.poller.request_matches_now();
  f.at(30999);
  CHECK(f.source.count("matches") == 1);
  f.at(31000);  // lost after 30 s: the remembered request goes out at once
  CHECK(f.source.count("matches") == 2);
}

TEST_CASE("poller: request_all_now with empty teams and news fetches all three in order") {
  Fixture f;
  f.at(0);
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  f.source.answer_teams(false, FetchError::Offline);
  f.source.answer_news(false, FetchError::Offline);
  f.source.calls.clear();
  f.at(1000);
  CHECK(f.source.calls.empty());  // teams wait 60 s, news 5 min
  f.poller.request_all_now();
  f.at(1001);
  CHECK(f.source.calls == std::vector<std::string>{"matches", "teams", "news"});
}

TEST_CASE("poller: request_all_now with teams and news in only fetches matches") {
  Fixture f;
  f.start(false);
  f.poller.request_all_now();
  f.at(1000);
  CHECK(f.source.calls == std::vector<std::string>{"matches"});
}

TEST_CASE("poller: request_all_now refetches news whose last fetch failed") {
  Fixture f;
  f.start(false);
  f.at(300000);  // news due
  REQUIRE(f.source.count("news") == 1);
  f.source.answer_news(false, FetchError::Server);
  REQUIRE_FALSE(f.store.news().empty());  // kept
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  f.source.calls.clear();
  f.poller.request_all_now();
  f.at(300001);
  CHECK(f.source.calls == std::vector<std::string>{"matches", "news"});
}

TEST_CASE("poller: request_all_now while teams and news are in flight honours them after") {
  Fixture f;
  f.at(0);
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  f.source.calls.clear();
  f.poller.request_all_now();  // teams and news still pending
  f.at(10);
  CHECK(f.source.calls == std::vector<std::string>{"matches"});
  f.source.answer_teams(false, FetchError::Offline);
  f.source.answer_news(false, FetchError::Offline);
  f.at(20);
  CHECK(f.source.calls == std::vector<std::string>{"matches", "teams", "news"});
}

TEST_CASE("poller: a monotonic clock that jumps backwards restarts the timing") {
  Fixture f;
  f.at(100000);
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  f.source.answer_teams(true);
  f.source.answer_news(true);
  f.source.calls.clear();
  f.at(5000);  // the clock went back (a wrapped counter)
  CHECK(f.source.calls.empty());
  f.at(184999);
  CHECK(f.source.count("matches") == 0);
  f.at(185000);  // 180 s after the new base
  CHECK(f.source.count("matches") == 1);
}

TEST_CASE("poller: a backwards clock does not keep a pending fetch from being lost") {
  Fixture f;
  f.start(false);
  f.at(200000);
  REQUIRE(f.source.count("matches") == 1);  // pending, never answered
  f.at(1000);                               // clock back
  f.at(30999);
  CHECK(f.source.count("matches") == 1);
  f.at(31000);  // lost 30 s after the new base
  f.at(180999);
  CHECK(f.source.count("matches") == 1);
  f.at(181000);  // and retried by the normal delay, counted from the new base
  CHECK(f.source.count("matches") == 2);
}

TEST_CASE("poller: news every 5 min; teams only while empty, at most once a minute") {
  Fixture f;
  f.at(0);
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  f.source.answer_teams(false, FetchError::Server);
  f.source.answer_news(true);
  f.source.calls.clear();

  f.at(59999);
  CHECK(f.source.count("teams") == 0);
  f.at(60000);
  CHECK(f.source.count("teams") == 1);
  f.source.answer_teams(true);
  f.at(200000);
  f.at(300000);
  CHECK(f.source.count("teams") == 1);  // the list is in: never again

  CHECK(f.source.count("news") == 1);  // at 300 s
  f.source.answer_news(true);
  f.at(599999);
  CHECK(f.source.count("news") == 1);
  f.at(600000);
  CHECK(f.source.count("news") == 2);
}

TEST_CASE("poller: news and teams errors show only when matches are fine") {
  Fixture f;
  f.at(0);
  f.source.answer_matches(false, {}, FetchError::Offline);
  f.source.answer_teams(false, FetchError::Auth);
  f.source.answer_news(false, FetchError::Parse);
  CHECK(f.store.last_error() == FetchError::Offline);  // matches win

  // A matches success clears it.
  f.poller.request_matches_now();
  f.at(1000);
  f.source.answer_matches(true, {match("m1", MatchState::Pre)});
  CHECK(f.store.last_error() == FetchError::None);

  // With matches fine, a news or teams failure shows...
  f.at(60000);  // teams retry (the list is still empty)
  f.source.answer_teams(false, FetchError::Server);
  CHECK(f.store.last_error() == FetchError::Server);
  // ...and clears once neither news nor teams is failing.
  f.at(300000);
  f.source.answer_news(true);
  CHECK(f.store.last_error() == FetchError::Server);  // teams still failing
  f.source.answer_teams(true);
  CHECK(f.store.last_error() == FetchError::None);
  CHECK(f.store.news().size() == 1);
}

TEST_CASE("poller: a failed news or teams fetch keeps what the Store has") {
  Fixture f;
  f.start(false);
  f.at(300000);
  f.source.answer_news(false, FetchError::Server);
  CHECK(f.store.news().size() == 1);
  CHECK(f.store.teams().size() == 1);
}

TEST_CASE("poller: the watched detail refreshes every 15 s while its match is live") {
  Fixture f;
  f.start(true);
  // The detail is already cached (the app fetched it when it opened).
  MatchDetail cached;
  cached.match.id = "m1";
  f.store.set_detail(cached);
  f.at(1000);
  f.poller.set_detail_watch("m1");
  f.at(15999);
  CHECK(f.source.calls.empty());
  f.at(16000);
  CHECK(f.source.calls == std::vector<std::string>{"detail:m1"});
  MatchDetail before = *f.store.detail_for("m1");
  f.source.answer_detail(true, "m1");
  CHECK(f.store.detail_for("m1") != nullptr);
  f.at(31000);
  CHECK(f.source.count("detail:m1") == 2);
  f.source.answer_detail(false, "m1");
  CHECK(f.store.detail_for("m1") != nullptr);  // a failure keeps the cache
  CHECK(f.store.last_error() == FetchError::None);  // and is not a data error

  // No watch: no more detail fetches.
  f.poller.set_detail_watch("");
  f.at(200000);
  CHECK(f.source.count("detail:m1") == 2);
}

TEST_CASE("poller: a watched detail of a finished match refreshes every 60 s") {
  Fixture f;
  f.start(false);
  MatchDetail cached;
  cached.match.id = "m1";
  f.store.set_detail(cached);
  f.poller.set_detail_watch("m1");
  f.at(59999);
  CHECK(f.source.count("detail:m1") == 0);
  f.at(60000);
  CHECK(f.source.count("detail:m1") == 1);
}

TEST_CASE("poller: a new watch with nothing cached fetches at once; same id twice is a no-op") {
  Fixture f;
  f.start(true);
  f.poller.set_detail_watch("m1");
  f.at(10);
  CHECK(f.source.calls == std::vector<std::string>{"detail:m1"});
  f.poller.set_detail_watch("m1");
  f.at(20);
  CHECK(f.source.count("detail:m1") == 1);
  f.source.answer_detail(true, "m1");
  CHECK(f.store.detail_for("m1") != nullptr);
}

TEST_CASE("poller: works with a synchronous source (MockSource)") {
  Store store;
  MockSource source(CONTRACT_FIXTURES_DIR);
  int changed = 0;
  Poller poller(store, source, [&](int64_t) { ++changed; });
  poller.tick(0, kEpoch);
  CHECK(store.has_data());
  CHECK(store.matches().size() == 1);
  CHECK(store.teams().size() == 20);
  CHECK(store.news().size() == 6);
  CHECK(changed == 1);

  source.set_fail(true);
  poller.tick(180000, kEpoch + 180);
  CHECK(store.failures() == 1);
  CHECK(store.last_error() == FetchError::Offline);
  CHECK(store.matches().size() == 1);
  CHECK(changed == 1);

  source.set_fail(true, FetchError::Auth);
  poller.request_matches_now();
  poller.tick(180001, kEpoch + 180);
  CHECK(store.last_error() == FetchError::Auth);
  CHECK(store.data_status_text() == "Helper rejected the token");

  source.set_fail(false);
  poller.request_matches_now();
  poller.tick(180002, kEpoch + 180);
  CHECK(store.failures() == 0);
  CHECK(store.last_error() == FetchError::None);
  CHECK(changed == 2);
}

TEST_CASE("poller: a source calling back from inside fetch can be asked again from the callback") {
  // onMatchesChanged asks for a refresh (as a host might after a goal): no
  // recursion, the fetch happens at the next tick.
  Store store;
  MockSource source(CONTRACT_FIXTURES_DIR);
  int changed = 0;
  Poller* self = nullptr;
  Poller poller(store, source, [&](int64_t) {
    ++changed;
    self->request_matches_now();
  });
  self = &poller;
  poller.tick(0, kEpoch);
  CHECK(changed == 1);
  poller.tick(1, kEpoch);
  CHECK(changed == 2);
}

TEST_CASE("poller: an answer arriving after the poller is gone is ignored") {
  Store store;
  ScriptedSource source;
  {
    Poller poller(store, source, nullptr);
    poller.tick(0, kEpoch);
  }
  source.answer_matches(true, {match("m1", MatchState::Live)});
  CHECK_FALSE(store.has_data());
}
