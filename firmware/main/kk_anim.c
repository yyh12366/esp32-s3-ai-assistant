#include "kk_anim.h"

#include <stdbool.h>
#include <string.h>

#define KK_ANIM_SPEED 0.25f

float kk_anim_blend(float dt_ms)
{
    if (dt_ms <= 0.0f) return 0.0f;
    return KK_ANIM_SPEED;
}

static float approach(float value, float target, float blend)
{
    return value + (target - value) * blend;
}

void kk_anim_init(kk_anim_t *anim, const kk_anim_target_t *initial)
{
    memset(anim, 0, sizeof(*anim));
    if (initial) {
        anim->cursor = initial->cursor;
        anim->scroll_x = initial->scroll_x;
        anim->scroll_y = initial->scroll_y;
        anim->toast_y = initial->toast_y;
        anim->modal_scale = initial->modal_scale;
        anim->switch_x = initial->switch_x;
    }
}

void kk_anim_step(kk_anim_t *anim, const kk_anim_target_t *target, float dt_ms)
{
    float cursor_blend = kk_anim_blend(dt_ms);
    float scroll_factor = cursor_blend;
    anim->cursor.x = approach(anim->cursor.x, target->cursor.x, cursor_blend);
    anim->cursor.y = approach(anim->cursor.y, target->cursor.y, cursor_blend);
    anim->cursor.w = approach(anim->cursor.w, target->cursor.w, cursor_blend);
    anim->cursor.h = approach(anim->cursor.h, target->cursor.h, cursor_blend);
    anim->cursor.r = approach(anim->cursor.r, target->cursor.r, cursor_blend);
    anim->scroll_x = approach(anim->scroll_x, target->scroll_x, scroll_factor);
    anim->scroll_y = approach(anim->scroll_y, target->scroll_y, scroll_factor);
    anim->toast_y = approach(anim->toast_y, target->toast_y, cursor_blend);
    anim->modal_scale = approach(anim->modal_scale, target->modal_scale, cursor_blend);
    anim->switch_x = approach(anim->switch_x, target->switch_x, cursor_blend);
}

static bool continuation(unsigned char byte)
{
    return byte >= 0x80 && byte <= 0xBF;
}

static uint32_t next_codepoint(const unsigned char **text)
{
    const unsigned char *s = *text;
    uint32_t cp;
    size_t length;
    uint32_t minimum;
    if (s[0] < 0x80) {
        *text = s + 1;
        return s[0];
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) {
        cp = s[0] & 0x1F; length = 2; minimum = 0x80;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        cp = s[0] & 0x0F; length = 3; minimum = 0x800;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        cp = s[0] & 0x07; length = 4; minimum = 0x10000;
    } else {
        *text = s + 1;
        return 0xFFFD;
    }
    for (size_t i = 1; i < length; i++) {
        if (s[i] == 0 || !continuation(s[i])) {
            *text = s + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *text = s + 1;
        return 0xFFFD;
    }
    *text = s + length;
    return cp;
}

float kk_measure_utf8(const char *text, kk_glyph_measure_fn measure, void *context)
{
    if (!text || !measure) return 0.0f;
    float width = 0.0f;
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) width += measure(next_codepoint(&cursor), context);
    return width;
}
