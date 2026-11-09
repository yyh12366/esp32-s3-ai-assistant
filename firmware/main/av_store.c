#include "av_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "av_media.h"
#include "av_url.h"

static const char *TAG = "av_store";
static const char *NS = "astrbot_voice";

esp_err_t av_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    // 合并固件里开机选择器可能已经初始化过 NVS，重复初始化返回 INVALID_STATE 属正常。
    if (err == ESP_ERR_INVALID_STATE) return ESP_OK;
    if (err != ESP_OK) {
        // 不自动擦除：分区里还有 Wi-Fi 凭据等用户数据
        ESP_LOGE(TAG, "NVS 初始化失败: %s（未自动擦除）", esp_err_to_name(err));
    }
    return err;
}

uint8_t av_store_get_server(void)
{
    nvs_handle_t h;
    uint8_t v = AV_SERVER_LOCAL;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "server", &v);
        nvs_close(h);
    }
    return v == AV_SERVER_LA ? AV_SERVER_LA : AV_SERVER_LOCAL;
}

esp_err_t av_store_set_server(uint8_t server)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "server", server == AV_SERVER_LA ? AV_SERVER_LA : AV_SERVER_LOCAL);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// 本机中继地址：NVS 里保存的，没有或无效时用编译默认值。
static void load_local_relay(char *buf, size_t size)
{
    nvs_handle_t h;
    size_t len = size;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_str(h, "relay", buf, &len);
        nvs_close(h);
        av_url_t url;
        if (err == ESP_OK && av_url_parse(buf, &url)) {
            // 在设备上改 IP 时如果是从无 token 的地址开始，存下的地址就没有 token；
            // 这里沿用编译默认地址里的 ?token=...（中继没开鉴权时多带一个参数无害）。
            if (av_url_inherit_query(buf, size, CONFIG_AV_RELAY_URL_DEFAULT)) {
                ESP_LOGI(TAG, "中继地址沿用默认 token");
            }
            return;
        }
    }
    snprintf(buf, size, "%s", CONFIG_AV_RELAY_URL_DEFAULT);
}

void av_store_get_relay(char *buf, size_t size)
{
    if (av_store_get_server() == AV_SERVER_LA) {
        // la 地址不落盘：每次从 Kconfig 取，token 沿用默认地址的 ?token=...
        snprintf(buf, size, "%s", CONFIG_AV_LA_RELAY_URL);
        av_url_inherit_query(buf, size, CONFIG_AV_RELAY_URL_DEFAULT);
        return;
    }
    load_local_relay(buf, size);
}

void av_store_get_media_base(char *buf, size_t size)
{
    if (!buf || !size) return;
    buf[0] = '\0';
    if (av_store_get_server() == AV_SERVER_LA) {
        snprintf(buf, size, "%s", CONFIG_AV_LA_MEDIA_BASE);
        return;
    }
    char relay[AV_URL_MAX];
    av_url_t url;
    load_local_relay(relay, sizeof(relay));
    if (av_url_parse(relay, &url)) {
        snprintf(buf, size, "http://%s:%d", url.host, AV_MEDIA_PORT_DEFAULT);
    }
}

esp_err_t av_store_set_relay(const char *url)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "relay", url);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint8_t av_store_get_volume(void)
{
    nvs_handle_t h;
    uint8_t v = CONFIG_AV_VOLUME_DEFAULT;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "volume", &v);
        nvs_close(h);
    }
    if (v < 10 || v > 100) v = CONFIG_AV_VOLUME_DEFAULT;
    return v;
}

esp_err_t av_store_set_volume(uint8_t percent)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "volume", percent);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void av_store_device_id(char *buf, size_t size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, size, "passport-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

bool av_store_get_read_pos(int *book, int *page)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint16_t raw = 0;
    const esp_err_t err = nvs_get_u16(h, "read_pos", &raw);
    nvs_close(h);
    if (err != ESP_OK) return false;
    *book = (int)(raw >> 8);
    *page = (int)(raw & 0xFF);
    return true;
}

esp_err_t av_store_set_read_pos(int book, int page)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    const uint16_t raw = (uint16_t)(((book & 0xFF) << 8) | (page & 0xFF));
    err = nvs_set_u16(h, "read_pos", raw);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
