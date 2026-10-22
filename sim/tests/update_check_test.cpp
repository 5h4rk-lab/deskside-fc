// update_check: versions, the strict manifest, the decision, the battery rule,
// the image head check and the Settings row texts.
#include <doctest/doctest.h>

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "update_check.h"
#include "version.h"

namespace {

Version V(int a, int b, int c) {
  Version v;
  v.major = a;
  v.minor = b;
  v.patch = c;
  return v;
}

const std::string kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string manifest(const std::string& version = "0.1.1",
                     const std::string& url = "https://soccer-display-helper.vercel.app/firmware/0.1.1.bin",
                     const std::string& sha = kHash, const std::string& size = "1634512",
                     const std::string& extra = "") {
  return "{\"version\":\"" + version + "\",\"url\":\"" + url + "\",\"sha256\":\"" + sha + "\",\"size\":" + size +
         extra + "}";
}

}  // namespace

TEST_CASE("version: parse accepts MAJOR.MINOR.PATCH") {
  Version v;
  REQUIRE(parse_version("0.1.0", v));
  CHECK(v == V(0, 1, 0));
  REQUIRE(parse_version("1.0.0", v));
  CHECK(v == V(1, 0, 0));
  REQUIRE(parse_version("12.345.9999", v));
  CHECK(v == V(12, 345, 9999));
}

TEST_CASE("version: parse refuses anything else and leaves the output alone") {
  const char* bad[] = {"",       "1",       "1.2",    "1.2.3.4", "v1.2.3", "1.2.3-beta", " 1.2.3", "1.2.3 ",
                       "01.2.3", "1.02.3",  "1.2.03", "1..3",    "1.2.",   ".1.2",       "-1.2.3", "+1.2.3",
                       "1.2.x",  "10000.0.0", "1.2.3\n", "1,2,3"};
  for (const char* s : bad) {
    Version v = V(7, 7, 7);
    CHECK_MESSAGE(!parse_version(s, v), s);
    CHECK(v == V(7, 7, 7));
  }
  Version v = V(7, 7, 7);
  CHECK_FALSE(parse_version(std::string("1.2\0.3", 6), v));
}

TEST_CASE("version: text and ordering") {
  CHECK(version_text(V(0, 1, 0)) == "0.1.0");
  CHECK(version_text(V(10, 0, 123)) == "10.0.123");
  CHECK(is_newer(V(0, 1, 1), V(0, 1, 0)));
  CHECK(is_newer(V(0, 2, 0), V(0, 1, 9)));
  CHECK(is_newer(V(1, 0, 0), V(0, 99, 99)));
  CHECK_FALSE(is_newer(V(0, 1, 0), V(0, 1, 0)));
  CHECK_FALSE(is_newer(V(0, 1, 0), V(0, 1, 1)));
  CHECK_FALSE(is_newer(V(0, 9, 9), V(1, 0, 0)));
}

TEST_CASE("version: the app's own version parses and is 0.1.0") {
  Version v;
  REQUIRE(parse_version(APP_VERSION_TEXT, v));
  CHECK(v == V(0, 1, 0));
  CHECK(std::string(kFirmwareProjectName) == "soccer-display");
}

TEST_CASE("manifest: a good one parses") {
  UpdateInfo info;
  REQUIRE(parse_update_manifest(manifest(), info));
  CHECK(info.version == V(0, 1, 1));
  CHECK(info.url == "https://soccer-display-helper.vercel.app/firmware/0.1.1.bin");
  CHECK(info.sha256 == kHash);
  CHECK(info.sizeBytes == 1634512);
  CHECK(info.notes.empty());

  REQUIRE(parse_update_manifest(manifest("0.2.0", "https://soccer-display-helper.vercel.app/firmware/0.2.0.bin", kHash,
                                         "200000", ",\"notes\":\"Faster pictures\",\"extra\":[1,2]"),
                                info));
  CHECK(info.notes == "Faster pictures");
  REQUIRE(parse_update_manifest(manifest("0.2.0", "https://soccer-display-helper.vercel.app/x.bin", kHash, "200000",
                                         ",\"notes\":null"),
                                info));
  CHECK(info.notes.empty());
}

TEST_CASE("manifest: the sha256 must be 64 hex characters, kept in lower case") {
  UpdateInfo info;
  std::string upper = kHash;
  for (char& c : upper) c = static_cast<char>(toupper(c));
  REQUIRE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin", upper), info));
  CHECK(info.sha256 == kHash);
  UpdateInfo untouched;
  untouched.url = "keep";
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin", kHash.substr(1)), untouched));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin", kHash + "0"), untouched));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin", "g" + kHash.substr(1)), untouched));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin", ""), untouched));
  CHECK(untouched.url == "keep");
}

