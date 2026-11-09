#!/usr/bin/env python3
"""Generate the KK renderer's 1-bit table from the pinned Fusion Pixel BDF."""

from __future__ import annotations

import argparse
import ast
import hashlib
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
TEXTS = ROOT / "main" / "kk_render_texts.c"
OUTPUT = ROOT / "assets" / "fonts" / "kk_font_data.h"
HEIGHT = 12
ASCII_WIDTH = 8
CJK_WIDTH = 12
FONT_SHA256 = "bdb520006cddacccc1d3f780d5fab79007a4d6d1f3559d04665e652af4cdf50f"
FONT_COPYRIGHT = "Copyright (c) 2022, TakWolf (https://takwolf.com); SIL Open Font License 1.1."


def read_texts() -> list[str]:
    source = TEXTS.read_text(encoding="utf-8")
    literals = re.findall(r'"((?:\\.|[^"\\])*)"', source)
    return [ast.literal_eval(f'"{literal}"') for literal in literals]


def codepoints() -> list[int]:
    return sorted({*range(0x20, 0x7F), *(ord(char) for text in read_texts() for char in text)})


def read_bdf(font_path: Path) -> tuple[int, dict[int, tuple[tuple[int, int], tuple[int, int, int, int], list[str]]]]:
    lines = font_path.read_text(encoding="ascii").splitlines()
    ascent = 0
    descent = 0
    font_bbox = None
    glyphs = {}
    required = set(codepoints())
    for line in lines:
        if line.startswith("FONT_ASCENT "):
            ascent = int(line.split()[1])
        elif line.startswith("FONT_DESCENT "):
            descent = int(line.split()[1])
        elif line.startswith("FONTBOUNDINGBOX "):
            font_bbox = tuple(map(int, line.split()[1:]))
        elif line == "CHARS 37857":
            break
    if ascent != 10 or descent != 2 or font_bbox != (12, 12, 0, -2):
        raise RuntimeError(f"unexpected native BDF metrics: {font_bbox}, ascent={ascent}, descent={descent}")

    index = 0
    while index < len(lines):
        if not lines[index].startswith("STARTCHAR "):
            index += 1
            continue
        index += 1
        codepoint = None
        advance = None
        box = None
        bitmap = []
        while index < len(lines) and lines[index] != "ENDCHAR":
            line = lines[index]
            if line.startswith("ENCODING "):
                codepoint = int(line.split()[1])
            elif line.startswith("DWIDTH "):
                advance = tuple(map(int, line.split()[1:]))
            elif line.startswith("BBX "):
                box = tuple(map(int, line.split()[1:]))
            elif line == "BITMAP":
                if box is None:
                    raise RuntimeError("BDF glyph has no BBX before BITMAP")
                index += 1
                bitmap = lines[index:index + box[1]]
                index += box[1]
                continue
            index += 1
        if codepoint in required:
            if codepoint in glyphs or advance is None or box is None:
                raise RuntimeError(f"invalid or duplicate BDF glyph U+{codepoint:04X}")
            glyphs[codepoint] = (advance, box, bitmap)
        index += 1

    missing = sorted(required - glyphs.keys())
    if missing:
        formatted = ", ".join(f"U+{codepoint:04X}" for codepoint in missing)
        raise RuntimeError(f"font source is missing UI glyphs: {formatted}")
    return ascent, glyphs


