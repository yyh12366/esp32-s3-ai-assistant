#include "bsp_display.h"
#include "bsp_display_mono.h"
#include "bsp_display_rounding.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "bsp_mono";

#define MONO_STRIPE_ROWS 8
#define MONO_STRIPE_BYTES (BSP_DISPLAY_MONO2_WIDTH * MONO_STRIPE_ROWS * 8)
#define MONO_TRANSFER_TIMEOUT_MS 100

static uint8_t *s_previous;  // 2.4KB，按需分配（只有 KK 直写路径用）
static uint8_t *s_stripe;
static SemaphoreHandle_t s_transfer_done;
static bool s_previous_valid;
static bool s_callbacks_registered;
static bool s_transfer_faulted;
static uint16_t s_previous_foreground;
static uint16_t s_previous_background;

static bool mono_transfer_done(esp_lcd_panel_io_handle_t io,
                              esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    (void)context;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_transfer_done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t mono_prepare(void)
{
    if (!bsp_display_panel() || !bsp_display_io()) return ESP_ERR_INVALID_STATE;
    if (!s_transfer_done) {
        s_transfer_done = xSemaphoreCreateBinary();
        if (!s_transfer_done) return ESP_ERR_NO_MEM;
    }
    if (!s_callbacks_registered) {
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = mono_transfer_done,
        };
        esp_err_t error = esp_lcd_panel_io_register_event_callbacks(
            bsp_display_io(), &callbacks, NULL);
        if (error != ESP_OK) return error;
        s_callbacks_registered = true;
    }
    if (!s_stripe) {
        s_stripe = heap_caps_malloc(MONO_STRIPE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_stripe) return ESP_ERR_NO_MEM;
    }
    if (!s_previous) {
        s_previous = heap_caps_calloc(1, BSP_DISPLAY_MONO2_FRAME_BYTES,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_previous) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool row_changed(const uint8_t *frame, size_t y, bool force)
{
    const size_t row_bytes = BSP_DISPLAY_MONO2_WIDTH / 8;
    return force || memcmp(frame + y * row_bytes,
                           s_previous + y * row_bytes, row_bytes) != 0;
}

static esp_err_t draw_rows(const uint8_t *frame, size_t first, size_t count,
                           uint16_t foreground, uint16_t background)
{
    const size_t source_stride = BSP_DISPLAY_MONO2_WIDTH / 8;
    const size_t expanded_row_bytes = BSP_DISPLAY_MONO2_WIDTH * 4;
    for (size_t y = 0; y < count; ++y) {
        uint8_t *row = s_stripe + y * expanded_row_bytes * 2;
        bsp_display_mono2_pack_row(frame + (first + y) * source_stride,
                                   BSP_DISPLAY_MONO2_WIDTH, foreground,
                                   background, row);
        memcpy(row + expanded_row_bytes, row, expanded_row_bytes);
        for (size_t physical_y = first * 2 + y * 2; physical_y < first * 2 + y * 2 + 2; ++physical_y) {
            int32_t visible_x1;
            int32_t visible_x2;
            uint8_t *physical_row = row + (physical_y % 2) * expanded_row_bytes;
            if (!bsp_display_rounded_row_span((int32_t)physical_y, BSP_LCD_W,
                                               BSP_LCD_H, BSP_LVGL_SCREEN_RADIUS,
                                               &visible_x1, &visible_x2)) {
                memset(physical_row, 0, expanded_row_bytes);
            } else {
                memset(physical_row, 0, (size_t)visible_x1 * 2);
                memset(physical_row + ((size_t)visible_x2 + 1) * 2, 0,
                       (BSP_LCD_W - (size_t)visible_x2 - 1) * 2);
            }
        }
    }

    const size_t x_end = BSP_DISPLAY_MONO2_WIDTH * 2;
    const size_t y_start = first * 2;
    const size_t y_end = (first + count) * 2;
    esp_err_t error = esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, y_start,
                                                 x_end, y_end, s_stripe);
    if (error != ESP_OK) return error;
    if (xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(MONO_TRANSFER_TIMEOUT_MS)) != pdTRUE) {
        s_transfer_faulted = true;
        ESP_LOGE(TAG, "DMA transfer callback timed out; stripe retained and display halted");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t bsp_display_mono2_blit(const uint8_t *frame, uint16_t foreground_rgb565,
                                 uint16_t background_rgb565)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    if (s_transfer_faulted) return ESP_ERR_INVALID_STATE;

    esp_err_t error = mono_prepare();
    if (error != ESP_OK) return error;

    const bool force = !s_previous_valid ||
                       foreground_rgb565 != s_previous_foreground ||
                       background_rgb565 != s_previous_background;
    for (size_t y = 0; y < BSP_DISPLAY_MONO2_HEIGHT;) {
        while (y < BSP_DISPLAY_MONO2_HEIGHT && !row_changed(frame, y, force)) ++y;
        if (y == BSP_DISPLAY_MONO2_HEIGHT) break;

        const size_t first = y;
        size_t count = 0;
        while (y < BSP_DISPLAY_MONO2_HEIGHT && count < MONO_STRIPE_ROWS &&
               row_changed(frame, y, force)) {
            ++y;
            ++count;
        }
        error = draw_rows(frame, first, count, foreground_rgb565, background_rgb565);
        if (error != ESP_OK) {
            s_previous_valid = false;
            if (error != ESP_ERR_TIMEOUT) ESP_LOGE(TAG, "mono panel draw failed: %s", esp_err_to_name(error));
            return error;
        }
    }

    memcpy(s_previous, frame, BSP_DISPLAY_MONO2_FRAME_BYTES);
    s_previous_foreground = foreground_rgb565;
    s_previous_background = background_rgb565;
    s_previous_valid = true;
    return ESP_OK;
}