TEST_CASE("manifest: the image must be https on the helper's own host") {
  UpdateInfo info;
  const char* bad[] = {
      "http://soccer-display-helper.vercel.app/f.bin",       // not https
      "https://evil.example.com/f.bin",                       // another host
      "https://soccer-display-helper.vercel.app.evil.com/f.bin",
      "https://evil.com/soccer-display-helper.vercel.app/f.bin",
      "https://soccer-display-helper.vercel.app:8443/f.bin",  // another port
      "https://user@soccer-display-helper.vercel.app/f.bin",  // user info
      "https://soccer-display-helper.vercel.app",             // no path
      "https://soccer-display-helper.vercel.app/",            // nothing to download
      "https://soccer-display-helper.vercel.app/f.bin?x=1",   // query
      "https://soccer-display-helper.vercel.app/f.bin#a",     // fragment
      "https://soccer-display-helper.vercel.app/f bin",       // space
      "https://soccer-display-helper.vercel.app/f\\bin",      // backslash
      "ftp://soccer-display-helper.vercel.app/f.bin",
      "soccer-display-helper.vercel.app/f.bin",
      "",
  };
  for (const char* u : bad) CHECK_MESSAGE(!parse_update_manifest(manifest("0.1.1", u), info), u);
  // Host case does not matter.
  CHECK(parse_update_manifest(manifest("0.1.1", "https://Soccer-Display-Helper.vercel.app/f.bin"), info));
  // Too long.
  CHECK_FALSE(parse_update_manifest(
      manifest("0.1.1", "https://soccer-display-helper.vercel.app/" + std::string(300, 'a')), info));
  // A control character (JSON escape) in the URL.
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/a\\u0001b"), info));
}

TEST_CASE("manifest: rules from another helper address, and http only when allowed") {
  UpdateInfo info;
  ManifestRules r = rules_for_manifest_url("https://my-helper.example.org/api/firmware/version", false);
  CHECK(r.host == "my-helper.example.org");
  CHECK_FALSE(r.allowHttp);
  CHECK(parse_update_manifest(manifest("0.1.1", "https://my-helper.example.org/firmware/0.1.1.bin"), r, info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin"), r, info));

  // A local test server (diagnostics builds only): http on that host and port.
  ManifestRules local = rules_for_manifest_url("http://192.168.0.20:8000/manifest.json", true);
  CHECK(local.host == "192.168.0.20:8000");
  CHECK(local.allowHttp);
  CHECK(parse_update_manifest(manifest("0.1.1", "http://192.168.0.20:8000/0.1.1.bin"), local, info));
  CHECK(parse_update_manifest(manifest("0.1.1", "https://192.168.0.20:8000/0.1.1.bin"), local, info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "http://192.168.0.21:8000/0.1.1.bin"), local, info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "http://192.168.0.20/0.1.1.bin"), local, info));

  // allowHttp is never given to an https manifest, nor without the flag.
  CHECK_FALSE(rules_for_manifest_url("https://192.168.0.20:8000/m.json", true).allowHttp);
  CHECK_FALSE(rules_for_manifest_url("http://192.168.0.20:8000/m.json", false).allowHttp);
}

TEST_CASE("manifest: url_host and the manifest URL of a helper") {
  CHECK(url_host("https://Example.COM/a/b") == "example.com");
  CHECK(url_host("https://example.com:8443") == "example.com:8443");
  CHECK(url_host("http://10.0.0.1:8000/x") == "10.0.0.1:8000");
  CHECK(url_host("ftp://example.com/x").empty());
  CHECK(url_host("https:///x").empty());
  CHECK(url_host("example.com").empty());
  CHECK(manifest_url_for_helper("https://h.example") == "https://h.example/api/firmware/version");
  CHECK(manifest_url_for_helper("https://h.example/") == "https://h.example/api/firmware/version");
  CHECK(manifest_url_for_helper("https://h.example/base") == "https://h.example/base/api/firmware/version");
}

TEST_CASE("manifest: size between 100 KB and the slot") {
  UpdateInfo info;
  const std::string url = "https://soccer-display-helper.vercel.app/f.bin";
  CHECK(parse_update_manifest(manifest("0.1.1", url, kHash, std::to_string(100 * 1024)), info));
  CHECK(info.sizeBytes == 102400);
  CHECK(parse_update_manifest(manifest("0.1.1", url, kHash, std::to_string(0x500000)), info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", url, kHash, std::to_string(100 * 1024 - 1)), info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", url, kHash, std::to_string(0x500000 + 1)), info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", url, kHash, "-5"), info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", url, kHash, "1634512.5"), info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", url, kHash, "\"1634512\""), info));
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", url, kHash, "99999999999999999999"), info));
  ManifestRules small;
  small.maxBytes = 200000;
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", url, kHash, "200001"), small, info));
}

