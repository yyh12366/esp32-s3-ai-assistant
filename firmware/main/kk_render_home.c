#include "kk_render_internal.h"

static const kk_icon_t s_icons[] = {
    KK_ICON_DATA, KK_ICON_WAVE, KK_ICON_SETTINGS, KK_ICON_ABOUT,
};

static const uint8_t s_titles[] = {
    KK_TEXT_TITLE_DATA, KK_TEXT_TITLE_WAVE, KK_TEXT_TITLE_SETTINGS, KK_TEXT_TITLE_ABOUT,
};

void kk_render_home(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim)
{
    int scroll_x = (int)(anim->scroll_x + 0.5f);
    int selected_index = (int)state->carousel_index;
    for (int i = 0; i < KK_CAROUSEL_COUNT; i++) {
        int x = 17 + i * 90 - scroll_x;
        bool selected = i == selected_index;
        kk_draw_mode_t mode = selected ? DRAW_XOR : DRAW_NORMAL;
        if (selected) kk_fb_round_rect(fb, x, 48, 86, 48, 10, true, mode);
        else kk_fb_round_rect(fb, x, 48, 86, 48, 10, false, mode);
        kk_icon_draw(fb, s_icons[i], x + 27, 56, 32, mode);
    }

    int title_id = s_titles[state->carousel_index];
    int title_width = (int)kk_font_width(kk_render_texts[title_id]);
    int title_x = (KK_DISPLAY_WIDTH - title_width) / 2;
    int title_y = 113;
    kk_font_draw(fb, title_x, title_y, kk_render_texts[title_id], DRAW_NORMAL);

    int x = (int)(anim->cursor.x + 0.5f);
    int y = (int)(anim->cursor.y + 0.5f);
    int width = (int)(anim->cursor.w + 0.5f);
    int height = (int)(anim->cursor.h + 0.5f);
    kk_fb_round_rect(fb, x, y, width, height, (int)(anim->cursor.r + 0.5f), true, DRAW_XOR);

    for (int i = 0; i < KK_CAROUSEL_COUNT; i++) {
        int dot_x = 53 + i * 5;
        kk_fb_round_rect(fb, dot_x, 134, 3, 3, 1, i == selected_index, DRAW_NORMAL);
    }
}
