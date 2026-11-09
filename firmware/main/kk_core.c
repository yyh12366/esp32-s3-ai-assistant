#include "kk_core.h"

#include <string.h>

#define KK_REPEAT_DELAY_MS 400U
#define KK_REPEAT_PERIOD_MS 90U
#define KK_TOAST_HOLD_MS 1200U
#define KK_TOAST_SETTLE_FRAMES 16U

static kk_config_t default_config(void)
{
    return (kk_config_t){
        .brightness = 80,
        .interval_ms = 25,
        .beep_enabled = true,
        .theme = KK_THEME_WHITE,
    };
}

static bool push_page(kk_state_t *state, kk_page_t page)
{
    if (state->stack_depth >= KK_PAGE_STACK_MAX) return false;
    state->stack[state->stack_depth++] = page;
    return true;
}

static void pop_page(kk_state_t *state)
{
    if (state->stack_depth > 1) state->stack_depth--;
}

static void set_effect(kk_effects_t *effects, uint8_t flag, const kk_config_t *config)
{
    if (!effects) return;
    effects->flags |= flag;
    if (config) effects->config = *config;
}

static void toast_start(kk_state_t *state, kk_toast_t toast, uint64_t now_ms)
{
    state->toast = toast;
    state->toast_started_ms = now_ms;
    state->toast_slide_started_ms = UINT64_MAX;
    state->toast_interval_ms = kk_model_visible_config(state)->interval_ms;
}

static void close_modal(kk_state_t *state, kk_effects_t *effects)
{
    if (state->modal == KK_MODAL_BRIGHTNESS && state->draft.brightness != state->config.brightness) {
        set_effect(effects, KK_EFFECT_APPLY_BRIGHTNESS, &state->config);
    }
    state->draft = state->config;
    state->modal = KK_MODAL_NONE;
    state->modal_mode = KK_MODAL_EDIT;
    state->save_failed = false;
    state->repeat_direction = 0;
    pop_page(state);
}

static bool open_modal(kk_state_t *state, kk_setting_t setting)
{
    static const kk_modal_t modals[] = {
        KK_MODAL_BRIGHTNESS,
        KK_MODAL_BEEP,
        KK_MODAL_INTERVAL,
        KK_MODAL_THEME,
        KK_MODAL_FACTORY_RESET,
    };
    if (setting == KK_SETTING_BACK || setting > KK_SETTING_FACTORY_RESET) return false;
    if (!push_page(state, KK_PAGE_MODAL)) return false;
    state->modal = modals[setting];
    state->modal_mode = setting == KK_SETTING_FACTORY_RESET ? KK_MODAL_ACTION : KK_MODAL_EDIT;
    state->modal_action = setting == KK_SETTING_FACTORY_RESET ? KK_ACTION_CANCEL : KK_ACTION_CONFIRM;
    state->draft = state->config;
    state->save_failed = false;
    state->repeat_direction = 0;
    return true;
}

static void begin_save(kk_state_t *state, bool reset, kk_effects_t *effects)
{
    state->pending_factory_reset = reset;
    state->pending_config = reset ? default_config() : state->draft;
    state->save_pending = true;
    state->save_failed = false;
    state->request_id = state->next_request_id++;
    if (state->next_request_id == 0) state->next_request_id = 1;
    if (state->request_id == 0) state->request_id = state->next_request_id++;
    if (effects) {
        effects->request_id = state->request_id;
        effects->config = state->pending_config;
        effects->flags |= reset ? KK_EFFECT_RESET_CONFIG : KK_EFFECT_SAVE_CONFIG;
    }
}

static void step_number(kk_state_t *state, int direction, kk_effects_t *effects)
{
    uint8_t old_brightness = state->draft.brightness;
    if (state->modal == KK_MODAL_BRIGHTNESS) {
        int value = state->draft.brightness + direction * 5;
        if (value < KK_BRIGHTNESS_MIN) value = KK_BRIGHTNESS_MIN;
        if (value > KK_BRIGHTNESS_MAX) value = KK_BRIGHTNESS_MAX;
        state->draft.brightness = (uint8_t)value;
    } else if (state->modal == KK_MODAL_INTERVAL) {
        int value = state->draft.interval_ms + direction;
        if (value < KK_INTERVAL_MIN_MS) value = KK_INTERVAL_MIN_MS;
        if (value > KK_INTERVAL_MAX_MS) value = KK_INTERVAL_MAX_MS;
        state->draft.interval_ms = (uint8_t)value;
    }
    if (state->draft.brightness != old_brightness) {
        set_effect(effects, KK_EFFECT_APPLY_BRIGHTNESS, &state->draft);
    }
}

