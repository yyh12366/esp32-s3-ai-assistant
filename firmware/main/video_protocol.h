#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { VIDEO_WIDTH = 320, VIDEO_HEIGHT = 180, VIDEO_FPS = 10, VIDEO_HEADER_SIZE = 8, VIDEO_MAX_JPEG = 128 * 1024 };

bool video_packet_size(const uint8_t header[VIDEO_HEADER_SIZE], uint32_t *size);
