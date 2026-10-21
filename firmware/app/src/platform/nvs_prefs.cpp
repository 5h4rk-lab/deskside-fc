#include "nvs_prefs.h"

#include <Arduino.h>
#include <nvs_flash.h>
#include <string.h>

namespace {

constexpr const char* kNamespace = "app";

// One read buffer (value plus its terminating NUL) on the stack: no heap.
constexpr size_t kBufBytes = kPrefsMaxValueLen + 1;

}  // namespace

NvsPrefs::NvsPrefs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.printf("[nvs] %s: erasing the partition and starting empty\n", esp_err_to_name(err));
    const esp_err_t erased = nvs_flash_erase();
    if (erased != ESP_OK) Serial.printf("[nvs] ERROR: erase failed: %s\n", esp_err_to_name(erased));
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    Serial.printf("[nvs] ERROR: init failed: %s; settings will not be saved\n", esp_err_to_name(err));
    return;
  }
  nvs_handle_t h = 0;
  err = nvs_open(kNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    Serial.printf("[nvs] ERROR: cannot open namespace '%s': %s; settings will not be saved\n", kNamespace,
                  esp_err_to_name(err));
    return;
  }
  handle_ = h;
  nvs_stats_t stats;
  if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
    Serial.printf("[nvs] ok, namespace '%s': %u entries used, %u free of %u\n", kNamespace,
                  (unsigned)stats.used_entries, (unsigned)stats.free_entries, (unsigned)stats.total_entries);
  } else {
    Serial.printf("[nvs] ok, namespace '%s'\n", kNamespace);
  }
}

NvsPrefs::~NvsPrefs() {
  if (handle_ != 0) nvs_close(handle_);
}

bool NvsPrefs::get(const std::string& key, std::string& out) const {
  if (!prefs_key_valid(key)) return false;
  if (handle_ == 0) return fallback_.get(key, out);

  size_t len = 0;
  esp_err_t err = nvs_get_str(handle_, key.c_str(), nullptr, &len);
  if (err == ESP_ERR_NVS_NOT_FOUND) return false;
  if (err != ESP_OK) {
    Serial.printf("[nvs] get '%s': %s (treated as missing)\n", key.c_str(), esp_err_to_name(err));
    return false;
  }
  // `len` counts the terminating NUL: an empty string is 1. Anything else is
  // corrupted or was not written by us.
  if (len < 1 || len > kBufBytes) {
    Serial.printf("[nvs] get '%s': bad length %u (treated as missing)\n", key.c_str(), (unsigned)len);
    return false;
  }
  char buf[kBufBytes];
  err = nvs_get_str(handle_, key.c_str(), buf, &len);
  if (err != ESP_OK || len < 1 || buf[len - 1] != '\0') {
    Serial.printf("[nvs] get '%s': %s (treated as missing)\n", key.c_str(),
                  err != ESP_OK ? esp_err_to_name(err) : "not terminated");
    return false;
  }
  out.assign(buf, len - 1);
  return true;
}

void NvsPrefs::set(const std::string& key, const std::string& value) {
  if (!prefs_key_valid(key)) {
    Serial.printf("[nvs] set rejected: invalid key (%u characters, 1 to %u of [a-z0-9._])\n",
                  (unsigned)key.size(), (unsigned)kPrefsMaxKeyLen);
    return;
  }
  if (!prefs_value_valid(value)) {
    Serial.printf("[nvs] set '%s' rejected: value of %u bytes (at most %u)\n", key.c_str(),
                  (unsigned)value.size(), (unsigned)kPrefsMaxValueLen);
    return;
  }
  // A NUL inside the value would silently cut it short when read back.
  if (value.find('\0') != std::string::npos) {
    Serial.printf("[nvs] set '%s' rejected: value contains a NUL byte\n", key.c_str());
    return;
  }
  if (handle_ == 0) {
    fallback_.set(key, value);
    return;
  }

  std::string current;
  if (get(key, current) && current == value) return;  // unchanged: no flash write

  esp_err_t err = nvs_set_str(handle_, key.c_str(), value.c_str());
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) {
    Serial.printf("[nvs] set '%s' FAILED: %s\n", key.c_str(), esp_err_to_name(err));
  } else if (key.compare(0, 9, "selftest.") != 0) {
    Serial.printf("[nvs] saved '%s'\n", key.c_str());  // the key only, never the value
  }
}

void NvsPrefs::remove(const std::string& key) {
  if (!prefs_key_valid(key)) return;
  if (handle_ == 0) {
    fallback_.remove(key);
    return;
  }
  esp_err_t err = nvs_erase_key(handle_, key.c_str());
  if (err == ESP_ERR_NVS_NOT_FOUND) return;
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) Serial.printf("[nvs] remove '%s' FAILED: %s\n", key.c_str(), esp_err_to_name(err));
}

bool NvsPrefs::clear() {
  if (handle_ == 0) return false;
  esp_err_t err = nvs_erase_all(handle_);
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) {
    Serial.printf("[nvs] clear FAILED: %s\n", esp_err_to_name(err));
    return false;
  }
  Serial.println("[nvs] all settings erased");
  return true;
}
