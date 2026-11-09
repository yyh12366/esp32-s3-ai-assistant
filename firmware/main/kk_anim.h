#pragma once

#include <stdint.h>

#define KK_DISPLAY_WIDTH 120
#define KK_DISPLAY_HEIGHT 160

typedef struct {
    float x;
    float y;
    float w;
    float h;
    float r;
} kk_cursor_t;

typedef struct {
    kk_cursor_t cursor;
    float scroll_x;
    float scroll_y;
    float toast_y;
    float modal_scale;
    float switch_x;
} kk_anim_t;

typedef struct {
    kk_cursor_t cursor;
    float scroll_x;
    float scroll_y;
    float toast_y;
    float modal_scale;
    float switch_x;
} kk_anim_target_t;

typedef float (*kk_glyph_measure_fn)(uint32_t codepoint, void *context);

void kk_anim_init(kk_anim_t *anim, const kk_anim_target_t *initial);
void kk_anim_step(kk_anim_t *anim, const kk_anim_target_t *target, float dt_ms);
float kk_anim_blend(float dt_ms);
float kk_measure_utf8(const char *text, kk_glyph_measure_fn measure, void *context);
