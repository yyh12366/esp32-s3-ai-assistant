// main/av_wifi.h —— Wi-Fi STA 连接与 BLUFI 蓝牙配网（参考 demo/blufi-provisioning@9c039cc）。
//
// 凭据由 esp_wifi 存在 NVS（WIFI_STORAGE_FLASH）。断线后按 1→30s 退避自动重连。
// 状态通过 av_app_post_*：AV_EV_WIFI_NO_CONFIG / CONNECTING / UP / DOWN(reason)。
// 配网时启动 NimBLE + BLUFI（广播名 BLUFI_FoloPassport），配网结束释放 BLE 资源。
// BLUFI 自定义数据 "relay=ws://..." 可以同时设置中继地址。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define AV_BLUFI_NAME "BLUFI_FoloPassport"

typedef enum {
    AV_PROV_OFF = 0,
    AV_PROV_STARTING,
    AV_PROV_ADVERTISING,
    AV_PROV_PHONE_CONNECTED,
    AV_PROV_WIFI_CONNECTING,
    AV_PROV_WIFI_CONNECTED,
    AV_PROV_WIFI_FAILED,
    AV_PROV_ERROR,
} av_prov_state_t;

esp_err_t av_wifi_init(void);    // 需要 NVS 已初始化
bool av_wifi_has_config(void);
bool av_wifi_connected(void);
void av_wifi_info(char *ssid, size_t ssid_size, char *ip, size_t ip_size);
esp_err_t av_wifi_clear(void);   // 断开并清除保存的凭据

esp_err_t av_wifi_provision_start(void);
void av_wifi_provision_stop(void);
av_prov_state_t av_wifi_provision_state(void);
int av_wifi_provision_reason(void);  // 最近一次失败原因（AV_PROV_WIFI_FAILED 时有效）
