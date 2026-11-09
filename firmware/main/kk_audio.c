#include "kk_audio.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "kk_audio";

#define AUDIO_RATE 16000
#define AUDIO_CHUNK_SAMPLES 160
#define AUDIO_BEEP_SAMPLES 128
#define AUDIO_COMMAND_DEPTH 8
#define AUDIO_IDLE_TIMEOUT_MS 1000
#define AUDIO_DRAIN_CHUNKS 10

typedef enum {
    AUDIO_COMMAND_BEEP,
    AUDIO_COMMAND_MIC_START,
    AUDIO_COMMAND_MIC_STOP,
} audio_command_t;

typedef struct {
    audio_command_t type;
    uint32_t capture_id;
} audio_request_t;

static QueueHandle_t s_commands;
static QueueHandle_t s_samples;
static bool s_started;
static bool s_mic_requested;
static uint32_t s_capture_id;

static void publish_unavailable(bool available, uint32_t capture_id)
{
    kk_audio_sample_t sample = {
        .mic_level_pct = -1,
        .capture_id = capture_id,
        .available = available,
    };
    xQueueOverwrite(s_samples, &sample);
}

static esp_err_t audio_open(bool initialized, bool sleeping)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (sleeping) return bsp_audio_wake();
    return bsp_audio_set_format(AUDIO_RATE, 16, 1);
}

static esp_err_t play_beep(void)
{
    static int16_t pcm[AUDIO_BEEP_SAMPLES];
    for (size_t i = 0; i < AUDIO_BEEP_SAMPLES; ++i) pcm[i] = (i % 16 < 8) ? 2600 : -2600;
    return bsp_audio_write(pcm, sizeof(pcm));
}

static void publish_wave(const int16_t *pcm, int16_t *ring, size_t *write_index,
                         size_t *sample_count, uint32_t capture_id)
{
    uint64_t sum_squares = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < AUDIO_CHUNK_SAMPLES; ++i) {
        int32_t value = pcm[i];
        sum_squares += (uint64_t)(value * value);
        int32_t magnitude = value < 0 ? -value : value;
        if (magnitude > peak) peak = magnitude;
    }
    if (peak > INT16_MAX) peak = INT16_MAX;
    int32_t rms = (int32_t)(sqrtf((float)sum_squares / AUDIO_CHUNK_SAMPLES) * 100.0f / 32767.0f);
    if (rms > 100) rms = 100;
    ring[*write_index] = (int16_t)peak;
    *write_index = (*write_index + 1) % KK_AUDIO_WAVE_POINTS;
    if (*sample_count < KK_AUDIO_WAVE_POINTS) ++*sample_count;

    kk_audio_sample_t sample = {
        .mic_level_pct = rms,
        .capture_id = capture_id,
        .wave_sample_count = (uint8_t)*sample_count,
        .available = true,
    };
    const size_t start = (*write_index + KK_AUDIO_WAVE_POINTS - *sample_count) % KK_AUDIO_WAVE_POINTS;
    for (size_t i = 0; i < *sample_count; ++i) {
        sample.wave[i] = ring[(start + i) % KK_AUDIO_WAVE_POINTS];
    }
    xQueueOverwrite(s_samples, &sample);
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t pcm[AUDIO_CHUNK_SAMPLES];
    int16_t ring[KK_AUDIO_WAVE_POINTS] = {0};
    size_t write_index = 0;
    size_t sample_count = 0;
    bool initialized = bsp_audio_init() == ESP_OK;
    bool sleeping = false;
    bool opened = false;
    bool mic_active = false;
    uint32_t capture_id = 0;
    int64_t last_use_ms = 0;
    if (!initialized) {
        ESP_LOGI(TAG, "audio unavailable; UI continues without beep/microphone");
        publish_unavailable(false, capture_id);
    } else {
        ESP_LOGI(TAG, "serialized audio worker ready");
        publish_unavailable(true, capture_id);
    }

    for (;;) {
        audio_request_t request;
        TickType_t wait = mic_active ? pdMS_TO_TICKS(1)
                         : opened ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        if (xQueueReceive(s_commands, &request, wait) == pdTRUE) {
            audio_command_t command = request.type;
            if (command == AUDIO_COMMAND_MIC_START && initialized && !mic_active) {
                esp_err_t error = audio_open(initialized, sleeping);
                if (error == ESP_OK) {
                    sleeping = false;
                    opened = true;
                    for (int i = 0; i < AUDIO_DRAIN_CHUNKS && error == ESP_OK; ++i) {
                        error = bsp_audio_read(pcm, sizeof(pcm));
                    }
                }
                if (error == ESP_OK) {
                    memset(ring, 0, sizeof(ring));
                    write_index = sample_count = 0;
                    mic_active = true;
                    capture_id = request.capture_id;
                    last_use_ms = esp_timer_get_time() / 1000;
                    publish_unavailable(true, capture_id);
                    ESP_LOGI(TAG, "microphone capture started");
                } else {
                    ESP_LOGW(TAG, "microphone start failed: %s", esp_err_to_name(error));
                    sleeping = true;
                    opened = false;
                    (void)bsp_audio_sleep();
                    publish_unavailable(false, request.capture_id);
                }
            } else if (command == AUDIO_COMMAND_MIC_STOP && mic_active) {
                mic_active = false;
                esp_err_t error = bsp_audio_sleep();
                sleeping = true;
                opened = false;
                publish_unavailable(true, request.capture_id);
                if (error != ESP_OK) ESP_LOGW(TAG, "audio suspend failed: %s", esp_err_to_name(error));
                ESP_LOGI(TAG, "microphone capture stopped");
            } else if (command == AUDIO_COMMAND_BEEP && initialized) {
                esp_err_t error = audio_open(initialized, sleeping);
                if (error == ESP_OK) {
                    sleeping = false;
                    opened = true;
                    error = play_beep();
                    last_use_ms = esp_timer_get_time() / 1000;
                }
                if (error != ESP_OK) {
                    ESP_LOGW(TAG, "key beep failed: %s", esp_err_to_name(error));
                    publish_unavailable(false, capture_id);
                    if (!mic_active && opened) {
                        sleeping = true;
                        opened = false;
                        (void)bsp_audio_sleep();
                    }
                }
            }
            continue;
        }

        if (mic_active) {
            esp_err_t error = bsp_audio_read(pcm, sizeof(pcm));
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "microphone read failed: %s", esp_err_to_name(error));
                mic_active = false;
                sleeping = true;
                opened = false;
                (void)bsp_audio_sleep();
                publish_unavailable(false, capture_id);
            } else {
                publish_wave(pcm, ring, &write_index, &sample_count, capture_id);
            }
        } else if (opened && esp_timer_get_time() / 1000 - last_use_ms >= AUDIO_IDLE_TIMEOUT_MS) {
            esp_err_t error = bsp_audio_sleep();
            sleeping = true;
            opened = false;
            if (error != ESP_OK) ESP_LOGW(TAG, "audio idle suspend failed: %s", esp_err_to_name(error));
        }
    }
}

