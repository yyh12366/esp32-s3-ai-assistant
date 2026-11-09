// main/av_store.h —— 应用设置的 NVS 持久化（命名空间 "astrbot_voice"）。
// Wi-Fi 凭据由 esp_wifi 自己存在 NVS（WIFI_STORAGE_FLASH），这里只存应用设置。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 初始化 NVS。不会自动擦除分区：NVS 损坏时返回错误，由上层提示。
esp_err_t av_store_init(void);

// 服务器选择：本机（局域网里的 Mac，默认）或 la（远端服务器）。
#define AV_SERVER_LOCAL 0
#define AV_SERVER_LA    1
uint8_t av_store_get_server(void);
esp_err_t av_store_set_server(uint8_t server);

// 读取当前服务器的中继地址：本机 = 保存的地址（没保存过用 Kconfig 默认值），
// la = Kconfig 的 la 地址。两种都会沿用默认地址里的 ?token=...。
void av_store_get_relay(char *buf, size_t size);
esp_err_t av_store_set_relay(const char *url);  // 调用方先用 av_url_parse 校验

// 当前服务器的媒体 HTTP 基址（不含路径和 token）。本机 = http://<本机中继主机>:8766，
// la = Kconfig 的 la 媒体地址。本机中继地址无效时返回空串。
void av_store_get_media_base(char *buf, size_t size);

uint8_t av_store_get_volume(void);
esp_err_t av_store_set_volume(uint8_t percent);

// 设备 ID：passport-<MAC 后 3 字节>，用作中继的 device_id（AstrBot username）。
void av_store_device_id(char *buf, size_t size);

// 阅读进度：书名索引 + 页码。没有记录时返回 false。
bool av_store_get_read_pos(int *book, int *page);
esp_err_t av_store_set_read_pos(int book, int page);
