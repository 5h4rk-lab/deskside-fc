#include "json_parse.h"

#include <ArduinoJson.h>

#include <string.h>

#include <utility>

#include "theme.h"

namespace {

using Obj = JsonObjectConst;

// True when `key` is present, even with a null value. (isNull() cannot tell
// an absent key from an explicit null, and containsKey() is deprecated.)
bool has_key(Obj o, const char* key) {
  for (JsonPairConst kv : o) {
    if (strcmp(kv.key().c_str(), key) == 0) return true;
  }
  return false;
}

// Required string field. A missing key or a non-string value fails.
bool get_str(Obj o, const char* key, std::string& out) {
  JsonVariantConst v = o[key];
  if (!v.is<const char*>()) return false;
  out = v.as<const char*>();
  return true;
}

// Nullable string field: the key must be present. null gives "", a string is
// copied, any other type (or an absent key) fails.
bool get_opt_str(Obj o, const char* key, std::string& out) {
  if (!has_key(o, key)) return false;
  JsonVariantConst v = o[key];
  if (v.isNull()) {
    out.clear();
    return true;
  }
  if (!v.is<const char*>()) return false;
  out = v.as<const char*>();
  return true;
}

bool get_int(Obj o, const char* key, int& out) {
  JsonVariantConst v = o[key];
  if (!v.is<int>()) return false;
  out = v.as<int>();
  return true;
}

bool get_i64(Obj o, const char* key, int64_t& out) {
  JsonVariantConst v = o[key];
  if (!v.is<int64_t>()) return false;
  out = v.as<int64_t>();
  return true;
}

bool get_float(Obj o, const char* key, float& out) {
  JsonVariantConst v = o[key];
  if (!v.is<float>()) return false;
  out = v.as<float>();
  return true;
}

// Nullable integer: the key must be present. null gives nullopt.
bool get_opt_int(Obj o, const char* key, std::optional<int>& out) {
  if (!has_key(o, key)) return false;
  JsonVariantConst v = o[key];
  if (v.isNull()) {
    out.reset();
    return true;
  }
  if (!v.is<int>()) return false;
  out = v.as<int>();
  return true;
}

bool get_obj(Obj o, const char* key, Obj& out) {
  JsonVariantConst v = o[key];
  if (!v.is<JsonObjectConst>()) return false;
  out = v.as<JsonObjectConst>();
  return true;
}

bool get_arr(Obj o, const char* key, JsonArrayConst& out) {
  JsonVariantConst v = o[key];
  if (!v.is<JsonArrayConst>()) return false;
  out = v.as<JsonArrayConst>();
  return true;
}

bool parse_json(const std::string& body, JsonDocument& doc) {
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
  return doc.is<JsonObjectConst>();
}

bool read_team(Obj o, bool withLeague, Team& t) {
  std::string color;
  if (!get_str(o, "id", t.id) || !get_str(o, "abbr", t.abbr) || !get_str(o, "name", t.name) ||
      !get_str(o, "crest", t.crest) || !get_str(o, "color", color)) {
    return false;
  }
  t.color = parse_hex(color.c_str());
  // Optional: absent, not a string or not "#RRGGBB" gives the accent colour,
  // so a helper that predates `primary` still works.
  t.primary = t.color;
  JsonVariantConst primary = o["primary"];
  if (primary.is<const char*>()) try_parse_hex(primary.as<const char*>(), t.primary);
  if (withLeague && !get_str(o, "league", t.league)) return false;
  return true;
}

bool read_state(const std::string& s, MatchState& out) {
  if (s == "pre") out = MatchState::Pre;
  else if (s == "live") out = MatchState::Live;
  else if (s == "post") out = MatchState::Post;
  else if (s == "postponed") out = MatchState::Postponed;
  else return false;
  return true;
}

bool read_match(Obj o, MatchSummary& m) {
  std::string state;
  Obj home, away;
  if (!get_str(o, "id", m.id) || !get_str(o, "league", m.league) || !get_str(o, "state", state) ||
      !read_state(state, m.state) || !get_opt_str(o, "clock", m.clock) ||
      !get_i64(o, "kickoff", m.kickoff) || !get_obj(o, "home", home) ||
      !get_obj(o, "away", away) || !read_team(home, false, m.home) ||
      !read_team(away, false, m.away) || !get_opt_int(o, "homeScore", m.homeScore) ||
      !get_opt_int(o, "awayScore", m.awayScore)) {
    return false;
  }
  return true;
}

bool read_event(Obj o, MatchEvent& e) {
  std::string type, team;
  if (!get_str(o, "minute", e.minute) || !get_str(o, "type", type) || !get_str(o, "team", team) ||
      !get_str(o, "player", e.player) || !get_opt_str(o, "detail", e.detail)) {
    return false;
  }
  if (type == "goal") e.type = EventType::Goal;
  else if (type == "yellow") e.type = EventType::Yellow;
  else if (type == "red") e.type = EventType::Red;
  else if (type == "sub") e.type = EventType::Sub;
  else return false;
  if (team == "home") e.team = Side::Home;
  else if (team == "away") e.team = Side::Away;
  else return false;
  return true;
}

bool read_player(Obj o, LineupPlayer& p) {
  return get_str(o, "name", p.name) && get_int(o, "number", p.number) && get_float(o, "x", p.x) &&
         get_float(o, "y", p.y) && get_opt_str(o, "photo", p.photo);
}

bool read_lineup(Obj o, Lineup& l) {
  JsonArrayConst players;
  if (!get_str(o, "formation", l.formation) || !get_arr(o, "players", players)) return false;
  l.players.reserve(players.size());
  for (JsonVariantConst v : players) {
    if (!v.is<JsonObjectConst>()) return false;
    LineupPlayer p;
    if (!read_player(v.as<JsonObjectConst>(), p)) return false;
    l.players.push_back(std::move(p));
  }
  return true;
}

bool read_stat(Obj o, StatRow& s) {
  return get_str(o, "label", s.label) && get_str(o, "home", s.home) && get_str(o, "away", s.away);
}

bool read_news(Obj o, NewsItem& n) {
  return get_str(o, "id", n.id) && get_str(o, "headline", n.headline) &&
         get_i64(o, "published", n.published) && get_opt_str(o, "image", n.image);
}

// Reads every element of `arr` with `reader` into a new vector.
template <typename T, typename Reader>
bool read_list(JsonArrayConst arr, std::vector<T>& out, Reader reader) {
  out.reserve(arr.size());
  for (JsonVariantConst v : arr) {
    if (!v.is<JsonObjectConst>()) return false;
    T item;
    if (!reader(v.as<JsonObjectConst>(), item)) return false;
    out.push_back(std::move(item));
  }
  return true;
}

}  // namespace

