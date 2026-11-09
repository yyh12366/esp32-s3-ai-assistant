#include "av_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_pins.h"
#include "lvgl.h"

LV_FONT_DECLARE(av_font_cjk_16);
LV_IMAGE_DECLARE(av_character_idle);
LV_IMAGE_DECLARE(av_character_blink);
LV_IMAGE_DECLARE(av_character_listening);
LV_IMAGE_DECLARE(av_character_thinking);
LV_IMAGE_DECLARE(av_character_speaking);

#define BG        0x0B0F14
#define PANEL     0x151B23
#define INK       0xE8EDF3
#define SELECT_BG 0x23406B
#define ERROR_BG  0x5A1E24

// 屏幕四角被 BSP 以 30px 圆角裁成黑色：内容左右至少留 16px，顶/底至少 10px。
#define MARGIN_X  16
#define CONTENT_W (BSP_LCD_W - 2 * MARGIN_X)

static lv_font_t s_font;  // 中文字体 + Montserrat 14 兜底 LVGL 图标

static lv_obj_t *s_main;
static lv_obj_t *s_link, *s_battery, *s_error;
static lv_obj_t *s_state, *s_asr, *s_reply_box, *s_reply;
static lv_obj_t *s_avatar;
static lv_obj_t *s_blink_eyes;
static lv_timer_t *s_blink_timer;
static av_voice_t s_voice = AV_VOICE_IDLE;
static bool s_blink_closed;

static lv_obj_t *s_page;

static const uint32_t VOICE_COLOR[] = {AV_UI_GREEN, AV_UI_BLUE, AV_UI_AMBER, 0xBA68C8};
static const char *const VOICE_HINT[] = {
    "待机 · 按住 OK 说话",
    "正在聆听… 松开发送",
    "思考中…",
    "说话中 · 按 OK 打断",
};

// Runs on the LVGL task. Both screens and the avatar live for the app's lifetime.
static void blink_timer_cb(lv_timer_t *timer)
{
    s_blink_closed = !s_blink_closed;
    lv_obj_set_flag(s_blink_eyes, LV_OBJ_FLAG_HIDDEN, !s_blink_closed);
    lv_timer_set_period(timer, s_blink_closed ? 140 : lv_rand(3000, 5000));
}

static void reset_blink(void)
{
    lv_timer_pause(s_blink_timer);
    s_blink_closed = false;
    lv_obj_add_flag(s_blink_eyes, LV_OBJ_FLAG_HIDDEN);
    if (s_voice != AV_VOICE_IDLE) return;

    lv_image_set_src(s_avatar, &av_character_idle);
    if (lv_screen_active() == s_main) {
        lv_timer_set_period(s_blink_timer, lv_rand(3000, 5000));
        lv_timer_reset(s_blink_timer);
        lv_timer_resume(s_blink_timer);
    }
}

bool av_ui_has_glyph(uint32_t codepoint, void *user)
{
    (void)user;
    lv_font_glyph_dsc_t g = {0};
    return lv_font_get_glyph_dsc(&av_font_cjk_16, &g, codepoint, 0) && !g.is_placeholder;
}

