// Checks for the helper's address and access token, applied by the firmware
// before it stores them (serial :set-url / :set-token) and again when it loads
// them (NVS, build defaults); a value that fails is never used. Pure, shared
// with the simulator's tests.
//
// URL: written exactly "https://" + host [":" port] [path]; host not empty; no
// user info ('@' anywhere), no query or fragment ('?', '#'), no backslash; only
// printable ASCII without spaces (0x21..0x7e); at most kHelperUrlMaxLen bytes.
// Token: kTokenMinLen..kTokenMaxLen printable ASCII characters without spaces
// (0x21..0x7e), so it can be sent as the x-token header as it is.
#pragma once

#include <stddef.h>

#include <string>

constexpr size_t kHelperUrlMaxLen = 200;
constexpr size_t kTokenMinLen = 16;
constexpr size_t kTokenMaxLen = 128;

bool valid_helper_url(const std::string& url);
bool valid_token(const std::string& token);

// What may be printed about a token: its first 4 characters followed by
// "..." and its length; "(none)" when empty, "(invalid)" when it fails
// valid_token (nothing of an invalid value is shown).
std::string masked_token(const std::string& token);
