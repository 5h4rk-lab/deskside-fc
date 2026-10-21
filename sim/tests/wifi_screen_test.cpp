// Wi-Fi setup: pure helpers, MockWifi, and the network list / password page
// driven through MockWifi (synchronous unless a test says otherwise).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "mock_wifi.h"
#include "screens/wifi.h"
#include "test_display.h"
#include "theme.h"

namespace {

WifiNetwork net(const char* ssid, int rssi, bool open = false) {
  WifiNetwork n;
  n.ssid = ssid;
  n.rssi = rssi;
  n.open = open;
  return n;
}

std::vector<std::string> ssids(const std::vector<WifiNetwork>& nets) {
  std::vector<std::string> out;
  for (const WifiNetwork& n : nets) out.push_back(n.ssid);
  return out;
}

std::string text(lv_obj_t* label) { return lv_label_get_text(label); }
bool visible(lv_obj_t* obj) { return obj != nullptr && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }
bool disabled(lv_obj_t* obj) { return lv_obj_has_state(obj, LV_STATE_DISABLED); }

constexpr const char* kLongSsid = "Stadium-Fan-Zone-WiFi-Network-032";  // 33 bytes
constexpr const char* kSsid32 = "Stadium-Fan-Zone-WiFi-Network-32";     // 32 characters

// A Wi-Fi screen on the 240x320 test display, over a MockWifi that the test
// scripts before open().
struct Screen {
  MockWifi wifi;
  int backs = 0;
  int connected = 0;
  lv_obj_t* obj = nullptr;

  Screen() {
    init_test_display();
    theme_reset();
    wifi.set_networks({net("Home", -50), net("Cafe", -70, true), net("Office", -60)});
    wifi.set_password("Home", "homehome1");
    wifi.set_password("Office", "office-pass");
  }
  ~Screen() { deinit_test_display(); }

  void open(WifiMode mode = WifiMode::Normal) {
    WifiCallbacks cb;
    cb.onBack = [this] { ++backs; };
    cb.onConnected = [this] { ++connected; };
    obj = wifi_create(lv_screen_active(), wifi, mode, cb);
    settle();
  }
  void settle() {
    test_advance(20);
    lv_obj_update_layout(lv_screen_active());
  }
  void click(lv_obj_t* o) { lv_obj_send_event(o, LV_EVENT_CLICKED, nullptr); }
  const WifiParts* parts() const { return wifi_parts(obj); }
  const WifiPasswordParts* pw() const { return wifi_password_parts(obj); }
  std::vector<WifiNearbyRow> nearby() const { return wifi_nearby_rows(obj); }
  std::vector<WifiSavedRow> saved() const { return wifi_saved_rows(obj); }
  std::vector<std::string> nearby_names() const {
    std::vector<std::string> out;
    for (const WifiNearbyRow& r : nearby()) out.push_back(r.network.ssid);
    return out;
  }
  const WifiNearbyRow* nearby_row(const std::string& ssid) {
    rows_ = nearby();
    for (const WifiNearbyRow& r : rows_) {
      if (r.network.ssid == ssid) return &r;
    }
    return nullptr;
  }
  const WifiSavedRow* saved_row(const std::string& ssid) {
    savedRows_ = saved();
    for (const WifiSavedRow& r : savedRows_) {
      if (r.ssid == ssid) return &r;
    }
    return nullptr;
  }
  void type(const char* s) { lv_textarea_set_text(pw()->field, s); }

 private:
  std::vector<WifiNearbyRow> rows_;
  std::vector<WifiSavedRow> savedRows_;
};

}  // namespace

// ------------------------------------------------------------ pure helpers

TEST_CASE("wifi: signal bars thresholds") {
  CHECK(wifi_signal_bars(-30) == 4);
  CHECK(wifi_signal_bars(-55) == 4);
  CHECK(wifi_signal_bars(-56) == 3);
  CHECK(wifi_signal_bars(-65) == 3);
  CHECK(wifi_signal_bars(-66) == 2);
  CHECK(wifi_signal_bars(-75) == 2);
  CHECK(wifi_signal_bars(-76) == 1);
  CHECK(wifi_signal_bars(-100) == 1);
}

