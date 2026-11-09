#include "av_shell.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "av_shell_boot";
#define BOOT_NS "boot"
#define BOOT_KEY "target"

// ---------------------------------------------------------------------------
// 开机目标（NVS boot/target）：监测 ↔ 首页
// ---------------------------------------------------------------------------
uint8_t av_shell_boot_target(void)
{
    nvs_handle_t h;
    uint8_t target = AV_SHELL_TARGET_SHELL;
    if (nvs_open(BOOT_NS, NVS_READONLY, &h) != ESP_OK) return AV_SHELL_TARGET_SHELL;
    nvs_get_u8(h, BOOT_KEY, &target);
    nvs_close(h);
#ifdef AV_S3_VIDEO
    if (target == AV_SHELL_TARGET_VIDEO) return target;
#endif
    return target == AV_SHELL_TARGET_MONITOR ? AV_SHELL_TARGET_MONITOR : AV_SHELL_TARGET_SHELL;
}
static esp_err_t save_boot_target(uint8_t target)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOOT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, BOOT_KEY, target);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

#ifdef AV_S3_VIDEO
bool av_shell_consume_video_target(void)
{
    esp_err_t err = save_boot_target(AV_SHELL_TARGET_SHELL);
    if (err != ESP_OK) ESP_LOGE(TAG, "Could not clear one-shot video boot: %s", esp_err_to_name(err));
    return err == ESP_OK;
}
#endif

void av_shell_reboot_to(uint8_t target)
{
    esp_err_t err = save_boot_target(target);
    if (err != ESP_OK) ESP_LOGE(TAG, "开机目标保存失败: %s", esp_err_to_name(err));
    // 能走到这里说明新固件已启动、画出了首页并响应了按键：先确认 OTA 有效，
    // 否则 20 秒内切去监测会让 bootloader 当作启动失败而回滚。
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "切换前确认新固件有效");
    }
    ESP_LOGI(TAG, "重启到目标 %u", (unsigned)target);
    esp_restart();
}
