#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bsp_button.h"
#include "esp_err.h"

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t event;
} kk_app_input_event_t;

esp_err_t kk_app_input_init(void);
esp_err_t kk_app_input_start_buttons(void);
bool kk_app_input_receive(kk_app_input_event_t *event);
uint8_t kk_app_input_pressed_mask(void);
uint8_t kk_app_input_take_release_recovery(void);
bool kk_app_input_available(void);
void kk_app_input_log_drops(uint64_t now_ms);
