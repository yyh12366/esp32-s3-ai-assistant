#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KK_PAGE_STACK_MAX 8
#define KK_SETTINGS_COUNT 6
#define KK_BRIGHTNESS_MIN 10
#define KK_BRIGHTNESS_MAX 100
#define KK_INTERVAL_MIN_MS 15
#define KK_INTERVAL_MAX_MS 50

typedef enum {
    KK_PAGE_HOME = 0,
    KK_PAGE_DATA,
    KK_PAGE_WAVE,
    KK_PAGE_SETTINGS_LIST,
    KK_PAGE_ABOUT,
    KK_PAGE_MODAL,
} kk_page_t;

typedef enum {
    KK_CAROUSEL_DATA = 0,
    KK_CAROUSEL_WAVE,
    KK_CAROUSEL_SETTINGS,
    KK_CAROUSEL_ABOUT,
    KK_CAROUSEL_COUNT,
} kk_carousel_t;

typedef enum {
    KK_SETTING_BRIGHTNESS = 0,
    KK_SETTING_BEEP,
    KK_SETTING_INTERVAL,
    KK_SETTING_THEME,
    KK_SETTING_FACTORY_RESET,
    KK_SETTING_BACK,
} kk_setting_t;

typedef enum {
    KK_MODAL_NONE = 0,
    KK_MODAL_BRIGHTNESS,
    KK_MODAL_BEEP,
    KK_MODAL_INTERVAL,
    KK_MODAL_THEME,
    KK_MODAL_FACTORY_RESET,
} kk_modal_t;

typedef enum {
    KK_MODAL_EDIT = 0,
    KK_MODAL_ACTION,
} kk_modal_mode_t;

typedef enum {
    KK_ACTION_CANCEL = 0,
    KK_ACTION_CONFIRM,
} kk_action_t;

typedef enum {
    KK_THEME_WHITE = 0,
    KK_THEME_CYAN,
    KK_THEME_AMBER,
    KK_THEME_COUNT,
} kk_theme_t;

typedef enum {
    KK_WAVE_MIC = 0,
    KK_WAVE_ADC,
    KK_WAVE_SINE,
    KK_WAVE_SOURCE_COUNT,
} kk_wave_source_t;

typedef struct {
    uint8_t brightness;
    uint8_t interval_ms;
    bool beep_enabled;
    kk_theme_t theme;
} kk_config_t;

typedef enum {
    KK_TOAST_NONE = 0,
    KK_TOAST_SAVED,
    KK_TOAST_RESET,
    KK_TOAST_SAVE_FAILED,
} kk_toast_t;

typedef enum {
    KK_KEY_UP = 0,
    KK_KEY_DOWN,
    KK_KEY_OK,
} kk_key_t;

typedef enum {
    KK_KEY_EVENT_PRESS = 0,
    KK_KEY_EVENT_CLICK,
    KK_KEY_EVENT_DOUBLE,
    KK_KEY_EVENT_LONG,
    KK_KEY_EVENT_RELEASE,
} kk_key_event_t;

typedef struct {
    kk_key_t key;
    kk_key_event_t event;
} kk_input_t;

typedef enum {
    KK_EFFECT_NONE = 0,
    KK_EFFECT_APPLY_BRIGHTNESS = 1 << 0,
    KK_EFFECT_SAVE_CONFIG = 1 << 1,
    KK_EFFECT_RESET_CONFIG = 1 << 2,
    KK_EFFECT_EXIT_SHELL = 1 << 3,  // 在首页长按 OK：请求回到合并固件的首页（仅 allinone 生效）
} kk_effect_flag_t;

typedef struct {
    uint8_t flags;
    uint32_t request_id;
    kk_config_t config;
} kk_effects_t;

typedef struct {
    kk_page_t stack[KK_PAGE_STACK_MAX];
    uint8_t stack_depth;
    kk_carousel_t carousel_index;
    uint8_t settings_cursor;
    kk_wave_source_t wave_source;
    kk_modal_t modal;
    kk_modal_mode_t modal_mode;
    kk_action_t modal_action;
    kk_config_t config;
    kk_config_t draft;
    kk_config_t pending_config;
    bool save_pending;
    bool pending_factory_reset;
    bool save_failed;
    uint32_t request_id;
    uint32_t next_request_id;
    int8_t repeat_direction;
    uint64_t repeat_next_ms;
    kk_toast_t toast;
    uint64_t toast_started_ms;
    uint64_t toast_slide_started_ms;
    uint8_t toast_interval_ms;
} kk_state_t;

void kk_model_init(kk_state_t *state);
kk_page_t kk_model_page(const kk_state_t *state);
const kk_config_t *kk_model_visible_config(const kk_state_t *state);
void kk_model_input(kk_state_t *state, kk_input_t input, uint64_t now_ms, kk_effects_t *effects);
void kk_model_tick(kk_state_t *state, uint64_t now_ms, kk_effects_t *effects);
void kk_model_save_complete(kk_state_t *state, uint32_t request_id, bool success, uint64_t now_ms);