TEST_CASE("wifi: nearby list is sorted, deduplicated, without hidden or saved networks") {
  const std::vector<WifiNetwork> scanned = {
      net("Weak", -80), net("Dup", -72), net("", -40), net("Strong", -45),
      net("Dup", -58), net("Saved", -50), net("Mid", -60), net("Dup", -66),
  };
  const std::vector<WifiNetwork> out = wifi_nearby(scanned, {"Saved"});
  CHECK(ssids(out) == std::vector<std::string>{"Strong", "Dup", "Mid", "Weak"});
  CHECK(out[1].rssi == -58);  // the strongest of the three
}

TEST_CASE("wifi: status text") {
  WifiStatus s;
  CHECK(wifi_status_text(s) == "Not connected");
  s.connected = true;
  s.ssid = "Home";
  CHECK(wifi_status_text(s) == "Connected to Home");
}

// ------------------------------------------------------------ MockWifi

TEST_CASE("mock wifi: a successful connect saves most recently used first, replacing the same SSID") {
  MockWifi w;
  w.set_networks({net("A", -50), net("B", -60), net("C", -70, true)});
  w.set_password("A", "aaaaaaaa");
  w.set_password("B", "bbbbbbbb");
  bool ok = false;
  w.connect("A", "aaaaaaaa", [&](bool r) { ok = r; });
  CHECK(ok);
  w.connect("B", "bbbbbbbb", [&](bool r) { ok = r; });
  w.connect("C", "", [&](bool r) { ok = r; });
  CHECK(w.saved() == std::vector<std::string>{"C", "B", "A"});
  w.connect("A", "aaaaaaaa", [&](bool r) { ok = r; });
  CHECK(w.saved() == std::vector<std::string>{"A", "C", "B"});
  CHECK(w.status().connected);
  CHECK(w.status().ssid == "A");
  CHECK_FALSE(w.status().ip.empty());
  // A wrong password fails, saves nothing and keeps the connection.
  w.connect("B", "wrongpass", [&](bool r) { ok = r; });
  CHECK_FALSE(ok);
  CHECK(w.saved() == std::vector<std::string>{"A", "C", "B"});
  CHECK(w.status().ssid == "A");
  // connect_saved uses the stored password and moves it to the front.
  w.connect_saved("B", [&](bool r) { ok = r; });
  CHECK(ok);
  CHECK(w.saved().front() == "B");
  CHECK(w.status().ssid == "B");
}

TEST_CASE("mock wifi: keeps at least 9 (up to 16) saved networks; forget removes one") {
  MockWifi w;
  std::vector<WifiNetwork> nets;
  for (int i = 0; i < 20; ++i) nets.push_back(net(("N" + std::to_string(i)).c_str(), -60, true));
  w.set_networks(nets);
  for (int i = 0; i < 9; ++i) w.connect("N" + std::to_string(i), "", [](bool) {});
  CHECK(w.saved().size() == 9);
  for (int i = 9; i < 20; ++i) w.connect("N" + std::to_string(i), "", [](bool) {});
  CHECK(w.saved().size() == MockWifi::kCapacity);
  CHECK(MockWifi::kCapacity >= kMinSavedNetworks);
  CHECK(w.saved().front() == "N19");
  w.forget("N19");
  CHECK(w.saved().size() == MockWifi::kCapacity - 1);
  CHECK(w.saved().front() == "N18");
  CHECK_FALSE(w.status().connected);  // forgetting the joined network leaves it
}

TEST_CASE("mock wifi: async results arrive on poll after their latency") {
  MockWifi w;
  w.set_networks({net("A", -50, true)});
  w.set_async(true);
  int scans = 0;
  bool joined = false;
  w.scan([&](std::vector<WifiNetwork> n) { scans += static_cast<int>(n.size()); });
  w.connect("A", "", [&](bool ok) { joined = ok; });
  CHECK(w.pending() == 2);
  w.poll(MockWifi::kDefaultScanMs - 1);
  CHECK(scans == 0);
  w.poll(1);
  CHECK(scans == 1);
  CHECK_FALSE(joined);
  CHECK(w.saved().empty());  // saved at delivery
  w.poll(MockWifi::kDefaultConnectMs);
  CHECK(joined);
  CHECK(w.saved() == std::vector<std::string>{"A"});
  CHECK(w.pending() == 0);
}

