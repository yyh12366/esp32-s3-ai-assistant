// main/av_ws.h —— 极简 WebSocket 客户端协议层（纯逻辑，不依赖 ESP-IDF，主机端可测）。
//
// 只实现中继需要的子集（RFC 6455）：客户端握手、带掩码的单帧发送、
// 服务端帧的流式解析（payload 边收边回调，二进制音频不需要整帧缓存）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "av_url.h"

#define AV_WS_OP_CONT  0x0
#define AV_WS_OP_TEXT  0x1
#define AV_WS_OP_BIN   0x2
#define AV_WS_OP_CLOSE 0x8
#define AV_WS_OP_PING  0x9
#define AV_WS_OP_PONG  0xA

#define AV_WS_HEADER_MAX 14  // 2 + 8 (64 位长度) + 4 (掩码)
#define AV_WS_KEY_LEN    24  // base64(16 字节随机数)

// ---- 工具：SHA-1 与 base64（握手校验用） ----
void av_sha1(const uint8_t *data, size_t len, uint8_t out[20]);
// 返回写入的字符数（不含 NUL）；空间不足返回 0。
size_t av_base64(const uint8_t *data, size_t len, char *out, size_t out_size);

// 由 16 字节随机数生成 Sec-WebSocket-Key（24 字符 + NUL）。
void av_ws_make_key(const uint8_t nonce[16], char key[AV_WS_KEY_LEN + 1]);

// 生成握手请求。返回长度；空间不足返回 -1。
int av_ws_handshake_request(char *buf, size_t size, const av_url_t *url, const char *key);

typedef enum {
    AV_WS_HS_INCOMPLETE = 0,  // 还没收到完整的响应头（\r\n\r\n）
    AV_WS_HS_OK,
    AV_WS_HS_BAD_STATUS,      // 非 101，*status 给出 HTTP 状态码
    AV_WS_HS_BAD_ACCEPT,      // 101 但 Sec-WebSocket-Accept 不对
    AV_WS_HS_MALFORMED,
} av_ws_hs_result_t;

// 校验握手响应。*header_len 为响应头总长度（之后的字节已经是 WebSocket 帧）。
av_ws_hs_result_t av_ws_handshake_check(const char *resp, size_t len, const char *key,
                                        int *status, size_t *header_len);

// ---- 发送：客户端帧必须带掩码 ----
// 写帧头（FIN=1），返回帧头长度。
size_t av_ws_frame_header(uint8_t hdr[AV_WS_HEADER_MAX], uint8_t opcode, size_t payload_len,
                          const uint8_t mask[4]);
// 对 payload 原地异或掩码；offset 为这段数据在整个 payload 中的起始位置。
void av_ws_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4], size_t offset);

// ---- 接收：流式解析服务端帧 ----
typedef struct {
    // 一条消息开始（首帧）。opcode 为 TEXT/BIN；控制帧也走这里（PING/PONG/CLOSE）。
    void (*begin)(void *user, uint8_t opcode, uint64_t frame_len);
    // payload 片段（已去掩码）。消息分片时对每片都回调。
    void (*data)(void *user, uint8_t opcode, const uint8_t *data, size_t len);
    // 一帧结束；fin=true 表示整条消息结束。
    void (*end)(void *user, uint8_t opcode, bool fin);
} av_ws_sink_t;

typedef enum {
    AV_WS_PARSE_OK = 0,
    AV_WS_PARSE_ERR_RSV,          // RSV 位非零（未协商扩展）
    AV_WS_PARSE_ERR_MASKED,       // 服务端帧不能带掩码
    AV_WS_PARSE_ERR_OPCODE,       // 未知 opcode 或分片顺序错误
    AV_WS_PARSE_ERR_CONTROL,      // 控制帧分片或超过 125 字节
    AV_WS_PARSE_ERR_TOO_LARGE,    // 超过 max_frame
} av_ws_parse_err_t;

typedef struct {
    uint64_t max_frame;
    // 内部状态
    uint8_t hdr[AV_WS_HEADER_MAX];
    uint8_t hdr_len;
    uint8_t hdr_need;
    uint8_t opcode;        // 当前帧 opcode（续帧会替换为消息 opcode）
    uint8_t msg_opcode;    // 当前分片消息的 opcode，0 = 不在分片中
    bool fin;
    bool in_payload;
    uint64_t remaining;
    av_ws_parse_err_t err;
} av_ws_parser_t;

void av_ws_parser_init(av_ws_parser_t *p, uint64_t max_frame);
// 返回 AV_WS_PARSE_OK 或错误；出错后解析器保持错误状态，调用方应关闭连接。
av_ws_parse_err_t av_ws_parse(av_ws_parser_t *p, const uint8_t *data, size_t len,
                              const av_ws_sink_t *sink, void *user);
