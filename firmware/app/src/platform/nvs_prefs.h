// Prefs over the ESP-IDF NVS API (namespace "app", partition "nvs" of
// partitions.csv, which a USB upload never wipes).
//
// - Keys: 1 to 15 characters of [a-z0-9._] (prefs_key_valid, ui/src/prefs.h);
//   set() and remove() reject (and log) any other key, get() answers false.
// - Values: at most 1000 bytes (prefs_value_valid), stored as NVS strings.
// - get() is false for a missing, wrongly typed, oversized or unreadable entry.
// - set() writes (and commits) only when the stored value differs, so a
//   repeated save of the same settings costs no flash wear.
// - If NVS cannot be opened at all, the object keeps the values in RAM for this
//   session (the UI still works, nothing persists) and says so on serial.
// Values are never logged (they may hold secrets later); key names are.
// NVS writes are power-fail safe by design: after a power cut a key holds
// either its old or its new value.
#pragma once

#include <nvs.h>

#include <string>

#include "prefs.h"

class NvsPrefs : public Prefs {
 public:
  // Initialises the NVS partition (erasing and re-initialising it when it is
  // full or written by a newer NVS version, logged) and opens namespace "app".
  NvsPrefs();
  ~NvsPrefs() override;
  NvsPrefs(const NvsPrefs&) = delete;
  NvsPrefs& operator=(const NvsPrefs&) = delete;

  bool get(const std::string& key, std::string& out) const override;
  void set(const std::string& key, const std::string& value) override;
  void remove(const std::string& key) override;

  // Erases every key of the namespace (settings back to defaults at the next
  // start). Returns false when it failed or nothing persists.
  bool clear();

  // True when values persist (NVS opened); false in the RAM-only fallback.
  bool persistent() const { return handle_ != 0; }

 private:
  nvs_handle_t handle_ = 0;
  MemoryPrefs fallback_;
};
