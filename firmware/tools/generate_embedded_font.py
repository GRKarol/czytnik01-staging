#!/usr/bin/env python3
"""Generate an embedded reader-font header from a TTF using Pillow.

Successor to generate_embedded_serif_font.py: same output format/geometry
(112x128 canvas, glyph slots 1-255 with the Polish/Central-European custom
slot map, alpha-threshold crop to a shared line height), but rendered with
Pillow instead of shelling out to Ghostscript (not installed on this
machine) and emitting the shared EmbeddedFontGlyph struct from
EmbeddedFontCommon.h instead of a fresh per-font struct.

Point size is auto-calibrated per font (see --target-height) so different
families with different cap-height/UPM ratios end up visually similar in
size on the reader screen, matching how the three existing embedded fonts
were each tuned to a different source point size by hand.

--fnt-output additionally writes the same glyph data as a binary .fnt file
for SD-card loading (see docs/FONT_FNT_FORMAT.md) instead of PROGMEM.

--thumbnail-output writes a small flash-resident header covering only the
printable-ASCII range (32-126) at the same point size as the main output —
used for font-picker button-preview thumbnails (Etap 6 of
docs/PLAN_FONTY_NA_SD.md), so a font's own name can render in its own face
without touching the SD card or the (single-slot) SdFontLoader cache.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import unicodedata

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

CANVAS_WIDTH = 160
CANVAS_HEIGHT = 176
ORIGIN_X = 10
BASELINE_Y = 110
ALPHA_THRESHOLD = 16
FONT_TOP_PADDING = 4
FONT_BOTTOM_PADDING = 2
FIRST_CHAR = 1
LAST_CHAR = 255

# Keep in sync with src/text/LatinText.h (customSlotForCodepoint) and the
# identical table in generate_embedded_serif_font.py.
CUSTOM_GLYPH_CODEPOINTS = {
    0x01: 0x010E, 0x02: 0x010F, 0x03: 0x011A, 0x04: 0x011B, 0x05: 0x0147,
    0x06: 0x0148, 0x07: 0x0158, 0x08: 0x0159, 0x0E: 0x0164, 0x0F: 0x0165,
    0x10: 0x016E, 0x11: 0x016F, 0x12: 0x0150, 0x13: 0x0151, 0x14: 0x0170,
    0x15: 0x0171, 0x80: 0x0152, 0x81: 0x0153, 0x82: 0x0141, 0x83: 0x0142,
    0x84: 0x010C, 0x85: 0x010D, 0x86: 0x0160, 0x87: 0x0161, 0x88: 0x017D,
    0x89: 0x017E, 0x8A: 0x0102, 0x8B: 0x0103, 0x8C: 0x0218, 0x8D: 0x0219,
    0x8E: 0x021A, 0x8F: 0x021B, 0x90: 0x011E, 0x91: 0x011F, 0x92: 0x015E,
    0x93: 0x015F, 0x94: 0x0130, 0x95: 0x0131, 0x96: 0x0104, 0x97: 0x0105,
    0x98: 0x0118, 0x99: 0x0119, 0x9A: 0x0106, 0x9B: 0x0107, 0x9C: 0x0143,
    0x9D: 0x0144, 0x9E: 0x015A, 0x9F: 0x015B, 0xB2: 0x0179, 0xB3: 0x017A,
    0xB4: 0x017B, 0xB5: 0x017C, 0xA1: 0x0100, 0xA2: 0x0101, 0xA3: 0x0112,
    0xA4: 0x0113, 0xA5: 0x0122, 0xA6: 0x0123, 0xA7: 0x012A, 0xA8: 0x012B,
    0xA9: 0x0136, 0xAA: 0x0137, 0xAB: 0x013B, 0xAC: 0x013C, 0xAE: 0x0145,
    0xAF: 0x0146, 0xB0: 0x0116, 0xB1: 0x0117, 0xB6: 0x012E, 0xB7: 0x012F,
    0xB8: 0x0172, 0xB9: 0x0173, 0xBA: 0x016A, 0xBB: 0x016B, 0xBC: 0x0110,
    0xBD: 0x0111, 0xBE: 0x014A, 0xBF: 0x014B, 0xD7: 0x0166, 0xF7: 0x0167,
}


def display_codepoint_for_slot(slot: int) -> int:
    return CUSTOM_GLYPH_CODEPOINTS.get(slot, slot)


def base_ascii_fallback(codepoint: int) -> str:
    """Strip diacritics via Unicode decomposition for glyphs the font lacks."""
    decomposed = unicodedata.normalize("NFKD", chr(codepoint))
    base = decomposed[0] if decomposed else "?"
    if 32 <= ord(base) <= 126:
        return base
    return "?"


def load_font(ttf_path: pathlib.Path, point_size: int) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(ttf_path), point_size)
    # Variable fonts: weight 400, other axes at their defaults. The old
    # set_variation_by_name("Regular") never matched (Pillow lists names as
    # bytes) and left Bitter at its default Thin and Merriweather at Light.
    try:
        axes = font.get_variation_axes()
    except Exception:
        return font
    values = []
    for axis in axes:
        name = axis.get("name", b"")
        if isinstance(name, bytes):
            name = name.decode("ascii", "ignore")
        if name.lower() == "weight":
            values.append(min(max(400, axis["minimum"]), axis["maximum"]))
        else:
            values.append(axis["default"])
    font.set_variation_by_axes(values)
    return font


def resolve_char(slot: int, cmap: dict) -> str:
    codepoint = display_codepoint_for_slot(slot)
    if codepoint in cmap:
        return chr(codepoint)
    return base_ascii_fallback(codepoint)


def measure_ink_span(ttf_path: pathlib.Path, point_size: int, cmap: dict) -> int:
    font = load_font(ttf_path, point_size)
    top = CANVAS_HEIGHT
    bottom = -1
    for slot in range(FIRST_CHAR, LAST_CHAR + 1):
        ch = resolve_char(slot, cmap)
        img = Image.new("L", (CANVAS_WIDTH, CANVAS_HEIGHT), 0)
        draw = ImageDraw.Draw(img)
        draw.text((ORIGIN_X, BASELINE_Y), ch, font=font, fill=255, anchor="ls")
        data = img.tobytes()
        for y in range(CANVAS_HEIGHT):
            row = data[y * CANVAS_WIDTH:(y + 1) * CANVAS_WIDTH]
            if any(b > ALPHA_THRESHOLD for b in row):
                top = min(top, y)
                bottom = max(bottom, y)
    if bottom < top:
        raise RuntimeError("no ink found while measuring font")
    return bottom - top + 1


def measure_char_ink_height(ttf_path: pathlib.Path, point_size: int, ch: str) -> int:
    font = load_font(ttf_path, point_size)
    img = Image.new("L", (CANVAS_WIDTH, CANVAS_HEIGHT), 0)
    draw = ImageDraw.Draw(img)
    draw.text((ORIGIN_X, BASELINE_Y), ch, font=font, fill=255, anchor="ls")
    bbox = img.point(lambda v: 255 if v > ALPHA_THRESHOLD else 0).getbbox()
    if bbox is None:
        raise RuntimeError(f"no ink found for {ch!r}")
    return bbox[3] - bbox[1]


def optical_size(ttf_path: pathlib.Path, point_size: int) -> float:
    """Mean of x-height and cap height in px: what the eye reads as "size".

    The full ink span (calibrate_point_size) is dominated by accents on
    capitals and descenders, so families with tall diacritics or long tails
    came out with letters half as big as Atkinson's at the same setting."""
    return (measure_char_ink_height(ttf_path, point_size, "x") +
            measure_char_ink_height(ttf_path, point_size, "H")) / 2


