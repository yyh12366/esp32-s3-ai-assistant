// main/av_media_app.h —— 小说 / 音乐：进入、离开、按键、轮询（由 av_shell.c 调度）。
//
// 只用显示、Wi-Fi 和媒体 HTTP 客户端，不启动语音会话（不连中继 WS）。
// 媒体客户端在第一次进入时启动（16KB worker 只在需要时才占 RAM），之后只更新地址。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

// start_with_music=true 进音乐，否则进小说。
void av_media_app_enter(bool start_with_music);
void av_media_app_exit(void);

// 返回 true：应回首页（列表页长按 OK）。
bool av_media_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// shell 任务周期调用：取异步结果并刷新页面。
void av_media_app_poll(void);
