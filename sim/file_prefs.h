// Prefs kept in a text file, so the simulator remembers the favourite team,
// time zone and display settings between runs. Simulator only: uses the
// standard filesystem.
//
// Format: one "key=value" per line; in values a backslash is written as "\\"
// and a newline as "\n". Lines without '=' are ignored.
#pragma once

#include <map>
#include <string>

#include "prefs.h"

class FilePrefs : public Prefs {
 public:
  // Loads `path` when it exists. Every change is written back at once; the
  // directory is created on demand.
  explicit FilePrefs(std::string path);

  bool get(const std::string& key, std::string& out) const override;
  void set(const std::string& key, const std::string& value) override;
  void remove(const std::string& key) override;

  // False after a failed write (the values stay in memory).
  bool saved() const { return saved_; }
  const std::string& path() const { return path_; }

  // $XDG_CONFIG_HOME/soccer-display-sim/prefs.txt, else
  // $HOME/.config/soccer-display-sim/prefs.txt, else "soccer-display-sim-prefs.txt".
  static std::string default_path();

 private:
  void save();

  std::string path_;
  std::map<std::string, std::string> values_;
  bool saved_ = true;
};
