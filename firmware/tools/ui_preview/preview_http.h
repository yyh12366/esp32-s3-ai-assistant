#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "preview_audio.h"

typedef enum {
    PREVIEW_HTTP_OK = 0,
    PREVIEW_HTTP_CONNECT_ERROR,
    PREVIEW_HTTP_TRANSFER_ERROR,
    PREVIEW_HTTP_STATUS_ERROR,
    PREVIEW_HTTP_TOO_LARGE,
    PREVIEW_HTTP_URL_ERROR,
    PREVIEW_HTTP_CANCELLED,
    PREVIEW_HTTP_AUDIO_ERROR,
    PREVIEW_HTTP_PCM_ERROR,
} preview_http_error_t;

typedef struct {
    preview_http_error_t error;
    long status;
    long page_count;
    size_t length;
} preview_http_result_t;

typedef bool (*preview_http_is_current_fn)(uint64_t generation);
typedef void (*preview_http_audio_data_fn)(uint64_t generation, size_t bytes);

bool preview_http_init(const char *base_url, const char *token, preview_http_is_current_fn is_current);
void preview_http_shutdown(void);
preview_http_result_t preview_http_get_text(uint64_t generation, const char *path,
                                            char *body, size_t capacity);
preview_http_result_t preview_http_stream_pcm(uint64_t generation, const char *path,
                                              preview_audio_stream_t *stream,
                                              preview_http_audio_data_fn audio_data);
