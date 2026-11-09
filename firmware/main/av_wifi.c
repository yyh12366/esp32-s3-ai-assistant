#include "av_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "lwip/ip4_addr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include "av_app.h"
#include "av_blufi_security.h"
#include "av_session.h"
#include "av_url.h"

static const char *TAG = "av_wifi";

#define BLUFI_AP_LIST_COUNT 16

static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;
static wifi_config_t s_sta;
static volatile bool s_connected;
static volatile bool s_connecting;
static bool s_reconnect_after_disconnect;  // 配网时换网络：等这次断开后再连
static bool s_user_disconnect;             // 清除凭据时：不要自动重连
static unsigned s_retry;
static char s_ip[16];

// ---- 配网状态 ----
static volatile av_prov_state_t s_prov;
static volatile int s_prov_reason;
static bool s_host_initialized, s_host_running, s_gatt_initialized, s_btc_initialized;
static bool s_profile_initialized, s_ble_connected;
static SemaphoreHandle_t s_host_stopped;

static bool has_ssid(void) { return s_sta.sta.ssid[0] != '\0'; }

static void connect_now(void)
{
    if (!has_ssid()) return;
    s_connecting = true;
    av_app_post_simple(AV_EV_WIFI_CONNECTING);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (!s_connected && !s_user_disconnect) connect_now();
}

static void schedule_retry(void)
{
    uint32_t ms = av_backoff_ms(s_retry);
    if (s_retry < 100) s_retry++;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)ms * 1000);
}

static void send_report(esp_blufi_sta_conn_state_t state)
{
    if (!s_ble_connected) return;
    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    esp_blufi_extra_info_t info = {0};
    size_t len = strnlen((const char *)s_sta.sta.ssid, sizeof(s_sta.sta.ssid));
    if (len) {
        info.sta_ssid = s_sta.sta.ssid;
        info.sta_ssid_len = (int)len;
    }
    esp_blufi_send_wifi_conn_report(mode, state, 0, &info);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_get_config(WIFI_IF_STA, &s_sta);
        if (has_ssid()) connect_now();
        else av_app_post_simple(AV_EV_WIFI_NO_CONFIG);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        bool was_connecting = s_connecting;
        s_connected = false;
        s_connecting = false;
        s_ip[0] = '\0';
        ESP_LOGW(TAG, "Wi-Fi 断开 reason=%u", ev->reason);
        if (s_reconnect_after_disconnect) {
            s_reconnect_after_disconnect = false;
            connect_now();
            return;
        }
        if (s_prov == AV_PROV_WIFI_CONNECTING && was_connecting) {
            s_prov_reason = ev->reason;
            s_prov = AV_PROV_WIFI_FAILED;
            send_report(ESP_BLUFI_STA_CONN_FAIL);
        }
        if (s_user_disconnect) return;
        if (!has_ssid()) {
            // 凭据刚被清除（断开事件晚于清除到达）：状态是“未配网”而不是“掉线”
            av_app_post_simple(AV_EV_WIFI_NO_CONFIG);
            return;
        }
        av_app_post_wifi(AV_EV_WIFI_DOWN, ev->reason);
        // 配网过程中由手机端决定何时重试，避免与 BLUFI 的连接请求打架
        if (s_prov == AV_PROV_OFF) schedule_retry();
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        if (!s_ble_connected) return;
        uint16_t count = BLUFI_AP_LIST_COUNT;
        wifi_ap_record_t *records = calloc(BLUFI_AP_LIST_COUNT, sizeof(*records));
        esp_blufi_ap_record_t *list = calloc(BLUFI_AP_LIST_COUNT, sizeof(*list));
        if (records && list && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            for (uint16_t i = 0; i < count; i++) {
                list[i].rssi = records[i].rssi;
                memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
            }
            esp_blufi_send_wifi_list(count, list);
        } else {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        free(records);
        free(list);
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *ev = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_connected = true;
    s_connecting = false;
    s_retry = 0;
    esp_timer_stop(s_retry_timer);
    ESP_LOGI(TAG, "已获取 IP %s", s_ip);
    if (s_prov != AV_PROV_OFF) {
        s_prov = AV_PROV_WIFI_CONNECTED;
        send_report(ESP_BLUFI_STA_CONN_SUCCESS);
    }
    av_app_post_simple(AV_EV_WIFI_UP);
}

esp_err_t av_wifi_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "av_wifi_retry"};
    err = esp_timer_create(&targs, &s_retry_timer);
    if (err == ESP_OK) err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    if (err == ESP_OK) err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL, NULL);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    // 对讲需要低延迟：关闭 modem sleep 省掉 DTIM 唤醒带来的几十到上百毫秒抖动
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) err = esp_wifi_start();
    return err;
}

