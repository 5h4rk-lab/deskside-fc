#include "format.h"

#include <vector>

namespace {

std::string two_digits(int v) {
  char buf[3];
  buf[0] = static_cast<char>('0' + (v / 10) % 10);
  buf[1] = static_cast<char>('0' + v % 10);
  buf[2] = '\0';
  return buf;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

std::string hh_mm(int64_t local) {
  const int64_t secs = local - floor_div(local, 86400) * 86400;
  return two_digits(static_cast<int>(secs / 3600)) + ":" +
         two_digits(static_cast<int>((secs % 3600) / 60));
}

const char* weekday(int64_t local) {
  static const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int64_t d = (floor_div(local, 86400) + 4) % 7;  // 1970-01-01 was a Thursday
  if (d < 0) d += 7;
  return kDays[d];
}

std::string to_upper(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;  // non-ASCII letters keep their case (fonts carry both)
}

std::string url_encode(const std::string& s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (keep) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

std::string with_param(const std::string& path, const std::string& key, const std::string& value) {
  const size_t q = path.find('?');
  if (q == std::string::npos) return path + "?" + key + "=" + value;
  size_t pos = q + 1;
  while (pos < path.size()) {
    size_t end = path.find('&', pos);
    if (end == std::string::npos) end = path.size();
    if (path.compare(pos, key.size() + 1, key + "=") == 0) {
      return path.substr(0, pos) + key + "=" + value + path.substr(end);
    }
    pos = end + 1;
  }
  return path + "&" + key + "=" + value;
}

std::string sized(const std::string& path, int w, int h) {
  if (path.empty()) return path;
  return with_param(with_param(path, "w", std::to_string(w)), "h", std::to_string(h));
}

const char* league_name(const std::string& code) {
  struct Entry {
    const char* code;
    const char* name;
  };
  static const Entry kLeagues[] = {
      {"eng.1", "Premier League"},     {"esp.1", "La Liga"},
      {"ita.1", "Serie A"},            {"ger.1", "Bundesliga"},
      {"fra.1", "Ligue 1"},            {"uefa.champions", "Champions League"},
      {"uefa.europa", "Europa League"}, {"eng.fa", "FA Cup"},
      {"eng.league_cup", "League Cup"}, {"fifa.world", "World Cup"},
      {"uefa.euro", "Euro"},
  };
  for (const Entry& e : kLeagues) {
    if (code == e.code) return e.name;
  }
  return code.c_str();
}

std::string countdown_text(int64_t seconds) {
  if (seconds < 60) return "Kick-off soon";
  if (seconds < 3600) return "Kick-off in " + std::to_string(seconds / 60) + " min";
  if (seconds < 86400) {
    const int64_t minutes = (seconds % 3600) / 60;
    std::string out = "Kick-off in " + std::to_string(seconds / 3600) + " h";
    if (minutes > 0) out += " " + std::to_string(minutes) + " min";
    return out;
  }
  const int64_t days = seconds / 86400;
  return "Kick-off in " + std::to_string(days) + (days == 1 ? " day" : " days");
}

std::string stale_text(int64_t age) {
  int64_t minutes = age / 60;
  if (minutes < 1) minutes = 1;
  if (minutes < 60) return "Updated " + std::to_string(minutes) + " min ago";
  return "Updated " + std::to_string(minutes / 60) + " h ago";
}

std::string formation_blurb(const std::string& formation) {
  std::vector<int> lines;
  int value = 0;
  bool inNumber = false;
  for (char c : formation) {
    if (is_digit(c)) {
      value = value * 10 + (c - '0');
      if (value > 99) return "";
      inNumber = true;
    } else if (c == '-' && inNumber) {
      lines.push_back(value);
      value = 0;
      inNumber = false;
    } else {
      return "";
    }
  }
  if (!inNumber) return "";
  lines.push_back(value);
  if (lines.size() < 3) return "";

  int midfielders = 0;
  for (size_t i = 1; i + 1 < lines.size(); ++i) midfielders += lines[i];
  auto count = [](int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
  };
  return count(lines.front(), "defender", "defenders") + ", " +
         count(midfielders, "midfielder", "midfielders") + ", " +
         count(lines.back(), "attacker", "attackers");
}

int minute_sort_key(const std::string& minute) {
  constexpr int kLast = 1 << 30;
  size_t i = 0;
  int base = 0;
  if (i >= minute.size() || !is_digit(minute[i])) return kLast;
  while (i < minute.size() && is_digit(minute[i]) && base < 10000) {
    base = base * 10 + (minute[i++] - '0');
  }
  int added = 0;
  const size_t plus = minute.find('+', i);
  if (plus != std::string::npos) {
    for (size_t j = plus + 1; j < minute.size() && is_digit(minute[j]) && added < 1000; ++j) {
      added = added * 10 + (minute[j] - '0');
    }
  }
  return base * 1000 + added;
}

bool stat_number(const std::string& value, double& out) {
  size_t i = 0;
  while (i < value.size() && value[i] == ' ') ++i;
  if (i >= value.size() || !is_digit(value[i])) return false;
  double v = 0;
  while (i < value.size() && is_digit(value[i])) v = v * 10 + (value[i++] - '0');
  if (i + 1 < value.size() && value[i] == '.' && is_digit(value[i + 1])) {
    double scale = 0.1;
    for (++i; i < value.size() && is_digit(value[i]); ++i) {
      v += (value[i] - '0') * scale;
      scale /= 10;
    }
  }
  out = v;
  return true;
}
