#include <doctest/doctest.h>

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "file_prefs.h"

namespace {

std::string temp_path(const char* name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "soccer-display-file-prefs-test" / name;
  std::filesystem::remove_all(dir);
  return (dir / "sub" / "prefs.txt").string();
}

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("FilePrefs: a missing file starts empty; set creates the directory and file") {
  const std::string path = temp_path("create");
  FilePrefs p(path);
  std::string v = "untouched";
  CHECK_FALSE(p.get("zone", v));
  CHECK(v == "untouched");
  p.set("zone", "Europe/London");
  CHECK(p.saved());
  CHECK(std::filesystem::exists(path));
  CHECK(slurp(path) == "zone=Europe/London\n");
}

TEST_CASE("FilePrefs: values survive a reload, including '=', backslashes and newlines") {
  const std::string path = temp_path("reload");
  {
    FilePrefs p(path);
    p.set("fav.name", "Brighton & Hove Albion");
    p.set("odd", "a=b\\c\nd");
    p.set("gone", "x");
    p.remove("gone");
  }
  FilePrefs q(path);
  std::string v;
  CHECK(q.get("fav.name", v));
  CHECK(v == "Brighton & Hove Albion");
  CHECK(q.get("odd", v));
  CHECK(v == "a=b\\c\nd");
  CHECK_FALSE(q.get("gone", v));
}

TEST_CASE("FilePrefs: lines without '=' are ignored") {
  const std::string path = temp_path("junk");
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  {
    std::ofstream out(path);
    out << "garbage\nbright=40\n\n";
  }
  FilePrefs p(path);
  std::string v;
  CHECK(p.get("bright", v));
  CHECK(v == "40");
  CHECK_FALSE(p.get("garbage", v));
}

TEST_CASE("FilePrefs: default path under the config directory") {
  const char* old = getenv("XDG_CONFIG_HOME");
  const std::string saved = old != nullptr ? old : "";
  setenv("XDG_CONFIG_HOME", "/tmp/cfg", 1);
  CHECK(FilePrefs::default_path() == "/tmp/cfg/soccer-display-sim/prefs.txt");
  if (old != nullptr) {
    setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
  } else {
    unsetenv("XDG_CONFIG_HOME");
  }
}
