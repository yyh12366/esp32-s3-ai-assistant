#pragma once

#include "kk_render.h"

void kk_app_telemetry_init(kk_telemetry_t *telemetry);
void kk_app_telemetry_update(const kk_state_t *state, kk_telemetry_t *telemetry,
                            uint8_t pressed_mask, bool audio_ready,
                            bool monitor_ready, bool input_ready, uint64_t now_ms);
