// A static guard for the helper token: no log or serial statement in the code
// that handles it may print it. Outside string literals, a logging statement
// may not name anything called "token" (the masked form, masked_token(), is
// the one exception), and :set-token's handler prints nothing of its value.
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
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

std::vector<std::string> log_statements(const std::string& src) {
  static const char* const kCalls[] = {"Serial.print", "Serial.write", "log_e(", "log_w(", "log_i(", "log_d(",
                                       "ESP_LOG", "printf(", "puts(", "fprintf("};
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

// The statement without its string literals (the code that supplies values).
std::string code_part(const std::string& st) {
  std::string out;
  bool inString = false;
  for (size_t i = 0; i < st.size(); ++i) {
    const char c = st[i];
    if (inString) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
      continue;
    }
    out += c;
  }
  return out;
}

bool prints_token(const std::string& statement) {
  std::string code = lower(code_part(statement));
  // masked_token(<anything>) as a whole, up to its closing parenthesis.
  for (size_t p; (p = code.find("masked_token(")) != std::string::npos;) {
    const size_t close = code.find(')', p);
    code.erase(p, close == std::string::npos ? std::string::npos : close + 1 - p);
  }
  return code.find("token") != std::string::npos;
}

// The body of the function whose definition starts with `head`.
std::string function_body(const std::string& src, const std::string& head) {
  const size_t start = src.find(head);
  if (start == std::string::npos) return "";
  const size_t end = src.find("\n}\n", start);
  return src.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

const char* const kSources[] = {
    "firmware/app/src/main.cpp",
    "firmware/app/src/app_host.cpp",
    "firmware/app/src/platform/esp_http_transport.cpp",
    "firmware/app/src/platform/esp_http_transport.h",
    "firmware/app/src/platform/time_sync.cpp",
    "firmware/app/src/platform/nvs_prefs.cpp",
    "ui/src/http_source.cpp",
    "ui/src/helper_config.cpp",
};

}  // namespace

TEST_CASE("helper secrets scanner: flags what it should") {
  CHECK(prints_token("Serial.printf(\"t=%s\", token.c_str())"));
  CHECK(prints_token("Serial.printf(\"t=%s\", job->token.c_str())"));
  CHECK(prints_token("Serial.printf(\"%s\", helper.TOKEN)"));
  CHECK_FALSE(prints_token("Serial.printf(\"token ok\")"));
  CHECK_FALSE(prints_token("Serial.printf(\"token %s\", masked_token(token).c_str())"));
  CHECK_FALSE(prints_token("Serial.printf(\"[cmd] helper token \\\"saved\\\" (%u characters)\", n)"));
}

TEST_CASE("no log or serial statement prints the helper token") {
  size_t checked = 0;
  for (const char* rel : kSources) {
    const std::string src = read_file(rel);
    INFO("file " << rel);
    REQUIRE_FALSE(src.empty());
    for (const std::string& st : log_statements(src)) {
      INFO("statement: " << st);
      CHECK_FALSE(prints_token(st));
      ++checked;
    }
  }
  CHECK(checked > 40);
}

TEST_CASE(":set-token prints nothing of its value") {
  const std::string body = function_body(read_file("firmware/app/src/main.cpp"), "void setHelperToken(");
  REQUIRE_FALSE(body.empty());
  size_t logs = 0;
  for (const std::string& st : log_statements(body)) {
    INFO("statement: " << st);
    CHECK(lower(code_part(st)).find("value") == std::string::npos);
    ++logs;
  }
  CHECK(logs >= 2);  // the refusal and the confirmation
}

TEST_CASE("an unknown serial word is never echoed with its value") {
  const std::string body = function_body(read_file("firmware/app/src/main.cpp"), "void handleSerialItem(");
  REQUIRE_FALSE(body.empty());
  for (const std::string& st : log_statements(body)) {
    INFO("statement: " << st);
    const std::string code = lower(code_part(st));
    CHECK(code.find("value") == std::string::npos);
    CHECK(code.find("it.word") == std::string::npos);
  }
}

TEST_CASE("the release build carries no token: secrets.ini is absent and not required") {
  std::ifstream secrets(std::string(REPO_ROOT_DIR) + "/firmware/app/secrets.ini");
  CHECK_FALSE(secrets.good());
  const std::string ini = read_file("firmware/app/platformio.ini");
  CHECK(ini.find("HELPER_TOKEN=") == std::string::npos);
}
