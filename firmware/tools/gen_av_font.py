#!/usr/bin/env python3
"""Regenerate the AstrBot voice app's 16 px Chinese font (assets/fonts/av_font_cjk_16.c).

Character inventory (the dynamic-text contract of the app, see main/av_text.h):
  * printable ASCII U+0020..U+007E
  * every GB2312 character: symbols rows 1-9 and all 6763 level-1/level-2 hanzi
  * a few extra punctuation marks that AI replies use but GB2312 lacks
Anything else is replaced by U+25A1 (WHITE SQUARE) at the application boundary.

Source font: Noto Sans SC Regular (SIL OFL 1.1), not committed because of its size.
The script downloads it into a cache directory and checks its SHA-256.

    python3 tools/gen_av_font.py            # writes assets/fonts/av_font_cjk_16.c
    python3 tools/gen_av_font.py --charset  # only writes assets/fonts/av_font_cjk_16.charset.txt
"""
from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_URL = "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf"
FONT_SHA256 = "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9"
LV_FONT_CONV = "lv_font_conv@1.5.3"
FONT_NAME = "av_font_cjk_16"
SIZE = 16
BPP = 4
OUT_C = ROOT / "assets" / "fonts" / f"{FONT_NAME}.c"
OUT_CHARSET = ROOT / "assets" / "fonts" / f"{FONT_NAME}.charset.txt"
EXTRA = "·–—‘’“”…•□、。「」『』～°"


def gb2312_chars() -> list[str]:
    out: list[str] = []
    for hi in range(0xA1, 0xF8):
        if 0xAA <= hi <= 0xAF:  # unassigned rows 10-15
            continue
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def codepoints() -> list[int]:
    cps = set(range(0x20, 0x7F))
    cps.update(ord(c) for c in gb2312_chars())
    cps.update(ord(c) for c in EXTRA)
    return sorted(cps)


def ranges(cps: list[int]) -> list[str]:
    out: list[str] = []
    start = prev = cps[0]
    for cp in cps[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        out.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
        start = prev = cp
    out.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
    return out


def fetch_font(cache: Path) -> Path:
    path = cache / "NotoSansSC-Regular.otf"
    if not path.exists():
        cache.mkdir(parents=True, exist_ok=True)
        print(f"downloading {FONT_URL}", file=sys.stderr)
        urllib.request.urlretrieve(FONT_URL, path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != FONT_SHA256:
        raise SystemExit(f"font checksum mismatch: {digest}")
    return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--charset", action="store_true", help="only write the character inventory")
    ap.add_argument("--font", type=Path, help="use an already downloaded NotoSansSC-Regular.otf")
    ap.add_argument("--cache", type=Path, default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "ai-passport-fonts")
    args = ap.parse_args()

    cps = codepoints()
    OUT_CHARSET.write_text("".join(chr(c) for c in cps) + "\n", encoding="utf-8")
    print(f"{len(cps)} code points -> {OUT_CHARSET.relative_to(ROOT)}")
    if args.charset:
        return 0

    font = args.font or fetch_font(args.cache)
    if hashlib.sha256(font.read_bytes()).hexdigest() != FONT_SHA256:
        raise SystemExit("font checksum mismatch")
    cmd = [
        "npx", "-y", LV_FONT_CONV,
        "--font", str(font), "--range", ",".join(ranges(cps)),
        "--size", str(SIZE), "--bpp", str(BPP), "--format", "lvgl", "--no-compress",
        "--lv-font-name", FONT_NAME, "--lv-include", "lvgl.h",
        "--output", str(OUT_C),
    ]
    subprocess.run(cmd, check=True)
    # Keep machine-specific paths out of the committed source.
    text = OUT_C.read_text(encoding="utf-8").replace(str(font), font.name).replace(str(OUT_C), str(OUT_C.relative_to(ROOT)))
    OUT_C.write_text(text, encoding="utf-8")
    print(f"wrote {OUT_C.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
