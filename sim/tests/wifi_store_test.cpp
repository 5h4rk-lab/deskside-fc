// WifiStore: the saved-network list the firmware keeps in one NVS blob.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "wifi_store.h"

namespace {

WifiNetwork net(const char* ssid, int rssi, bool open = false) {
  WifiNetwork n;
  n.ssid = ssid;
  n.rssi = rssi;
  n.open = open;
  return n;
}

std::string pw(const WifiStore& s, const std::string& ssid) {
  std::string out = "untouched";
  if (!s.password_of(ssid, out)) return "<none>";
  return out;
}

const std::string kSsid32 = "Stadium-Fan-Zone-WiFi-Network-32";  // exactly 32 bytes
const std::string kPass63(63, 'p');

// A store holding "n0".."n<count-1>", n0 the most recently used.
WifiStore filled(size_t count) {
  WifiStore s;
  for (size_t i = count; i-- > 0;) {
    s.add_or_replace("n" + std::to_string(i), "password" + std::to_string(i));
  }
  return s;
}

}  // namespace

TEST_CASE("WifiStore starts empty") {
  WifiStore s;
  CHECK(s.size() == 0);
  CHECK(s.ssids().empty());
  CHECK_FALSE(s.contains("x"));
  CHECK(pw(s, "x") == "<none>");
  CHECK(s.serialize().empty());
}

TEST_CASE("WifiStore keeps the most recently used first") {
  WifiStore s;
  CHECK(s.add_or_replace("one", "password1"));
  CHECK(s.add_or_replace("two", "password2"));
  CHECK(s.add_or_replace("three", "password3"));
  CHECK(s.ssids() == std::vector<std::string>{"three", "two", "one"});
  CHECK(s.touch("one"));
  CHECK(s.ssids() == std::vector<std::string>{"one", "three", "two"});
  CHECK(s.touch("one"));  // already first
  CHECK(s.ssids() == std::vector<std::string>{"one", "three", "two"});
  CHECK_FALSE(s.touch("unknown"));
  CHECK(s.ssids() == std::vector<std::string>{"one", "three", "two"});
}

TEST_CASE("WifiStore: the same SSID replaces the entry and moves to the front") {
  WifiStore s;
  s.add_or_replace("home", "oldpassword");
  s.add_or_replace("work", "workpassword");
  CHECK(s.add_or_replace("home", "newpassword"));
  CHECK(s.size() == 2);
  CHECK(s.ssids() == std::vector<std::string>{"home", "work"});
  CHECK(pw(s, "home") == "newpassword");
  CHECK(pw(s, "work") == "workpassword");
}

TEST_CASE("WifiStore: SSIDs are case sensitive and compared byte for byte") {
  WifiStore s;
  s.add_or_replace("Home", "password1");
  s.add_or_replace("home", "password2");
  CHECK(s.size() == 2);
  CHECK(pw(s, "Home") == "password1");
  CHECK(pw(s, "home") == "password2");
}

TEST_CASE("WifiStore: a full store drops the least recently used entry") {
  WifiStore s = filled(WifiStore::kMaxSaved);
  REQUIRE(s.size() == 16);
  CHECK(s.ssids().front() == "n0");
  CHECK(s.ssids().back() == "n15");
  CHECK(s.add_or_replace("fresh", "freshpassword"));
  CHECK(s.size() == 16);
  CHECK(s.ssids().front() == "fresh");
  CHECK_FALSE(s.contains("n15"));  // the oldest went
  CHECK(s.contains("n14"));
  // Using an old one protects it: touch n14, then add another; n13 goes, not n14.
  s.touch("n14");
  s.add_or_replace("fresher", "fresherpassword");
  CHECK(s.contains("n14"));
  CHECK_FALSE(s.contains("n13"));
  CHECK(s.size() == 16);
}

TEST_CASE("WifiStore: replacing an entry of a full store drops nothing") {
  WifiStore s = filled(16);
  CHECK(s.add_or_replace("n7", "changedpassword"));
  CHECK(s.size() == 16);
  CHECK(s.ssids().front() == "n7");
  for (int i = 0; i < 16; ++i) CHECK(s.contains("n" + std::to_string(i)));
  CHECK(pw(s, "n7") == "changedpassword");
}

TEST_CASE("WifiStore never drops an entry while it holds fewer than kMinSavedNetworks") {
  WifiStore s;
  for (size_t i = 0; i < kMinSavedNetworks; ++i) {
    CHECK(s.add_or_replace("net" + std::to_string(i), "password" + std::to_string(i)));
    CHECK(s.size() == i + 1);  // nothing dropped on the way up
  }
  for (size_t i = 0; i < kMinSavedNetworks; ++i) CHECK(s.contains("net" + std::to_string(i)));
  // Removing and re-adding keeps everything else.
  CHECK(s.remove("net3"));
  CHECK(s.size() == kMinSavedNetworks - 1);
  CHECK(s.add_or_replace("again", "againpassword"));
  CHECK(s.size() == kMinSavedNetworks);
  CHECK(s.contains("net0"));
}