bool parse_matches(const std::string& body, std::vector<MatchSummary>& out, int64_t& updated) {
  JsonDocument doc;
  if (!parse_json(body, doc)) return false;
  Obj root = doc.as<JsonObjectConst>();
  int64_t u;
  JsonArrayConst arr;
  std::vector<MatchSummary> tmp;
  if (!get_i64(root, "updated", u) || !get_arr(root, "matches", arr) ||
      !read_list(arr, tmp, read_match)) {
    return false;
  }
  out = std::move(tmp);
  updated = u;
  return true;
}

bool parse_detail(const std::string& body, MatchDetail& out) {
  JsonDocument doc;
  if (!parse_json(body, doc)) return false;
  Obj root = doc.as<JsonObjectConst>();
  MatchDetail d;
  Obj match, home, away;
  JsonArrayConst events, stats;
  if (!get_obj(root, "match", match) || !read_match(match, d.match) ||
      !get_arr(root, "events", events) || !read_list(events, d.events, read_event) ||
      !get_obj(root, "home", home) || !read_lineup(home, d.home) || !get_obj(root, "away", away) ||
      !read_lineup(away, d.away) || !get_arr(root, "stats", stats) ||
      !read_list(stats, d.stats, read_stat)) {
    return false;
  }
  out = std::move(d);
  return true;
}

bool parse_news(const std::string& body, std::vector<NewsItem>& out, int64_t& updated) {
  JsonDocument doc;
  if (!parse_json(body, doc)) return false;
  Obj root = doc.as<JsonObjectConst>();
  int64_t u;
  JsonArrayConst arr;
  std::vector<NewsItem> tmp;
  if (!get_i64(root, "updated", u) || !get_arr(root, "articles", arr) ||
      !read_list(arr, tmp, read_news)) {
    return false;
  }
  out = std::move(tmp);
  updated = u;
  return true;
}

bool parse_teams(const std::string& body, std::vector<Team>& out) {
  JsonDocument doc;
  if (!parse_json(body, doc)) return false;
  Obj root = doc.as<JsonObjectConst>();
  JsonArrayConst arr;
  std::vector<Team> tmp;
  if (!get_arr(root, "teams", arr) ||
      !read_list(arr, tmp, [](Obj o, Team& t) { return read_team(o, true, t); })) {
    return false;
  }
  out = std::move(tmp);
  return true;
}
