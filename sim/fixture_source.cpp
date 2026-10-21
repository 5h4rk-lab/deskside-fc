#include "fixture_source.h"

#include <fstream>
#include <sstream>

#include "demo_data.h"

namespace {

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) return "";
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

std::string fixture_text(const std::string& name) {
  return read_file(std::string(CONTRACT_FIXTURES_DIR) + "/" + name);
}

FixtureSource::FixtureSource(const std::string& fixturesDir) {
  const std::string dir = fixturesDir + "/";
  load_json(read_file(dir + "matches-normalised.json"), read_file(dir + "detail-normalised.json"),
            read_file(dir + "news-normalised.json"), read_file(dir + "teams-normalised.json"));
}

std::vector<NewsItem> demo_news(int64_t now) {
  return demo_news(fixture_text("news-normalised.json"), now);
}

std::vector<Team> demo_teams() { return demo_teams(fixture_text("teams-normalised.json")); }