esp_err_t kk_audio_start(void)
{
    if (s_started) return ESP_OK;
    s_commands = xQueueCreate(AUDIO_COMMAND_DEPTH, sizeof(audio_request_t));
    s_samples = xQueueCreate(1, sizeof(kk_audio_sample_t));
    if (!s_commands || !s_samples) {
        if (s_commands) vQueueDelete(s_commands);
        if (s_samples) vQueueDelete(s_samples);
        s_commands = s_samples = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "kk_audio", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_commands);
        vQueueDelete(s_samples);
        s_commands = s_samples = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "audio worker started");
    return ESP_OK;
}

bool kk_audio_request_beep(void)
{
    const audio_request_t request = { .type = AUDIO_COMMAND_BEEP };
    return s_commands && xQueueSend(s_commands, &request, 0) == pdTRUE;
}

void kk_audio_set_mic_enabled(bool enabled)
{
    if (!s_commands || s_mic_requested == enabled) return;
    uint32_t next_capture_id = s_capture_id + 1;
    if (next_capture_id == 0) next_capture_id = 1;
    const audio_request_t request = {
        .type = enabled ? AUDIO_COMMAND_MIC_START : AUDIO_COMMAND_MIC_STOP,
        .capture_id = next_capture_id,
    };
    if (xQueueSend(s_commands, &request, 0) == pdTRUE) {
        s_capture_id = next_capture_id;
        s_mic_requested = enabled;
    }
}

uint32_t kk_audio_capture_id(void)
{
    return s_capture_id;
}

bool kk_audio_take(kk_audio_sample_t *sample)
{
    return s_samples && sample && xQueueReceive(s_samples, sample, 0) == pdTRUE;
}
