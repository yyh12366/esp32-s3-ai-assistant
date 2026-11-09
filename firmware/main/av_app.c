#include "av_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "av_audio.h"
#include "av_net.h"
#include "av_store.h"
#include "av_text.h"
#include "av_ui.h"
#include "av_url.h"
#include "av_wifi.h"

static const char *TAG = "av_app";

#define QUEUE_DEPTH        24
#define TICK_MS            100
#define BATTERY_PERIOD_MS  30000

typedef enum { Q_SESSION, Q_KEY, Q_RELAY, Q_USE_RELAY, Q_CMD } q_kind_t;

// Q_CMD 的 code 取值：前台/挂起切换与配网收尾都在应用任务里串行执行。
enum { CMD_RESUME, CMD_SUSPEND, CMD_PROV_BEGIN, CMD_PROV_DONE };

typedef struct {
    uint8_t kind;
    uint8_t btn, bev;
    av_ev_type_t type;
    int code, http;
    uint32_t gen;
    av_msg_t *msg;
    char *url;
} q_item_t;

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;
static volatile bool s_started;     // av_app_start 成功
static volatile bool s_active;      // 对讲在前台：才画界面、才接收按键
static void (*s_home_cb)(void);

static av_session_t s_sess;
static av_textbuf_t s_reply;
static char s_device_id[32];
static char s_relay[AV_URL_MAX];   // 当前使用的中继地址（la 时带 token，仅内存中）
static int s_battery = -1;
static uint32_t s_ui_dirty;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// 去掉 ?query（含 token），用于日志
static void relay_for_display(char *buf, size_t size, const char *url)
{
    snprintf(buf, size, "%s", url);
    char *q = strchr(buf, '?');
    if (q) *q = '\0';
}

// ---------------------------------------------------------------------------
// 投递（任何任务 / 回调）
// ---------------------------------------------------------------------------
static bool post(const q_item_t *item)
{
    if (!s_queue || xQueueSend(s_queue, item, 0) != pdTRUE) {
        s_dropped++;
        return false;
    }
    return true;
}

static void post_cmd(int cmd)
{
    q_item_t q = {.kind = Q_CMD, .code = cmd};
    post(&q);
}

void av_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    q_item_t q = {.kind = Q_KEY, .btn = (uint8_t)btn, .bev = (uint8_t)ev};
    post(&q);
}

void av_app_post_simple(av_ev_type_t type)
{
    q_item_t q = {.kind = Q_SESSION, .type = type};
    post(&q);
}

void av_app_post_wifi(av_ev_type_t type, int reason)
{
    q_item_t q = {.kind = Q_SESSION, .type = type, .code = reason};
    post(&q);
}

void av_app_post_ws(av_ev_type_t type, int err, int http, uint32_t gen)
{
    q_item_t q = {.kind = Q_SESSION, .type = type, .code = err, .http = http, .gen = gen};
    post(&q);
}

void av_app_post_msg(av_msg_t *msg)
{
    q_item_t q = {.kind = Q_SESSION, .type = AV_EV_MSG, .msg = msg};
    if (!post(&q)) free(msg);
}

// BLUFI 自定义数据 "relay=..."：保存为本机中继地址，服务器回到「本机」。
// 对讲未启动时只写 NVS（启动时会读取），不需要队列。
static void save_relay_from_ble(const char *url)
{
    av_url_t parsed;
    if (!av_url_parse(url, &parsed)) return;
    esp_err_t err = av_store_set_relay(url);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存中继地址失败: %s", esp_err_to_name(err));
    if (av_store_get_server() != AV_SERVER_LOCAL) av_store_set_server(AV_SERVER_LOCAL);
    ESP_LOGI(TAG, "中继地址改为 %s:%u", parsed.host, (unsigned)parsed.port);
}

void av_app_post_relay(char *url)
{
    if (!s_started) {
        save_relay_from_ble(url);
        free(url);
        return;
    }
    q_item_t q = {.kind = Q_RELAY, .url = url};
    if (!post(&q)) free(url);
}