def calibrate_point_size_optical(ttf_path: pathlib.Path, target: float) -> int:
    trial = 60
    guess = max(8, round(trial * target / optical_size(ttf_path, trial)))
    candidates = range(max(8, guess - 2), guess + 3)
    return min(candidates, key=lambda pt: abs(optical_size(ttf_path, pt) - target))


def calibrate_point_size(ttf_path: pathlib.Path, cmap: dict, target_height: int) -> int:
    trial = 60
    span = measure_ink_span(ttf_path, trial, cmap)
    scale = target_height / span
    calibrated = max(8, round(trial * scale))
    return calibrated


def generate(
    ttf_path: pathlib.Path,
    point_size: int,
    cmap: dict,
    first_char: int = FIRST_CHAR,
    last_char: int = LAST_CHAR,
) -> tuple[int, list[int], list[tuple[int, int, int, int]]]:
    font = load_font(ttf_path, point_size)

    rasters: dict[int, bytes] = {}
    global_top = CANVAS_HEIGHT
    global_bottom = -1

    for slot in range(first_char, last_char + 1):
        ch = resolve_char(slot, cmap)
        img = Image.new("L", (CANVAS_WIDTH, CANVAS_HEIGHT), 0)
        draw = ImageDraw.Draw(img)
        draw.text((ORIGIN_X, BASELINE_Y), ch, font=font, fill=255, anchor="ls")
        raster = img.tobytes()
        rasters[slot] = raster
        for y in range(CANVAS_HEIGHT):
            row = raster[y * CANVAS_WIDTH:(y + 1) * CANVAS_WIDTH]
            if any(b > ALPHA_THRESHOLD for b in row):
                global_top = min(global_top, y)
                global_bottom = max(global_bottom, y)

    if global_bottom < global_top:
        raise RuntimeError("Failed to detect any font pixels")

    crop_top = max(0, global_top - FONT_TOP_PADDING)
    crop_bottom = min(CANVAS_HEIGHT - 1, global_bottom + FONT_BOTTOM_PADDING)
    font_height = crop_bottom - crop_top + 1

    bitmap_bytes: list[int] = []
    glyph_entries: list[tuple[int, int, int, int]] = []

    for slot in range(first_char, last_char + 1):
        raster = rasters[slot]
        min_x = CANVAS_WIDTH
        max_x = -1
        for y in range(crop_top, crop_bottom + 1):
            row = raster[y * CANVAS_WIDTH:(y + 1) * CANVAS_WIDTH]
            for x, value in enumerate(row):
                if value > ALPHA_THRESHOLD:
                    min_x = min(min_x, x)
                    max_x = max(max_x, x)

        bitmap_offset = len(bitmap_bytes)
        ch = resolve_char(slot, cmap)
        raw_advance = font.getlength(ch)
        x_advance = max(1, round(raw_advance))

        if max_x >= min_x:
            glyph_width = max_x - min_x + 1
            for y in range(crop_top, crop_bottom + 1):
                row = raster[y * CANVAS_WIDTH:(y + 1) * CANVAS_WIDTH]
                for x in range(min_x, max_x + 1):
                    value = row[x]
                    bitmap_bytes.append(value if value > ALPHA_THRESHOLD else 0)
            x_offset = min_x - ORIGIN_X
        else:
            x_offset = 0
            glyph_width = 0

        glyph_entries.append((bitmap_offset, x_offset, glyph_width, x_advance))

    return font_height, bitmap_bytes, glyph_entries


