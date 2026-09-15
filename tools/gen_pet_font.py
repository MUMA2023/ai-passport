#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the LVGL 9 Chinese bitmap font used by the Desk Pal play.

The repository's default LVGL configuration only enables the Latin Montserrat
faces, so a Chinese interface needs its own font. Instead of shipping a whole
CJK face, this script scans the application sources for the characters that are
actually rendered and emits a tiny subset font.

Usage:
    python3 tools/gen_pet_font.py --font C:/Windows/Fonts/NotoSansSC-VF.ttf

Output:
    main/lv_font_pet_16.c   (glyph data is const, lives in flash/.rodata)

Notes:
    - 2bpp greyscale: clearly better than 1bpp for small CJK glyphs while still
      costing half of 4bpp.
    - The character set is derived from the sources, so adding a new Chinese
      string to the application requires re-running this script.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - developer environment guard
    # Imported lazily-checked rather than fatal: tools/check_font_coverage.py
    # imports this module to reuse collect_characters(), and that check must be
    # runnable on a plain python3 without Pillow.
    Image = ImageDraw = ImageFont = None

BPP = 2
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = ("main",)
SCAN_SUFFIXES = (".c", ".h")
OUT_PATH = os.path.join(REPO_ROOT, "main", "lv_font_pet_16.c")
FONT_SYMBOL = "lv_font_pet_16"

# Punctuation, arrows and the ellipsis used by the interface copy. Every one of
# these has to survive collection even when the application never spells it out,
# so the set is listed rather than inferred.
EXTRA_SYMBOLS = (
    "，。！？：；、‘’“”（）【】《》…—·￥↑↓→←"
    "０１２３４５６７８９"
)


def needs_glyph(char: str) -> bool:
    """Whether `char` has to be in the subset font.

    Everything above ASCII qualifies. Filtering on CJK ranges instead looks
    tidier but silently drops the punctuation that sits outside them — U+2026
    "…" and U+2014 "—" are the ones this interface actually uses. A missing
    glyph is invisible to the compiler and to every other check; it just renders
    as a blank on the device.
    """
    return ord(char) >= 0x80


def collect_characters() -> list[str]:
    """Return the sorted set of non-ASCII characters the application can render."""
    found: set[str] = set()

    string_literal = re.compile(r'"(?:[^"\\]|\\.)*"')
    for directory in SCAN_DIRS:
        base = os.path.join(REPO_ROOT, directory)
        for name in sorted(os.listdir(base)):
            if not name.endswith(SCAN_SUFFIXES):
                continue
            with open(os.path.join(base, name), encoding="utf-8") as handle:
                text = handle.read()
            for literal in string_literal.findall(text):
                for char in literal:
                    if needs_glyph(char):
                        found.add(char)

    for char in EXTRA_SYMBOLS:
        if needs_glyph(char):
            found.add(char)

    return sorted(found, key=ord)


def pack_bits(values: list[int], bpp: int) -> bytes:
    """Pack pixel values MSB-first, the layout LVGL's fmt_txt bitmaps use."""
    out = bytearray()
    accumulator = 0
    bits = 0
    for value in values:
        accumulator = (accumulator << bpp) | (value & ((1 << bpp) - 1))
        bits += bpp
        while bits >= 8:
            bits -= 8
            out.append((accumulator >> bits) & 0xFF)
    if bits:
        out.append((accumulator << (8 - bits)) & 0xFF)
    return bytes(out)


def render_glyph(font: ImageFont.FreeTypeFont, char: str, ascent: int) -> tuple:
    """Render one glyph and return (bitmap, box_w, box_h, ofs_x, ofs_y, adv_w)."""
    adv_w = int(round(font.getlength(char) * 16))  # LVGL expresses adv_w in 1/16 px
    bbox = font.getbbox(char)
    if bbox is None:
        return b"", 0, 0, 0, 0, adv_w
    x0, y0, x1, y1 = bbox
    width, height = x1 - x0, y1 - y0
    if width <= 0 or height <= 0:  # blank glyph such as a space
        return b"", 0, 0, 0, 0, adv_w

    image = Image.new("L", (width, height), 0)
    ImageDraw.Draw(image).text((-x0, -y0), char, font=font, fill=255)
    shift = 8 - BPP
    # tobytes() 对 "L" 图就是逐像素一字节，和 getdata() 等价，
    # 但不受 Pillow 14 弃用 getdata() 的影响。
    values = [pixel >> shift for pixel in image.tobytes()]
    return pack_bits(values, BPP), width, height, x0, ascent - y1, adv_w


def split_cmap_groups(characters: list[str]) -> list[list[str]]:
    """Split sorted characters so every SPARSE_TINY offset fits in uint16."""
    groups: list[list[str]] = []
    for char in characters:
        if not groups or ord(char) - ord(groups[-1][0]) > 0xFFFF:
            groups.append([])
        groups[-1].append(char)
    return groups


