#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t low_byte;
    bool has_low_byte;
} preview_audio_stream_t;

bool preview_audio_init(void);
void preview_audio_shutdown(void);
void preview_audio_begin(uint64_t generation);
void preview_audio_cancel(uint64_t generation);
bool preview_audio_available(void);
uint32_t preview_audio_queued_bytes(void);
bool preview_audio_set_paused(uint64_t generation, bool paused);
bool preview_audio_write_pcm16le(uint64_t generation, preview_audio_stream_t *stream,
                                 const uint8_t *bytes, size_t length);
void preview_audio_wait_drained(uint64_t generation);
bool preview_audio_self_check(void);
