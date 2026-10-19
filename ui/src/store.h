// Latest data fetched from the helper, shared by every screen.
//
// Pointers returned by live_for() and next_for() point into matches() and are
// invalidated by the next set_matches(). Nothing here reads the clock: every
// time-dependent call takes `now` (epoch seconds).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "model.h"

class Store {
 public:
  // Replaces the match list and records `now` as the last success time.
  void set_matches(std::vector<MatchSummary> matches, int64_t now);
  // A failed poll: keeps the old data and does not touch the success time.
  void mark_failure() {}

  const std::vector<MatchSummary>& matches() const { return matches_; }
  // True once set_matches() has succeeded at least once (UI shows "Connecting"
  // until then).
  bool has_data() const { return hasData_; }

  // First Live match in which the team plays (home or away), else nullptr.
  const MatchSummary* live_for(const std::string& teamId) const;
  // Earliest Pre match of the team with kickoff >= now, else nullptr.
  const MatchSummary* next_for(const std::string& teamId, int64_t now) const;

  // Time of the last successful set_matches() (0 before the first one).
  int64_t last_success() const { return lastSuccess_; }

  // True when data exists but the last success is more than 90 s old. False
  // when no data has ever arrived.
  bool stale(int64_t now) const;

  // Match details are cached for the last few matches (most recent last).
  // Setting a detail for an id already cached replaces it. Pointers returned
  // by detail_for() are invalidated by the next set_detail().
  static constexpr int kDetailCacheSize = 4;
  void set_detail(MatchDetail detail);
  const MatchDetail* detail_for(const std::string& matchId) const;

  void set_news(std::vector<NewsItem> news) { news_ = std::move(news); }
  const std::vector<NewsItem>& news() const { return news_; }
  void set_teams(std::vector<Team> teams) { teams_ = std::move(teams); }
  const std::vector<Team>& teams() const { return teams_; }

  void set_favourite(std::string teamId) { favourite_ = std::move(teamId); }
  const std::string& favourite() const { return favourite_; }

  // Local timezone offset from UTC in seconds (default 0).
  void set_tz_offset(int seconds) { tzOffset_ = seconds; }
  int tz_offset() const { return tzOffset_; }

 private:
  std::vector<MatchSummary> matches_;
  std::vector<MatchDetail> details_;  // oldest first, at most kDetailCacheSize
  std::vector<NewsItem> news_;
  std::vector<Team> teams_;
  std::string favourite_;
  int64_t lastSuccess_ = 0;
  bool hasData_ = false;
  int tzOffset_ = 0;
};
