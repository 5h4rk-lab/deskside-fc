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
// - get_prepared(): the same, plus a `prepare` step that turns the finished
//   result into something ready to use (the parsed body), delivered in
//   HttpResult::prepared. A transport may run it on its worker thread, so the
//   slow part of an answer (a 30 KB JSON parse: ~50 ms on the board) is not
//   paid on the UI thread; the default runs it on the UI thread just before
//   `done`. `prepare` must be pure: it reads the result and nothing else
//   shared (no UI, no Store, no `this` of a UI object), and may be empty.
#pragma once

#include <functional>
#include <memory>
#include <string>

struct HttpResult {
  int status = 0;
  std::string body;  // may be emptied by a transport once `prepared` holds it parsed
  bool transportError = false;
  std::shared_ptr<void> prepared;  // get_prepared(): what `prepare` returned (may be null)
};

using HttpPrepare = std::function<std::shared_ptr<void>(const HttpResult&)>;

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual void get(const std::string& url, const std::string& token, int timeoutMs,
                   std::function<void(HttpResult)> done) = 0;

  virtual void get_prepared(const std::string& url, const std::string& token, int timeoutMs, HttpPrepare prepare,
                            std::function<void(HttpResult)> done) {
    get(url, token, timeoutMs, [prepare = std::move(prepare), done = std::move(done)](HttpResult r) {
      if (prepare) r.prepared = prepare(r);
      done(std::move(r));
    });
  }
};
