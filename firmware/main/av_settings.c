// main/av_settings.c —— 设置页（原 av_app.c 的设置部分，行为保持不变）。
//
// 子页面（中继地址、配网、清除 Wi-Fi、设备信息）退出后回到设置根菜单；
// 根菜单的「返回」或长按 OK 才回首页。
#include "av_settings.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "av_app.h"
#include "av_audio.h"
#include "av_ipedit.h"
#include "av_media.h"
#include "av_ota.h"
#include "av_store.h"
#include "av_text.h"
#include "av_ui.h"
#include "av_url.h"
#include "av_wifi.h"

static const char *TAG = "av_settings";

#define PROV_TIMEOUT_MS    (5 * 60 * 1000)
#define PROV_DONE_EXIT_MS  4000
#define VOLUME_MIN         30
#define OTA_REFRESH_MS     500   // 下载进度界面刷新间隔

typedef enum { SCR_MENU, SCR_IPEDIT, SCR_PROV, SCR_CONFIRM, SCR_INFO, SCR_OTA } screen_t;

enum { MENU_RELAY, MENU_SERVER, MENU_PROVISION, MENU_VOLUME, MENU_CLEAR_WIFI, MENU_OTA, MENU_INFO, MENU_BACK, MENU_COUNT };

static screen_t s_screen = SCR_MENU;
static int s_menu_sel;
static av_ipedit_t s_edit;
static uint32_t s_prov_since, s_prov_done_at;
static av_prov_state_t s_prov_shown;
static uint32_t s_ota_rev, s_ota_shown_ms;   // 固件升级页最后绘制的状态版本与时间

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void sanitize(char *dst, size_t size, const char *src)
{
    av_text_sanitize(dst, size, src, strlen(src), av_ui_has_glyph, NULL);
}

// 去掉 ?query（含 token），用于显示和日志
static void relay_for_display(char *buf, size_t size, const char *url)
{
    snprintf(buf, size, "%s", url);
    char *q = strchr(buf, '?');
    if (q) *q = '\0';
}

static const char *server_name(uint8_t server) { return server == AV_SERVER_LA ? "la" : "本机"; }

static void show_menu(void)
{
    static const char *const labels[MENU_COUNT] = {"中继地址", "服务器", "蓝牙配网", "音量", "清除 Wi-Fi", "固件升级", "设备信息", "返回"};
    static char ota_ver[16];  // 静态：设置在 shell 任务里运行，省栈
    char relay[AV_URL_HOST_MAX + 8] = "", vol[8], cur[AV_URL_MAX];
    av_url_t url;
    av_store_get_relay(cur, sizeof(cur));
    if (av_url_parse(cur, &url)) snprintf(relay, sizeof(relay), "%s:%u", url.host, (unsigned)url.port);
    snprintf(vol, sizeof(vol), "%u%%", (unsigned)av_store_get_volume());
    snprintf(ota_ver, sizeof(ota_ver), "%.12s", esp_app_get_description()->version);
    const char *values[MENU_COUNT] = {relay, server_name(av_store_get_server()), "", vol, "", ota_ver, "", ""};
    if (!bsp_lvgl_lock(500)) return;
    av_ui_menu_show_dense("设置", labels, values, MENU_COUNT, s_menu_sel, "长按 OK 返回");
    bsp_lvgl_unlock();
}

static void show_ipedit(void)
{
    if (!bsp_lvgl_lock(500)) return;
    av_ui_ipedit_show(&s_edit);
    bsp_lvgl_unlock();
}

