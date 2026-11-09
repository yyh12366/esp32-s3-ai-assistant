#pragma once

#include <stddef.h>
#include <stdint.h>

#define AV_ADPCM_MAX_SAMPLES 640
#define AV_ADPCM_MAX_BYTES (6 + AV_ADPCM_MAX_SAMPLES / 2)

// 只看 6 字节头部，返回整块字节数（6 + 采样数/2）；头部不合法返回 0。
// 流式读取时先用它决定还要再读多少字节。
size_t av_adpcm_block_bytes(const uint8_t header[6]);

// LE int16 predictor, uint8 index, zero, LE uint16 count, then high nibble first.
// Returns sample count, or zero for an invalid independent IMA block.
size_t av_adpcm_decode(const uint8_t *block, size_t size, int16_t *pcm, size_t capacity);
