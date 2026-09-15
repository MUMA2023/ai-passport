#!/usr/bin/env python3
"""Check that the generated LVGL font still covers every string the app renders.

main/lv_font_pet_16.c is produced by tools/gen_pet_font.py from the string
literals found under main/. Editing Chinese copy without re-running the
generator leaves the font stale, and a stale font fails silently: the missing
character renders as nothing on the device while the compiler, the repository
checks and the host tests all stay green. This check closes that gap.

It also validates the parts of the font structure that LVGL depends on and that
a bad generator would break quietly:

  * LV_FONT_FMT_TXT_CMAP_SPARSE_TINY stores offsets relative to range_start and
    looks them up with lv_utils_bsearch, so the list must be sorted and every
    offset must be smaller than range_length.
  * glyph_id_start values must be contiguous from 1, and the total number of
    characters covered by the cmaps must match the glyph descriptor table.

The required character set is derived here, not taken from the generator. If
this check reused gen_pet_font.collect_characters() for both sides it could only
ever detect a stale font, never a collector that drops characters — which is
exactly how U+2026 "…" went missing while every check reported PASS.

Usage:
    python3 tools/check_font_coverage.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_pet_font  # noqa: E402  (path set up above)

REPO_ROOT = Path(__file__).resolve().parent.parent
FONT_C = REPO_ROOT / "main" / "lv_font_pet_16.c"

# Independent of gen_pet_font's own scan: same inputs, separate implementation.
STRING_LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')


def required_codepoints() -> dict[int, str]:
    """Every non-ASCII character that appears in a main/ string literal."""
    needed: dict[int, str] = {}

    for directory in gen_pet_font.SCAN_DIRS:
        base = REPO_ROOT / directory
        for path in sorted(base.iterdir()):
            if path.suffix not in gen_pet_font.SCAN_SUFFIXES:
                continue
            for literal in STRING_LITERAL.findall(path.read_text(encoding="utf-8")):
                for char in literal:
                    if ord(char) >= 0x80:
                        needed.setdefault(ord(char), char)

    for char in gen_pet_font.EXTRA_SYMBOLS:
        if ord(char) >= 0x80:
            needed.setdefault(ord(char), char)

    return needed


def parse_offset_lists(source: str) -> dict[str, list[int]]:
    lists: dict[str, list[int]] = {}
    for name, body in re.findall(
        r"static const uint16_t (\w+)\[\] = \{(.*?)\};", source, re.S
    ):
        lists[name] = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{4})", body)]
    return lists


def parse_cmaps(source: str) -> list[dict]:
    block = re.search(
        r"static const lv_font_fmt_txt_cmap_t cmaps\[\] = \{(.*?)\n\};", source, re.S
    )
    if not block:
        raise ValueError("no lv_font_fmt_txt_cmap_t table found")

    cmaps = []
    for match in re.finditer(
        r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),"
        r"(.*?)\.type = LV_FONT_FMT_TXT_CMAP_(\w+)",
        block.group(1),
        re.S,
    ):
        cmaps.append(
            {
                "range_start": int(match.group(1)),
                "range_length": int(match.group(2)),
                "glyph_id_start": int(match.group(3)),
                "body": match.group(4),
                "type": match.group(5),
            }
        )
    if not cmaps:
        raise ValueError("cmap table parsed as empty")
    return cmaps


def covered_codepoints(source: str, errors: list[str]) -> set[int]:
    lists = parse_offset_lists(source)
    cmaps = parse_cmaps(source)

    covered: set[int] = set()
    next_glyph_id = 1
    total = 0

    for index, cmap in enumerate(cmaps):
        where = f"cmap[{index}] (0x{cmap['range_start']:04X}, {cmap['type']})"
        if cmap["glyph_id_start"] != next_glyph_id:
            errors.append(
                f"{where}: glyph_id_start is {cmap['glyph_id_start']}, "
                f"expected {next_glyph_id} (must be contiguous from 1)"
            )

        if cmap["type"] == "FORMAT0_TINY":
            if ".unicode_list = NULL" not in cmap["body"]:
                errors.append(f"{where}: FORMAT0_TINY must not carry a unicode_list")
            count = cmap["range_length"]
            covered.update(range(cmap["range_start"], cmap["range_start"] + count))
        elif cmap["type"] == "SPARSE_TINY":
            name_match = re.search(r"\.unicode_list = (\w+)", cmap["body"])
            if not name_match:
                errors.append(f"{where}: SPARSE_TINY without a unicode_list")
                continue
            name = name_match.group(1)
            offsets = lists.get(name)
            if offsets is None:
                errors.append(f"{where}: unicode_list {name} is not defined")
                continue
            declared = int(re.search(r"\.list_length = (\d+)", cmap["body"]).group(1))
            if len(offsets) != declared:
                errors.append(
                    f"{where}: list_length is {declared} but {name} holds {len(offsets)}"
                )
            if offsets != sorted(offsets):
                errors.append(f"{where}: {name} is not sorted; lv_utils_bsearch would miss")
            if offsets and offsets[-1] >= cmap["range_length"]:
                errors.append(
                    f"{where}: largest offset {offsets[-1]} is outside range_length "
                    f"{cmap['range_length']}; LVGL skips the cmap before the lookup"
                )
            count = len(offsets)
            covered.update(cmap["range_start"] + offset for offset in offsets)
        else:
            errors.append(f"{where}: unsupported cmap type {cmap['type']}")
            continue

        total += count
        next_glyph_id += count

    descriptor_block = re.search(
        r"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};", source, re.S
    )
    if not descriptor_block:
        errors.append("no lv_font_fmt_txt_glyph_dsc_t table found")
    else:
        entries = descriptor_block.group(1).count(".bitmap_index")
        if entries != total + 1:
            errors.append(
                f"glyph_dsc holds {entries} entries but the cmaps cover {total} "
                f"characters (expected {total + 1} including the reserved glyph 0)"
            )

    return covered


def main() -> int:
    if not FONT_C.is_file():
        print(f"ERROR: {FONT_C.relative_to(REPO_ROOT)} is missing", file=sys.stderr)
        return 1

    source = FONT_C.read_text(encoding="utf-8")
    errors: list[str] = []
    covered = covered_codepoints(source, errors)

    needed = required_codepoints()
    missing = sorted(set(needed) - covered)

    # Cross-check the generator's own scan against this one: a collector that
    # drops characters would otherwise keep producing a font that matches itself.
    from_generator = {ord(char) for char in gen_pet_font.collect_characters()}
    collector_gap = sorted(set(needed) - from_generator)
    if collector_gap:
        errors.append(
            "tools/gen_pet_font.py collects fewer characters than main/ contains: "
            + " ".join(f"U+{code:04X} {chr(code)}" for code in collector_gap)
        )

    if missing:
        errors.append(
            f"{len(missing)} character(s) are rendered by main/ but have no glyph: "
            + " ".join(f"U+{code:04X} {chr(code)}" for code in missing)
        )
        errors.append("re-run: python3 tools/gen_pet_font.py --font <CJK font file>")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(
        f"Font coverage: PASS ({len(needed)} characters used by main/, "
        f"{len(covered)} glyphs in {FONT_C.name})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