TEST_CASE("manifest: missing or mistyped fields and broken JSON are refused") {
  UpdateInfo info;
  const char* bad[] = {
      "",
      "null",
      "[]",
      "\"text\"",
      "{",
      "{}",
      "{\"url\":\"https://soccer-display-helper.vercel.app/f.bin\",\"sha256\":\"x\",\"size\":200000}",
      "{\"version\":\"0.1.1\",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"size\":200000}",
      "{\"version\":\"0.1.1\",\"url\":\"https://soccer-display-helper.vercel.app/f.bin\",\"size\":200000}",
      "{\"version\":\"0.1.1\",\"url\":\"https://soccer-display-helper.vercel.app/f.bin\",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
      "{\"version\":011,\"url\":\"https://soccer-display-helper.vercel.app/f.bin\",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"size\":200000}",
  };
  for (const char* b : bad) CHECK_MESSAGE(!parse_update_manifest(b, info), b);
  CHECK_FALSE(parse_update_manifest(manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin", kHash, "200000",
                                             ",\"notes\":5"),
                                    info));
  CHECK_FALSE(parse_update_manifest(manifest("1.2"), info));
  // Larger than kManifestMaxBytes.
  CHECK_FALSE(parse_update_manifest(
      manifest("0.1.1", "https://soccer-display-helper.vercel.app/f.bin", kHash, "200000",
               ",\"notes\":\"" + std::string(kManifestMaxBytes, 'a') + "\""),
      info));
}

TEST_CASE("decide: newer is Available, same or older is UpToDate, broken is Invalid") {
  const Version cur = V(0, 1, 0);
  CHECK(decide(cur, manifest("0.1.1")) == UpdateDecision::Available);
  CHECK(decide(cur, manifest("0.1.0")) == UpdateDecision::UpToDate);
  CHECK(decide(cur, manifest("0.0.9")) == UpdateDecision::UpToDate);  // never a downgrade
  CHECK(decide(cur, "{oops") == UpdateDecision::Invalid);
  CHECK(decide(cur, manifest("0.1.1", "https://evil.example.com/f.bin")) == UpdateDecision::Invalid);

  UpdateInfo info;
  ManifestRules r;
  CHECK(decide(cur, manifest("0.2.0"), r, &info) == UpdateDecision::Available);
  CHECK(info.version == V(0, 2, 0));
  UpdateInfo kept;
  kept.notes = "kept";
  CHECK(decide(cur, manifest("0.1.0"), r, &kept) == UpdateDecision::UpToDate);
  CHECK(kept.notes == "kept");
  CHECK(decide(cur, manifest("0.2.0"), r, nullptr) == UpdateDecision::Available);
}

TEST_CASE("battery rule: 30 percent, or charging, or no battery") {
  PowerStatus p;
  p.present = true;
  p.percent = 30;
  CHECK(update_power_ok(p));
  p.percent = 29;
  CHECK_FALSE(update_power_ok(p));
  p.percent = 5;
  CHECK_FALSE(update_power_ok(p));
  p.charging = true;  // charging: the charger (USB) is in
  CHECK(update_power_ok(p));
  p.charging = false;
  p.present = false;  // no battery (or the setting None): the board runs from USB
  CHECK(update_power_ok(p));
  p.present = true;
  p.percent = 100;
  CHECK(update_power_ok(p));
  CHECK(kUpdateMinBatteryPercent == 30);
}

namespace {

// A plausible image head: header, segment header, app descriptor.
std::vector<uint8_t> image_head(const char* project, const char* version, uint16_t chip = kImageChipEsp32S3) {
  std::vector<uint8_t> b(kImageHeadBytes + 50, 0);
  b[0] = 0xE9;
  b[1] = 5;
  b[12] = static_cast<uint8_t>(chip & 0xFF);
  b[13] = static_cast<uint8_t>(chip >> 8);
  const uint32_t magic = 0xABCD5432;
  memcpy(&b[32], &magic, 4);
  strncpy(reinterpret_cast<char*>(&b[48]), version, 32);
  strncpy(reinterpret_cast<char*>(&b[80]), project, 32);
  return b;
}

}  // namespace