TEST_CASE("WifiStore remove") {
  WifiStore s = filled(3);
  CHECK_FALSE(s.remove("nope"));
  CHECK(s.size() == 3);
  CHECK(s.remove("n1"));
  CHECK(s.ssids() == std::vector<std::string>{"n0", "n2"});
  CHECK_FALSE(s.contains("n1"));
  CHECK(pw(s, "n1") == "<none>");
  s.clear();
  CHECK(s.size() == 0);
}

TEST_CASE("WifiStore: open networks keep an empty password") {
  WifiStore s;
  CHECK(s.add_or_replace("Cafe", ""));
  CHECK(s.contains("Cafe"));
  CHECK(pw(s, "Cafe") == "");
  const WifiStore back = [&] {
    WifiStore t;
    t.parse(s.serialize());
    return t;
  }();
  CHECK(back.contains("Cafe"));
  CHECK(pw(back, "Cafe") == "");
}

TEST_CASE("WifiStore: 32-byte and UTF-8 SSIDs, 63-byte passwords") {
  WifiStore s;
  CHECK(kSsid32.size() == 32);
  CHECK(s.add_or_replace(kSsid32, kPass63));
  const std::string utf8 = "Caf\xC3\xA9 \xE2\x9A\xBD \xE6\x97\xA5\xE6\x9C\xAC";  // Café ⚽ 日本
  CHECK(s.add_or_replace(utf8, "p\xC3\xA4ssw\xC3\xB6rd"));
  CHECK(pw(s, kSsid32) == kPass63);
  CHECK(pw(s, utf8) == "p\xC3\xA4ssw\xC3\xB6rd");

  WifiStore t;
  const WifiStore::ParseResult r = t.parse(s.serialize());
  CHECK(r.loaded == 2);
  CHECK(r.skippedBytes == 0);
  CHECK(t.ssids() == s.ssids());
  CHECK(pw(t, kSsid32) == kPass63);
  CHECK(pw(t, utf8) == "p\xC3\xA4ssw\xC3\xB6rd");
}

TEST_CASE("WifiStore rejects what the radio cannot use") {
  WifiStore s;
  CHECK_FALSE(s.add_or_replace("", "password1"));               // hidden / empty SSID
  CHECK_FALSE(s.add_or_replace(kSsid32 + "x", "password1"));    // 33 bytes
  CHECK_FALSE(s.add_or_replace("home", std::string(64, 'a')));  // passphrase over 63 bytes
  CHECK_FALSE(s.add_or_replace(std::string("a\0b", 3), "password1"));
  CHECK_FALSE(s.add_or_replace("tab\there", "password1"));
  CHECK_FALSE(s.add_or_replace("line\nfeed", "password1"));
  CHECK_FALSE(s.add_or_replace("esc\x1b[31m", "password1"));
  CHECK_FALSE(s.add_or_replace("del\x7f", "password1"));
  CHECK_FALSE(s.add_or_replace("home", std::string("pass\0word", 9)));
  CHECK_FALSE(s.add_or_replace("home", "pass\nword"));
  CHECK(s.size() == 0);
  // A rejected replacement leaves the old entry alone.
  s.add_or_replace("home", "goodpassword");
  CHECK_FALSE(s.add_or_replace("home", std::string(64, 'a')));
  CHECK(pw(s, "home") == "goodpassword");
  CHECK(wifi_ssid_valid("ok"));
  CHECK_FALSE(wifi_ssid_valid(""));
  CHECK(wifi_password_valid(""));
  CHECK(wifi_password_valid(kPass63));
}

TEST_CASE("WifiStore password_of leaves its argument alone when the SSID is unknown") {
  WifiStore s;
  s.add_or_replace("a", "password1");
  std::string out = "keep";
  CHECK_FALSE(s.password_of("b", out));
  CHECK(out == "keep");
  CHECK(s.password_of("a", out));
  CHECK(out == "password1");
}

TEST_CASE("WifiStore ordered_by_signal: saved networks in range, strongest first") {
  WifiStore s;
  s.add_or_replace("c", "password3");
  s.add_or_replace("b", "password2");
  s.add_or_replace("a", "password1");  // MRU order: a, b, c
  const std::vector<WifiNetwork> scan = {net("other", -40), net("c", -50), net("a", -70, false),
                                         net("b", -60), net("stranger", -30, true)};
  const std::vector<WifiNetwork> ordered = s.ordered_by_signal(scan);
  REQUIRE(ordered.size() == 3);
  CHECK(ordered[0].ssid == "c");
  CHECK(ordered[0].rssi == -50);
  CHECK(ordered[1].ssid == "b");
  CHECK(ordered[2].ssid == "a");
  CHECK(ordered[2].rssi == -70);
}

