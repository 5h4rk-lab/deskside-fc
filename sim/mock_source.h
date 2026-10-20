// Scriptable DataSource for the simulator and tests. Callbacks run
// synchronously. Defaults come from the contract fixtures directory.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "data_source.h"

class MockSource : public DataSource {
 public:
  // Loads matches/detail/news/teams from `fixturesDir`. Files that are missing
  // or fail to parse leave the corresponding default empty.
  explicit MockSource(const std::string& fixturesDir);

  void fetch_matches(std::function<void(bool, std::vector<MatchSummary>)> cb) override;
  void fetch_detail(const std::string& id, std::function<void(bool, MatchDetail)> cb) override;
  void fetch_news(std::function<void(bool, std::vector<NewsItem>)> cb) override;
  void fetch_teams(std::function<void(bool, std::vector<Team>)> cb) override;

  void set_matches(std::vector<MatchSummary> matches) { matches_ = std::move(matches); }
  // Registers a detail under detail.match.id. fetch_detail falls back to the
  // fixture detail for ids with no registered detail.
  void set_detail(MatchDetail detail);
  void set_news(std::vector<NewsItem> news) { news_ = std::move(news); }
  void set_teams(std::vector<Team> teams) { teams_ = std::move(teams); }
  // While true every fetch reports ok=false with empty data, and last_error()
  // reports `error`.
  void set_fail(bool fail, FetchError error = FetchError::Offline) {
    fail_ = fail;
    failError_ = error;
  }
  FetchError last_error() const override { return fail_ ? failError_ : FetchError::None; }

  // Increments one side's score of the match; a Pre match becomes Live. Unknown
  // ids are ignored.
  void advance_score(const std::string& matchId, Side side);

 private:
  std::vector<MatchSummary> matches_;
  std::map<std::string, MatchDetail> details_;
  MatchDetail defaultDetail_;
  bool hasDefaultDetail_ = false;
  std::vector<NewsItem> news_;
  std::vector<Team> teams_;
  bool fail_ = false;
  FetchError failError_ = FetchError::Offline;
};
