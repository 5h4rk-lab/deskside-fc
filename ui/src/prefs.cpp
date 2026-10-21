#include "prefs.h"

bool MemoryPrefs::get(const std::string& key, std::string& out) const {
  const auto it = values_.find(key);
  if (it == values_.end()) return false;
  out = it->second;
  return true;
}

void MemoryPrefs::set(const std::string& key, const std::string& value) { values_[key] = value; }

void MemoryPrefs::remove(const std::string& key) { values_.erase(key); }

int pref_int(const Prefs& prefs, const std::string& key, int def) {
  std::string raw;
  if (!prefs.get(key, raw)) return def;
  size_t i = 0;
  const bool negative = !raw.empty() && raw[0] == '-';
  if (negative) i = 1;
  if (i >= raw.size()) return def;
  // Accumulate negatively so INT_MIN parses; stop on overflow.
  const long long limit = negative ? 2147483648LL : 2147483647LL;
  long long v = 0;
  for (; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c < '0' || c > '9') return def;
    v = v * 10 + (c - '0');
    if (v > limit) return def;
  }
  return static_cast<int>(negative ? -v : v);
}

void set_pref_int(Prefs& prefs, const std::string& key, int value) {
  prefs.set(key, std::to_string(value));
}

bool pref_bool(const Prefs& prefs, const std::string& key, bool def) {
  std::string raw;
  if (!prefs.get(key, raw)) return def;
  if (raw == "1") return true;
  if (raw == "0") return false;
  return def;
}

void set_pref_bool(Prefs& prefs, const std::string& key, bool value) {
  prefs.set(key, value ? "1" : "0");
}

bool prefs_key_valid(const std::string& key) {
  if (key.empty() || key.size() > kPrefsMaxKeyLen) return false;
  for (const char c : key) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool prefs_value_valid(const std::string& value) { return value.size() <= kPrefsMaxValueLen; }
