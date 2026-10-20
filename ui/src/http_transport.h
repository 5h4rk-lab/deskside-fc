// The one thing HttpSource needs from the network: an HTTP GET. The simulator
// implements it with libcurl on a worker thread (sim/curl_transport.h), the
// firmware with the ESP32 HTTP client. Shared with the firmware: no stdio, no
// threads, no clock reads here.
//
// Contract:
// - get() never blocks and never calls `done` itself: `done` is called exactly
//   once, later, on the UI thread, from the owner's polling hook (poll() of the
//   concrete transport, or the firmware's task glue). Never from another
//   thread and never from inside get().
// - A response of any HTTP status is a result (`transportError` false,
//   `status` the code, `body` the payload, possibly empty). Timeouts, DNS and
//   connection failures, and bodies over the transport's size cap give
//   `status` 0, `transportError` true and an empty body.
// - `token` is sent as the `x-token` header. Implementations must never log it.
// - `timeoutMs` bounds the whole request.
// - Several requests may be outstanding; callers that need ordering serialise
//   them (HttpSource runs one at a time).
// - A transport destroyed with requests outstanding drops them: their `done`
//   is never called.
#pragma once

#include <functional>
#include <string>

struct HttpResult {
  int status = 0;
  std::string body;
  bool transportError = false;
};

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual void get(const std::string& url, const std::string& token, int timeoutMs,
                   std::function<void(HttpResult)> done) = 0;
};
