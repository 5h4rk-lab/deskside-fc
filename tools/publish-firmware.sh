#!/usr/bin/env bash
# Publishes the release firmware for over-the-air updates, locally: builds the
# release `app` env, checks the image, copies it to helper/public/firmware/<version>.bin
# and writes helper/firmware-manifest.json (version, file, sha256, size, notes).
# It does NOT deploy: commit the two files and deploy the helper yourself
# (helper/README.md, "Firmware updates"). Devices see the new version at their
# next check (at most 6 h, or when Settings is opened) and install it only
# when the user taps "Install".
#
# Usage: tools/publish-firmware.sh [--notes "One line for the release"] [--skip-build]
#
# Refuses to run when:
#  - firmware/app/secrets.ini defines HELPER_URL or HELPER_TOKEN (that image
#    would carry a token: such images are never published);
#  - PLATFORMIO_BUILD_FLAGS is set (it could override the version, or anything);
#  - the firmware sources (firmware/app, ui) have uncommitted changes (the
#    published image must be a commit);
#  - the version in ui/src/version.h (APP_VERSION_TEXT) is not newer than the
#    one already published.
# The image is also checked: size 100 KB to 5 MB (one app slot), app
# descriptor project "soccer-display" and the same version. When HELPER_TOKEN
# is in the environment, the image is checked not to contain it (the token is
# read from the environment by a child process and never printed).
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
app="$repo/firmware/app"
helper="$repo/helper"
notes=""
build=1
while [ $# -gt 0 ]; do
  case "$1" in
    --notes) notes="${2:-}"; shift 2 ;;
    --skip-build) build=0; shift ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
  esac
done

die() { echo "publish-firmware: $*" >&2; exit 1; }

if [ -f "$app/secrets.ini" ] && grep -Eq 'HELPER_(TOKEN|URL)' "$app/secrets.ini"; then
  die "firmware/app/secrets.ini sets HELPER_URL/HELPER_TOKEN: that image would carry them. Move secrets.ini away (the device gets the helper over the serial console) and run again."
fi
if [ -n "${PLATFORMIO_BUILD_FLAGS:-}" ]; then
  die "PLATFORMIO_BUILD_FLAGS is set: unset it (a published image is built from the committed sources only)."
fi
if [ -n "$(git -C "$repo" status --porcelain -- firmware/app ui)" ]; then
  die "firmware/app or ui has uncommitted changes: commit them first (the published image must be a commit)."
fi

version="$(sed -n 's/^#define APP_VERSION_TEXT "\([0-9]*\.[0-9]*\.[0-9]*\)"$/\1/p' "$repo/ui/src/version.h")"
[ -n "$version" ] || die "cannot read APP_VERSION_TEXT from ui/src/version.h"
notes_len=${#notes}
[ "$notes_len" -le 200 ] || die "--notes is longer than 200 characters"

python3 - "$helper/firmware-manifest.json" "$version" <<'EOF' || die "version $version is not newer than the published one"
import json, sys
cur = json.load(open(sys.argv[1])).get("version")
new = tuple(int(x) for x in sys.argv[2].split("."))
if cur is not None and tuple(int(x) for x in cur.split(".")) >= new:
    print(f"published: {cur}, ui/src/version.h: {sys.argv[2]}", file=sys.stderr)
    sys.exit(1)
EOF

if [ "$build" = 1 ]; then
  (cd "$app" && pio run -e app)
fi
bin="$app/.pio/build/app/firmware.bin"
[ -f "$bin" ] || die "no $bin (build failed?)"

# Size, app descriptor (project and version) and, when HELPER_TOKEN is set, no token inside.
python3 - "$bin" "$version" <<'EOF' || die "the image failed its checks (see above); nothing was published"
import os, struct, sys
data = open(sys.argv[1], "rb").read()
version = sys.argv[2]
ok = True
if not (100 * 1024 <= len(data) <= 0x500000):
    print(f"size {len(data)} B is outside 100 KB .. 5 MB", file=sys.stderr); ok = False
if data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
    print("not an ESP app image with an app descriptor", file=sys.stderr); ok = False
else:
    field = lambda off: data[off:off + 32].split(b"\0", 1)[0].decode("ascii", "replace")
    if field(80) != "soccer-display":
        print(f"app descriptor project is '{field(80)}', not 'soccer-display'", file=sys.stderr); ok = False
    if field(48) != version:
        print(f"app descriptor version is '{field(48)}', not '{version}'", file=sys.stderr); ok = False
token = os.environ.get("HELPER_TOKEN", "").encode()
if token and token in data:
    print("the image CONTAINS the helper token: never publish it", file=sys.stderr); ok = False
sys.exit(0 if ok else 1)
EOF

mkdir -p "$helper/public/firmware"
cp "$bin" "$helper/public/firmware/$version.bin"
python3 - "$helper/public/firmware/$version.bin" "$version" "$helper/firmware-manifest.json" "$notes" <<'EOF'
import hashlib, json, sys
path, version, manifest, notes = sys.argv[1:5]
data = open(path, "rb").read()
m = {"version": version, "file": f"firmware/{version}.bin", "sha256": hashlib.sha256(data).hexdigest(),
     "size": len(data), "notes": notes}
with open(manifest, "w") as f:
    json.dump(m, f, indent=2)
    f.write("\n")
print(f"published {version} locally: helper/public/firmware/{version}.bin, {len(data)} B, sha256 {m['sha256']}")
EOF
echo "next: cd helper && npx vitest run   (checks the manifest against the file)"
echo "      git add helper/firmware-manifest.json helper/public/firmware/$version.bin && git commit"
echo "      then deploy the helper (helper/README.md). This script deployed nothing."
