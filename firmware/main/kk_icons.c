#include "kk_icons.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ICON_MAX_SIZE 32
#define ICON_STRIDE ((ICON_MAX_SIZE + 7) / 8)

typedef struct {
    uint8_t bits[ICON_MAX_SIZE * ICON_STRIDE];
    int size;
} icon_mask_t;

static void mask_point(icon_mask_t *mask, int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= mask->size || y >= mask->size) return;
    uint8_t *byte = &mask->bits[y * ICON_STRIDE + x / 8];
    uint8_t bit = 0x80 >> (x & 7);
    if (on) *byte |= bit;
    else *byte &= (uint8_t)~bit;
}

static bool mask_get(const icon_mask_t *mask, int x, int y)
{
    if (x < 0 || y < 0 || x >= mask->size || y >= mask->size) return false;
    return (mask->bits[y * ICON_STRIDE + x / 8] & (0x80 >> (x & 7))) != 0;
}

static bool inside_round_rect(int x, int y, int w, int h, int radius)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    if (radius <= 0 || (x >= radius && x < w - radius) || (y >= radius && y < h - radius)) return true;
    int cx = x < radius ? radius - 1 : w - radius;
    int cy = y < radius ? radius - 1 : h - radius;
    int dx = x - cx;
    int dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static void mask_round_rect(icon_mask_t *mask, int x, int y, int w, int h,
                            int radius, bool filled)
{
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            if (!inside_round_rect(px, py, w, h, radius)) continue;
            if (filled || !inside_round_rect(px - 1, py - 1, w - 2, h - 2, radius - 1) ||
                          !inside_round_rect(px + 1, py + 1, w - 2, h - 2, radius - 1)) {
                mask_point(mask, x + px, y + py, true);
            }
        }
    }
}

static void mask_line(icon_mask_t *mask, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        mask_point(mask, x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static void draw_data(icon_mask_t *mask)
{
    const int size = mask->size;
    const int width = size / 6;
    const int heights[] = { size / 3, size / 2, size * 2 / 3, size - 5 };
    for (int i = 0; i < 4; i++) {
        int x = 1 + i * size / 4;
        int y = size - 2 - heights[i];
        mask_round_rect(mask, x, y, width, heights[i], 2, true);
    }
}

static void draw_wave(icon_mask_t *mask)
{
    static const int8_t sample[] = { 0, -2, -4, -1, 4, 1, -4, -2, 0, 2, 4, 1, -4, -1, 3, 2, 0 };
    int size = mask->size;
    for (int i = 0; i < 16; i++) {
        int x0 = i * (size - 1) / 16;
        int x1 = (i + 1) * (size - 1) / 16;
        int center = size / 2;
        int y0 = center + sample[i] * size / 16;
        int y1 = center + sample[i + 1] * size / 16;
        mask_line(mask, x0, y0, x1, y1);
    }
}

static void draw_settings(icon_mask_t *mask)
{
    int size = mask->size;
    int tooth = size / 5;
    int inner = size / 3;
    mask_round_rect(mask, size / 3, 0, tooth, tooth, 2, true);
    mask_round_rect(mask, size / 3, size - tooth, tooth, tooth, 2, true);
    mask_round_rect(mask, 0, size / 3, tooth, tooth, 2, true);
    mask_round_rect(mask, size - tooth, size / 3, tooth, tooth, 2, true);
    mask_round_rect(mask, tooth, tooth, size - tooth * 2, size - tooth * 2,
                    size / 4, true);
    int left = (size - inner) / 2;
    int top = (size - inner) / 2;
    mask_round_rect(mask, left, top, inner, inner, inner / 3, false);
    for (int y = top + 2; y < top + inner - 2; y++) {
        for (int x = left + 2; x < left + inner - 2; x++) mask_point(mask, x, y, false);
    }
}

static void draw_about(icon_mask_t *mask)
{
    int size = mask->size;
    int inset = size / 8;
    mask_round_rect(mask, inset, inset, size - inset * 2, size - inset * 2,
                    size / 4, false);
    mask_round_rect(mask, size / 2 - 1, size / 4, 3, 3, 1, true);
    mask_round_rect(mask, size / 2 - 1, size / 2, 3, size / 3, 1, true);
}

void kk_icon_draw(kk_fb_t *fb, kk_icon_t icon, int x, int y, int size, kk_draw_mode_t mode)
{
    icon_mask_t mask = { .size = size };
    if (size > ICON_MAX_SIZE) return;
    switch (icon) {
    case KK_ICON_DATA: draw_data(&mask); break;
    case KK_ICON_WAVE: draw_wave(&mask); break;
    case KK_ICON_SETTINGS: draw_settings(&mask); break;
    case KK_ICON_ABOUT: draw_about(&mask); break;
    }

    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            if (mask_get(&mask, px, py)) kk_fb_point(fb, x + px, y + py, mode);
        }
    }
}
