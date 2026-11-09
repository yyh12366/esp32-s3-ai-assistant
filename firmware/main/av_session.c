#include "av_session.h"

#include <stdio.h>
#include <string.h>

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

uint32_t av_backoff_ms(unsigned attempt)
{
    static const uint32_t steps[] = {1000, 2000, 4000, 8000, 15000, 30000};
    size_t n = sizeof(steps) / sizeof(steps[0]);
    return steps[attempt < n ? attempt : n - 1];
}

const char *av_wifi_reason_text(int reason)
{
    switch (reason) {
    case 2:    // AUTH_EXPIRE
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
        return "Wi-Fi 密码错误，请重新配网";
    case 201:  // NO_AP_FOUND
    case 210:
    case 211:
    case 212:
        return "找不到已保存的 Wi-Fi（仅支持 2.4GHz）";
    case 200:  // BEACON_TIMEOUT
        return "Wi-Fi 信号丢失，正在重连";
    case 203:
    case 205:
        return "Wi-Fi 连接失败，正在重试";
    default:
        return "Wi-Fi 已断开，正在重连";
    }
}

static bool wifi_reason_sticky(int reason)
{
    return reason == 2 || reason == 15 || reason == 202 || reason == 204 ||
           reason == 201 || (reason >= 210 && reason <= 212);
}

void av_ws_error_text(char *buf, size_t size, av_ws_err_t err, int http)
{
    switch (err) {
    case AV_WS_ERR_BAD_URL: snprintf(buf, size, "中继地址无效，请在设置里修改"); break;
    case AV_WS_ERR_DNS: snprintf(buf, size, "无法解析中继地址"); break;
    case AV_WS_ERR_CONNECT: snprintf(buf, size, "连不上中继，请检查地址和中继服务"); break;
    case AV_WS_ERR_HTTP:
        if (http == 401 || http == 403) snprintf(buf, size, "中继拒绝连接：令牌错误 (%d)", http);
        else if (http == 404) snprintf(buf, size, "中继路径不对 (404)");
        else snprintf(buf, size, "中继握手失败 (HTTP %d)", http);
        break;
    case AV_WS_ERR_HANDSHAKE: snprintf(buf, size, "中继握手校验失败"); break;
    case AV_WS_ERR_TIMEOUT: snprintf(buf, size, "中继无响应，连接超时"); break;
    case AV_WS_ERR_CLOSED: snprintf(buf, size, "中继关闭了连接"); break;
    case AV_WS_ERR_PROTOCOL: snprintf(buf, size, "中继数据格式错误"); break;
    case AV_WS_ERR_IO:
    default: snprintf(buf, size, "与中继的连接中断"); break;
    }
}