TEST_CASE("WifiStore ordered_by_signal: one entry per SSID, the strongest; ties by recent use") {
  WifiStore s;
  s.add_or_replace("b", "password2");
  s.add_or_replace("a", "password1");  // a is more recent
  const std::vector<WifiNetwork> scan = {net("b", -80), net("a", -60), net("b", -55, true),
                                         net("a", -90)};
  std::vector<WifiNetwork> ordered = s.ordered_by_signal(scan);
  REQUIRE(ordered.size() == 2);
  CHECK(ordered[0].ssid == "b");
  CHECK(ordered[0].rssi == -55);
  CHECK(ordered[0].open);  // the entry of the strongest sighting
  CHECK(ordered[1].ssid == "a");
  CHECK(ordered[1].rssi == -60);

  const std::vector<WifiNetwork> tie = {net("b", -60), net("a", -60)};
  ordered = s.ordered_by_signal(tie);
  REQUIRE(ordered.size() == 2);
  CHECK(ordered[0].ssid == "a");  // equal signal: the more recently used first
  CHECK(ordered[1].ssid == "b");
}

TEST_CASE("WifiStore ordered_by_signal: nothing saved, nothing in range, hidden networks") {
  WifiStore empty;
  CHECK(empty.ordered_by_signal({net("a", -50)}).empty());
  WifiStore s;
  s.add_or_replace("a", "password1");
  CHECK(s.ordered_by_signal({}).empty());
  CHECK(s.ordered_by_signal({net("zzz", -40)}).empty());
  CHECK(s.ordered_by_signal({net("", -40)}).empty());
}

TEST_CASE("WifiStore round trip keeps order and passwords") {
  WifiStore s = filled(11);
  s.touch("n6");
  const std::string blob = s.serialize();
  CHECK(blob.size() <= WifiStore::kMaxSerializedBytes);
  WifiStore t;
  const WifiStore::ParseResult r = t.parse(blob);
  CHECK(r.loaded == 11);
  CHECK(r.skippedBytes == 0);
  CHECK(t.ssids() == s.ssids());
  for (const std::string& id : s.ssids()) CHECK(pw(t, id) == pw(s, id));
  CHECK(t.serialize() == blob);  // stable
}

TEST_CASE("WifiStore: the largest possible list fits in the blob limit") {
  WifiStore s;
  for (size_t i = 0; i < WifiStore::kMaxSaved; ++i) {
    std::string ssid = kSsid32;
    ssid[0] = static_cast<char>('A' + i);
    CHECK(s.add_or_replace(ssid, kPass63));
  }
  CHECK(s.size() == WifiStore::kMaxSaved);
  CHECK(s.serialize().size() <= WifiStore::kMaxSerializedBytes);
  WifiStore t;
  CHECK(t.parse(s.serialize()).loaded == WifiStore::kMaxSaved);
}

TEST_CASE("WifiStore parse of empty or garbage data never throws and loads nothing") {
  WifiStore s;
  s.add_or_replace("old", "password1");
  CHECK(s.parse("").loaded == 0);
  CHECK(s.size() == 0);  // parse replaces the content
  s.add_or_replace("old", "password1");
  const std::string junk = "hello, this is not a wifi list\x01\x02\xFE\xFE\xFE";
  const WifiStore::ParseResult r = s.parse(junk);
  CHECK(r.loaded == 0);
  CHECK(r.skippedBytes == junk.size());
  CHECK(s.size() == 0);
  // 0xFF bytes, a lone marker, a truncated record.
  CHECK(s.parse(std::string(300, '\xFF')).loaded == 0);
  CHECK(s.parse("\xFE").loaded == 0);
  CHECK(s.parse("\xFE\x20\x20").loaded == 0);
  CHECK(s.parse(std::string("\xFE\x00\x00\x00\x00", 5)).loaded == 0);
}

