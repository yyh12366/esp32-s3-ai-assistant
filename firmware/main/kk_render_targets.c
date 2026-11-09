#include "kk_render.h"

#include <stdio.h>
#include <string.h>

#include "kk_font.h"
#include "kk_render_texts.h"

#define SETTINGS_TOP 42.0f
#define SETTINGS_ROW 19.0f
#define SETTINGS_BOTTOM 132.0f
#define TOAST_HIDDEN_OFFSET 40.0f

static const uint8_t s_setting_text[] = {
    KK_TEXT_BRIGHTNESS, KK_TEXT_BEEP, KK_TEXT_INTERVAL,
    KK_TEXT_THEME, KK_TEXT_FACTORY_RESET, KK_TEXT_BACK,
};

static float settings_cursor_width(const kk_state_t *state)
{
    uint8_t setting = state->settings_cursor;
    float label_width = kk_font_width(kk_render_texts[s_setting_text[setting]]);
    float value_width = 0.0f;
    char value[12];

    switch (setting) {
    case KK_SETTING_BRIGHTNESS:
        snprintf(value, sizeof(value), "%u%%", state->config.brightness);
        value_width = kk_font_width(value);
        break;
    case KK_SETTING_BEEP:
        value_width = kk_font_width(kk_render_texts[
            state->config.beep_enabled ? KK_TEXT_ON : KK_TEXT_OFF]);
        break;
    case KK_SETTING_INTERVAL:
        snprintf(value, sizeof(value), "%ums", state->config.interval_ms);
        value_width = kk_font_width(value);
        break;
    case KK_SETTING_THEME: {
        const char *theme = kk_render_texts[KK_TEXT_WHITE + state->config.theme];
        value_width = kk_font_width(theme);
        break;
    }
    default:
        break;
    }
    float width = label_width + value_width + 20.0f;
    if (width < 42.0f) width = 42.0f;
    if (width > 108.0f) width = 108.0f;
    return width;
}

static void set_home_target(const kk_state_t *state, kk_anim_target_t *target)
{
    static const uint8_t title_ids[] = {
        KK_TEXT_TITLE_DATA, KK_TEXT_TITLE_WAVE, KK_TEXT_TITLE_SETTINGS, KK_TEXT_TITLE_ABOUT,
    };
    uint8_t selected = (uint8_t)state->carousel_index;
    float text_width = kk_font_width(kk_render_texts[title_ids[selected]]);
    target->scroll_x = selected * 90.0f;
    target->cursor = (kk_cursor_t){
        .x = (120.0f - text_width - 10.0f) / 2.0f,
        .y = 111.0f,
        .w = text_width + 10.0f,
        .h = 19.0f,
        .r = 7.0f,
    };
}

static void set_settings_target(const kk_state_t *state, kk_anim_target_t *target)
{
    float row_y = SETTINGS_TOP + state->settings_cursor * SETTINGS_ROW;
    if (row_y + 18.0f > SETTINGS_BOTTOM) target->scroll_y = row_y + 18.0f - SETTINGS_BOTTOM;
    target->cursor = (kk_cursor_t){
        .x = 5.0f,
        .y = row_y - target->scroll_y,
        .w = settings_cursor_width(state),
        .h = 17.0f,
        .r = 6.0f,
    };
}

static void set_modal_target(const kk_state_t *state, kk_anim_target_t *target)
{
    if (state->modal_mode == KK_MODAL_ACTION) {
        bool confirm = state->modal_action == KK_ACTION_CONFIRM;
        target->cursor = (kk_cursor_t){
            .x = confirm ? 66.0f : 17.0f,
            .y = 109.0f,
            .w = 37.0f,
            .h = 17.0f,
            .r = 5.0f,
        };
        return;
    }
    if (state->modal == KK_MODAL_BEEP) {
        target->cursor = (kk_cursor_t){ .x = 39.0f, .y = 82.0f, .w = 42.0f, .h = 20.0f, .r = 8.0f };
    } else if (state->modal == KK_MODAL_THEME) {
        int text_id = KK_TEXT_WHITE + state->draft.theme;
        float width = kk_font_width(kk_render_texts[text_id]) + 10.0f;
        target->cursor = (kk_cursor_t){
            .x = (120.0f - width) / 2.0f,
            .y = 64.0f + state->draft.theme * 16.0f,
            .w = width,
            .h = 15.0f,
            .r = 5.0f,
        };
    } else {
        target->cursor = (kk_cursor_t){ .x = 19.0f, .y = 92.0f, .w = 82.0f, .h = 18.0f, .r = 7.0f };
    }
}

void kk_render_anim_target(const kk_state_t *state, uint64_t now_ms,
                           kk_anim_target_t *target)
{
    memset(target, 0, sizeof(*target));
    (void)now_ms;
    kk_page_t page = kk_model_page(state);
    const kk_config_t *config = kk_model_visible_config(state);
    target->switch_x = config->beep_enabled ? 1.0f : 0.0f;
    target->modal_scale = page == KK_PAGE_MODAL ? 1.0f : 0.0f;
    target->toast_y = state->toast != KK_TOAST_NONE &&
                      state->toast_slide_started_ms == UINT64_MAX ?
                      0.0f : TOAST_HIDDEN_OFFSET;

    if (page == KK_PAGE_HOME) set_home_target(state, target);
    else if (page == KK_PAGE_SETTINGS_LIST) set_settings_target(state, target);
    else if (page == KK_PAGE_MODAL) set_modal_target(state, target);
}

void kk_render_anim_reset_modal(kk_anim_t *anim)
{
    anim->modal_scale = 0.0f;
}
