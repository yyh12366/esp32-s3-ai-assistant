#include "av_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp_audio.h"
#include "av_adpcm.h"
#include "av_url.h"

static const char *TAG = "av_media";

#define WORKER_STACK   16384
#define QUEUE_DEPTH    4
#define READ_CHUNK     1024
#define HTTP_TIMEOUT_MS 8000
#define LIST_BODY_MAX  (AV_MEDIA_ITEMS_MAX * AV_MEDIA_TITLE_MAX)
#define URL_MAX        256     // 基址 + 路径 + ?token=，超出则请求失败

typedef enum {
    REQ_BOOKS = 0,
    REQ_TRACKS,
    REQ_BOOK_PAGE,
    REQ_PLAY,
} req_kind_t;

typedef struct {
    uint8_t kind;
    int index;
    int page;
    int cols;
    int lines;
} req_t;

static QueueHandle_t s_queue;
static char s_base[AV_MEDIA_BASE_MAX];
static char s_token[AV_MEDIA_TOKEN_MAX];
static bool s_ready;

static volatile bool s_stop;
static volatile bool s_playing;
static volatile uint32_t s_play_started_ms;
static volatile int s_playing_index;

// 这三个结构体各约 2KB。语音模式不跑媒体，所以按需分配，避免白白吃掉静态 BSS。
static av_media_result_t *s_result;
static av_media_result_t *s_taken;
static av_media_result_t *s_staging;
static volatile bool s_result_ready;
static SemaphoreHandle_t s_result_lock;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void publish(av_media_result_t *result)
{
    if (xSemaphoreTake(s_result_lock, portMAX_DELAY) != pdTRUE) return;
    *s_result = *result;
    s_result_ready = true;
    xSemaphoreGive(s_result_lock);
}

static void publish_error(const char *msg)
{
    memset(s_staging, 0, sizeof(*s_staging));
    s_staging->kind = AV_MEDIA_RES_ERROR;
    snprintf(s_staging->error, sizeof(s_staging->error), "%s", msg);
    ESP_LOGW(TAG, "error: %s", msg);
    publish(s_staging);
}

// 基址 + 路径 + token；失败返回 false（地址放不下）。
static bool build_url(char *buf, size_t size, const char *path)
{
    return av_media_url(buf, size, s_base, path, s_token) >= 0;
}

// 读完整响应体到 buf（最多 size-1 字节，始终补 NUL）。返回字节数，失败返回 -1。
static int http_get_body(const char *path, char *buf, size_t size, int *page_count)
{
    char url[URL_MAX];
    if (!build_url(url, sizeof(url), path)) return -1;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = READ_CHUNK,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        // 日志只打路径，不打带 token 的完整地址
        ESP_LOGW(TAG, "open %s failed: %s", path, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }
    const int64_t status = esp_http_client_fetch_headers(client);
    if (status < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    if (page_count) {
        char *value = NULL;
        if (esp_http_client_get_header(client, "X-Page-Count", &value) == ESP_OK && value) {
            *page_count = atoi(value);
        }
    }
    size_t used = 0;
    while (used + 1 < size) {
        const int n = esp_http_client_read(client, buf + used, (int)(size - 1 - used));
        if (n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (int)used;
}

static void parse_list(const char *body, av_media_result_t *result)
{
    result->list.count = 0;
    const char *line = body;
    while (*line && result->list.count < AV_MEDIA_ITEMS_MAX) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[len - 1] == '\r') len--;
        if (len) {
            if (len >= AV_MEDIA_TITLE_MAX) len = AV_MEDIA_TITLE_MAX - 1;
            memcpy(result->list.titles[result->list.count], line, len);
            result->list.titles[result->list.count][len] = '\0';
            result->list.count++;
        }
        if (!end) break;
        line = end + 1;
    }
}

static void do_list(req_kind_t kind)
{
    char *body = pvPortMalloc(LIST_BODY_MAX);
    if (!body) {
        publish_error("内存不足");
        return;
    }
    const char *path = kind == REQ_BOOKS ? "/books" : "/music";
    // 刚连上 Wi-Fi 时第一次 TCP 握手偶尔超时，重试两次再报错
    int got = -1;
    for (int attempt = 0; attempt < 3 && got < 0; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(1000));
        got = http_get_body(path, body, LIST_BODY_MAX, NULL);
    }
    if (got < 0) {
        vPortFree(body);
        // 带上地址：列表为空和连不上是两回事，后者多半是服务器地址没设对（不显示 token）
        char msg[64];
        snprintf(msg, sizeof(msg), "连不上 %.40s", s_base);
        publish_error(msg);
        return;
    }
    av_media_result_t *r = s_staging;
    memset(r, 0, sizeof(*r));
    r->kind = kind == REQ_BOOKS ? AV_MEDIA_RES_BOOKS : AV_MEDIA_RES_TRACKS;
    parse_list(body, r);
    vPortFree(body);
    ESP_LOGI(TAG, "%s: %d items", kind == REQ_BOOKS ? "books" : "tracks", r->list.count);
    publish(r);
}

