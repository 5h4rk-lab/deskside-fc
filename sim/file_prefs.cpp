#include "file_prefs.h"

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace {

std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

std::string unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      ++i;
      out += s[i] == 'n' ? '\n' : s[i];
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace

FilePrefs::FilePrefs(std::string path) : path_(std::move(path)) {
  std::ifstream in(path_);
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    values_[line.substr(0, eq)] = unescape(line.substr(eq + 1));
  }
}

bool FilePrefs::get(const std::string& key, std::string& out) const {
  const auto it = values_.find(key);
  if (it == values_.end()) return false;
  out = it->second;
  return true;
}

void FilePrefs::set(const std::string& key, const std::string& value) {
  auto it = values_.find(key);
  if (it != values_.end() && it->second == value) return;
  values_[key] = value;
  save();
}

void FilePrefs::remove(const std::string& key) {
  if (values_.erase(key) != 0) save();
}

void FilePrefs::save() {
  std::error_code ec;
  const std::filesystem::path p(path_);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  // Written to a temporary file first, so a crash never leaves half a file.
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const auto& kv : values_) out << kv.first << '=' << escape(kv.second) << '\n';
    saved_ = out.good();
  }
  if (saved_) {
    std::filesystem::rename(tmp, p, ec);
    saved_ = !ec;
  }
}

std::string FilePrefs::default_path() {
  const char* xdg = getenv("XDG_CONFIG_HOME");
  if (xdg != nullptr && *xdg != '\0') return std::string(xdg) + "/soccer-display-sim/prefs.txt";
  const char* home = getenv("HOME");
  if (home != nullptr && *home != '\0') return std::string(home) + "/.config/soccer-display-sim/prefs.txt";
  return "soccer-display-sim-prefs.txt";
}
