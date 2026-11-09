#include "kk_font.h"

#include "../assets/fonts/kk_font_data.h"

static const kk_font_glyph_t *find_glyph(uint32_t codepoint)
{
    size_t low = 0;
    size_t high = KK_FONT_GLYPH_COUNT;

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        uint32_t candidate = KK_FONT_GLYPHS[middle].codepoint;
        if (candidate == codepoint) return &KK_FONT_GLYPHS[middle];
        if (candidate < codepoint) low = middle + 1;
        else high = middle;
    }
    return NULL;
}

static uint32_t next_codepoint(const unsigned char **cursor)
{
    const unsigned char *text = *cursor;
    uint32_t codepoint;

    if (text[0] < 0x80) {
        *cursor = text + 1;
        return text[0];
    }
    if ((text[0] & 0xE0) == 0xC0) {
        codepoint = (text[0] & 0x1F) << 6 | (text[1] & 0x3F);
        *cursor = text + 2;
        return codepoint;
    }
    if ((text[0] & 0xF0) == 0xE0) {
        codepoint = (text[0] & 0x0F) << 12 | (text[1] & 0x3F) << 6 | (text[2] & 0x3F);
        *cursor = text + 3;
        return codepoint;
    }

    codepoint = (text[0] & 0x07) << 18 | (text[1] & 0x3F) << 12 |
                (text[2] & 0x3F) << 6 | (text[3] & 0x3F);
    *cursor = text + 4;
    return codepoint;
}

static int codepoint_advance(uint32_t codepoint)
{
    const kk_font_glyph_t *glyph = find_glyph(codepoint);
    return glyph ? glyph->advance : codepoint < 0x80 ? 8 : 12;
}

float kk_font_measure(uint32_t codepoint, void *context)
{
    (void)context;
    return (float)codepoint_advance(codepoint);
}

float kk_font_width(const char *text)
{
    return kk_measure_utf8(text, kk_font_measure, NULL);
}

bool kk_font_has(uint32_t codepoint)
{
    return find_glyph(codepoint) != NULL;
}

int kk_font_draw(kk_fb_t *fb, int x, int y, const char *text, kk_draw_mode_t mode)
{
    return kk_font_draw_clipped(fb, x, y, text, mode, 0, 0, KK_DISPLAY_WIDTH, KK_DISPLAY_HEIGHT);
}

int kk_font_draw_clipped(kk_fb_t *fb, int x, int y, const char *text, kk_draw_mode_t mode,
                         int clip_x, int clip_y, int clip_w, int clip_h)
{
    const unsigned char *cursor = (const unsigned char *)text;
    int start_x = x;
    int clip_right = clip_x + clip_w;
    int clip_bottom = clip_y + clip_h;

    while (*cursor) {
        uint32_t codepoint = next_codepoint(&cursor);
        const kk_font_glyph_t *glyph = find_glyph(codepoint);
        if (glyph) {
            for (int row = 0; row < KK_FONT_LINE_HEIGHT - 2; row++) {
                for (int col = 0; col < glyph->width; col++) {
                    if (glyph->bitmap[row * 2 + col / 8] & (0x80 >> (col & 7))) {
                        int px = x + col;
                        int py = y + row + 1;
                        if (px >= clip_x && px < clip_right && py >= clip_y && py < clip_bottom) {
                            kk_fb_point(fb, px, py, mode);
                        }
                    }
                }
            }
        }
        x += codepoint_advance(codepoint);
    }
    return x - start_x;
}
