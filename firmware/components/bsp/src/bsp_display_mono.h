#pragma once

#include <stddef.h>
#include <stdint.h>

void bsp_display_mono2_pack_row(const uint8_t *source, size_t width,
                               uint16_t foreground_rgb565,
                               uint16_t background_rgb565, uint8_t *destination);
