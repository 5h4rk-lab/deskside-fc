#include "mock_image_provider.h"

#include <fstream>
#include <iterator>
#include <utility>

namespace {

// --- Path to asset name ------------------------------------------------------

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes %XX sequences; malformed sequences are kept literally.
std::string percent_decode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2])));
      i += 2;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

// Value of query parameter `key` (percent-decoded), or false when absent.
bool query_param(const std::string& query, const std::string& key, std::string& value) {
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t end = query.find('&', pos);
    if (end == std::string::npos) end = query.size();
    const std::string pair = query.substr(pos, end - pos);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.compare(0, eq, key) == 0) {
      value = percent_decode(pair.substr(eq + 1));
      return true;
    }
    pos = end + 1;
  }
  return false;
}

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

// "<id>.png" -> "<id>" when the name is digits plus ".png".
bool png_id(const std::string& file, std::string& id) {
  if (!ends_with(file, ".png")) return false;
  id = file.substr(0, file.size() - 4);
  return all_digits(id);
}

std::string image_asset_name(const std::string& url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  const size_t hostStart = scheme + 3;
  const size_t slash = url.find('/', hostStart);
  if (slash == std::string::npos) return "";
  const std::string host = url.substr(hostStart, slash - hostStart);
  size_t pathEnd = url.find_first_of("?#", slash);
  if (pathEnd == std::string::npos) pathEnd = url.size();
  const std::string urlPath = url.substr(slash, pathEnd - slash);
  const std::string base = urlPath.substr(urlPath.rfind('/') + 1);

  std::string id;
  if (host == "crests.football-data.org" && urlPath.find('/', 1) == std::string::npos &&
      png_id(base, id)) {
    return "crest-" + id + ".png";
  }
  if (host == "a.espncdn.com" && starts_with(urlPath, "/i/teamlogos/soccer/500/") &&
      urlPath.find('/', 24) == std::string::npos && png_id(base, id)) {
    return "espn-" + id + ".png";
  }
  if (base.empty() || base == "." || base == "..") return "";
  std::string safe;
  for (char c : base) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    safe.push_back(ok ? c : '_');
  }
  return "img-" + safe;
}

std::string cutout_asset_name(const std::string& name) {
  if (name.empty()) return "";
  std::string out = "cutout-";
  for (unsigned char c : name) {
    if ((c & 0xC0) == 0x80) continue;  // continuation byte: part of the previous character
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    out.push_back(alnum ? static_cast<char>(c) : '_');
  }
  return out + ".png";
}

// --- File reading --------------------------------------------------------------

constexpr size_t kMaxFileBytes = 8u * 1024 * 1024;

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty() && out.size() <= kMaxFileBytes;
}

}  // namespace

std::string mock_asset_name(const std::string& path) {
  const size_t q = path.find('?');
  if (q == std::string::npos) return "";
  const std::string route = path.substr(0, q);
  const std::string query = path.substr(q + 1);
  std::string value;
  if (route == "/api/img") {
    if (!query_param(query, "u", value) || value.empty()) return "";
    return image_asset_name(value);
  }
  if (route == "/api/cutout") {
    if (!query_param(query, "name", value)) return "";
    return cutout_asset_name(value);
  }
  return "";
}

MockImageProvider::MockImageProvider(ImageDecoder& decoder, std::string assetDir, size_t cacheBudget)
    : decoder_(decoder), dir_(std::move(assetDir)), cache_(cacheBudget) {}

ImagePtr MockImageProvider::load(const std::string& name) const {
  std::string file;
  if (!read_file(dir_ + "/" + name, file)) return nullptr;
  return decoder_.decode(file);
}

void MockImageProvider::request(const std::string& path, std::function<void(ImagePtr)> done) {
  const std::string name = mock_asset_name(path);
  if (name.empty()) return done(nullptr);

  if (ImagePtr* hit = cache_.get(name)) {
    ImagePtr image = *hit;  // copy before the callback can touch the cache
    return done(std::move(image));
  }
  ImagePtr image = load(name);
  if (!image) return done(nullptr);
  cache_.put(name, image, image->data_size);
  done(std::move(image));
}
