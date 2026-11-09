#include "kk_render_internal.h"

#include <stdio.h>

static kk_page_t background_page(const kk_state_t *state)
{
    if (kk_model_page(state) != KK_PAGE_MODAL || state->stack_depth < 2) return kk_model_page(state);
    return state->stack[state->stack_depth - 2];
}

static int page_title(kk_page_t page)
{
    switch (page) {
    case KK_PAGE_DATA: return KK_TEXT_TITLE_DATA;
    case KK_PAGE_WAVE: return KK_TEXT_TITLE_WAVE;
    case KK_PAGE_SETTINGS_LIST: return KK_TEXT_TITLE_SETTINGS;
    case KK_PAGE_ABOUT: return KK_TEXT_TITLE_ABOUT;
    default: return -1;
    }
}

static void draw_battery(kk_fb_t *fb, const kk_telemetry_t *telemetry)
{
    kk_fb_round_rect(fb, 92, 4, 16, 9, 2, false, DRAW_NORMAL);
    kk_fb_round_rect(fb, 108, 6, 3, 5, 1, true, DRAW_NORMAL);
    if (telemetry->battery_soc >= 0) {
        int soc = telemetry->battery_soc > 100 ? 100 : telemetry->battery_soc;
        int fill_width = 12 * soc / 100;
        if (fill_width > 0) kk_fb_round_rect(fb, 94, 6, fill_width, 5, 1, true, DRAW_NORMAL);
        char value[8];
        snprintf(value, sizeof(value), "%d%%", (int)telemetry->battery_soc);
        kk_font_draw(fb, 56, 3, value, DRAW_NORMAL);
    }
}

void kk_render_header(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim,
                      const kk_telemetry_t *telemetry)
{
    kk_page_t page = background_page(state);
    int title_id = page_title(page);
    kk_font_draw(fb, 7, 3, "KK", DRAW_NORMAL);
    draw_battery(fb, telemetry);
    kk_fb_line(fb, 7, 14, 112, 14, DRAW_NORMAL);

    if (page == KK_PAGE_HOME) kk_font_draw(fb, 7, 18, "KK OLED", DRAW_NORMAL);
    else if (page == KK_PAGE_WAVE) {
        kk_fb_round_rect(fb, 6, 17, 38, 19, 6, true, DRAW_XOR);
        kk_font_draw(fb, 13, 19, kk_render_texts[KK_TEXT_WAVE], DRAW_CLEAR);
    }
    else if (page == KK_PAGE_SETTINGS_LIST && !telemetry->storage_available) {
        kk_font_draw(fb, 7, 18, kk_render_texts[KK_TEXT_SETTINGS], DRAW_NORMAL);
        kk_font_draw_clipped(fb, 40, 18, kk_render_texts[KK_TEXT_NVS_ERROR], DRAW_NORMAL,
                             40, 16, 72, KK_FONT_LINE_HEIGHT);
        kk_font_draw(fb, 2, 29, kk_render_texts[KK_TEXT_SETTINGS_WORD], DRAW_NORMAL);
        kk_font_draw(fb, 70, 29, kk_render_texts[KK_TEXT_LOCKED], DRAW_NORMAL);
    } else if (title_id >= 0) {
        int title_width = (int)kk_font_width(kk_render_texts[title_id]);
        kk_font_draw(fb, (KK_DISPLAY_WIDTH - title_width) / 2, 18,
                     kk_render_texts[title_id], DRAW_NORMAL);
    }

    if (page != KK_PAGE_HOME && !(page == KK_PAGE_SETTINGS_LIST && !telemetry->storage_available)) {
        kk_fb_line(fb, 7, 37, 112, 37, DRAW_NORMAL);
    }
    (void)anim;
}

