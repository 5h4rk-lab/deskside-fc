// RequestDeadline and read_body: one total time budget per HTTP request, and
// the body loop that re-checks it before every read. The firmware's transport
// runs the same read_body() over esp_http_client_read; here a fake client
// stalls, trickles and fails on a fake clock.
#include <doctest/doctest.h>

#include <stdint.h>

#include <string>
#include <vector>

#include "request_deadline.h"

namespace {

// A fake client: a script of reads, each with the bytes it returns (or an
// error) and how long it takes on the fake clock.
struct FakeRead {
  int result;  // > 0 bytes, 0 end, < 0 error
  int64_t costMs;
};

struct FakeClient {
  std::vector<FakeRead> script;
  size_t next = 0;
  int64_t clock = 0;
  int calls = 0;
  int maxAsked = 0;

  int read(char* buf, int len) {
    ++calls;
    if (len > maxAsked) maxAsked = len;
    const FakeRead r = next < script.size() ? script[next++] : FakeRead{0, 0};
    clock += r.costMs;
    const int n = r.result > len ? len : r.result;
    for (int i = 0; i < n; ++i) buf[i] = 'x';
    return r.result > 0 ? n : r.result;
  }
};

BodyResult run(FakeClient& c, int totalMs, size_t cap, int64_t contentLength, std::string& body, int chunk = 1024) {
  RequestDeadline d(c.clock, totalMs);
  std::vector<char> scratch(static_cast<size_t>(chunk));
  return read_body([&](char* b, int n) { return c.read(b, n); }, [&] { return c.clock; }, d, cap, contentLength,
                   scratch.data(), chunk, body);
}

}  // namespace

TEST_CASE("request deadline: remaining time, expiry and the timeout of one step") {
  RequestDeadline d(1000, 8000);
  CHECK(d.deadline_ms() == 9000);
  CHECK(d.remaining_ms(1000) == 8000);
  CHECK(d.remaining_ms(8500) == 500);
  CHECK(d.remaining_ms(9000) == 0);
  CHECK(d.remaining_ms(20000) == 0);
  CHECK_FALSE(d.expired(8999));
  CHECK(d.expired(9000));
  CHECK(d.step_timeout_ms(1000, 1000) == 1000);  // capped by the step
  CHECK(d.step_timeout_ms(8700, 1000) == 300);   // capped by what is left
  CHECK(d.step_timeout_ms(9000, 1000) == 0);     // nothing left: do not start the step
  CHECK(d.step_timeout_ms(8999, 1000) == 1);
  // A clock that goes backwards never gives more than the whole budget.
  CHECK(d.remaining_ms(0) == 8000);
  // Nonsense budgets: nothing is allowed.
  RequestDeadline none(0, 0);
  CHECK(none.expired(0));
  RequestDeadline negative(0, -5);
  CHECK(negative.expired(0));
  CHECK(negative.remaining_ms(0) == 0);
}

TEST_CASE("read_body: a whole body within the time") {
  FakeClient c;
  c.script = {{1024, 10}, {1024, 10}, {500, 10}, {0, 1}};
  std::string body;
  CHECK(run(c, 8000, 1 << 20, 2548, body) == BodyResult::Complete);
  CHECK(body.size() == 2548);
  // Content-Length reached: no extra read needed for the end.
  FakeClient c2;
  c2.script = {{100, 1}};
  std::string b2;
  CHECK(run(c2, 8000, 1 << 20, 100, b2) == BodyResult::Complete);
  CHECK(c2.calls == 1);
  // Without a length (chunked): until the client says the end.
  FakeClient c3;
  c3.script = {{300, 1}, {300, 1}, {0, 1}};
  std::string b3;
  CHECK(run(c3, 8000, 1 << 20, 0, b3) == BodyResult::Complete);
  CHECK(b3.size() == 600);
}