void av_app_use_relay(const char *url)
{
    const size_t n = strlen(url) + 1;
    char *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, url, n);
    q_item_t q = {.kind = Q_USE_RELAY, .url = copy};
    if (!post(&q)) free(copy);
}

void av_app_provision_begin(void) { post_cmd(CMD_PROV_BEGIN); }
void av_app_provision_done(void) { post_cmd(CMD_PROV_DONE); }

// 生命周期切换只改标志位（立即生效）；挂起/恢复的会话动作排在队列里按顺序执行。
void av_app_set_home_cb(void (*cb)(void)) { s_home_cb = cb; }

void av_app_enter(void)
{
    if (!s_started) return;
    s_active = true;
    post_cmd(CMD_RESUME);
}

void av_app_hide(void) { s_active = false; }

void av_app_exit(void)
{
    s_active = false;
    post_cmd(CMD_SUSPEND);
}

// ---------------------------------------------------------------------------
// 主界面刷新（只在前台画）
// ---------------------------------------------------------------------------
static void sanitize(char *dst, size_t size, const char *src)
{
    av_text_sanitize(dst, size, src, strlen(src), av_ui_has_glyph, NULL);
}

static void link_text(char *buf, size_t size, uint32_t *color)
{
    switch (s_sess.link) {
    case AV_LINK_NO_CONFIG: snprintf(buf, size, "未配网"); *color = AV_UI_RED; break;
    case AV_LINK_PROVISIONING: snprintf(buf, size, "蓝牙配网中"); *color = AV_UI_BLUE; break;
    case AV_LINK_WIFI_CONNECTING: snprintf(buf, size, "连接 Wi-Fi…"); *color = AV_UI_AMBER; break;
    case AV_LINK_WS_CONNECTING: snprintf(buf, size, "连接中继…"); *color = AV_UI_AMBER; break;
    case AV_LINK_ONLINE: snprintf(buf, size, "在线"); *color = AV_UI_GREEN; break;
    case AV_LINK_RETRY_WAIT: {
        int32_t left = (int32_t)(s_sess.retry_at_ms - now_ms());
        unsigned sec = left > 0 ? (unsigned)(left + 999) / 1000 : 0;
        snprintf(buf, size, "离线 · %us 后重试", sec);
        *color = AV_UI_RED;
        break;
    }
    }
}