TEST_CASE("WifiStore: a corrupted blob loses at most the bad records") {
  WifiStore s = filled(6);
  const std::string good = s.serialize();
  const size_t recLen = good.size() / 6;  // all records here have the same size
  REQUIRE(recLen * 6 == good.size());

  SUBCASE("one flipped byte in a password") {
    std::string bad = good;
    bad[2 * recLen + 6] ^= 0x40;  // inside record 2's body
    WifiStore t;
    const WifiStore::ParseResult r = t.parse(bad);
    CHECK(r.loaded == 5);
    CHECK(r.skippedBytes > 0);
    CHECK_FALSE(t.contains("n2"));
    for (int i : {0, 1, 3, 4, 5}) CHECK(t.contains("n" + std::to_string(i)));
    CHECK(t.ssids() == std::vector<std::string>{"n0", "n1", "n3", "n4", "n5"});  // order kept
  }
  SUBCASE("a corrupted length byte loses that record only") {
    std::string bad = good;
    bad[3 * recLen + 1] = static_cast<char>(0x7F);  // record 3's SSID length
    WifiStore t;
    t.parse(bad);
    CHECK_FALSE(t.contains("n3"));
    for (int i : {0, 1, 2, 4, 5}) CHECK(t.contains("n" + std::to_string(i)));
  }
  SUBCASE("a destroyed record marker loses that record only") {
    std::string bad = good;
    bad[4 * recLen] = 'x';
    WifiStore t;
    t.parse(bad);
    CHECK_FALSE(t.contains("n4"));
    for (int i : {0, 1, 2, 3, 5}) CHECK(t.contains("n" + std::to_string(i)));
  }
  SUBCASE("garbage in front and between records") {
    std::string bad = "\x01\x02garbage" + good.substr(0, 2 * recLen) + "\xFE\xFE\xFE\xFE" + good.substr(2 * recLen);
    WifiStore t;
    const WifiStore::ParseResult r = t.parse(bad);
    CHECK(r.loaded == 6);
    CHECK(t.ssids() == s.ssids());
  }
  SUBCASE("truncated blob keeps the complete records") {
    WifiStore t;
    const WifiStore::ParseResult r = t.parse(good.substr(0, good.size() - 3));
    CHECK(r.loaded == 5);
    CHECK(t.ssids() == std::vector<std::string>{"n0", "n1", "n2", "n3", "n4"});
  }
  SUBCASE("every single-byte corruption loses at most one record") {
    for (size_t pos = 0; pos < good.size(); ++pos) {
      std::string bad = good;
      bad[pos] = static_cast<char>(bad[pos] ^ 0x5A);
      WifiStore t;
      const WifiStore::ParseResult r = t.parse(bad);
      INFO("byte " << pos);
      CHECK(r.loaded >= 5);
      CHECK(r.loaded <= 6);
      // Everything loaded is a record that was in the original, unchanged.
      for (const std::string& id : t.ssids()) {
        CHECK(s.contains(id));
        CHECK(pw(t, id) == pw(s, id));
      }
    }
  }
}

TEST_CASE("WifiStore parse skips duplicates and caps at the capacity") {
  WifiStore s = filled(3);
  const std::string one = s.serialize().substr(0, s.serialize().size() / 3);
  WifiStore t;
  const WifiStore::ParseResult r = t.parse(one + one + one);
  CHECK(r.loaded == 1);  // the same SSID three times: the first wins
  CHECK(t.size() == 1);

  // Twenty distinct records: only the first 16 (the most recent) are kept.
  std::string big;
  for (int i = 0; i < 20; ++i) {
    WifiStore one_rec;
    one_rec.add_or_replace("net" + std::to_string(100 + i), "password");
    big += one_rec.serialize();
  }
  const WifiStore::ParseResult r2 = t.parse(big);
  CHECK(t.size() == WifiStore::kMaxSaved);
  CHECK(r2.loaded == WifiStore::kMaxSaved);
  CHECK(t.ssids().front() == "net100");
}

TEST_CASE("WifiStore parse drops records that hold an invalid SSID or password") {
  // Build a record by hand with a valid checksum but an empty SSID.
  WifiStore good;
  good.add_or_replace("fine", "password1");
  std::string blob = WifiStore::encode_record("", "password1");
  blob += good.serialize();
  blob += WifiStore::encode_record("bad\nssid", "password1");
  blob += WifiStore::encode_record("longpw", std::string(64, 'x'));
  WifiStore t;
  const WifiStore::ParseResult r = t.parse(blob);
  CHECK(r.loaded == 1);
  CHECK(t.ssids() == std::vector<std::string>{"fine"});
}

TEST_CASE("WifiStore: a password never appears in what the UI can see") {
  WifiStore s;
  const std::string secret = "TopSecretPassw0rd!";
  s.add_or_replace("home", secret);
  s.add_or_replace("work", "AnotherSecret99");
  // ssids() is the only list the service hands to the UI.
  for (const std::string& id : s.ssids()) CHECK(id.find(secret) == std::string::npos);
  std::vector<WifiNetwork> ordered = s.ordered_by_signal({net("home", -50), net("work", -60)});
  for (const WifiNetwork& n : ordered) {
    CHECK(n.ssid.find("Secret") == std::string::npos);
  }
}