void kk_render_key_icon(kk_fb_t *fb, int x, int y, int key, bool pressed)
{
    kk_draw_mode_t ink = pressed ? DRAW_CLEAR : DRAW_NORMAL;
    kk_fb_round_rect(fb, x, y, 20, 12, 3, pressed, DRAW_NORMAL);
    kk_fb_round_rect(fb, x, y, 20, 12, 3, false, DRAW_NORMAL);
    if (key == KK_KEY_UP) {
        kk_fb_line(fb, x + 6, y + 8, x + 10, y + 3, ink);
        kk_fb_line(fb, x + 10, y + 3, x + 14, y + 8, ink);
        kk_fb_line(fb, x + 10, y + 3, x + 10, y + 9, ink);
    } else if (key == KK_KEY_DOWN) {
        kk_fb_line(fb, x + 6, y + 4, x + 10, y + 9, ink);
        kk_fb_line(fb, x + 10, y + 9, x + 14, y + 4, ink);
        kk_fb_line(fb, x + 10, y + 3, x + 10, y + 9, ink);
    } else {
        kk_font_draw(fb, x + 2, y + 1, "OK", ink);
    }
}

void kk_render_footer(kk_fb_t *fb, const kk_telemetry_t *telemetry)
{
    kk_fb_line(fb, 7, 141, 112, 141, DRAW_NORMAL);
    kk_render_key_icon(fb, 8, 145, KK_KEY_UP,
                       (telemetry->key_pressed_mask & KK_KEY_MASK_UP) != 0);
    kk_render_key_icon(fb, 37, 145, KK_KEY_DOWN,
                       (telemetry->key_pressed_mask & KK_KEY_MASK_DOWN) != 0);
    kk_render_key_icon(fb, 66, 145, KK_KEY_OK,
                       (telemetry->key_pressed_mask & KK_KEY_MASK_OK) != 0);
}

static void draw_page(kk_fb_t *fb, kk_page_t page, const kk_state_t *state,
                      const kk_anim_t *anim, const kk_telemetry_t *telemetry)
{
    switch (page) {
    case KK_PAGE_HOME: kk_render_home(fb, state, anim); break;
    case KK_PAGE_DATA: kk_render_data(fb, telemetry); break;
    case KK_PAGE_WAVE: kk_render_wave(fb, state, telemetry); break;
    case KK_PAGE_SETTINGS_LIST: kk_render_settings(fb, state, anim); break;
    case KK_PAGE_ABOUT: kk_render_about(fb, telemetry); break;
    case KK_PAGE_MODAL: break;
    }
}

void kk_render_toast(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim,
                     uint64_t now_ms)
{
    (void)now_ms;
    if (state->toast == KK_TOAST_NONE) return;
    int y = 121 + (int)(anim->toast_y + 0.5f);
    int text_id = state->toast == KK_TOAST_SAVE_FAILED ? KK_TEXT_SAVE_FAILED :
                  state->toast == KK_TOAST_RESET ? KK_TEXT_RESET_COMPLETE : KK_TEXT_SAVED;
    const char *text = kk_render_texts[text_id];
    int width = (int)kk_font_width(text);
    kk_fb_round_rect(fb, 9, y, 102, 18, 6, true, DRAW_CLEAR);
    kk_fb_round_rect(fb, 9, y, 102, 18, 6, true, DRAW_XOR);
    kk_fb_round_rect(fb, 9, y, 102, 18, 6, false, DRAW_NORMAL);
    kk_font_draw_clipped(fb, (KK_DISPLAY_WIDTH - width) / 2, y + 2, text, DRAW_CLEAR,
                         14, y + 1, 92, 16);
}

void kk_render_frame(const kk_state_t *state, const kk_anim_t *anim,
                     const kk_telemetry_t *telemetry, kk_fb_t *fb, uint64_t now_ms)
{
    kk_fb_clear(fb, false);
    kk_fb_round_rect(fb, 1, 1, 118, 158, 8, false, DRAW_NORMAL);
    kk_page_t page = kk_model_page(state);
    kk_page_t background = background_page(state);
    draw_page(fb, background, state, anim, telemetry);
    kk_render_header(fb, state, anim, telemetry);
    kk_render_footer(fb, telemetry);
    if (page == KK_PAGE_MODAL) kk_render_modal(fb, state, anim, telemetry);
    kk_render_toast(fb, state, anim, now_ms);
}