def rasterize(codepoint: int, ascent: int, glyph: tuple) -> tuple[int, int, list[int]]:
    source_advance, box, rows = glyph
    source_width, source_height, offset_x, offset_y = box
    width = ASCII_WIDTH if codepoint < 0x80 else CJK_WIDTH
    advance = width
    expected_source_advance = 6 if codepoint < 0x80 else 12
    if source_advance != (expected_source_advance, 0):
        raise RuntimeError(f"unexpected native advance for U+{codepoint:04X}: {source_advance}")
    if len(rows) != source_height:
        raise RuntimeError(f"bad BDF row count for U+{codepoint:04X}")

    bitmap = [0] * (HEIGHT * 2)
    row_bytes = (source_width + 7) // 8
    left_pad = (width - source_advance[0]) // 2
    top = ascent - (offset_y + source_height)
    for row, encoded in enumerate(rows):
        source_row = bytes.fromhex(encoded)
        if len(source_row) != row_bytes:
            raise RuntimeError(f"bad BDF row width for U+{codepoint:04X}")
        for col in range(source_width):
            if source_row[col // 8] & (0x80 >> (col & 7)):
                x = left_pad + offset_x + col
                y = top + row
                if x < 0 or x >= width or y < 0 or y >= HEIGHT:
                    raise RuntimeError(f"native glyph exceeds its {width}x{HEIGHT}px cell: U+{codepoint:04X}")
                bitmap[y * 2 + x // 8] |= 0x80 >> (x & 7)
    if codepoint >= 0x80 and not any(bitmap):
        raise RuntimeError(f"source glyph U+{codepoint:04X} is empty")
    return width, advance, bitmap


def render_source(font_path: Path) -> str:
    digest = hashlib.sha256(font_path.read_bytes()).hexdigest()
    if digest != FONT_SHA256:
        raise RuntimeError(f"unexpected Fusion Pixel BDF SHA-256: {digest}")
    ascent, source_glyphs = read_bdf(font_path)
    rows = []
    glyphs = {}
    for codepoint in codepoints():
        width, advance, bitmap = rasterize(codepoint, ascent, source_glyphs[codepoint])
        glyphs[codepoint] = (width, advance, bitmap)
        values = ", ".join(f"0x{value:02X}" for value in bitmap)
        rows.append(f"    {{ 0x{codepoint:04X}, {width}, {advance}, {{ {values} }} }},")
    source = "\n".join([
        "/* Generated by tools/generate_kk_font.py; do not edit. */",
        f"/* Fusion Pixel 12px monospaced zh_hans, 2026.10.25. {FONT_COPYRIGHT} See OFL.txt. */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    uint32_t codepoint;",
        "    uint8_t width;",
        "    uint8_t advance;",
        "    uint8_t bitmap[24];",
        "} kk_font_glyph_t;",
        "",
        "static const kk_font_glyph_t KK_FONT_GLYPHS[] = {",
        *rows,
        "};",
        "",
        "#define KK_FONT_GLYPH_COUNT (sizeof(KK_FONT_GLYPHS) / sizeof(KK_FONT_GLYPHS[0]))",
        "",
    ])
    verify_round_trip(source, glyphs)
    return source


def verify_round_trip(source: str, glyphs: dict[int, tuple[int, int, list[int]]]) -> None:
    rows = re.findall(
        r"\{ 0x([0-9A-F]{4}), (\d+), (\d+), \{ ([0-9A-Fx, ]+) \} \},",
        source,
    )
    if len(rows) != len(codepoints()):
        raise RuntimeError(f"generated {len(rows)} glyphs; expected {len(codepoints())}")
    for codepoint, width, advance, encoded in rows:
        cp = int(codepoint, 16)
        actual = [int(value, 16) for value in encoded.split(", ")]
        if (int(width), int(advance), actual) != glyphs[cp]:
            raise RuntimeError(f"serialized pixels differ from BDF U+{cp:04X}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, required=True, help="pinned 12px monospaced zh_hans BDF")
    parser.add_argument("--check", action="store_true", help="verify output without rewriting it")
    args = parser.parse_args()
    if not args.font.is_file():
        parser.error(f"BDF file not found: {args.font}")
    generated = render_source(args.font)
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_text(encoding="utf-8") != generated:
            print(f"{OUTPUT.relative_to(ROOT)} is stale; regenerate it", file=sys.stderr)
            return 1
    else:
        OUTPUT.write_text(generated, encoding="utf-8")
    print(f"verified {len(codepoints())} native-grid glyphs in {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