TEST_CASE("mock wifi: a new join replaces a pending one, which fails at once and saves nothing") {
  MockWifi w;
  w.set_networks({net("A", -50), net("B", -60, true), net("C", -55)});
  w.set_password("A", "aaaaaaaa");
  w.set_password("C", "cccccccc");
  w.set_async(true);
  std::vector<std::string> answers;
  w.connect("A", "aaaaaaaa", [&](bool ok) { answers.push_back(std::string("A") + (ok ? "+" : "-")); });
  CHECK(answers.empty());
  w.connect("B", "", [&](bool ok) { answers.push_back(std::string("B") + (ok ? "+" : "-")); });
  // A is answered false at the moment of replacement.
  CHECK(answers == std::vector<std::string>{"A-"});
  CHECK(w.pending() == 1);
  w.poll(MockWifi::kDefaultConnectMs * 2);
  CHECK(answers == std::vector<std::string>{"A-", "B+"});
  CHECK(w.saved() == std::vector<std::string>{"B"});
  CHECK(w.status().ssid == "B");

  // connect_saved replaces a pending connect too; scans are untouched.
  int scans = 0;
  w.scan([&](std::vector<WifiNetwork>) { ++scans; });
  w.connect("C", "cccccccc", [&](bool ok) { answers.push_back(std::string("C") + (ok ? "+" : "-")); });
  w.connect_saved("B", [&](bool ok) { answers.push_back(std::string("b") + (ok ? "+" : "-")); });
  CHECK(answers.back() == "C-");
  w.poll(MockWifi::kDefaultConnectMs * 2);
  CHECK(scans == 1);
  CHECK(answers == std::vector<std::string>{"A-", "B+", "C-", "b+"});
  CHECK(w.saved() == std::vector<std::string>{"B"});
  CHECK(w.pending() == 0);
}

TEST_CASE("mock wifi: every join is answered exactly once, sync or async") {
  for (bool async : {false, true}) {
    MockWifi w;
    w.set_networks({net("A", -50, true), net("B", -60, true)});
    w.set_async(async);
    int a = 0;
    int b = 0;
    int c = 0;
    w.connect("A", "", [&](bool) { ++a; });
    w.connect("B", "", [&](bool) { ++b; });
    w.connect_saved("A", [&](bool) { ++c; });
    w.poll(10000);
    w.poll(10000);
    CHECK(a == 1);
    CHECK(b == 1);
    CHECK(c == 1);
  }
}

TEST_CASE("mock wifi: auto_join picks the strongest saved network in range") {
  MockWifi w;
  w.set_networks({net("A", -70), net("B", -50), net("C", -40)});
  w.set_password("A", "aaaaaaaa");
  w.set_password("B", "bbbbbbbb");
  w.set_saved({"A", "B", "Gone"});
  CHECK(w.auto_join());
  CHECK(w.status().ssid == "B");
}

// ------------------------------------------------------------ network list

TEST_CASE("wifi screen: nearby rows sorted by signal, deduplicated, saved ones only under Saved") {
  Screen s;
  s.wifi.set_networks({net("Weak", -82), net("Home", -50), net("Dup", -70), net("", -30),
                       net("Dup", -52), net("Cafe", -64, true)});
  s.wifi.set_saved({"Home"});
  s.open();
  CHECK(s.nearby_names() == std::vector<std::string>{"Dup", "Cafe", "Weak"});
  REQUIRE(s.saved().size() == 1);
  CHECK(s.saved()[0].ssid == "Home");
  CHECK(wifi_bars_level(s.nearby()[0].bars) == 4);  // -52
  CHECK(wifi_bars_level(s.nearby()[1].bars) == 3);  // -64
  CHECK(wifi_bars_level(s.nearby()[2].bars) == 1);  // -82
  // Lock on secured networks only.
  CHECK(visible(s.nearby()[0].lock));
  CHECK_FALSE(visible(s.nearby()[1].lock));
  CHECK(visible(s.parts()->savedTitle));
  CHECK(text(s.parts()->savedTitle) == "Saved networks");
  CHECK(text(s.parts()->nearbyTitle) == "Nearby networks");
}

TEST_CASE("wifi screen: no Saved section when nothing is saved") {
  Screen s;
  s.open();
  CHECK(s.saved().empty());
  CHECK_FALSE(visible(s.parts()->savedTitle));
  CHECK(text(s.parts()->title) == "Wi-Fi");
  CHECK_FALSE(visible(s.parts()->help));
}

