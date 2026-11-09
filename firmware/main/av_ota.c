// main/av_ota.c —— 固件 OTA：版本检查、下载写入 ota 分区、校验、切换启动分区、回滚确认。
//
// 下载任务与检查任务各自独立，同一时间只允许一个运行。固件数据用静态缓冲分块读写，
// 不做整包缓存（ESP32-C3 无 PSRAM）。进度与结果只写入 s_status（互斥锁保护），界面轮询读取。
#include "av_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "av_media.h"
#include "av_url.h"

static const char *TAG = "av_ota";

#define TASK_STACK        6144
#define CHUNK_SIZE        2048   // 下载/写入块大小（静态缓冲）
#define MANIFEST_MAX      512    // version.json 上限，正常只有 ~150 字节
#define HTTP_TIMEOUT_MS   8000
#define RESTART_DELAY_MS  1500
#define BOOT_CONFIRM_MS   20000  // 联网后至少运行这么久才确认有效
#define BOOT_GIVE_UP_MS   60000  // 一直没联网也在此时间后确认（界面可用即可）

static SemaphoreHandle_t s_lock;
static av_ota_status_t s_status;          // 由 s_lock 保护
static char s_base[AV_MEDIA_BASE_MAX];    // 检查时记下，更新沿用
static char s_token[AV_MEDIA_TOKEN_MAX];
static av_ota_manifest_t s_manifest;      // 最近一次检查得到的清单（AVAILABLE 时有效）
static char s_manifest_body[MANIFEST_MAX];
static uint8_t s_chunk[CHUNK_SIZE];
static mbedtls_sha256_context s_sha;

static bool s_boot_pending;
static uint32_t s_boot_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

const char *av_ota_current_version(void)
{
    static char identity[AV_OTA_VERSION_MAX];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(identity, sizeof(identity), "%.32s %.16s %.16s", app->version, app->date, app->time);
    return identity;
}

// ---------------------------------------------------------------------------
// 状态发布（任何任务都可以调用）
// ---------------------------------------------------------------------------
static void publish(av_ota_state_t state, uint8_t percent, const char *error)
{
    if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    s_status.state = state;
    s_status.percent = percent;
    s_status.revision++;
    snprintf(s_status.error, sizeof(s_status.error), "%s", error ? error : "");
    xSemaphoreGive(s_lock);
}

static void publish_error(const char *reason)
{
    ESP_LOGW(TAG, "升级失败: %s", reason);
    publish(AV_OTA_ERROR, 0, reason);
}

static void log_heap(const char *what)
{
    ESP_LOGI(TAG, "%s: heap free=%u largest=%u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// ---------------------------------------------------------------------------
// HTTP：路径只写固定名字，token 通过 av_media_url() 附加；日志不打印带 token 的地址
// ---------------------------------------------------------------------------
// 打开 GET 请求并取响应头。成功返回句柄（调用方负责 close/cleanup），status 为 HTTP 状态码。
static esp_http_client_handle_t open_get(const char *path, int *status)
{
    char url[AV_MEDIA_BASE_MAX + 128];
    if (av_media_url(url, sizeof(url), s_base, path, s_token) < 0) return NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGW(TAG, "连接失败: %s", path);
        esp_http_client_cleanup(client);
        return NULL;
    }
    esp_http_client_fetch_headers(client);
    *status = esp_http_client_get_status_code(client);
    return client;
}

static void close_client(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

// 把 HTTP 状态翻译成界面短句
static const char *status_reason(int status)
{
    if (status == 401 || status == 403) return "令牌无效，无法升级";
    if (status == 404) return "服务器暂无固件";
    return "无法连接升级服务器";
}

// ---------------------------------------------------------------------------
// 检查任务：GET version.json，与当前版本比较
// ---------------------------------------------------------------------------
static void check_task(void *arg)
{
    (void)arg;
    int status = 0;
    esp_http_client_handle_t client = open_get("/ota/version.json", &status);
    if (!client) {
        publish_error("无法连接升级服务器");
        vTaskDelete(NULL);
        return;
    }
    if (status != 200) {
        close_client(client);
        publish_error(status_reason(status));
        vTaskDelete(NULL);
        return;
    }
    size_t used = 0;
    while (used + 1 < sizeof(s_manifest_body)) {
        const int n = esp_http_client_read(client, s_manifest_body + used,
                                           (int)(sizeof(s_manifest_body) - 1 - used));
        if (n <= 0) break;
        used += (size_t)n;
    }
    s_manifest_body[used] = '\0';
    close_client(client);

    av_ota_manifest_t manifest;
    if (!av_ota_manifest_parse(s_manifest_body, &manifest)) {
        publish_error("升级清单格式错误");
        vTaskDelete(NULL);
        return;
    }
    s_manifest = manifest;
    const bool same = strcmp(manifest.version, av_ota_current_version()) == 0;
    ESP_LOGI(TAG, "最新版本 %s，%s", manifest.version, same ? "已是最新" : "有新版本");
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_status.latest, sizeof(s_status.latest), "%s", manifest.version);
        s_status.state = same ? AV_OTA_LATEST : AV_OTA_AVAILABLE;
        s_status.percent = 0;
        s_status.error[0] = '\0';
        s_status.revision++;
        xSemaphoreGive(s_lock);
    }
    vTaskDelete(NULL);
}

