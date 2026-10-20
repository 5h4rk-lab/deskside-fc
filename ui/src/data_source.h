// Where the UI gets its data. Implemented by the simulator (curl against the
// helper, or MockSource) and by the firmware (HTTP over WiFi).
//
// Callbacks may be invoked synchronously, before the fetch_* call returns (the
// simulator's MockSource does), or later from the caller's main loop (the HTTP
// source will). Callers must therefore not assume either ordering, and must not
// hold references into their own state across the call that a synchronous
// callback could invalidate.
//
// On failure the callback receives ok=false and an empty/default value, and
// last_error() says why until the next request completes (it is already set
// when the callback runs).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "model.h"

// Why the last request failed. Offline: the helper could not be reached
// (timeout, DNS, connection). Auth: it refused the token (401). Server: any
// other non-200 answer (400, 502...). Parse: a 200 whose body is unreadable.
enum class FetchError { None, Offline, Auth, Server, Parse };

class DataSource {
 public:
  virtual ~DataSource() = default;

  virtual void fetch_matches(std::function<void(bool ok, std::vector<MatchSummary>)> cb) = 0;
  virtual void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) = 0;
  virtual void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) = 0;
  virtual void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) = 0;

  // The outcome of the most recently completed request (None after a success).
  virtual FetchError last_error() const { return FetchError::None; }
};