def write_header(
    output: pathlib.Path,
    symbol_prefix: str,
    font_height: int,
    bitmap_bytes: list[int],
    glyph_entries: list[tuple[int, int, int, int]],
    font_label: str,
    point_size: int,
    first_char: int = FIRST_CHAR,
    last_char: int = LAST_CHAR,
) -> None:
    lines: list[str] = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        '#include "display/EmbeddedFontCommon.h"',
        "",
        f"// Generated by tools/generate_embedded_font.py from a real font and",
        "// embedded as glyph data.",
        f"// Source font: {font_label} at {point_size} pt",
        "",
        f"constexpr uint8_t k{symbol_prefix}FirstChar = {first_char};",
        f"constexpr uint8_t k{symbol_prefix}LastChar = {last_char};",
        f"constexpr uint8_t k{symbol_prefix}Height = {font_height};",
        "",
        f"static const uint8_t k{symbol_prefix}Bitmaps[] PROGMEM = " + "{",
    ]

    for offset in range(0, len(bitmap_bytes), 16):
        chunk = bitmap_bytes[offset:offset + 16]
        lines.append("    " + ", ".join(f"{value:3d}" for value in chunk) + ",")

    lines += [
        "};",
        "",
        f"static const EmbeddedFontGlyph k{symbol_prefix}Glyphs[] PROGMEM = " + "{",
    ]
    for bitmap_offset, x_offset, glyph_width, x_advance in glyph_entries:
        lines.append(f"    {{{bitmap_offset}, {x_offset}, {glyph_width}, {x_advance}}},")
    lines += ["};", ""]

    output.write_text("\n".join(lines) + "\n", encoding="ascii")


# --- .fnt binary format (see docs/FONT_FNT_FORMAT.md) --------------------
#
# Header (16 bytes, all multi-byte fields little-endian):
#   magic        4s   b"FNT1"
#   version      H    format version, currently 1
#   firstChar    B
#   lastChar     B
#   height       B
#   reserved     B    must be 0
#   glyphCount   H    == lastChar - firstChar + 1
#   bitmapLength I    byte length of the bitmap blob
#
# Glyph table: glyphCount records of 8 bytes each, in slot order
# (firstChar..lastChar), matching EmbeddedFontGlyph's in-memory layout so
# the runtime loader can point straight into the loaded buffer instead of
# parsing field-by-field:
#   bitmapOffset I
#   xOffset      b
#   width        B
#   xAdvance     B
#   pad          B    must be 0 (mirrors EmbeddedFontGlyph's trailing pad)
#
# Bitmap blob: bitmapLength raw alpha bytes, exactly the same content
# written into kXxxBitmaps[] by write_header() above.

