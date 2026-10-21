// WifiController: the firmware's Wi-Fi logic, driven by a fake radio, a fake
// blob storage and a fake clock. Covers the WifiService contract (exactly-once
// callbacks, replace semantics, no waiting inside calls), auto-join with
// back-off, timeouts, hostile event sequences and secret handling.
#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wifi_controller.h"

namespace {

struct FakeRadio : WifiRadio {
  int64_t* clock = nullptr;
  bool isReady = true;
  bool startOk = true;       // start_scan succeeds
  bool scanFails = false;    // the scan ends Failed
  bool scanHangs = false;    // the scan never ends
  int64_t scanLatency = 0;   // ms until a scan is Done
  std::vector<WifiScanEntry> air;
  bool up = false;
  int rssiValue = -61;

  bool scanning = false;
  int64_t scanDoneAt = 0;
  int scanStarts = 0;
  int scanCancels = 0;
  int disconnects = 0;
  std::vector<int64_t> scanStartTimes;
  struct JoinCall {
    uint32_t gen;
    std::string ssid;
    std::string password;
    int64_t at;
  };
  std::vector<JoinCall> joins;

  bool ready() override { return isReady; }
  bool start_scan() override {
    if (!startOk) return false;
    scanning = true;
    scanDoneAt = *clock + scanLatency;
    ++scanStarts;
    scanStartTimes.push_back(*clock);
    return true;
  }
  ScanState poll_scan(std::vector<WifiScanEntry>& out) override {
    if (!scanning) return ScanState::Failed;
    if (scanHangs) return ScanState::Running;
    if (*clock < scanDoneAt) return ScanState::Running;
    scanning = false;
    if (scanFails) return ScanState::Failed;
    out = air;
    return ScanState::Done;
  }
  void cancel_scan() override {
    scanning = false;
    ++scanCancels;
  }
  void join(uint32_t gen, const std::string& ssid, const std::string& password) override {
    joins.push_back({gen, ssid, password, *clock});
    up = false;
  }
  void disconnect() override {
    ++disconnects;
    up = false;
  }
  bool link_up() override { return up; }
  int rssi() override { return rssiValue; }

  uint32_t lastGen() const { return joins.empty() ? 0 : joins.back().gen; }
};

struct FakeStorage : WifiBlobStorage {
  std::string blob;
  bool present = false;
  bool failSaves = false;
  int saves = 0;
  bool load(std::string& out) override {
    if (!present) return false;
    out = blob;
    return true;
  }
  bool save(const std::string& data) override {
    ++saves;
    if (failSaves) return false;
    blob = data;
    present = !data.empty();
    return true;
  }
};

WifiScanEntry ap(const char* ssid, int rssi, WifiAuth auth = WifiAuth::Wpa2Personal) {
  WifiScanEntry e;
  e.ssid = ssid;
  e.rssi = rssi;
  e.auth = auth;
  return e;
}

const char* kPass = "TopSecretPassw0rd!";
const char* kPass2 = "AnotherSecret-99";

struct Rig {
  int64_t t = 1000;
  FakeRadio radio;
  FakeStorage storage;
  std::vector<std::string> logs;
  WifiController ctrl;

  explicit Rig(WifiTiming timing = WifiTiming())
      : radio(),
        ctrl(radio, storage, [this] { return t; }, [this](const std::string& s) { logs.push_back(s); },
             timing) {
    radio.clock = &t;
  }

  void tick() { ctrl.tick(); }
  void advance(int64_t ms, int64_t step = 10) {
    for (int64_t done = 0; done < ms; done += step) {
      t += step;
      ctrl.tick();
    }
  }
  // The driver reports the IP of the latest join attempt.
  void gotIp(const char* ip = "192.168.1.50") {
    radio.up = true;
    ctrl.on_ip(radio.lastGen(), ip);
  }
  void lost(int reason = 200) {
    radio.up = false;
    ctrl.on_disconnected(radio.lastGen(), reason);
  }
  // Seeds the storage with saved networks (most recent first) and loads them.
  void seed(const std::vector<std::pair<std::string, std::string>>& nets) {
    WifiStore s;
    for (size_t i = nets.size(); i-- > 0;) s.add_or_replace(nets[i].first, nets[i].second);
    storage.blob = s.serialize();
    storage.present = !nets.empty();
    ctrl.load();
  }
  bool logged(const std::string& needle) const {
    for (const std::string& l : logs) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  std::string stored_password(const std::string& ssid) const {
    WifiStore s;
    s.parse(storage.blob);
    std::string out = "<none>";
    s.password_of(ssid, out);
    return out;
  }
};

struct Result {
  int calls = 0;
  bool ok = false;
};

std::function<void(bool)> into(Result& r) {
  return [&r](bool ok) {
    ++r.calls;
    r.ok = ok;
  };
}

}  // namespace

// ------------------------------------------------------------------ scan

TEST_CASE("controller scan: asynchronous, sorted, deduplicated, unsupported and hidden networks left out") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("Weak", -80),
                 ap("Strong", -50),
                 ap("Strong", -70),
                 ap("", -40),
                 ap("Cafe", -60, WifiAuth::Open),
                 ap("OldWep", -45, WifiAuth::Unsupported),
                 ap("Corp", -42, WifiAuth::Unsupported),
                 ap("bad\nname", -41)};
  int calls = 0;
  std::vector<WifiNetwork> got;
  r.ctrl.scan([&](std::vector<WifiNetwork> n) {
    ++calls;
    got = std::move(n);
  });
  CHECK(calls == 0);  // never inside the call
  r.advance(100);
  REQUIRE(calls == 1);
  REQUIRE(got.size() == 3);
  CHECK(got[0].ssid == "Strong");
  CHECK(got[0].rssi == -50);
  CHECK_FALSE(got[0].open);
  CHECK(got[1].ssid == "Cafe");
  CHECK(got[1].open);
  CHECK(got[2].ssid == "Weak");
  r.advance(1000);
  CHECK(calls == 1);  // exactly once
}

