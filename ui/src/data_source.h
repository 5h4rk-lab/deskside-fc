// Where the UI gets its data. Implemented by the simulator (curl against the
// helper, or MockSource) and by the firmware (HTTP over WiFi).
//
// Callbacks may be invoked synchronously, before the fetch_* call returns (the
// simulator's MockSource does), or later from the caller's main loop (the HTTP
// source will). Callers must therefore not assume either ordering, and must not
// hold references into their own state across the call that a synchronous
// callback could invalidate.
//
// On failure the callback receives ok=false and an empty/default value.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "model.h"

class DataSource {
 public:
  virtual ~DataSource() = default;

  virtual void fetch_matches(std::function<void(bool ok, std::vector<MatchSummary>)> cb) = 0;
  virtual void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) = 0;
  virtual void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) = 0;
  virtual void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) = 0;
};