static void show_prov(void)
{
    static const char body[] =
        "1. 微信打开小程序\n「蓝牙配网-FoloToy AI PASSPORT」\n"
        "2. 选择设备 " AV_BLUFI_NAME "\n"
        "3. 选择 2.4GHz Wi-Fi，输入密码\n\n"
        "也可用自定义数据发送\nrelay=ws://IP:端口/ws";
    char status[96];
    uint32_t color = AV_UI_AMBER;
    av_prov_state_t st = av_wifi_provision_state();
    s_prov_shown = st;
    switch (st) {
    case AV_PROV_STARTING: snprintf(status, sizeof(status), "正在启动蓝牙…"); break;
    case AV_PROV_ADVERTISING: snprintf(status, sizeof(status), "等待手机连接"); color = AV_UI_BLUE; break;
    case AV_PROV_PHONE_CONNECTED: snprintf(status, sizeof(status), "手机已连接，请发送 Wi-Fi 信息"); color = AV_UI_BLUE; break;
    case AV_PROV_WIFI_CONNECTING: snprintf(status, sizeof(status), "正在连接 Wi-Fi…"); break;
    case AV_PROV_WIFI_CONNECTED: {
        char ssid[33], ip[16];
        av_wifi_info(ssid, sizeof(ssid), ip, sizeof(ip));
        char raw[96];
        snprintf(raw, sizeof(raw), "√ 已连接 %s", ssid);
        sanitize(status, sizeof(status), raw);
        color = AV_UI_GREEN;
        break;
    }
    case AV_PROV_WIFI_FAILED:
        snprintf(status, sizeof(status), "%s", av_wifi_reason_text(av_wifi_provision_reason()));
        color = AV_UI_RED;
        break;
    case AV_PROV_ERROR: snprintf(status, sizeof(status), "蓝牙启动失败，请重启后再试"); color = AV_UI_RED; break;
    case AV_PROV_OFF:
    default: status[0] = '\0'; break;
    }
    if (!bsp_lvgl_lock(500)) return;
    av_ui_page_show("蓝牙配网", body, status, color, "长按 OK 退出");
    bsp_lvgl_unlock();
}

static void show_confirm(void)
{
    if (!bsp_lvgl_lock(500)) return;
    av_ui_page_show("清除 Wi-Fi", "清除已保存的 Wi-Fi 名称和密码？\n\n清除后需要重新蓝牙配网。",
                    "OK 确认清除", AV_UI_RED, "长按 OK 取消");
    bsp_lvgl_unlock();
}

// 固件升级页：按状态给出正文、状态行颜色和底部提示。静态缓冲，见 show_info。
static void show_ota(const av_ota_status_t *st)
{
    static char body[2 * AV_OTA_VERSION_MAX + 40];  // 两个版本标识 + 前缀文字
    static char status[72];
    const char *cur = st->current, *latest = st->latest[0] ? st->latest : "-";
    const char *footer = "长按 OK 返回";
    uint32_t color = AV_UI_AMBER;
    status[0] = '\0';
    s_ota_rev = st->revision;
    s_ota_shown_ms = now_ms();
    switch (st->state) {
    case AV_OTA_LATEST:
        snprintf(body, sizeof(body), "当前  %s\n最新  %s", cur, latest);
        snprintf(status, sizeof(status), "已是最新");
        color = AV_UI_GREEN;
        break;
    case AV_OTA_AVAILABLE:
        snprintf(body, sizeof(body), "当前  %s\n最新  %s", cur, latest);
        snprintf(status, sizeof(status), "发现新版本");
        footer = "OK 升级  长按取消";
        break;
    case AV_OTA_DOWNLOADING:
        snprintf(body, sizeof(body), "正在下载并写入新固件\n完成后自动重启");
        snprintf(status, sizeof(status), "升级中 %u%%", (unsigned)st->percent);
        color = AV_UI_BLUE;
        footer = "升级中请勿断电";
        break;
    case AV_OTA_DONE:
        snprintf(body, sizeof(body), "固件已写入并校验通过");
        snprintf(status, sizeof(status), "升级完成，重启中");
        color = AV_UI_GREEN;
        footer = "请勿断电";
        break;
    case AV_OTA_ERROR:
        snprintf(body, sizeof(body), "当前  %s", cur);
        snprintf(status, sizeof(status), "%s", st->error);
        color = AV_UI_RED;
        footer = "OK 重试  长按返回";
        break;
    case AV_OTA_CHECKING:
    case AV_OTA_IDLE:
    default:
        snprintf(body, sizeof(body), "正在检查更新…\n\n当前  %s", cur);
        break;
    }
    if (!bsp_lvgl_lock(500)) return;
    av_ui_page_show("固件升级", body, status, color, footer);
    bsp_lvgl_unlock();
}

// 进入升级页：取当前媒体基址和 token，后台检查更新（已在检查或下载时只显示当前状态）
static void ota_start_check(void)
{
    static char base[AV_MEDIA_BASE_MAX], relay[AV_URL_MAX], token[AV_MEDIA_TOKEN_MAX];
    static av_ota_status_t st;
    av_store_get_media_base(base, sizeof(base));
    av_store_get_relay(relay, sizeof(relay));
    if (!av_url_query_param(relay, "token", token, sizeof(token))) token[0] = '\0';
    av_ota_check(base, token);  // ESP_ERR_INVALID_STATE 表示已在运行，直接显示现状
    av_ota_get_status(&st);
    show_ota(&st);
}

