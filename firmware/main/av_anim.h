// main/av_anim.h —— 四种语音状态动画的几何计算（纯逻辑；绘制在 av_ui.c）。
//
// 动画由 AV_ANIM_PARTS 个圆点组成，围绕中心 (0,0) 摆放：
//   待机  一个慢速“呼吸”的大圆，其余点隐藏
//   聆听  外圈一圈点随麦克风音量向外扩张（音量 0..100）
//   思考  三个点沿圆周追逐旋转
//   说话  一排竖条按不同相位上下跳动（像均衡器）
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "av_proto.h"

#define AV_ANIM_PARTS 8

typedef struct {
    int16_t x, y;   // 中心相对坐标（像素）
    int16_t w, h;   // 尺寸
    uint8_t opa;    // 0..255
    bool visible;
} av_anim_part_t;

// t_ms：动画时间；level：麦克风音量 0..100（仅聆听用）。
void av_anim_frame(av_voice_t voice, uint32_t t_ms, int level, av_anim_part_t parts[AV_ANIM_PARTS]);

// 把一帧 16 位 PCM 的音量换算为 0..100（对数刻度，用于聆听动画）。
int av_anim_level(const int16_t *pcm, unsigned samples);
