// HttpSource over a FakeTransport: URLs, token, status and body handling,
// the detail league lookup and request ordering. No network.
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fake_transport.h"
#include "http_source.h"
#include "model.h"

namespace {

std::string fixture(const std::string& name) {
  std::ifstream in(std::string(CONTRACT_FIXTURES_DIR) + "/" + name, std::ios::binary);
  REQUIRE(in.good());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
    s.replace(pos, from.size(), to);
  }
  return s;
}

constexpr const char* kBase = "http://helper.test:8787";
constexpr const char* kToken = "s3cret-token";

struct Fixture {
  FakeTransport transport;
  HttpSource source{transport, kBase, kToken};
};

}  // namespace

TEST_CASE("http source: matches URL lists the leagues; token and timeout are passed") {
  Fixture f;
  f.source.fetch_matches([](bool, std::vector<MatchSummary>) {});
  REQUIRE(f.transport.requests.size() == 1);
  CHECK(f.transport.requests[0].url ==
        "http://helper.test:8787/api/matches?leagues=eng.1,esp.1,ita.1,ger.1,fra.1");
  CHECK(f.transport.requests[0].token == kToken);
  CHECK(f.transport.requests[0].timeoutMs == kHttpTimeoutMs);
  CHECK(kHttpTimeoutMs == 8000);
}

TEST_CASE("http source: custom leagues, news league, and a base URL with a trailing slash") {
  FakeTransport t;
  HttpSource src(t, "https://h.example/", "tok", {"eng.1", "usa.1"}, "esp.1");
  src.fetch_matches([](bool, std::vector<MatchSummary>) {});
  t.respond(500, "");
  t.flush();
  src.fetch_news([](bool, std::vector<NewsItem>) {});
  t.respond(500, "");
  t.flush();
  src.fetch_teams([](bool, std::vector<Team>) {});
  REQUIRE(t.requests.size() == 3);
  CHECK(t.requests[0].url == "https://h.example/api/matches?leagues=eng.1,usa.1");
  CHECK(t.requests[1].url == "https://h.example/api/news?league=esp.1");
  CHECK(t.requests[2].url == "https://h.example/api/teams");
}

TEST_CASE("http source: 200 with valid bodies gives ok with the parsed data") {
  Fixture f;
  std::vector<MatchSummary> matches;
  std::vector<NewsItem> news;
  std::vector<Team> teams;
  MatchDetail detail;
  bool okM = false, okN = false, okT = false, okD = false;
  f.source.fetch_matches([&](bool ok, std::vector<MatchSummary> v) { okM = ok; matches = std::move(v); });
  f.transport.respond(200, fixture("matches-normalised.json"));
  f.transport.flush();
  f.source.fetch_news([&](bool ok, std::vector<NewsItem> v) { okN = ok; news = std::move(v); });
  f.transport.respond(200, fixture("news-normalised.json"));
  f.transport.flush();
  f.source.fetch_teams([&](bool ok, std::vector<Team> v) { okT = ok; teams = std::move(v); });
  f.transport.respond(200, fixture("teams-normalised.json"));
  f.transport.flush();
  f.source.fetch_detail("401879257", [&](bool ok, MatchDetail d) { okD = ok; detail = std::move(d); });
  f.transport.respond(200, fixture("detail-normalised.json"));
  f.transport.flush();

  CHECK(okM);
  REQUIRE(matches.size() == 1);
  CHECK(matches[0].id == "401879257");
  CHECK(okN);
  CHECK(news.size() == 6);
  CHECK(okT);
  CHECK(teams.size() == 20);
  CHECK(okD);
  CHECK(detail.match.id == "401879257");
  CHECK(f.source.last_error() == FetchError::None);
  REQUIRE(f.transport.requests.size() == 4);
  CHECK(f.transport.requests[1].url == "http://helper.test:8787/api/news?league=eng.1");
  CHECK(f.transport.requests[2].url == "http://helper.test:8787/api/teams");
  CHECK(f.transport.requests[3].url == "http://helper.test:8787/api/match/401879257?league=eng.1");
}

