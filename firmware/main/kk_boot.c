// main/kk_boot.c —— 合并固件的开机分流。
//
// 开机目标存在 NVS 命名空间 "boot" 的 "target" 里：0 = 首页（默认），1 = 监测。
// 不再显示开机选择器：切换功能在首页里完成，只有进入/离开监测才会保存目标并重启。
#include "kk_boot.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "av_shell.h"
#include "kk_app.h"
#ifdef AV_S3_VIDEO
#include "s3_video.h"
#endif

static const char *TAG = "kk_boot";

void kk_boot_run(void)
{
    // 读开机目标需要 NVS；失败时按首页启动，av_store_init 会再报告存储错误
    if (nvs_flash_init() != ESP_OK) ESP_LOGW(TAG, "NVS 初始化失败，按首页启动");
    const uint8_t target = av_shell_boot_target();
    ESP_LOGI(TAG, "boot target=%u", (unsigned)target);
    if (target == AV_SHELL_TARGET_MONITOR) {
        kk_app_start();
        return;
    }
#ifdef AV_S3_VIDEO
    if (target == AV_SHELL_TARGET_VIDEO && av_shell_consume_video_target()) {
        s3_video_run();
        return;
    }
#endif
    av_shell_start();
}
