# Desktop UI Preview

简体中文：[README.zh_CN.md](README.zh_CN.md)

This host application reuses the firmware LVGL UI, CJK font, images, and KK renderer. Its book and music screens read the existing media HTTP service. It does not connect the voice WebSocket relay.

## Build and run

From the repository root:

```sh
cmake -S firmware/tools/ui_preview -B /tmp/astr2esp32-ui-preview
cmake --build /tmp/astr2esp32-ui-preview
/tmp/astr2esp32-ui-preview/ui_preview
```

The default media service is `http://127.0.0.1:8766`. Set `AV_MEDIA_BASE_URL` to use another media base. If that service requires authentication, set `AV_MEDIA_TOKEN` in the shell before starting the preview. The token is URL encoded for requests and is not logged or stored by the preview.

The build requires CMake, a C compiler, SDL2, libcurl, and the existing `firmware/managed_components/lvgl__lvgl` checkout.

Keys: `Up`/`Down` select items or move through book pages; `Enter` opens a book or starts a track, and pauses or resumes on the playback screen; `Esc` returns one level, stopping playback when leaving it.

## Checks and screenshots

```sh
/tmp/astr2esp32-ui-preview/ui_preview --check
/tmp/astr2esp32-ui-preview/ui_preview --check-audio
/tmp/astr2esp32-ui-preview/ui_preview --screenshots /tmp/astr2esp32-ui-shots
```

`--check` reads the live book and music lists and the first book page, checks PCM chunk handling, and confirms a refused connection is reported as an error. It does not play audio. Screenshot mode uses the current HTTP responses and does not start a window or play audio.

`--check-audio` streams the first live track to SDL's dummy audio driver, pauses for four seconds, resumes, then cancels and checks the queue. It produces no audible output.

## Scope

The preview does not implement voice/WebSocket, Wi-Fi setup, or video playback. The voice page identifies itself as a computer preview. The monitor reports desktop uptime and unknown hardware readings; the media service being reachable does not indicate that voice relay is connected.