TEST_CASE("controller scan: at most kWifiMaxScanResults networks, the strongest") {
  Rig r;
  r.ctrl.load();
  for (int i = 0; i < 50; ++i) r.radio.air.push_back(ap(("net" + std::to_string(i)).c_str(), -30 - i));
  std::vector<WifiNetwork> got;
  r.ctrl.scan([&](std::vector<WifiNetwork> n) { got = std::move(n); });
  r.advance(100);
  REQUIRE(got.size() == kWifiMaxScanResults);
  CHECK(got.front().ssid == "net0");
  CHECK(got.back().ssid == "net29");
}

TEST_CASE("controller scan: several requests share one radio scan and each is answered once") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("A", -50)};
  r.radio.scanLatency = 500;
  int a = 0, b = 0;
  r.ctrl.scan([&](std::vector<WifiNetwork>) { ++a; });
  r.advance(100);
  r.ctrl.scan([&](std::vector<WifiNetwork>) { ++b; });
  r.advance(1000);
  CHECK(a == 1);
  CHECK(b == 1);
  CHECK(r.radio.scanStarts == 1);
}

TEST_CASE("controller scan: a callback may start the next scan") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("A", -50)};
  int calls = 0;
  std::function<void(std::vector<WifiNetwork>)> again = [&](std::vector<WifiNetwork>) {
    if (++calls < 3) r.ctrl.scan(again);
  };
  r.ctrl.scan(again);
  r.advance(500);
  CHECK(calls == 3);
  CHECK(r.radio.scanStarts == 3);
}

TEST_CASE("controller scan: a failed or hung scan answers with an empty list") {
  SUBCASE("driver reports failure") {
    Rig r;
    r.ctrl.load();
    r.radio.scanFails = true;
    int calls = 0;
    size_t n = 99;
    r.ctrl.scan([&](std::vector<WifiNetwork> v) {
      ++calls;
      n = v.size();
    });
    r.advance(100);
    CHECK(calls == 1);
    CHECK(n == 0);
  }
  SUBCASE("the scan never ends") {
    Rig r;
    r.ctrl.load();
    r.radio.scanHangs = true;
    int calls = 0;
    r.ctrl.scan([&](std::vector<WifiNetwork>) { ++calls; });
    r.advance(11000);
    CHECK(calls == 0);
    r.advance(2000);  // 12 s timeout
    CHECK(calls == 1);
    CHECK(r.radio.scanCancels == 1);
    r.advance(5000);
    CHECK(calls == 1);
  }
  SUBCASE("the driver cannot start a scan") {
    Rig r;
    r.ctrl.load();
    r.radio.startOk = false;
    int calls = 0;
    r.ctrl.scan([&](std::vector<WifiNetwork>) { ++calls; });
    r.advance(5000);
    CHECK(calls == 0);
    r.radio.startOk = true;  // it recovers within the window
    r.advance(500);
    CHECK(calls == 1);

    int calls2 = 0;
    r.radio.startOk = false;
    r.ctrl.scan([&](std::vector<WifiNetwork>) { ++calls2; });
    r.advance(13000);
    CHECK(calls2 == 1);  // gave up with an empty list
  }
  SUBCASE("the radio is not ready yet") {
    Rig r;
    r.ctrl.load();
    r.radio.isReady = false;
    r.radio.air = {ap("A", -50)};
    int calls = 0;
    r.ctrl.scan([&](std::vector<WifiNetwork>) { ++calls; });
    r.advance(2000);
    CHECK(calls == 0);
    CHECK(r.radio.scanStarts == 0);
    r.radio.isReady = true;
    r.advance(200);
    CHECK(calls == 1);
  }
}

// ------------------------------------------------------------------ user joins

TEST_CASE("controller connect: success saves the network with its password, once") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("Home", -55)};
  r.radio.scanLatency = 0;
  std::vector<WifiNetwork> list;
  r.ctrl.scan([&](std::vector<WifiNetwork> n) { list = n; });
  r.advance(100);

  Result res;
  std::vector<std::string> savedInside;
  WifiStatus statusInside;
  r.ctrl.connect("Home", kPass, [&](bool ok) {
    ++res.calls;
    res.ok = ok;
    savedInside = r.ctrl.saved();   // the UI asks from inside the callback
    statusInside = r.ctrl.status();
  });
  CHECK(res.calls == 0);
  REQUIRE(r.radio.joins.size() == 1);
  CHECK(r.radio.joins[0].ssid == "Home");
  CHECK(r.radio.joins[0].password == kPass);
  CHECK(r.ctrl.saved().empty());           // not saved before it works
  CHECK_FALSE(r.ctrl.status().connected);

  r.advance(2000);
  CHECK(res.calls == 0);
  r.gotIp("10.0.0.7");
  CHECK(res.calls == 1);
  CHECK(res.ok);
  CHECK(savedInside == std::vector<std::string>{"Home"});
  CHECK(statusInside.connected);
  CHECK(statusInside.ssid == "Home");
  CHECK(statusInside.ip == "10.0.0.7");
  CHECK(statusInside.rssi == -61);
  CHECK(r.stored_password("Home") == kPass);
  CHECK(r.storage.saves == 1);
  CHECK(r.logged("[wifi] joined Home rssi=-61 ip=10.0.0.7"));

  r.gotIp("10.0.0.7");  // a duplicate event changes nothing
  r.advance(1000);
  CHECK(res.calls == 1);
  CHECK(r.storage.saves == 1);
}

