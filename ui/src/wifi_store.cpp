#include "wifi_store.h"

#include <algorithm>

namespace {

constexpr unsigned char kMarker = 0xFE;

bool plain_text(const std::string& s) {
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

// CRC-16/CCITT-FALSE (polynomial 0x1021, start 0xFFFF).
uint16_t crc16(const unsigned char* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace

bool wifi_ssid_valid(const std::string& ssid) {
  return !ssid.empty() && ssid.size() <= WifiStore::kMaxSsidBytes && plain_text(ssid);
}

bool wifi_password_valid(const std::string& password) {
  return password.size() <= WifiStore::kMaxPasswordBytes && plain_text(password);
}

bool WifiStore::add_or_replace(const std::string& ssid, const std::string& password) {
  if (!wifi_ssid_valid(ssid) || !wifi_password_valid(password)) return false;
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) { return e.ssid == ssid; }),
                 entries_.end());
  // Only a new SSID in a full list pushes the oldest out.
  if (entries_.size() >= kMaxSaved) entries_.resize(kMaxSaved - 1);
  entries_.insert(entries_.begin(), Entry{ssid, password});
  return true;
}

bool WifiStore::touch(const std::string& ssid) {
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.ssid == ssid; });
  if (it == entries_.end()) return false;
  std::rotate(entries_.begin(), it, it + 1);
  return true;
}

bool WifiStore::remove(const std::string& ssid) {
  auto it = std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.ssid == ssid; });
  if (it == entries_.end()) return false;
  entries_.erase(it, entries_.end());
  return true;
}

bool WifiStore::contains(const std::string& ssid) const {
  return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.ssid == ssid; });
}

std::vector<std::string> WifiStore::ssids() const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const Entry& e : entries_) out.push_back(e.ssid);
  return out;
}

bool WifiStore::password_of(const std::string& ssid, std::string& out) const {
  for (const Entry& e : entries_) {
    if (e.ssid == ssid) {
      out = e.password;
      return true;
    }
  }
  return false;
}

std::vector<WifiNetwork> WifiStore::ordered_by_signal(const std::vector<WifiNetwork>& scan) const {
  std::vector<WifiNetwork> out;  // in most-recently-used order first, then sorted (stable)
  for (const Entry& e : entries_) {
    const WifiNetwork* best = nullptr;
    for (const WifiNetwork& n : scan) {
      if (n.ssid == e.ssid && (best == nullptr || n.rssi > best->rssi)) best = &n;
    }
    if (best != nullptr) out.push_back(*best);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });
  return out;
}

std::string WifiStore::encode_record(const std::string& ssid, const std::string& password) {
  const std::string s = ssid.substr(0, 255);
  const std::string p = password.substr(0, 255);
  std::string rec;
  rec.reserve(s.size() + p.size() + 5);
  rec.push_back(static_cast<char>(kMarker));
  rec.push_back(static_cast<char>(s.size()));
  rec.push_back(static_cast<char>(p.size()));
  rec += s;
  rec += p;
  const uint16_t crc = crc16(reinterpret_cast<const unsigned char*>(rec.data()), rec.size());
  rec.push_back(static_cast<char>(crc >> 8));
  rec.push_back(static_cast<char>(crc & 0xFF));
  return rec;
}

std::string WifiStore::serialize() const {
  std::string out;
  for (const Entry& e : entries_) out += encode_record(e.ssid, e.password);
  return out;
}

WifiStore::ParseResult WifiStore::parse(const std::string& data) {
  ParseResult result;
  entries_.clear();
  const unsigned char* d = reinterpret_cast<const unsigned char*>(data.data());
  const size_t n = data.size();
  size_t pos = 0;
  while (pos < n) {
    bool ok = false;
    size_t len = 0;
    if (d[pos] == kMarker && pos + 3 <= n) {
      const size_t sl = d[pos + 1];
      const size_t pl = d[pos + 2];
      len = 3 + sl + pl + 2;
      if (pos + len <= n) {
        const uint16_t want = static_cast<uint16_t>((d[pos + len - 2] << 8) | d[pos + len - 1]);
        if (crc16(d + pos, len - 2) == want) {
          const std::string ssid(data, pos + 3, sl);
          const std::string password(data, pos + 3 + sl, pl);
          ok = true;
          // A well-formed record that holds something the radio cannot use, or
          // an SSID already seen, or the 17th entry, is dropped as a whole.
          if (wifi_ssid_valid(ssid) && wifi_password_valid(password) && !contains(ssid) &&
              entries_.size() < kMaxSaved) {
            entries_.push_back(Entry{ssid, password});
            ++result.loaded;
          }
        }
      }
    }
    if (ok) {
      pos += len;
    } else {
      ++pos;
      ++result.skippedBytes;
    }
  }
  return result;
}
