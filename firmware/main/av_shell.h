// main/av_shell.h —— 合并固件的首页与功能切换（大肥鱼 / 小说 / 音乐 / 设置 / 监测）。
//
// 一次性初始化显示、LVGL、NVS、电量计、音频、Wi-Fi 和按键；所有按键只进一个队列，由 shell 任务分发给当前功能。
// 切换功能不重启；只有「监测」（1-bit 直写，和 LVGL 共用 LCD 完成回调）要保存目标后 esp_restart()。
//
// astrbot_voice 使用 av_shell_start_voice_only()：没有首页，直接进入对讲，设置从对讲进入。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AV_SHELL_TARGET_SHELL   0  // 开机进入首页（默认）
#ifdef AV_S3_VIDEO
#define AV_SHELL_TARGET_VIDEO   2
bool av_shell_consume_video_target(void);
#endif
#define AV_SHELL_TARGET_MONITOR 1  // 开机进入监测（KK 1-bit UI）

// 读取开机目标（NVS 需已初始化）；读取失败按首页处理。
uint8_t av_shell_boot_target(void);
// 保存开机目标并重启。不返回。
void av_shell_reboot_to(uint8_t target);

void av_shell_start(void);             // 合并固件：进入首页
void av_shell_start_voice_only(void);  // astrbot_voice：直接进入对讲