bool av_wifi_has_config(void) { return has_ssid(); }
bool av_wifi_connected(void) { return s_connected; }

void av_wifi_info(char *ssid, size_t ssid_size, char *ip, size_t ip_size)
{
    snprintf(ssid, ssid_size, "%.32s", (const char *)s_sta.sta.ssid);
    snprintf(ip, ip_size, "%s", s_ip);
}

esp_err_t av_wifi_clear(void)
{
    s_user_disconnect = true;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    wifi_config_t empty = {0};
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    memset(&s_sta, 0, sizeof(s_sta));
    s_connected = false;
    s_connecting = false;
    s_ip[0] = '\0';
    s_retry = 0;
    s_user_disconnect = false;
    av_app_post_simple(AV_EV_WIFI_NO_CONFIG);
    return err;
}

// ---------------------------------------------------------------------------
// BLUFI
// ---------------------------------------------------------------------------
static void request_connect(void)
{
    bool was_connected = s_connected;
    s_prov = AV_PROV_WIFI_CONNECTING;
    s_retry = 0;
    esp_timer_stop(s_retry_timer);
    if (was_connected || s_connecting) {
        s_reconnect_after_disconnect = true;
        if (esp_wifi_disconnect() == ESP_OK) return;
        s_reconnect_after_disconnect = false;
    }
    connect_now();
}

static void handle_custom_data(const uint8_t *data, uint32_t len)
{
    // 约定：文本 "relay=ws://host:port/path"，也接受直接发 "ws://..."
    char text[AV_URL_MAX + 8];
    if (len >= sizeof(text)) {
        esp_blufi_send_custom_data((uint8_t *)"relay: too long", 15);
        return;
    }
    memcpy(text, data, len);
    text[len] = '\0';
    const char *url = strncmp(text, "relay=", 6) == 0 ? text + 6 : text;
    av_url_t parsed;
    if (!av_url_parse(url, &parsed)) {
        esp_blufi_send_custom_data((uint8_t *)"relay: invalid", 14);
        return;
    }
    char *copy = strdup(url);
    if (!copy) return;
    av_app_post_relay(copy);  // 应用任务保存到 NVS 并释放
    esp_blufi_send_custom_data((uint8_t *)"relay: ok", 9);
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        esp_blufi_adv_start_with_name(AV_BLUFI_NAME);
        s_prov = s_connected ? AV_PROV_WIFI_CONNECTED : AV_PROV_ADVERTISING;
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_ble_connected = true;
        esp_blufi_adv_stop();
        if (av_blufi_security_init() != 0) s_prov = AV_PROV_ERROR;
        else if (!s_connected) s_prov = AV_PROV_PHONE_CONNECTED;
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        av_blufi_security_deinit();
        esp_blufi_adv_start_with_name(AV_BLUFI_NAME);
        if (s_prov != AV_PROV_WIFI_CONNECTED && s_prov != AV_PROV_WIFI_FAILED) s_prov = AV_PROV_ADVERTISING;
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        esp_wifi_set_mode(WIFI_MODE_STA);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(s_sta.sta.bssid, param->sta_bssid.bssid, 6);
        s_sta.sta.bssid_set = true;
        esp_wifi_set_config(WIFI_IF_STA, &s_sta);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len <= 0 || param->sta_ssid.ssid_len >= (int)sizeof(s_sta.sta.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(&s_sta, 0, sizeof(s_sta));
        memcpy(s_sta.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        s_sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &s_sta);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len < 0 || param->sta_passwd.passwd_len >= (int)sizeof(s_sta.sta.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_sta.sta.password, 0, sizeof(s_sta.sta.password));
        memcpy(s_sta.sta.password, param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_sta);  // 密码只写入 NVS，不打印
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        request_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        send_report(s_connected ? ESP_BLUFI_STA_CONN_SUCCESS
                    : s_connecting ? ESP_BLUFI_STA_CONNECTING : ESP_BLUFI_STA_CONN_FAIL);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
        wifi_scan_config_t scan = {0};
        if (esp_wifi_scan_start(&scan, false) != ESP_OK) esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        handle_custom_data(param->custom_data.data, param->custom_data.data_len);
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        esp_blufi_send_error_info(param->report_error.state);
        break;
    default:
        break;
    }
}

