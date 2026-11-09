// main/av_audio.h —— 录音/播放 worker（16kHz/16bit/单声道，全双工）。
//
// 录音：独立任务每次 bsp_audio_read 40ms（1280 字节），立刻作为一帧二进制发给中继，
//       不整段缓存。
// 播放：先积累 320ms，欠载后重新缓冲；播放任务每次取 20ms
//       调 bsp_audio_write。缓冲满时网络任务等待（TCP 自然限流），中继本身也按实时速度下发。
// 所有函数都可以在应用任务/网络任务里调用；都不能在按键回调里调用。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "av_proto.h"

#define AV_AUDIO_RATE        16000
#define AV_AUDIO_FRAME_BYTES 1280   // 40ms，与中继 audio.frame_ms 一致
#define AV_AUDIO_RING_BYTES  20480  // 640ms，容纳公网到包抖动和中继提前发送
#define AV_AUDIO_RING_MIN_BYTES 12288  // 384ms，堆碎片化时的退路
#define AV_AUDIO_PREBUFFER_BYTES 10240  // 320ms，短回复在 tts_end 后直接播放

esp_err_t av_audio_init(uint8_t volume);
void av_audio_set_volume(uint8_t volume);

// 开始/停止录音。stop 会等录音任务把当前这一帧处理完再返回（最多约 100ms），
// 保证 listen_stop 之后不会再有音频帧发出。
void av_audio_mic_start(void);
void av_audio_mic_stop(void);
int av_audio_mic_level(void);  // 0..100，聆听动画用

// 网络任务：写入收到的 PCM（缓冲满时最多等 1s）。
void av_audio_feed(const uint8_t *pcm, size_t len);
// 网络任务：按到达顺序报告中继消息（用于打断后的闸门，见 av_ring.h）。
void av_audio_on_server_msg(av_msg_type_t type, av_voice_t state);
// 应用任务：打断——清空缓冲，丢弃本轮剩余音频。
void av_audio_interrupt(void);
// 新连接建立时重置闸门。
void av_audio_reset(void);
// 应用任务：蓝牙配网期间把播放缓冲还给堆（BLE + Wi-Fi 共存时堆最紧），
// 期间收到的音频直接丢弃；退出配网后取回。取回失败时只是没有声音，不影响其他功能。
void av_audio_release_buffer(void);
esp_err_t av_audio_restore_buffer(void);
