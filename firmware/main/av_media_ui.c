#include "av_media_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "av_media.h"
#include "av_store.h"
#include "av_ui.h"

static const char *TAG = "av_media_ui";

// 12px 中文字体，正文左右各留 18px。每页 15 行、每行 17 个半角宽度。
#define READER_COLS  17
#define READER_LINES 15

typedef enum {
    MP_NONE = 0,
    MP_BOOKS,
    MP_READER,
    MP_TRACKS,
    MP_PLAYING,
} media_page_t;

static media_page_t s_page = MP_NONE;
// 两个列表各约 1KB，按需分配：语音模式不跑媒体，不该占静态 BSS。
static av_media_list_t *s_books;
static av_media_list_t *s_tracks;
static int s_cursor;
static int s_book_index;
static int s_page_no;
static int s_page_count;
static char s_body[AV_MEDIA_TEXT_MAX];
static bool s_loading;
static char s_status[64];

static void show_books(void)
{
    if (!bsp_lvgl_lock(500)) return;
    // 读取失败时 s_status 是错误原因；只有真正拿到空列表才提示没有文件
    static const char *empty[1];
    empty[0] = s_status[0] ? s_status : "还没有 TXT";
    const char *const *labels = empty;
    int count = 1;
    if (s_books && s_books->count) {
        static const char *rows[AV_MEDIA_ITEMS_MAX];
        for (int i = 0; i < s_books->count; i++) rows[i] = s_books->titles[i];
        labels = rows;
        count = s_books->count;
    }
    const bool has_books = s_books && s_books->count;
    av_ui_menu_show("小说", labels, NULL, count, s_cursor,
                    s_loading ? "读取书单…" : has_books ? "OK 打开  长按 OK 返回" : "OK 重新读取");
    bsp_lvgl_unlock();
}

static void show_reader(void)
{
    if (!bsp_lvgl_lock(500)) return;
    const char *title = (s_books && s_book_index < s_books->count) ? s_books->titles[s_book_index] : "阅读";
    av_ui_reader_show(title, s_loading ? "加载中…" : s_body, s_page_no,
                      s_page_count > 0 ? s_page_count : 1);
    bsp_lvgl_unlock();
}

static void show_tracks(void)
{
    if (!bsp_lvgl_lock(500)) return;
    // 读取失败时 s_status 是错误原因；只有真正拿到空列表才提示没有文件
    static const char *empty[1];
    empty[0] = s_status[0] ? s_status : "还没有 MP3";
    const char *const *labels = empty;
    int count = 1;
    if (s_tracks && s_tracks->count) {
        static const char *rows[AV_MEDIA_ITEMS_MAX];
        for (int i = 0; i < s_tracks->count; i++) rows[i] = s_tracks->titles[i];
        labels = rows;
        count = s_tracks->count;
    }
    const bool has_tracks = s_tracks && s_tracks->count;
    av_ui_menu_show("音乐", labels, NULL, count, s_cursor,
                    s_loading ? "读取歌单…" : has_tracks ? "OK 播放  长按 OK 返回" : "OK 重新读取");
    bsp_lvgl_unlock();
}

static void show_playing(void)
{
    if (!bsp_lvgl_lock(500)) return;
    char body[160];
    const char *title = (s_tracks && s_cursor < s_tracks->count) ? s_tracks->titles[s_cursor] : "";
    snprintf(body, sizeof(body), "%s\n\n已播放 %d 秒\n\nOK 暂停/继续\nUP/DOWN 切歌",
             title, av_media_elapsed_s());
    av_ui_page_show("音乐", body, s_status[0] ? s_status : NULL, 0x4FC3F7, "长按 OK 返回");
    bsp_lvgl_unlock();
}

