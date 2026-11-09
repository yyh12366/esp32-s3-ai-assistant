#include "preview_cli.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL.h>

#include "preview_audio.h"
#include "preview_media.h"
#include "bsp_pins.h"

static bool wait_for_media(preview_media_state_t *media, int timeout_ms)
{
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        preview_media_snapshot(media);
        if (!media->loading) return true;
        SDL_Delay(10);
    }
    preview_media_snapshot(media);
    return !media->loading;
}

static int save_page(const char *dir, const char *name, int page,
                     const preview_media_state_t *media)
{
    preview_show_capture(page, media);
    SDL_Surface *image = SDL_CreateRGBSurfaceWithFormatFrom(
        preview_capture_pixels(), BSP_LCD_W, BSP_LCD_H, 16, BSP_LCD_W * 2, SDL_PIXELFORMAT_RGB565);
    if (!image) return 1;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
    const int result = SDL_SaveBMP(image, path);
    SDL_FreeSurface(image);
    return result != 0;
}

static int report_request_error(const char *name, const preview_media_state_t *media)
{
    fprintf(stderr, "%s request failed: %s\n", name,
            media->error[0] ? media->error : "timeout");
    return 1;
}

int preview_cli_check(const char *token, preview_media_state_t *media)
{
    if (!preview_audio_self_check()) {
        fprintf(stderr, "PCM parser/pause self-check failed\n");
        return 1;
    }
    preview_media_request_books();
    if (!wait_for_media(media, 9000) || media->error[0]) return report_request_error("books", media);
    printf("books=%d\n", media->book_count);
    if (media->book_count) {
        preview_media_request_book_page(0, 0);
        if (!wait_for_media(media, 9000) || media->error[0])
            return report_request_error("book page", media);
        printf("book_page index=0 pages=%d bytes=%zu%s\n", media->pages, strlen(media->body),
               media->body[0] ? "" : " (no content available)");
    } else {
        printf("book content unavailable: the library is empty\n");
    }
    preview_media_request_tracks();
    if (!wait_for_media(media, 9000) || media->error[0]) return report_request_error("music", media);
    printf("music=%d\n", media->track_count);
    printf("PCM parser/pause/cancel self-check PASS\n");

    preview_media_shutdown();
    if (!preview_media_init("http://127.0.0.1:1", token)) return 1;
    preview_media_request_books();
    if (!wait_for_media(media, 4000) || !media->error[0]) {
        fprintf(stderr, "connection refusal was not reported as an error\n");
        return 1;
    }
    printf("connection refusal reported as error: %s (not an empty library)\n", media->error);
    return 0;
}

static bool wait_for_playback(preview_media_state_t *media, preview_play_state_t state, int timeout_ms)
{
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        preview_media_snapshot(media);
        if (media->playback == state || media->playback == PREVIEW_PLAY_ERROR) break;
        SDL_Delay(10);
    }
    preview_media_snapshot(media);
    return media->playback == state;
}

int preview_cli_check_audio(const char *token, preview_media_state_t *media)
{
    (void)token;
    if (!preview_audio_available()) {
        fprintf(stderr, "dummy SDL audio device unavailable\n");
        return 1;
    }
    preview_media_request_tracks();
    if (!wait_for_media(media, 9000) || media->error[0]) return report_request_error("music", media);
    if (!media->track_count) {
        fprintf(stderr, "no real track available for pause check\n");
        return 1;
    }
    if (!preview_media_request_play(0)) return 1;
    if (!wait_for_playback(media, PREVIEW_PLAY_ACTIVE, 5000)) {
        fprintf(stderr, "PCM stream did not become active: %s\n", media->error);
        preview_media_cancel();
        return 1;
    }
    preview_media_toggle_pause();
    if (!wait_for_playback(media, PREVIEW_PLAY_PAUSED, 1000)) {
        fprintf(stderr, "PCM stream did not pause\n");
        preview_media_cancel();
        return 1;
    }
    SDL_Delay(100);
    preview_media_snapshot(media);
    const uint64_t paused_bytes = media->playback_bytes;
    SDL_Delay(4200);
    preview_media_snapshot(media);
    if (media->playback != PREVIEW_PLAY_PAUSED || media->playback_bytes != paused_bytes) {
        fprintf(stderr, "PCM stream changed while paused\n");
        preview_media_cancel();
        return 1;
    }
    preview_media_toggle_pause();
    for (int elapsed = 0; elapsed < 2500; elapsed += 10) {
        preview_media_snapshot(media);
        if (media->playback_bytes > paused_bytes || media->playback == PREVIEW_PLAY_ERROR) break;
        SDL_Delay(10);
    }
    preview_media_snapshot(media);
    const bool resumed = media->playback_bytes > paused_bytes && media->playback != PREVIEW_PLAY_ERROR;
    preview_media_cancel();
    preview_media_snapshot(media);
    if (!resumed || media->playback != PREVIEW_PLAY_IDLE || preview_audio_queued_bytes() != 0) {
        fprintf(stderr, "PCM resume or cancellation check failed\n");
        return 1;
    }
    printf("real PCM pause/resume/cancel PASS (dummy audio, track index 0)\n");
    return 0;
}

int preview_cli_screenshots(const char *dir, preview_media_state_t *media)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return 1;
    preview_media_request_books();
    wait_for_media(media, 9000);
    if (save_page(dir, "books", PREVIEW_BOOKS, media)) return 1;
    if (!media->error[0] && media->book_count) {
        preview_media_request_book_page(0, 0);
        wait_for_media(media, 9000);
        if (save_page(dir, "reader", PREVIEW_READER, media)) return 1;
    }
    preview_media_request_tracks();
    wait_for_media(media, 9000);
    if (save_page(dir, "music", PREVIEW_MUSIC, media)) return 1;
    const int pages[] = {PREVIEW_HOME, PREVIEW_VOICE, PREVIEW_SETTINGS, PREVIEW_MONITOR};
    const char *names[] = {"home", "voice", "settings", "monitor"};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if (save_page(dir, names[i], pages[i], media)) return 1;
    }
    return 0;
}
