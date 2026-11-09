#include <string.h>
#include "video_protocol.h"

bool video_packet_size(const uint8_t header[VIDEO_HEADER_SIZE], uint32_t *size)
{
    if (memcmp(header, "VDO1", 4) != 0) {
        return false;
    }
    *size = (uint32_t)header[4] | (uint32_t)header[5] << 8 | (uint32_t)header[6] << 16 | (uint32_t)header[7] << 24;
    return *size <= VIDEO_MAX_JPEG;
}
