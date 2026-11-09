<p align="right"><a href="kk-oled-ui.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# KK OLED UI and hardware monitor

This application starts from local `main` (`488beda`) on `feature/kk-oled-ui`.
It replaces the baseline test-menu startup with an independent monochrome
instrument interface; the original demos remain reference sources only.

## Display and boundaries

The logical display is 120 × 160 at one bit per pixel (2,400 bytes). Primitives
support set, clear, and XOR. Rounded cursors invert the underlying text and
icons pixel by pixel. The BSP scales pixels exactly 2× to the 240 × 320
ST7789P3 and maps lit pixels to white, cyan, or amber RGB565. There is no
antialiasing or full-screen RGB565 allocation.

`main/` owns the state machine, bounded page stack, animation, fonts, screens,
settings, and application workers. `components/bsp/` owns hardware access.
The application does not start LVGL or reuse the test-menu shell. Shared ADC
and I2C ownership remains in the BSP. Button callbacks enqueue events; slow
storage and audio work run outside the callback and rendering path.

## Controls and settings

| Context | UP / DOWN | Short OK | Long or double OK |
| --- | --- | --- | --- |
| Home carousel | Previous / next icon | Open selected page | Stay home |
| Settings list | Previous / next row | Open editor or return | Return home |
| Editor | Adjust; numeric holds repeat | Focus action buttons | Cancel |
| Dialog buttons | Choose Cancel / Confirm | Execute focused action | Cancel |
| Waveform | Previous / next source | No action | Return home |
| Data / About | Read-only | No action | Return home |

The four entries are data, waveform, settings, and about. The data page shows
battery mV/V, SOC, uptime, and measured FPS. The waveform sources are microphone
level, GPIO0 key voltage, and an explicitly labelled synthetic mixed sine wave.
Unavailable hardware readings are never replaced with synthetic sensor data.

| Setting | Range | Initial default |
| --- | --- | --- |
| Brightness | 10–100% | 80% |
| Key sound | Off / on | On |
| Frame interval | 15–50 ms | 25 ms |
| Lit-pixel color | White / cyan / amber | White |

Editors start in adjustment mode. The first short OK focuses the bottom action
buttons. Cancel discards the draft and restores any previewed brightness.
Confirm commits settings to NVS; only a successful commit shows the saved
toast, which slides in, stays 1.2 seconds, and slides out. Failed commits show
an error. Factory reset affects only this application's configuration; it
does not erase other NVS namespaces. Navigation and animation do not write Flash.

## Validation

Activate ESP-IDF 5.5.3, then run `./tools/validate.sh` from this checkout.
The shared gate runs repository checks and host tests, builds with isolated
configuration from `sdkconfig.defaults`, merges the image, and verifies the
partition layout and matching debug archive. The flashable result is
`build/FoloToy-AI-Passport-full.bin`, written at offset `0x0`.

In the unified repository, build this variant with
`./tools/build_firmware.sh kk_oled`; its merged image is
`build/kk_oled/FoloToy-AI-Passport-full.bin`.

Host checks cover the pure navigation, stack, modal actions, rollback, save
completion, repeat, cursor geometry, damping, clipping, and XOR logic.
Host-rendered images check layout and glyph coverage; they do not prove panel
colors, audio, electrical timing, or real-device animation smoothness.

## Device acceptance

1. Boot the exact verified image. Check USB logs for initialization failures,
   watchdog resets, minimum heap, and largest free block. Confirm the redesigned
   carousel and complete Chinese glyphs.
2. Visit all pages repeatedly. Check 2× pixels, portrait orientation, black
   background, theme colors, XOR text inversion, cursor stretching, scrolling,
   modal zoom, switch motion, and toast. Test taps, quick double taps, and that
   releasing a numeric hold stops repeating.
3. Preview minimum and maximum brightness; cancel by both Cancel and long OK.
   Confirm restoration. Save each setting, wait for the saved toast, power-cycle,
   and verify NVS restoration.
4. Check audible clicks, microphone response to ambient sound, ADC waveform
   response to all keys, and distinct synthetic mode. Navigate and save while
   listening; measure responsiveness at both interval limits.
5. Compare battery readings with an instrument and charge state. Check graceful
   unavailable readings if the gauge is absent. Uptime must advance; FPS must
   reflect actual frame timing.
6. Cancel factory reset once, then confirm it. Reboot and check defaults.
   Repeat navigation and saves while observing heap and responsiveness.

Build success is not device acceptance. Flash only after identifying the target
and obtaining approval for the exact artifact. Merged flashing can reset NVS;
use a compatible segmented flash when preserving settings. No full-chip erase
or original-firmware backup is a prerequisite. See the
[firmware data policy](engineering/firmware-layout.md#flashing-and-stored-data).
