#include "kk_app_telemetry.h"
#include "kk_audio.h"
#include "kk_monitor.h"
#include "bsp_button.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <math.h>

#define TWO_PI_F 6.28318530718f

static int16_t s_adc_ring[KK_AUDIO_WAVE_POINTS];
static int16_t s_adc_frame[KK_AUDIO_WAVE_POINTS];
static int16_t s_sine_table[KK_AUDIO_WAVE_POINTS];
static int16_t s_sine_frame[KK_AUDIO_WAVE_POINTS];
static kk_audio_sample_t s_audio_sample;
static size_t s_adc_write;
static size_t s_adc_count;
static uint8_t s_sine_phase;
static uint32_t s_capture_id;
static bool s_mic_fresh;
static bool s_wave_mode_initialized;
static bool s_last_wave_active;
static kk_wave_source_t s_last_wave_source;
static int64_t s_start_us;
static int64_t s_heap_sampled_ms;

void kk_app_telemetry_init(kk_telemetry_t *telemetry)
{
    s_start_us = esp_timer_get_time();
    const uint64_t now_ms = (uint64_t)(s_start_us / 1000);
    *telemetry = (kk_telemetry_t){
        .battery_mv = -1,
        .battery_soc = -1,
        .uptime_ms = 0,
        .stopwatch_ms = -1,
        .fps = -1,
        .adc_mv = -1,
        .mic_level_pct = -1,
        .free_heap_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .largest_free_block_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    };
    s_heap_sampled_ms = (int64_t)now_ms;
    for (size_t i = 0; i < KK_AUDIO_WAVE_POINTS; ++i) {
        const float phase = TWO_PI_F * (float)i / KK_AUDIO_WAVE_POINTS;
        s_sine_table[i] = (int16_t)(18000.0f * sinf(phase) + 7000.0f * sinf(phase * 3.0f));
    }
}

void kk_app_telemetry_update(const kk_state_t *state, kk_telemetry_t *telemetry,
                            uint8_t pressed_mask, bool audio_ready,
                            bool monitor_ready, bool input_ready, uint64_t now_ms)
{
    kk_battery_sample_t battery;
    if (monitor_ready && kk_monitor_take(&battery)) {
        telemetry->battery_mv = battery.battery_mv;
        telemetry->battery_soc = battery.battery_soc;
    }

    const bool active = kk_model_page(state) == KK_PAGE_WAVE;
    const kk_wave_source_t source = state->wave_source;
    if (!s_wave_mode_initialized || active != s_last_wave_active || source != s_last_wave_source) {
        s_adc_write = s_adc_count = 0;
        s_sine_phase = 0;
        s_mic_fresh = false;
        s_wave_mode_initialized = true;
        s_last_wave_active = active;
        s_last_wave_source = source;
    }
    if (audio_ready) {
        kk_audio_set_mic_enabled(active && source == KK_WAVE_MIC);
        s_capture_id = kk_audio_capture_id();
        kk_audio_sample_t audio;
        while (kk_audio_take(&audio)) {
            if (audio.capture_id != s_capture_id) continue;
            s_mic_fresh = audio.available && audio.mic_level_pct >= 0;
            if (s_mic_fresh) s_audio_sample = audio;
        }
    }

    telemetry->uptime_ms = (esp_timer_get_time() - s_start_us) / 1000;
    telemetry->stopwatch_ms = -1;
    telemetry->key_pressed_mask = pressed_mask;
    if (now_ms - s_heap_sampled_ms >= 1000) {
        telemetry->free_heap_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        telemetry->largest_free_block_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_heap_sampled_ms = (int64_t)now_ms;
    }

    telemetry->adc_mv = -1;
    telemetry->mic_level_pct = -1;
    telemetry->wave_samples = NULL;
    telemetry->wave_sample_count = 0;
    if (!active) return;

    if (source == KK_WAVE_MIC) {
        if (audio_ready && s_mic_fresh && s_audio_sample.capture_id == s_capture_id) {
            telemetry->mic_level_pct = s_audio_sample.mic_level_pct;
            telemetry->wave_samples = s_audio_sample.wave;
            telemetry->wave_sample_count = s_audio_sample.wave_sample_count;
        }
    } else if (source == KK_WAVE_ADC) {
        const int mv = input_ready ? bsp_button_read_mv() : -1;
        telemetry->adc_mv = mv;
        if (mv >= 0) {
            int32_t amplitude = ((mv - 1650) * 32767) / 1650;
            if (amplitude < -32767) amplitude = -32767;
            if (amplitude > 32767) amplitude = 32767;
            s_adc_ring[s_adc_write] = (int16_t)amplitude;
            s_adc_write = (s_adc_write + 1) % KK_AUDIO_WAVE_POINTS;
            if (s_adc_count < KK_AUDIO_WAVE_POINTS) ++s_adc_count;
            const size_t start = (s_adc_write + KK_AUDIO_WAVE_POINTS - s_adc_count) % KK_AUDIO_WAVE_POINTS;
            for (size_t i = 0; i < s_adc_count; ++i) {
                s_adc_frame[i] = s_adc_ring[(start + i) % KK_AUDIO_WAVE_POINTS];
            }
            telemetry->wave_samples = s_adc_frame;
            telemetry->wave_sample_count = s_adc_count;
        }
    } else {
        for (size_t i = 0; i < KK_AUDIO_WAVE_POINTS; ++i) {
            s_sine_frame[i] = s_sine_table[(i + s_sine_phase) % KK_AUDIO_WAVE_POINTS];
        }
        s_sine_phase = (s_sine_phase + 1) % KK_AUDIO_WAVE_POINTS;
        telemetry->wave_samples = s_sine_frame;
        telemetry->wave_sample_count = KK_AUDIO_WAVE_POINTS;
    }
}
