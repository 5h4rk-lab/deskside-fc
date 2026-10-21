// Simulator and tests only: the contract fixtures read from disk, for the
// pure ui/ classes that take JSON text (MockSource, demo_news, demo_teams).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "mock_source.h"
#include "model.h"

// File `name` of contract/fixtures (CONTRACT_FIXTURES_DIR), "" when it cannot
// be read.
std::string fixture_text(const std::string& name);

// A MockSource filled from the *-normalised.json files of `fixturesDir`.
// Files that are missing or fail to parse leave the corresponding default
// empty.
class FixtureSource : public MockSource {
 public:
  explicit FixtureSource(const std::string& fixturesDir);
};

// demo_news / demo_teams over the repository's fixtures.
std::vector<NewsItem> demo_news(int64_t now);
std::vector<Team> demo_teams();