FNT_MAGIC = b"FNT1"
FNT_VERSION = 1
_FNT_HEADER = struct.Struct("<4sHBBBBHI")
_FNT_GLYPH = struct.Struct("<IbBBB")


def pack_fnt(
    font_height: int,
    bitmap_bytes: list[int],
    glyph_entries: list[tuple[int, int, int, int]],
) -> bytes:
    glyph_count = len(glyph_entries)
    header = _FNT_HEADER.pack(
        FNT_MAGIC,
        FNT_VERSION,
        FIRST_CHAR,
        LAST_CHAR,
        font_height,
        0,
        glyph_count,
        len(bitmap_bytes),
    )
    glyph_table = b"".join(
        _FNT_GLYPH.pack(bitmap_offset, x_offset, glyph_width, x_advance, 0)
        for bitmap_offset, x_offset, glyph_width, x_advance in glyph_entries
    )
    return header + glyph_table + bytes(bitmap_bytes)


def write_fnt(
    output: pathlib.Path,
    font_height: int,
    bitmap_bytes: list[int],
    glyph_entries: list[tuple[int, int, int, int]],
) -> None:
    output.write_bytes(pack_fnt(font_height, bitmap_bytes, glyph_entries))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ttf_path", type=pathlib.Path)
    parser.add_argument("--symbol-prefix", required=True)
    parser.add_argument("--output", type=pathlib.Path, default=None,
                         help="Path for the generated C++ header (PROGMEM, flash-embedded). "
                              "Omit for SD-only fonts that only need --fnt-output.")
    parser.add_argument("--fnt-output", type=pathlib.Path, default=None,
                         help="Optional path for the binary .fnt file (SD-card format, "
                              "see docs/FONT_FNT_FORMAT.md).")
    parser.add_argument("--thumbnail-output", type=pathlib.Path, default=None,
                         help="Optional path for a small flash-resident header covering only "
                              "printable ASCII (32-126), for font-picker button previews.")
    parser.add_argument("--thumbnail-target-height", type=int, default=24,
                         help="Raster height for --thumbnail-output (px). Buttons draw labels "
                              "scaled to ~26%% of the full reading size, so a much smaller "
                              "source raster than --target-height keeps flash usage low "
                              "(default: 24).")
    parser.add_argument("--font-label", required=True)
    parser.add_argument("--target-height", type=int, required=True,
                         help="Desired glyph raster height in px (auto-calibrates point size).")
    parser.add_argument("--target-optical", type=float, default=None,
                         help="Calibrate on the mean of x-height and cap height (px) instead of "
                              "the full ink span. Atkinson, the default reading face: 41.5 at "
                              "72 pt (large), 25.5 at 44 pt (medium).")
    parser.add_argument("--point-size", type=int, default=None,
                         help="Skip calibration and use this point size directly.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    ttfont = TTFont(str(args.ttf_path), lazy=True)
    cmap = ttfont.getBestCmap()

    point_size = args.point_size
    if point_size is None and args.target_optical is not None:
        point_size = calibrate_point_size_optical(args.ttf_path, args.target_optical)
    if point_size is None:
        point_size = calibrate_point_size(args.ttf_path, cmap, args.target_height)

    font_height, bitmap_bytes, glyph_entries = generate(args.ttf_path, point_size, cmap)
    if args.output is not None:
        write_header(args.output, args.symbol_prefix, font_height, bitmap_bytes, glyph_entries,
                     args.font_label, point_size)
    print(f"{args.symbol_prefix}: point_size={point_size} height={font_height} "
          f"bytes={len(bitmap_bytes)}")

    if args.fnt_output is not None:
        write_fnt(args.fnt_output, font_height, bitmap_bytes, glyph_entries)
        fnt_size = args.fnt_output.stat().st_size
        print(f"{args.symbol_prefix}: wrote {args.fnt_output} ({fnt_size} bytes)")

    if args.thumbnail_output is not None:
        thumb_first, thumb_last = 32, 126
        thumb_point_size = max(8, round(point_size * args.thumbnail_target_height / args.target_height))
        thumb_height, thumb_bitmap_bytes, thumb_glyph_entries = generate(
            args.ttf_path, thumb_point_size, cmap, first_char=thumb_first, last_char=thumb_last)
        write_header(args.thumbnail_output, args.symbol_prefix, thumb_height, thumb_bitmap_bytes,
                     thumb_glyph_entries, args.font_label, thumb_point_size,
                     first_char=thumb_first, last_char=thumb_last)
        print(f"{args.symbol_prefix}: wrote {args.thumbnail_output} "
              f"(ascii-only, point_size={thumb_point_size} height={thumb_height} "
              f"bytes={len(thumb_bitmap_bytes)})")


if __name__ == "__main__":
    main()
