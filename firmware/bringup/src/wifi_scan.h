// Wi-Fi bring-up: scan, and optionally connect + one HTTPS GET.
#pragma once

namespace wifi_test {

// Scans and logs every network (sorted by RSSI). Returns the number of
// networks found (0 on failure). If WIFI_SSID / WIFI_PASS were set at build
// time (secrets.ini), also connects and fetches one PNG over HTTPS.
int run();

}  // namespace wifi_test
