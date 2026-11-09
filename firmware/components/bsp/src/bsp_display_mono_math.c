#include "bsp_display_mono.h"

void bsp_display_mono2_pack_row(const uint8_t *source, size_t width,
                               uint16_t foreground_rgb565,
                               uint16_t background_rgb565, uint8_t *destination)
{
    for (size_t x = 0; x < width; ++x) {
        const uint16_t color = source[x / 8] & (0x80u >> (x % 8))
                             ? foreground_rgb565 : background_rgb565;
        const size_t offset = x * 4;
        destination[offset] = (uint8_t)(color >> 8);
        destination[offset + 1] = (uint8_t)color;
        destination[offset + 2] = (uint8_t)(color >> 8);
        destination[offset + 3] = (uint8_t)color;
    }
}
