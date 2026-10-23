// The real DataSource: the helper's JSON API over an HttpTransport (libcurl in
// the simulator, the ESP32 HTTP client in the firmware). Shared with the
// firmware: no stdio, no threads, no clock reads.
//
// - GET <base>/api/matches?leagues=<l1,l2,...>, /api/match/<id>?league=<code>,
//   /api/news?league=<newsLeague> and /api/teams, each with the token in the
//   x-token header and a kHttpTimeoutMs timeout.
// - Success is a 200 whose body parses with the matching json_parse.h parser
//   (through HttpTransport::get_prepared: the firmware parses on its HTTP
//   worker, off the UI task; results reach the callbacks on the UI thread);
//   anything else calls back with ok=false and empty data, and last_error()
//   says why (Auth for 401, Offline for transport errors, Server for other
//   statuses, Parse for an unreadable 200).
// - The detail endpoint needs the match's league: it is remembered per match
//   id from the last successful matches result and from detail results, and
//   looked up when the request is sent; unknown ids try "eng.1".
// - Requests run one at a time, first in first out; callbacks run from the
//   transport's delivery (never inside a fetch_* call). While more than
//   kMaxQueued requests wait, the oldest waiting news or teams request that
//   has a newer duplicate is folded into it: both callers get that answer.
//   Matches and detail requests are never dropped.
// - The token is never logged or exposed.
#pragma once

#include <stddef.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "data_source.h"
#include "http_transport.h"
#include "model.h"

constexpr int kHttpTimeoutMs = 8000;

class HttpSource : public DataSource {
 public:
  static constexpr size_t kMaxQueued = 8;

  // `transport` must outlive this source. A trailing '/' of baseUrl is ignored.
  HttpSource(HttpTransport& transport, std::string baseUrl, std::string token,
             std::vector<std::string> leagues = {"eng.1", "esp.1", "ita.1", "ger.1", "fra.1"},
             std::string newsLeague = "eng.1");
  ~HttpSource() override;

  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override;
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override;
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override;
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override;
  FetchError last_error() const override { return lastError_; }

  // Requests waiting behind the one in flight.
  size_t queued() const { return queue_.size(); }

 private:
  enum class Kind { Matches, Detail, News, Teams };
  struct Request {
    Kind kind;
    std::string target;  // the URL, or the match id of a detail request
    HttpPrepare prepare;  // parses a 200's body (pure: may run on the transport's worker)
    std::function<void(const HttpResult&)> handle;
  };

  void enqueue(Kind kind, std::string target, HttpPrepare prepare, std::function<void(const HttpResult&)> handle);
  std::string detail_url(const std::string& id) const;
  void trim_queue();
  void pump();
  // Classifies a result: true for a 200 (the caller still has to parse it).
  bool accept(const HttpResult& r);

  HttpTransport& transport_;
  std::string base_;
  std::string token_;
  std::string leaguesParam_;
  std::string newsLeague_;
  std::map<std::string, std::string> leagueOf_;  // match id -> league code
  std::deque<Request> queue_;
  bool busy_ = false;
  FetchError lastError_ = FetchError::None;
  // Cleared on destruction, for answers delivered after it.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
