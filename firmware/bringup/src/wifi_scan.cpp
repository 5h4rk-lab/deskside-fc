#include "wifi_scan.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "bringup_log.h"

namespace wifi_test {
namespace {

constexpr int kMaxListed = 40;
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr const char *kTestUrl = "https://crests.football-data.org/57.png";

const char *authName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK: return "WAPI-PSK";
    case WIFI_AUTH_OWE: return "OWE";
    default: return "other";
  }
}

int scan() {
  logf("[wifi] scanning (STA mode)...");
  const uint32_t t0 = millis();
  const int n = WiFi.scanNetworks();
  if (n < 0) {
    logf("[wifi] scan FAILED (code %d)", n);
    return 0;
  }
  logf("[wifi] scan done in %lu ms: %d network(s)", (unsigned long)(millis() - t0), n);

  // Sort indices by RSSI, strongest first (insertion sort, n is small).
  int order[kMaxListed];
  const int listed = n < kMaxListed ? n : kMaxListed;
  for (int i = 0; i < listed; ++i) {
    int j = i;
    while (j > 0 && WiFi.RSSI(order[j - 1]) < WiFi.RSSI(i)) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = i;
  }
  for (int k = 0; k < listed; ++k) {
    const int i = order[k];
    const String ssid = WiFi.SSID(i);
    logf("[wifi] %2d  %4ld dBm  ch %2ld  %-15s  %s", k + 1, (long)WiFi.RSSI(i), (long)WiFi.channel(i),
         authName(WiFi.encryptionType(i)), ssid.length() ? ssid.c_str() : "<hidden>");
  }
  if (n > listed) logf("[wifi] (%d more not listed)", n - listed);
  WiFi.scanDelete();
  return n;
}

#if defined(WIFI_SSID) && defined(WIFI_PASS)
bool connect() {
  logf("[wifi] connecting to \"%s\" ...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    logf("[wifi] connect FAILED after %lu ms (status %d)", (unsigned long)(millis() - t0), (int)WiFi.status());
    return false;
  }
  logf("[wifi] connected in %lu ms: ip=%s rssi=%d dBm", (unsigned long)(millis() - t0),
       WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

void httpsGet() {
  WiFiClientSecure client;
  client.setInsecure();  // bring-up only: no certificate check. Do not copy into real code.
  HTTPClient http;
  logf("[wifi] HTTPS GET %s", kTestUrl);
  if (!http.begin(client, kTestUrl)) {
    logf("[wifi] HTTPS begin FAILED");
    return;
  }
  const uint32_t t0 = millis();
  const int status = http.GET();
  if (status <= 0) {
    logf("[wifi] HTTPS GET FAILED: %s", http.errorToString(status).c_str());
    http.end();
    return;
  }
  // Count the body bytes without keeping them.
  size_t total = 0;
  const int expected = http.getSize();  // -1 when chunked / unknown
  WiFiClient *stream = http.getStreamPtr();
  uint8_t chunk[512];
  uint32_t lastData = millis();
  while (http.connected() && (expected < 0 || total < size_t(expected)) && millis() - lastData < 5000) {
    const size_t avail = stream->available();
    if (avail) {
      const int got = stream->readBytes(chunk, avail < sizeof(chunk) ? avail : sizeof(chunk));
      if (got > 0) total += size_t(got);
      lastData = millis();
    } else {
      delay(5);
    }
  }
  logf("[wifi] HTTPS status=%d bytes=%u (content-length %d) in %lu ms", status, (unsigned)total, expected,
       (unsigned long)(millis() - t0));
  http.end();
}
#endif

}  // namespace

int run() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  logf("[wifi] mode STA, mac=%s", WiFi.macAddress().c_str());
  const int n = scan();
#if defined(WIFI_SSID) && defined(WIFI_PASS)
  if (strlen(WIFI_SSID) > 0) {
    if (connect()) httpsGet();
  } else {
    logf("[wifi] WIFI_SSID is empty: scan only");
  }
#else
  logf("[wifi] no credentials (no secrets.ini): scan only");
#endif
  return n;
}

}  // namespace wifi_test
