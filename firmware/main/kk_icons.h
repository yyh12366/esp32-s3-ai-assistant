#pragma once

#include "kk_fb.h"

typedef enum {
    KK_ICON_DATA = 0,
    KK_ICON_WAVE,
    KK_ICON_SETTINGS,
    KK_ICON_ABOUT,
} kk_icon_t;

void kk_icon_draw(kk_fb_t *fb, kk_icon_t icon, int x, int y, int size, kk_draw_mode_t mode);
