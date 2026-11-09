#include "kk_monitor.h"
#include "bsp_battery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "kk_monitor";

#define BATTERY_SAMPLE_PERIOD_MS 1000

static QueueHandle_t s_samples;
static bool s_started;

static void battery_task(void *arg)
{
    (void)arg;
    kk_battery_sample_t sample = { .battery_mv = -1, .battery_soc = -1 };
    esp_err_t error = bsp_battery_init();
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "battery monitor ready");
    } else if (error == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "battery gauge unavailable");
    } else {
        ESP_LOGW(TAG, "battery monitor unavailable: %s", esp_err_to_name(error));
    }

    for (;;) {
        if (error == ESP_OK) {
            sample.battery_mv = bsp_battery_mv();
            sample.battery_soc = bsp_battery_soc();
        }
        xQueueOverwrite(s_samples, &sample);
        vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_PERIOD_MS));
    }
}

esp_err_t kk_monitor_start(void)
{
    if (s_started) return ESP_OK;
    s_samples = xQueueCreate(1, sizeof(kk_battery_sample_t));
    if (!s_samples) return ESP_ERR_NO_MEM;
    if (xTaskCreate(battery_task, "kk_battery", 3072, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_samples);
        s_samples = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "battery sampling task started");
    return ESP_OK;
}

bool kk_monitor_take(kk_battery_sample_t *sample)
{
    return s_samples && sample && xQueueReceive(s_samples, sample, 0) == pdTRUE;
}