esp_err_t av_ota_check(const char *media_base, const char *token)
{
    if (!s_lock || !media_base) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (s_status.state == AV_OTA_CHECKING || s_status.state == AV_OTA_DOWNLOADING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_base, sizeof(s_base), "%s", media_base);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    s_status.state = AV_OTA_CHECKING;
    s_status.percent = 0;
    s_status.error[0] = '\0';
    snprintf(s_status.current, sizeof(s_status.current), "%s", av_ota_current_version());
    s_status.revision++;
    xSemaphoreGive(s_lock);

    if (xTaskCreate(check_task, "av_ota_chk", TASK_STACK, NULL, 3, NULL) != pdPASS) {
        publish_error("内存不足，无法检查更新");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 下载任务：写入 ota 分区，校验后切换启动分区并重启
// ---------------------------------------------------------------------------
static void update_task(void *arg)
{
    (void)arg;
    const av_ota_manifest_t manifest = s_manifest;  // 任务开始时固定一份，避免中途被改
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        publish_error("没有可写入的升级分区");
        vTaskDelete(NULL);
        return;
    }
    if (manifest.size > part->size) {
        publish_error("固件超过升级分区大小");
        vTaskDelete(NULL);
        return;
    }
    log_heap("ota begin");

    int status = 0;
    esp_http_client_handle_t client = open_get("/ota/firmware.bin", &status);
    if (!client) {
        publish_error("无法连接升级服务器");
        vTaskDelete(NULL);
        return;
    }
    if (status != 200) {
        close_client(client);
        publish_error(status_reason(status));
        vTaskDelete(NULL);
        return;
    }
    if (esp_http_client_get_content_length(client) != (int64_t)manifest.size) {
        close_client(client);
        publish_error("固件大小与清单不一致");
        vTaskDelete(NULL);
        return;
    }

    // 按清单大小擦除，比 OTA_SIZE_UNKNOWN 少擦几百个扇区，进度不会长时间停在 0%
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, manifest.size, &handle) != ESP_OK) {
        close_client(client);
        publish_error("擦除升级分区失败");
        vTaskDelete(NULL);
        return;
    }
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    publish(AV_OTA_DOWNLOADING, 0, NULL);

    uint32_t done = 0;
    uint8_t shown = 0;
    const char *failure = NULL;
    while (done < manifest.size) {
        const uint32_t want = manifest.size - done < CHUNK_SIZE ? manifest.size - done : CHUNK_SIZE;
        const int n = esp_http_client_read(client, (char *)s_chunk, (int)want);
        if (n <= 0) {
            failure = "下载中断，请重试";
            break;
        }
        if (esp_ota_write(handle, s_chunk, (size_t)n) != ESP_OK) {
            failure = "写入升级分区失败";
            break;
        }
        mbedtls_sha256_update(&s_sha, s_chunk, (size_t)n);
        done += (uint32_t)n;
        const uint8_t pct = (uint8_t)((uint64_t)done * 100 / manifest.size);
        if (pct != shown) {
            shown = pct;
            publish(AV_OTA_DOWNLOADING, pct, NULL);
        }
    }
    close_client(client);

    uint8_t digest[AV_OTA_SHA256_LEN];
    mbedtls_sha256_finish(&s_sha, digest);
    mbedtls_sha256_free(&s_sha);

    if (!failure && memcmp(digest, manifest.sha256, AV_OTA_SHA256_LEN) != 0) {
        failure = "固件校验失败，已放弃";
    }
    if (failure) {
        esp_ota_abort(handle);  // 失败时不改启动分区，当前固件保持可用
        publish_error(failure);
        vTaskDelete(NULL);
        return;
    }
    // esp_ota_end 检查镜像头和结尾校验；失败时句柄已释放，不能再 abort
    if (esp_ota_end(handle) != ESP_OK) {
        publish_error("固件镜像校验失败");
        vTaskDelete(NULL);
        return;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        publish_error("切换启动分区失败");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "写入 %u 字节到 %s，校验通过，准备重启", (unsigned)done, part->label);
    log_heap("ota done");
    publish(AV_OTA_DONE, 100, NULL);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

esp_err_t av_ota_update(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (s_status.state != AV_OTA_AVAILABLE) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = AV_OTA_DOWNLOADING;
    s_status.percent = 0;
    s_status.error[0] = '\0';
    s_status.revision++;
    xSemaphoreGive(s_lock);

    if (xTaskCreate(update_task, "av_ota_dl", TASK_STACK, NULL, 3, NULL) != pdPASS) {
        publish_error("内存不足，无法升级");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void av_ota_get_status(av_ota_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    *out = s_status;
    xSemaphoreGive(s_lock);
}

bool av_ota_busy(void)
{
    av_ota_status_t st;
    av_ota_get_status(&st);
    return st.state == AV_OTA_CHECKING || st.state == AV_OTA_DOWNLOADING || st.state == AV_OTA_DONE;
}

// ---------------------------------------------------------------------------
// 开机与回滚确认
// ---------------------------------------------------------------------------
void av_ota_init_boot(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &state) == ESP_OK) {
        ESP_LOGI(TAG, "运行分区 %s，OTA 状态 %d，版本 %s", run->label, (int)state, av_ota_current_version());
    } else {
        ESP_LOGI(TAG, "运行分区无 OTA 状态记录，版本 %s", av_ota_current_version());
    }
    s_boot_pending = state == ESP_OTA_IMG_PENDING_VERIFY;
    s_boot_ms = now_ms();
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        snprintf(s_status.current, sizeof(s_status.current), "%s", av_ota_current_version());
        xSemaphoreGive(s_lock);
    }
}

void av_ota_boot_poll(bool wifi_up)
{
    if (!s_boot_pending) return;
    const uint32_t up = now_ms() - s_boot_ms;
    if (!((wifi_up && up >= BOOT_CONFIRM_MS) || up >= BOOT_GIVE_UP_MS)) return;
    s_boot_pending = false;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "新固件确认有效（联网=%d，运行 %u ms）: %s", wifi_up ? 1 : 0, (unsigned)up,
             esp_err_to_name(err));
}
