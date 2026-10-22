// helper_config: the checks the firmware applies to the helper URL and token
// before it stores or uses them, and the masked token it may print.
#include <doctest/doctest.h>

#include <string>

#include "helper_config.h"

TEST_CASE("helper url: https with a host is accepted") {
  CHECK(valid_helper_url("https://soccer-display-helper.vercel.app"));
  CHECK(valid_helper_url("https://soccer-display-helper.vercel.app/"));
  CHECK(valid_helper_url("https://example.com:8443"));
  CHECK(valid_helper_url("https://example.com/base/path"));
  CHECK(valid_helper_url("https://192.168.0.10"));
  CHECK(valid_helper_url("https://a"));
}

TEST_CASE("helper url: anything but https is refused") {
  CHECK_FALSE(valid_helper_url(""));
  CHECK_FALSE(valid_helper_url("http://example.com"));
  CHECK_FALSE(valid_helper_url("HTTPS://example.com"));  // written exactly as https://
  CHECK_FALSE(valid_helper_url("ftp://example.com"));
  CHECK_FALSE(valid_helper_url("example.com"));
  CHECK_FALSE(valid_helper_url("https:/example.com"));
  CHECK_FALSE(valid_helper_url(" https://example.com"));
}

TEST_CASE("helper url: the host must not be empty") {
  CHECK_FALSE(valid_helper_url("https://"));
  CHECK_FALSE(valid_helper_url("https:///api"));
  CHECK_FALSE(valid_helper_url("https://:443"));
  CHECK_FALSE(valid_helper_url("https://example.com:"));  // empty port
  CHECK_FALSE(valid_helper_url("https://example.com:44x"));
}

TEST_CASE("helper url: no user info, no query or fragment, no control characters or spaces") {
  CHECK_FALSE(valid_helper_url("https://user@example.com"));
  CHECK_FALSE(valid_helper_url("https://user:pw@example.com"));
  CHECK_FALSE(valid_helper_url("https://example.com/a@b"));  // '@' anywhere is suspicious
  CHECK_FALSE(valid_helper_url("https://example.com?x=1"));
  CHECK_FALSE(valid_helper_url("https://example.com/#top"));
  CHECK_FALSE(valid_helper_url("https://exa mple.com"));
  CHECK_FALSE(valid_helper_url("https://example.com\n"));
  CHECK_FALSE(valid_helper_url("https://example.com\r"));
  CHECK_FALSE(valid_helper_url("https://example.com\t"));
  CHECK_FALSE(valid_helper_url(std::string("https://example.com\0x", 21)));
  CHECK_FALSE(valid_helper_url("https://example.com\x7f"));
  CHECK_FALSE(valid_helper_url("https://ex\xC3\xA4mple.com"));  // non-ASCII bytes
  CHECK_FALSE(valid_helper_url("https://example.com\\evil"));
}

TEST_CASE("helper url: at most 200 characters") {
  const std::string head = "https://";
  const std::string at200 = head + std::string(200 - head.size(), 'a');
  REQUIRE(at200.size() == 200);
  CHECK(valid_helper_url(at200));
  CHECK_FALSE(valid_helper_url(at200 + "a"));
  CHECK(kHelperUrlMaxLen == 200);
}

TEST_CASE("helper token: 16 to 128 printable ASCII characters") {
  CHECK(valid_token(std::string(16, 'a')));
  CHECK(valid_token(std::string(128, 'Z')));
  CHECK(valid_token("abcDEF0123456789-_.~+/=!"));
  CHECK_FALSE(valid_token(""));
  CHECK_FALSE(valid_token(std::string(15, 'a')));
  CHECK_FALSE(valid_token(std::string(129, 'a')));
  CHECK(kTokenMinLen == 16);
  CHECK(kTokenMaxLen == 128);
}

TEST_CASE("helper token: control characters, spaces and non-ASCII bytes are refused") {
  const std::string base(20, 'a');
  for (int c = 0; c < 0x20; ++c) {
    CHECK_FALSE(valid_token(base + static_cast<char>(c)));
    CHECK_FALSE(valid_token(static_cast<char>(c) + base));
  }
  CHECK_FALSE(valid_token(base + '\x7f'));
  CHECK_FALSE(valid_token(base + ' '));
  CHECK_FALSE(valid_token("aaaaaaaa aaaaaaaa"));
  CHECK_FALSE(valid_token(base + "\xC3\xA4"));
  CHECK_FALSE(valid_token(base + '\xff'));
}

TEST_CASE("helper token: the masked form shows at most the first 4 characters") {
  const std::string token = "abcdEFGHijklMNOPqrst";
  const std::string m = masked_token(token);
  CHECK(m.find("abcd") == 0);
  CHECK(m.find("EFGH") == std::string::npos);
  CHECK(m.find("E") == std::string::npos);
  CHECK(m.find("qrst") == std::string::npos);
  // No 5-character run of the token appears anywhere.
  for (size_t i = 0; i + 5 <= token.size(); ++i) CHECK(m.find(token.substr(i, 5)) == std::string::npos);
  CHECK(masked_token("") == "(none)");
  // Short (invalid) values show nothing of themselves.
  CHECK(masked_token("abc") == "(invalid)");
  CHECK(masked_token(std::string(15, 'x')) == "(invalid)");
  CHECK(masked_token(std::string(20, 'x') + "\n").find('\n') == std::string::npos);
  CHECK(masked_token(std::string(20, 'x') + "\n") == "(invalid)");
}
