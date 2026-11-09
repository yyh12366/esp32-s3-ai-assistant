#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kk_anim.h"
#include "kk_core.h"
#include "kk_fb.h"

#define KK_KEY_MASK_UP (1u << KK_KEY_UP)
#define KK_KEY_MASK_DOWN (1u << KK_KEY_DOWN)
#define KK_KEY_MASK_OK (1u << KK_KEY_OK)

typedef struct {
    int32_t battery_mv;
    int32_t battery_soc;
    int64_t uptime_ms;
    int64_t stopwatch_ms;
    int32_t fps;
    int32_t adc_mv;
    int32_t mic_level_pct;
    const int16_t *wave_samples;
    size_t wave_sample_count;
    const char *board_id;
    uint32_t free_heap_bytes;
    uint32_t largest_free_block_bytes;
    uint8_t key_pressed_mask;
    bool storage_available;
} kk_telemetry_t;

void kk_render_anim_target(const kk_state_t *state, uint64_t now_ms,
                           kk_anim_target_t *target);
void kk_render_anim_reset_modal(kk_anim_t *anim);
void kk_render_frame(const kk_state_t *state, const kk_anim_t *anim,
                     const kk_telemetry_t *telemetry, kk_fb_t *fb, uint64_t now_ms);