static void show_info(void)
{
    // 静态缓冲：设置只在 shell 任务里运行，省栈
    static char ssid[33], ip[16], id[32], cur[AV_URL_MAX], relay[AV_URL_MAX], raw[400], body[400];
    av_wifi_info(ssid, sizeof(ssid), ip, sizeof(ip));
    av_store_device_id(id, sizeof(id));
    av_store_get_relay(cur, sizeof(cur));
    relay_for_display(relay, sizeof(relay), cur);  // 不显示 token
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(raw, sizeof(raw), "设备 ID  %s\nWi-Fi  %s\nIP  %s\n服务器  %s\n中继\n%s\n固件  %s\n可用内存  %u KB",
             id, ssid[0] ? ssid : "未配置", ip[0] ? ip : "-", server_name(av_store_get_server()), relay, app->version,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    sanitize(body, sizeof(body), raw);
    if (!bsp_lvgl_lock(500)) return;
    av_ui_page_show("设备信息", body, NULL, 0, "按 OK 返回");
    bsp_lvgl_unlock();
}

// 手动输入的地址保存为本机中继地址；输入的是局域网地址，所以服务器回到「本机」。
static void apply_relay(const char *url)
{
    av_url_t parsed;
    if (!av_url_parse(url, &parsed)) return;
    esp_err_t err = av_store_set_relay(url);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存中继地址失败: %s", esp_err_to_name(err));
    if (av_store_get_server() != AV_SERVER_LOCAL) av_store_set_server(AV_SERVER_LOCAL);
    ESP_LOGI(TAG, "中继地址改为 %s:%u", parsed.host, (unsigned)parsed.port);
    av_app_use_relay(url);
}

// 在「本机」和「la」之间切换：只改当前地址，本机保存的中继地址保持不变。
static bool toggle_server(void)
{
    const uint8_t next = av_store_get_server() == AV_SERVER_LA ? AV_SERVER_LOCAL : AV_SERVER_LA;
    if (next == AV_SERVER_LA) {
        av_url_t remote_url;
        if (strlen(CONFIG_AV_LA_RELAY_URL) >= AV_URL_MAX ||
            !av_url_parse(CONFIG_AV_LA_RELAY_URL, &remote_url)) {
            ESP_LOGW(TAG, "远端服务器地址未配置，保留当前服务器");
            if (bsp_lvgl_lock(500)) {
                av_ui_page_show("远端服务器未配置", "请先在本地 menuconfig 中填写有效的远端中继 URL。",
                                NULL, AV_UI_AMBER, "长按 OK 返回");
                bsp_lvgl_unlock();
            }
            return false;
        }
    }
    if (av_store_set_server(next) != ESP_OK) {
        ESP_LOGE(TAG, "保存服务器选择失败");
        return true;
    }
    char relay[AV_URL_MAX];
    av_store_get_relay(relay, sizeof(relay));
    char shown[AV_URL_MAX];
    relay_for_display(shown, sizeof(shown), relay);
    ESP_LOGI(TAG, "服务器切换为 %s，中继 %s", server_name(next), shown);
    av_app_use_relay(relay);
    return true;
}

static void provision_exit(void)
{
    av_wifi_provision_stop();          // 先放掉 BLE，堆回来了再取回播放缓冲
    av_audio_restore_buffer();
    av_app_provision_done();           // 对讲按当前 Wi-Fi 状态恢复（未前台时只记下）
    s_screen = SCR_MENU;
    show_menu();
}

void av_settings_enter(void)
{
    s_screen = SCR_MENU;
    s_menu_sel = 0;
    show_menu();
}

void av_settings_exit(void) { s_screen = SCR_MENU; }

bool av_settings_key(bsp_btn_t btn, bsp_btn_ev_t bev)
{
    const bool click = bev == BSP_BTN_CLICK, lng = bev == BSP_BTN_LONG;
    switch (s_screen) {
    case SCR_MENU:
        if (btn == BSP_BTN_UP && click) {
            s_menu_sel = (s_menu_sel + MENU_COUNT - 1) % MENU_COUNT;
            show_menu();
        } else if (btn == BSP_BTN_DOWN && click) {
            s_menu_sel = (s_menu_sel + 1) % MENU_COUNT;
            show_menu();
        } else if ((btn == BSP_BTN_OK || btn == BSP_BTN_UP) && lng) {
            return true;  // 根菜单长按：回首页
        } else if (btn == BSP_BTN_OK && click) {
            switch (s_menu_sel) {
            case MENU_RELAY: {
                char cur[AV_URL_MAX];
                av_store_get_relay(cur, sizeof(cur));
                av_ipedit_begin(&s_edit, cur);
                s_screen = SCR_IPEDIT;
                show_ipedit();
                break;
            }
            case MENU_SERVER:
                if (toggle_server()) show_menu();
                break;
            case MENU_PROVISION:
                av_app_provision_begin();
                s_screen = SCR_PROV;
                s_prov_since = now_ms();
                s_prov_done_at = 0;
                show_prov();
                av_audio_release_buffer();  // BLE + Wi-Fi 共存时堆最紧，让出 20KB
                av_wifi_provision_start();
                show_prov();
                break;
            case MENU_VOLUME: {
                uint8_t v = av_store_get_volume();
                v = v >= 100 ? VOLUME_MIN : (uint8_t)(v + 10);
                av_audio_set_volume(v);
                av_store_set_volume(v);
                show_menu();
                break;
            }
            case MENU_CLEAR_WIFI:
                s_screen = SCR_CONFIRM;
                show_confirm();
                break;
            case MENU_OTA:
                s_screen = SCR_OTA;
                ota_start_check();
                break;
            case MENU_INFO:
                s_screen = SCR_INFO;
                show_info();
                break;
            default:
                return true;  // 「返回」
            }
        }
        break;

    case SCR_IPEDIT: {
        av_ipedit_key_t k;
        if (btn == BSP_BTN_UP) k = click ? AV_IPEDIT_KEY_UP : AV_IPEDIT_KEY_UP_LONG;
        else if (btn == BSP_BTN_DOWN) k = click ? AV_IPEDIT_KEY_DOWN : AV_IPEDIT_KEY_DOWN_LONG;
        else k = click ? AV_IPEDIT_KEY_OK : AV_IPEDIT_KEY_OK_LONG;
        if (!click && !lng) break;
        av_ipedit_result_t r = av_ipedit_key(&s_edit, k);
        if (r == AV_IPEDIT_EDITING) {
            show_ipedit();
        } else {
            if (r == AV_IPEDIT_SAVED) {
                char url[AV_URL_MAX];
                if (av_ipedit_url(&s_edit, url, sizeof(url)) > 0) apply_relay(url);
            }
            s_screen = SCR_MENU;
            show_menu();
        }
        break;
    }

    case SCR_PROV:
        if (btn == BSP_BTN_OK && lng) provision_exit();
        break;

    case SCR_CONFIRM:
        if (btn == BSP_BTN_OK && click) {
            av_wifi_clear();
            s_screen = SCR_MENU;
            show_menu();
        } else if (btn == BSP_BTN_OK && lng) {
            s_screen = SCR_MENU;
            show_menu();
        }
        break;

    case SCR_INFO:
        if (btn == BSP_BTN_OK && (click || lng)) {
            s_screen = SCR_MENU;
            show_menu();
        }
        break;

    case SCR_OTA: {
        static av_ota_status_t st;
        av_ota_get_status(&st);
        // 写入和切换分区期间不接受任何操作，只显示进度
        if (st.state == AV_OTA_DOWNLOADING || st.state == AV_OTA_DONE) break;
        if (lng) {
            s_screen = SCR_MENU;
            show_menu();
        } else if (click && btn == BSP_BTN_OK && st.state == AV_OTA_AVAILABLE) {
            if (av_ota_update() == ESP_OK) {
                av_ota_get_status(&st);
                show_ota(&st);
            }
        } else if (click && btn == BSP_BTN_OK && st.state == AV_OTA_ERROR) {
            ota_start_check();
        }
        break;
    }
    }
    return false;
}

void av_settings_poll(void)
{
    if (s_screen == SCR_OTA) {
        static av_ota_status_t st;
        av_ota_get_status(&st);
        const uint32_t now = now_ms();
        const bool throttled = st.state == AV_OTA_DOWNLOADING && now - s_ota_shown_ms < OTA_REFRESH_MS;
        if (st.revision != s_ota_rev && !throttled) show_ota(&st);
        return;
    }
    if (s_screen != SCR_PROV) return;
    const uint32_t now = now_ms();
    const av_prov_state_t st = av_wifi_provision_state();
    if (st != s_prov_shown) show_prov();
    if (st == AV_PROV_WIFI_CONNECTED && !s_prov_done_at) s_prov_done_at = now;
    if (st != AV_PROV_WIFI_CONNECTED) s_prov_done_at = 0;
    if ((s_prov_done_at && now - s_prov_done_at >= PROV_DONE_EXIT_MS) ||
        now - s_prov_since >= PROV_TIMEOUT_MS) {
        provision_exit();
    }
}
