#include "kk_render_internal.h"

#include <stdio.h>
#include <string.h>

static void draw_value_row(kk_fb_t *fb, int label_id, const char *value, int y)
{
    const char *label = kk_render_texts[label_id];
    int value_width = (int)kk_font_width(value);
    int value_x = 112 - value_width;
    if (value_x < 44) value_x = 44;
    kk_font_draw(fb, 10, y, label, DRAW_NORMAL);
    kk_font_draw_clipped(fb, value_x, y, value, DRAW_NORMAL, 42, y, 72, KK_FONT_LINE_HEIGHT);
}

static void format_duration(char *buffer, size_t size, int64_t milliseconds)
{
    if (milliseconds < 0) {
        snprintf(buffer, size, "--");
        return;
    }

    uint64_t seconds = (uint64_t)milliseconds / 1000;
    uint64_t hours = seconds / 3600;
    uint64_t minutes = (seconds / 60) % 60;
    uint64_t remaining = seconds % 60;
    if (hours < 100) snprintf(buffer, size, "%02llu:%02llu:%02llu",
                              (unsigned long long)hours,
                              (unsigned long long)minutes,
                              (unsigned long long)remaining);
    else if (hours / 24 < 100) snprintf(buffer, size, "%02llud %02llu:%02llu",
                                        (unsigned long long)(hours / 24),
                                        (unsigned long long)(hours % 24),
                                        (unsigned long long)minutes);
    else snprintf(buffer, size, "%llud", (unsigned long long)(hours / 24));
}

void kk_render_data(kk_fb_t *fb, const kk_telemetry_t *telemetry)
{
    char value[24];
    kk_fb_round_rect(fb, 5, 41, 110, 97, 7, false, DRAW_NORMAL);

    if (telemetry->battery_mv < 0) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%d.%02dV", (int)(telemetry->battery_mv / 1000),
                  (int)(telemetry->battery_mv % 1000 / 10));
    draw_value_row(fb, KK_TEXT_VOLTAGE, value, 46);

    if (telemetry->battery_mv < 0) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%dmV", (int)telemetry->battery_mv);
    draw_value_row(fb, KK_TEXT_MILLIVOLTS, value, 64);

    if (telemetry->battery_soc < 0) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%d%%", (int)telemetry->battery_soc);
    draw_value_row(fb, KK_TEXT_BATTERY, value, 79);

    format_duration(value, sizeof(value), telemetry->uptime_ms);
    draw_value_row(fb, KK_TEXT_UPTIME, value, 97);

    if (telemetry->fps < 0) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%d FPS", (int)telemetry->fps);
    draw_value_row(fb, KK_TEXT_FPS, value, 115);
}

static void draw_wave_graph(kk_fb_t *fb, const kk_state_t *state,
                            const kk_telemetry_t *telemetry)
{
    const int x = 9;
    const int y = 47;
    const int width = 102;
    const int height = 53;
    const int center = y + height / 2;
    kk_fb_round_rect(fb, 6, 44, 108, 59, 6, false, DRAW_NORMAL);

    if (!telemetry->wave_samples || telemetry->wave_sample_count == 0) {
        int label_width = (int)kk_font_width(kk_render_texts[KK_TEXT_NO_SIGNAL]);
        kk_fb_round_rect(fb, 35, center - 7, 50, 14, 4, true, DRAW_CLEAR);
        kk_font_draw(fb, (KK_DISPLAY_WIDTH - label_width) / 2, center - 6,
                     kk_render_texts[KK_TEXT_NO_SIGNAL], DRAW_NORMAL);
        return;
    }

    kk_fb_line(fb, x, center, x + width - 1, center, DRAW_NORMAL);

    int previous_y = center;
    for (int column = 0; column < width; column++) {
        size_t sample_index = telemetry->wave_sample_count == 1 ? 0 :
            (size_t)column * (telemetry->wave_sample_count - 1) / (width - 1);
        int sample = telemetry->wave_samples[sample_index];
        int amplitude = height / 2 - 3;
        int sample_y = state->wave_source == KK_WAVE_MIC ?
            center - sample * amplitude / 32767 : center - sample * amplitude / 32768;
        if (column) kk_fb_line(fb, x + column - 1, previous_y, x + column, sample_y, DRAW_NORMAL);
        else kk_fb_point(fb, x, sample_y, DRAW_NORMAL);
        previous_y = sample_y;
    }
}