TEST_CASE("wifi screen: status card texts") {
  SUBCASE("not connected") {
    Screen s;
    s.open();
    CHECK(text(s.parts()->statusText) == "Not connected");
    CHECK_FALSE(visible(s.parts()->statusBars));
    CHECK_FALSE(visible(s.parts()->statusSpinner));
  }
  SUBCASE("connected: SSID, signal and IP") {
    Screen s;
    s.wifi.set_saved({"Home"});
    REQUIRE(s.wifi.auto_join());
    s.open();
    CHECK(text(s.parts()->statusText) == "Connected to Home");
    CHECK(visible(s.parts()->statusBars));
    CHECK(wifi_bars_level(s.parts()->statusBars) == 4);
    CHECK(visible(s.parts()->statusDetail));
    CHECK(text(s.parts()->statusDetail) == s.wifi.status().ip);
  }
  SUBCASE("connecting") {
    Screen s;
    s.wifi.set_saved({"Office"});
    s.wifi.set_async(true);
    s.open();
    s.wifi.poll(MockWifi::kDefaultScanMs);
    s.click(s.saved_row("Office")->row);
    CHECK(text(s.parts()->statusText) == "Connecting to Office...");
    CHECK(visible(s.parts()->statusSpinner));
    s.wifi.poll(MockWifi::kDefaultConnectMs);
    CHECK(text(s.parts()->statusText) == "Connected to Office");
    CHECK_FALSE(visible(s.parts()->statusSpinner));
    CHECK(s.connected == 1);
  }
}

TEST_CASE("wifi screen: scan states") {
  Screen s;
  s.wifi.set_async(true);
  s.open();
  CHECK(s.wifi.scanCalls == 1);
  CHECK(visible(s.parts()->message));
  CHECK(text(s.parts()->message) == "Looking for networks...");
  CHECK(visible(s.parts()->messageSpinner));
  CHECK(s.nearby().empty());
  // A second request while scanning is ignored.
  s.click(s.parts()->scanAgain);
  wifi_scan(s.obj);
  CHECK(s.wifi.scanCalls == 1);
  s.wifi.poll(MockWifi::kDefaultScanMs);
  CHECK_FALSE(visible(s.parts()->message));
  CHECK(s.nearby().size() == 3);

  // Nothing in range.
  s.wifi.set_networks({});
  s.click(s.parts()->scanAgain);
  CHECK(s.wifi.scanCalls == 2);
  CHECK(text(s.parts()->message) == "Looking for networks...");
  s.wifi.poll(MockWifi::kDefaultScanMs);
  CHECK(visible(s.parts()->message));
  CHECK(text(s.parts()->message) ==
        "No networks found. Move closer to your router and scan again.");
  CHECK_FALSE(visible(s.parts()->messageSpinner));
  CHECK(s.nearby().empty());
}

TEST_CASE("wifi screen: a scan result after the screen is gone is ignored") {
  Screen s;
  s.wifi.set_async(true);
  s.open();
  lv_obj_delete(s.obj);
  s.obj = nullptr;
  s.wifi.poll(MockWifi::kDefaultScanMs);
  s.settle();
  CHECK(s.wifi.pending() == 0);
}

TEST_CASE("wifi screen: a 32-character SSID is cut with dots on one fixed line") {
  Screen s;
  s.wifi.set_networks({net(kSsid32, -60), net(kLongSsid, -70, true)});
  s.wifi.set_saved({kSsid32});
  s.wifi.set_password(kSsid32, "longlonglong");
  s.open();
  s.settle();
  const WifiNearbyRow* r = s.nearby_row(kLongSsid);
  REQUIRE(r != nullptr);
  const WifiSavedRow* sr = s.saved_row(kSsid32);
  REQUIRE(sr != nullptr);
  for (lv_obj_t* name : {r->name, sr->name}) {
    CHECK(lv_label_get_long_mode(name) == LV_LABEL_LONG_DOT);
    CHECK(lv_obj_get_height(name) == lv_font_get_line_height(lv_obj_get_style_text_font(name, 0)));
    CHECK(lv_obj_get_width(name) <= 160);
    const std::string shown = text(name);
    CHECK(shown.size() >= 3);
    CHECK(shown.substr(shown.size() - 3) == "...");
    // Inside its row.
    lv_area_t a;
    lv_area_t row;
    lv_obj_get_coords(name, &a);
    lv_obj_get_coords(lv_obj_get_parent(name), &row);
    CHECK(a.x2 <= row.x2);
  }
}

