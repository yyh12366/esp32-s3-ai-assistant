#!/usr/bin/env python3
"""Convert the expression sheet and blink frame into LVGL display assets.

Requires Pillow, pypng and lz4 in the active Python environment. The source
sheet has idle/listening in its first row and thinking/speaking in its second.
"""

from pathlib import Path
import subprocess
import sys

from PIL import Image


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    assets = root / "assets/images"
    converter = root / "managed_components/lvgl__lvgl/scripts/LVGLImage.py"
    sheet = Image.open(assets / "voice_character_states_source.png").convert("RGBA")
    assert sheet.width == sheet.height and sheet.width % 2 == 0
    cell_size = sheet.width // 2
    for index, state in enumerate(("idle", "listening", "thinking", "speaking")):
        x = index % 2 * cell_size
        y = index // 2 * cell_size
        frame = sheet.crop((x, y, x + cell_size, y + cell_size))
        frame = frame.resize((192, 192), Image.Resampling.LANCZOS)
        output = assets / f"voice_character_{state}.png"
        frame.save(output)
        subprocess.run([
            sys.executable, str(converter), "--ofmt", "C", "--cf", "RGB565A8",
            "--name", f"av_character_{state}", "-o", str(assets), str(output),
        ], check=True)

    blink = Image.open(assets / "voice_character_blink_source.png").convert("RGBA")
    blink = blink.resize((192, 192), Image.Resampling.LANCZOS)
    # Pack only the two eye crops. The transparent gap preserves the original
    # bangs between them. av_ui.c places this overlay at (60, 75) on the avatar.
    eyes = Image.new("RGBA", (62, 20))
    eyes.paste(blink.crop((60, 75, 85, 95)), (0, 0))
    eyes.paste(blink.crop((98, 75, 122, 95)), (38, 0))
    output = assets / "voice_character_blink.png"
    eyes.save(output)
    subprocess.run([
        sys.executable, str(converter), "--ofmt", "C", "--cf", "RGB565A8",
        "--name", "av_character_blink", "-o", str(assets), str(output),
    ], check=True)


if __name__ == "__main__":
    main()