static void draw_wave_source(kk_fb_t *fb, const kk_state_t *state,
                             const kk_telemetry_t *telemetry)
{
    char value[32];
    const char *label;
    switch (state->wave_source) {
    case KK_WAVE_ADC:
        if (telemetry->adc_mv < 0) snprintf(value, sizeof(value), "%s --",
                                             kk_render_texts[KK_TEXT_GPIO0]);
        else snprintf(value, sizeof(value), "%s %dmV", kk_render_texts[KK_TEXT_GPIO0],
                      (int)telemetry->adc_mv);
        label = value;
        break;
    case KK_WAVE_SINE:
        label = kk_render_texts[KK_TEXT_SIMULATED_SOURCE];
        break;
    case KK_WAVE_MIC:
    default:
        if (telemetry->mic_level_pct < 0) snprintf(value, sizeof(value), "%s --",
                                                    kk_render_texts[KK_TEXT_MICROPHONE]);
        else snprintf(value, sizeof(value), "%s %d%%",
                      kk_render_texts[KK_TEXT_MICROPHONE], (int)telemetry->mic_level_pct);
        label = value;
        break;
    }
    int width = (int)kk_font_width(label);
    kk_font_draw_clipped(fb, (KK_DISPLAY_WIDTH - width) / 2, 107, label, DRAW_NORMAL,
                         2, 105, 116, KK_FONT_LINE_HEIGHT);
    kk_font_draw(fb, 8, 125, kk_render_texts[KK_TEXT_SWITCH_SOURCE], DRAW_NORMAL);
}

void kk_render_wave(kk_fb_t *fb, const kk_state_t *state, const kk_telemetry_t *telemetry)
{
    draw_wave_graph(fb, state, telemetry);
    draw_wave_source(fb, state, telemetry);
}

void kk_render_about(kk_fb_t *fb, const kk_telemetry_t *telemetry)
{
    char value[24];
    char board_lines[2][13] = { { 0 }, { 0 } };
    const char *board_id = telemetry->board_id ? telemetry->board_id : "--";
    size_t board_length = strlen(board_id);
    size_t first_length = board_length > 12 ? 12 : board_length;
    memcpy(board_lines[0], board_id, first_length);
    if (board_length > 12) {
        size_t second_length = board_length - 12 > 12 ? 12 : board_length - 12;
        memcpy(board_lines[1], board_id + 12, second_length);
    }
    kk_fb_round_rect(fb, 6, 42, 108, 44, 7, false, DRAW_NORMAL);
    kk_font_draw(fb, 12, 46, "BOARD ID", DRAW_NORMAL);
    kk_font_draw_clipped(fb, 12, 58, board_lines[0], DRAW_NORMAL, 12, 57, 96, 14);
    kk_font_draw_clipped(fb, 12, 70, board_lines[1], DRAW_NORMAL, 12, 70, 96, 14);

    kk_fb_round_rect(fb, 6, 89, 108, 48, 7, false, DRAW_NORMAL);
    if (telemetry->free_heap_bytes == UINT32_MAX) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%luKB",
                  (unsigned long)(telemetry->free_heap_bytes / 1024));
    draw_value_row(fb, KK_TEXT_MEMORY, value, 95);

    if (telemetry->largest_free_block_bytes == UINT32_MAX) snprintf(value, sizeof(value), "--");
    else snprintf(value, sizeof(value), "%luKB",
                  (unsigned long)(telemetry->largest_free_block_bytes / 1024));
    draw_value_row(fb, KK_TEXT_LARGEST_BLOCK, value, 115);
}
