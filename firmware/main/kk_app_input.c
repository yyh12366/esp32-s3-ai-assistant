#include "kk_app_input.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "kk_input";

#define INPUT_QUEUE_DEPTH 16
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

static QueueHandle_t s_events;
static bool s_callback_ready;
static bool s_available;
static uint8_t s_pressed_mask;
static uint8_t s_release_recovery;
static uint32_t s_drop_count;
static uint32_t s_reported_drop_count;
static uint64_t s_last_drop_log_ms;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static key_guard_t s_key_guard;

static uint8_t button_mask(bsp_btn_t button)
{
    return (uint8_t)(1u << (unsigned)button);
}

static void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    if (key_guard_consume(&s_key_guard, event == BSP_BTN_RELEASE, esp_timer_get_time())) {
        return;
    }

    const uint8_t mask = button_mask(button);
    portENTER_CRITICAL(&s_mux);
    if (event == BSP_BTN_PRESS) s_pressed_mask |= mask;
    if (event == BSP_BTN_RELEASE) s_pressed_mask &= (uint8_t)~mask;
    const bool ready = s_callback_ready;
    portEXIT_CRITICAL(&s_mux);
    if (!ready || !s_events) return;

    const kk_app_input_event_t input = { .button = button, .event = event };
    if (xQueueSend(s_events, &input, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_mux);
        ++s_drop_count;
        if (event == BSP_BTN_RELEASE) s_release_recovery |= mask;
        portEXIT_CRITICAL(&s_mux);
    }
}

esp_err_t kk_app_input_init(void)
{
    if (s_events) return ESP_OK;
    s_events = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(kk_app_input_event_t));
    return s_events ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t kk_app_input_start_buttons(void)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "GPIO wake: waiting for key release before dispatch");
    }
    portENTER_CRITICAL(&s_mux);
    s_callback_ready = true;
    portEXIT_CRITICAL(&s_mux);
    esp_err_t error = bsp_button_init(on_button, NULL);
    portENTER_CRITICAL(&s_mux);
    s_available = error == ESP_OK;
    s_callback_ready = s_available;
    if (error != ESP_OK) {
        s_pressed_mask = 0;
        s_release_recovery = 0;
    }
    portEXIT_CRITICAL(&s_mux);
    if (error != ESP_OK) xQueueReset(s_events);
    if (error == ESP_OK && s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) ESP_LOGI(TAG, "wake key released before button startup");
    }
    return error;
}

bool kk_app_input_receive(kk_app_input_event_t *event)
{
    return s_events && event && xQueueReceive(s_events, event, 0) == pdTRUE;
}

uint8_t kk_app_input_pressed_mask(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t mask = s_pressed_mask;
    portEXIT_CRITICAL(&s_mux);
    return mask;
}

uint8_t kk_app_input_take_release_recovery(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t released = s_release_recovery;
    s_release_recovery = 0;
    portEXIT_CRITICAL(&s_mux);
    return released;
}

bool kk_app_input_available(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool available = s_available;
    portEXIT_CRITICAL(&s_mux);
    return available;
}

void kk_app_input_log_drops(uint64_t now_ms)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t dropped = s_drop_count;
    portEXIT_CRITICAL(&s_mux);
    if (dropped == s_reported_drop_count || now_ms - s_last_drop_log_ms < 1000) return;
    ESP_LOGW(TAG, "input queue full; %lu events dropped",
             (unsigned long)(dropped - s_reported_drop_count));
    s_reported_drop_count = dropped;
    s_last_drop_log_ms = now_ms;
}