static bool is_numeric_modal(const kk_state_t *state)
{
    return state->modal == KK_MODAL_BRIGHTNESS || state->modal == KK_MODAL_INTERVAL;
}

static void step_direction(kk_state_t *state, int direction, kk_effects_t *effects)
{
    switch (kk_model_page(state)) {
    case KK_PAGE_HOME:
        state->carousel_index = (kk_carousel_t)((state->carousel_index +
                (direction > 0 ? 1 : KK_CAROUSEL_COUNT - 1)) % KK_CAROUSEL_COUNT);
        break;
    case KK_PAGE_WAVE: {
        int source = (int)state->wave_source + direction;
        if (source < 0) source = 0;
        if (source >= KK_WAVE_SOURCE_COUNT) source = KK_WAVE_SOURCE_COUNT - 1;
        state->wave_source = (kk_wave_source_t)source;
        break;
    }
    case KK_PAGE_SETTINGS_LIST: {
        int cursor = (int)state->settings_cursor + direction;
        if (cursor < 0) cursor = 0;
        if (cursor >= KK_SETTINGS_COUNT) cursor = KK_SETTINGS_COUNT - 1;
        state->settings_cursor = (uint8_t)cursor;
        break;
    }
    case KK_PAGE_MODAL:
        if (state->modal_mode == KK_MODAL_ACTION) {
            state->modal_action = state->modal_action == KK_ACTION_CONFIRM ? KK_ACTION_CANCEL : KK_ACTION_CONFIRM;
        } else if (is_numeric_modal(state)) {
            step_number(state, -direction, effects);
        } else if (state->modal == KK_MODAL_BEEP) {
            state->draft.beep_enabled = !state->draft.beep_enabled;
        } else if (state->modal == KK_MODAL_THEME) {
            int theme = ((int)state->draft.theme + direction + KK_THEME_COUNT) % KK_THEME_COUNT;
            state->draft.theme = (kk_theme_t)theme;
        }
        break;
    default:
        break;
    }
}

void kk_model_init(kk_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->stack[0] = KK_PAGE_HOME;
    state->stack_depth = 1;
    state->carousel_index = KK_CAROUSEL_DATA;
    state->wave_source = KK_WAVE_MIC;
    state->config = default_config();
    state->draft = state->config;
    state->pending_config = state->config;
    state->next_request_id = 1;
}

kk_page_t kk_model_page(const kk_state_t *state)
{
    return state->stack_depth ? state->stack[state->stack_depth - 1] : KK_PAGE_HOME;
}

const kk_config_t *kk_model_visible_config(const kk_state_t *state)
{
    return kk_model_page(state) == KK_PAGE_MODAL ? &state->draft : &state->config;
}

