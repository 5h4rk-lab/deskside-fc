// Wi-Fi setup: the network list (status, saved networks, nearby networks)
// and the password page with the on-screen keyboard, over a WifiService.
// The app hosts it as a full-screen overlay (the password page needs the
// keyboard), opened from Settings or, at first start without a saved
// network, in first-setup mode. Shared with the ESP32 firmware: no clock
// reads, no SDL, no stdio. The typed password is never stored or logged: it
// lives in the text field until it goes to WifiService::connect(), and the
// field is cleared when the page closes.
#pragma once

#include <lvgl.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "wifi_service.h"

// Texts (exact wording). "Connected to " + ssid, "Connecting to " + ssid + "...",
// "Forget " + ssid + "?".
constexpr const char* kWifiTitle = "Wi-Fi";
constexpr const char* kWifiFirstTitle = "Connect to Wi-Fi";
constexpr const char* kWifiFirstHelp = "Pick your network to get live scores.";
constexpr const char* kWifiNotConnected = "Not connected";
constexpr const char* kWifiConnectedPrefix = "Connected to ";
constexpr const char* kWifiConnectingPrefix = "Connecting to ";
constexpr const char* kWifiSavedTitle = "Saved networks";
constexpr const char* kWifiNearbyTitle = "Nearby networks";
constexpr const char* kWifiSavedTag = "Saved";
constexpr const char* kWifiForget = "Forget";
constexpr const char* kWifiCancel = "Cancel";
constexpr const char* kWifiScanning = "Looking for networks...";
constexpr const char* kWifiNoNetworks = "No networks found. Move closer to your router and scan again.";
constexpr const char* kWifiScanAgain = "Scan again";
constexpr const char* kWifiJoinFailedPrefix = "Could not connect to ";  // + ssid + "."
constexpr const char* kWifiPasswordPlaceholder = "Password";
constexpr const char* kWifiShow = "Show";
constexpr const char* kWifiHide = "Hide";
constexpr const char* kWifiPasswordHint = "At least 8 characters";
constexpr const char* kWifiWrongPassword = "Could not connect. Check the password.";
constexpr const char* kWifiConnect = "Connect";
constexpr const char* kWifiConnectingButton = "Connecting...";
constexpr const char* kWifiConnectedButton = "Connected";

// WPA2-Personal passphrases are 8 to 63 characters.
constexpr int kWifiMinPassword = 8;
constexpr int kWifiMaxPassword = 63;
// How long "Connected" stays before the page moves on.
constexpr uint32_t kWifiConnectedHoldMs = 1000;

// "Connected to <ssid>" or "Not connected" (also the Settings row's text).
std::string wifi_status_text(const WifiStatus& status);

// Signal bars (1 to 4) for a signal strength in dBm: >= -55 four, >= -65
// three, >= -75 two, anything weaker one.
int wifi_signal_bars(int rssi);

// The Nearby section of a scan: hidden networks (empty SSID) dropped, one
// entry per SSID (the strongest), networks in `saved` left out, strongest
// first (ties keep scan order).
std::vector<WifiNetwork> wifi_nearby(const std::vector<WifiNetwork>& scanned,
                                     const std::vector<std::string>& saved);

enum class WifiMode {
  Normal,      // opened from Settings
  FirstSetup,  // first start without a saved network: welcome title and text
};

struct WifiCallbacks {
  std::function<void()> onBack;  // Back on the network list
  // After every successful join made on these pages. In first-setup mode it
  // comes after the "Connected" pause and the host closes the pages.
  std::function<void()> onConnected;
  // After every successful join started on these pages, as soon as the
  // service answers: also when the user has left the page or the screen is
  // gone by then (the function is copied into the service's callback, so it
  // must stay safe to call; guard it with the host's own liveness). Never for
  // failures or replaced attempts. Called after the page has handled the
  // result.
  std::function<void()> onJoinSucceeded;
};

// Builds the list as a child of `parent`, filling it, and starts a scan.
// `wifi` must outlive the screen; results that arrive after the screen is
// deleted are ignored.
lv_obj_t* wifi_create(lv_obj_t* parent, WifiService& wifi, WifiMode mode,
                      const WifiCallbacks& callbacks);

