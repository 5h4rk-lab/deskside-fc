// Test double for HttpTransport: records every request and answers them only
// when the test says so (flush() / deliver_next()), with scripted responses in
// order. Never touches the network.
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "http_transport.h"

class FakeTransport : public HttpTransport {
 public:
  struct Request {
    std::string url;
    std::string token;
    int timeoutMs = 0;
  };

  void get(const std::string& url, const std::string& token, int timeoutMs,
           std::function<void(HttpResult)> done) override {
    requests.push_back({url, token, timeoutMs});
    pending_.push_back(std::move(done));
  }

  // Scripts the answer to a future request (answers are used in order).
  void respond(int status, std::string body) { script_.push_back({status, std::move(body), false}); }
  void respond_transport_error() { script_.push_back({0, "", true}); }

  // Requests made and not answered yet.
  size_t pending() const { return pending_.size(); }

  // Answers the oldest pending request with the next scripted answer. False
  // when nothing is pending or nothing is scripted.
  bool deliver_next() {
    if (pending_.empty() || script_.empty()) return false;
    std::function<void(HttpResult)> done = std::move(pending_.front());
    pending_.pop_front();
    HttpResult r = std::move(script_.front());
    script_.pop_front();
    done(std::move(r));  // may issue new requests
    return true;
  }

  // Answers the pending request at `index` (0 = oldest) with the given result,
  // ignoring the script. False when there is no such request.
  bool deliver_at(size_t index, int status, std::string body) {
    if (index >= pending_.size()) return false;
    HttpResult r;
    r.status = status;
    r.body = std::move(body);
    return complete_at(index, std::move(r));
  }
  bool deliver_error_at(size_t index) {
    HttpResult r;
    r.transportError = true;
    return complete_at(index, std::move(r));
  }

  // Answers pending requests (including ones made by the callbacks) while
  // there are scripted answers left. Returns how many were answered.
  int flush() {
    int n = 0;
    while (n < 1000 && deliver_next()) ++n;
    return n;
  }

  std::vector<Request> requests;

 private:
  bool complete_at(size_t index, HttpResult r) {
    if (index >= pending_.size()) return false;
    std::function<void(HttpResult)> done = std::move(pending_[index]);
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
    done(std::move(r));  // may issue new requests
    return true;
  }

  std::deque<std::function<void(HttpResult)>> pending_;
  std::deque<HttpResult> script_;
};
