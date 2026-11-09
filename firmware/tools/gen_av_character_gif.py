#!/usr/bin/env python3
"""Export the character's gentle floating motion as a transparent looping GIF.

Requires Pillow. The device uses LVGL object movement with the same amplitude
and period, reading one RGB565A8 character image from Flash.
"""

from pathlib import Path
import math

from PIL import Image


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    image = Image.open(root / "assets/images/voice_character_cutout.png").convert("RGBA")
    assert image.size == (128, 128)
    # Reserve palette index 255 for transparency in every frame.
    palette = image.convert("RGB").quantize(colors=255)
    frames = []
    for frame_number in range(48):
        offset = round(-3 * math.cos(2 * math.pi * frame_number / 48))
        canvas = Image.new("RGBA", (136, 136))
        canvas.alpha_composite(image, (4 + int(offset / 2), 4 + offset))
        frame = canvas.convert("RGB").quantize(palette=palette, dither=Image.Dither.NONE)
        transparent = canvas.getchannel("A").point(lambda alpha: 255 if alpha < 128 else 0)
        frame.paste(255, mask=transparent)
        frame.info["transparency"] = 255
        frames.append(frame)
    output = root / "assets/images/voice_character_float.gif"
    frames[0].save(output, save_all=True, append_images=frames[1:], duration=50,
                   loop=0, transparency=255, disposal=2, optimize=False)
    print(f"Saved {output}")


if __name__ == "__main__":
    main()