TEST_CASE("controller connect: an open network joins with no password and is saved as open") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("Cafe", -60, WifiAuth::Open)};
  r.ctrl.scan([](std::vector<WifiNetwork>) {});
  r.advance(100);
  Result res;
  r.ctrl.connect("Cafe", "ignored-because-open", into(res));
  REQUIRE(r.radio.joins.size() == 1);
  CHECK(r.radio.joins[0].password.empty());
  r.gotIp();
  CHECK(res.ok);
  CHECK(r.stored_password("Cafe").empty());
  CHECK(r.ctrl.saved() == std::vector<std::string>{"Cafe"});
}

TEST_CASE("controller connect: WEP and enterprise networks, bad input and short passwords are refused at once") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("OldWep", -45, WifiAuth::Unsupported), ap("Home", -50), ap("Cafe", -60, WifiAuth::Open)};
  r.ctrl.scan([](std::vector<WifiNetwork>) {});
  r.advance(100);

  Result a, b, c, d, e, f;
  r.ctrl.connect("OldWep", "password123", into(a));
  CHECK(a.calls == 1);
  CHECK_FALSE(a.ok);
  r.ctrl.connect("Home", "", into(b));  // secured, no password
  CHECK(b.calls == 1);
  CHECK_FALSE(b.ok);
  r.ctrl.connect("", "password123", into(c));
  CHECK(c.calls == 1);
  CHECK_FALSE(c.ok);
  r.ctrl.connect("Home", "short", into(d));  // WPA2 passphrases have 8 to 63 characters
  CHECK(d.calls == 1);
  CHECK_FALSE(d.ok);
  r.ctrl.connect("Home", std::string(64, 'x'), into(e));
  CHECK(e.calls == 1);
  CHECK_FALSE(e.ok);
  r.ctrl.connect(std::string(33, 'n'), "password123", into(f));
  CHECK(f.calls == 1);
  CHECK_FALSE(f.ok);
  CHECK(r.radio.joins.empty());
  CHECK(r.ctrl.saved().empty());
}

TEST_CASE("controller connect: a null callback is allowed") {
  Rig r;
  r.ctrl.load();
  r.ctrl.connect("Home", kPass, nullptr);
  r.gotIp();
  r.ctrl.scan(nullptr);
  r.ctrl.connect_saved("Home", nullptr);
  r.gotIp();
  r.advance(1000);
  CHECK(r.ctrl.status().connected);
}

TEST_CASE("controller connect: the 15 s timeout answers false once, drops the attempt and saves nothing") {
  Rig r;
  r.ctrl.load();
  Result res;
  r.ctrl.connect("Home", kPass, into(res));
  r.advance(14900);
  CHECK(res.calls == 0);
  CHECK(r.ctrl.joining());
  r.advance(200);
  CHECK(res.calls == 1);
  CHECK_FALSE(res.ok);
  CHECK_FALSE(r.ctrl.joining());
  CHECK(r.radio.disconnects >= 1);
  CHECK(r.ctrl.saved().empty());
  CHECK(r.storage.saves == 0);
  // A late IP from the abandoned attempt is ignored.
  r.radio.up = true;
  r.ctrl.on_ip(r.radio.joins.back().gen, "1.2.3.4");
  CHECK(res.calls == 1);
  CHECK_FALSE(r.ctrl.status().connected);
  CHECK(r.ctrl.saved().empty());
}

TEST_CASE("controller connect: a wrong password fails after two authentication failures, before the timeout") {
  Rig r;
  r.ctrl.load();
  Result res;
  r.ctrl.connect("Home", kPass, into(res));
  r.advance(500);
  r.lost(15);  // 4-way handshake timeout
  r.advance(500);
  CHECK(res.calls == 0);  // one failure can be radio noise: try again
  r.advance(1500);
  REQUIRE(r.radio.joins.size() >= 2);  // the driver was asked again
  r.lost(202);
  CHECK(res.calls == 1);
  CHECK_FALSE(res.ok);
  CHECK(r.ctrl.saved().empty());
  CHECK(r.t - 1000 < 15000);
}

TEST_CASE("controller connect: ordinary failures are retried inside the window") {
  Rig r;
  r.ctrl.load();
  Result res;
  r.ctrl.connect("Weak", kPass, into(res));
  for (int i = 0; i < 4; ++i) {
    r.advance(1500);
    r.lost(201);  // no AP found
  }
  CHECK(res.calls == 0);
  REQUIRE(r.radio.joins.size() >= 4);
  r.advance(1500);
  r.gotIp();
  CHECK(res.calls == 1);
  CHECK(res.ok);
}

TEST_CASE("controller connect: the radio's own disconnect after we start a join is not a failure") {
  Rig r;
  r.ctrl.load();
  Result res;
  r.ctrl.connect("Home", kPass, into(res));
  const uint32_t gen = r.radio.lastGen();
  r.ctrl.on_disconnected(gen, kWifiReasonAssocLeave);  // the old link going away
  r.advance(3000);
  CHECK(res.calls == 0);
  CHECK(r.radio.joins.size() == 1);  // no retry was triggered
  r.gotIp();
  CHECK(res.ok);
}