static void do_book_page(const req_t *req)
{
    char path[96];
    snprintf(path, sizeof(path), "/books/%d?page=%d&cols=%d&lines=%d",
             req->index, req->page, req->cols, req->lines);
    av_media_result_t *r = s_staging;
    memset(r, 0, sizeof(*r));
    r->kind = AV_MEDIA_RES_BOOK_PAGE;
    r->index = req->index;
    r->page = req->page;
    if (http_get_body(path, r->text, sizeof(r->text), &r->pages) < 0) {
        publish_error("读取章节失败");
        return;
    }
    publish(r);
}

// 精确读满 len 字节：esp_http_client_read 可能短读。读满返回 true；
// 出错、流结束或收到停止请求返回 false。
static bool read_exact(esp_http_client_handle_t client, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        if (s_stop) return false;
        const int n = esp_http_client_read(client, (char *)buf + got, (int)(len - got));
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static void do_play(const req_t *req)
{
    char path[48];
    snprintf(path, sizeof(path), "/music/%d.adpcm", req->index);
    char url[URL_MAX];
    if (!build_url(url, sizeof(url), path)) {
        publish_error("地址过长");
        return;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = READ_CHUNK,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        publish_error("播放器初始化失败");
        return;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        publish_error("无法打开曲目");
        return;
    }
    esp_http_client_fetch_headers(client);

    // 静态缓冲：一个 IMA 块最多 326 字节，解码后最多 640 个采样（1280 字节）。
    static uint8_t block[AV_ADPCM_MAX_BYTES];
    static int16_t pcm[AV_ADPCM_MAX_SAMPLES];
    s_playing = true;
    s_playing_index = req->index;
    s_play_started_ms = now_ms();
    memset(s_staging, 0, sizeof(*s_staging));
    s_staging->kind = AV_MEDIA_RES_PLAY_STARTED;
    s_staging->index = req->index;
    publish(s_staging);

    // 逐块读取：6 字节头 -> 按头部算出整块长度 -> 读剩余字节 -> 解码 -> 写音频。
    while (!s_stop) {
        if (!read_exact(client, block, 6)) break;  // 流结束（块边界）或出错
        const size_t total = av_adpcm_block_bytes(block);
        if (!total) {
            ESP_LOGW(TAG, "ADPCM 块头不合法，停止播放");
            break;
        }
        if (!read_exact(client, block + 6, total - 6)) break;
        const size_t samples = av_adpcm_decode(block, total, pcm, AV_ADPCM_MAX_SAMPLES);
        if (!samples) {
            ESP_LOGW(TAG, "ADPCM 块解码失败，停止播放");
            break;
        }
        // bsp_audio_write 在 I2S DMA 满时阻塞，天然按实时速度限流。
        if (bsp_audio_write(pcm, samples * sizeof(int16_t)) != ESP_OK) break;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_playing = false;

    memset(s_staging, 0, sizeof(*s_staging));
    s_staging->kind = AV_MEDIA_RES_PLAY_ENDED;
    s_staging->index = req->index;
    s_staging->duration_s = (int)((now_ms() - s_play_started_ms) / 1000);
    publish(s_staging);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        req_t req;
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        switch (req.kind) {
        case REQ_BOOKS:
        case REQ_TRACKS: do_list((req_kind_t)req.kind); break;
        case REQ_BOOK_PAGE: do_book_page(&req); break;
        case REQ_PLAY: do_play(&req); break;
        default: break;
        }
    }
}

esp_err_t av_media_start(const char *base_url, const char *token)
{
    if (s_ready) return ESP_OK;
    if (!base_url || !base_url[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(base_url) >= sizeof(s_base) || (token && strlen(token) >= sizeof(s_token))) return ESP_ERR_INVALID_ARG;
    snprintf(s_base, sizeof(s_base), "%s", base_url);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    s_result = heap_caps_calloc(1, sizeof(*s_result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_result) s_result = heap_caps_calloc(1, sizeof(*s_result), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_taken = heap_caps_malloc(sizeof(*s_taken), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_staging = heap_caps_malloc(sizeof(*s_staging), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_result_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(req_t));
    if (!s_result || !s_taken || !s_staging || !s_result_lock || !s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "av_media", WORKER_STACK, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_ready = true;
    ESP_LOGI(TAG, "media client ready, base=%s", s_base);
    return ESP_OK;
}

esp_err_t av_media_set_base(const char *base_url, const char *token)
{
    if (!s_ready) return av_media_start(base_url, token);
    if (!base_url || !base_url[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(base_url) >= sizeof(s_base) || (token && strlen(token) >= sizeof(s_token))) return ESP_ERR_INVALID_ARG;
    snprintf(s_base, sizeof(s_base), "%s", base_url);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    ESP_LOGI(TAG, "media base changed, base=%s", s_base);
    return ESP_OK;
}

static bool enqueue(const req_t *req)
{
    if (!s_ready) return false;
    return xQueueSend(s_queue, req, 0) == pdTRUE;
}

bool av_media_request_books(void)
{
    const req_t req = {.kind = REQ_BOOKS};
    return enqueue(&req);
}

bool av_media_request_tracks(void)
{
    const req_t req = {.kind = REQ_TRACKS};
    return enqueue(&req);
}

bool av_media_request_book_page(int index, int page, int cols, int lines)
{
    const req_t req = {.kind = REQ_BOOK_PAGE, .index = index, .page = page, .cols = cols, .lines = lines};
    return enqueue(&req);
}

bool av_media_request_play(int index)
{
    s_stop = false;  // 新一次播放清除上一次的停止请求
    const req_t req = {.kind = REQ_PLAY, .index = index};
    return enqueue(&req);
}

void av_media_request_stop(void)
{
    // 播放流式循环里 worker 不会回到队列，所以停止必须直接置标志，不能排队。
    s_stop = true;
}

bool av_media_playing(void) { return s_playing; }

int av_media_elapsed_s(void)
{
    if (!s_playing) return 0;
    return (int)((now_ms() - s_play_started_ms) / 1000);
}

const av_media_result_t *av_media_take(void)
{
    if (!s_result_lock || xSemaphoreTake(s_result_lock, 0) != pdTRUE) return NULL;
    if (!s_result_ready) {
        xSemaphoreGive(s_result_lock);
        return NULL;
    }
    *s_taken = *s_result;  // 拷出后再放锁，避免 worker 覆盖正在读的结果
    s_result_ready = false;
    xSemaphoreGive(s_result_lock);
    return s_taken;
}
