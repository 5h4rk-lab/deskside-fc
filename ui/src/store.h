// Latest data fetched from the helper, shared by every screen.
//
// Pointers returned by live_for(), next_for() and last_for() point into matches() and are
// invalidated by the next set_matches(). Nothing here reads the clock: every
// time-dependent call takes `now` (epoch seconds).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "data_source.h"
#include "model.h"
#include "power_service.h"

// The status rows' and Settings' text while the clock is not valid.
constexpr const char* kWaitingForClock = "Waiting for the clock";
// Settings' data line, and the status rows while the clock is not valid, when
// the Wi-Fi link is down (no clock can come without it).
constexpr const char* kNoWifiConnection = "No Wi-Fi connection";
constexpr const char* kNoWifiShort = "No Wi-Fi";

class Store {
 public:
  // Replaces the match list and records `now` as the last success time. A
  // success: resets failures() to 0.
  void set_matches(std::vector<MatchSummary> matches, int64_t now);
  // A failed poll: keeps the old data, does not touch the success time and
  // counts one more failure.
  void mark_failure() { ++failures_; }
  // Failed match polls since the last success (drives the refresh back-off).
  int failures() const { return failures_; }

  // Why fetching is failing, as last reported by the poller (None when fine).
  void set_error(FetchError error) { error_ = error; }
  FetchError last_error() const { return error_; }
  // Plain-English state of the data for Settings: "Live data is up to date",
  // "Waiting for the first update...", or what is wrong ("Cannot reach the
  // helper", "Helper rejected the token", "Helper returned an error", "Helper
  // sent unreadable data"). An error wins over old data, a missing clock
  // (kWaitingForClock: nothing is fetched without one) wins over both, and a
  // Wi-Fi link that is down (kNoWifiConnection) wins over everything.
  std::string data_status_text() const;

  const std::vector<MatchSummary>& matches() const { return matches_; }
  // False while the host has no valid clock (HostLoop sets it): the status
  // rows read "Waiting for the clock" and data_status_text() says so. True
  // until told otherwise.
  void set_clock_valid(bool valid) { clockValid_ = valid; }
  bool clock_valid() const { return clockValid_; }
  // True while the device's Wi-Fi link is down (app_refresh sets it from the
  // WifiService; hosts without one leave it false). It wins over the clock
  // and over fetch errors in data_status_text(), and over "Waiting for the
  // clock" in the status rows: the real problem is named first.
  void set_link_down(bool down) { linkDown_ = down; }
  bool link_down() const { return linkDown_; }

  // Grows by one at every set_matches(): screens that derive views from the
  // list compare it to know whether the list changed since they last looked.
  uint32_t matches_version() const { return matchesVersion_; }
  // True once set_matches() has succeeded at least once (UI shows "Connecting"
  // until then).
  bool has_data() const { return hasData_; }

  // First Live match in which the team plays (home or away), else nullptr.
  const MatchSummary* live_for(const std::string& teamId) const;
  // Earliest Pre match of the team with kickoff >= now, else nullptr.
  const MatchSummary* next_for(const std::string& teamId, int64_t now) const;

  // Most recent Post match of the team (home or away) with kickoff <= now
  // (the first of equal kickoffs), else nullptr.
  const MatchSummary* last_for(const std::string& teamId, int64_t now) const;

  // Time of the last successful set_matches() (0 before the first one).
  int64_t last_success() const { return lastSuccess_; }

  // True when data exists but the last success is older than stale_after_s():
  // the poll interval the list asks for (RefreshPolicy: 25 s with a live
  // match, 180 s without) plus kStaleGraceSeconds, and never less than
  // kStaleMinSeconds. So polls on time never show stale, whatever the
  // interval, and one missed poll does. False when no data has ever arrived.
  bool stale(int64_t now) const;
  static constexpr int64_t kStaleMinSeconds = 90;
  static constexpr int64_t kStaleGraceSeconds = 60;
  int64_t stale_after_s() const;

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

  // Battery state, as last read from the PowerService by app_refresh()
  // (present = false until then, and on hosts without a battery).
  void set_power(PowerStatus power) { power_ = power; }
  const PowerStatus& power() const { return power_; }

 private:
  std::vector<MatchSummary> matches_;
  std::vector<MatchDetail> details_;  // oldest first, at most kDetailCacheSize
  std::vector<NewsItem> news_;
  std::vector<Team> teams_;
  std::string favourite_;
  int64_t lastSuccess_ = 0;
  uint32_t matchesVersion_ = 0;
  bool clockValid_ = true;
  bool linkDown_ = false;
  bool hasData_ = false;
  int failures_ = 0;
  FetchError error_ = FetchError::None;
  int tzOffset_ = 0;
  PowerStatus power_;
};