TEST_CASE("controller connect: a new join replaces the pending one (false once, not saved, before the call returns)") {
  Rig r;
  r.ctrl.load();
  std::vector<std::string> order;
  Result first, second;
  r.ctrl.connect("First", kPass, [&](bool ok) {
    order.push_back("first");
    first.calls++;
    first.ok = ok;
  });
  r.advance(500);
  const uint32_t firstGen = r.radio.lastGen();
  r.ctrl.connect("Second", kPass2, [&](bool ok) {
    order.push_back("second");
    second.calls++;
    second.ok = ok;
  });
  CHECK(first.calls == 1);  // answered inside the replacing call
  CHECK_FALSE(first.ok);
  CHECK(second.calls == 0);
  REQUIRE(r.radio.joins.size() == 2);
  CHECK(r.radio.joins[1].ssid == "Second");
  // The first attempt's driver result arrives late: ignored.
  r.radio.up = true;
  r.ctrl.on_ip(firstGen, "9.9.9.9");
  CHECK_FALSE(r.ctrl.status().connected);
  CHECK(second.calls == 0);
  r.radio.up = false;
  r.gotIp("10.0.0.2");
  CHECK(second.ok);
  CHECK(first.calls == 1);
  CHECK(r.ctrl.saved() == std::vector<std::string>{"Second"});  // only the latest is saved
  CHECK(order == std::vector<std::string>{"first", "second"});
}

TEST_CASE("controller connect: replacing from connect_saved and the reverse") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.advance(100);  // the boot auto-join starts a scan
  Result a, b, c;
  r.ctrl.connect("Other", kPass2, into(a));
  r.ctrl.connect_saved("Home", into(b));
  CHECK(a.calls == 1);
  CHECK_FALSE(a.ok);
  r.ctrl.connect("Third", kPass2, into(c));
  CHECK(b.calls == 1);
  CHECK_FALSE(b.ok);
  r.gotIp();
  CHECK(c.ok);
  CHECK(a.calls == 1);
  CHECK(b.calls == 1);
  CHECK(c.calls == 1);
  CHECK(r.ctrl.saved().front() == "Third");
}

TEST_CASE("controller connect: the replaced callback may start a new join itself") {
  Rig r;
  r.ctrl.load();
  Result a, b, c;
  r.ctrl.connect("A", kPass, [&](bool ok) {
    a.calls++;
    a.ok = ok;
    r.ctrl.connect("C", kPass, into(c));  // re-entrant: from inside the replaced callback
  });
  r.ctrl.connect("B", kPass, into(b));
  // A's callback ran, and then started C, which replaced B.
  CHECK(a.calls == 1);
  CHECK(b.calls == 1);
  CHECK_FALSE(b.ok);
  CHECK(c.calls == 0);
  r.gotIp();
  CHECK(c.ok);
  CHECK(r.ctrl.saved() == std::vector<std::string>{"C"});
}

TEST_CASE("controller connect_saved: uses the stored password and makes the network most recent") {
  Rig r;
  r.seed({{"Home", kPass}, {"Work", kPass2}});
  r.radio.air = {};  // nothing in range for the auto-join
  r.advance(200);
  Result res;
  r.ctrl.connect_saved("Work", into(res));
  REQUIRE_FALSE(r.radio.joins.empty());
  CHECK(r.radio.joins.back().ssid == "Work");
  CHECK(r.radio.joins.back().password == kPass2);
  r.gotIp();
  CHECK(res.ok);
  CHECK(r.ctrl.saved() == std::vector<std::string>{"Work", "Home"});
  Result unknown;
  r.ctrl.connect_saved("Nope", into(unknown));
  CHECK(unknown.calls == 1);
  CHECK_FALSE(unknown.ok);
}

TEST_CASE("controller connect_saved: failure keeps the entry and its place") {
  Rig r;
  r.seed({{"Home", kPass}, {"Work", kPass2}});
  r.advance(200);
  Result res;
  r.ctrl.connect_saved("Work", into(res));
  r.advance(16000);
  CHECK(res.calls == 1);
  CHECK_FALSE(res.ok);
  CHECK(r.ctrl.saved() == std::vector<std::string>{"Home", "Work"});
}

// ------------------------------------------------------------------ auto-join

TEST_CASE("controller auto-join: nothing saved means no scanning at all") {
  Rig r;
  r.ctrl.load();
  r.advance(120000);
  CHECK(r.radio.scanStarts == 0);
  CHECK(r.radio.joins.empty());
}

TEST_CASE("controller auto-join at boot: the strongest saved network in range, with its stored password") {
  Rig r;
  r.seed({{"Home", kPass}, {"Work", kPass2}, {"Cafe", ""}});
  r.radio.air = {ap("Home", -79), ap("Work", -55), ap("Stranger", -30), ap("Cafe", -70, WifiAuth::Open)};
  r.advance(100);
  REQUIRE(r.radio.joins.size() == 1);
  CHECK(r.radio.joins[0].ssid == "Work");
  CHECK(r.radio.joins[0].password == kPass2);
  r.gotIp("192.168.0.9");
  CHECK(r.ctrl.status().connected);
  CHECK(r.ctrl.status().ssid == "Work");
  CHECK(r.logged("[wifi] joined Work rssi=-61 ip=192.168.0.9"));
  CHECK(r.ctrl.saved().front() == "Work");  // most recently used
  CHECK(r.storage.saves == 1);              // the order changed, so it was written once
  r.advance(60000);
  CHECK(r.radio.scanStarts == 1);  // connected: no more scans
}

TEST_CASE("controller auto-join: joining the network that is already first writes nothing") {
  Rig r;
  r.seed({{"Home", kPass}, {"Work", kPass2}});
  r.radio.air = {ap("Home", -60)};
  r.advance(100);
  r.gotIp();
  CHECK(r.ctrl.status().ssid == "Home");
  CHECK(r.storage.saves == 0);
}

TEST_CASE("controller auto-join: an open saved network joins without a password") {
  Rig r;
  r.seed({{"Cafe", ""}});
  r.radio.air = {ap("Cafe", -50, WifiAuth::Open)};
  r.advance(100);
  REQUIRE(r.radio.joins.size() == 1);
  CHECK(r.radio.joins[0].password.empty());
}

