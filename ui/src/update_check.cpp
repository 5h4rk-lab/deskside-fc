#include "update_check.h"

#include <ArduinoJson.h>
#include <string.h>

namespace {

constexpr int kVersionPartMax = 9999;

// One version part: 1..4 digits, no leading zero (except "0" itself).
bool parse_part(const std::string& s, size_t& pos, int& out) {
  const size_t start = pos;
  int value = 0;
  while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
    if (pos - start >= 4) return false;
    value = value * 10 + (s[pos] - '0');
    ++pos;
  }
  const size_t len = pos - start;
  if (len == 0) return false;
  if (len > 1 && s[start] == '0') return false;
  if (value > kVersionPartMax) return false;
  out = value;
  return true;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = lower(c);
  return out;
}

// The text after the scheme, or npos for anything but http:// and https://.
size_t after_scheme(const std::string& url, bool& https) {
  if (url.compare(0, 8, "https://") == 0) {
    https = true;
    return 8;
  }
  if (url.compare(0, 7, "http://") == 0) {
    https = false;
    return 7;
  }
  return std::string::npos;
}

bool good_url(const std::string& url, const ManifestRules& rules) {
  if (url.empty() || url.size() > kUpdateUrlMaxLen) return false;
  for (char c : url) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x21 || u > 0x7e) return false;
    if (c == '@' || c == '?' || c == '#' || c == '\\') return false;
  }
  bool https = false;
  const size_t start = after_scheme(url, https);
  if (start == std::string::npos) return false;
  if (!https && !rules.allowHttp) return false;
  const size_t slash = url.find('/', start);
  if (slash == std::string::npos) return false;  // no path
  if (url.size() - slash < 2) return false;      // "/" alone: nothing to download
  const std::string host = lowered(url.substr(start, slash - start));
  return !host.empty() && host == lowered(rules.host);
}

bool good_sha(const std::string& s, std::string& out) {
  if (s.size() != 64) return false;
  std::string l = lowered(s);
  for (char c : l) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  out = l;
  return true;
}

uint16_t read_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// A NUL-terminated string field of `n` bytes, or false when it has no NUL.
bool field(const uint8_t* p, size_t n, std::string& out) {
  const void* nul = memchr(p, 0, n);
  if (nul == nullptr) return false;
  out.assign(reinterpret_cast<const char*>(p), static_cast<const uint8_t*>(nul) - p);
  return true;
}

}  // namespace

bool parse_version(const std::string& text, Version& out) {
  Version v;
  size_t pos = 0;
  if (!parse_part(text, pos, v.major)) return false;
  if (pos >= text.size() || text[pos] != '.') return false;
  ++pos;
  if (!parse_part(text, pos, v.minor)) return false;
  if (pos >= text.size() || text[pos] != '.') return false;
  ++pos;
  if (!parse_part(text, pos, v.patch)) return false;
  if (pos != text.size()) return false;
  out = v;
  return true;
}

