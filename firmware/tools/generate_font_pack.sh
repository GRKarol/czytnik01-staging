#!/usr/bin/env bash
# Regenerates the fonts-pack-vX.zip release asset from scratch: downloads the
# 17 SD-card reader typefaces (Google Fonts, OFL) and runs
# generate_embedded_font.py --fnt-output for each, at both reader sizes.
#
# Font source files are NOT committed to the repo (same policy as before the
# SD migration: only the generated output is a build artifact, not the TTF
# itself). Re-run this script to reproduce the pack; it re-downloads from
# raw.githubusercontent.com/google/fonts each time.
#
# Usage: tools/generate_font_pack.sh <output_dir>
# Produces <output_dir>/<name>.fnt and <output_dir>/<name>_70.fnt for all 17
# fonts (34 files total, ~8-9 MB). Zip the directory yourself for the release
# asset (see docs/PLAN_FONTY_NA_SD.md, Etap 5).

set -euo pipefail

OUT_DIR="${1:?usage: generate_font_pack.sh <output_dir>}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(mktemp -d)"
mkdir -p "$OUT_DIR"

trap 'rm -rf "$SRC_DIR"' EXIT

# name -> raw.githubusercontent.com/google/fonts/main/<path> (URL-encoded)
declare -A FONT_URLS=(
  [literata]="ofl/literata/Literata%5Bopsz,wght%5D.ttf"
  [merriweather]="ofl/merriweather/Merriweather%5Bopsz,wdth,wght%5D.ttf"
  [lora]="ofl/lora/Lora%5Bwght%5D.ttf"
  [bitter]="ofl/bitter/Bitter%5Bwght%5D.ttf"
  [ebgaramond]="ofl/ebgaramond/EBGaramond%5Bwght%5D.ttf"
  [vollkorn]="ofl/vollkorn/Vollkorn%5Bwght%5D.ttf"
  [gelasio]="ofl/gelasio/Gelasio%5Bwght%5D.ttf"
  [ptserif]="ofl/ptserif/PT_Serif-Web-Regular.ttf"
  [ibmplexserif]="ofl/ibmplexserif/IBMPlexSerif-Regular.ttf"
  [cardo]="ofl/cardo/Cardo-Regular.ttf"
  [zillaslab]="ofl/zillaslab/ZillaSlab-Regular.ttf"
  [oldstandard]="ofl/oldstandardtt/OldStandard-Regular.ttf"
  [domine]="ofl/domine/Domine%5Bwght%5D.ttf"
  [alegreya]="ofl/alegreya/Alegreya%5Bwght%5D.ttf"
  [newsreader]="ofl/newsreader/Newsreader%5Bopsz,wght%5D.ttf"
  [notoserif]="ofl/notoserif/NotoSerif%5Bwdth,wght%5D.ttf"
  [spectral]="ofl/spectral/Spectral-Regular.ttf"
)

# Display labels only (embedded as a comment in --output headers, unused
# here since these are SD-only fonts with no --output).
declare -A FONT_LABELS=(
  [literata]="Literata" [merriweather]="Merriweather" [lora]="Lora" [bitter]="Bitter"
  [ebgaramond]="EB Garamond" [vollkorn]="Vollkorn" [gelasio]="Gelasio"
  [ptserif]="PT Serif" [ibmplexserif]="IBM Plex Serif" [cardo]="Cardo"
  [zillaslab]="Zilla Slab" [oldstandard]="Old Standard TT" [domine]="Domine"
  [alegreya]="Alegreya" [newsreader]="Newsreader" [notoserif]="Noto Serif" [spectral]="Spectral"
)

# Target raster heights, calibrated to match the visual size of the existing
# flash-resident fonts (Atkinson/Serif) — see docs/PLAN_FONTY_NA_SD.md.
TARGET_HEIGHT_BASE=52
TARGET_HEIGHT_70=39
# Letter size (mean of x-height and cap height, px) of Atkinson, the default
# reading face, at 72 pt (large) and 44 pt (medium). Calibrating on the full
# ink span instead left some families at half Atkinson's letter size.
TARGET_OPTICAL_BASE=41.5
TARGET_OPTICAL_70=25.5
# File name suffix: firmware asks for <name>-v2.fnt, so readers holding the
# older, smaller files download these (DisplayManager sdFontBaseName).
PACK_SUFFIX="-v2"

for name in "${!FONT_URLS[@]}"; do
  url="https://raw.githubusercontent.com/google/fonts/main/${FONT_URLS[$name]}"
  echo "downloading $name..."
  curl -fsSL --max-time 30 -o "$SRC_DIR/$name.ttf" "$url"
done

for name in "${!FONT_URLS[@]}"; do
  label="${FONT_LABELS[$name]}"
  python3 "$SCRIPT_DIR/generate_embedded_font.py" "$SRC_DIR/$name.ttf" \
    --symbol-prefix "Tmp" --fnt-output "$OUT_DIR/${name}${PACK_SUFFIX}.fnt" \
    --font-label "$label" --target-height "$TARGET_HEIGHT_BASE" \
    --target-optical "$TARGET_OPTICAL_BASE"
  python3 "$SCRIPT_DIR/generate_embedded_font.py" "$SRC_DIR/$name.ttf" \
    --symbol-prefix "Tmp70" --fnt-output "$OUT_DIR/${name}${PACK_SUFFIX}_70.fnt" \
    --font-label "$label" --target-height "$TARGET_HEIGHT_70" \
    --target-optical "$TARGET_OPTICAL_70"
done

echo "done: $(ls "$OUT_DIR"/*.fnt | wc -l) .fnt files in $OUT_DIR"
