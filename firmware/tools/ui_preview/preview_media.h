#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PREVIEW_MEDIA_ITEMS_MAX 32
#define PREVIEW_MEDIA_TITLE_MAX 128
#define PREVIEW_MEDIA_BODY_MAX 4096

typedef enum {
    PREVIEW_PLAY_IDLE = 0,
    PREVIEW_PLAY_CONNECTING,
    PREVIEW_PLAY_ACTIVE,
    PREVIEW_PLAY_PAUSED,
    PREVIEW_PLAY_ENDED,
    PREVIEW_PLAY_ERROR,
} preview_play_state_t;

typedef struct {
    char books[PREVIEW_MEDIA_ITEMS_MAX][PREVIEW_MEDIA_TITLE_MAX];
    int book_count;
    char tracks[PREVIEW_MEDIA_ITEMS_MAX][PREVIEW_MEDIA_TITLE_MAX];
    int track_count;
    char body[PREVIEW_MEDIA_BODY_MAX];
    int page;
    int pages;
    int playing_index;
    uint64_t playback_bytes;
    bool loading;
    char error[64];
    preview_play_state_t playback;
    uint64_t revision;
} preview_media_state_t;

bool preview_media_init(const char *base_url, const char *token);
void preview_media_shutdown(void);
void preview_media_cancel(void);
void preview_media_snapshot(preview_media_state_t *state);
void preview_media_request_books(void);
void preview_media_request_tracks(void);
void preview_media_request_book_page(int index, int page);
bool preview_media_request_play(int index);
void preview_media_toggle_pause(void);
