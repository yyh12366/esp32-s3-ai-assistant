// main/av_proto.h —— astrbot-relay 设备协议的 JSON 控制消息（纯逻辑）。
//
// 中继 → 设备：state / asr / reply / tts_start / tts_end / error
// 设备 → 中继：hello / listen_start / listen_stop / abort
// 协议定义见 docs/reference/astrbot-relay/README.md。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define AV_MSG_TEXT_MAX 768  // UTF-8 字节，含 NUL；超长时按字符边界截断

typedef enum {
    AV_MSG_UNKNOWN = 0,
    AV_MSG_STATE,
    AV_MSG_ASR,
    AV_MSG_REPLY,
    AV_MSG_TTS_START,
    AV_MSG_TTS_END,
    AV_MSG_ERROR,
} av_msg_type_t;

typedef enum {
    AV_VOICE_IDLE = 0,
    AV_VOICE_LISTENING,
    AV_VOICE_THINKING,
    AV_VOICE_SPEAKING,
} av_voice_t;

typedef struct {
    av_msg_type_t type;
    av_voice_t state;    // AV_MSG_STATE
    bool state_known;    // value 是否为已知状态
    bool final;          // AV_MSG_REPLY
    bool truncated;      // text 被截断
    char audio_codec[16]; // state 消息的下行音频协商结果，空值表示原始 PCM
    char text[AV_MSG_TEXT_MAX];  // asr/reply 的 text、error 的 msg
} av_msg_t;

// 解析一条文本帧。返回 0 成功（未知 type 也算成功，type=AV_MSG_UNKNOWN），<0 表示 JSON 格式错误。
int av_proto_parse(const char *json, size_t len, av_msg_t *out);

// 生成设备 → 中继消息，返回长度；空间不足返回 -1。
int av_proto_hello(char *buf, size_t size, const char *device_id);
int av_proto_listen_start(char *buf, size_t size);
int av_proto_listen_stop(char *buf, size_t size);
int av_proto_abort(char *buf, size_t size);

// 把 Unicode 码点编码为 UTF-8，返回字节数（1..4），非法码点返回 0。
size_t av_utf8_encode(unsigned cp, char out[4]);
