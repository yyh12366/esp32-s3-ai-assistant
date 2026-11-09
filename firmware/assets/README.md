<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### KK OLED 1-bit UI font

[`fonts/kk_font_data.h`](fonts/kk_font_data.h) is the generated renderer table: 185 glyphs covering printable ASCII and every code point in `main/kk_render_texts.c`. It comes from Fusion Pixel Font 12 px monospaced `zh_hans`, release 2026.10.25. The BDF rows are copied on their native pixel grid without scaling or grayscale thresholding. Chinese glyphs use 12 × 12 cells; the source's 6 px ASCII advances are centered into 8 × 12 cells to preserve the renderer's existing text widths. The packed table uses about 5.8 KiB of read-only data and no runtime font allocation.

Source: [`fusion-pixel-12px-monospaced-zh_hans.bdf`](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.10.25), in the official [2026.10.25 release archive](https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.10.25/fusion-pixel-font-12px-monospaced-bdf-v2026.10.25.zip). Archive SHA-256: `d8788cf5ffd6a2dabec397c33c878261195fb9146cdb67d8afd418fe73db15bd`. Extracted BDF SHA-256: `bdb520006cddacccc1d3f780d5fab79007a4d6d1f3559d04665e652af4cdf50f`. The font is licensed under SIL Open Font License 1.1; its copyright and full license are in [`fonts/OFL.txt`](fonts/OFL.txt).

The release's component notices are retained for [Ark Pixel](fonts/licenses/ark-pixel-OFL.txt), [Cubic 11](fonts/licenses/cubic-11-OFL.txt), and [Galmuri](fonts/licenses/galmuri-LICENSE.txt).

Regenerate with Python 3's standard library and `unzip`:

```sh
curl -fsSL 'https://github.com/TakWolf/fusion-pixel-font/releases/download/2026.10.25/fusion-pixel-font-12px-monospaced-bdf-v2026.10.25.zip' -o /tmp/fusion-pixel-12px-bdf.zip
unzip -p /tmp/fusion-pixel-12px-bdf.zip fusion-pixel-12px-monospaced-zh_hans.bdf > /tmp/fusion-pixel-12px-monospaced-zh_hans.bdf
unzip -p /tmp/fusion-pixel-12px-bdf.zip OFL.txt > assets/fonts/OFL.txt
python3 tools/generate_kk_font.py --font /tmp/fusion-pixel-12px-monospaced-zh_hans.bdf
python3 tools/generate_kk_font.py --font /tmp/fusion-pixel-12px-monospaced-zh_hans.bdf --check
```

The generator checks the pinned BDF checksum and metrics, coverage for all UI code points, pixel-cell bounds, and the serialized output. The 5.3 MB source BDF is not stored in the repository; only the fixed UI subset is embedded.

| File | Contents | Source, license, and generation |
| --- | --- | --- |
| [`fonts/av_font_cjk_16.c`](fonts/av_font_cjk_16.c) | LVGL 16 px, 4 bpp, uncompressed; 7544 glyphs: printable ASCII, all of GB2312 (symbol rows and 6763 hanzi), and a few extra punctuation marks; about 1 MB of Flash | Subset of [Noto Sans SC Regular](https://github.com/notofonts/noto-cjk) (`Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`, SHA-256 `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`), SIL Open Font License 1.1; full license text is in [`fonts/licenses/noto-sans-sc-OFL.txt`](fonts/licenses/noto-sans-sc-OFL.txt). Regenerate with `python3 tools/gen_av_font.py` (`lv_font_conv@1.5.3`); the source OTF is downloaded to a cache and is not committed. Used by `main/av_ui.c` with Montserrat 14 as the LVGL symbol fallback. |
| [`fonts/av_font_cjk_16.charset.txt`](fonts/av_font_cjk_16.charset.txt) | The exact character inventory of the font above | Written by `tools/gen_av_font.py`; `tests/test_av_font.py` checks the font against it. |

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/voice_character_source.jpg`](images/voice_character_source.jpg) | 640 × 640, JPEG | Character image supplied by the user for the voice application's main screen. The user authorized public distribution with this project; this does not claim ownership of third-party rights. |
| [`images/voice_character.png`](images/voice_character.png), [`images/av_voice_character.c`](images/av_voice_character.c) | 128 × 128 PNG RGB and LVGL 9 RGB565 | Earlier version with a white background, retained as a source variant. |
| [`images/voice_character_cutout_source.png`](images/voice_character_cutout_source.png) | 1254 × 1254, PNG RGBA | Background extraction from the supplied character using the built-in image generation tool. White clothing and headwear are preserved. The generative edit changes some line and costume details. |
| [`images/voice_character_cutout.png`](images/voice_character_cutout.png), [`images/av_voice_character_cutout.c`](images/av_voice_character_cutout.c) | 128 × 128 PNG RGBA and LVGL 9 RGB565A8, 48 KiB of pixel data | Earlier transparent character, retained as the expression sheet's reference. |
| [`images/voice_character_float.gif`](images/voice_character_float.gif) | 136 × 136, transparent GIF, 2.4 s loop | Earlier floating variant. Regenerate with `python3 tools/gen_av_character_gif.py` using Pillow. |
| [`images/voice_character_states_source.png`](images/voice_character_states_source.png) | 1254 × 1254, PNG RGBA, 2 × 2 sheet | Four facial expressions edited with built-in imagegen from the cutout reference. The first row contains idle and listening, the second thinking and speaking. Some generated clothing and line details differ between states. |
| `images/voice_character_{idle,listening,thinking,speaking}.png`, `images/av_character_{idle,listening,thinking,speaking}.c` | Four 192 × 192 PNG RGBA and LVGL 9 RGB565A8 resources, 108 KiB each | Used by `main/av_ui.c` at a fixed position. Pixels and alpha reside in Flash, with one LVGL image object selecting the current expression. |
| [`images/voice_character_blink_source.png`](images/voice_character_blink_source.png), [`images/voice_character_blink.png`](images/voice_character_blink.png), [`images/av_character_blink.c`](images/av_character_blink.c) | Generated transparent source; 62 × 20 PNG RGBA and LVGL 9 RGB565A8 eye overlay, 3720 bytes | Closed eyes from the existing imagegen frame, cropped into two patches with a transparent gap for the original bangs. The prompt requested gently closed eyelids with matching pose and transparency. `main/av_ui.c` overlays the patches at avatar offset (60, 75) for 140 ms after random 3–5 s intervals while idle. The base sprite stays fixed during blinking. Other screens and voice states pause the LVGL timer. Regenerate with `tools/gen_av_character_states.py`. |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

Regenerate the active expression resources with `python3 tools/gen_av_character_states.py`, using Pillow, `pypng` and `lz4`. The script extracts equal quadrants from the expression sheet, resizes them to 192 × 192 and invokes the pinned LVGL converter. The imagegen prompt requested identical fixed frontal poses on transparent backgrounds, changing eyes, eyebrows and mouth for a relaxed closed-mouth smile, attentive listening, puzzled thinking and cheerful open-mouth speaking.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).
