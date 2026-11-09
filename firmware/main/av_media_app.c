// main/av_media_app.c —— 小说 / 音乐的进入、离开与按键转发。页面本身在 av_media_ui.c。
#include "av_media_app.h"

#include "bsp_display.h"
#include "esp_log.h"

#include "av_media.h"
#include "av_media_ui.h"
#include "av_store.h"
#include "av_ui.h"
#include "av_url.h"
#include "av_wifi.h"

static const char *TAG = "av_media_app";

static void show_unavailable(const char *message)
{
    if (!bsp_lvgl_lock(500)) return;
    av_ui_page_show("媒体服务不可用", message, NULL, AV_UI_AMBER, "长按 OK 返回");
    bsp_lvgl_unlock();
}

void av_media_app_enter(bool start_with_music)
{
    ESP_LOGI(TAG, "media app enter (%s)", start_with_music ? "music" : "books");
    // 媒体地址跟随「服务器」选择（本机 / la）；token 取自当前中继地址的 ?token=，不打印。
    char base[AV_MEDIA_BASE_MAX] = "", relay[AV_URL_MAX], token[AV_MEDIA_TOKEN_MAX] = "";
    av_store_get_relay(relay, sizeof(relay));
    av_store_get_media_base(base, sizeof(base));
    av_url_query_param(relay, "token", token, sizeof(token));
    if (!av_wifi_connected()) {
        // 没有 Wi-Fi 就没有媒体源：明确告诉用户去配网，而不是停在空白列表。
        if (bsp_lvgl_lock(500)) {
            av_ui_page_show("小说 / 音乐",
                            "未连接 Wi-Fi。\n\n请返回首页，进入「设置」，\n用「蓝牙配网」连接 2.4G Wi-Fi 后再回来。",
                            NULL, 0, "长按 OK 返回");
            bsp_lvgl_unlock();
        }
    } else if (!base[0]) {
        ESP_LOGW(TAG, "媒体服务地址未配置");
        show_unavailable("请在设置中配置本机中继地址，或在 menuconfig 中填写远端服务地址。");
    } else {
        ESP_LOGI(TAG, "media server=%s", av_store_get_server() == AV_SERVER_LA ? "remote" : "local");
        const esp_err_t err = av_media_set_base(base, token);  // 第一次调用会启动客户端
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "媒体客户端启动失败: %s", esp_err_to_name(err));
            show_unavailable("无法启动媒体客户端，请检查本地中继或远端配置。");
            return;
        }
        av_media_ui_init();
        if (start_with_music) av_media_ui_open_music();
        else av_media_ui_open_books();
    }
}

void av_media_app_exit(void)
{
    ESP_LOGI(TAG, "media app exit");
    av_media_ui_close();
}

bool av_media_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (av_media_ui_key(btn, ev)) return !av_media_ui_active();  // 列表页长按 OK：回首页
    // 没有列表页（如未连接 Wi-Fi 的提示页）：长按 OK 直接回首页
    return btn == BSP_BTN_OK && ev == BSP_BTN_LONG;
}

void av_media_app_poll(void) { av_media_ui_poll(); }