TEST_CASE("read_body: a trickling peer is cut off at the total deadline, not later") {
  // One byte every 900 ms, forever: each read is short, but together they would run for ever.
  FakeClient c;
  for (int i = 0; i < 1000; ++i) c.script.push_back({1, 900});
  std::string body;
  CHECK(run(c, 8000, 1 << 20, 4000, body) == BodyResult::TimedOut);
  // Stopped at the first check past 8000 ms: at most one read started before the deadline ends after it.
  CHECK(c.clock <= 8000 + 900);
  CHECK(c.calls <= 10);
  CHECK(body.size() < 10);
}

TEST_CASE("read_body: a stall that ends with an error after the deadline is a timeout; before it, an error") {
  FakeClient c;
  c.script = {{200, 5}, {-1, 9000}};  // the read blocks past the deadline, then fails (socket shut down)
  std::string body;
  CHECK(run(c, 8000, 1 << 20, 4000, body) == BodyResult::TimedOut);
  FakeClient c2;
  c2.script = {{200, 5}, {-1, 5}};  // a real error early
  std::string b2;
  CHECK(run(c2, 8000, 1 << 20, 4000, b2) == BodyResult::ReadError);
}

TEST_CASE("read_body: no read starts once the time is used up") {
  FakeClient c;
  c.clock = 0;
  c.script = {{10, 1}};
  RequestDeadline d(-8000, 8000);  // already expired at clock 0
  std::vector<char> scratch(64);
  std::string body;
  CHECK(read_body([&](char* b, int n) { return c.read(b, n); }, [&] { return c.clock; }, d, 1 << 20, 0,
                  scratch.data(), 64, body) == BodyResult::TimedOut);
  CHECK(c.calls == 0);
}

TEST_CASE("read_body: the size cap, before and while reading") {
  FakeClient c;
  std::string body;
  CHECK(run(c, 8000, 1000, 1001, body) == BodyResult::OverCap);  // the announced length is too big
  CHECK(c.calls == 0);
  FakeClient c2;
  c2.script = {{600, 1}, {600, 1}, {0, 1}};
  std::string b2;
  CHECK(run(c2, 8000, 1000, 0, b2) == BodyResult::OverCap);  // no length, grows past the cap
  CHECK(b2.size() <= 1000);
  FakeClient c3;
  c3.script = {{1000, 1}, {0, 1}};
  std::string b3;
  CHECK(run(c3, 8000, 1000, 0, b3) == BodyResult::Complete);  // exactly the cap is fine
}

TEST_CASE("read_body: a body shorter than its Content-Length is cut short") {
  FakeClient c;
  c.script = {{100, 1}, {0, 1}};
  std::string body;
  CHECK(run(c, 8000, 1 << 20, 500, body) == BodyResult::CutShort);
}

TEST_CASE("read_body: reads are asked in chunks no larger than the chunk size") {
  FakeClient c;
  c.script = {{5000, 1}, {5000, 1}, {0, 1}};  // the fake would hand more if asked
  std::string body;
  CHECK(run(c, 8000, 1 << 20, 0, body, 512) == BodyResult::Complete);
  CHECK(c.maxAsked == 512);
  CHECK(body.size() == 1024);
}

TEST_CASE("read_body: \"nothing yet\" steps are retried while time is left, then it is a timeout") {
  FakeClient c;
  c.script = {{kBodyReadAgain, 1000}, {kBodyReadAgain, 1000}, {300, 5}, {0, 1}};
  std::string body;
  CHECK(run(c, 8000, 1 << 20, 0, body) == BodyResult::Complete);
  CHECK(body.size() == 300);
  FakeClient stalled;
  for (int i = 0; i < 100; ++i) stalled.script.push_back({kBodyReadAgain, 1000});
  std::string b2;
  CHECK(run(stalled, 8000, 1 << 20, 0, b2) == BodyResult::TimedOut);
  CHECK(stalled.clock == 8000);
  CHECK(stalled.calls == 8);
}
