// main/av_session.h —— 对讲应用的核心状态机（纯逻辑，不依赖 ESP-IDF/LVGL，主机端可测）。
//
// 输入：网络事件、中继消息、按键事件、定时 tick（都带当前毫秒时间）。
// 输出：av_actions_t —— 要发送的消息、麦克风开关、清空播放、发起连接、界面刷新等。
// 应用层（av_app.c）只负责执行这些动作，不再自己做判断。
//
// 按键约定：
//   OK 按下            在线且待机：开始录音（listen_start）
//                      思考/说话中：打断当前回复并开始录音（中继把 listen_start 视为打断）
//   OK 松开            按住 >= AV_TAP_MS：结束录音（listen_stop）；
//                      短按：不发送录音（abort）——在说话中短按 = 只打断
//   OK 长按            未连上中继时：返回首页（在线时 OK 长按就是在录音，不拦截）
//   UP 长按            任何时候（录音中除外）：返回首页
//   SUSPEND / RESUME   切到其他功能时挂起：停麦、清播放、断开中继，挂起期间不重连；恢复时再连
//   UP / DOWN 单击      滚动回复文字
//   离线时 OK 单击      立即重连
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "av_proto.h"

#define AV_TAP_MS              350     // 短于此视为短按，不发送录音
#define AV_MAX_LISTEN_MS       30000   // 与中继 max_listen_seconds 一致，到时自动结束
#define AV_REPLY_TIMEOUT_MS    60000   // 松开后这么久没有任何回复 → 提示超时
#define AV_ERROR_SHOW_MS       6000    // 错误提示保留时间（之后自动隐藏）

typedef enum {
    AV_LINK_NO_CONFIG = 0,  // 没有 Wi-Fi 凭据
    AV_LINK_PROVISIONING,   // 正在蓝牙配网（暂停中继连接）
    AV_LINK_WIFI_CONNECTING,
    AV_LINK_WS_CONNECTING,
    AV_LINK_ONLINE,
    AV_LINK_RETRY_WAIT,     // 中继连接失败，等待重试
} av_link_t;

typedef enum {
    AV_WS_ERR_NONE = 0,
    AV_WS_ERR_BAD_URL,
    AV_WS_ERR_DNS,
    AV_WS_ERR_CONNECT,      // TCP 连接失败（拒绝/超时）
    AV_WS_ERR_HTTP,         // 握手返回非 101，code 为 HTTP 状态
    AV_WS_ERR_HANDSHAKE,    // 握手校验失败
    AV_WS_ERR_IO,           // 收发出错
    AV_WS_ERR_TIMEOUT,      // 长时间收不到任何数据
    AV_WS_ERR_CLOSED,       // 对方关闭
    AV_WS_ERR_PROTOCOL,     // 帧格式错误
} av_ws_err_t;

typedef enum {
    AV_EV_TICK = 0,
    AV_EV_WIFI_NO_CONFIG,
    AV_EV_WIFI_CONNECTING,
    AV_EV_WIFI_UP,
    AV_EV_WIFI_DOWN,        // code = Wi-Fi 断开原因
    AV_EV_PROVISION_START,
    AV_EV_PROVISION_STOP,
    AV_EV_WS_CONNECTED,     // 握手完成且 hello 已发出
    AV_EV_WS_FAILED,        // code = av_ws_err_t，http = HTTP 状态
    AV_EV_MSG,              // msg 有效
    AV_EV_OK_PRESS,
    AV_EV_OK_RELEASE,
    AV_EV_OK_LONG,
    AV_EV_OK_CLICK,
    AV_EV_UP_CLICK,
    AV_EV_DOWN_CLICK,
    AV_EV_UP_LONG,
    AV_EV_SEND_FAILED,      // 发送控制消息/音频失败
    AV_EV_RELAY_CHANGED,    // 中继地址改了：断开重连
    AV_EV_SUSPEND,          // 切到其他功能：停麦、断开中继，挂起期间不触发任何连接
    AV_EV_RESUME,           // 回到对讲：挂起解除，需要时立即连接
} av_ev_type_t;

typedef struct {
    av_ev_type_t type;
    uint32_t now_ms;
    int code;
    int http;
    uint32_t gen;               // AV_EV_WS_*：发起该连接时的 ws_gen，过期事件被忽略
    const av_msg_t *msg;
} av_event_t;

enum {
    AV_ACT_SEND_LISTEN_START = 1u << 0,
    AV_ACT_SEND_LISTEN_STOP  = 1u << 1,
    AV_ACT_SEND_ABORT        = 1u << 2,
    AV_ACT_MIC_START         = 1u << 3,
    AV_ACT_MIC_STOP          = 1u << 4,
    AV_ACT_PLAY_INTERRUPT    = 1u << 5,  // 立即清空播放缓冲并丢弃本轮剩余音频
    AV_ACT_WS_CONNECT        = 1u << 6,
    AV_ACT_WS_DISCONNECT     = 1u << 7,
    AV_ACT_GO_HOME           = 1u << 8,  // 对讲请求回首页（由应用层交给 shell 切换）
    AV_ACT_SCROLL_UP         = 1u << 9,
    AV_ACT_SCROLL_DOWN       = 1u << 10,
    AV_ACT_UI_STATUS         = 1u << 11, // 连接状态/错误提示变了
    AV_ACT_UI_VOICE          = 1u << 12, // 语音状态变了
    AV_ACT_UI_ASR            = 1u << 13, // 识别文字变了（asr 字段）
    AV_ACT_UI_REPLY          = 1u << 14, // 回复文字变了（reply_clear / reply_append）
};

typedef struct {
    uint32_t flags;
    bool reply_clear;            // 先清空回复区
    const char *reply_append;    // 再追加这句（指向事件里的 msg->text）
} av_actions_t;

typedef struct {
    av_link_t link;
    av_voice_t voice;
    bool mic_on;
    uint32_t press_ms;           // OK 按下时间
    uint32_t wait_since_ms;      // 进入思考的时间（等待回复超时用）
    bool waiting_reply;
    bool got_reply_this_turn;
    // 重连
    uint32_t retry_at_ms;
    unsigned retry_attempt;
    uint32_t ws_gen;             // 每次 AV_ACT_WS_CONNECT 加一；执行连接时带上它
    bool suspended;              // 挂起（不在前台）：不连接、不处理按键
    // 提示
    char error[96];              // 当前错误提示（UTF-8），空 = 无
    bool error_sticky;           // 不自动消失（如未配网、密码错误）
    uint32_t error_since_ms;
    char asr[AV_MSG_TEXT_MAX];   // 最近一次识别文字（空 + asr_empty = 没听清）
    bool asr_empty;
    int last_wifi_reason;
} av_session_t;

void av_session_init(av_session_t *s);
av_actions_t av_session_handle(av_session_t *s, const av_event_t *ev);

// 下次重连的等待时间：1, 2, 4, 8, 15, 30, 30, ... 秒
uint32_t av_backoff_ms(unsigned attempt);

// 错误文案（纯函数，便于测试与统一措辞）
const char *av_wifi_reason_text(int reason);
void av_ws_error_text(char *buf, size_t size, av_ws_err_t err, int http);
