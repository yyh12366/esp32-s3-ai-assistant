#include "kk_app.h"
#include "kk_app_input.h"
#include "kk_app_telemetry.h"
#include "kk_audio.h"
#include "kk_core.h"
#include "kk_monitor.h"
#include "kk_render.h"
#include "kk_store.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef KK_HAS_SHELL
#include "av_shell.h"
#endif

static const char *TAG = "kk_app";

#define APP_TASK_STACK 8192

static kk_state_t s_state;
static kk_anim_t s_anim;
static kk_fb_t *s_fb;  // 2.4KB，按需分配
static kk_telemetry_t s_telemetry;
static bool s_audio_ready;
static bool s_monitor_ready;
static bool s_storage_available;
static bool s_display_failed;
static int64_t s_fps_started_ms;
static uint32_t s_fps_frames;
static uint8_t s_last_interval_ms;

static kk_input_t model_input(const kk_app_input_event_t *input)
{
    static const kk_key_event_t events[] = {
        [BSP_BTN_PRESS] = KK_KEY_EVENT_PRESS,
        [BSP_BTN_CLICK] = KK_KEY_EVENT_CLICK,
        [BSP_BTN_DOUBLE] = KK_KEY_EVENT_DOUBLE,
        [BSP_BTN_LONG] = KK_KEY_EVENT_LONG,
        [BSP_BTN_RELEASE] = KK_KEY_EVENT_RELEASE,
    };
    return (kk_input_t){ .key = (kk_key_t)input->button, .event = events[input->event] };
}

static bool input_is_actionable(kk_input_t input)
{
    if (s_state.save_pending) return false;
    const kk_page_t page = kk_model_page(&s_state);
    if (input.key == KK_KEY_OK) {
        if (input.event == KK_KEY_EVENT_CLICK) {
            return page == KK_PAGE_HOME || page == KK_PAGE_SETTINGS_LIST || page == KK_PAGE_MODAL;
        }
        return (input.event == KK_KEY_EVENT_LONG || input.event == KK_KEY_EVENT_DOUBLE) &&
               s_state.stack_depth > 1;
    }
    if (input.key != KK_KEY_UP && input.key != KK_KEY_DOWN) return false;
    if (input.event == KK_KEY_EVENT_CLICK || input.event == KK_KEY_EVENT_DOUBLE) {
        return page == KK_PAGE_HOME || page == KK_PAGE_WAVE ||
               page == KK_PAGE_SETTINGS_LIST || page == KK_PAGE_MODAL;
    }
    return input.event == KK_KEY_EVENT_LONG && page == KK_PAGE_MODAL &&
           s_state.modal_mode == KK_MODAL_EDIT &&
           (s_state.modal == KK_MODAL_BRIGHTNESS || s_state.modal == KK_MODAL_INTERVAL);
}

static void handle_effects(const kk_effects_t *effects)
{
#ifdef KK_HAS_SHELL
    if (effects->flags & KK_EFFECT_EXIT_SHELL) {
        // 合并固件：下次开机回到首页。esp_restart 不返回。
        ESP_LOGI(TAG, "leaving monitor, back to shell");
        av_shell_reboot_to(AV_SHELL_TARGET_SHELL);
    }
#endif
    if (effects->flags & KK_EFFECT_APPLY_BRIGHTNESS) {
        bsp_display_backlight(effects->config.brightness);
    }
    const bool reset = effects->flags & KK_EFFECT_RESET_CONFIG;
    const bool save = effects->flags & KK_EFFECT_SAVE_CONFIG;
    if (!reset && !save) return;

    kk_store_request_t request = {
        .request_id = effects->request_id,
        .reset = reset,
        .config = effects->config,
    };
    if ((!reset && !s_storage_available) || !kk_store_worker_request(&request)) {
        ESP_LOGE(TAG, "settings request %lu could not be queued",
                 (unsigned long)effects->request_id);
        kk_model_save_complete(&s_state, effects->request_id, false,
                               (uint64_t)(esp_timer_get_time() / 1000));
    } else {
        ESP_LOGI(TAG, "settings request %lu queued%s", (unsigned long)effects->request_id,
                 reset ? " for factory reset" : "");
    }
}