TEST_CASE("controller auto-join: back-off 5, 10, 30, 60 s with a scan each time") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.radio.air = {ap("Elsewhere", -50)};
  r.advance(400000, 50);
  REQUIRE(r.radio.scanStartTimes.size() >= 7);
  std::vector<int64_t> gaps;
  for (size_t i = 1; i < r.radio.scanStartTimes.size(); ++i) {
    gaps.push_back(r.radio.scanStartTimes[i] - r.radio.scanStartTimes[i - 1]);
  }
  auto near = [](int64_t v, int64_t want) { return v >= want && v <= want + 200; };
  CHECK(near(gaps[0], 5000));
  CHECK(near(gaps[1], 10000));
  CHECK(near(gaps[2], 30000));
  CHECK(near(gaps[3], 60000));
  CHECK(near(gaps[4], 60000));
  CHECK(near(gaps[5], 60000));
  CHECK(r.radio.joins.empty());
  // The network turns up: the next attempt joins it, and the back-off starts over later.
  r.radio.air = {ap("Home", -70)};
  for (int i = 0; i < 1300 && r.radio.joins.empty(); ++i) r.advance(50, 50);  // until the next attempt
  REQUIRE(r.radio.joins.size() == 1);
  r.gotIp();
  CHECK(r.ctrl.status().connected);
}

TEST_CASE("controller auto-join: after a drop it rescans at once, then backs off from 5 s again") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.radio.air = {ap("Home", -60)};
  r.advance(100);
  r.gotIp();
  REQUIRE(r.ctrl.status().connected);
  r.advance(30000);
  const int scansBefore = r.radio.scanStarts;

  r.radio.air = {};  // router unplugged
  r.lost(200);
  CHECK_FALSE(r.ctrl.status().connected);
  CHECK(r.ctrl.status().ssid.empty());
  r.advance(100);
  CHECK(r.radio.scanStarts == scansBefore + 1);  // immediate attempt
  r.advance(4800);
  CHECK(r.radio.scanStarts == scansBefore + 1);
  r.advance(500);
  CHECK(r.radio.scanStarts == scansBefore + 2);  // after 5 s

  r.radio.air = {ap("Home", -62)};  // router back
  r.advance(10500);
  REQUIRE(r.radio.joins.size() == 2);
  r.gotIp();
  CHECK(r.ctrl.status().connected);
  CHECK(r.ctrl.status().ssid == "Home");
}

TEST_CASE("controller auto-join: a failing network is skipped for the next one in range, then backs off") {
  Rig r;
  r.seed({{"Strong", kPass}, {"Weak", kPass2}});
  r.radio.air = {ap("Strong", -50), ap("Weak", -75)};
  r.advance(100);
  REQUIRE(r.radio.joins.size() == 1);
  CHECK(r.radio.joins[0].ssid == "Strong");
  r.lost(15);
  r.advance(1100);
  r.lost(15);  // wrong password twice
  REQUIRE(r.radio.joins.size() >= 2);
  CHECK(r.radio.joins.back().ssid == "Weak");
  r.advance(16000);  // times out
  CHECK_FALSE(r.ctrl.status().connected);
  const int scans = r.radio.scanStarts;
  r.advance(5300);
  CHECK(r.radio.scanStarts > scans);  // back-off rescan
}

TEST_CASE("controller auto-join: a stale or foreign driver event never changes the link") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.radio.air = {ap("Home", -60)};
  r.advance(100);
  r.gotIp();
  const uint32_t gen = r.radio.lastGen();
  r.ctrl.on_disconnected(gen + 5, 200);  // not our attempt
  r.ctrl.on_disconnected(0, 200);
  CHECK(r.ctrl.status().connected);
  r.ctrl.on_ip(gen + 9, "6.6.6.6");
  CHECK(r.ctrl.status().ip == "192.168.1.50");
  r.lost(200);
  CHECK_FALSE(r.ctrl.status().connected);
  r.ctrl.on_disconnected(gen, 200);  // the same loss reported again
  r.advance(100);
  CHECK(r.radio.scanStarts >= 2);
}

TEST_CASE("controller auto-join: a link that vanished without an event is noticed") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.radio.air = {ap("Home", -60)};
  r.advance(100);
  r.gotIp();
  REQUIRE(r.ctrl.status().connected);
  r.radio.up = false;  // the event was lost on its way
  r.advance(100);
  CHECK_FALSE(r.ctrl.status().connected);
}

TEST_CASE("controller never fights a user join") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.radio.air = {ap("Home", -60), ap("Other", -50)};
  r.advance(100);
  REQUIRE(r.radio.joins.size() == 1);  // auto-join of Home in progress
  Result res;
  r.ctrl.connect("Other", kPass2, into(res));
  REQUIRE(r.radio.joins.size() == 2);
  CHECK(r.radio.joins[1].ssid == "Other");
  const int scans = r.radio.scanStarts;
  // The old auto-join's failure and the long wait do not start anything new.
  r.ctrl.on_disconnected(r.radio.joins[0].gen, 201);
  r.advance(14000);
  CHECK(r.radio.joins.size() == 2);
  CHECK(r.radio.scanStarts == scans);
  r.advance(2000);  // the user join times out
  CHECK(res.calls == 1);
  CHECK_FALSE(res.ok);
  r.advance(200);
  CHECK(r.radio.scanStarts > scans);  // only now the auto-join resumes (it rejoins Home)
}

