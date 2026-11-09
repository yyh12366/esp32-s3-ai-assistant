#include "av_adpcm.h"

static const int s_steps[] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
};
static const int s_index[] = {-1, -1, -1, -1, 2, 4, 6, 8};

size_t av_adpcm_block_bytes(const uint8_t header[6])
{
    if (header[2] > 88 || header[3] != 0) return 0;
    const size_t count = (size_t)header[4] | ((size_t)header[5] << 8);
    if (!count || count > AV_ADPCM_MAX_SAMPLES) return 0;
    return 6 + count / 2;
}

size_t av_adpcm_decode(const uint8_t *block, size_t size, int16_t *pcm, size_t capacity)
{
    if (size < 6 || av_adpcm_block_bytes(block) != size) return 0;
    const size_t count = (size_t)block[4] | ((size_t)block[5] << 8);
    if (count > capacity) return 0;
    int predictor = (int)block[0] | ((int)block[1] << 8);
    if (predictor >= 32768) predictor -= 65536;
    int index = block[2];
    pcm[0] = (int16_t)predictor;
    for (size_t i = 1; i < count; i++) {
        unsigned packed = block[6 + (i - 1) / 2];
        unsigned code = i % 2 ? packed >> 4 : packed & 15;
        int step = s_steps[index];
        int delta = step >> 3;
        if (code & 4) delta += step;
        if (code & 2) delta += step >> 1;
        if (code & 1) delta += step >> 2;
        predictor += code & 8 ? -delta : delta;
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        index += s_index[code & 7];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        pcm[i] = (int16_t)predictor;
    }
    return count;
}
