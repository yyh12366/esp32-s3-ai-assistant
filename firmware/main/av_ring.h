// main/av_ring.h —— 播放用的小环形缓冲 + 打断闸门（纯逻辑，调用方负责加锁）。
//
// 闸门解决“打断后上一轮残留音频”的问题：
//   设备打断（abort / 新的 listen_start）时关闭闸门并清空缓冲；
//   中继按顺序先发完被取消那一轮的音频，再发 tts_end / state idle / state listening。
//   网络任务按收到的顺序处理：遇到这三种消息就重新打开闸门。
//   闸门关闭期间到达的音频全部丢弃。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "av_proto.h"

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;   // 下一个读位置
    size_t len;    // 已存字节数
    bool gate_open;
    bool playing;  // 达到预缓冲门槛后连续取样，欠载时回到等待状态
    bool receiving;
    uint32_t underruns;
    uint32_t dropped;  // 闸门关闭期间丢弃的字节数（调试用）
} av_ring_t;

void av_ring_init(av_ring_t *r, uint8_t *storage, size_t cap);
// 写入尽可能多的数据，返回实际写入字节数；闸门关闭时丢弃并返回 len（视为已处理）。
size_t av_ring_write(av_ring_t *r, const uint8_t *data, size_t len);
size_t av_ring_read(av_ring_t *r, uint8_t *out, size_t max);
// 播放读取：启动/欠载后先积累 prebuffer 字节，结束后直接排空短尾音。
size_t av_ring_play_read(av_ring_t *r, uint8_t *out, size_t max, size_t prebuffer);
size_t av_ring_free(const av_ring_t *r);
// 设备侧打断：关闸并清空。
void av_ring_interrupt(av_ring_t *r);
// 中继消息按到达顺序喂给闸门；返回闸门是否因此打开。
bool av_ring_on_server_msg(av_ring_t *r, av_msg_type_t type, av_voice_t state);
