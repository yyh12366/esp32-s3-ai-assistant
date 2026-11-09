<p align="right"><a href="README.zh_CN.md">简体中文</a></p>

# ESP32-S3 AI Assistant

An ESP32-S3 N16R8 voice assistant project with an ESP-IDF firmware, a Python relay, and a desktop SDL/LVGL UI preview.

## Project layout

| Path | Purpose |
| --- | --- |
| `firmware/` | ESP-IDF firmware for the ESP32-S3 board |
| `relay/` | Python WebSocket relay for ASR, AstrBot, and TTS |
| `firmware/tools/ui_preview/` | Desktop preview that reuses the firmware LVGL interface |
| `media/` | Local book and music files served to the device or preview |
| `tests/` | Python relay unit tests |

## ESP32-S3 hardware and firmware

The S3 target uses an ILI9341 display, an ES8311 audio codec, and an NS4150B amplifier. The board is configured for an N16R8 module.

| Connection | GPIO pins |
| --- | --- |
| ILI9341 CS / DC / RST / SCK / MOSI | 10 / 4 / 5 / 12 / 11 |
| ES8311 I2C SDA / SCL | 8 / 9 |
| ES8311 I2S MCLK / BCLK / WS / DOUT / DIN | 16 / 17 / 15 / 7 / 18 |

Power the board through its 5 V input and connect grounds together. Check the board and each module's input voltage, current, and polarity before wiring or powering them; GPIO signals are not 5 V tolerant.

With ESP-IDF 5.5.3 activated, build from the `firmware/` directory:

```sh
cd firmware
./tools/build_firmware.sh esp32s3
```

The merged image is written to `firmware/build/esp32s3/FoloToy-AI-Passport-full.bin`. To flash and open the serial monitor, connect the board yourself and run from `firmware/`:

```sh
idf.py -B build/esp32s3 flash monitor
```

The default relay URL is empty. Set your relay URL in `menuconfig` or through the device settings/BLUFI provisioning. The device supports a custom relay address such as `relay=ws://<relay-host>:8765/ws`. The S3 firmware has passed build and layout checks; it has not been verified on a physical board.

## Python relay

Requires Python 3.11 or newer. From the repository root:

```sh
python -m venv .venv
. .venv/bin/activate
python -m pip install -e '.[dev]'
cp .env.example .env
cp config.example.yaml config.yaml
```

Edit `.env` with your own `ASTRBOT_API_KEY`, `GROQ_API_KEY`, `BREEZE_API_KEY`, and `BREEZE_VOICE_ID`. Edit `config.yaml` for your AstrBot endpoint and relay settings. If you configure an HTTP proxy for ASR, install the optional proxy dependency with `python -m pip install -e '.[proxy]'` and set the proxy in `config.yaml`.

Load the environment and start the relay:

```sh
set -a
. ./.env
set +a
python -m relay --config config.yaml
```

The relay also serves media on port 8766 by default. To run only the media server, for example for the desktop preview, use:

```sh
python tools/serve_media.py --host 127.0.0.1 --root ./media
```

Keep personal book and music files under `media/books/` and `media/music/`. These files are ignored by Git; only directory markers and documentation are included in this repository.

## Desktop UI preview

The preview requires CMake, a C compiler, SDL2, libcurl, and the fetched LVGL component at `firmware/managed_components/lvgl__lvgl`.

```sh
cmake -S firmware/tools/ui_preview -B build/ui-preview
cmake --build build/ui-preview
build/ui-preview/ui_preview
```

It reads media from `http://127.0.0.1:8766` by default. Set `AV_MEDIA_BASE_URL` to use another base URL and `AV_MEDIA_TOKEN` when the media service requires a token. The book and music screens use real files served by the media endpoint. Use Up/Down to select items or turn book pages, Enter to open a book or play/pause music, and Esc to return.

The desktop preview does not implement voice WebSocket, Wi-Fi, or video playback. It is a UI preview, not a full device simulation.

Run the included relay unit tests with `pytest`. The firmware tests directory is not part of this public snapshot.

## Licenses and media

The repository root is licensed under MIT; the firmware directory retains its original MIT license and attribution. Third-party components and fonts retain their own license notices. The user who supplied the character artwork has authorized its public distribution with this project; this notice does not claim ownership of third-party rights.

Do not add private credentials, device tokens, personal media, or runtime logs to the repository.