void av_media_ui_init(void)
{
    s_page = MP_NONE;
    if (!s_books) s_books = heap_caps_calloc(1, sizeof(*s_books), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_tracks) s_tracks = heap_caps_calloc(1, sizeof(*s_tracks), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool av_media_ui_active(void) { return s_page != MP_NONE; }

void av_media_ui_open_books(void)
{
    s_page = MP_BOOKS;
    s_loading = true;
    s_status[0] = '\0';
    int book = 0, page = 0;
    if (av_store_get_read_pos(&book, &page) && book < AV_MEDIA_ITEMS_MAX) {
        s_book_index = book;
        s_page_no = page;
    }
    if (!av_media_request_books()) {
        s_loading = false;
        snprintf(s_status, sizeof(s_status), "媒体服务不可用");
    }
    show_books();
}

void av_media_ui_open_music(void)
{
    s_page = MP_TRACKS;
    s_cursor = 0;
    s_loading = true;
    s_status[0] = '\0';
    if (!av_media_request_tracks()) {
        s_loading = false;
        snprintf(s_status, sizeof(s_status), "媒体服务不可用");
    }
    show_tracks();
}

static void request_page(int page)
{
    s_page_no = page;
    s_loading = true;
    s_body[0] = '\0';
    av_media_request_book_page(s_book_index, page, READER_COLS, READER_LINES);
    show_reader();
}

static void open_reader(void)
{
    if (!s_books || !s_books->count) return;
    s_book_index = s_cursor;
    s_page = MP_READER;
    request_page(s_page_no);
}

static void play_track(void)
{
    if (!s_tracks || !s_tracks->count) return;
    s_page = MP_PLAYING;
    s_status[0] = '\0';
    av_media_request_play(s_cursor);
    show_playing();
}

static void back_to_list(void)
{
    switch (s_page) {
    case MP_READER:
        s_page = MP_BOOKS;
        s_cursor = s_book_index;
        show_books();
        break;
    case MP_PLAYING:
        av_media_request_stop();
        s_page = MP_TRACKS;
        show_tracks();
        break;
    default:
        s_page = MP_NONE;
        break;
    }
}

bool av_media_ui_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_page == MP_NONE) return false;
    const bool click = ev == BSP_BTN_CLICK;
    const bool longp = ev == BSP_BTN_LONG;

    if (btn == BSP_BTN_OK && longp) {
        back_to_list();
        return true;
    }

    switch (s_page) {
    case MP_BOOKS:
        if (btn == BSP_BTN_UP && click && s_books && s_books->count) {
            s_cursor = (s_cursor + s_books->count - 1) % s_books->count;
            show_books();
        } else if (btn == BSP_BTN_DOWN && click && s_books && s_books->count) {
            s_cursor = (s_cursor + 1) % s_books->count;
            show_books();
        } else if (btn == BSP_BTN_OK && click) {
            // 列表为空（多半是上次没连上）时 OK = 重新读取
            if (s_books && s_books->count) open_reader();
            else if (!s_loading) av_media_ui_open_books();
        }
        return true;

    case MP_READER:
        if (btn == BSP_BTN_UP && click && s_page_no > 0) {
            request_page(s_page_no - 1);
        } else if (btn == BSP_BTN_DOWN && click && s_page_no + 1 < s_page_count) {
            request_page(s_page_no + 1);
        }
        return true;

    case MP_TRACKS:
        if (btn == BSP_BTN_UP && click && s_tracks && s_tracks->count) {
            s_cursor = (s_cursor + s_tracks->count - 1) % s_tracks->count;
            show_tracks();
        } else if (btn == BSP_BTN_DOWN && click && s_tracks && s_tracks->count) {
            s_cursor = (s_cursor + 1) % s_tracks->count;
            show_tracks();
        } else if (btn == BSP_BTN_OK && click) {
            if (s_tracks && s_tracks->count) play_track();
            else if (!s_loading) av_media_ui_open_music();
        }
        return true;

    case MP_PLAYING:
        if (btn == BSP_BTN_OK && click) {
            if (av_media_playing()) {
                av_media_request_stop();
                snprintf(s_status, sizeof(s_status), "已暂停");
            } else {
                av_media_request_play(s_cursor);
                s_status[0] = '\0';
            }
            show_playing();
        } else if (btn == BSP_BTN_DOWN && click && s_tracks && s_tracks->count) {
            av_media_request_stop();
            s_cursor = (s_cursor + 1) % s_tracks->count;
            play_track();
        } else if (btn == BSP_BTN_UP && click && s_tracks && s_tracks->count) {
            av_media_request_stop();
            s_cursor = (s_cursor + s_tracks->count - 1) % s_tracks->count;
            play_track();
        }
        return true;

    default:
        return false;
    }
}

void av_media_ui_poll(void)
{
    const av_media_result_t *r = av_media_take();
    if (!r) return;

    switch (r->kind) {
    case AV_MEDIA_RES_BOOKS:
        *s_books = r->list;
        s_loading = false;
        if (s_cursor >= s_books->count) s_cursor = 0;
        if (s_page == MP_BOOKS) show_books();
        ESP_LOGI(TAG, "books loaded: %d", s_books->count);
        break;

    case AV_MEDIA_RES_TRACKS:
        *s_tracks = r->list;
        s_loading = false;
        if (s_cursor >= s_tracks->count) s_cursor = 0;
        if (s_page == MP_TRACKS) show_tracks();
        ESP_LOGI(TAG, "tracks loaded: %d", s_tracks->count);
        break;

    case AV_MEDIA_RES_BOOK_PAGE:
        s_loading = false;
        s_page_count = r->pages > 0 ? r->pages : 1;
        s_page_no = r->page;
        snprintf(s_body, sizeof(s_body), "%s", r->text);
        av_store_set_read_pos(s_book_index, s_page_no);
        if (s_page == MP_READER) show_reader();
        break;

    case AV_MEDIA_RES_PLAY_STARTED:
        if (s_page == MP_PLAYING) {
            s_status[0] = '\0';
            show_playing();
        }
        break;

    case AV_MEDIA_RES_PLAY_ENDED:
        if (s_page == MP_PLAYING) {
            snprintf(s_status, sizeof(s_status), "播放结束");
            show_playing();
        }
        break;

    case AV_MEDIA_RES_ERROR:
        s_loading = false;
        snprintf(s_status, sizeof(s_status), "%s", r->error);
        ESP_LOGW(TAG, "media error: %s", r->error);
        if (s_page == MP_BOOKS) show_books();
        else if (s_page == MP_TRACKS) show_tracks();
        else if (s_page == MP_READER) show_reader();
        else if (s_page == MP_PLAYING) show_playing();
        break;

    default:
        break;
    }
}

void av_media_ui_stop_playback(void)
{
    if (av_media_playing()) av_media_request_stop();
}

void av_media_ui_close(void)
{
    av_media_ui_stop_playback();
    s_page = MP_NONE;
}