TEST_CASE("image head: ours, for the S3, of the announced version") {
  std::vector<uint8_t> b = image_head("soccer-display", "0.1.1");
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::Ok);
  CHECK(check_image_head(b.data(), kImageHeadBytes, "soccer-display", V(0, 1, 1)) == ImageHead::Ok);
  CHECK(check_image_head(b.data(), kImageHeadBytes - 1, "soccer-display", V(0, 1, 1)) == ImageHead::NeedMore);
  CHECK(check_image_head(b.data(), 0, "soccer-display", V(0, 1, 1)) == ImageHead::NeedMore);
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 2)) == ImageHead::WrongVersion);
}

TEST_CASE("image head: foreign, broken or other-chip images are refused") {
  std::vector<uint8_t> b = image_head("arduino-lib-builder", "0.1.1");
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::WrongProject);
  b = image_head("soccer-display-x", "0.1.1");
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::WrongProject);
  b = image_head("soccer-display", "0.1.1", 0);  // ESP32
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::WrongChip);
  b = image_head("soccer-display", "0.1.1");
  b[0] = 0x00;
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::NotAnApp);
  b = image_head("soccer-display", "0.1.1");
  b[32] = 0;  // no descriptor magic
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::NotAnApp);
  b = image_head("soccer-display", "garbage");
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::WrongVersion);
  // A version field without a terminating NUL is not a version.
  b = image_head("soccer-display", "0.1.1");
  memset(&b[48], '1', 32);
  CHECK(check_image_head(b.data(), b.size(), "soccer-display", V(0, 1, 1)) == ImageHead::WrongVersion);
}

TEST_CASE("row texts") {
  UpdateStatus s;
  s.current = V(0, 1, 0);
  s.phase = UpdatePhase::Idle;
  CHECK(update_status_text(s) == "Version 0.1.0 \xC2\xB7 Tap to check");
  s.phase = UpdatePhase::Checking;
  CHECK(update_status_text(s) == "Checking for updates...");
  s.phase = UpdatePhase::UpToDate;
  CHECK(update_status_text(s) == "Up to date (0.1.0)");
  s.phase = UpdatePhase::Available;
  s.available = V(0, 1, 1);
  CHECK(update_status_text(s) == "Version 0.1.1 available \xC2\xB7 Install");
  CHECK_FALSE(update_status_busy(s));
  s.phase = UpdatePhase::Preparing;
  CHECK(update_status_text(s) == "Getting ready...");
  CHECK(update_status_busy(s));
  s.phase = UpdatePhase::Downloading;
  s.percent = 42;
  CHECK(update_status_text(s) == "Downloading 42%");
  CHECK(update_status_busy(s));
  s.phase = UpdatePhase::Installing;
  CHECK(update_status_text(s) == "Installing...");
  s.phase = UpdatePhase::Restarting;
  CHECK(update_status_text(s) == "Restarting...");
  CHECK_FALSE(update_status_problem(s));
  s.phase = UpdatePhase::Failed;
  s.error = UpdateError::LowBattery;
  CHECK(update_status_text(s) == update_error_text(UpdateError::LowBattery));
  CHECK(update_status_problem(s));
  CHECK_FALSE(update_status_busy(s));
}

TEST_CASE("row texts: a notice from the last start shows while idle or up to date") {
  UpdateStatus s;
  s.current = V(0, 1, 1);
  s.notice = "Updated to 0.1.1";
  s.phase = UpdatePhase::Idle;
  CHECK(update_status_text(s) == "Updated to 0.1.1");
  CHECK_FALSE(update_status_problem(s));
  s.phase = UpdatePhase::UpToDate;
  CHECK(update_status_text(s) == "Updated to 0.1.1");
  s.phase = UpdatePhase::Available;
  s.available = V(0, 1, 2);
  CHECK(update_status_text(s) == "Version 0.1.2 available \xC2\xB7 Install");
  s.phase = UpdatePhase::Idle;
  s.noticeIsProblem = true;
  CHECK(update_status_problem(s));
}

TEST_CASE("error texts: plain English, every error has one, short enough for two lines") {
  const UpdateError all[] = {UpdateError::Offline,     UpdateError::CheckFailed, UpdateError::BadManifest,
                             UpdateError::LowBattery,  UpdateError::Unconfirmed, UpdateError::LowMemory,
                             UpdateError::DownloadFailed, UpdateError::Damaged,  UpdateError::WrongImage,
                             UpdateError::SaveFailed};
  for (UpdateError e : all) {
    const std::string t = update_error_text(e);
    CHECK(!t.empty());
    CHECK(t.size() <= 64);
    CHECK(t.find("error") == std::string::npos);  // no jargon
    CHECK(t.find("ESP_") == std::string::npos);
  }
  CHECK(std::string(update_error_text(UpdateError::LowBattery)).find("30%") != std::string::npos);
  CHECK(std::string(update_error_text(UpdateError::Damaged)).find("Nothing changed") != std::string::npos);
}
