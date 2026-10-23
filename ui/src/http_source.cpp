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

namespace {

// What the prepare step makes of a 200 (on the transport's worker in the
// firmware). `ok` false: the body did not parse.
struct ParsedMatches {
  bool ok = false;
  std::vector<MatchSummary> matches;
};
struct ParsedDetail {
  bool ok = false;
  MatchDetail detail;
};
struct ParsedNews {
  bool ok = false;
  std::vector<NewsItem> news;
};
struct ParsedTeams {
  bool ok = false;
  std::vector<Team> teams;
};

bool is_ok(const HttpResult& r) { return !r.transportError && r.status == 200; }

std::shared_ptr<void> prepare_matches(const HttpResult& r) {
  if (!is_ok(r)) return nullptr;
  auto p = std::make_shared<ParsedMatches>();
  int64_t updated = 0;
  p->ok = parse_matches(r.body, p->matches, updated);
  return p;
}

std::shared_ptr<void> prepare_detail(const HttpResult& r) {
  if (!is_ok(r)) return nullptr;
  auto p = std::make_shared<ParsedDetail>();
  p->ok = parse_detail(r.body, p->detail);
  return p;
}

std::shared_ptr<void> prepare_news(const HttpResult& r) {
  if (!is_ok(r)) return nullptr;
  auto p = std::make_shared<ParsedNews>();
  int64_t updated = 0;
  p->ok = parse_news(r.body, p->news, updated);
  return p;
}

std::shared_ptr<void> prepare_teams(const HttpResult& r) {
  if (!is_ok(r)) return nullptr;
  auto p = std::make_shared<ParsedTeams>();
  p->ok = parse_teams(r.body, p->teams);
  return p;
}

// The prepared result of an accepted 200; parsed here when the transport did
// not (a transport may drop `prepared` only by not supporting it: never null
// for a 200 otherwise).
template <typename T>
std::shared_ptr<T> prepared(const HttpResult& r, std::shared_ptr<void> (*prepare)(const HttpResult&)) {
  std::shared_ptr<void> p = r.prepared ? r.prepared : prepare(r);
  return std::static_pointer_cast<T>(p);
}

}  // namespace

void HttpSource::fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) {
  enqueue(Kind::Matches, base_ + "/api/matches?leagues=" + leaguesParam_, prepare_matches,
          [this, cb = std::move(cb)](const HttpResult& r) {
            if (!accept(r)) return cb(false, {});
            const std::shared_ptr<ParsedMatches> p = prepared<ParsedMatches>(r, prepare_matches);
            if (!p || !p->ok) {
              lastError_ = FetchError::Parse;
              return cb(false, {});
            }
            lastError_ = FetchError::None;
            leagueOf_.clear();
            for (const MatchSummary& m : p->matches) leagueOf_[m.id] = m.league;
            cb(true, std::move(p->matches));  // matches requests are never folded: one reader
          });
}

void HttpSource::fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) {
  // The URL is made when the request is sent: a matches result still in the
  // queue may teach the league first.
  enqueue(Kind::Detail, id, prepare_detail, [this, cb = std::move(cb)](const HttpResult& r) {
    if (!accept(r)) return cb(false, MatchDetail{});
    const std::shared_ptr<ParsedDetail> p = prepared<ParsedDetail>(r, prepare_detail);
    if (!p || !p->ok) {
      lastError_ = FetchError::Parse;
      return cb(false, MatchDetail{});
    }
    lastError_ = FetchError::None;
    if (!p->detail.match.league.empty()) leagueOf_[p->detail.match.id] = p->detail.match.league;
    cb(true, std::move(p->detail));  // detail requests are never folded: one reader
  });
}

std::string HttpSource::detail_url(const std::string& id) const {
  auto it = leagueOf_.find(id);
  const std::string league = it != leagueOf_.end() && !it->second.empty() ? it->second : kFallbackLeague;
  return base_ + "/api/match/" + url_encode(id) + "?league=" + url_encode(league);
}

void HttpSource::fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) {
  enqueue(Kind::News, base_ + "/api/news?league=" + url_encode(newsLeague_), prepare_news,
          [this, cb = std::move(cb)](const HttpResult& r) {
            if (!accept(r)) return cb(false, {});
            const std::shared_ptr<ParsedNews> p = prepared<ParsedNews>(r, prepare_news);
            if (!p || !p->ok) {
              lastError_ = FetchError::Parse;
              return cb(false, {});
            }
            lastError_ = FetchError::None;
            cb(true, p->news);  // a copy: a folded request hands the same answer to two callers
          });
}

void HttpSource::fetch_teams(std::function<void(bool, std::vector<Team>)> cb) {
  enqueue(Kind::Teams, base_ + "/api/teams", prepare_teams, [this, cb = std::move(cb)](const HttpResult& r) {
    if (!accept(r)) return cb(false, {});
    const std::shared_ptr<ParsedTeams> p = prepared<ParsedTeams>(r, prepare_teams);
    if (!p || !p->ok) {
      lastError_ = FetchError::Parse;
      return cb(false, {});
    }
    lastError_ = FetchError::None;
    cb(true, p->teams);  // a copy: a folded request hands the same answer to two callers
  });
}

void HttpSource::enqueue(Kind kind, std::string target, HttpPrepare prepare,
                         std::function<void(const HttpResult&)> handle) {
  queue_.push_back({kind, std::move(target), std::move(prepare), std::move(handle)});
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
        // older()'s caller may destroy the source: newer() then must not run.
        queue_[j].handle = [alive = alive_, older = std::move(older),
                            newer = std::move(newer)](const HttpResult& r) {
          older(r);
          if (*alive) newer(r);
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
  transport_.get_prepared(url, token_, kHttpTimeoutMs, std::move(req.prepare),
                          [this, alive, handle = std::move(handle)](HttpResult r) {
                            if (!*alive) return;
                            busy_ = false;
                            handle(r);  // may queue more requests (and start one)
                            if (*alive) pump();
                          });
}