TEST_CASE("wifi screen: UTF-8 SSIDs are shown as they are") {
  Screen s;
  s.wifi.set_networks({net("Caf\xC3\xA9 M\xC3\xBCller", -60, true)});
  s.open();
  REQUIRE(s.nearby().size() == 1);
  CHECK(text(s.nearby()[0].name) == "Caf\xC3\xA9 M\xC3\xBCller");
}

// ------------------------------------------------------------ joining

TEST_CASE("wifi screen: tapping an open network connects without the keyboard") {
  Screen s;
  s.open();
  s.click(s.nearby_row("Cafe")->row);
  CHECK(s.wifi.connectCalls == 1);
  CHECK(s.wifi.lastConnectSsid == "Cafe");
  CHECK(s.pw() == nullptr);
  s.settle();
  CHECK(text(s.parts()->statusText) == "Connected to Cafe");
  CHECK(s.connected == 1);
  // Now saved, so listed under Saved and no longer under Nearby.
  CHECK(s.saved_row("Cafe") != nullptr);
  CHECK(s.nearby_row("Cafe") == nullptr);
}

TEST_CASE("wifi screen: an open network that fails says so in amber") {
  Screen s;
  s.open();
  s.wifi.set_networks({net("Home", -50)});  // Cafe went out of range
  s.click(s.nearby_row("Cafe")->row);
  CHECK(text(s.parts()->statusText) == "Not connected");
  REQUIRE(visible(s.parts()->statusDetail));
  CHECK(text(s.parts()->statusDetail) == "Could not connect to Cafe.");
  CHECK(lv_color_to_u32(lv_obj_get_style_text_color(s.parts()->statusDetail, 0)) ==
        lv_color_to_u32(theme().amber));
}

TEST_CASE("wifi screen: tapping a secured network opens the password page with the keyboard") {
  Screen s;
  s.open();
  s.click(s.nearby_row("Office")->row);
  s.settle();
  CHECK(s.wifi.connectCalls == 0);
  const WifiPasswordParts* p = s.pw();
  REQUIRE(p != nullptr);
  CHECK(p->ssid == "Office");
  CHECK(text(p->title) == "Office");
  CHECK(visible(p->keyboard));
  CHECK(lv_keyboard_get_textarea(p->keyboard) == p->field);
  CHECK(lv_textarea_get_password_mode(p->field));
  CHECK(std::string(lv_textarea_get_placeholder_text(p->field)) == "Password");
  // Covers the list.
  CHECK(lv_obj_get_height(p->page) == 320);
}

TEST_CASE("wifi screen: Connect is enabled from 8 characters") {
  Screen s;
  s.open();
  wifi_open_password(s.obj, "Office");
  const WifiPasswordParts* p = s.pw();
  REQUIRE(p != nullptr);
  CHECK(disabled(p->connect));
  CHECK(visible(p->hint));
  CHECK(text(p->hint) == "At least 8 characters");
  s.type("1234567");
  CHECK(disabled(p->connect));
  s.click(p->connect);  // ignored while disabled
  CHECK(s.wifi.connectCalls == 0);
  s.type("12345678");
  CHECK_FALSE(disabled(p->connect));
  CHECK_FALSE(visible(p->hint));
}

TEST_CASE("wifi screen: Show and Hide toggle the password mode") {
  Screen s;
  s.open();
  wifi_open_password(s.obj, "Office");
  const WifiPasswordParts* p = s.pw();
  CHECK(text(p->showLabel) == "Show");
  s.click(p->show);
  CHECK_FALSE(lv_textarea_get_password_mode(p->field));
  CHECK(text(p->showLabel) == "Hide");
  s.click(p->show);
  CHECK(lv_textarea_get_password_mode(p->field));
  CHECK(text(p->showLabel) == "Show");
}

