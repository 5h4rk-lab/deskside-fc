// HttpTransport for the simulator: libcurl easy handles on one worker thread.
// Requests run one at a time in the order they were made; their results wait
// in a completed queue until poll() delivers them on the calling (UI) thread.
// See http_transport.h for the contract.
//
// - Connect timeout 5 s, whole-request timeout from get(); no redirects are
//   followed; TLS peers and host names are verified; http and https only.
// - Headers: x-token (never logged or printed), Accept: application/json,
//   User-Agent: soccer-display-sim/0.1.
// - Bodies over kMaxBodyBytes are aborted and reported as transport errors.
// - The destructor aborts the request in flight, drops the queued and the
//   undelivered ones (their callbacks are never called) and joins the worker.
#pragma once

#include <stddef.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "http_transport.h"

class CurlTransport : public HttpTransport {
 public:
  static constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;
  static constexpr long kConnectTimeoutMs = 5000;

  CurlTransport();
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
  std::thread worker_;
};
