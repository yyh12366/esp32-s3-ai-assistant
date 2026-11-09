// main/av_settings.h —— 设置：服务器、中继地址、蓝牙配网、音量、清除 Wi-Fi、设备信息。
//
// 从首页「设置」进入，与对讲应用无关：对讲未在前台时也能改地址、配网。
// 所有函数都在 shell 任务里调用（按键、轮询）。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

void av_settings_enter(void);   // 显示设置根菜单
void av_settings_exit(void);

// 返回 true：应回首页（根菜单长按 OK / 选「返回」）。
bool av_settings_key(bsp_btn_t btn, bsp_btn_ev_t bev);

// shell 任务周期调用：蓝牙配网的状态刷新与超时退出。
void av_settings_poll(void);