std::string version_text(const Version& v) {
  return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

bool is_newer(Version a, Version b) {
  if (a.major != b.major) return a.major > b.major;
  if (a.minor != b.minor) return a.minor > b.minor;
  return a.patch > b.patch;
}

std::string url_host(const std::string& url) {
  bool https = false;
  const size_t start = after_scheme(url, https);
  if (start == std::string::npos) return "";
  const size_t slash = url.find('/', start);
  return lowered(url.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
}

ManifestRules rules_for_manifest_url(const std::string& manifestUrl, bool allowHttp) {
  ManifestRules r;
  r.host = url_host(manifestUrl);
  r.allowHttp = allowHttp && manifestUrl.compare(0, 7, "http://") == 0;
  return r;
}

std::string manifest_url_for_helper(const std::string& helperUrl) {
  std::string base = helperUrl;
  if (!base.empty() && base.back() == '/') base.pop_back();
  return base + "/api/firmware/version";
}

bool parse_update_manifest(const std::string& json, UpdateInfo& out) {
  return parse_update_manifest(json, ManifestRules(), out);
}

bool parse_update_manifest(const std::string& json, const ManifestRules& rules, UpdateInfo& out) {
  if (json.empty() || json.size() > kManifestMaxBytes) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (o.isNull()) return false;

  UpdateInfo info;
  JsonVariantConst version = o["version"];
  if (!version.is<const char*>() || !parse_version(version.as<const char*>(), info.version)) return false;

  JsonVariantConst url = o["url"];
  if (!url.is<const char*>()) return false;
  info.url = url.as<const char*>();
  if (info.url.size() != strlen(url.as<const char*>()) || !good_url(info.url, rules)) return false;

  JsonVariantConst sha = o["sha256"];
  if (!sha.is<const char*>() || !good_sha(sha.as<const char*>(), info.sha256)) return false;

  JsonVariantConst size = o["size"];
  if (!size.is<long long>()) return false;
  const long long bytes = size.as<long long>();
  if (bytes < kUpdateMinBytes || bytes > rules.maxBytes) return false;
  info.sizeBytes = static_cast<int>(bytes);

  JsonVariantConst notes = o["notes"];
  if (notes.is<const char*>()) {
    info.notes = notes.as<const char*>();
  } else if (!notes.isNull()) {
    return false;
  }
  out = info;
  return true;
}

UpdateDecision decide(const Version& current, const std::string& manifestBody) {
  return decide(current, manifestBody, ManifestRules(), nullptr);
}

UpdateDecision decide(const Version& current, const std::string& manifestBody, const ManifestRules& rules,
                      UpdateInfo* info) {
  UpdateInfo parsed;
  if (!parse_update_manifest(manifestBody, rules, parsed)) return UpdateDecision::Invalid;
  if (!is_newer(parsed.version, current)) return UpdateDecision::UpToDate;
  if (info != nullptr) *info = parsed;
  return UpdateDecision::Available;
}

bool update_power_ok(const PowerStatus& power) {
  if (!power.present) return true;
  if (power.charging) return true;
  return power.percent >= kUpdateMinBatteryPercent;
}

ImageHead check_image_head(const uint8_t* data, size_t size, const char* projectName, const Version& expected) {
  if (data == nullptr || size < kImageHeadBytes) return ImageHead::NeedMore;
  if (data[0] != 0xE9) return ImageHead::NotAnApp;
  if (read_u32(data + 32) != 0xABCD5432u) return ImageHead::NotAnApp;
  if (read_u16(data + 12) != kImageChipEsp32S3) return ImageHead::WrongChip;
  std::string project;
  if (!field(data + 80, 32, project) || project != projectName) return ImageHead::WrongProject;
  std::string versionField;
  Version v;
  if (!field(data + 48, 32, versionField) || !parse_version(versionField, v) || v != expected) {
    return ImageHead::WrongVersion;
  }
  return ImageHead::Ok;
}

const char* update_error_text(UpdateError e) {
  switch (e) {
    case UpdateError::None: return "";
    case UpdateError::Offline: return "No internet connection. Try again later.";
    case UpdateError::CheckFailed: return "Could not check for updates. Try again later.";
    case UpdateError::BadManifest: return "The update information looked wrong. Nothing changed.";
    case UpdateError::LowBattery: return "Battery below 30%. Charge it, then try again.";
    case UpdateError::Unconfirmed: return "Still settling in after the last update. Try in a minute.";
    case UpdateError::LowMemory: return "Too busy right now. Try again in a minute.";
    case UpdateError::DownloadFailed: return "The download stopped. Nothing changed; tap to retry.";
    case UpdateError::Damaged: return "The download was damaged. Nothing changed.";
    case UpdateError::WrongImage: return "That update is not for this display. Nothing changed.";
    case UpdateError::SaveFailed: return "Could not save the update. Nothing changed.";
  }
  return "";
}

std::string update_status_text(const UpdateStatus& s) {
  switch (s.phase) {
    case UpdatePhase::Idle:
      if (!s.notice.empty()) return s.notice;
      return "Version " + version_text(s.current) + " \xC2\xB7 Tap to check";
    case UpdatePhase::Checking: return "Checking for updates...";
    case UpdatePhase::UpToDate:
      if (!s.notice.empty()) return s.notice;
      return "Up to date (" + version_text(s.current) + ")";
    case UpdatePhase::Available: return "Version " + version_text(s.available) + " available \xC2\xB7 Install";
    case UpdatePhase::Preparing: return "Getting ready...";
    case UpdatePhase::Downloading: return "Downloading " + std::to_string(s.percent) + "%";
    case UpdatePhase::Installing: return "Installing...";
    case UpdatePhase::Restarting: return "Restarting...";
    case UpdatePhase::Failed: return update_error_text(s.error);
  }
  return "";
}

bool update_status_problem(const UpdateStatus& s) {
  if (s.phase == UpdatePhase::Failed) return true;
  return (s.phase == UpdatePhase::Idle || s.phase == UpdatePhase::UpToDate) && !s.notice.empty() &&
         s.noticeIsProblem;
}

bool update_status_busy(const UpdateStatus& s) {
  return s.phase == UpdatePhase::Preparing || s.phase == UpdatePhase::Downloading ||
         s.phase == UpdatePhase::Installing || s.phase == UpdatePhase::Restarting;
}