TEST_CASE("controller: a user scan cancels a background join, which resumes afterwards") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.radio.air = {ap("Home", -60), ap("Other", -50)};
  r.advance(100);
  REQUIRE(r.radio.joins.size() == 1);
  std::vector<WifiNetwork> list;
  r.ctrl.scan([&](std::vector<WifiNetwork> n) { list = n; });
  r.advance(200);
  CHECK(list.size() == 2);
  CHECK_FALSE(r.ctrl.joining_for_user());
  r.advance(2000);
  CHECK(r.radio.joins.size() >= 2);  // the auto-join started again
}

TEST_CASE("controller: a scan requested during a user join waits for it") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("A", -50)};
  Result res;
  r.ctrl.connect("Home", kPass, into(res));
  int calls = 0;
  r.ctrl.scan([&](std::vector<WifiNetwork>) { ++calls; });
  r.advance(3000);
  CHECK(calls == 0);
  CHECK(r.radio.scanStarts == 0);
  r.gotIp();
  r.advance(200);
  CHECK(res.ok);
  CHECK(calls == 1);
}

TEST_CASE("controller: the auto-join scan answers a waiting user scan too") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.radio.air = {ap("Home", -60), ap("Other", -50)};
  r.radio.scanLatency = 300;
  r.advance(50);  // the auto scan is running
  int calls = 0;
  r.ctrl.scan([&](std::vector<WifiNetwork>) { ++calls; });
  r.advance(500);
  CHECK(calls == 1);
  CHECK(r.radio.scanStarts == 1);
  CHECK(r.radio.joins.size() == 1);  // and the auto-join went on to join
}

// ------------------------------------------------------------------ forget, wipe, persistence

TEST_CASE("controller forget: the joined network is dropped and status says not connected") {
  Rig r;
  r.seed({{"Home", kPass}, {"Work", kPass2}});
  r.radio.air = {ap("Home", -60)};
  r.advance(100);
  r.gotIp();
  REQUIRE(r.ctrl.status().connected);
  r.radio.air = {};  // so the auto-join does not pick anything up afterwards
  r.ctrl.forget("Home");
  CHECK_FALSE(r.ctrl.status().connected);
  CHECK(r.ctrl.saved() == std::vector<std::string>{"Work"});
  CHECK(r.radio.disconnects >= 1);
  CHECK(r.stored_password("Home") == "<none>");
  // The driver's own report of the drop is not a second event to act on.
  r.ctrl.on_disconnected(r.radio.lastGen(), kWifiReasonAssocLeave);
  CHECK_FALSE(r.ctrl.status().connected);
}

TEST_CASE("controller forget: an unknown SSID is ignored; another network leaves the link alone") {
  Rig r;
  r.seed({{"Home", kPass}, {"Work", kPass2}});
  r.radio.air = {ap("Home", -60)};
  r.advance(100);
  r.gotIp();
  const int saves = r.storage.saves;
  r.ctrl.forget("Nope");
  CHECK(r.storage.saves == saves);
  r.ctrl.forget("Work");
  CHECK(r.ctrl.status().connected);
  CHECK(r.ctrl.saved() == std::vector<std::string>{"Home"});
}

TEST_CASE("controller forget: cancels a join of that saved network (false, once)") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.advance(100);
  Result res;
  r.ctrl.connect_saved("Home", into(res));
  r.ctrl.forget("Home");
  CHECK(res.calls == 0);  // only a REPLACED join is answered inside a service call
  r.tick();
  CHECK(res.calls == 1);  // the next tick answers it
  CHECK_FALSE(res.ok);
  r.advance(20000);
  CHECK(res.calls == 1);
  CHECK(r.ctrl.saved().empty());
}

TEST_CASE("controller wipe: a pending join is answered false by the next tick, not inside wipe") {
  Rig r;
  r.seed({{"Home", kPass}});
  r.advance(100);
  Result res;
  r.ctrl.connect("New", kPass2, into(res));
  r.ctrl.wipe();
  CHECK(res.calls == 0);
  r.tick();
  CHECK(res.calls == 1);
  CHECK_FALSE(res.ok);
  r.advance(20000);
  CHECK(res.calls == 1);
}

TEST_CASE("controller connect before the radio is ready: nothing reaches the driver, the callback ends false once") {
  Rig r;
  r.ctrl.load();
  r.radio.isReady = false;
  Result res;
  r.ctrl.connect("Home", kPass, into(res));
  r.advance(14000);
  CHECK(res.calls == 0);
  CHECK(r.radio.joins.empty());  // join() is never called on a radio that is not up
  r.advance(1500);               // the 15 s deadline
  CHECK(res.calls == 1);
  CHECK_FALSE(res.ok);
  CHECK(r.radio.joins.empty());
  CHECK(r.ctrl.saved().empty());
  r.advance(20000);
  CHECK(res.calls == 1);
}

TEST_CASE("controller connect before the radio is ready: the join goes out as soon as it is") {
  Rig r;
  r.ctrl.load();
  r.radio.isReady = false;
  Result res;
  r.ctrl.connect("Home", kPass, into(res));
  r.advance(2500);
  CHECK(r.radio.joins.empty());
  r.radio.isReady = true;
  r.advance(1200);
  REQUIRE(r.radio.joins.size() == 1);
  CHECK(r.radio.joins[0].ssid == "Home");
  r.gotIp();
  CHECK(res.ok);
  CHECK(res.calls == 1);
}

TEST_CASE("controller forget: the user's own pending join is not touched by forgetting an old entry") {
  Rig r;
  r.seed({{"Old", kPass}});
  r.radio.air = {};
  r.advance(100);
  Result res;
  r.ctrl.connect("Old", kPass2, into(res));  // retyped password for the same SSID
  r.ctrl.forget("Old");
  CHECK(res.calls == 0);                      // still trying
  r.gotIp();
  CHECK(res.ok);
  CHECK(r.stored_password("Old") == kPass2);  // saved again with the new password
}

