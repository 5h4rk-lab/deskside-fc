// The saved Wi-Fi networks, as the firmware keeps them: up to kMaxSaved
// entries {ssid, password} in most-recently-used order, and a compact blob
// form for one NVS value. Pure logic (no ESP types, no stdio, no exceptions),
// shared with the simulator tests.
//
// Passwords: the only way to read one is password_of(), which the connect
// code uses. ssids(), ordered_by_signal() and everything else the service
// hands to the UI carry SSIDs only. serialize() returns the storage form
// (passwords included); it goes to the NVS blob and nowhere else.
//
// Blob format (versioned by the record marker): a sequence of records
//   0xFE | ssid_len (1) | password_len (1) | ssid | password | crc16 (2, big endian)
// in most-recently-used order. The CRC covers everything before it, so a
// damaged record is detected, and the marker lets parse() find the next
// record after one. 0xFE never occurs in UTF-8 text.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "wifi_service.h"

// An SSID is 1 to 32 bytes without control characters (NUL, 0x01..0x1F, 0x7F).
bool wifi_ssid_valid(const std::string& ssid);
// A passphrase is 0 (open network) to 63 bytes without control characters.
bool wifi_password_valid(const std::string& password);

class WifiStore {
 public:
  static constexpr size_t kMaxSaved = 16;
  static_assert(kMaxSaved >= kMinSavedNetworks, "the list must be able to hold the contract's minimum");
  static constexpr size_t kMaxSsidBytes = 32;
  static constexpr size_t kMaxPasswordBytes = 63;
  // marker + two lengths + checksum around the longest SSID and password.
  static constexpr size_t kMaxRecordBytes = 1 + 2 + kMaxSsidBytes + kMaxPasswordBytes + 2;
  static constexpr size_t kMaxSerializedBytes = kMaxSaved * kMaxRecordBytes;

  struct ParseResult {
    size_t loaded = 0;        // records accepted
    size_t skippedBytes = 0;  // bytes that belonged to no valid record
  };

  // Saves `ssid` with `password` (empty for an open network) as the most
  // recently used entry. The same SSID replaces its entry; a new SSID in a
  // full list drops the least recently used one (never while the list holds
  // fewer than kMaxSaved entries, so never below kMinSavedNetworks). False,
  // and nothing changes, for an invalid SSID or password.
  bool add_or_replace(const std::string& ssid, const std::string& password);
  // Makes `ssid` the most recently used entry. False when it is not saved.
  bool touch(const std::string& ssid);
  // False when it was not saved.
  bool remove(const std::string& ssid);
  void clear() { entries_.clear(); }

  size_t size() const { return entries_.size(); }
  bool contains(const std::string& ssid) const;
  // Saved SSIDs, most recently used first.
  std::vector<std::string> ssids() const;
  // The stored password of `ssid` (empty for an open network). False, and
  // `out` untouched, when it is not saved. For the connect code only.
  bool password_of(const std::string& ssid, std::string& out) const;

  // The saved networks that appear in `scan`, one entry per SSID (the
  // strongest sighting, with that sighting's `open`), strongest first; equal
  // signals keep the most recently used first. Hidden networks never match.
  std::vector<WifiNetwork> ordered_by_signal(const std::vector<WifiNetwork>& scan) const;

  // Storage form (see the top). Empty for an empty list.
  std::string serialize() const;
  // Replaces the content with the records found in `data`; records that are
  // damaged, hold an invalid SSID or password, repeat an SSID, or exceed
  // kMaxSaved are skipped. Never throws or fails.
  ParseResult parse(const std::string& data);

  // One record in storage form, without any validation (tests build damaged
  // lists with it). Lengths above 255 are cut.
  static std::string encode_record(const std::string& ssid, const std::string& password);

 private:
  struct Entry {
    std::string ssid;
    std::string password;
  };
  std::vector<Entry> entries_;  // most recently used first
};
