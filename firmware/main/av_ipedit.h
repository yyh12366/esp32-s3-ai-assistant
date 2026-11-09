// main/av_ipedit.h —— 用三个键编辑中继地址 IPv4:端口（纯逻辑）。
//
//   UP / DOWN 单击   当前字段 +1 / -1（循环）
//   UP / DOWN 长按   当前字段 +10 / -10（端口为 +100 / -100）
//   OK 单击          下一个字段；在端口上按 OK = 保存
//   OK 长按          放弃修改
// 路径（如 /ws?token=...）保持原值不变。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "av_url.h"

#define AV_IPEDIT_FIELDS 5  // 4 段 IP + 端口

typedef enum {
    AV_IPEDIT_KEY_UP,
    AV_IPEDIT_KEY_DOWN,
    AV_IPEDIT_KEY_UP_LONG,
    AV_IPEDIT_KEY_DOWN_LONG,
    AV_IPEDIT_KEY_OK,
    AV_IPEDIT_KEY_OK_LONG,
} av_ipedit_key_t;

typedef enum {
    AV_IPEDIT_EDITING = 0,
    AV_IPEDIT_SAVED,
    AV_IPEDIT_CANCELLED,
} av_ipedit_result_t;

typedef struct {
    uint16_t value[AV_IPEDIT_FIELDS];
    uint8_t field;
    char path[AV_URL_PATH_MAX];
} av_ipedit_t;

// 从当前地址开始编辑；host 不是 IPv4 时从 192.168.1.100 开始，端口缺省 8765。
void av_ipedit_begin(av_ipedit_t *e, const char *current_url);
av_ipedit_result_t av_ipedit_key(av_ipedit_t *e, av_ipedit_key_t key);
// 生成完整地址 ws://a.b.c.d:port/path
int av_ipedit_url(const av_ipedit_t *e, char *buf, size_t size);
