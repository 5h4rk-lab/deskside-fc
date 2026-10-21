// C++ mirror of the helper's JSON contract (helper/src/contract.ts).
// Times are epoch seconds UTC. Shared by the simulator and the ESP32 firmware.
#pragma once

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

enum class MatchState { Pre, Live, Post, Postponed };
enum class EventType { Goal, Yellow, Red, Sub };
enum class Side { Home, Away };

struct Team {
  std::string id;
  std::string abbr;
  std::string name;
  std::string crest;   // helper-relative path
  uint32_t color = 0;    // 0xRRGGBB: readable accent for small details
  uint32_t primary = 0;  // 0xRRGGBB: real brand colour for large surfaces (may be dark)
  std::string league;  // only set in the teams response
};

struct MatchSummary {
  std::string id;
  std::string league;
  MatchState state = MatchState::Pre;
  std::string clock;  // empty when the contract value is null
  int64_t kickoff = 0;
  Team home;
  Team away;
  std::optional<int> homeScore;
  std::optional<int> awayScore;
};

struct MatchEvent {
  std::string minute;  // e.g. "45'+2'"
  EventType type = EventType::Goal;
  Side team = Side::Home;
  std::string player;
  std::string detail;  // empty when null
};

struct LineupPlayer {
  std::string name;
  int number = 0;
  float x = 0.0f;  // 0..1 across the pitch
  float y = 0.0f;  // 0..1 along the pitch
  std::string photo;  // helper-relative path, empty when null
};

struct Lineup {
  std::string formation;
  std::vector<LineupPlayer> players;  // empty when the lineup is not published
};

struct StatRow {
  std::string label;
  std::string home;
  std::string away;
};

struct MatchDetail {
  MatchSummary match;
  std::vector<MatchEvent> events;
  Lineup home;
  Lineup away;
  std::vector<StatRow> stats;
};

struct NewsItem {
  std::string id;
  std::string headline;
  int64_t published = 0;
  std::string image;  // helper-relative path, empty when null
};
