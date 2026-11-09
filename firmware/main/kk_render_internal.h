#pragma once

#include "kk_font.h"
#include "kk_icons.h"
#include "kk_render.h"
#include "kk_render_texts.h"

void kk_render_header(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim,
                      const kk_telemetry_t *telemetry);
void kk_render_footer(kk_fb_t *fb, const kk_telemetry_t *telemetry);
void kk_render_home(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim);
void kk_render_data(kk_fb_t *fb, const kk_telemetry_t *telemetry);
void kk_render_wave(kk_fb_t *fb, const kk_state_t *state, const kk_telemetry_t *telemetry);
void kk_render_settings(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim);
void kk_render_about(kk_fb_t *fb, const kk_telemetry_t *telemetry);
void kk_render_modal(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim,
                    const kk_telemetry_t *telemetry);
void kk_render_toast(kk_fb_t *fb, const kk_state_t *state, const kk_anim_t *anim,
                    uint64_t now_ms);
void kk_render_key_icon(kk_fb_t *fb, int x, int y, int key, bool pressed);