TEST_CASE("wifi screen: a wrong password keeps the keyboard and the text") {
  Screen s;
  s.open();
  wifi_open_password(s.obj, "Office");
  const WifiPasswordParts* p = s.pw();
  s.type("not-the-password");
  s.click(p->connect);
  s.settle();
  CHECK(s.wifi.connectCalls == 1);
  REQUIRE(s.pw() != nullptr);
  CHECK(visible(p->error));
  CHECK(text(p->error) == "Could not connect. Check the password.");
  CHECK(lv_color_to_u32(lv_obj_get_style_text_color(p->error, 0)) ==
        lv_color_to_u32(theme().amber));
  CHECK(visible(p->keyboard));
  CHECK_FALSE(disabled(p->keyboard));
  CHECK_FALSE(disabled(p->field));
  CHECK(std::string(lv_textarea_get_text(p->field)) == "not-the-password");
  CHECK_FALSE(disabled(p->connect));
  CHECK(text(p->connectLabel) == "Connect");
  CHECK(s.connected == 0);
  // Editing the text clears the message.
  s.type("office-pas");
  CHECK_FALSE(visible(p->error));
}

TEST_CASE("wifi screen: the right password shows Connected, then the list") {
  Screen s;
  s.open();
  wifi_open_password(s.obj, "Office");
  const WifiPasswordParts* p = s.pw();
  s.type("office-pass");
  s.click(p->connect);
  CHECK(text(p->connectLabel) == "Connected");
  CHECK(s.pw() != nullptr);
  s.settle();
  CHECK(s.pw() != nullptr);  // still showing "Connected"
  test_advance(kWifiConnectedHoldMs + 50);
  s.settle();
  CHECK(s.pw() == nullptr);
  CHECK(text(s.parts()->statusText) == "Connected to Office");
  CHECK(s.saved_row("Office") != nullptr);
  CHECK(visible(s.saved_row("Office")->tick));
  CHECK(s.connected == 1);
}

TEST_CASE("wifi screen: first setup reports the join after the pause") {
  Screen s;
  s.open(WifiMode::FirstSetup);
  CHECK(text(s.parts()->title) == "Connect to Wi-Fi");
  CHECK(visible(s.parts()->help));
  CHECK(text(s.parts()->help) == "Pick your network to get live scores.");
  CHECK(visible(s.parts()->back));
  wifi_open_password(s.obj, "Home");
  s.type("homehome1");
  s.click(s.pw()->connect);
  s.settle();
  CHECK(s.connected == 0);
  test_advance(kWifiConnectedHoldMs + 50);
  CHECK(s.connected == 1);
}

TEST_CASE("wifi screen: while connecting the field and keyboard are disabled and a spinner shows") {
  Screen s;
  s.wifi.set_async(true);
  s.open();
  s.wifi.poll(MockWifi::kDefaultScanMs);
  wifi_open_password(s.obj, "Office");
  const WifiPasswordParts* p = s.pw();
  s.type("office-pass");
  s.click(p->connect);
  CHECK(text(p->connectLabel) == "Connecting...");
  CHECK(visible(p->connectSpinner));
  CHECK(disabled(p->field));
  CHECK(disabled(p->keyboard));
  CHECK(disabled(p->connect));
  s.click(p->connect);  // no second request
  CHECK(s.wifi.connectCalls == 1);
  s.wifi.poll(MockWifi::kDefaultConnectMs);
  CHECK(text(p->connectLabel) == "Connected");
  CHECK_FALSE(visible(p->connectSpinner));
}

TEST_CASE("wifi screen: Back while connecting, then a late success, only updates the status") {
  Screen s;
  s.wifi.set_async(true);
  s.open();
  s.wifi.poll(MockWifi::kDefaultScanMs);
  wifi_open_password(s.obj, "Office");
  s.type("office-pass");
  s.click(s.pw()->connect);
  s.click(s.pw()->back);
  s.settle();
  CHECK(s.pw() == nullptr);
  // The device is still trying: the card says so until the answer arrives.
  CHECK(text(s.parts()->statusText) == "Connecting to Office...");
  CHECK(visible(s.parts()->statusSpinner));
  s.wifi.poll(MockWifi::kDefaultConnectMs);
  s.settle();
  test_advance(kWifiConnectedHoldMs + 50);
  // No page, no "Connected" pause, no callback: the card and lists follow
  // the device.
  CHECK(s.pw() == nullptr);
  CHECK(s.connected == 0);
  CHECK(s.backs == 0);
  CHECK(text(s.parts()->statusText) == "Connected to Office");
  CHECK_FALSE(visible(s.parts()->statusSpinner));
  CHECK(s.saved_row("Office") != nullptr);
}

