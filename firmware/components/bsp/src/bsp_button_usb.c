#include <stdbool.h>
#include "bsp_button.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_btn";
static bsp_btn_cb_t s_callback;
static void *s_user;
static portMUX_TYPE s_callback_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ok_held;

static void send_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    portENTER_CRITICAL(&s_callback_mux);
    bsp_btn_cb_t callback = s_callback;
    void *user = s_user;
    portEXIT_CRITICAL(&s_callback_mux);
    if (callback) callback(button, event, user);
}

static void release_ok(void)
{
    if (!s_ok_held) return;
    s_ok_held = false;
    send_key(BSP_BTN_OK, BSP_BTN_RELEASE);
}

static void usb_keys(void *arg)
{
    (void)arg;
    TickType_t pressed_at = 0;
    while (true) {
        uint8_t key;
        if (usb_serial_jtag_read_bytes(&key, 1, pdMS_TO_TICKS(100)) == 1) {
            switch (key) {
            case 'w': send_key(BSP_BTN_UP, BSP_BTN_CLICK); break;
            case 's': send_key(BSP_BTN_DOWN, BSP_BTN_CLICK); break;
            case 'e': send_key(BSP_BTN_OK, BSP_BTN_CLICK); break;
            case '1': send_key(BSP_BTN_UP, BSP_BTN_DOUBLE); break;
            case '2': send_key(BSP_BTN_DOWN, BSP_BTN_DOUBLE); break;
            case '3': send_key(BSP_BTN_OK, BSP_BTN_DOUBLE); break;
            case 'W': send_key(BSP_BTN_UP, BSP_BTN_LONG); break;
            case 'S': send_key(BSP_BTN_DOWN, BSP_BTN_LONG); break;
            case 'E': send_key(BSP_BTN_OK, BSP_BTN_LONG); break;
            case 'r':
                if (!s_ok_held) {
                    s_ok_held = true;
                    pressed_at = xTaskGetTickCount();
                    send_key(BSP_BTN_OK, BSP_BTN_PRESS);
                }
                break;
            case 't': release_ok(); break;
            default: break;
            }
        }
        // If the terminal disconnects during recording, still deliver a key release.
        if (s_ok_held && xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(30000)) release_ok();
    }
}

esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user)
{
    if (!callback) {
        ESP_LOGE(TAG, "USB key callback missing");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_callback) {
        portENTER_CRITICAL(&s_callback_mux);
        s_callback = callback;
        s_user = user;
        portEXIT_CRITICAL(&s_callback_mux);
        ESP_LOGI(TAG, "USB key callback updated");
        return ESP_OK;
    }
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB key driver: %s", esp_err_to_name(err));
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
    portENTER_CRITICAL(&s_callback_mux);
    s_callback = callback;
    s_user = user;
    portEXIT_CRITICAL(&s_callback_mux);
    if (xTaskCreate(usb_keys, "usb_keys", 3072, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_callback_mux);
        s_callback = NULL;
        s_user = NULL;
        portEXIT_CRITICAL(&s_callback_mux);
        usb_serial_jtag_vfs_use_nonblocking();
        usb_serial_jtag_driver_uninstall();
        ESP_LOGE(TAG, "USB key task allocation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB keys: w/s/e=click, 1/2/3=double, W/S/E=long, r/t=OK press/release");
    return ESP_OK;
}

int bsp_button_read_mv(void) { return -1; }

esp_err_t bsp_button_prepare_deep_sleep(int *level)
{
    (void)level;
    ESP_LOGW(TAG, "USB keys cannot wake from deep sleep");
    return ESP_ERR_NOT_SUPPORTED;
}
