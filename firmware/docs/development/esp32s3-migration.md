**English** · [简体中文](esp32s3-migration.zh_CN.md)

# ESP32-S3 N16R8 unified firmware

This board keeps the unified home menu, AstrBot voice, books, music, settings, and KK monitor, and adds **Video** to the home menu. ESP32-C3 retains its original display and button drivers. Books and music still use the unified repository's relay media service.

| Hardware | ESP32-S3 GPIO |
| --- | --- |
| ILI9341 CS / DC / RST / SCK / MOSI | 10 / 4 / 5 / 12 / 11 |
| ES8311 I2C SDA / SCL | 8 / 9 |
| ES8311 I2S MCLK / BCLK / LRCK | 16 / 17 / 15 |
| ESP32 transmit → module DIN; module DOUT → ESP32 receive | 7; 18 |

Power the ES8311 module from 5 V and share ground with the ESP32. The module amplifier is enabled by default without an extra GPIO. This S3 build enables audio. If the module supply has not been verified, disable `AV_S3_AUDIO_ENABLE` in the S3 menuconfig. No battery gauge, backlight control pin, or touch controller is connected or driven.

Build with ESP-IDF 5.5.3:

```sh
cd firmware
./tools/build_firmware.sh esp32s3
```

The merged image is `build/esp32s3/FoloToy-AI-Passport-full.bin`. The S3 image uses 16 MB flash, 8 MB octal PSRAM, and retains two OTA slots. Use S3 images for both flashing and S3 OTA. Flashing the merged image can overwrite local settings.

After connecting the board, use `idf.py -B build/esp32s3 monitor` to open the serial monitor and use the computer keyboard in place of physical keys:

| Key | Action |
| --- | --- |
| `w` / `s` / `e` | Up / down / OK |
| `W` / `S` / `E` | Corresponding long press |
| `1` / `2` / `3` | Corresponding double click |
| `r` / `t` | Press / release OK in the voice app; press `t` to submit the recording |

Use the existing Bluetooth provisioning and relay-address settings. The new board defaults to a different device ID; existing device sessions and settings do not transfer automatically.

For video, enter **Video** from the home menu, exit the serial monitor (Ctrl+]), and select a file in the existing USB player. The host protocol and video folder are unchanged. To return to the home menu, stop the host player and press RESET. Silent USB video runs separately from voice and monitoring.

Touch support depends on the replacement display's controller and wiring. Touch, physical keys, and battery readings are outside current on-device validation.