static void process_input(const kk_app_input_event_t *input)
{
    kk_input_t action = model_input(input);
    const kk_page_t old_page = kk_model_page(&s_state);
    const bool was_actionable = input_is_actionable(action);
    const kk_config_t *visible = kk_model_visible_config(&s_state);
    const bool beep_enabled = visible->beep_enabled;
    kk_effects_t effects = {0};
    kk_model_input(&s_state, action, (uint64_t)(esp_timer_get_time() / 1000), &effects);

    const kk_page_t new_page = kk_model_page(&s_state);
    if (old_page != new_page) {
        ESP_LOGI(TAG, "page %u -> %u", (unsigned)old_page, (unsigned)new_page);
        if (new_page == KK_PAGE_MODAL) kk_render_anim_reset_modal(&s_anim);
    }
    if (was_actionable && beep_enabled && s_audio_ready &&
        (action.event == KK_KEY_EVENT_CLICK || action.event == KK_KEY_EVENT_DOUBLE ||
         action.event == KK_KEY_EVENT_LONG)) {
        (void)kk_audio_request_beep();
    }
    handle_effects(&effects);
}

static void process_recovered_releases(void)
{
    const uint8_t released = kk_app_input_take_release_recovery();
    for (unsigned button = 0; button < BSP_BTN_COUNT; ++button) {
        if (released & (1u << button)) {
            kk_app_input_event_t input = { .button = (bsp_btn_t)button, .event = BSP_BTN_RELEASE };
            process_input(&input);
        }
    }
}

static void process_store_results(void)
{
    kk_store_result_t result;
    while (kk_store_worker_take(&result)) {
        const uint8_t old_brightness = s_state.config.brightness;
        const bool success = result.result == ESP_OK;
        kk_model_save_complete(&s_state, result.request_id, success,
                               (uint64_t)(esp_timer_get_time() / 1000));
        if (success) {
            s_storage_available = true;
            s_telemetry.storage_available = true;
            if (s_state.config.brightness != old_brightness) {
                bsp_display_backlight(s_state.config.brightness);
            }
            ESP_LOGI(TAG, "settings request %lu committed", (unsigned long)result.request_id);
        } else {
            ESP_LOGE(TAG, "settings request %lu failed: %s",
                     (unsigned long)result.request_id, esp_err_to_name(result.result));
        }
    }
}

static const uint16_t s_theme_colors[KK_THEME_COUNT] = { 0xFFFF, 0x07FF, 0xFD20 };
static uint16_t foreground_color(kk_theme_t theme) { return s_theme_colors[theme]; }

