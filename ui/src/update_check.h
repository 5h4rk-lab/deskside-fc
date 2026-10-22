// Software updates, the pure part: version numbers, the update manifest the
// helper serves (GET /api/firmware/version), whether an update is offered,
// whether the battery allows one, the first bytes of a downloaded image and
// the texts of the Settings row. Shared by the firmware and the simulator: no
// clock reads, no stdio, no threads.
//
// Manifest (JSON object, at most kManifestMaxBytes):
//   {"version": "0.1.1", "url": "https://<helper host>/firmware/0.1.1.bin",
//    "sha256": "<64 hex characters>", "size": 1634512, "notes": "..."}
// Strict: version is MAJOR.MINOR.PATCH (digits, no leading zeros, each at
// most 9999); url is https (http only where the rules allow it: a diagnostics
// build pointed at a local test server) on exactly the rules' host (host and
// port, case-insensitive), with a path, and without user info, query,
// fragment, backslash, spaces or control characters (at most 300 bytes);
// sha256 is 64 hex characters (kept in lower case); size is a JSON integer
// from kUpdateMinBytes to the rules' maxBytes (the size of an app slot);
// notes is optional (a string or null). Other keys are ignored.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "power_service.h"

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool operator==(const Version& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
  bool operator!=(const Version& o) const { return !(*this == o); }
};

// "1.2.3" exactly (see above). False (and `out` untouched) otherwise.
bool parse_version(const std::string& text, Version& out);
// "1.2.3".
std::string version_text(const Version& v);
// True when `a` is a later version than `b`.
bool is_newer(Version a, Version b);

struct UpdateInfo {
  Version version;
  std::string url;
  std::string sha256;  // 64 lower-case hex characters
  int sizeBytes = 0;
  std::string notes;
};

constexpr int kUpdateMinBytes = 100 * 1024;
constexpr int kUpdateSlotBytes = 0x500000;  // one app slot (firmware/app/partitions.csv)
constexpr size_t kManifestMaxBytes = 4096;
constexpr size_t kUpdateUrlMaxLen = 300;
// The helper the release firmware talks to unless it is provisioned with
// another address (the device builds its rules from the address in use).
constexpr const char* kDefaultHelperHost = "soccer-display-helper.vercel.app";

// Where an update may come from.
struct ManifestRules {
  std::string host = kDefaultHelperHost;  // "host" or "host:port", compared case-insensitively
  bool allowHttp = false;                 // only a diagnostics build sets it (local test server)
  int maxBytes = kUpdateSlotBytes;
};

// The host ("host" or "host:port", lower case) of an http:// or https:// URL,
// "" when the URL has neither scheme or no host.
std::string url_host(const std::string& url);

// The rules for a manifest fetched from `manifestUrl`: the image must come from
// the same host. allowHttp is set only when `allowHttp` is true and the
// manifest URL itself is http:// (a diagnostics build's local test server).
ManifestRules rules_for_manifest_url(const std::string& manifestUrl, bool allowHttp);

// The manifest URL for a helper address: "<helper>/api/firmware/version" (one
// trailing slash of the helper address dropped).
std::string manifest_url_for_helper(const std::string& helperUrl);

// Parses and checks a manifest (see the top of this file). `out` is written
// only on success.
bool parse_update_manifest(const std::string& json, UpdateInfo& out);  // the default rules
bool parse_update_manifest(const std::string& json, const ManifestRules& rules, UpdateInfo& out);

enum class UpdateDecision { UpToDate, Available, Invalid };

// What a manifest means for a device running `current`: Invalid when it does
// not parse or fails a check; UpToDate when its version is not newer (the
// same, or older: never a downgrade through this path); else Available (and
// `info`, when given, gets the parsed manifest).
UpdateDecision decide(const Version& current, const std::string& manifestBody);
UpdateDecision decide(const Version& current, const std::string& manifestBody, const ManifestRules& rules,
                      UpdateInfo* info);

// The battery rule: an update may start when the battery has at least
// kUpdateMinBatteryPercent, or it is charging, or there is no battery (none
// connected, or the Battery setting is None: then the board runs from USB).
// The board cannot sense USB power, so "on USB" is read as "charging or no
// battery". `power` is what the UI shows (apply_battery_choice already applied).
constexpr int kUpdateMinBatteryPercent = 30;
bool update_power_ok(const PowerStatus& power);

// The first bytes of a downloaded image: the ESP image header (magic 0xE9,
// chip ESP32-S3), the first segment header, then the app descriptor (magic
// 0xABCD5432, version, project name). kImageHeadBytes are enough to decide.
constexpr size_t kImageHeadBytes = 112;
constexpr uint16_t kImageChipEsp32S3 = 9;
enum class ImageHead {
  NeedMore,      // fewer than kImageHeadBytes given
  Ok,            // ours, for this chip, and exactly `expected`
  NotAnApp,      // no image magic or no app descriptor
  WrongChip,     // built for another chip
  WrongProject,  // another project's firmware
  WrongVersion,  // not the version the manifest announced
};
ImageHead check_image_head(const uint8_t* data, size_t size, const char* projectName, const Version& expected);

// ---- the Settings row ---------------------------------------------------------

enum class UpdatePhase {
  Idle,         // not checked yet
  Checking,     // asking the helper
  UpToDate,     // the newest version runs
  Available,    // a newer version can be installed (a tap installs it)
  Preparing,    // pausing the data and pictures before the download
  Downloading,  // percent
  Installing,   // checking the download and switching to it
  Restarting,   // about to restart into the new version
  Failed,       // error says why; nothing was changed
};

enum class UpdateError {
  None,
  Offline,         // no network (or no valid clock) for the check or the download
  CheckFailed,     // the helper did not answer the check usefully
  BadManifest,     // the answer failed the manifest checks
  LowBattery,      // update_power_ok() said no
  Unconfirmed,     // the running version is still on trial (it was just installed)
  LowMemory,       // not enough internal RAM for the download
  DownloadFailed,  // the download stopped or timed out
  Damaged,         // the download's SHA-256 or size did not match
  WrongImage,      // check_image_head() refused it
  SaveFailed,      // writing to flash or switching the boot slot failed
};

struct UpdateStatus {
  UpdatePhase phase = UpdatePhase::Idle;
  Version current;
  Version available;  // Available and later phases
  int percent = 0;    // Downloading
  UpdateError error = UpdateError::None;  // Failed
  // A note from the last start, shown while nothing else is going on:
  // "Updated to 0.1.1" or the rollback text (see update_flow.h).
  std::string notice;
  bool noticeIsProblem = false;
};

// Plain-English text for an error (Settings row).
const char* update_error_text(UpdateError e);
// The status line of the Software update row, for example "Up to date (0.1.0)",
// "Version 0.1.1 available \xC2\xB7 Install", "Downloading 42%".
std::string update_status_text(const UpdateStatus& s);
// The row shows the line in amber (an error, or a rollback notice).
bool update_status_problem(const UpdateStatus& s);
// The row shows its progress bar (Preparing to Restarting).
bool update_status_busy(const UpdateStatus& s);
