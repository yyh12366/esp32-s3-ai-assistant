#include "kk_render_internal.h"

#include <stdio.h>

#define MODAL_WIDTH 98
#define MODAL_HEIGHT 100
#define MODAL_CONTENT_READY 0.98f

static int modal_title(const kk_state_t *state)
{
    switch (state->modal) {
    case KK_MODAL_BRIGHTNESS: return KK_TEXT_BRIGHTNESS;
    case KK_MODAL_BEEP: return KK_TEXT_BEEP;
    case KK_MODAL_INTERVAL: return KK_TEXT_INTERVAL;
    case KK_MODAL_THEME: return KK_TEXT_THEME;
    case KK_MODAL_FACTORY_RESET: return KK_TEXT_FACTORY_RESET;
    default: return KK_TEXT_SETTINGS;
    }
}

static void draw_centered(kk_fb_t *fb, const char *text, int y, kk_draw_mode_t mode,
                          int clip_x, int clip_y, int clip_w, int clip_h)
{
    int x = (KK_DISPLAY_WIDTH - (int)kk_font_width(text)) / 2;
    kk_font_draw_clipped(fb, x, y, text, mode, clip_x, clip_y, clip_w, clip_h);
}

static void draw_modal_frame(kk_fb_t *fb, float scale, int *x, int *y, int *w, int *h)
{
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 1.0f) scale = 1.0f;
    *w = (int)(MODAL_WIDTH * scale + 0.5f);
    *h = (int)(MODAL_HEIGHT * scale + 0.5f);
    *x = (KK_DISPLAY_WIDTH - *w) / 2;
    *y = (KK_DISPLAY_HEIGHT - *h) / 2;
    if (*w < 1 || *h < 1) return;
    int radius = *w < *h ? *w / 6 : *h / 6;
    kk_fb_round_rect(fb, *x, *y, *w, *h, radius, true, DRAW_CLEAR);
    kk_fb_round_rect(fb, *x, *y, *w, *h, radius, false, DRAW_NORMAL);
    if (*w > 8 && *h > 8) {
        kk_fb_round_rect(fb, *x + 3, *y + 3, *w - 6, *h - 6,
                         radius > 2 ? radius - 2 : 1, false, DRAW_NORMAL);
    }
}

static void draw_number_progress(kk_fb_t *fb, const kk_state_t *state)
{
    char value[12];
    int progress;
    if (state->modal == KK_MODAL_BRIGHTNESS) {
        snprintf(value, sizeof(value), "%u%%", state->draft.brightness);
        progress = (state->draft.brightness - KK_BRIGHTNESS_MIN) * 100 /
                   (KK_BRIGHTNESS_MAX - KK_BRIGHTNESS_MIN);
    } else {
        snprintf(value, sizeof(value), "%ums", state->draft.interval_ms);
        progress = (state->draft.interval_ms - KK_INTERVAL_MIN_MS) * 100 /
                   (KK_INTERVAL_MAX_MS - KK_INTERVAL_MIN_MS);
    }
    draw_centered(fb, value, 68, DRAW_NORMAL, 17, 35, 86, 90);
    kk_fb_round_rect(fb, 23, 92, 74, 10, 5, false, DRAW_NORMAL);
    int fill_width = progress * 68 / 100;
    if (fill_width > 0) kk_fb_round_rect(fb, 26, 95, fill_width, 4, 2, true, DRAW_NORMAL);
    kk_fb_round_rect(fb, 23 + progress * 68 / 100, 91, 7, 12, 4, true, DRAW_NORMAL);
}

static void draw_switch(kk_fb_t *fb, const kk_anim_t *anim)
{
    int knob_x = 43 + (int)(anim->switch_x * 18.0f + 0.5f);
    kk_font_draw(fb, 22, 85, kk_render_texts[KK_TEXT_OFF], DRAW_NORMAL);
    kk_fb_round_rect(fb, 41, 82, 38, 18, 9, false, DRAW_NORMAL);
    kk_fb_round_rect(fb, knob_x, 85, 12, 12, 6, true, DRAW_NORMAL);
    kk_font_draw(fb, 84, 85, kk_render_texts[KK_TEXT_ON], DRAW_NORMAL);
}

static void draw_theme_options(kk_fb_t *fb)
{
    static const uint8_t text_ids[] = { KK_TEXT_WHITE, KK_TEXT_CYAN, KK_TEXT_AMBER };
    for (int theme = 0; theme < KK_THEME_COUNT; theme++) {
        draw_centered(fb, kk_render_texts[text_ids[theme]], 65 + theme * 16,
                      DRAW_NORMAL, 17, 35, 86, 90);
    }
}

static void draw_factory_warning(kk_fb_t *fb)
{
    draw_centered(fb, kk_render_texts[KK_TEXT_RESET_WARNING], 65,
                  DRAW_NORMAL, 17, 35, 86, 90);
    draw_centered(fb, kk_render_texts[KK_TEXT_RESET_CAUTION], 82,
                  DRAW_NORMAL, 17, 35, 86, 90);
}

static void draw_action_buttons(kk_fb_t *fb)
{
    kk_font_draw(fb, 23, 112, kk_render_texts[KK_TEXT_CANCEL], DRAW_NORMAL);
    kk_font_draw(fb, 73, 112, kk_render_texts[KK_TEXT_CONFIRM], DRAW_NORMAL);
}

static void draw_cursor(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim)
{
    bool action = state->modal_mode == KK_MODAL_ACTION;
    kk_fb_round_rect(fb, (int)(anim->cursor.x + 0.5f),
                     (int)(anim->cursor.y + 0.5f),
                     (int)(anim->cursor.w + 0.5f),
                     (int)(anim->cursor.h + 0.5f),
                     (int)(anim->cursor.r + 0.5f), action, DRAW_XOR);
}

void kk_render_modal(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim,
                    const kk_telemetry_t *telemetry)
{
    int x, y, w, h;
    (void)telemetry;
    draw_modal_frame(fb, anim->modal_scale, &x, &y, &w, &h);
    if (anim->modal_scale < MODAL_CONTENT_READY) return;

    int clip_x = x + 6;
    int clip_y = y + 5;
    int clip_w = w - 12;
    int clip_h = h - 10;
    draw_centered(fb, kk_render_texts[modal_title(state)], 38,
                  DRAW_NORMAL, clip_x, clip_y, clip_w, clip_h);

    if (state->modal == KK_MODAL_FACTORY_RESET) draw_factory_warning(fb);
    else if (state->modal == KK_MODAL_BEEP) draw_switch(fb, anim);
    else if (state->modal == KK_MODAL_THEME) draw_theme_options(fb);
    else draw_number_progress(fb, state);

    draw_action_buttons(fb);
    draw_cursor(fb, state, anim);
}
