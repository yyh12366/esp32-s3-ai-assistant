#include "s3_video.h"
#include "video_protocol.h"
#include "bsp_display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "video_display";
static SemaphoreHandle_t s_complete;
static uint8_t *s_pixels;
enum { VIDEO_Y = (BSP_LCD_W - VIDEO_HEIGHT) / 2, FRAME_BYTES = VIDEO_WIDTH * VIDEO_HEIGHT * 2 };

static bool transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    BaseType_t awakened = pdFALSE;
    xSemaphoreGiveFromISR(context, &awakened);
    return awakened == pdTRUE;
}

static esp_err_t draw_rows(int y, int rows)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, y, VIDEO_WIDTH, y + rows, s_pixels),
                        TAG, "Submit LCD DMA failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_complete, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "LCD DMA timeout");
    return ESP_OK;
}

esp_err_t s3_video_display_init(uint8_t **video_pixels)
{
    ESP_RETURN_ON_ERROR(bsp_display_init(), TAG, "Initialize LCD failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(bsp_display_panel(), true), TAG, "Rotate LCD failed");
    s_complete = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_complete, ESP_ERR_NO_MEM, TAG, "Allocate LCD semaphore failed");
    s_pixels = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_pixels, ESP_ERR_NO_MEM, TAG, "Allocate DMA framebuffer failed");
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = transfer_done};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(bsp_display_io(), &callbacks, s_complete),
                        TAG, "Register video DMA callback failed");
    ESP_RETURN_ON_ERROR(draw_rows(0, VIDEO_HEIGHT), TAG, "Clear LCD top failed");
    ESP_RETURN_ON_ERROR(draw_rows(VIDEO_HEIGHT, BSP_LCD_W - VIDEO_HEIGHT), TAG, "Clear LCD bottom failed");
    *video_pixels = s_pixels;
    ESP_LOGI(TAG, "USB video display ready; reset the board to return to the home menu");
    return ESP_OK;
}

esp_err_t s3_video_display_frame(void)
{
    return draw_rows(VIDEO_Y, VIDEO_HEIGHT);
}
