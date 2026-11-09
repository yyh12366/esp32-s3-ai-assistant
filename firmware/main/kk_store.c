#include "kk_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "kk_store";

#define KK_STORE_NAMESPACE "kk_ui"
#define KK_STORE_KEY "settings"
#define KK_STORE_MAGIC 0x4B4B5549u
#define KK_STORE_SCHEMA 1

static bool s_flash_ready;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint8_t brightness;
    uint8_t interval_ms;
    uint8_t beep_enabled;
    uint8_t theme;
} kk_store_blob_t;

static esp_err_t validate_config(const kk_config_t *config)
{
    if (!config || config->brightness < KK_BRIGHTNESS_MIN ||
        config->brightness > KK_BRIGHTNESS_MAX ||
        config->interval_ms < KK_INTERVAL_MIN_MS ||
        config->interval_ms > KK_INTERVAL_MAX_MS ||
        config->theme >= KK_THEME_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static kk_store_blob_t make_blob(const kk_config_t *config)
{
    return (kk_store_blob_t){
        .magic = KK_STORE_MAGIC,
        .schema = KK_STORE_SCHEMA,
        .brightness = config->brightness,
        .interval_ms = config->interval_ms,
        .beep_enabled = config->beep_enabled ? 1 : 0,
        .theme = (uint8_t)config->theme,
    };
}

static esp_err_t validate_blob(const kk_store_blob_t *blob)
{
    if (blob->magic != KK_STORE_MAGIC || blob->schema != KK_STORE_SCHEMA ||
        blob->beep_enabled > 1 || blob->theme >= KK_THEME_COUNT ||
        blob->brightness < KK_BRIGHTNESS_MIN || blob->brightness > KK_BRIGHTNESS_MAX ||
        blob->interval_ms < KK_INTERVAL_MIN_MS || blob->interval_ms > KK_INTERVAL_MAX_MS) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static kk_config_t config_from_blob(const kk_store_blob_t *blob)
{
    return (kk_config_t){
        .brightness = blob->brightness,
        .interval_ms = blob->interval_ms,
        .beep_enabled = blob->beep_enabled != 0,
        .theme = (kk_theme_t)blob->theme,
    };
}

static esp_err_t write_blob(nvs_handle_t handle, const kk_config_t *config)
{
    const kk_store_blob_t blob = make_blob(config);
    esp_err_t error = nvs_set_blob(handle, KK_STORE_KEY, &blob, sizeof(blob));
    if (error == ESP_OK) error = nvs_commit(handle);
    return error;
}

static esp_err_t ensure_flash(void)
{
    if (s_flash_ready) return ESP_OK;
    esp_err_t error = nvs_flash_init();
    if (error == ESP_OK || error == ESP_ERR_INVALID_STATE) {
        s_flash_ready = true;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "NVS init failed: %s; settings were not erased", esp_err_to_name(error));
    return error;
}

esp_err_t kk_store_init(const kk_config_t *defaults, kk_config_t *loaded)
{
    if (validate_config(defaults) != ESP_OK || !loaded) return ESP_ERR_INVALID_ARG;

    esp_err_t error = ensure_flash();
    if (error != ESP_OK) return error;
    nvs_handle_t handle;
    error = nvs_open(KK_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;

    kk_store_blob_t blob;
    size_t size = sizeof(blob);
    error = nvs_get_blob(handle, KK_STORE_KEY, &blob, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = write_blob(handle, defaults);
        if (error == ESP_OK) {
            *loaded = *defaults;
            ESP_LOGI(TAG, "fresh settings namespace initialized");
        }
    } else if (error == ESP_OK) {
        error = size == sizeof(blob) ? validate_blob(&blob) : ESP_ERR_INVALID_VERSION;
        if (error == ESP_OK) {
            *loaded = config_from_blob(&blob);
            ESP_LOGI(TAG, "settings loaded schema=%u", KK_STORE_SCHEMA);
        } else {
            ESP_LOGE(TAG, "stored settings have unsupported schema or values; refusing to overwrite them");
        }
    }
    nvs_close(handle);
    if (error != ESP_OK) ESP_LOGE(TAG, "settings load failed: %s", esp_err_to_name(error));
    return error;
}

esp_err_t kk_store_save(const kk_config_t *config)
{
    esp_err_t error = validate_config(config);
    if (error != ESP_OK) return error;
    error = ensure_flash();
    if (error != ESP_OK) return error;
    nvs_handle_t handle;
    error = nvs_open(KK_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = write_blob(handle, config);
    nvs_close(handle);
    if (error == ESP_OK) ESP_LOGI(TAG, "settings committed");
    else ESP_LOGE(TAG, "settings commit failed: %s", esp_err_to_name(error));
    return error;
}

esp_err_t kk_store_reset(const kk_config_t *defaults)
{
    esp_err_t error = validate_config(defaults);
    if (error != ESP_OK) return error;
    error = ensure_flash();
    if (error != ESP_OK) return error;
    nvs_handle_t handle;
    error = nvs_open(KK_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_all(handle);
    if (error == ESP_OK) error = write_blob(handle, defaults);
    nvs_close(handle);
    if (error == ESP_OK) ESP_LOGI(TAG, "app settings namespace reset");
    else ESP_LOGE(TAG, "app settings reset failed: %s", esp_err_to_name(error));
    return error;
}
