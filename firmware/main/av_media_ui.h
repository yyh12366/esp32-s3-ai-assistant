// main/av_media_ui.h —— 小说/音乐两个页面的按键与状态（LVGL 页面本身在 av_ui.c）。
//
// 页面：书单 → 阅读页；歌单 → 播放页。全部按键先问这里，返回 true 表示已消费。
// 阅读进度（书名索引 + 页码）存在 NVS 命名空间 "astrbot_voice" 的 read_pos。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

void av_media_ui_init(void);
void av_media_ui_open_books(void);
void av_media_ui_open_music(void);

// 媒体页是否在前台（是的话按键不交给对讲状态机）。
bool av_media_ui_active(void);
bool av_media_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 应用 tick 调用：取异步结果并刷新页面。
void av_media_ui_poll(void);

// 进入对讲前调用：音乐正在放就停掉，避免和 TTS 抢 I2S。
void av_media_ui_stop_playback(void);

// 离开小说/音乐（回首页）：停止播放，页面状态回到“不在前台”。
void av_media_ui_close(void);
