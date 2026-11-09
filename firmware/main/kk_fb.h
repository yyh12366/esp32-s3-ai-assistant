#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kk_anim.h"

#define KK_FB_STRIDE ((KK_DISPLAY_WIDTH + 7) / 8)
#define KK_FB_BYTES (KK_FB_STRIDE * KK_DISPLAY_HEIGHT)

typedef struct {
    uint8_t pixels[KK_FB_BYTES];
} kk_fb_t;

typedef enum {
    DRAW_NORMAL = 0,
    DRAW_CLEAR,
    DRAW_XOR,
} kk_draw_mode_t;

void kk_fb_clear(kk_fb_t *fb, bool on);
void kk_fb_point(kk_fb_t *fb, int x, int y, kk_draw_mode_t mode);
void kk_fb_line(kk_fb_t *fb, int x0, int y0, int x1, int y1, kk_draw_mode_t mode);
void kk_fb_round_rect(kk_fb_t *fb, int x, int y, int w, int h, int radius, bool filled, kk_draw_mode_t mode);
void kk_fb_bitmap(kk_fb_t *fb, int x, int y, int w, int h, const uint8_t *bitmap, size_t bitmap_size, kk_draw_mode_t mode);