TEST_CASE("http source: failures are classified and give ok=false with empty data") {
  struct Case {
    int status;
    bool transportError;
    std::string body;
    FetchError expected;
  };
  const std::string good = fixture("matches-normalised.json");
  const std::vector<Case> cases = {
      {401, false, "{\"error\":\"unauthorized\"}", FetchError::Auth},
      {500, false, "", FetchError::Server},
      {502, false, "{\"error\":\"upstream\"}", FetchError::Server},
      {400, false, "", FetchError::Server},
      {404, false, good, FetchError::Server},  // a good body with a bad status is still a failure
      {0, true, "", FetchError::Offline},
      {200, false, good.substr(0, good.size() / 2), FetchError::Parse},  // truncated
      {200, false, "<html>nope</html>", FetchError::Parse},
      {200, false, "", FetchError::Parse},
  };
  for (const Case& c : cases) {
    CAPTURE(c.status);
    CAPTURE(c.body);
    Fixture f;
    bool called = false, ok = true;
    std::vector<MatchSummary> got{MatchSummary{}};
    f.source.fetch_matches([&](bool o, std::vector<MatchSummary> v) {
      called = true;
      ok = o;
      got = std::move(v);
      // The error is already set when the callback runs.
      CHECK(f.source.last_error() == c.expected);
    });
    if (c.transportError) {
      f.transport.respond_transport_error();
    } else {
      f.transport.respond(c.status, c.body);
    }
    f.transport.flush();
    CHECK(called);
    CHECK_FALSE(ok);
    CHECK(got.empty());
    CHECK(f.source.last_error() == c.expected);
  }
}

TEST_CASE("http source: failures of news, teams and detail") {
  Fixture f;
  bool okN = true, okT = true, okD = true;
  std::vector<NewsItem> news{NewsItem{}};
  f.source.fetch_news([&](bool ok, std::vector<NewsItem> v) { okN = ok; news = std::move(v); });
  f.transport.respond(401, "");
  f.transport.flush();
  CHECK_FALSE(okN);
  CHECK(news.empty());
  CHECK(f.source.last_error() == FetchError::Auth);

  f.source.fetch_teams([&](bool ok, std::vector<Team>) { okT = ok; });
  f.transport.respond(200, "{\"teams\":");
  f.transport.flush();
  CHECK_FALSE(okT);
  CHECK(f.source.last_error() == FetchError::Parse);

  MatchDetail d;
  d.match.id = "x";
  f.source.fetch_detail("1", [&](bool ok, MatchDetail v) { okD = ok; d = std::move(v); });
  f.transport.respond_transport_error();
  f.transport.flush();
  CHECK_FALSE(okD);
  CHECK(d.match.id.empty());
  CHECK(f.source.last_error() == FetchError::Offline);

  // A success clears the error.
  f.source.fetch_news([&](bool ok, std::vector<NewsItem>) { okN = ok; });
  f.transport.respond(200, fixture("news-normalised.json"));
  f.transport.flush();
  CHECK(okN);
  CHECK(f.source.last_error() == FetchError::None);
}

TEST_CASE("http source: the detail uses the league from the last matches result") {
  Fixture f;
  // The fixture's match moved to La Liga.
  const std::string body = replace_all(fixture("matches-normalised.json"), "\"eng.1\"", "\"esp.1\"");
  f.source.fetch_matches([](bool ok, std::vector<MatchSummary>) { CHECK(ok); });
  f.transport.respond(200, body);
  f.transport.flush();
  f.source.fetch_detail("401879257", [](bool, MatchDetail) {});
  // Unknown ids try the Premier League; ids are URL-encoded.
  f.source.fetch_detail("a b/c", [](bool, MatchDetail) {});
  f.transport.respond(500, "");
  f.transport.respond(500, "");
  f.transport.flush();
  REQUIRE(f.transport.requests.size() == 3);
  CHECK(f.transport.requests[1].url == "http://helper.test:8787/api/match/401879257?league=esp.1");
  CHECK(f.transport.requests[2].url == "http://helper.test:8787/api/match/a%20b%2Fc?league=eng.1");
}

TEST_CASE("http source: a failed matches fetch keeps the leagues learnt before") {
  Fixture f;
  const std::string body = replace_all(fixture("matches-normalised.json"), "\"eng.1\"", "\"ita.1\"");
  f.source.fetch_matches([](bool, std::vector<MatchSummary>) {});
  f.transport.respond(200, body);
  f.source.fetch_matches([](bool, std::vector<MatchSummary>) {});
  f.transport.respond(502, "");
  f.source.fetch_detail("401879257", [](bool, MatchDetail) {});
  f.transport.respond(500, "");
  f.transport.flush();
  REQUIRE(f.transport.requests.size() == 3);
  CHECK(f.transport.requests[2].url == "http://helper.test:8787/api/match/401879257?league=ita.1");
}

TEST_CASE("http source: a detail result teaches the league of its match") {
  Fixture f;
  const std::string body = replace_all(fixture("detail-normalised.json"), "\"eng.1\"", "\"ger.1\"");
  f.source.fetch_detail("401879257", [](bool ok, MatchDetail) { CHECK(ok); });
  f.transport.respond(200, body);
  f.transport.flush();
  f.source.fetch_detail("401879257", [](bool, MatchDetail) {});
  REQUIRE(f.transport.requests.size() == 2);
  CHECK(f.transport.requests[0].url == "http://helper.test:8787/api/match/401879257?league=eng.1");
  CHECK(f.transport.requests[1].url == "http://helper.test:8787/api/match/401879257?league=ger.1");
}

