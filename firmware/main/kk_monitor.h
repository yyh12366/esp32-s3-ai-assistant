#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int battery_mv;
    int battery_soc;
} kk_battery_sample_t;

esp_err_t kk_monitor_start(void);
bool kk_monitor_take(kk_battery_sample_t *sample);