static lv_obj_t *label(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &s_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_remove_style_all(s);
    lv_obj_set_style_bg_color(s, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

void av_ui_init(void)
{
    s_font = av_font_cjk_16;
    s_font.fallback = &lv_font_montserrat_14;

    s_main = screen();
    // 顶栏
    s_link = label(s_main, AV_UI_AMBER);
    lv_obj_set_pos(s_link, 26, 12);
    lv_obj_set_width(s_link, 140);
    lv_label_set_long_mode(s_link, LV_LABEL_LONG_DOT);
    s_battery = label(s_main, AV_UI_DIM);
    lv_obj_set_width(s_battery, 60);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_battery, BSP_LCD_W - 26 - 60, 12);
    lv_label_set_text(s_battery, "");

    // 192px 透明角色图直接读取 Flash 中的 RGB565A8 数据。
    s_avatar = lv_image_create(s_main);
    lv_obj_set_pos(s_avatar, 24, 34);
    // Crop offsets in tools/gen_av_character_states.py are relative to the avatar.
    // Keep the base sprite fixed throughout a blink; show only the eye patches.
    s_blink_eyes = lv_image_create(s_avatar);
    lv_image_set_src(s_blink_eyes, &av_character_blink);
    lv_obj_set_pos(s_blink_eyes, 60, 75);
    lv_obj_add_flag(s_blink_eyes, LV_OBJ_FLAG_HIDDEN);
    s_blink_timer = lv_timer_create(blink_timer_cb, 4000, NULL);
    lv_timer_pause(s_blink_timer);

    s_state = label(s_main, AV_UI_GREEN);
    lv_obj_set_width(s_state, CONTENT_W);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_state, MARGIN_X, 230);

    // 识别文字：单行显示，超出用省略号
    s_asr = label(s_main, INK);
    lv_obj_set_pos(s_asr, MARGIN_X + 4, 254);
    lv_label_set_long_mode(s_asr, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_asr, CONTENT_W - 8, 22);

    // 回复文字：单行横向循环滚动
    s_reply_box = lv_obj_create(s_main);
    lv_obj_remove_style_all(s_reply_box);
    lv_obj_set_pos(s_reply_box, MARGIN_X, 280);
    lv_obj_set_size(s_reply_box, CONTENT_W, 28);
    lv_obj_set_style_bg_color(s_reply_box, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(s_reply_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_reply_box, 12, 0);
    lv_obj_set_style_pad_all(s_reply_box, 3, 0);
    lv_obj_set_style_pad_left(s_reply_box, 10, 0);
    lv_obj_set_style_pad_right(s_reply_box, 10, 0);
    lv_obj_set_scrollbar_mode(s_reply_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_reply_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_reply = label(s_reply_box, INK);
    lv_label_set_long_mode(s_reply, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_reply, CONTENT_W - 20);
    lv_label_set_text(s_reply, "");

    // 错误横幅（覆盖在角色图上方）
    s_error = label(s_main, 0xFFFFFF);
    lv_obj_set_pos(s_error, MARGIN_X, 38);
    lv_obj_set_width(s_error, CONTENT_W);
    lv_label_set_long_mode(s_error, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(s_error, lv_color_hex(ERROR_BG), 0);
    lv_obj_set_style_bg_opa(s_error, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_error, 8, 0);
    lv_obj_set_style_pad_all(s_error, 6, 0);
    lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);

    av_ui_main_voice(AV_VOICE_IDLE);
    av_ui_main_asr("按住 OK 键说话", true);

    s_page = screen();
}

void av_ui_main_show(void)
{
    if (lv_screen_active() != s_main) {
        lv_screen_load(s_main);
        reset_blink();
    }
}

void av_ui_main_status(const char *link_text, uint32_t link_color, const char *error, int battery)
{
    lv_label_set_text_fmt(s_link, LV_SYMBOL_WIFI " %s", link_text);
    lv_obj_set_style_text_color(s_link, lv_color_hex(link_color), 0);
    if (battery < 0) {
        lv_label_set_text(s_battery, "");  // 电量计不可用时不画数字
    } else {
        const char *icon = battery > 80 ? LV_SYMBOL_BATTERY_FULL
                         : battery > 55 ? LV_SYMBOL_BATTERY_3
                         : battery > 30 ? LV_SYMBOL_BATTERY_2
                         : battery > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_battery, "%d%% %s", battery, icon);
        lv_obj_set_style_text_color(s_battery, lv_color_hex(battery <= 10 ? AV_UI_RED : AV_UI_DIM), 0);
    }
    if (error && error[0]) {
        lv_label_set_text(s_error, error);
        lv_obj_remove_flag(s_error, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);
    }
}

void av_ui_main_voice(av_voice_t voice)
{
    const lv_image_dsc_t *avatar_image;
    switch (voice) {
    case AV_VOICE_IDLE:
        avatar_image = &av_character_idle;
        break;
    case AV_VOICE_LISTENING:
        avatar_image = &av_character_listening;
        break;
    case AV_VOICE_THINKING:
        avatar_image = &av_character_thinking;
        break;
    case AV_VOICE_SPEAKING:
        avatar_image = &av_character_speaking;
        break;
    default:
        return;
    }
    lv_image_set_src(s_avatar, avatar_image);
    s_voice = voice;
    reset_blink();
    lv_label_set_text(s_state, VOICE_HINT[voice]);
    lv_obj_set_style_text_color(s_state, lv_color_hex(VOICE_COLOR[voice]), 0);
}

void av_ui_main_asr(const char *text, bool placeholder)
{
    if (placeholder) {
        lv_label_set_text(s_asr, text);
        lv_obj_set_style_text_color(s_asr, lv_color_hex(AV_UI_DIM), 0);
    } else {
        lv_label_set_text_fmt(s_asr, "你：%s", text);
        lv_obj_set_style_text_color(s_asr, lv_color_hex(INK), 0);
    }
}

void av_ui_main_reply(const char *text)
{
    lv_label_set_text(s_reply, text);
    char *reply_text = lv_label_get_text(s_reply);
    for (char *p = reply_text; p && *p; p++) {
        if (*p == '\r' || *p == '\n') *p = ' ';
    }
    lv_label_set_text(s_reply, NULL);  // 刷新 LVGL 对已复制文本的布局
}

void av_ui_main_scroll(int direction)
{
    // 单行回复由 LV_LABEL_LONG_SCROLL_CIRCULAR 自动横向滚动。
    (void)direction;
}

// ---- 设置类页面 ----
static lv_obj_t *page_begin(const char *title)
{
    lv_obj_clean(s_page);
    lv_obj_t *t = label(s_page, INK);
    lv_obj_set_width(t, CONTENT_W);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, MARGIN_X, 14);
    lv_label_set_text(t, title);
    box(s_page, 40, 38, BSP_LCD_W - 80, 2, SELECT_BG, 1);
    return s_page;
}

