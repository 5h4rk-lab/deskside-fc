// A static guard for the secrets rule: no log or serial call in the Wi-Fi code
// may mention a password, and only the connect code may read one from the
// store. Reads the sources, so it fails the moment someone adds a log line
// such as Serial.printf("pw=%s", password).
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifndef REPO_ROOT_DIR
#error "REPO_ROOT_DIR must name the repository root"
#endif

namespace {

std::string read_file(const std::string& rel) {
  std::ifstream in(std::string(REPO_ROOT_DIR) + "/" + rel, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Every statement (from the call to its ';') that starts with a logging call.
std::vector<std::string> log_statements(const std::string& src) {
  static const char* const kCalls[] = {"Serial.print", "Serial.write", "say(", "log_(", "log_e(", "log_w(",
                                       "log_i(", "log_d(", "ESP_LOG", "printf(", "puts(", "fprintf(", "wlog("};
  std::vector<std::string> out;
  for (const char* call : kCalls) {
    size_t pos = 0;
    while ((pos = src.find(call, pos)) != std::string::npos) {
      const size_t end = src.find(';', pos);
      out.push_back(src.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
      pos += 1;
    }
  }
  return out;
}

// Words that must never appear in a logging statement, in code or in text.
bool mentions_secret(const std::string& statement) {
  static const char* const kBanned[] = {"password", "passwd", "passphrase", "psk", "secret", "serialize", "password_of"};
  const std::string s = lower(statement);
  for (const char* w : kBanned) {
    if (s.find(w) != std::string::npos) return true;
  }
  // "pw" as a word of its own (pw, .pw, m_pw), but not inside another word.
  size_t pos = 0;
  while ((pos = s.find("pw", pos)) != std::string::npos) {
    const bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(s[pos - 1]));
    const bool rightOk = pos + 2 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[pos + 2]));
    if (leftOk && rightOk) return true;
    pos += 2;
  }
  return false;
}

const char* const kWifiSources[] = {
    "firmware/app/src/platform/esp_wifi_service.cpp", "firmware/app/src/platform/esp_wifi_service.h",
    "ui/src/wifi_controller.cpp",                    "ui/src/wifi_controller.h",
    "ui/src/wifi_store.cpp",                         "ui/src/wifi_store.h",
};

}  // namespace

TEST_CASE("the scanner itself flags what it should") {
  CHECK(mentions_secret("Serial.printf(\"pw=%s\", password.c_str())"));
  CHECK(mentions_secret("say(\"[wifi] joining with \" + pw)"));
  CHECK(mentions_secret("Serial.println(store_.serialize())"));
  CHECK(mentions_secret("log_(\"wrong Password\")"));
  CHECK_FALSE(mentions_secret("Serial.printf(\"[wifi] joined %s rssi=%d ip=%s\", ssid, rssi, ip)"));
  CHECK_FALSE(mentions_secret("say(\"[wifi] scan: \" + std::to_string(n) + \" network(s)\")"));
  const std::vector<std::string> found =
      log_statements("void f() { Serial.printf(\"a %s\", x); say(\"b\"); int y = 1; }");
  CHECK(found.size() >= 2);
}

TEST_CASE("no log or serial call in the Wi-Fi code mentions a password") {
  size_t checked = 0;
  for (const char* rel : kWifiSources) {
    const std::string src = read_file(rel);
    INFO("file " << rel);
    REQUIRE_FALSE(src.empty());
    for (const std::string& st : log_statements(src)) {
      INFO("statement: " << st);
      CHECK_FALSE(mentions_secret(st));
      ++checked;
    }
  }
  CHECK(checked > 20);  // the scan found the log calls at all
}

TEST_CASE("only the connect code reads a stored password") {
  // password_of() is called by the controller (the connect code) and defined
  // by the store; nothing else may use it.
  const std::vector<std::string> others = {
      "firmware/app/src/main.cpp",
      "firmware/app/src/app_host.cpp",
      "firmware/app/src/platform/esp_wifi_service.cpp",
      "firmware/app/src/platform/nvs_prefs.cpp",
      "ui/src/app.cpp",
      "ui/src/screens/wifi.cpp",
      "ui/src/screens/settings.cpp",
  };
  for (const std::string& rel : others) {
    INFO("file " << rel);
    const std::string src = read_file(rel);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("password_of") == std::string::npos);
  }
  const std::string controller = read_file("ui/src/wifi_controller.cpp");
  CHECK(controller.find("password_of") != std::string::npos);
}

TEST_CASE("the serial console has no command that prints saved networks' passwords") {
  const std::string main = lower(read_file("firmware/app/src/main.cpp"));
  const std::string host = lower(read_file("firmware/app/src/app_host.cpp"));
  const std::string svc = lower(read_file("firmware/app/src/platform/esp_wifi_service.cpp"));
  for (const std::string* src : {&main, &host, &svc}) {
    for (const std::string& st : log_statements(*src)) {
      INFO("statement: " << st);
      CHECK_FALSE(mentions_secret(st));
    }
  }
}