// 拼接 prefix + text 到 dst，超长时在 UTF-8 字符边界截断（不留半个汉字）。
static void copy_utf8(char *dst, size_t size, const char *prefix, const char *text)
{
    size_t o = 0;
    const char *parts[2] = {prefix, text};
    for (int k = 0; k < 2; k++) {
        for (const char *p = parts[k]; p && *p; p++) {
            if (o + 1 >= size) {
                // 下一个字节是续字节 = 最后一个字符只写了一半：整个去掉
                if (((unsigned char)*p & 0xC0) == 0x80) {
                    while (o > 0 && ((unsigned char)dst[o - 1] & 0xC0) == 0x80) o--;
                    if (o > 0) o--;
                }
                dst[o] = '\0';
                return;
            }
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

static void set_error(av_session_t *s, av_actions_t *a, const char *text, bool sticky, uint32_t now)
{
    copy_utf8(s->error, sizeof(s->error), NULL, text);
    s->error_sticky = sticky;
    s->error_since_ms = now;
    a->flags |= AV_ACT_UI_STATUS;
}

static void clear_error(av_session_t *s, av_actions_t *a)
{
    if (s->error[0]) {
        s->error[0] = '\0';
        s->error_sticky = false;
        a->flags |= AV_ACT_UI_STATUS;
    }
}

static void set_link(av_session_t *s, av_actions_t *a, av_link_t link)
{
    if (s->link != link) {
        s->link = link;
        a->flags |= AV_ACT_UI_STATUS;
    }
}

static void set_voice(av_session_t *s, av_actions_t *a, av_voice_t v)
{
    if (s->voice != v) {
        s->voice = v;
        a->flags |= AV_ACT_UI_VOICE;
    }
}

// 本地结束当前轮：关麦、清空播放，回到待机。不发任何消息（连接已断或即将断开）。
static void end_turn_local(av_session_t *s, av_actions_t *a)
{
    if (s->mic_on) {
        s->mic_on = false;
        a->flags |= AV_ACT_MIC_STOP;
    }
    if (s->voice != AV_VOICE_IDLE || s->waiting_reply) a->flags |= AV_ACT_PLAY_INTERRUPT;
    s->waiting_reply = false;
    set_voice(s, a, AV_VOICE_IDLE);
}

static void connect_now(av_session_t *s, av_actions_t *a)
{
    s->ws_gen++;
    a->flags |= AV_ACT_WS_CONNECT;
    set_link(s, a, AV_LINK_WS_CONNECTING);
}

static void ws_failed(av_session_t *s, av_actions_t *a, av_ws_err_t err, int http, uint32_t now)
{
    end_turn_local(s, a);
    char text[sizeof(s->error)];
    av_ws_error_text(text, sizeof(text), err, http);
    set_error(s, a, text, true, now);
    s->retry_at_ms = now + av_backoff_ms(s->retry_attempt);
    if (s->retry_attempt < 100) s->retry_attempt++;
    set_link(s, a, AV_LINK_RETRY_WAIT);
}

static void start_listening(av_session_t *s, av_actions_t *a, uint32_t now)
{
    if (s->voice != AV_VOICE_IDLE || s->waiting_reply) a->flags |= AV_ACT_PLAY_INTERRUPT;
    a->flags |= AV_ACT_SEND_LISTEN_START | AV_ACT_MIC_START | AV_ACT_UI_ASR | AV_ACT_UI_REPLY;
    a->reply_clear = true;
    s->mic_on = true;
    s->press_ms = now;
    s->waiting_reply = false;
    s->got_reply_this_turn = false;
    s->asr[0] = '\0';
    s->asr_empty = false;
    set_voice(s, a, AV_VOICE_LISTENING);
}

static void stop_listening(av_session_t *s, av_actions_t *a, uint32_t now)
{
    s->mic_on = false;
    a->flags |= AV_ACT_MIC_STOP;
    if ((uint32_t)(now - s->press_ms) < AV_TAP_MS) {
        a->flags |= AV_ACT_SEND_ABORT;
        s->waiting_reply = false;
        set_voice(s, a, AV_VOICE_IDLE);
    } else {
        a->flags |= AV_ACT_SEND_LISTEN_STOP;
        s->waiting_reply = true;
        s->wait_since_ms = now;
        set_voice(s, a, AV_VOICE_THINKING);
    }
}

static void handle_msg(av_session_t *s, av_actions_t *a, const av_msg_t *m, uint32_t now)
{
    if (s->link != AV_LINK_ONLINE) return;
    if (s->waiting_reply) s->wait_since_ms = now;  // 有进展就重新计时
    switch (m->type) {
    case AV_MSG_STATE:
        // 录音中（包括打断后刚开始的新一轮），上一轮遗留的状态消息不改变界面
        if (!m->state_known || s->mic_on) break;
        if (m->state == AV_VOICE_IDLE) s->waiting_reply = false;
        if (m->state == AV_VOICE_LISTENING) break;  // 只会跟在我们的 listen_start 之后
        set_voice(s, a, m->state);
        break;
    case AV_MSG_ASR:
        if (s->mic_on) break;
        copy_utf8(s->asr, sizeof(s->asr), NULL, m->text);
        s->asr_empty = m->text[0] == '\0';
        a->flags |= AV_ACT_UI_ASR;
        break;
    case AV_MSG_TTS_START:
        if (!s->mic_on) set_voice(s, a, AV_VOICE_SPEAKING);
        break;
    case AV_MSG_REPLY:
        if (s->mic_on || m->final || m->text[0] == '\0') break;
        if (!s->got_reply_this_turn) {
            a->reply_clear = true;
            s->got_reply_this_turn = true;
        }
        a->reply_append = m->text;
        a->flags |= AV_ACT_UI_REPLY;
        break;
    case AV_MSG_ERROR: {
        char text[sizeof(s->error)];
        copy_utf8(text, sizeof(text), "出错：", m->text[0] ? m->text : "未知错误");
        set_error(s, a, text, false, now);
        break;
    }
    case AV_MSG_TTS_END:
    case AV_MSG_UNKNOWN:
    default:
        break;
    }
}

void av_session_init(av_session_t *s)
{
    memset(s, 0, sizeof(*s));
    s->link = AV_LINK_WIFI_CONNECTING;
    s->voice = AV_VOICE_IDLE;
}

av_actions_t av_session_handle(av_session_t *s, const av_event_t *ev)
{
    av_actions_t a = {0};
    const uint32_t now = ev->now_ms;

    switch (ev->type) {
    case AV_EV_TICK:
        if (s->mic_on && (uint32_t)(now - s->press_ms) >= AV_MAX_LISTEN_MS) stop_listening(s, &a, now);
        if (s->waiting_reply && (uint32_t)(now - s->wait_since_ms) >= AV_REPLY_TIMEOUT_MS) {
            s->waiting_reply = false;
            a.flags |= AV_ACT_SEND_ABORT | AV_ACT_PLAY_INTERRUPT;
            set_voice(s, &a, AV_VOICE_IDLE);
            set_error(s, &a, "等待回复超时，请再试一次", false, now);
        }
        if (s->link == AV_LINK_RETRY_WAIT && !s->suspended && reached(now, s->retry_at_ms)) connect_now(s, &a);
        if (s->error[0] && !s->error_sticky && (uint32_t)(now - s->error_since_ms) >= AV_ERROR_SHOW_MS) {
            clear_error(s, &a);
        }
        break;

    case AV_EV_WIFI_NO_CONFIG:
        if (s->link == AV_LINK_PROVISIONING) break;
        end_turn_local(s, &a);
        if (s->link == AV_LINK_ONLINE || s->link == AV_LINK_WS_CONNECTING) a.flags |= AV_ACT_WS_DISCONNECT;
        set_link(s, &a, AV_LINK_NO_CONFIG);
        set_error(s, &a, "未配网：返回首页进入「设置」，选择蓝牙配网", true, now);
        break;

    case AV_EV_WIFI_CONNECTING:
        if (s->link == AV_LINK_PROVISIONING) break;
        set_link(s, &a, AV_LINK_WIFI_CONNECTING);
        break;

    case AV_EV_WIFI_UP:
        s->last_wifi_reason = 0;
        if (s->link == AV_LINK_PROVISIONING) break;
        clear_error(s, &a);
        if (s->link != AV_LINK_ONLINE && s->link != AV_LINK_WS_CONNECTING) {
            s->retry_attempt = 0;
            if (s->suspended) {
                // 不在前台：只记下“可以连了”，等 AV_EV_RESUME 再真正连接
                set_link(s, &a, AV_LINK_RETRY_WAIT);
                s->retry_at_ms = now;
            } else {
                connect_now(s, &a);
            }
        }
        break;

    case AV_EV_WIFI_DOWN:
        s->last_wifi_reason = ev->code;
        if (s->link == AV_LINK_PROVISIONING) break;
        end_turn_local(s, &a);
        if (s->link == AV_LINK_ONLINE || s->link == AV_LINK_WS_CONNECTING) a.flags |= AV_ACT_WS_DISCONNECT;
        set_link(s, &a, AV_LINK_WIFI_CONNECTING);
        set_error(s, &a, av_wifi_reason_text(ev->code), wifi_reason_sticky(ev->code), now);
        break;

    case AV_EV_PROVISION_START:
        end_turn_local(s, &a);
        if (s->link == AV_LINK_ONLINE || s->link == AV_LINK_WS_CONNECTING) a.flags |= AV_ACT_WS_DISCONNECT;
        clear_error(s, &a);
        set_link(s, &a, AV_LINK_PROVISIONING);
        break;

    case AV_EV_PROVISION_STOP:
        if (s->link == AV_LINK_PROVISIONING) set_link(s, &a, AV_LINK_WIFI_CONNECTING);
        break;

    case AV_EV_WS_CONNECTED:
        if (ev->gen != s->ws_gen || s->link != AV_LINK_WS_CONNECTING) break;
        s->retry_attempt = 0;
        clear_error(s, &a);
        set_link(s, &a, AV_LINK_ONLINE);
        set_voice(s, &a, AV_VOICE_IDLE);
        break;

    case AV_EV_WS_FAILED:
        if (ev->gen != s->ws_gen) break;
        if (s->link != AV_LINK_ONLINE && s->link != AV_LINK_WS_CONNECTING) break;
        ws_failed(s, &a, (av_ws_err_t)ev->code, ev->http, now);
        break;

    case AV_EV_SEND_FAILED:
        if (s->link != AV_LINK_ONLINE) break;
        a.flags |= AV_ACT_WS_DISCONNECT;
        ws_failed(s, &a, AV_WS_ERR_IO, 0, now);
        break;

    case AV_EV_RELAY_CHANGED:
        if (s->suspended) {
            // 挂起中改地址：下次 RESUME 用新地址连接（RESUME 只在 RETRY_WAIT 时连接）
            if (s->link == AV_LINK_RETRY_WAIT) {
                s->retry_attempt = 0;
                s->retry_at_ms = now;
            }
            break;
        }
        if (s->link == AV_LINK_ONLINE || s->link == AV_LINK_WS_CONNECTING ||
            s->link == AV_LINK_RETRY_WAIT) {
            end_turn_local(s, &a);
            a.flags |= AV_ACT_WS_DISCONNECT;
            clear_error(s, &a);
            s->retry_attempt = 0;
            connect_now(s, &a);
        }
        break;

    case AV_EV_MSG:
        if (ev->msg) handle_msg(s, &a, ev->msg, now);
        break;

    case AV_EV_OK_PRESS:
        if (s->link != AV_LINK_ONLINE || s->mic_on) break;
        start_listening(s, &a, now);
        break;

    case AV_EV_OK_RELEASE:
        if (s->mic_on) stop_listening(s, &a, now);
        break;

    case AV_EV_OK_LONG:
        if (s->link != AV_LINK_ONLINE && !s->mic_on) a.flags |= AV_ACT_GO_HOME;
        break;

    case AV_EV_OK_CLICK:
        if (s->link == AV_LINK_RETRY_WAIT && !s->suspended) connect_now(s, &a);
        break;

    case AV_EV_UP_LONG:
        if (s->mic_on) break;
        if (s->link == AV_LINK_ONLINE && (s->voice != AV_VOICE_IDLE || s->waiting_reply)) {
            a.flags |= AV_ACT_SEND_ABORT;  // 回首页前结束正在进行的回复
            end_turn_local(s, &a);
        }
        a.flags |= AV_ACT_GO_HOME;
        break;

    case AV_EV_SUSPEND:
        if (s->suspended) break;
        s->suspended = true;
        s->ws_gen++;                 // 让挂起前发起的连接事件全部过期
        end_turn_local(s, &a);       // 停麦、清播放（不发消息）
        a.flags |= AV_ACT_PLAY_INTERRUPT;
        if (s->link == AV_LINK_ONLINE || s->link == AV_LINK_WS_CONNECTING) {
            a.flags |= AV_ACT_WS_DISCONNECT;
            s->retry_attempt = 0;
            s->retry_at_ms = now;
            set_link(s, &a, AV_LINK_RETRY_WAIT);
        }
        break;

    case AV_EV_RESUME:
        if (!s->suspended) break;
        s->suspended = false;
        if (s->link == AV_LINK_RETRY_WAIT) {
            s->retry_attempt = 0;
            connect_now(s, &a);
        }
        break;

    case AV_EV_UP_CLICK:
        a.flags |= AV_ACT_SCROLL_UP;
        break;

    case AV_EV_DOWN_CLICK:
        a.flags |= AV_ACT_SCROLL_DOWN;
        break;
    }
    return a;
}
