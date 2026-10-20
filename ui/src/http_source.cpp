#include "http_source.h"

#include <stdint.h>

#include <cstddef>
#include <utility>

#include "format.h"
#include "json_parse.h"

namespace {
constexpr const char* kFallbackLeague = "eng.1";
}  // namespace

HttpSource::HttpSource(HttpTransport& transport, std::string baseUrl, std::string token,
                       std::vector<std::string> leagues, std::string newsLeague)
    : transport_(transport), base_(std::move(baseUrl)), token_(std::move(token)),
      newsLeague_(std::move(newsLeague)) {
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
  for (const std::string& l : leagues) {
    if (!leaguesParam_.empty()) leaguesParam_ += ',';
    leaguesParam_ += url_encode(l);
  }
}

HttpSource::~HttpSource() { *alive_ = false; }

bool HttpSource::accept(const HttpResult& r) {
  if (r.transportError) {
    lastError_ = FetchError::Offline;
  } else if (r.status == 401) {
    lastError_ = FetchError::Auth;
  } else if (r.status != 200) {
    lastError_ = FetchError::Server;
  } else {
    return true;
  }
  return false;
}

void HttpSource::fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) {
  enqueue(Kind::Matches, base_ + "/api/matches?leagues=" + leaguesParam_,
          [this, cb = std::move(cb)](const HttpResult& r) {
            std::vector<MatchSummary> matches;
            int64_t updated = 0;
            if (!accept(r)) return cb(false, {});
            if (!parse_matches(r.body, matches, updated)) {
              lastError_ = FetchError::Parse;
              return cb(false, {});
            }
            lastError_ = FetchError::None;
            leagueOf_.clear();
            for (const MatchSummary& m : matches) leagueOf_[m.id] = m.league;
            cb(true, std::move(matches));
          });
}

void HttpSource::fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) {
  // The URL is made when the request is sent: a matches result still in the
  // queue may teach the league first.
  enqueue(Kind::Detail, id, [this, cb = std::move(cb)](const HttpResult& r) {
    MatchDetail detail;
    if (!accept(r)) return cb(false, MatchDetail{});
    if (!parse_detail(r.body, detail)) {
      lastError_ = FetchError::Parse;
      return cb(false, MatchDetail{});
    }
    lastError_ = FetchError::None;
    if (!detail.match.league.empty()) leagueOf_[detail.match.id] = detail.match.league;
    cb(true, std::move(detail));
  });
}

std::string HttpSource::detail_url(const std::string& id) const {
  auto it = leagueOf_.find(id);
  const std::string league = it != leagueOf_.end() && !it->second.empty() ? it->second : kFallbackLeague;
  return base_ + "/api/match/" + url_encode(id) + "?league=" + url_encode(league);
}

void HttpSource::fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) {
  enqueue(Kind::News, base_ + "/api/news?league=" + url_encode(newsLeague_),
          [this, cb = std::move(cb)](const HttpResult& r) {
            std::vector<NewsItem> news;
            int64_t updated = 0;
            if (!accept(r)) return cb(false, {});
            if (!parse_news(r.body, news, updated)) {
              lastError_ = FetchError::Parse;
              return cb(false, {});
            }
            lastError_ = FetchError::None;
            cb(true, std::move(news));
          });
}

void HttpSource::fetch_teams(std::function<void(bool, std::vector<Team>)> cb) {
  enqueue(Kind::Teams, base_ + "/api/teams", [this, cb = std::move(cb)](const HttpResult& r) {
    std::vector<Team> teams;
    if (!accept(r)) return cb(false, {});
    if (!parse_teams(r.body, teams)) {
      lastError_ = FetchError::Parse;
      return cb(false, {});
    }
    lastError_ = FetchError::None;
    cb(true, std::move(teams));
  });
}

void HttpSource::enqueue(Kind kind, std::string target, std::function<void(const HttpResult&)> handle) {
  queue_.push_back({kind, std::move(target), std::move(handle)});
  trim_queue();
  pump();
}

// Folds the oldest waiting news/teams request that has a newer duplicate into
// that duplicate, until the queue is within bounds or nothing can be folded.
void HttpSource::trim_queue() {
  while (queue_.size() > kMaxQueued) {
    bool folded = false;
    for (size_t i = 0; i < queue_.size() && !folded; ++i) {
      if (queue_[i].kind != Kind::News && queue_[i].kind != Kind::Teams) continue;
      for (size_t j = i + 1; j < queue_.size(); ++j) {
        if (queue_[j].kind != queue_[i].kind || queue_[j].target != queue_[i].target) continue;
        std::function<void(const HttpResult&)> older = std::move(queue_[i].handle);
        std::function<void(const HttpResult&)> newer = std::move(queue_[j].handle);
        queue_[j].handle = [older = std::move(older), newer = std::move(newer)](const HttpResult& r) {
          older(r);
          newer(r);
        };
        queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(i));
        folded = true;
        break;
      }
    }
    if (!folded) return;
  }
}

void HttpSource::pump() {
  if (busy_ || queue_.empty()) return;
  busy_ = true;
  Request req = std::move(queue_.front());
  queue_.pop_front();
  std::shared_ptr<bool> alive = alive_;
  std::function<void(const HttpResult&)> handle = std::move(req.handle);
  const std::string url = req.kind == Kind::Detail ? detail_url(req.target) : req.target;
  transport_.get(url, token_, kHttpTimeoutMs,
                 [this, alive, handle = std::move(handle)](HttpResult r) {
                   if (!*alive) return;
                   busy_ = false;
                   handle(r);  // may queue more requests (and start one)
                   if (*alive) pump();
                 });
}