static void page_footer(const char *footer)
{
    if (!footer) return;
    lv_obj_t *f = label(s_page, AV_UI_DIM);
    lv_obj_set_width(f, CONTENT_W);
    lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(f, MARGIN_X, BSP_LCD_H - 34);
    lv_label_set_text(f, footer);
}

static void page_end(void)
{
    if (lv_screen_active() != s_page) lv_screen_load(s_page);
    reset_blink();
}

static void menu_show(const char *title, const char *const *labels, const char *const *values,
                      int count, int selected, const char *footer, int pitch, int height)
{
    page_begin(title);
    for (int i = 0; i < count; i++) {
        lv_obj_t *row = box(s_page, MARGIN_X, 48 + i * pitch, CONTENT_W, height,
                            i == selected ? SELECT_BG : BG, 10);
        lv_obj_t *l = label(row, i == selected ? 0xFFFFFF : INK);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
        lv_label_set_text(l, labels[i]);
        if (values && values[i] && values[i][0]) {
            lv_obj_t *v = label(row, i == selected ? 0xC9D7EA : AV_UI_DIM);
            lv_obj_set_width(v, 100);
            lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -10, 0);
            lv_label_set_text(v, values[i]);
        }
    }
    page_footer(footer);
    page_end();
}

void av_ui_menu_show(const char *title, const char *const *labels, const char *const *values,
                     int count, int selected, const char *footer)
{
    menu_show(title, labels, values, count, selected, footer, 38, 34);
}

// 行数多的设置页（8 项）用更小的行距，否则最后一行会压到底部提示上（240x320 屏）
void av_ui_menu_show_dense(const char *title, const char *const *labels, const char *const *values,
                           int count, int selected, const char *footer)
{
    menu_show(title, labels, values, count, selected, footer, 29, 26);
}

void av_ui_page_show(const char *title, const char *body, const char *status, uint32_t status_color,
                     const char *footer)
{
    page_begin(title);
    lv_obj_t *b = label(s_page, INK);
    lv_obj_set_pos(b, MARGIN_X + 4, 50);
    lv_obj_set_width(b, CONTENT_W - 8);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_label_set_text(b, body ? body : "");
    if (status && status[0]) {
        lv_obj_t *s = label(s_page, status_color);
        lv_obj_set_width(s, CONTENT_W);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s, MARGIN_X, BSP_LCD_H - 82);
        lv_label_set_text(s, status);
    }
    page_footer(footer);
    page_end();
}

void av_ui_ipedit_show(const av_ipedit_t *e)
{
    page_begin("中继地址");
    lv_obj_t *hint = label(s_page, AV_UI_DIM);
    lv_obj_set_pos(hint, MARGIN_X + 4, 50);
    lv_obj_set_width(hint, CONTENT_W - 8);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "UP/DOWN 改数值\n长按 UP/DOWN 每次 ±10\nOK 下一项，端口上按 OK 保存");

    // 四段 IP：每段一个 48px 宽的格子
    for (int i = 0; i < 5; i++) {
        bool port = i == 4;
        int x = port ? 64 : MARGIN_X + 4 + i * 52;
        int y = port ? 186 : 138;
        int w = port ? 112 : 46;
        lv_obj_t *cell = box(s_page, x, y, w, 36, e->field == i ? SELECT_BG : PANEL, 8);
        lv_obj_t *v = lv_label_create(cell);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(e->field == i ? 0xFFFFFF : INK), 0);
        lv_label_set_text_fmt(v, "%u", e->value[i]);
        lv_obj_center(v);
    }
    lv_obj_t *colon = label(s_page, AV_UI_DIM);
    lv_obj_set_pos(colon, MARGIN_X + 4, 194);
    lv_label_set_text(colon, "端口");
    lv_obj_t *path = label(s_page, AV_UI_DIM);
    lv_obj_set_pos(path, MARGIN_X + 4, 232);
    lv_obj_set_width(path, CONTENT_W - 8);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(path, "路径 %s", e->path);
    page_footer("长按 OK 取消");
    page_end();
}

void av_ui_reader_show(const char *title, const char *body, int page, int pages)
{
    page_begin(title);
    lv_obj_t *text = label(s_page, INK);
    lv_obj_set_pos(text, MARGIN_X + 2, 48);
    lv_obj_set_width(text, CONTENT_W - 4);
    lv_obj_set_height(text, BSP_LCD_H - 100);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(text, 3, 0);
    lv_label_set_text(text, body ? body : "");
    char footer[80];
    snprintf(footer, sizeof(footer), "%d / %d   UP/DOWN 翻页  长按 OK 返回", page + 1, pages);
    page_footer(footer);
    page_end();
}
