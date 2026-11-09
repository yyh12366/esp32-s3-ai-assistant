#include "kk_fb.h"

#include <stdbool.h>
#include <string.h>

_Static_assert(sizeof(((kk_fb_t *)0)->pixels) == 2400, "KK framebuffer must be exactly 2400 bytes");

static void apply_pixel(kk_fb_t *fb, int x, int y, kk_draw_mode_t mode)
{
    if (!fb || x < 0 || x >= KK_DISPLAY_WIDTH || y < 0 || y >= KK_DISPLAY_HEIGHT) return;
    size_t offset = (size_t)y * KK_FB_STRIDE + (size_t)x / 8;
    uint8_t bit = (uint8_t)(0x80U >> (x & 7));
    if (mode == DRAW_NORMAL) fb->pixels[offset] |= bit;
    else if (mode == DRAW_CLEAR) fb->pixels[offset] &= (uint8_t)~bit;
    else if (mode == DRAW_XOR) fb->pixels[offset] ^= bit;
}

void kk_fb_clear(kk_fb_t *fb, bool on)
{
    memset(fb->pixels, on ? 0xFF : 0x00, KK_FB_BYTES);
}

void kk_fb_point(kk_fb_t *fb, int x, int y, kk_draw_mode_t mode)
{
    apply_pixel(fb, x, y, mode);
}

static bool clip_line(double *x0, double *y0, double *x1, double *y1)
{
    double dx = *x1 - *x0;
    double dy = *y1 - *y0;
    double p[4] = { -dx, dx, -dy, dy };
    double q[4] = { *x0, (KK_DISPLAY_WIDTH - 1) - *x0,
                    *y0, (KK_DISPLAY_HEIGHT - 1) - *y0 };
    double first = 0.0;
    double last = 1.0;
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return false;
            continue;
        }
        double t = q[i] / p[i];
        if (p[i] < 0.0) {
            if (t > last) return false;
            if (t > first) first = t;
        } else {
            if (t < first) return false;
            if (t < last) last = t;
        }
    }
    double start_x = *x0;
    double start_y = *y0;
    *x1 = start_x + last * dx;
    *y1 = start_y + last * dy;
    *x0 = start_x + first * dx;
    *y0 = start_y + first * dy;
    return true;
}

static void raster_line(kk_fb_t *fb, int x, int y, int end_x, int end_y, kk_draw_mode_t mode)
{
    int dx = end_x > x ? end_x - x : x - end_x;
    int sx = x < end_x ? 1 : -1;
    int dy = end_y > y ? y - end_y : end_y - y;
    int sy = y < end_y ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        apply_pixel(fb, x, y, mode);
        if (x == end_x && y == end_y) break;
        int twice_error = 2 * error;
        if (twice_error >= dy) { error += dy; x += sx; }
        if (twice_error <= dx) { error += dx; y += sy; }
    }
}

void kk_fb_line(kk_fb_t *fb, int x0, int y0, int x1, int y1, kk_draw_mode_t mode)
{
    if (!fb) return;
    if (x0 >= 0 && x0 < KK_DISPLAY_WIDTH && x1 >= 0 && x1 < KK_DISPLAY_WIDTH &&
        y0 >= 0 && y0 < KK_DISPLAY_HEIGHT && y1 >= 0 && y1 < KK_DISPLAY_HEIGHT) {
        raster_line(fb, x0, y0, x1, y1, mode);
        return;
    }
    double ax = x0, ay = y0, bx = x1, by = y1;
    if (!clip_line(&ax, &ay, &bx, &by)) return;
    raster_line(fb, (int)(ax + 0.5), (int)(ay + 0.5),
                (int)(bx + 0.5), (int)(by + 0.5), mode);
}

typedef struct {
    int64_t left;
    int64_t right;
    bool valid;
} pixel_span_t;

static int64_t integer_sqrt(int64_t value)
{
    int64_t low = 0;
    int64_t high = value;
    while (low < high) {
        int64_t middle = low + (high - low + 1) / 2;
        if (middle <= value / middle) low = middle;
        else high = middle - 1;
    }
    return low;
}

