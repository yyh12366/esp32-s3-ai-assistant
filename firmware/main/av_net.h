// main/av_net.h —— 与 astrbot-relay 的 WebSocket 连接（lwIP socket + av_ws 协议层）。
//
// 一个网络任务负责连接、握手、接收；发送可以在任意任务里调用（内部加锁）。
// 结果通过 av_app_post_* 报告：AV_EV_WS_CONNECTED / AV_EV_WS_FAILED（都带 gen）、
// 以及每条文本消息 AV_EV_MSG。应用主动断开（av_net_disconnect）不会再报告失败。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t av_net_init(void);
// 异步连接；gen 原样带回连接结果事件。正在连接/已连接时先断开旧连接。
void av_net_connect(const char *url, const char *device_id, uint32_t gen);
void av_net_disconnect(void);

esp_err_t av_net_send_text(const char *json, size_t len);
esp_err_t av_net_send_binary(const uint8_t *data, size_t len);
