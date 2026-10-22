// One total time budget per HTTP request, and the body-reading loop that
// enforces it. Pure (the clock and the client's read are passed in), shared
// with the simulator's tests; the firmware's EspHttpTransport runs
// read_body() over esp_http_client_read().
//
// The budget starts when the request starts. Every blocking step (connect,
// headers, each read) gets at most the time that is left (step_timeout_ms),
// no step starts once it is used up, and the body is read in small chunks so
// the deadline is checked often. A step that blocks anyway (a peer that
// trickles inside one read) is ended from outside by the transport at the
// deadline (it shuts the socket down), which turns into a read error after
// the deadline: read_body() reports that as TimedOut.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>

class RequestDeadline {
 public:
  // `totalMs` <= 0 allows nothing.
  RequestDeadline(int64_t startMs, int totalMs)
      : start_(startMs), deadline_(startMs + (totalMs > 0 ? totalMs : 0)), total_(totalMs > 0 ? totalMs : 0) {}

  int64_t deadline_ms() const { return deadline_; }
  // Time left, 0 when used up; never more than the whole budget (a clock that
  // went backwards).
  int remaining_ms(int64_t nowMs) const;
  bool expired(int64_t nowMs) const { return remaining_ms(nowMs) <= 0; }
  // Timeout for the next blocking step: the time left, at most `maxStepMs`;
  // 0 means "do not start it".
  int step_timeout_ms(int64_t nowMs, int maxStepMs) const;

 private:
  int64_t start_;
  int64_t deadline_;
  int total_;
};

enum class BodyResult { Complete, TimedOut, ReadError, OverCap, CutShort };

// What `read` returns when one bounded step ended with nothing read yet (a
// client timeout shorter than the deadline): read_body() tries again while
// time is left.
constexpr int kBodyReadAgain = -1000000;

// Reads a response body into `body`: `read(buf, len)` returns the bytes read
// (> 0), 0 at the end of the body, kBodyReadAgain for "nothing yet", any other
// value < 0 on an error; it is never asked for more
// than `chunkBytes`. Before every read the deadline is checked. A body over
// `capBytes` (announced by `contentLength` > 0, or grown while reading) stops
// with OverCap; a body that ends before `contentLength` is CutShort; with a
// known length reading stops as soon as it is complete. An error after the
// deadline counts as TimedOut.
BodyResult read_body(const std::function<int(char* buf, int len)>& read, const std::function<int64_t()>& nowMs,
                     const RequestDeadline& deadline, size_t capBytes, int64_t contentLength, char* scratch,
                     int chunkBytes, std::string& body);
