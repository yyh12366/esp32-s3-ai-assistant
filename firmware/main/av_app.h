// main/av_app.h —— 对讲（大肥鱼）应用任务：串行处理按键、网络、Wi-Fi 事件，驱动 av_session 状态机并更新界面。
//
// 生命周期（由 av_shell.c 或 av_boot.c 调用）：
//   av_app_start()  一次性：队列、网络任务、应用任务。之后重复调用无效。
//   av_app_enter()  回到对讲：显示对讲界面，同步 Wi-Fi 状态，恢复连接。
//   av_app_hide()   离开前台但保持中继连接（仅 astrbot_voice 进设置时用，行为与旧版一致）。
//   av_app_exit()   离开对讲：停麦、清播放、断开中继，挂起期间不重连。
//
// av_app_post_* 都是非阻塞的（队列满时丢弃并计数），可以在按键回调、事件循环、网络任务里调用。
#pragma once

#include <stdint.h>

#include "bsp_button.h"
#include "esp_err.h"

#include "av_proto.h"
#include "av_session.h"

// startup_error 非 NULL 时作为持久错误显示（例如 NVS 不可用）。仅第一次调用生效。
esp_err_t av_app_start(const char *startup_error);
void av_app_enter(void);
void av_app_hide(void);
void av_app_exit(void);

// 对讲请求回首页（离线时 OK 长按、在线时 UP 长按）。在应用任务里调用，回调只能非阻塞地投递。
void av_app_set_home_cb(void (*cb)(void));

// 设置页：改了中继地址（地址已由调用方写入 NVS）→ 只切换并重连，不再保存。
void av_app_use_relay(const char *url);
// 设置页：蓝牙配网开始 / 结束（结束后按当前 Wi-Fi 状态恢复）。
void av_app_provision_begin(void);
void av_app_provision_done(void);

void av_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void av_app_post_simple(av_ev_type_t type);
void av_app_post_wifi(av_ev_type_t type, int reason);
void av_app_post_ws(av_ev_type_t type, int err, int http, uint32_t gen);
void av_app_post_msg(av_msg_t *heap_msg);    // 应用负责 free
void av_app_post_relay(char *heap_url);      // BLUFI 收到的中继地址；应用负责 free