static esp_blufi_callbacks_t s_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = av_blufi_negotiate,
    .encrypt_func = av_blufi_encrypt,
    .decrypt_func = av_blufi_decrypt,
    .checksum_func = av_blufi_checksum,
};

static void ble_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
    s_prov = AV_PROV_ERROR;
}

static void ble_sync(void)
{
    if (esp_blufi_profile_init() == 0) s_profile_initialized = true;
    else s_prov = AV_PROV_ERROR;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

esp_err_t av_wifi_provision_start(void)
{
    if (s_prov != AV_PROV_OFF) return ESP_OK;
    s_prov = AV_PROV_STARTING;
    s_prov_reason = 0;
    esp_err_t err = esp_blufi_register_callbacks(&s_callbacks);
    if (err == ESP_OK) err = nimble_port_init();
    if (err != ESP_OK) goto fail;
    s_host_initialized = true;
    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) { err = ESP_ERR_NO_MEM; goto fail; }
    ble_hs_cfg.reset_cb = ble_reset;
    ble_hs_cfg.sync_cb = ble_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    if (esp_blufi_gatt_svr_init() != 0) { err = ESP_FAIL; goto fail; }
    s_gatt_initialized = true;
    if (ble_svc_gap_device_name_set(AV_BLUFI_NAME) != 0) { err = ESP_FAIL; goto fail; }
    esp_blufi_btc_init();
    s_btc_initialized = true;
    err = esp_nimble_enable(host_task);
    if (err != ESP_OK) goto fail;
    s_host_running = true;
    ESP_LOGI(TAG, "BLUFI 配网已启动，广播名 %s", AV_BLUFI_NAME);
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "BLUFI 启动失败: %s", esp_err_to_name(err));
    av_wifi_provision_stop();
    s_prov = AV_PROV_ERROR;
    return err;
}

void av_wifi_provision_stop(void)
{
    s_ble_connected = false;
    av_blufi_security_deinit();
    if (s_host_initialized) {
        if (s_profile_initialized) esp_blufi_adv_stop();
        if (s_gatt_initialized) {
            esp_blufi_gatt_svr_deinit();
            s_gatt_initialized = false;
        }
        bool stopped = !s_host_running;
        if (s_host_running && nimble_port_stop() == 0) {
            xSemaphoreTake(s_host_stopped, portMAX_DELAY);
            stopped = true;
        }
        if (stopped) nimble_port_deinit();
        s_host_running = false;
        if (s_profile_initialized) {
            esp_blufi_profile_deinit();
            s_profile_initialized = false;
        }
        if (s_btc_initialized) {
            esp_blufi_btc_deinit();
            s_btc_initialized = false;
        }
        s_host_initialized = false;
    }
    if (s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    s_prov = AV_PROV_OFF;
    // 恢复正常的自动重连
    if (!s_connected && has_ssid() && !s_connecting) schedule_retry();
}

av_prov_state_t av_wifi_provision_state(void) { return s_prov; }
int av_wifi_provision_reason(void) { return s_prov_reason; }