void kk_model_input(kk_state_t *state, kk_input_t input, uint64_t now_ms, kk_effects_t *effects)
{
    if (effects) memset(effects, 0, sizeof(*effects));
    if (input.event == KK_KEY_EVENT_RELEASE) {
        if ((input.key == KK_KEY_UP && state->repeat_direction < 0) ||
            (input.key == KK_KEY_DOWN && state->repeat_direction > 0)) state->repeat_direction = 0;
        return;
    }
    if (state->save_pending) return;
    if (input.key == KK_KEY_UP || input.key == KK_KEY_DOWN) {
        int direction = input.key == KK_KEY_UP ? -1 : 1;
        if (input.event == KK_KEY_EVENT_LONG) {
            if (kk_model_page(state) == KK_PAGE_MODAL && state->modal_mode == KK_MODAL_EDIT && is_numeric_modal(state)) {
                step_direction(state, direction, effects);
                state->repeat_direction = (int8_t)direction;
                state->repeat_next_ms = now_ms + KK_REPEAT_DELAY_MS;
            }
        } else if (input.event == KK_KEY_EVENT_CLICK || input.event == KK_KEY_EVENT_DOUBLE) {
            int count = input.event == KK_KEY_EVENT_DOUBLE ? 2 : 1;
            for (int i = 0; i < count; i++) step_direction(state, direction, effects);
        }
        return;
    }
    if (input.key != KK_KEY_OK) return;
    if (input.event == KK_KEY_EVENT_LONG || input.event == KK_KEY_EVENT_DOUBLE) {
        if (kk_model_page(state) == KK_PAGE_MODAL) {
            close_modal(state, effects);
        } else if (kk_model_page(state) == KK_PAGE_HOME) {
            // 首页没有上一级：长按 OK 请求离开监测，回到合并固件的首页
            if (effects && input.event == KK_KEY_EVENT_LONG) effects->flags |= KK_EFFECT_EXIT_SHELL;
        } else {
            pop_page(state);
        }
        return;
    }
    if (input.event != KK_KEY_EVENT_CLICK) return;
    switch (kk_model_page(state)) {
    case KK_PAGE_HOME: {
        static const kk_page_t pages[] = {
            KK_PAGE_DATA, KK_PAGE_WAVE, KK_PAGE_SETTINGS_LIST, KK_PAGE_ABOUT,
        };
        push_page(state, pages[state->carousel_index]);
        break;
    }
    case KK_PAGE_SETTINGS_LIST:
        if (state->settings_cursor == KK_SETTING_BACK) pop_page(state);
        else open_modal(state, (kk_setting_t)state->settings_cursor);
        break;
    case KK_PAGE_MODAL:
        if (state->modal_mode == KK_MODAL_EDIT) {
            state->modal_mode = KK_MODAL_ACTION;
            state->modal_action = KK_ACTION_CONFIRM;
            state->repeat_direction = 0;
        } else if (state->modal_action == KK_ACTION_CANCEL) {
            close_modal(state, effects);
        } else {
            begin_save(state, state->modal == KK_MODAL_FACTORY_RESET, effects);
        }
        break;
    default:
        break;
    }
}

void kk_model_tick(kk_state_t *state, uint64_t now_ms, kk_effects_t *effects)
{
    if (effects) memset(effects, 0, sizeof(*effects));
    if (state->repeat_direction && !state->save_pending &&
        kk_model_page(state) == KK_PAGE_MODAL && state->modal_mode == KK_MODAL_EDIT && is_numeric_modal(state) &&
        now_ms >= state->repeat_next_ms) {
        step_direction(state, state->repeat_direction, effects);
        state->repeat_next_ms = now_ms + KK_REPEAT_PERIOD_MS;
    }
    if (state->toast == KK_TOAST_NONE) return;
    if (state->toast_slide_started_ms == UINT64_MAX) {
        uint64_t settle_ms = (uint64_t)state->toast_interval_ms * KK_TOAST_SETTLE_FRAMES;
        if (now_ms - state->toast_started_ms >= settle_ms + KK_TOAST_HOLD_MS) {
            state->toast_slide_started_ms = now_ms;
        }
    } else if (now_ms - state->toast_slide_started_ms >=
               (uint64_t)state->toast_interval_ms * KK_TOAST_SETTLE_FRAMES) {
        state->toast = KK_TOAST_NONE;
        state->toast_slide_started_ms = 0;
    }
}

void kk_model_save_complete(kk_state_t *state, uint32_t request_id, bool success, uint64_t now_ms)
{
    if (!state->save_pending || state->request_id != request_id) return;
    state->save_pending = false;
    state->repeat_direction = 0;
    if (!success) {
        state->save_failed = true;
        toast_start(state, KK_TOAST_SAVE_FAILED, now_ms);
        return;
    }
    state->config = state->pending_config;
    state->draft = state->config;
    state->save_failed = false;
    state->modal = KK_MODAL_NONE;
    pop_page(state);
    toast_start(state, state->pending_factory_reset ? KK_TOAST_RESET : KK_TOAST_SAVED, now_ms);
    state->pending_factory_reset = false;
}