TEST_CASE("wifi screen: Back while connecting, then an open network: the second join wins") {
  Screen s;
  s.wifi.set_async(true);
  s.open();
  s.wifi.poll(MockWifi::kDefaultScanMs);
  wifi_open_password(s.obj, "Office");
  s.type("office-pass");
  s.click(s.pw()->connect);
  s.click(s.pw()->back);
  s.settle();
  CHECK(text(s.parts()->statusText) == "Connecting to Office...");
  s.click(s.nearby_row("Cafe")->row);
  CHECK(s.wifi.connectCalls == 2);
  // Office was replaced (answered false at once); Cafe is the attempt now.
  CHECK(text(s.parts()->statusText) == "Connecting to Cafe...");
  CHECK(s.wifi.pending() == 1);
  s.wifi.poll(MockWifi::kDefaultConnectMs);
  s.settle();
  CHECK(text(s.parts()->statusText) == "Connected to Cafe");
  CHECK_FALSE(visible(s.parts()->statusSpinner));
  CHECK(s.wifi.saved() == std::vector<std::string>{"Cafe"});
  CHECK(s.connected == 1);
  CHECK(s.pw() == nullptr);
}

TEST_CASE("wifi screen: Back while connecting, reopen and Connect again works") {
  Screen s;
  s.wifi.set_async(true);
  s.open();
  s.wifi.poll(MockWifi::kDefaultScanMs);
  wifi_open_password(s.obj, "Office");
  s.type("office-pass");
  s.click(s.pw()->connect);
  s.click(s.pw()->back);
  s.settle();
  wifi_open_password(s.obj, "Office");
  REQUIRE(s.pw() != nullptr);
  s.type("office-pass");
  CHECK_FALSE(disabled(s.pw()->connect));
  s.click(s.pw()->connect);
  CHECK(s.wifi.connectCalls == 2);
  CHECK(text(s.pw()->connectLabel) == "Connecting...");
  s.wifi.poll(MockWifi::kDefaultConnectMs);
  REQUIRE(s.pw() != nullptr);
  CHECK(text(s.pw()->connectLabel) == "Connected");
  CHECK_FALSE(visible(s.pw()->error));
  test_advance(kWifiConnectedHoldMs + 50);
  s.settle();
  CHECK(s.pw() == nullptr);
  CHECK(text(s.parts()->statusText) == "Connected to Office");
  CHECK(s.wifi.saved() == std::vector<std::string>{"Office"});
  CHECK(s.connected == 1);
}

TEST_CASE("wifi screen: a quick tap on a second network while joining is ignored") {
  Screen s;
  s.wifi.set_networks({net("Cafe", -70, true), net("Library", -60, true)});
  s.wifi.set_async(true);
  s.open();
  s.wifi.poll(MockWifi::kDefaultScanMs);
  lv_obj_t* cafe = s.nearby_row("Cafe")->row;
  lv_obj_t* library = s.nearby_row("Library")->row;
  s.click(cafe);
  s.click(library);
  CHECK(s.wifi.connectCalls == 1);
  CHECK(s.wifi.lastConnectSsid == "Cafe");
  CHECK(text(s.parts()->statusText) == "Connecting to Cafe...");
  s.wifi.poll(MockWifi::kDefaultConnectMs);
  CHECK(text(s.parts()->statusText) == "Connected to Cafe");
}

TEST_CASE("wifi screen: forgetting the connected network shows Not connected") {
  Screen s;
  s.wifi.set_saved({"Home"});
  REQUIRE(s.wifi.auto_join());
  s.open();
  CHECK(text(s.parts()->statusText) == "Connected to Home");
  const WifiSavedRow* r = s.saved_row("Home");
  REQUIRE(r != nullptr);
  s.click(r->forget);
  s.click(r->confirmForget);
  wifi_refresh(s.obj);
  s.settle();
  CHECK(text(s.parts()->statusText) == "Not connected");
  CHECK_FALSE(visible(s.parts()->statusBars));
  CHECK_FALSE(visible(s.parts()->statusDetail));
}