def format_bytes(values: bytes, indent: str = "    ", per_line: int = 16) -> list[str]:
    lines = []
    for start in range(0, len(values), per_line):
        chunk = values[start:start + per_line]
        lines.append(indent + "".join(f"0x{byte:02X}," for byte in chunk))
    return lines


def generate(font_path: str, size: int, out_path: str) -> None:
    if Image is None:
        sys.exit("Pillow is required: python -m pip install pillow")
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()

    ascii_chars = [chr(code) for code in range(0x20, 0x7F)]
    extra_chars = collect_characters()
    if not extra_chars:
        sys.exit("No non-ASCII characters found; nothing to generate.")

    bitmap = bytearray()
    descriptors = [(0, 0, 0, 0, 0, 0)]  # glyph id 0 is reserved
    for char in ascii_chars + extra_chars:
        glyph_bitmap, box_w, box_h, ofs_x, ofs_y, adv_w = render_glyph(font, char, ascent)
        descriptors.append((len(bitmap), adv_w, box_w, box_h, ofs_x, ofs_y))
        bitmap += glyph_bitmap

    groups = split_cmap_groups(extra_chars)
    lines: list[str] = []
    lines.append("// Generated by tools/gen_pet_font.py; do not edit manually.")
    lines.append("// Noto Sans SC is licensed under the SIL Open Font License 1.1.")
    lines.append(f"// Font: {os.path.basename(font_path)}  Size: {size}px  Depth: {BPP}bpp")
    lines.append(f"// Charset: ASCII(0x20-0x7E) + {len(extra_chars)} characters used by main/")
    lines.append("// Glyph data is const and stays in flash (.rodata); it does not use DRAM.")
    lines.append("// Re-run tools/gen_pet_font.py after adding new Chinese interface text.")
    lines.append('#include "lvgl.h"')
    lines.append("")
    lines.append("static const uint8_t glyph_bitmap[] = {")
    lines.extend(format_bytes(bitmap))
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    for index, (bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y) in enumerate(descriptors):
        lines.append(
            f"    {{.bitmap_index = {bitmap_index}, .adv_w = {adv_w}, .box_w = {box_w}, "
            f".box_h = {box_h}, .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}}, /* {index} */"
        )
    lines.append("};")
    lines.append("")
    lines.append("/* Codepoints are sparse, so offsets are listed explicitly. */")
    for index, group in enumerate(groups):
        if len(group) == 1:
            continue
        base = ord(group[0])
        offsets = [ord(char) - base for char in group]
        lines.append(f"static const uint16_t unicode_list_extra_{index}[] = {{")
        for start in range(0, len(offsets), 12):
            chunk = offsets[start:start + 12]
            lines.append("    " + "".join(f"0x{value:04X}, " for value in chunk).rstrip())
        lines.append("};")
        lines.append("")

    lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    lines.append("    {")
    lines.append(
        f"        .range_start = 32, .range_length = {len(ascii_chars)}, "
        f".glyph_id_start = 1,"
    )
    lines.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,")
    lines.append("        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
    lines.append("    },")
    glyph_id_start = 1 + len(ascii_chars)
    for index, group in enumerate(groups):
        base = ord(group[0])
        lines.append("    {")
        lines.append(
            f"        .range_start = {base}, .range_length = {ord(group[-1]) - base + 1}, "
            f".glyph_id_start = {glyph_id_start},"
        )
        if len(group) == 1:
            lines.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,")
            lines.append("        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
        else:
            lines.append(
                f"        .unicode_list = unicode_list_extra_{index}, "
                f".glyph_id_ofs_list = NULL, .list_length = {len(group)},"
            )
            lines.append("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY")
        glyph_id_start += len(group)
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    lines.append("    .glyph_bitmap = glyph_bitmap,")
    lines.append("    .glyph_dsc = glyph_dsc,")
    lines.append("    .cmaps = cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append(f"    .cmap_num = {1 + len(groups)},")
    lines.append(f"    .bpp = {BPP},")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = 0,")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_font_t {FONT_SYMBOL} = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append(f"    .line_height = {ascent + descent},")
    lines.append(f"    .base_line = {descent},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -2,")
    lines.append("    .underline_thickness = 1,")
    lines.append("    .dsc = &font_dsc,")
    lines.append("    .fallback = NULL,")
    lines.append("    .user_data = NULL,")
    lines.append("};")
    lines.append("")

    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))

    print(
        f"wrote {os.path.relpath(out_path, REPO_ROOT)}  glyphs={len(descriptors) - 1} "
        f"bitmap={len(bitmap) / 1024:.1f}KiB line_height={ascent + descent}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, help="Path to an OFL-compatible CJK TrueType/OpenType font")
    parser.add_argument("--size", type=int, default=16, help="Pixel size (default: 16)")
    parser.add_argument("--out", default=OUT_PATH, help="Output C file")
    args = parser.parse_args()
    if not os.path.isfile(args.font):
        sys.exit(f"Font not found: {args.font}")
    generate(args.font, args.size, args.out)


if __name__ == "__main__":
    main()
