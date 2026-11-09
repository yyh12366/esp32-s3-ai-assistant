#include "kk_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "kk_store";

static QueueHandle_t s_requests;
static QueueHandle_t s_results;
static bool s_started;

static void store_task(void *arg)
{
    (void)arg;
    for (;;) {
        kk_store_request_t request;
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) continue;
        kk_store_result_t result = {
            .request_id = request.request_id,
            .reset = request.reset,
            .result = request.reset ? kk_store_reset(&request.config)
                                    : kk_store_save(&request.config),
        };
        xQueueOverwrite(s_results, &result);
    }
}

esp_err_t kk_store_worker_start(void)
{
    if (s_started) return ESP_OK;
    s_requests = xQueueCreate(1, sizeof(kk_store_request_t));
    s_results = xQueueCreate(1, sizeof(kk_store_result_t));
    if (!s_requests || !s_results) {
        if (s_requests) vQueueDelete(s_requests);
        if (s_results) vQueueDelete(s_results);
        s_requests = s_results = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(store_task, "kk_store", 3072, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_requests);
        vQueueDelete(s_results);
        s_requests = s_results = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "settings worker started");
    return ESP_OK;
}

bool kk_store_worker_request(const kk_store_request_t *request)
{
    return s_requests && request && xQueueSend(s_requests, request, 0) == pdTRUE;
}

bool kk_store_worker_take(kk_store_result_t *result)
{
    return s_results && result && xQueueReceive(s_results, result, 0) == pdTRUE;
}