static void flush_ui(void)
{
    if (!s_ui_dirty) return;
    if (!bsp_lvgl_lock(200)) return;  // 下个 tick 再试
    uint32_t d = s_ui_dirty;
    s_ui_dirty = 0;
    if (d & AV_ACT_UI_STATUS) {
        char text[48], err[sizeof(s_sess.error) * 2];
        uint32_t color;
        link_text(text, sizeof(text), &color);
        sanitize(err, sizeof(err), s_sess.error);
        av_ui_main_status(text, color, err, s_battery);
    }
    if (d & AV_ACT_UI_VOICE) av_ui_main_voice(s_sess.voice);
    if (d & AV_ACT_UI_ASR) {
        if (s_sess.mic_on) {
            av_ui_main_asr("正在听…", true);
        } else if (s_sess.asr_empty) {
            av_ui_main_asr("没听清，请再说一次", true);
        } else if (s_sess.asr[0]) {
            static char asr[AV_MSG_TEXT_MAX];
            sanitize(asr, sizeof(asr), s_sess.asr);
            av_ui_main_asr(asr, false);
        } else {
            av_ui_main_asr("识别中…", true);
        }
    }
    if (d & AV_ACT_UI_REPLY) av_ui_main_reply(s_reply.buf);
    if (d & AV_ACT_SCROLL_UP) av_ui_main_scroll(-1);
    if (d & AV_ACT_SCROLL_DOWN) av_ui_main_scroll(1);
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------------------
// 状态机动作（只在应用任务里调用）
// ---------------------------------------------------------------------------
static void handle_session(const av_event_t *ev);

static void session_simple(av_ev_type_t type)
{
    av_event_t ev = {.type = type, .now_ms = now_ms()};
    handle_session(&ev);
}

// 只切换当前使用的中继地址并重连，不写 NVS（保存由调用方决定）。
static void use_relay(const char *url)
{
    snprintf(s_relay, sizeof(s_relay), "%s", url);
    session_simple(AV_EV_RELAY_CHANGED);
}

// BLUFI 自定义数据（应用任务里）：保存并切换。
static void apply_relay_from_ble(const char *url)
{
    save_relay_from_ble(url);
    use_relay(url);
}

static void send_json(int (*build)(char *, size_t))
{
    char buf[48];
    int n = build(buf, sizeof(buf));
    if (n > 0 && av_net_send_text(buf, (size_t)n) != ESP_OK) av_app_post_simple(AV_EV_SEND_FAILED);
}

static void run_actions(const av_actions_t *a)
{
    const uint32_t f = a->flags;
    if (f & AV_ACT_MIC_STOP) av_audio_mic_stop();          // 先停麦，保证 listen_stop 之后没有音频
    if (f & AV_ACT_PLAY_INTERRUPT) av_audio_interrupt();   // 先关闸，再告诉中继
    if (f & AV_ACT_SEND_ABORT) send_json(av_proto_abort);
    if (f & AV_ACT_SEND_LISTEN_STOP) send_json(av_proto_listen_stop);
    if (f & AV_ACT_SEND_LISTEN_START) send_json(av_proto_listen_start);
    if (f & AV_ACT_MIC_START) av_audio_mic_start();
    if (f & AV_ACT_WS_DISCONNECT) av_net_disconnect();
    if (f & AV_ACT_WS_CONNECT) av_net_connect(s_relay, s_device_id, s_sess.ws_gen);

    if (a->reply_clear) av_textbuf_clear(&s_reply);
    if (a->reply_append) {
        static char clean[AV_MSG_TEXT_MAX];
        sanitize(clean, sizeof(clean), a->reply_append);
        av_textbuf_append(&s_reply, clean);
    }
    s_ui_dirty |= f & (AV_ACT_UI_STATUS | AV_ACT_UI_VOICE | AV_ACT_UI_ASR | AV_ACT_UI_REPLY |
                       AV_ACT_SCROLL_UP | AV_ACT_SCROLL_DOWN);
    // 回首页由 shell 切换界面；这里只投递请求（回调必须非阻塞）
    if ((f & AV_ACT_GO_HOME) && s_home_cb) s_home_cb();
}

static void handle_session(const av_event_t *ev)
{
    av_actions_t a = av_session_handle(&s_sess, ev);
    run_actions(&a);
}

// 挂起/恢复前先同步 Wi-Fi 状态：挂起期间的 Wi-Fi 事件会被丢弃（队列不存在时）或只被记下。
static void sync_wifi(void)
{
    if (av_wifi_connected()) session_simple(AV_EV_WIFI_UP);
    else if (!av_wifi_has_config()) session_simple(AV_EV_WIFI_NO_CONFIG);
    else session_simple(AV_EV_WIFI_CONNECTING);
}

static void run_cmd(int cmd)
{
    switch (cmd) {
    case CMD_RESUME:
        sync_wifi();                       // 仍挂起：只记下“可以连了”
        session_simple(AV_EV_RESUME);      // 解除挂起，需要时连接
        if (bsp_lvgl_lock(500)) {
            av_ui_main_show();
            bsp_lvgl_unlock();
        }
        s_ui_dirty |= AV_ACT_UI_STATUS | AV_ACT_UI_VOICE | AV_ACT_UI_ASR | AV_ACT_UI_REPLY;
        break;
    case CMD_SUSPEND:
        session_simple(AV_EV_SUSPEND);
        break;
    case CMD_PROV_BEGIN:
        session_simple(AV_EV_PROVISION_START);
        break;
    case CMD_PROV_DONE:
        session_simple(AV_EV_PROVISION_STOP);
        sync_wifi();
        break;
    default:
        break;
    }
}

static void main_key(bsp_btn_t btn, bsp_btn_ev_t bev)
{
    av_ev_type_t t;
    if (btn == BSP_BTN_OK) {
        if (bev == BSP_BTN_PRESS) t = AV_EV_OK_PRESS;
        else if (bev == BSP_BTN_RELEASE) t = AV_EV_OK_RELEASE;
        else if (bev == BSP_BTN_LONG) t = AV_EV_OK_LONG;
        else if (bev == BSP_BTN_CLICK) t = AV_EV_OK_CLICK;
        else return;
    } else if (btn == BSP_BTN_UP) {
        if (bev == BSP_BTN_CLICK) t = AV_EV_UP_CLICK;
        else if (bev == BSP_BTN_LONG) t = AV_EV_UP_LONG;
        else return;
    } else {
        if (bev == BSP_BTN_CLICK || bev == BSP_BTN_LONG) t = AV_EV_DOWN_CLICK;
        else return;
    }
    session_simple(t);
}

static void tick(uint32_t now)
{
    static uint32_t last_battery, last_second;
    av_event_t ev = {.type = AV_EV_TICK, .now_ms = now};
    handle_session(&ev);

    if (now - last_battery >= BATTERY_PERIOD_MS || last_battery == 0) {
        last_battery = now ? now : 1;
        // 真机验收用：长期观察内部堆余量与最大连续块（无 PSRAM）
        ESP_LOGI(TAG, "heap free=%u largest=%u min=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        int soc = bsp_battery_soc();
        if (soc != s_battery) {
            s_battery = soc;
            s_ui_dirty |= AV_ACT_UI_STATUS;
        }
    }
    if (s_sess.link == AV_LINK_RETRY_WAIT && now - last_second >= 1000) {
        last_second = now;
        s_ui_dirty |= AV_ACT_UI_STATUS;  // 重试倒计时
    }
    if (s_dropped) {
        ESP_LOGW(TAG, "事件队列满，丢弃 %u 个事件", (unsigned)s_dropped);
        s_dropped = 0;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    uint32_t last_tick = now_ms();
    for (;;) {
        q_item_t q;
        if (xQueueReceive(s_queue, &q, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            switch (q.kind) {
            case Q_KEY:
                if (s_active) main_key(q.btn, q.bev);
                break;
            case Q_RELAY:
                apply_relay_from_ble(q.url);
                free(q.url);
                break;
            case Q_USE_RELAY:
                use_relay(q.url);
                free(q.url);
                break;
            case Q_CMD:
                run_cmd(q.code);
                break;
            case Q_SESSION:
            default: {
                av_event_t ev = {.type = q.type, .now_ms = now_ms(), .code = q.code, .http = q.http,
                                 .gen = q.gen, .msg = q.msg};
                handle_session(&ev);
                free(q.msg);
                break;
            }
            }
        }
        uint32_t now = now_ms();
        if (now - last_tick >= TICK_MS) {
            last_tick = now;
            tick(now);
        }
        if (s_active) flush_ui();
    }
}

esp_err_t av_app_start(const char *startup_error)
{
    if (s_started) return ESP_OK;
    av_session_init(&s_sess);
    av_textbuf_clear(&s_reply);
    av_store_device_id(s_device_id, sizeof(s_device_id));
    av_store_get_relay(s_relay, sizeof(s_relay));
    char shown[AV_URL_MAX];
    relay_for_display(shown, sizeof(shown), s_relay);  // 日志不打 token
    ESP_LOGI(TAG, "设备 %s，服务器 %s，中继 %s", s_device_id,
             av_store_get_server() == AV_SERVER_LA ? "la" : "本机", shown);

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(q_item_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (startup_error) {
        snprintf(s_sess.error, sizeof(s_sess.error), "%s", startup_error);
        s_sess.error_sticky = true;
    }
    esp_err_t err = av_net_init();
    if (err != ESP_OK) return err;
    if (xTaskCreate(app_task, "av_app", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_ui_dirty = AV_ACT_UI_STATUS | AV_ACT_UI_VOICE;
    s_started = true;
    return ESP_OK;
}