static pixel_span_t rounded_row(int64_t left, int64_t top, int64_t width,
                                int64_t height, int64_t radius, int64_t y)
{
    pixel_span_t span = { .valid = false };
    if (width <= 0 || height <= 0 || y < top || y >= top + height) return span;
    if (radius <= 0) return (pixel_span_t){ left, left + width - 1, true };
    int64_t center_y;
    if (y < top + radius) center_y = top + radius;
    else if (y >= top + height - radius) center_y = top + height - radius - 1;
    else return (pixel_span_t){ left, left + width - 1, true };
    int64_t dy = y - center_y;
    int64_t dx = integer_sqrt(radius * radius - dy * dy);
    span.left = left + radius - dx;
    span.right = left + width - radius - 1 + dx;
    span.valid = true;
    return span;
}

void kk_fb_round_rect(kk_fb_t *fb, int x, int y, int w, int h, int radius, bool filled, kk_draw_mode_t mode)
{
    if (!fb || w == 0 || h == 0) return;
    int64_t left = x, top = y, width = w, height = h;
    if (width < 0) { left += width; width = -width; }
    if (height < 0) { top += height; height = -height; }
    int64_t right = left + width;
    int64_t bottom = top + height;
    if (right <= 0 || bottom <= 0 || left >= KK_DISPLAY_WIDTH || top >= KK_DISPLAY_HEIGHT) return;
    int64_t max_radius = (width < height ? width : height) / 2;
    int64_t r = radius < 0 ? 0 : radius;
    if (r > max_radius) r = max_radius;
    int64_t y_start = top < 0 ? 0 : top;
    int64_t y_end = bottom > KK_DISPLAY_HEIGHT ? KK_DISPLAY_HEIGHT : bottom;
    for (int64_t py = y_start; py < y_end; py++) {
        pixel_span_t outer = rounded_row(left, top, width, height, r, py);
        if (!outer.valid) continue;
        int64_t segments[4] = { outer.left, outer.right, 1, 0 };
        if (!filled && width > 2 && height > 2) {
            int64_t inner_radius = r > 0 ? r - 1 : 0;
            if (inner_radius > (width - 2 < height - 2 ? width - 2 : height - 2) / 2) {
                inner_radius = (width - 2 < height - 2 ? width - 2 : height - 2) / 2;
            }
            pixel_span_t inner = rounded_row(left + 1, top + 1, width - 2, height - 2,
                                             inner_radius, py);
            if (inner.valid) {
                segments[1] = inner.left - 1;
                segments[2] = inner.right + 1;
                segments[3] = outer.right;
            }
        }
        for (int segment = 0; segment < 2; segment++) {
            int64_t px_start = segments[segment * 2];
            int64_t px_end = segments[segment * 2 + 1];
            if (px_start < 0) px_start = 0;
            if (px_end >= KK_DISPLAY_WIDTH) px_end = KK_DISPLAY_WIDTH - 1;
            for (int64_t px = px_start; px <= px_end; px++) {
                apply_pixel(fb, (int)px, (int)py, mode);
            }
        }
    }
}

void kk_fb_bitmap(kk_fb_t *fb, int x, int y, int w, int h, const uint8_t *bitmap,
                  size_t bitmap_size, kk_draw_mode_t mode)
{
    if (!fb || !bitmap || w == 0 || h == 0) return;
    int64_t left = x, top = y, width = w, height = h;
    if (width < 0) { left += width; width = -width; }
    if (height < 0) { top += height; height = -height; }
    size_t stride = (size_t)(width + 7) / 8;
    if ((uint64_t)stride * (uint64_t)height > bitmap_size) return;
    int64_t x_start = left < 0 ? 0 : left;
    int64_t y_start = top < 0 ? 0 : top;
    int64_t x_end = left + width > KK_DISPLAY_WIDTH ? KK_DISPLAY_WIDTH : left + width;
    int64_t y_end = top + height > KK_DISPLAY_HEIGHT ? KK_DISPLAY_HEIGHT : top + height;
    for (int64_t py = y_start; py < y_end; py++) {
        size_t source_row = (size_t)(py - top) * stride;
        for (int64_t px = x_start; px < x_end; px++) {
            size_t source_byte = source_row + (size_t)(px - left) / 8;
            uint8_t mask = (uint8_t)(0x80U >> ((px - left) & 7));
            if (bitmap[source_byte] & mask) apply_pixel(fb, (int)px, (int)py, mode);
        }
    }
}
