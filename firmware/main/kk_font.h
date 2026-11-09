#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kk_fb.h"

#define KK_FONT_LINE_HEIGHT 14

int kk_font_draw(kk_fb_t *fb, int x, int y, const char *text, kk_draw_mode_t mode);
int kk_font_draw_clipped(kk_fb_t *fb, int x, int y, const char *text, kk_draw_mode_t mode,
                         int clip_x, int clip_y, int clip_w, int clip_h);
float kk_font_measure(uint32_t codepoint, void *context);
float kk_font_width(const char *text);
bool kk_font_has(uint32_t codepoint);