TEST_CASE("controller wipe: clears the list, drops the link, stops auto-join") {
  Rig r;
  r.seed({{"Home", kPass}, {"Work", kPass2}});
  r.radio.air = {ap("Home", -60)};
  r.advance(100);
  r.gotIp();
  r.ctrl.wipe();
  CHECK(r.ctrl.saved().empty());
  CHECK_FALSE(r.ctrl.status().connected);
  CHECK_FALSE(r.storage.present);
  const int scans = r.radio.scanStarts;
  r.advance(120000);
  CHECK(r.radio.scanStarts == scans);
}

TEST_CASE("controller persistence: written on change only; a failed write is retried at the next change") {
  Rig r;
  r.ctrl.load();
  r.storage.failSaves = true;
  Result a, b;
  r.ctrl.connect("A", kPass, into(a));
  r.gotIp();
  CHECK(a.ok);
  CHECK(r.storage.saves == 1);
  CHECK(r.logged("could not save"));
  CHECK(r.ctrl.saved() == std::vector<std::string>{"A"});  // RAM stays authoritative
  r.storage.failSaves = false;
  r.advance(5000);
  CHECK(r.storage.saves == 1);  // nothing changed: no retry loop
  r.ctrl.connect("B", kPass2, into(b));
  r.gotIp();
  CHECK(r.storage.saves == 2);
  WifiStore s;
  s.parse(r.storage.blob);
  CHECK(s.ssids() == std::vector<std::string>{"B", "A"});  // both written now
}

TEST_CASE("controller persistence: a damaged blob loses only the bad records and is rewritten clean") {
  Rig r;
  WifiStore s;
  s.add_or_replace("C", "password3");
  s.add_or_replace("B", "password2");
  s.add_or_replace("A", "password1");
  std::string blob = s.serialize();
  const size_t rec = blob.size() / 3;
  blob[rec + 5] ^= 0x33;  // damage B
  r.storage.blob = blob;
  r.storage.present = true;
  r.ctrl.load();
  CHECK(r.ctrl.saved() == std::vector<std::string>{"A", "C"});
  CHECK(r.logged("damaged"));
  CHECK(r.storage.saves == 1);
  WifiStore clean;
  CHECK(clean.parse(r.storage.blob).skippedBytes == 0);
  CHECK(clean.ssids() == std::vector<std::string>{"A", "C"});
}

TEST_CASE("controller persistence: a blob that is garbage or missing starts empty without throwing") {
  Rig r;
  r.storage.present = true;
  r.storage.blob = std::string(500, '\xFE');
  r.ctrl.load();
  CHECK(r.ctrl.saved().empty());
  Rig r2;
  r2.ctrl.load();
  CHECK(r2.ctrl.saved().empty());
}

TEST_CASE("controller keeps at least 8 networks and at most 16, dropping the least recently used") {
  Rig r;
  r.ctrl.load();
  for (int i = 0; i < 20; ++i) {
    Result res;
    r.ctrl.connect("net" + std::to_string(i), kPass, into(res));
    r.gotIp();
    REQUIRE(res.ok);
    if (i < 16) CHECK(r.ctrl.saved().size() == static_cast<size_t>(i + 1));
  }
  CHECK(r.ctrl.saved().size() == 16);
  CHECK(r.ctrl.saved().front() == "net19");
  CHECK(r.ctrl.saved().back() == "net4");
  CHECK(r.ctrl.saved().size() >= kMinSavedNetworks);
}

TEST_CASE("controller persistence survives a restart: the new controller auto-joins the saved network") {
  FakeStorage disk;
  {
    Rig r;
    r.storage = disk;
    r.ctrl.load();
    Result res;
    r.ctrl.connect("Home", kPass, into(res));
    r.gotIp();
    disk = r.storage;
  }
  Rig r2;
  r2.storage = disk;
  r2.ctrl.load();
  r2.radio.air = {ap("Home", -70)};
  r2.advance(100);
  REQUIRE(r2.radio.joins.size() == 1);
  CHECK(r2.radio.joins[0].password == kPass);
}

// ------------------------------------------------------------------ secrets

TEST_CASE("controller never logs a password, and status, saved and describe never carry one") {
  Rig r;
  r.seed({{"Stored", kPass2}});
  r.radio.air = {ap("Stored", -60), ap("Home", -50), ap("OldWep", -40, WifiAuth::Unsupported)};
  r.advance(100);
  r.lost(15);
  r.advance(1200);
  r.lost(202);
  r.advance(20000);
  Result a, b, c;
  r.ctrl.connect("Home", kPass, into(a));
  r.advance(16000);
  r.ctrl.connect("Home", kPass, into(b));
  r.gotIp();
  r.ctrl.connect("OldWep", kPass, into(c));
  r.ctrl.forget("Home");
  r.ctrl.wipe();
  r.advance(1000);
  REQUIRE_FALSE(r.logs.empty());
  for (const std::string& l : r.logs) {
    INFO("log line: " << l);
    CHECK(l.find(kPass) == std::string::npos);
    CHECK(l.find(kPass2) == std::string::npos);
    CHECK(l.find("Secret") == std::string::npos);
    CHECK(l.find("assword") == std::string::npos);
  }
  const std::string d = r.ctrl.describe();
  CHECK(d.find("Secret") == std::string::npos);
}

TEST_CASE("controller logs SSIDs printable only") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("evil\x1b[2J", -40)};
  Result res;
  r.ctrl.connect("evil\x1b[2J", kPass, into(res));  // rejected: control character
  CHECK_FALSE(res.ok);
  for (const std::string& l : r.logs) {
    CHECK(l.find('\x1b') == std::string::npos);
  }
  CHECK(wifi_printable("a\x01" "b\n") == "a?b?");
  CHECK(wifi_printable("Caf\xC3\xA9") == "Caf\xC3\xA9");
}

