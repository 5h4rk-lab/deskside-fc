#include "mock_source.h"

#include "json_parse.h"

void MockSource::load_json(const std::string& matches, const std::string& detail,
                           const std::string& news, const std::string& teams) {
  int64_t updated = 0;
  if (!matches.empty()) parse_matches(matches, matches_, updated);
  if (!detail.empty()) hasDefaultDetail_ = parse_detail(detail, defaultDetail_);
  if (!news.empty()) parse_news(news, news_, updated);
  if (!teams.empty()) parse_teams(teams, teams_);
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
