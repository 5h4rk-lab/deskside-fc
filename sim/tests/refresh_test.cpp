// RefreshPolicy: how long to wait before the next fetch.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "model.h"
#include "refresh_policy.h"
#include "store.h"

namespace {

constexpr int64_t kNow = 1792526400;

MatchSummary match(const std::string& id, MatchState state) {
  MatchSummary m;
  m.id = id;
  m.state = state;
  m.home.id = "1";
  m.away.id = "2";
  return m;
}

}  // namespace

TEST_CASE("refresh policy: 25 s while any match is live, 180 s otherwise") {
  RefreshPolicy policy;
  Store s;
  CHECK(policy.next_delay_ms(s, kNow) == 180000);  // no data yet
  s.set_matches({match("a", MatchState::Pre), match("b", MatchState::Post)}, kNow);
  CHECK(policy.next_delay_ms(s, kNow) == 180000);
  s.set_matches({match("a", MatchState::Pre), match("b", MatchState::Live)}, kNow);
  CHECK(policy.next_delay_ms(s, kNow) == 25000);
  s.set_matches({match("a", MatchState::Postponed)}, kNow);
  CHECK(policy.next_delay_ms(s, kNow) == 180000);
}

TEST_CASE("refresh policy: doubles after each failure from the second one") {
  RefreshPolicy policy;
  Store s;
  s.set_matches({match("live", MatchState::Live)}, kNow);
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 25000);
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 50000);
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 100000);
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 200000);
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 300000);  // capped
  for (int i = 0; i < 100; ++i) s.mark_failure();  // no overflow
  CHECK(policy.next_delay_ms(s, kNow) == 300000);
}

TEST_CASE("refresh policy: the cap applies without a live match too") {
  RefreshPolicy policy;
  Store s;
  s.set_matches({match("a", MatchState::Pre)}, kNow);
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 180000);
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 300000);
}

TEST_CASE("refresh policy: a success resets the back-off") {
  RefreshPolicy policy;
  Store s;
  s.set_matches({match("live", MatchState::Live)}, kNow);
  s.mark_failure();
  s.mark_failure();
  s.mark_failure();
  CHECK(policy.next_delay_ms(s, kNow) == 100000);
  // The live match is still there after the failures (old data is kept).
  s.set_matches({match("live", MatchState::Live)}, kNow + 200);
  CHECK(policy.next_delay_ms(s, kNow + 200) == 25000);
}

TEST_CASE("refresh policy: news every 5 min, an open detail every 15 s while live") {
  RefreshPolicy policy;
  CHECK(policy.news_delay_ms() == 300000);
  CHECK(policy.detail_delay_ms() == 15000);
  CHECK(policy.detail_delay_ms(true) == 15000);
  CHECK(policy.detail_delay_ms(false) == 60000);
}