// ------------------------------------------------------------------ hostile sequences

TEST_CASE("controller survives driver events nobody asked for") {
  Rig r;
  r.ctrl.load();
  r.ctrl.on_ip(0, "1.2.3.4");
  r.ctrl.on_ip(7, "1.2.3.4");
  r.ctrl.on_disconnected(7, 200);
  r.ctrl.on_disconnected(0, 0);
  r.ctrl.on_ip(7, std::string(500, '9'));
  r.advance(1000);
  CHECK_FALSE(r.ctrl.status().connected);
  CHECK(r.ctrl.saved().empty());
}

TEST_CASE("controller: a callback that connects, forgets and scans from inside another callback stays consistent") {
  Rig r;
  r.ctrl.load();
  r.radio.air = {ap("Home", -50)};
  Result res;
  int scanCalls = 0;
  r.ctrl.connect("Home", kPass, [&](bool ok) {
    res.calls++;
    res.ok = ok;
    r.ctrl.scan([&](std::vector<WifiNetwork>) { ++scanCalls; });
    r.ctrl.forget("Home");
    r.ctrl.status();
    r.ctrl.saved();
  });
  r.gotIp();
  CHECK(res.calls == 1);
  r.advance(500);
  CHECK(scanCalls == 1);
  CHECK_FALSE(r.ctrl.status().connected);
  CHECK(r.ctrl.saved().empty());
}

TEST_CASE("controller fuzz: random calls and driver events keep every callback exactly-once") {
  for (uint32_t seed = 1; seed <= 40; ++seed) {
    Rig r;
    uint32_t rng = seed * 2654435761u;
    auto next = [&](uint32_t n) {
      rng = rng * 1664525u + 1013904223u;
      return (rng >> 8) % n;
    };
    r.seed({{"Home", kPass}, {"Work", kPass2}});
    const char* names[] = {"Home", "Work", "Cafe", "Other", "OldWep", ""};
    r.radio.air = {ap("Home", -60), ap("Work", -70), ap("Cafe", -50, WifiAuth::Open),
                   ap("OldWep", -40, WifiAuth::Unsupported), ap("Other", -80)};

    std::vector<std::unique_ptr<int>> counters;  // one per request; must end at exactly 1
    auto counter = [&]() {
      counters.push_back(std::make_unique<int>(0));
      return counters.back().get();
    };
    // Which WifiService call is running right now ("" none). Callbacks check it:
    // apart from a replaced join (answered inside connect / connect_saved), none may
    // run from inside a service call.
    std::string current;
    int violations = 0;
    struct Call {
      std::string& cur;
      Call(std::string& c, const char* name) : cur(c) { cur = name; }
      ~Call() { cur.clear(); }
    };
    auto scanCb = [&](int* c) {
      return [c, &current, &violations](std::vector<WifiNetwork>) {
        ++*c;
        if (!current.empty()) ++violations;
      };
    };
    auto joinCb = [&](int* c) {
      return [c, &current, &violations](bool) {
        ++*c;
        if (!current.empty() && current != "connect" && current != "connect_saved") ++violations;
      };
    };
    for (int step = 0; step < 600; ++step) {
      switch (next(14)) {
        case 0: {
          int* c = counter();
          Call g(current, "scan");
          r.ctrl.scan(scanCb(c));
          break;
        }
        case 1:
        case 2: {
          int* c = counter();
          Call g(current, "connect");
          r.ctrl.connect(names[next(6)], next(3) == 0 ? "short" : kPass, joinCb(c));
          break;
        }
        case 3: {
          int* c = counter();
          Call g(current, "connect_saved");
          r.ctrl.connect_saved(names[next(6)], joinCb(c));
          break;
        }
        case 4: {
          Call g(current, "forget");
          r.ctrl.forget(names[next(6)]);
          break;
        }
        case 5:
          r.radio.up = true;
          r.ctrl.on_ip(next(3) == 0 ? next(30) : r.radio.lastGen(), "10.0.0.1");
          break;
        case 6:
        case 7:
          r.radio.up = false;
          r.ctrl.on_disconnected(next(3) == 0 ? next(30) : r.radio.lastGen(), static_cast<int>(next(4) * 100 + (next(2) ? 2 : 15)));
          break;
        case 8:
          r.radio.air.clear();
          if (next(2)) r.radio.air = {ap("Home", -60), ap("Work", -70)};
          break;
        case 9:
          r.radio.scanFails = next(4) == 0;
          r.radio.scanHangs = next(8) == 0;
          r.radio.startOk = next(5) != 0;
          r.radio.isReady = next(6) != 0;
          break;
        case 10:
          if (next(30) == 0) {
            Call g(current, "wipe");
            r.ctrl.wipe();
          }
          break;
        default:
          r.advance(next(4) * 700 + 10);
          break;
      }
      WifiStatus st;
      size_t savedCount;
      {
        Call g(current, "status");
        st = r.ctrl.status();
      }
      {
        Call g(current, "saved");
        savedCount = r.ctrl.saved().size();
      }
      if (st.connected) CHECK_FALSE(st.ssid.empty());
      CHECK(savedCount <= WifiStore::kMaxSaved);
    }
    // Let everything finish: timeouts answer what is still pending.
    r.radio.isReady = true;
    r.radio.startOk = true;
    r.radio.scanHangs = false;
    r.advance(40000);
    for (const auto& c : counters) {
      INFO("seed " << seed);
      CHECK(*c == 1);
    }
    INFO("seed " << seed);
    CHECK(violations == 0);
  }
}
