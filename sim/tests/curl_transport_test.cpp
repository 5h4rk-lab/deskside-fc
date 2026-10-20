// CurlTransport against a tiny HTTP server on 127.0.0.1 (random port) run by
// the test itself. Never touches the real network.
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "curl_transport.h"

namespace {

using Clock = std::chrono::steady_clock;

// Serves one connection at a time from a helper thread. Paths:
//   /ok      200 with a small JSON body
//   /401     401
//   /500     500
//   /slow    waits 600 ms, then 200 (longer than the tests' short timeout)
//   /big     200 with a 5 MB body (over the transport's 4 MB cap)
//   /redir   302 to /ok (must not be followed)
class TinyServer {
 public:
  TinyServer() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // any free port
    REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(fd_, 8) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~TinyServer() {
    stop_ = true;
    thread_.join();
    ::close(fd_);
  }

  std::string url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
  }

  // Raw request heads received so far.
  std::vector<std::string> heads() {
    std::lock_guard<std::mutex> lock(mu_);
    return heads_;
  }

 private:
  void run() {
    while (!stop_) {
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      const int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      serve(c);
      ::close(c);
    }
  }

  // Waits up to `ms` (in small steps, so shutdown is quick). False when stopping.
  bool pause(int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
      if (stop_) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

  void serve(int c) {
    std::string head;
    char buf[1024];
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (head.find("\r\n\r\n") == std::string::npos && Clock::now() < deadline) {
      pollfd p{c, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
      if (n <= 0) return;
      head.append(buf, static_cast<size_t>(n));
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      heads_.push_back(head);
    }
    const size_t sp1 = head.find(' ');
    const size_t sp2 = head.find(' ', sp1 + 1);
    const std::string path = sp1 == std::string::npos ? "" : head.substr(sp1 + 1, sp2 - sp1 - 1);

    if (path == "/ok") {
      reply(c, "200 OK", "{\"teams\":[]}");
    } else if (path == "/401") {
      reply(c, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
    } else if (path == "/500") {
      reply(c, "500 Internal Server Error", "");
    } else if (path == "/slow") {
      if (pause(600)) reply(c, "200 OK", "{}");
    } else if (path == "/big") {
      reply(c, "200 OK", std::string(5 * 1024 * 1024, 'x'));
    } else if (path == "/redir") {
      const std::string r = "HTTP/1.1 302 Found\r\nLocation: /ok\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      send_all(c, r);
    } else {
      reply(c, "404 Not Found", "");
    }
  }

  static void send_all(int c, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(c, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) return;  // the client gave up (size cap): fine
      sent += static_cast<size_t>(n);
    }
  }

  static void reply(int c, const std::string& status, const std::string& body) {
    send_all(c, "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
  }

  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mu_;
  std::vector<std::string> heads_;
};

// A port nothing listens on: bound once, then released.
int closed_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  ::close(fd);
  return ntohs(addr.sin_port);
}

// Issues one GET and polls until its answer arrives (or 3 s pass).
std::optional<HttpResult> fetch(CurlTransport& t, const std::string& url, int timeoutMs = 2000,
                                const std::string& token = "tok") {
  std::optional<HttpResult> got;
  int calls = 0;
  const std::thread::id mainThread = std::this_thread::get_id();
  bool onMainThread = true;
  t.get(url, token, timeoutMs, [&](HttpResult r) {
    ++calls;
    onMainThread = std::this_thread::get_id() == mainThread;
    got = std::move(r);
  });
  CHECK_FALSE(got.has_value());  // never from inside get()
  const auto end = Clock::now() + std::chrono::seconds(3);
  while (!got && Clock::now() < end) {
    t.poll();
    if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  t.poll();
  CHECK(calls <= 1);
  CHECK(onMainThread);
  return got;
}

}  // namespace

TEST_CASE("curl transport: 200 with the body, token and headers sent") {
  TinyServer server;
  CurlTransport t;
  const auto r = fetch(t, server.url("/ok"), 2000, "my-token");
  REQUIRE(r.has_value());
  CHECK(r->status == 200);
  CHECK_FALSE(r->transportError);
  CHECK(r->body == "{\"teams\":[]}");
  const std::vector<std::string> heads = server.heads();
  REQUIRE(heads.size() == 1);
  const std::string& h = heads[0];
  CHECK(h.rfind("GET /ok HTTP/1.1\r\n", 0) == 0);
  CHECK(h.find("\r\nx-token: my-token\r\n") != std::string::npos);
  CHECK(h.find("\r\nAccept: application/json\r\n") != std::string::npos);
  CHECK(h.find("\r\nUser-Agent: soccer-display-sim/0.1\r\n") != std::string::npos);
}

TEST_CASE("curl transport: error statuses are results, not transport errors") {
  TinyServer server;
  CurlTransport t;
  auto r = fetch(t, server.url("/401"));
  REQUIRE(r.has_value());
  CHECK(r->status == 401);
  CHECK_FALSE(r->transportError);
  CHECK(r->body == "{\"error\":\"unauthorized\"}");
  r = fetch(t, server.url("/500"));
  REQUIRE(r.has_value());
  CHECK(r->status == 500);
  CHECK_FALSE(r->transportError);
  r = fetch(t, server.url("/redir"));  // redirects are not followed
  REQUIRE(r.has_value());
  CHECK(r->status == 302);
  CHECK(server.heads().size() == 3);
}

TEST_CASE("curl transport: a response slower than the timeout is a transport error") {
  TinyServer server;
  CurlTransport t;
  const auto start = Clock::now();
  const auto r = fetch(t, server.url("/slow"), 150);
  REQUIRE(r.has_value());
  CHECK(r->transportError);
  CHECK(r->status == 0);
  CHECK(r->body.empty());
  CHECK(Clock::now() - start < std::chrono::milliseconds(550));
}

TEST_CASE("curl transport: connection refused is a transport error") {
  CurlTransport t;
  const auto r = fetch(t, "http://127.0.0.1:" + std::to_string(closed_port()) + "/ok", 1000);
  REQUIRE(r.has_value());
  CHECK(r->transportError);
  CHECK(r->status == 0);
}

TEST_CASE("curl transport: a body over the size cap is dropped as a transport error") {
  TinyServer server;
  CurlTransport t;
  CHECK(CurlTransport::kMaxBodyBytes == 4 * 1024 * 1024);
  const auto r = fetch(t, server.url("/big"));
  REQUIRE(r.has_value());
  CHECK(r->transportError);
  CHECK(r->status == 0);
  CHECK(r->body.empty());
}

TEST_CASE("curl transport: only http and https URLs are fetched") {
  CurlTransport t;
  const auto r = fetch(t, "file:///etc/hostname", 500);
  REQUIRE(r.has_value());
  CHECK(r->transportError);
  CHECK(r->body.empty());
}

TEST_CASE("curl transport: requests are answered in order, one worker") {
  TinyServer server;
  CurlTransport t;
  std::vector<int> statuses;
  t.get(server.url("/ok"), "t", 2000, [&](HttpResult r) { statuses.push_back(r.status); });
  t.get(server.url("/401"), "t", 2000, [&](HttpResult r) { statuses.push_back(r.status); });
  t.get(server.url("/500"), "t", 2000, [&](HttpResult r) { statuses.push_back(r.status); });
  const auto end = Clock::now() + std::chrono::seconds(3);
  while (statuses.size() < 3 && Clock::now() < end) {
    t.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(statuses == std::vector<int>{200, 401, 500});
}

TEST_CASE("curl transport: destroying it with requests outstanding is quick and calls nothing") {
  TinyServer server;
  int calls = 0;
  const auto start = Clock::now();
  {
    CurlTransport t;
    t.get(server.url("/slow"), "t", 5000, [&](HttpResult) { ++calls; });
    t.get(server.url("/ok"), "t", 5000, [&](HttpResult) { ++calls; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the first is in flight
  }
  CHECK(calls == 0);
  CHECK(Clock::now() - start < std::chrono::milliseconds(500));
}

TEST_CASE("curl transport: several workers answer every request exactly once, on the polling thread") {
  TinyServer server;
  for (unsigned workers : {2u, 0u, 100u}) {  // 0 and 100 are clamped
    CurlTransport t(workers);
    std::vector<int> statuses;
    bool onMain = true;
    const std::thread::id mainThread = std::this_thread::get_id();
    for (int i = 0; i < 6; ++i) {
      t.get(server.url(i % 2 ? "/ok" : "/401"), "t", 2000, [&](HttpResult r) {
        onMain = onMain && std::this_thread::get_id() == mainThread;
        statuses.push_back(r.status);
      });
    }
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (statuses.size() < 6 && Clock::now() < end) {
      t.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    t.poll();
    CHECK(statuses.size() == 6);
    CHECK(onMain);
    int ok = 0, denied = 0;
    for (int s : statuses) {
      if (s == 200) ++ok;
      if (s == 401) ++denied;
    }
    CHECK(ok == 3);
    CHECK(denied == 3);
  }
}

TEST_CASE("curl transport: destroying a multi-worker transport with requests outstanding calls nothing") {
  TinyServer server;
  int calls = 0;
  const auto start = Clock::now();
  {
    CurlTransport t(2);
    for (int i = 0; i < 4; ++i) t.get(server.url("/slow"), "t", 5000, [&](HttpResult) { ++calls; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(calls == 0);
  CHECK(Clock::now() - start < std::chrono::milliseconds(500));
}
