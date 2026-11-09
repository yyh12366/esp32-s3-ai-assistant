[简体中文](allinone-firmware.zh_CN.md) · **English**

# Combined firmware (monitor + voice assistant)

`APP_VARIANT=allinone` builds every application into one image. Boot opens a home
menu (`av_shell.c`): **Voice** (push-to-talk assistant), **Books**, **Music**,
**Settings** and **Monitor** (the KK 1-bit hardware monitor). The first four switch
at runtime without a reboot. Choosing **Monitor** stores the boot target in NVS
namespace `boot`, key `target`, and reboots into the monitor; a long OK press on the
monitor's home page stores the home target and reboots back. The menu labels are
Chinese; the screen names above are the English equivalents.

## Key map

| Screen | UP / DOWN click | OK click | OK long | UP long |
| --- | --- | --- | --- | --- |
| Home | move cursor | enter item | — | — |
| Voice | scroll reply | push-to-talk (press/release) | offline: home | home (after stopping the turn) |
| Books / Music | page / track | open / play / pause | back one level; list level: home | — |
| Settings | move cursor | activate row | root: home | root: home |

Leaving Voice suspends it: the microphone stops, playback is interrupted, the relay
WebSocket disconnects, and no reconnect timer runs until the app returns to the
foreground (`av_session.c` `AV_EV_SUSPEND` / `AV_EV_RESUME`).

## Why the monitor needs a reboot

The monitor's `bsp_display_mono2_blit()` writes the LCD directly, while the voice
app renders through `esp_lvgl_port`. Both register `on_color_trans_done` via
`esp_lcd_panel_io_register_event_callbacks()`, and the SPI panel IO keeps only one
callback (a later registration overwrites the earlier one and logs a warning).
Running both would drop one side's DMA completion signal, stalling the panel or
timing out. The combined image therefore picks one renderer per boot.

Entering or leaving the monitor reboots the device automatically, so the user never
needs to reboot by hand.

## Build and flash

```sh
cd firmware
source $IDF_PATH/export.sh          # ESP-IDF 5.5.3
./tools/build_firmware.sh           # defaults to allinone
./tools/flash_firmware.sh allinone /dev/ttyACM0
```

`./tools/build_firmware.sh kk_oled` and `astrbot_voice` still build the single
applications for debugging or when only one is wanted.

## Resource use (ESP32-C3, no PSRAM)

| Variant | App size | Static DRAM | DRAM left |
| --- | --- | --- | --- |
| `kk_oled` | 403 KB | 84 KB (26%) | 237 KB |
| `astrbot_voice` | 3.11 MB | 225 KB (70%) | 96 KB |
| `allinone` | 3.15 MB | 234 KB (73%) | 88 KB |

The combined image adds about 9 KB of static DRAM over the voice-only build (the
2.4 KB mono framebuffer plus state and stacks). The real pressure is heap while
Wi-Fi, BLUFI, LVGL, audio, and the media HTTP client are all active. Watch the
`heap free/largest/min` line `av_app.c` logs every 30 seconds; treat a largest
block under 20 KB as a signal to trim features.

## Books and music

Both use the relay media HTTP service (`relay/media.py`, default port 8766). The
device never decodes MP3:

- `/books`, `/books/<i>?page=&cols=&lines=` — TXT paging, UTF-8 or GB18030.
- `/music`, `/music/<i>.pcm` — relay transcodes with ffmpeg to 16 kHz/16-bit/mono PCM.

The device feeds that PCM into the same `av_audio_feed()` ring and player task the
voice path uses, so music and TTS never fight over I2S; starting a recording stops
music first. Reading progress (book index + page) is stored in
`astrbot_voice/read_pos`.

## Not verified on hardware

Passing compilation and host tests does not prove these; confirm on the device:

1. Readability and key feel of the home menu and screen switches on the real panel.
2. Minimum usable heap with Wi-Fi, BLUFI, LVGL, and audio all enabled.
3. MP3 streaming continuity (underrun logs), pause/track switch, and exclusion with voice.
4. Paging speed and page-turn latency on a long TXT.
5. NVS read stability when switching from monitor to voice after a reboot.
