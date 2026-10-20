// HttpTransport for the simulator: libcurl easy handles on worker threads (one
// by default). Each worker runs one request at a time; requests start in the
// order they were made, and with several workers they may finish in any order.
// Results wait in a completed queue until poll() delivers them on the calling
// (UI) thread. See http_transport.h for the contract.
//
// - Connect timeout 5 s, whole-request timeout from get(); no redirects are
//   followed; TLS peers and host names are verified; http and https only.
// - Headers: x-token (never logged or printed), Accept: application/json,
//   User-Agent: soccer-display-sim/0.1.
// - Bodies over kMaxBodyBytes are aborted and reported as transport errors.
// - The destructor aborts the request in flight, drops the queued and the
//   undelivered ones (their callbacks are never called) and joins the workers.
#pragma once

#include <stddef.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "http_transport.h"

class CurlTransport : public HttpTransport {
 public:
  static constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;
  static constexpr long kConnectTimeoutMs = 5000;

  static constexpr unsigned kMaxWorkers = 8;

  // `workers` (clamped to 1..kMaxWorkers) is how many requests run at once.
  explicit CurlTransport(unsigned workers = 1);
  ~CurlTransport() override;
  CurlTransport(const CurlTransport&) = delete;
  CurlTransport& operator=(const CurlTransport&) = delete;

  void get(const std::string& url, const std::string& token, int timeoutMs,
           std::function<void(HttpResult)> done) override;

  // Delivers the finished requests' results (call it every loop iteration).
  void poll();

 private:
  struct Job {
    std::string url;
    std::string token;
    int timeoutMs = 0;
    std::function<void(HttpResult)> done;
  };
  struct Done {
    std::function<void(HttpResult)> done;
    HttpResult result;
  };

  void run();
  HttpResult perform(const Job& job);

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::deque<Done> completed_;
  std::atomic<bool> stop_{false};
  std::vector<std::thread> workers_;
};