static void render_frame(uint64_t now_ms, float dt_ms)
{
    kk_anim_target_t target;
    kk_render_anim_target(&s_state, now_ms, &target);
    kk_anim_step(&s_anim, &target, dt_ms);
    kk_render_frame(&s_state, &s_anim, &s_telemetry, s_fb, now_ms);
    esp_err_t error = bsp_display_mono2_blit(s_fb->pixels,
                     foreground_color(kk_model_visible_config(&s_state)->theme), 0x0000);
    if (error != ESP_OK) {
        s_display_failed = true;
        ESP_LOGE(TAG, "direct display stopped: %s", esp_err_to_name(error));
        return;
    }
    ++s_fps_frames;
    if (s_fps_started_ms == 0) s_fps_started_ms = (int64_t)now_ms;
    const int64_t elapsed_ms = (int64_t)now_ms - s_fps_started_ms;
    if (elapsed_ms >= 1000) {
        s_telemetry.fps = (int32_t)((uint64_t)s_fps_frames * 1000 / (uint64_t)elapsed_ms);
        s_fps_frames = 0;
        s_fps_started_ms = (int64_t)now_ms;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint64_t previous_ms = (uint64_t)(esp_timer_get_time() / 1000);
    for (;;) {
        kk_app_input_event_t input;
        while (kk_app_input_receive(&input)) process_input(&input);
        process_recovered_releases();
        process_store_results();

        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        kk_app_input_log_drops(now_ms);
        kk_effects_t effects = {0};
        kk_model_tick(&s_state, now_ms, &effects);
        handle_effects(&effects);
        kk_app_telemetry_update(&s_state, &s_telemetry, kk_app_input_pressed_mask(),
                                s_audio_ready, s_monitor_ready,
                                kk_app_input_available(), now_ms);

        const float dt_ms = (float)(now_ms - previous_ms);
        previous_ms = now_ms;
        if (!s_display_failed) render_frame(now_ms, dt_ms);

        const uint8_t interval_ms = kk_model_visible_config(&s_state)->interval_ms;
        const TickType_t period = pdMS_TO_TICKS(interval_ms);
        const TickType_t tick_now = xTaskGetTickCount();
        if (interval_ms != s_last_interval_ms || (TickType_t)(tick_now - last_wake) >= period) {
            last_wake = tick_now;
            s_last_interval_ms = interval_ms;
        }
        vTaskDelayUntil(&last_wake, period);
    }
}

static bool create_app_task(void)
{
    return xTaskCreate(app_task, "kk_ui", APP_TASK_STACK, NULL, 5, NULL) == pdPASS;
}

void kk_app_start(void)
{
    ESP_LOGI(TAG, "KK OLED UI starting");
    ESP_LOGI(TAG, "boot heap free=%lu min=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    kk_model_init(&s_state);
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    kk_anim_target_t initial_target;
    kk_render_anim_target(&s_state, now_ms, &initial_target);
    kk_anim_init(&s_anim, &initial_target);
    kk_app_telemetry_init(&s_telemetry);
    s_telemetry.board_id = "ESP32-C3";

    esp_err_t error = bsp_display_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(error));
        return;
    }
    s_fb = heap_caps_calloc(1, sizeof(*s_fb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return;
    }
    bsp_display_backlight(s_state.config.brightness);

    kk_config_t loaded;
    error = kk_store_init(&s_state.config, &loaded);
    s_storage_available = error == ESP_OK;
    s_telemetry.storage_available = s_storage_available;
    if (s_storage_available) {
        s_state.config = s_state.draft = s_state.pending_config = loaded;
        bsp_display_backlight(loaded.brightness);
    } else {
        ESP_LOGE(TAG, "settings unavailable; normal saves are locked. Use Settings > Factory reset to reset only the kk_ui namespace.");
    }

    error = kk_app_input_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "button event queue unavailable: %s", esp_err_to_name(error));
        return;
    }

    error = bsp_i2c_init();
    const bool i2c_ready = error == ESP_OK;
    if (!i2c_ready) ESP_LOGW(TAG, "shared I2C unavailable; battery/audio stay unknown: %s", esp_err_to_name(error));
    if (i2c_ready) {
        error = kk_monitor_start();
        s_monitor_ready = error == ESP_OK;
        if (!s_monitor_ready) ESP_LOGW(TAG, "battery worker unavailable: %s", esp_err_to_name(error));
        error = kk_audio_start();
        s_audio_ready = error == ESP_OK;
        if (!s_audio_ready) ESP_LOGW(TAG, "audio worker unavailable: %s", esp_err_to_name(error));
    }

    error = kk_store_worker_start();
    if (error != ESP_OK) {
        s_storage_available = false;
        s_telemetry.storage_available = false;
        ESP_LOGE(TAG, "settings worker unavailable: %s", esp_err_to_name(error));
    }
    if (!create_app_task()) {
        ESP_LOGE(TAG, "UI task allocation failed");
        return;
    }
    error = kk_app_input_start_buttons();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "button input unavailable: %s", esp_err_to_name(error));
    }
    ESP_LOGI(TAG, "runtime heap free=%lu min=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "KK OLED UI ready; LVGL task not started");
}
