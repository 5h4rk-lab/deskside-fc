#include "helper_config.h"

namespace {

constexpr const char* kScheme = "https://";
constexpr size_t kSchemeLen = 8;

// Printable ASCII without the space.
bool visible_ascii(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u >= 0x21 && u <= 0x7e;
}

}  // namespace

bool valid_helper_url(const std::string& url) {
  if (url.size() > kHelperUrlMaxLen || url.compare(0, kSchemeLen, kScheme) != 0) return false;
  for (char c : url) {
    if (!visible_ascii(c) || c == '@' || c == '?' || c == '#' || c == '\\') return false;
  }
  // Authority: up to the first '/' after the scheme.
  const size_t end = url.find('/', kSchemeLen);
  const std::string authority = url.substr(kSchemeLen, end == std::string::npos ? std::string::npos : end - kSchemeLen);
  const size_t colon = authority.find(':');
  const std::string host = authority.substr(0, colon);
  if (host.empty()) return false;
  if (colon != std::string::npos) {
    const std::string port = authority.substr(colon + 1);
    if (port.empty() || port.size() > 5) return false;
    for (char c : port) {
      if (c < '0' || c > '9') return false;
    }
  }
  return true;
}

bool valid_token(const std::string& token) {
  if (token.size() < kTokenMinLen || token.size() > kTokenMaxLen) return false;
  for (char c : token) {
    if (!visible_ascii(c)) return false;
  }
  return true;
}

std::string masked_token(const std::string& token) {
  if (token.empty()) return "(none)";
  if (!valid_token(token)) return "(invalid)";
  return token.substr(0, 4) + "... (" + std::to_string(token.size()) + " characters)";
}
