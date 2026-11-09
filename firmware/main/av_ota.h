// main/av_ota.h —— 固件 OTA 升级（HTTP 拉取媒体服务器 /ota/ 下的固件）。
//
// 流程：av_ota_check() 取 version.json，与当前版本比较；有新版本时界面让用户按 OK，
// 再调用 av_ota_update() 下载 firmware.bin 写入另一个 ota 分区，校验 size 与 sha256 后切换启动分区并重启。
// 两个任务都只在独立任务里跑（≤6KB 栈），界面通过 av_ota_get_status() 轮询，不直接碰 LVGL。
//
// 回滚确认：新固件以 PENDING_VERIFY 启动，av_ota_boot_poll() 在联网并运行一段时间后才标记有效；
// 之前复位则 bootloader 回滚到上一版。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "av_ota_manifest.h"

#define AV_OTA_MSG_MAX 64

typedef enum {
    AV_OTA_IDLE = 0,
    AV_OTA_CHECKING,     // 正在取 version.json
    AV_OTA_LATEST,       // 与当前版本相同
    AV_OTA_AVAILABLE,    // 有新版本，等待用户确认
    AV_OTA_DOWNLOADING,  // 下载、写入、校验中（percent 有效）
    AV_OTA_DONE,         // 已切换启动分区，即将重启
    AV_OTA_ERROR,        // error 为失败原因
} av_ota_state_t;

typedef struct {
    av_ota_state_t state;
    uint8_t percent;
    uint32_t revision;  // 状态或进度每次变化加一，界面据此判断是否重绘
    char current[AV_OTA_VERSION_MAX];
    char latest[AV_OTA_VERSION_MAX];
    char error[AV_OTA_MSG_MAX];
} av_ota_status_t;

// 开机调用一次（av_shell 初始化时）：记录运行分区的 OTA 状态。
void av_ota_init_boot(void);
// shell 任务周期调用：新固件联网稳定后确认有效，取消回滚。wifi_up 为是否已获得 IP。
void av_ota_boot_poll(bool wifi_up);

// 当前固件版本标识："<version> <date> <time>"，与发布清单的 version 字段比较。
const char *av_ota_current_version(void);

// 取清单并比较。media_base 为用户配置的媒体服务地址，token 可为空。
// 已有任务在跑时返回 ESP_ERR_INVALID_STATE。结果通过 av_ota_get_status() 取。
esp_err_t av_ota_check(const char *media_base, const char *token);
// 仅在 AV_OTA_AVAILABLE 时可调用：下载、写入、校验，成功后重启。
esp_err_t av_ota_update(void);

void av_ota_get_status(av_ota_status_t *out);
bool av_ota_busy(void);
