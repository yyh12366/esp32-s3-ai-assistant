#include "av_anim.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

static float wave(uint32_t t_ms, uint32_t period_ms, float phase)
{
    float x = (float)(t_ms % period_ms) / (float)period_ms;
    return sinf(2.0f * PI_F * x + phase);
}

static int16_t clamp16(float v, int lo, int hi)
{
    int i = (int)lroundf(v);
    if (i < lo) i = lo;
    if (i > hi) i = hi;
    return (int16_t)i;
}

void av_anim_frame(av_voice_t voice, uint32_t t_ms, int level, av_anim_part_t parts[AV_ANIM_PARTS])
{
    memset(parts, 0, sizeof(av_anim_part_t) * AV_ANIM_PARTS);
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    switch (voice) {
    case AV_VOICE_IDLE: {
        // 呼吸：直径 56..68，透明度 150..230，周期 3.2s
        float b = (wave(t_ms, 3200, 0) + 1.0f) / 2.0f;
        int16_t d = clamp16(56 + 12 * b, 0, 100);
        parts[0] = (av_anim_part_t){0, 0, d, d, (uint8_t)clamp16(150 + 80 * b, 0, 255), true};
        break;
    }
    case AV_VOICE_LISTENING: {
        // 中心实心圆 + 外圈 7 点，半径随音量 30..50 抖动
        parts[0] = (av_anim_part_t){0, 0, 44, 44, 255, true};
        float radius = 30.0f + 20.0f * (float)level / 100.0f;
        for (int i = 1; i < AV_ANIM_PARTS; i++) {
            float ang = 2.0f * PI_F * (float)(i - 1) / (AV_ANIM_PARTS - 1) + (float)(t_ms % 6000) / 6000.0f * 2.0f * PI_F;
            float r = radius + 3.0f * wave(t_ms, 600, (float)i);
            parts[i] = (av_anim_part_t){clamp16(r * cosf(ang), -60, 60), clamp16(r * sinf(ang), -60, 60),
                                        10, 10, (uint8_t)clamp16(140 + level, 0, 255), true};
        }
        break;
    }
    case AV_VOICE_THINKING: {
        // 三个点沿半径 26 的圆周追逐，周期 1.2s，彼此相差 0.35 rad，尾部渐淡
        for (int i = 0; i < 3; i++) {
            float base = (float)(t_ms % 1200) / 1200.0f * 2.0f * PI_F;
            float ang = base - 0.45f * (float)i;
            int16_t d = (int16_t)(16 - 3 * i);
            parts[i] = (av_anim_part_t){clamp16(26.0f * cosf(ang), -40, 40), clamp16(26.0f * sinf(ang), -40, 40),
                                        d, d, (uint8_t)(255 - 70 * i), true};
        }
        break;
    }
    case AV_VOICE_SPEAKING: {
        // 7 根竖条，宽 8，间距 14，高 10..54
        for (int i = 0; i < 7; i++) {
            float v = (wave(t_ms, 520 + 70u * (uint32_t)i, 1.3f * (float)i) + 1.0f) / 2.0f;
            int16_t h = clamp16(10 + 44 * v, 10, 54);
            parts[i] = (av_anim_part_t){(int16_t)((i - 3) * 14), 0, 8, h, 235, true};
        }
        break;
    }
    }
}

int av_anim_level(const int16_t *pcm, unsigned samples)
{
    if (!pcm || samples == 0) return 0;
    double acc = 0;
    for (unsigned i = 0; i < samples; i++) acc += (double)pcm[i] * pcm[i];
    double rms = sqrt(acc / samples);
    if (rms < 30.0) return 0;
    // 30（底噪）→ 0，约 8000（大声说话）→ 100
    double db = 20.0 * log10(rms / 30.0);
    int v = (int)(db / 48.5 * 100.0);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}