TEST_CASE("http source: requests made together run one at a time in FIFO order") {
  Fixture f;
  std::vector<std::string> order;
  f.source.fetch_matches([&](bool, std::vector<MatchSummary>) { order.push_back("matches"); });
  f.source.fetch_teams([&](bool, std::vector<Team>) { order.push_back("teams"); });
  f.source.fetch_news([&](bool, std::vector<NewsItem>) { order.push_back("news"); });
  // Only the first one is on the wire.
  CHECK(f.transport.requests.size() == 1);
  CHECK(f.transport.pending() == 1);

  f.transport.respond(500, "");
  REQUIRE(f.transport.deliver_next());
  CHECK(f.transport.requests.size() == 2);
  CHECK(f.transport.requests[1].url == "http://helper.test:8787/api/teams");

  // A request made from inside a callback goes to the back of the queue.
  f.transport.respond(500, "");
  f.transport.respond(500, "");
  f.transport.respond(500, "");
  f.source.fetch_detail("9", [&](bool, MatchDetail) { order.push_back("detail"); });
  CHECK(f.transport.flush() == 3);
  CHECK(order == std::vector<std::string>{"matches", "teams", "news", "detail"});
  REQUIRE(f.transport.requests.size() == 4);
  CHECK(f.transport.requests[2].url == "http://helper.test:8787/api/news?league=eng.1");
  CHECK(f.transport.requests[3].url == "http://helper.test:8787/api/match/9?league=eng.1");
  CHECK(f.transport.pending() == 0);
}

TEST_CASE("http source: a callback issuing the next fetch keeps one request in flight") {
  Fixture f;
  int calls = 0;
  std::function<void(bool, std::vector<MatchSummary>)> again = [&](bool, std::vector<MatchSummary>) {
    if (++calls < 3) f.source.fetch_matches(again);
  };
  f.source.fetch_matches(again);
  for (int i = 0; i < 3; ++i) {
    CHECK(f.transport.pending() == 1);
    f.transport.respond(500, "");
    f.transport.deliver_next();
  }
  CHECK(calls == 3);
  CHECK(f.transport.pending() == 0);
}

TEST_CASE("http source: the queue drops the oldest duplicate news and teams requests past 8") {
  Fixture f;
  int matchesCalls = 0, newsCalls = 0, teamsCalls = 0, newsOk = 0;
  f.source.fetch_matches([&](bool, std::vector<MatchSummary>) { ++matchesCalls; });  // in flight
  for (int i = 0; i < 6; ++i) {
    f.source.fetch_news([&](bool ok, std::vector<NewsItem>) {
      ++newsCalls;
      newsOk += ok ? 1 : 0;
    });
  }
  for (int i = 0; i < 5; ++i) f.source.fetch_teams([&](bool, std::vector<Team>) { ++teamsCalls; });
  CHECK(f.source.queued() <= HttpSource::kMaxQueued);
  CHECK(HttpSource::kMaxQueued == 8);

  f.transport.respond(500, "");  // matches
  for (int i = 0; i < 20; ++i) f.transport.respond(200, fixture("news-normalised.json"));
  f.transport.flush();
  // Every caller still hears back exactly once (dropped duplicates share the
  // answer of the request that replaced them), with fewer requests on the wire.
  CHECK(matchesCalls == 1);
  CHECK(newsCalls == 6);
  CHECK(teamsCalls == 5);
  CHECK(newsOk == 6);
  CHECK(f.transport.requests.size() <= 1 + HttpSource::kMaxQueued);
  CHECK(f.transport.requests.size() < 12);
}

TEST_CASE("http source: matches and detail requests are never dropped") {
  Fixture f;
  int calls = 0;
  for (int i = 0; i < 12; ++i) f.source.fetch_detail(std::to_string(i), [&](bool, MatchDetail) { ++calls; });
  for (int i = 0; i < 12; ++i) f.transport.respond(500, "");
  f.transport.flush();
  CHECK(calls == 12);
  CHECK(f.transport.requests.size() == 12);
}

TEST_CASE("http source: an answer arriving after the source is gone is ignored") {
  FakeTransport t;
  bool called = false;
  {
    HttpSource src(t, kBase, kToken);
    src.fetch_matches([&](bool, std::vector<MatchSummary>) { called = true; });
  }
  t.respond(200, fixture("matches-normalised.json"));
  t.flush();
  CHECK_FALSE(called);
}
