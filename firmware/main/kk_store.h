#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "kk_core.h"

typedef struct {
    uint32_t request_id;
    bool reset;
    kk_config_t config;
} kk_store_request_t;

typedef struct {
    uint32_t request_id;
    bool reset;
    esp_err_t result;
} kk_store_result_t;

esp_err_t kk_store_init(const kk_config_t *defaults, kk_config_t *loaded);
esp_err_t kk_store_save(const kk_config_t *config);
esp_err_t kk_store_reset(const kk_config_t *defaults);
esp_err_t kk_store_worker_start(void);
bool kk_store_worker_request(const kk_store_request_t *request);
bool kk_store_worker_take(kk_store_result_t *result);
