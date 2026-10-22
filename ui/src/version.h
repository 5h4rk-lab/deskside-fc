// The one place the firmware and simulator version is defined.
//
// The number lives here and nowhere else: the Settings About line and the
// Software update row, the serial banner, the firmware's app descriptor
// (firmware/app/src/platform/app_desc.cpp; the update check reads it from an
// image before installing it) and tools/publish-firmware.sh (which reads
// APP_VERSION_TEXT from this file) all use it. A test build may override it
// with a build flag only (PLATFORMIO_BUILD_FLAGS='-DAPP_VERSION_OVERRIDE=\"0.1.1\"'),
// never in a committed file; tools/publish-firmware.sh refuses the override.
#pragma once

#define APP_VERSION_TEXT "0.1.0"

#ifdef APP_VERSION_OVERRIDE
#define APP_VERSION APP_VERSION_OVERRIDE
#else
#define APP_VERSION APP_VERSION_TEXT
#endif

constexpr const char* kAppName = "Soccer display";
constexpr const char* kAppVersion = APP_VERSION;
// The project name in the app descriptor of every image of this firmware. An
// update whose image carries another name is refused (not ours).
constexpr const char* kFirmwareProjectName = "soccer-display";
