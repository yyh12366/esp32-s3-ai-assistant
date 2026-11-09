#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define KK_AUDIO_WAVE_POINTS 64

typedef struct {
    int16_t wave[KK_AUDIO_WAVE_POINTS];
    int32_t mic_level_pct;
    uint32_t capture_id;
    uint8_t wave_sample_count;
    bool available;
} kk_audio_sample_t;

esp_err_t kk_audio_start(void);
bool kk_audio_request_beep(void);
void kk_audio_set_mic_enabled(bool enabled);
uint32_t kk_audio_capture_id(void);
bool kk_audio_take(kk_audio_sample_t *sample);
