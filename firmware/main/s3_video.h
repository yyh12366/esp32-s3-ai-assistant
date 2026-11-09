#pragma once

#include <stdint.h>
#include "esp_err.h"

void s3_video_run(void);
esp_err_t s3_video_display_init(uint8_t **video_pixels);
esp_err_t s3_video_display_frame(void);
