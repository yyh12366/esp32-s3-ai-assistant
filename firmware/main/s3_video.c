#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "s3_video.h"
#include "video_protocol.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

static const char *TAG = "video";

static esp_err_t read_exact(uint8_t *data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        int count = usb_serial_jtag_read_bytes(data + received, length - received, pdMS_TO_TICKS(3000));
        ESP_RETURN_ON_FALSE(count > 0, ESP_ERR_TIMEOUT, TAG, "USB packet timeout after %u/%u bytes", (unsigned)received, (unsigned)length);
        received += count;
    }
    return ESP_OK;
}

static esp_err_t play_video(uint8_t *pixels, uint8_t *jpeg, void *working_buffer)
{
    esp_jpeg_image_cfg_t config = {
        .indata = jpeg,
        .outbuf = pixels,
        .outbuf_size = VIDEO_WIDTH * VIDEO_HEIGHT * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 1,
        .advanced = {.working_buffer = working_buffer, .working_buffer_size = 65536},
    };
    uint32_t frames = 0;
    ESP_LOGI(TAG, "Waiting for USB video sender; audio pins are unused");
    while (true) {
        uint8_t header[VIDEO_HEADER_SIZE];
        int received = usb_serial_jtag_read_bytes(header, sizeof(header), pdMS_TO_TICKS(1000));
        if (received == 0) {
            continue;
        }
        ESP_RETURN_ON_FALSE(received > 0, ESP_FAIL, TAG, "USB read failed");
        ESP_RETURN_ON_ERROR(read_exact(header + received, sizeof(header) - received), TAG, "Read header failed");
        uint32_t length;
        ESP_RETURN_ON_FALSE(video_packet_size(header, &length), ESP_ERR_INVALID_ARG, TAG, "Invalid video header or frame size");
        if (length == 0) {
            frames = 0;
            printf("VIDEO_READY %d %d %d\n", VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
            continue;
        }
        ESP_RETURN_ON_ERROR(read_exact(jpeg, length), TAG, "Read JPEG failed");
        config.indata_size = length;
        esp_jpeg_image_output_t image;
        int64_t start = esp_timer_get_time();
        ESP_RETURN_ON_ERROR(esp_jpeg_decode(&config, &image), TAG, "Decode JPEG failed");
        ESP_RETURN_ON_FALSE(image.width == VIDEO_WIDTH && image.height == VIDEO_HEIGHT, ESP_ERR_INVALID_SIZE, TAG, "Expected 320x180 JPEG");
        int64_t decoded = esp_timer_get_time();
        ESP_RETURN_ON_ERROR(s3_video_display_frame(), TAG, "Display JPEG failed");
        int64_t displayed = esp_timer_get_time();
        printf("FRAME_OK %" PRIu32 " %" PRId64 " %" PRId64 "\n", ++frames, decoded - start, displayed - decoded);
    }
}

void s3_video_run(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    usb_serial_jtag_driver_config_t usb_config = {.tx_buffer_size = 1024, .rx_buffer_size = 4096};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_use_driver();
    uint8_t *pixels;
    esp_err_t error = s3_video_display_init(&pixels);
    uint8_t *jpeg = NULL;
    void *working_buffer = NULL;
    if (error == ESP_OK) {
        jpeg = heap_caps_malloc(VIDEO_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        working_buffer = heap_caps_malloc(65536, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        error = jpeg && working_buffer ? play_video(pixels, jpeg, working_buffer) : ESP_ERR_NO_MEM;
    }
    free(working_buffer);
    free(jpeg);
    printf("VIDEO_ERROR %s\n", esp_err_to_name(error));
    ESP_LOGE(TAG, "Player stopped: %s; reset the board before restarting the sender", esp_err_to_name(error));
}
