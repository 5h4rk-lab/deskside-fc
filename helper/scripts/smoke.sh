#!/usr/bin/env bash
# Smoke test for a deployed helper.
# Usage: HELPER_TOKEN=... scripts/smoke.sh <base-url>     (preferred: argv shows no token)
#        scripts/smoke.sh <base-url> <token>              (still accepted, but the token is then visible in the process list)
# Prints OK/FAIL per check and exits non-zero if any check fails. The token is never printed.
set -u

base="${1:-}"
token="${2:-${HELPER_TOKEN:-}}"
if [ -z "$base" ] || [ -z "$token" ]; then
  echo "usage: HELPER_TOKEN=... $0 <base-url>   (or: $0 <base-url> <token>)" >&2
  exit 2
fi
base="${base%/}"

fail=0
# The token goes to curl through a private header file (-H @file, curl >= 7.55), not curl's argv.
hdr=$(mktemp)
chmod 600 "$hdr"
trap 'rm -f "$hdr"' EXIT
printf 'x-token: %s\n' "$token" > "$hdr"

check() { # name, expected, actual
  if [ "$2" = "$3" ]; then echo "OK   $1"; else echo "FAIL $1 (expected $2, got $3)"; fail=1; fi
}

# 1. No token -> 401
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$base/api/matches")
check "matches without token is 401" 401 "$code"

# 2. Wrong token -> 401
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 -H "x-token: not-the-token" "$base/api/matches")
check "matches with wrong token is 401" 401 "$code"

# 3. Right token -> 200 and a JSON body with "matches"
body=$(curl -s --max-time 30 -H "@$hdr" -w '\n%{http_code}' "$base/api/matches?leagues=eng.1")
code="${body##*$'\n'}"
json="${body%$'\n'*}"
check "matches with token is 200" 200 "$code"
auth_code="$code"
case "$json" in *'"matches"'*) echo "OK   matches body has a matches array" ;; *) echo "FAIL matches body has no matches array"; fail=1 ;; esac

# 3b. Same URL as check 3 without a token must still be 401. A CDN that ignores the
# x-token header (no "Vary: x-token") would answer 200 from the cache entry created above.
if [ "$auth_code" = 200 ]; then
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$base/api/matches?leagues=eng.1")
  check "unauthenticated request to the cached matches URL is 401" 401 "$code"
else
  echo "SKIP unauthenticated request to the cached matches URL (the authenticated request was not 200)"
fi

# 4. Image proxy returns a PNG
tmp=$(mktemp)
code=$(curl -s -o "$tmp" -w '%{http_code} %{content_type}' --max-time 30 -H "@$hdr" \
  "$base/api/img?u=https%3A%2F%2Fcrests.football-data.org%2F57.png&w=48&h=48")
check "image proxy is 200 image/png" "200 image/png" "$code"
rm -f "$tmp"

# 5. Image proxy refuses other hosts
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 -H "@$hdr" \
  "$base/api/img?u=https%3A%2F%2Fexample.com%2Fa.png&w=48&h=48")
check "image proxy refuses other hosts (400)" 400 "$code"

# 6. Teams list is non-empty
body=$(curl -s --max-time 40 -H "@$hdr" "$base/api/teams")
case "$body" in *'"teams"'*'"abbr"'*) echo "OK   teams list has teams" ;; *) echo "FAIL teams list looks empty"; fail=1 ;; esac

exit $fail
