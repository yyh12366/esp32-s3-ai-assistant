[简体中文](unified-repository.zh_CN.md) · **English**

# Unified relay and firmware repository

The repository keeps the Python relay at its original root paths and contains
one ESP-IDF project under `firmware/`. Three build targets share one BSP:

- `allinone` (default): one image with both applications. A boot selector picks
  monitor or voice; the choice is stored in NVS and auto-starts after 3 s.
- `kk_oled`: the monitor only.
- `astrbot_voice`: the voice app only.

The monitor's 1-bit direct-blit path and the voice app's LVGL both register the
same LCD completion callback, so they cannot render at the same time. That is
why the combined image switches at boot instead of running both concurrently.

| Part | Source | Behavior |
| --- | --- | --- |
| Relay | `relay/` | WebSocket, ASR, AstrBot, TTS, and audio pacing |
| Relay media | `relay/media.py` | TXT paging and MP3→16 kHz PCM over HTTP for the device |
| KK OLED UI | `firmware/main/kk_*.c` | Mono framebuffer, XOR/damping UI, NVS settings, and hardware monitoring |
| AstrBot Voice | `firmware/main/av_*.c` | Push-to-talk, BLUFI provisioning, voice playback, books, music |
| Boot selector | `firmware/main/kk_boot.c` | Chooses monitor or voice at startup, persisted in NVS |
| Board support | `firmware/components/bsp/` | Shared hardware pins, buses, display, buttons, codec, and battery |

The relay still starts from the repository root with
`python -m relay -c config.yaml`; existing package and deployment entry points
remain valid. Install its development dependencies with
`python -m pip install -e '.[dev]'` in a virtual environment.

Firmware commands require ESP-IDF 5.5.3. See the application manuals for
[combined firmware](../firmware/docs/development/allinone-firmware.md),
[KK OLED UI](../firmware/docs/development/kk-oled-ui.md) and
[AstrBot Voice](../firmware/docs/development/astrbot-voice.md), and the
[firmware agent instructions](../firmware/AGENTS.md).

Build an image (defaults to `allinone`):

```sh
cd firmware
./tools/build_firmware.sh            # allinone
./tools/build_firmware.sh kk_oled
./tools/build_firmware.sh astrbot_voice
```

The merged images are written to
`firmware/build/<variant>/FoloToy-AI-Passport-full.bin`. Flash one with
`./tools/flash_firmware.sh allinone /dev/ttyACM0`, or `idf.py -p <port> write_flash 0x0 <image>`.
`firmware/tools/validate.sh --static` runs both applications' host tests;
`firmware/tools/validate.sh --firmware` builds and verifies all three variants.

## Source provenance

The relay repository's existing history is the base. Firmware sources were
copied from the current local worktrees without changing or deleting them:

- KK UI: `ai-passport`, `feature/kk-oled-ui`, based on `488beda41309f052d81f08d0016d65a3fd85eb7e`, including the native bitmap font improvement.
- Voice: `ai-passport`, `feature/astrbot-voice`, `0dd073b`, including the local BLUFI heap diagnostic and Bluetooth buffer changes.
- Relay: `main`, `63f2a69`, including the local README and launch-agent templates.

The firmware project retains its engineering documentation, tools, component
lock, assets, licenses, and host tests. This source import does not merge the
separate Git histories or create a commit, push, or release.

## Local configuration and data

Runtime `.env`, `config.yaml`, sessions, audio captures, private firmware
profiles, generated binaries, and virtual environments are excluded. Prepare
local configuration from the tracked examples. The original running relay and
its private files remain at their existing location until explicitly migrated.

Each firmware keeps its own NVS settings namespace. Rebuilding does not touch
the device. Writing a merged image at `0x0` can reset NVS; use compatible
segmented flashing when settings must survive. Organizing sources does not
authorize flashing or starting/replacing a deployed service.
