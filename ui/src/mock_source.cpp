#include "mock_source.h"

#include <fstream>
#include <sstream>

#include "json_parse.h"

namespace {

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

MockSource::MockSource(const std::string& fixturesDir) {
  const std::string dir = fixturesDir + "/";
  std::string body;
  int64_t updated = 0;
  if (read_file(dir + "matches-normalised.json", body)) parse_matches(body, matches_, updated);
  if (read_file(dir + "detail-normalised.json", body)) {
    hasDefaultDetail_ = parse_detail(body, defaultDetail_);
  }
  if (read_file(dir + "news-normalised.json", body)) parse_news(body, news_, updated);
  if (read_file(dir + "teams-normalised.json", body)) parse_teams(body, teams_);
}

void MockSource::fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) {
  if (fail_) return cb(false, {});
  cb(true, matches_);
}

void MockSource::fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) {
  if (fail_) return cb(false, MatchDetail{});
  auto it = details_.find(id);
  if (it != details_.end()) return cb(true, it->second);
  if (hasDefaultDetail_) return cb(true, defaultDetail_);
  cb(false, MatchDetail{});
}

void MockSource::fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) {
  if (fail_) return cb(false, {});
  cb(true, news_);
}

void MockSource::fetch_teams(std::function<void(bool, std::vector<Team>)> cb) {
  if (fail_) return cb(false, {});
  cb(true, teams_);
}

void MockSource::set_detail(MatchDetail detail) {
  const std::string id = detail.match.id;
  details_[id] = std::move(detail);
}

void MockSource::advance_score(const std::string& matchId, Side side) {
  for (MatchSummary& m : matches_) {
    if (m.id != matchId) continue;
    if (m.state == MatchState::Pre) m.state = MatchState::Live;
    if (!m.homeScore) m.homeScore = 0;
    if (!m.awayScore) m.awayScore = 0;
    if (side == Side::Home) {
      ++*m.homeScore;
    } else {
      ++*m.awayScore;
    }
    return;
  }
}
