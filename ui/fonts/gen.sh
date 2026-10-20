#!/usr/bin/env bash
# Regenerates the LVGL font C files in this directory.
#
# Needs: bash, curl, python3 (with venv + pip), node/npx, network access.
# The generated .c files are committed, so a normal build never runs this.
#
# Source fonts: Google Fonts variable TTFs (SIL OFL 1.1), pinned to a
# google/fonts commit. They are instanced to static weights with fonttools,
# because lv_font_conv cannot read variable fonts.
#
#   bigshoulders_<size>.c  Big Shoulders Display, wght 800
#       sizes 16 22 28 40: ASCII 0x20-0x7E, Latin-1 Supplement 0xA0-0xFF,
#                          Latin Extended-A 0x100-0x17F (player/club names)
#       sizes 56 96:       digits and  space : - ' % /  only (scores, clocks)
#       size 72:           space, A-Z and ! only (the goal celebration's
#                          "GOAL!" and "WIN!")
#   hanken_<size>.c        Hanken Grotesk, wght 400, sizes 10 12 14,
#                          same ranges as the full Big Shoulders sizes
#
# All fonts: 4 bpp antialiasing, no compression, kerning kept.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="${FONT_WORK:-$ROOT/build/fonts}"
GF_COMMIT=47d54ca1ebb14fd864695f1d39dc734f39017c33
GF_RAW="https://raw.githubusercontent.com/google/fonts/$GF_COMMIT/ofl"
FONTTOOLS_VERSION=4.55.3
LV_FONT_CONV_VERSION=1.5.3

FULL_RANGE="0x20-0x7E,0xA0-0xFF,0x100-0x17F"
DIGIT_RANGE="0x20,0x25,0x27,0x2D,0x2F,0x30-0x3A"
STAMP_RANGE="0x20,0x21,0x41-0x5A"

mkdir -p "$WORK/src"
cd "$WORK"

fetch() { # url dest
  [ -s "$2" ] || curl -fsSL -o "$2" "$1"
}

fetch "$GF_RAW/bigshouldersdisplay/BigShouldersDisplay%5Bwght%5D.ttf" src/BigShouldersDisplay-VF.ttf
fetch "$GF_RAW/hankengrotesk/HankenGrotesk%5Bwght%5D.ttf" src/HankenGrotesk-VF.ttf
fetch "$GF_RAW/bigshouldersdisplay/OFL.txt" src/OFL-bigshoulders.txt
fetch "$GF_RAW/hankengrotesk/OFL.txt" src/OFL-hanken.txt

if [ ! -x venv/bin/python ]; then
  python3 -m venv venv
  venv/bin/pip install --quiet "fonttools==$FONTTOOLS_VERSION"
fi

instance() { # input output wght
  [ -s "$2" ] || venv/bin/python -m fontTools varLib.instancer "$1" "wght=$3" -o "$2" >/dev/null
}
instance src/BigShouldersDisplay-VF.ttf src/BigShouldersDisplay-800.ttf 800
instance src/HankenGrotesk-VF.ttf src/HankenGrotesk-400.ttf 400

conv() { # fontfile name size range
  npx --yes "lv_font_conv@$LV_FONT_CONV_VERSION" \
    --font "$1" --size "$3" --range "$4" \
    --bpp 4 --no-compress --format lvgl --lv-include lvgl.h \
    --lv-font-name "$2_$3" -o "$HERE/$2_$3.c"
  # keep absolute paths out of the generated header comment
  sed -i "s# -o $HERE/# -o #" "$HERE/$2_$3.c"
  echo "generated $2_$3.c"
}

for s in 16 22 28 40; do conv src/BigShouldersDisplay-800.ttf bigshoulders "$s" "$FULL_RANGE"; done
for s in 56 96;        do conv src/BigShouldersDisplay-800.ttf bigshoulders "$s" "$DIGIT_RANGE"; done
conv src/BigShouldersDisplay-800.ttf bigshoulders 72 "$STAMP_RANGE"
for s in 10 12 14;     do conv src/HankenGrotesk-400.ttf hanken "$s" "$FULL_RANGE"; done

{
  echo "Big Shoulders Display and Hanken Grotesk are licensed under the SIL Open Font"
  echo "License 1.1. Source: https://github.com/google/fonts (commit $GF_COMMIT)."
  echo "The generated .c files in this directory are derived bitmap renderings of those fonts."
  echo
  echo "===== Big Shoulders Display ====="
  cat src/OFL-bigshoulders.txt
  echo
  echo "===== Hanken Grotesk ====="
  cat src/OFL-hanken.txt
} > "$HERE/LICENSE-OFL.txt"
