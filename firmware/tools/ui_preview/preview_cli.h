#pragma once

#include "preview_media.h"

enum {
    PREVIEW_HOME = 0,
    PREVIEW_VOICE,
    PREVIEW_BOOKS,
    PREVIEW_MUSIC,
    PREVIEW_READER,
    PREVIEW_PLAYING,
    PREVIEW_SETTINGS,
    PREVIEW_MONITOR,
    PREVIEW_VIDEO,
};

void preview_show_capture(int page, const preview_media_state_t *media);
uint16_t *preview_capture_pixels(void);
int preview_cli_check(const char *token, preview_media_state_t *media);
int preview_cli_check_audio(const char *token, preview_media_state_t *media);
int preview_cli_screenshots(const char *dir, preview_media_state_t *media);