// Re-reads the status and the saved list (cheap when nothing changed; the
// app calls it on refresh).
void wifi_refresh(lv_obj_t* screen);

// Scans again, as the "Scan again" button. Ignored while a scan is running.
void wifi_scan(lv_obj_t* screen);

// Opens the password page for a secured network, as tapping its row.
void wifi_open_password(lv_obj_t* screen, const std::string& ssid);

// Widgets, for tests and screenshots.
struct WifiSavedRow {
  std::string ssid;
  lv_obj_t* row = nullptr;   // tap: connect_saved()
  lv_obj_t* name = nullptr;  // one line, "..." when too long
  lv_obj_t* tag = nullptr;   // "Saved"
  lv_obj_t* tick = nullptr;  // shown on the connected network only
  lv_obj_t* forget = nullptr;          // small "Forget" button: asks first
  lv_obj_t* confirm = nullptr;         // inline question, hidden until Forget is tapped
  lv_obj_t* confirmText = nullptr;     // "Forget <ssid>?"
  lv_obj_t* confirmForget = nullptr;   // calls forget()
  lv_obj_t* confirmCancel = nullptr;
};

struct WifiNearbyRow {
  WifiNetwork network;
  lv_obj_t* row = nullptr;   // tap: connect (open) or the password page (secured)
  lv_obj_t* name = nullptr;  // one line, "..." when too long
  lv_obj_t* lock = nullptr;  // secured networks only
  lv_obj_t* bars = nullptr;  // see wifi_bars_level()
};

struct WifiParts {
  lv_obj_t* back = nullptr;
  lv_obj_t* title = nullptr;  // "Wi-Fi" or, first setup, "Connect to Wi-Fi"
  lv_obj_t* help = nullptr;   // first setup only: kWifiFirstHelp
  lv_obj_t* list = nullptr;   // scrolling column

  lv_obj_t* statusCard = nullptr;
  lv_obj_t* statusText = nullptr;     // connected / not connected / connecting
  lv_obj_t* statusDetail = nullptr;   // the IP address, or why a join failed (amber)
  lv_obj_t* statusBars = nullptr;     // while connected
  lv_obj_t* statusSpinner = nullptr;  // while connecting

  lv_obj_t* savedTitle = nullptr;  // hidden when nothing is saved
  lv_obj_t* nearbyTitle = nullptr;
  lv_obj_t* scanAgain = nullptr;
  lv_obj_t* message = nullptr;  // scanning or no networks; hidden over results
  lv_obj_t* messageSpinner = nullptr;
};

struct WifiPasswordParts {
  std::string ssid;
  lv_obj_t* page = nullptr;
  lv_obj_t* back = nullptr;
  lv_obj_t* title = nullptr;  // the SSID, "..." when too long
  lv_obj_t* field = nullptr;  // lv_textarea, one line, password mode
  lv_obj_t* show = nullptr;   // "Show" / "Hide" toggle
  lv_obj_t* showLabel = nullptr;
  lv_obj_t* hint = nullptr;   // kWifiPasswordHint while too short
  lv_obj_t* error = nullptr;  // kWifiWrongPassword after a failed join
  lv_obj_t* connect = nullptr;  // LV_STATE_DISABLED until 8 characters
  lv_obj_t* connectLabel = nullptr;
  lv_obj_t* connectSpinner = nullptr;  // while connecting
  lv_obj_t* keyboard = nullptr;
};

// Parts of a screen made by wifi_create(), or nullptr for any other object.
const WifiParts* wifi_parts(lv_obj_t* screen);
std::vector<WifiSavedRow> wifi_saved_rows(lv_obj_t* screen);
std::vector<WifiNearbyRow> wifi_nearby_rows(lv_obj_t* screen);
// The open password page's parts, or nullptr when it is closed.
const WifiPasswordParts* wifi_password_parts(lv_obj_t* screen);
// Number of lit bars of a signal indicator.
int wifi_bars_level(lv_obj_t* bars);
