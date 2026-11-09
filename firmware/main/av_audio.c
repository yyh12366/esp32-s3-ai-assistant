#include "av_audio.h"

#include <string.h>

#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "av_anim.h"
#include "av_app.h"
#include "av_net.h"
#include "av_ring.h"

static const char *TAG = "av_audio";

#define PLAY_CHUNK_BYTES 640  // 20ms

// 播放环放在堆上而不是 .bss：蓝牙配网期间（BLE + Wi-Fi 共存，堆最紧）把它还给堆，
// 退出配网再拿回来；合并固件的小说/音乐/监测模式根本不调 av_audio_init，也就不占这 20KB。
static uint8_t *s_ring_storage;
static av_ring_t s_ring;
static SemaphoreHandle_t s_ring_lock;
static TaskHandle_t s_player_task;
static TaskHandle_t s_mic_task;
static SemaphoreHandle_t s_mic_idle;   // 录音任务处于“未录音”状态时为可获取
static volatile bool s_mic_on;
static volatile int s_mic_level;

static void player_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[PLAY_CHUNK_BYTES];
    for (;;) {
        size_t n = 0;
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        uint32_t before = s_ring.underruns;
        n = av_ring_play_read(&s_ring, chunk, sizeof(chunk), AV_AUDIO_PREBUFFER_BYTES);
        uint32_t underruns = s_ring.underruns;
        xSemaphoreGive(s_ring_lock);
        if (underruns != before) {
            ESP_LOGW(TAG, "播放欠载 #%u，重新积累 320ms 音频", (unsigned)underruns);
        }
        if (n == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        esp_err_t err = bsp_audio_write(chunk, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "播放写入失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void mic_task(void *arg)
{
    (void)arg;
    static uint8_t frame[AV_AUDIO_FRAME_BYTES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_mic_on) continue;
        xSemaphoreTake(s_mic_idle, pdMS_TO_TICKS(50));  // 标记为“录音中”（stop 只会短暂持有它）
        bool failed = false;
        while (s_mic_on) {
            if (bsp_audio_read(frame, sizeof(frame)) != ESP_OK) {
                ESP_LOGE(TAG, "录音读取失败");
                vTaskDelay(pdMS_TO_TICKS(40));
                continue;
            }
            s_mic_level = av_anim_level((const int16_t *)frame, sizeof(frame) / 2);
            if (!s_mic_on) break;  // stop 之后读到的帧不再发送
            if (!failed && av_net_send_binary(frame, sizeof(frame)) != ESP_OK) {
                failed = true;  // 只报告一次，等应用处理
                av_app_post_simple(AV_EV_SEND_FAILED);
            }
        }
        s_mic_level = 0;
        xSemaphoreGive(s_mic_idle);
    }
}

// 先要 640ms；碎片化时退到 384ms（仍大于一半预缓冲，欠载会多一些但能用）。
static uint8_t *ring_alloc(size_t *cap)
{
    static const size_t sizes[] = {AV_AUDIO_RING_BYTES, AV_AUDIO_RING_MIN_BYTES};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint8_t *buf = heap_caps_malloc(sizes[i], MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf) {
            if (i) ESP_LOGW(TAG, "播放缓冲退到 %u 字节", (unsigned)sizes[i]);
            *cap = sizes[i];
            return buf;
        }
    }
    *cap = 0;
    return NULL;
}

esp_err_t av_audio_init(uint8_t volume)
{
    esp_err_t err = bsp_audio_init();
    if (err == ESP_OK) err = bsp_audio_set_format(AV_AUDIO_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    bsp_audio_set_volume(volume);
    size_t cap = 0;
    s_ring_storage = ring_alloc(&cap);
    if (!s_ring_storage) return ESP_ERR_NO_MEM;
    av_ring_init(&s_ring, s_ring_storage, cap);
    s_ring_lock = xSemaphoreCreateMutex();
    s_mic_idle = xSemaphoreCreateBinary();
    if (!s_ring_lock || !s_mic_idle) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_mic_idle);
    // 播放优先级高于网络/界面，避免 I2S 欠载
    if (xTaskCreate(player_task, "av_play", 3072, NULL, 7, &s_player_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(mic_task, "av_mic", 3584, NULL, 6, &s_mic_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void av_audio_set_volume(uint8_t volume) { bsp_audio_set_volume(volume); }

static bool ready(void) { return s_ring_lock && s_mic_idle && s_player_task && s_mic_task; }

void av_audio_mic_start(void)
{
    if (!ready()) return;
    s_mic_on = true;
    xTaskNotifyGive(s_mic_task);
}

void av_audio_mic_stop(void)
{
    if (!ready() || !s_mic_on) return;
    s_mic_on = false;
    // 等录音任务退出循环（当前 40ms 读取 + 可能的一次发送）
    if (xSemaphoreTake(s_mic_idle, pdMS_TO_TICKS(300)) == pdTRUE) {
        xSemaphoreGive(s_mic_idle);
    } else {
        ESP_LOGW(TAG, "录音任务未及时停止");
    }
}

int av_audio_mic_level(void) { return s_mic_level; }

void av_audio_feed(const uint8_t *pcm, size_t len)
{
    if (!ready()) return;
    TickType_t start = xTaskGetTickCount();
    while (len) {
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        if (!s_ring.cap) {  // 配网期间缓冲已归还：直接丢弃
            xSemaphoreGive(s_ring_lock);
            return;
        }
        size_t n = av_ring_write(&s_ring, pcm, len);
        xSemaphoreGive(s_ring_lock);
        if (n) xTaskNotifyGive(s_player_task);
        pcm += n;
        len -= n;
        if (!len) break;
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(1000)) {
            ESP_LOGW(TAG, "播放缓冲长时间满，丢弃 %u 字节", (unsigned)len);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void av_audio_on_server_msg(av_msg_type_t type, av_voice_t state)
{
    if (!ready()) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    av_ring_on_server_msg(&s_ring, type, state);
    uint32_t underruns = s_ring.underruns;
    size_t queued = s_ring.len;
    xSemaphoreGive(s_ring_lock);
    xTaskNotifyGive(s_player_task);
    if (type == AV_MSG_TTS_END) {
        ESP_LOGI(TAG, "播放接收结束: 欠载=%u, 待播=%u 字节", (unsigned)underruns, (unsigned)queued);
    }
}

void av_audio_interrupt(void)
{
    if (!ready()) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    av_ring_interrupt(&s_ring);
    xSemaphoreGive(s_ring_lock);
}

void av_audio_reset(void)
{
    if (!ready()) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    if (s_ring.cap) av_ring_init(&s_ring, s_ring_storage, s_ring.cap);
    xSemaphoreGive(s_ring_lock);
}

void av_audio_release_buffer(void)
{
    if (!ready()) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    uint8_t *buf = s_ring_storage;
    av_ring_interrupt(&s_ring);  // len=0：播放任务读到空缓冲直接返回，不碰 buf
    s_ring.buf = NULL;
    s_ring.cap = 0;
    s_ring_storage = NULL;
    xSemaphoreGive(s_ring_lock);
    if (buf) {
        heap_caps_free(buf);
        ESP_LOGI(TAG, "配网期间归还播放缓冲");
    }
}

esp_err_t av_audio_restore_buffer(void)
{
    if (!ready()) return ESP_ERR_INVALID_STATE;
    if (s_ring_storage) return ESP_OK;
    size_t cap = 0;
    uint8_t *buf = ring_alloc(&cap);
    if (!buf) {
        ESP_LOGE(TAG, "播放缓冲取回失败，回复将无声");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_ring_storage = buf;
    av_ring_init(&s_ring, buf, cap);
    xSemaphoreGive(s_ring_lock);
    return ESP_OK;
}
