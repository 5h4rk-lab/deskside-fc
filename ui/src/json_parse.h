// Parsers for the helper's JSON responses (see model.h).
//
// Every parser returns false on malformed JSON, a wrong top-level shape, a
// missing required field or an unknown enum string. On failure the outputs are
// left untouched. Unknown JSON fields are ignored.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "model.h"

// {updated, matches:[MatchSummary...]}
bool parse_matches(const std::string& body, std::vector<MatchSummary>& out, int64_t& updated);

// A bare MatchDetail.
bool parse_detail(const std::string& body, MatchDetail& out);

// {updated, articles:[NewsItem...]}
bool parse_news(const std::string& body, std::vector<NewsItem>& out, int64_t& updated);

// {teams:[(Team + league)...]}
bool parse_teams(const std::string& body, std::vector<Team>& out);
