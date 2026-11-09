// main/av_ui.h —— 对讲应用界面（LVGL）。全部函数必须在持有 bsp_lvgl_lock() 时调用，
// 只有 av_ui_has_glyph() 例外（只读字体数据）。
//
// 页面：
//   主界面   顶栏（连接状态 + 电量）、角色图片、状态提示、识别文字、单行横向滚动回复、错误横幅
//   设置页   列表菜单 / 说明页 / 中继地址编辑器，都画在同一个 page 屏幕上
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "av_ipedit.h"
#include "av_proto.h"

#define AV_UI_GREEN  0x3DDC97
#define AV_UI_AMBER  0xFFB74D
#define AV_UI_RED    0xFF6B6B
#define AV_UI_BLUE   0x4FC3F7
#define AV_UI_DIM    0x8A96A8

void av_ui_init(void);
bool av_ui_has_glyph(uint32_t codepoint, void *user);

void av_ui_main_show(void);
void av_ui_main_status(const char *link_text, uint32_t link_color, const char *error, int battery);
void av_ui_main_voice(av_voice_t voice);
void av_ui_main_asr(const char *text, bool placeholder);
void av_ui_main_reply(const char *text);   // 整段替换；长文本单行横向循环滚动
void av_ui_main_scroll(int direction);     // 兼容主界面按键事件；回复由标签自动横向滚动

void av_ui_menu_show(const char *title, const char *const *labels, const char *const *values,
                     int count, int selected, const char *footer);
// 同 av_ui_menu_show，行距更小，用于 7 项的设置页。
void av_ui_menu_show_dense(const char *title, const char *const *labels, const char *const *values,
                           int count, int selected, const char *footer);
void av_ui_page_show(const char *title, const char *body, const char *status, uint32_t status_color,
                     const char *footer);
void av_ui_ipedit_show(const av_ipedit_t *edit);

// 阅读页：body 是整页正文（\n 分行），footer 显示页码。
void av_ui_reader_show(const char *title, const char *body, int page, int pages);