TEST_CASE("wifi screen: the typed password is cleared when the page closes") {
  Screen s;
  s.open();
  wifi_open_password(s.obj, "Office");
  lv_obj_t* field = s.pw()->field;
  s.type("secret-typed");
  s.click(s.pw()->back);
  // Cleared at once, before the page goes on the next cycle.
  CHECK(std::string(lv_textarea_get_text(field)) == "");
  s.settle();
  CHECK(s.pw() == nullptr);
  // Opening it again starts empty.
  wifi_open_password(s.obj, "Office");
  CHECK(std::string(lv_textarea_get_text(s.pw()->field)) == "");
}

TEST_CASE("wifi screen: Back on the list calls onBack") {
  Screen s;
  s.open();
  s.click(s.parts()->back);
  CHECK(s.backs == 1);
}

// ------------------------------------------------------------ saved networks

TEST_CASE("wifi screen: every saved network is listed (9 saved, 9 rows)") {
  Screen s;
  std::vector<std::string> nine;
  for (int i = 1; i <= 9; ++i) nine.push_back("Net " + std::to_string(i));
  s.wifi.set_saved(nine);
  s.open();
  REQUIRE(s.saved().size() == 9);
  for (int i = 0; i < 9; ++i) {
    CHECK(s.saved()[i].ssid == nine[i]);
    CHECK(text(s.saved()[i].tag) == "Saved");
  }
}

TEST_CASE("wifi screen: the connected saved network has the tick") {
  Screen s;
  s.wifi.set_saved({"Office", "Home"});
  REQUIRE(s.wifi.auto_join());  // Home is stronger
  s.open();
  CHECK(visible(s.saved_row("Home")->tick));
  CHECK_FALSE(visible(s.saved_row("Office")->tick));
}

TEST_CASE("wifi screen: tapping a saved network joins it with the stored password") {
  Screen s;
  s.wifi.set_saved({"Office", "Home"});
  s.open();
  s.click(s.saved_row("Office")->row);
  CHECK(s.wifi.connectSavedCalls == 1);
  CHECK(s.wifi.connectCalls == 0);
  CHECK(s.wifi.lastConnectSsid == "Office");
  CHECK(s.pw() == nullptr);
  s.settle();
  CHECK(text(s.parts()->statusText) == "Connected to Office");
  CHECK(visible(s.saved_row("Office")->tick));
}

TEST_CASE("wifi screen: Forget asks inline; Cancel keeps the network, Forget removes it") {
  Screen s;
  s.wifi.set_saved({"Office", "Home"});
  s.open();
  const WifiSavedRow* r = s.saved_row("Office");
  REQUIRE(r != nullptr);
  CHECK(text(r->forget) == "Forget");
  CHECK_FALSE(visible(r->confirm));
  s.click(r->forget);
  CHECK(s.wifi.connectSavedCalls == 0);  // not a tap on the row
  CHECK(visible(r->confirm));
  CHECK(text(r->confirmText) == "Forget Office?");
  CHECK(s.wifi.forgetCalls == 0);

  s.click(r->confirmCancel);
  CHECK_FALSE(visible(r->confirm));
  CHECK(s.saved().size() == 2);
  CHECK(s.wifi.forgetCalls == 0);

  s.click(r->forget);
  s.click(r->confirmForget);
  s.settle();
  CHECK(s.wifi.forgetCalls == 1);
  CHECK(s.wifi.saved() == std::vector<std::string>{"Home"});
  REQUIRE(s.saved().size() == 1);
  CHECK(s.saved()[0].ssid == "Home");
  // Forgotten but in range: back under Nearby.
  CHECK(s.nearby_row("Office") != nullptr);
}

TEST_CASE("wifi screen: refresh picks up changes made by the service") {
  Screen s;
  s.open();
  CHECK(s.saved().empty());
  s.wifi.set_saved({"Home"});
  s.wifi.auto_join();
  wifi_refresh(s.obj);
  s.settle();
  CHECK(s.saved().size() == 1);
  CHECK(text(s.parts()->statusText) == "Connected to Home");
  CHECK(s.nearby_row("Home") == nullptr);
}

TEST_CASE("wifi screen: every typed character is masked at once") {
  Screen s;
  s.open();
  wifi_open_password(s.obj, "Office");
  const WifiPasswordParts* p = s.pw();
  REQUIRE(p != nullptr);
  CHECK(lv_textarea_get_password_show_time(p->field) == 0);
  // Show/Hide still works.
  s.click(p->show);
  CHECK_FALSE(lv_textarea_get_password_mode(p->field));
}
