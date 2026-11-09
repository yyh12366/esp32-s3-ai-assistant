#include "kk_render_internal.h"

#include <stdio.h>

#define SETTINGS_TOP 42
#define SETTINGS_ROW 19

static const uint8_t s_setting_text[] = {
    KK_TEXT_BRIGHTNESS, KK_TEXT_BEEP, KK_TEXT_INTERVAL,
    KK_TEXT_THEME, KK_TEXT_FACTORY_RESET, KK_TEXT_BACK,
};

static void setting_value(char *buffer, size_t size, uint8_t setting, const kk_config_t *config)
{
    switch (setting) {
    case KK_SETTING_BRIGHTNESS:
        snprintf(buffer, size, "%u%%", config->brightness);
        break;
    case KK_SETTING_BEEP:
        snprintf(buffer, size, "%s", kk_render_texts[config->beep_enabled ? KK_TEXT_ON : KK_TEXT_OFF]);
        break;
    case KK_SETTING_INTERVAL:
        snprintf(buffer, size, "%ums", config->interval_ms);
        break;
    case KK_SETTING_THEME:
        snprintf(buffer, size, "%s", kk_render_texts[KK_TEXT_WHITE + config->theme]);
        break;
    default:
        buffer[0] = '\0';
        break;
    }
}

void kk_render_settings(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim)
{
    char value[16];
    const kk_config_t *config = &state->config;
    for (int setting = 0; setting < KK_SETTINGS_COUNT; setting++) {
        int y = SETTINGS_TOP + setting * SETTINGS_ROW - (int)(anim->scroll_y + 0.5f);
        setting_value(value, sizeof(value), (uint8_t)setting, config);
        int label_id = s_setting_text[setting];
        int label_width = (int)kk_font_width(kk_render_texts[label_id]);
        kk_font_draw_clipped(fb, 11, y + 2, kk_render_texts[label_id], DRAW_NORMAL,
                             8, SETTINGS_TOP, 104, 90);
        if (value[0]) {
            kk_font_draw_clipped(fb, 11 + label_width + 8, y + 2, value, DRAW_NORMAL,
                                 8, SETTINGS_TOP, 104, 90);
        }
    }

    if (kk_model_page(state) != KK_PAGE_MODAL) {
        kk_fb_round_rect(fb, (int)(anim->cursor.x + 0.5f),
                         (int)(anim->cursor.y + 0.5f),
                         (int)(anim->cursor.w + 0.5f),
                         (int)(anim->cursor.h + 0.5f),
                         (int)(anim->cursor.r + 0.5f), true, DRAW_XOR);
    }
}
