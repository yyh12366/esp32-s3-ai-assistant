#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdio.h>
#include <string.h>

#include "av_ui.h"
#include "bsp_pins.h"
#include "kk_render.h"
#include "lvgl.h"
#include "preview_audio.h"
#include "preview_cli.h"
#include "preview_media.h"

enum { WIDTH = BSP_LCD_W, HEIGHT = BSP_LCD_H, HOME = PREVIEW_HOME, VOICE = PREVIEW_VOICE,
       BOOKS = PREVIEW_BOOKS, MUSIC = PREVIEW_MUSIC, READER = PREVIEW_READER,
       PLAYING = PREVIEW_PLAYING, SETTINGS = PREVIEW_SETTINGS, MONITOR = PREVIEW_MONITOR,
       VIDEO = PREVIEW_VIDEO, VISIBLE_ROWS = 6 };

static uint16_t pixels[WIDTH * HEIGHT];
static uint16_t draw_buffer[WIDTH * 40];
static const char *const home[] = {"大肥鱼", "小说", "音乐", "设置", "监测", "Video"};
static const int home_pages[] = {VOICE, BOOKS, MUSIC, SETTINGS, MONITOR, VIDEO};
static const char *const settings[] = {"中继地址", "服务器", "蓝牙配网", "音量", "清除 Wi-Fi", "固件升级", "设备信息", "返回"};
static kk_state_t kk;
static kk_anim_t anim;
static kk_fb_t mono;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    const size_t width = (size_t)(area->x2 - area->x1 + 1);
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(pixels + y * WIDTH + area->x1, data, width * sizeof(uint16_t));
        data += width * sizeof(uint16_t);
    }
    lv_display_flush_ready(display);
}

static void render_monitor(void)
{
    const uint64_t now = SDL_GetTicks();
    kk_anim_target_t target;
    kk_render_anim_target(&kk, now, &target);
    kk_anim_step(&anim, &target, 16);
    const kk_telemetry_t telemetry = {
        .battery_mv = -1, .battery_soc = -1, .uptime_ms = now, .fps = -1,
        .adc_mv = -1, .mic_level_pct = -1, .board_id = "Desktop preview",
    };
    kk_render_frame(&kk, &anim, &telemetry, &mono, now);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            const unsigned index = (unsigned)(y / 2) * (WIDTH / 2) + (unsigned)(x / 2);
            pixels[y * WIDTH + x] = (mono.pixels[index / 8] & (0x80 >> (index % 8))) ? 0xffff : 0;
        }
    }
}

static void show_list(const char *title, const char titles[][PREVIEW_MEDIA_TITLE_MAX], int item_count,
                      const char *empty, bool loading, const char *error, int selection,
                      const char *action)
{
    const char *rows[VISIBLE_ROWS];
    int count = item_count;
    int visible_selection = 0;
    if (loading) {
        rows[0] = "读取中…";
        count = 1;
    } else if (error && error[0]) {
        rows[0] = error;
        count = 1;
    } else if (!count) {
        rows[0] = empty;
        count = 1;
    } else {
        const int first = selection >= VISIBLE_ROWS ? selection - VISIBLE_ROWS + 1 : 0;
        count = item_count - first < VISIBLE_ROWS ? item_count - first : VISIBLE_ROWS;
        for (int i = 0; i < count; i++) rows[i] = titles[first + i];
        visible_selection = selection - first;
    }
    const char *footer = loading ? "正在读取…" : (error && error[0]) ? "Enter重试 Esc返回"
                          : item_count ? action : "Enter重试 Esc返回";
    av_ui_menu_show(title, rows, NULL, count, visible_selection, footer);
}

static void show(int page, int selection, int setting, int book_index, const char *play_error,
                 const preview_media_state_t *media)
{
    switch (page) {
    case HOME:
        av_ui_menu_show("首页", home, NULL, 6, selection, "上下选择 Enter进入");
        break;
    case VOICE:
        av_ui_main_status("电脑预览", AV_UI_AMBER, NULL, -1);
        av_ui_main_asr("语音中继未接入", true);
        av_ui_main_show();
        break;
    case BOOKS:
        show_list("小说", media->books, media->book_count, "还没有 TXT", media->loading,
                  media->error, selection, "上下选择 Enter打开");
        break;
    case MUSIC:
        show_list("音乐", media->tracks, media->track_count, "还没有 MP3", media->loading,
                  media->error, selection, "上下选择 Enter播放");
        break;
    case READER: {
        const char *title = book_index < media->book_count ? media->books[book_index] : "阅读";
        const char *body = media->loading ? "加载中…" : media->error[0] ? media->error : media->body;
        const int pages = media->pages > 0 ? media->pages : 1;
        char footer[48];
        snprintf(footer, sizeof(footer), "%d/%d 上下翻页 Esc返回", media->page + 1, pages);
        av_ui_page_show(title, body, NULL, AV_UI_BLUE, footer);
        break;
    }
    case PLAYING: {
        const char *title = media->playing_index < media->track_count
                                ? media->tracks[media->playing_index] : "音乐";
        const char *status = play_error && play_error[0] ? play_error : media->error;
        if (!status || !status[0]) {
            switch (media->playback) {
            case PREVIEW_PLAY_CONNECTING: status = "正在连接媒体服务"; break;
            case PREVIEW_PLAY_ACTIVE: status = "播放中"; break;
            case PREVIEW_PLAY_PAUSED: status = "已暂停"; break;
            case PREVIEW_PLAY_ENDED: status = "播放完成"; break;
            case PREVIEW_PLAY_ERROR: status = "播放失败"; break;
            default: status = "等待播放"; break;
            }
        }
        char body[384];
        snprintf(body, sizeof(body), "%s\n\n%s", title, status);
        av_ui_page_show("音乐", body, NULL, AV_UI_BLUE, "Enter暂停 Esc返回");
        break;
    }
    case SETTINGS:
        av_ui_menu_show_dense("设置", settings, NULL, 8, setting, "Esc返回");
        break;
    case MONITOR:
        render_monitor();
        return;
    case VIDEO:
        av_ui_page_show("Video", "桌面预览不支持视频", NULL, AV_UI_DIM, "Esc返回");
        break;
    }
    lv_refr_now(NULL);
}

void preview_show_capture(int page, const preview_media_state_t *media)
{
    show(page, 0, 0, 0, NULL, media);
}

uint16_t *preview_capture_pixels(void) { return pixels; }

int main(int argc, char **argv)
{
    const bool check = argc > 1 && strcmp(argv[1], "--check") == 0;
    const bool check_audio = argc > 1 && strcmp(argv[1], "--check-audio") == 0;
    const bool screenshots = argc > 1 && strcmp(argv[1], "--screenshots") == 0 && argc > 2;
    if (argc > 1 && !check && !check_audio && !screenshots) {
        fprintf(stderr, "usage: %s [--check | --check-audio | --screenshots DIR]\n", argv[0]);
        return 2;
    }
    if (check_audio) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    const uint32_t sdl_flags = SDL_INIT_TIMER | ((check || screenshots) ? 0 : SDL_INIT_AUDIO) |
                               ((!check && !check_audio && !screenshots) ? SDL_INIT_VIDEO : 0);
    if (SDL_Init(sdl_flags) != 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!check && !screenshots) preview_audio_init();
    lv_init();
    lv_tick_set_cb(SDL_GetTicks);
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    av_ui_init();
    kk_model_init(&kk);
    kk_anim_target_t target;
    kk_render_anim_target(&kk, 0, &target);
    kk_anim_init(&anim, &target);

    const char *base = getenv("AV_MEDIA_BASE_URL");
    const char *token = getenv("AV_MEDIA_TOKEN");
    if (!preview_media_init(base, token)) {
        fprintf(stderr, "media worker init failed\n");
        preview_audio_shutdown();
        SDL_Quit();
        return 1;
    }
    preview_media_state_t media;
    preview_media_snapshot(&media);
    if (check || check_audio || screenshots) {
        const int result = check ? preview_cli_check(token, &media)
                           : check_audio ? preview_cli_check_audio(token, &media)
                                         : preview_cli_screenshots(argv[2], &media);
        preview_media_shutdown();
        preview_audio_shutdown();
        SDL_Quit();
        return result;
    }

    SDL_Window *window = SDL_CreateWindow("ESP32-S3 desktop preview — arrows, Enter, Esc",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WIDTH * 2, HEIGHT * 2, 0);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : NULL;
    SDL_Texture *texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                                       SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT) : NULL;
    if (!texture) {
        fprintf(stderr, "SDL window init failed: %s\n", SDL_GetError());
        preview_media_shutdown();
        preview_audio_shutdown();
        SDL_Quit();
        return 1;
    }
    int page = HOME, home_selection = 0, selection = 0, setting = 0, book_index = 0;
    char play_error[64] = "";
    uint64_t shown_revision = media.revision;
    show(page, home_selection, setting, book_index, play_error, &media);
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                preview_media_cancel();
                running = false;
                break;
            }
            if (event.type != SDL_KEYDOWN) continue;
            const SDL_Keycode key = event.key.keysym.sym;
            const int direction = (key == SDLK_DOWN || key == SDLK_s) -
                                  (key == SDLK_UP || key == SDLK_w);
            const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_e;
            if (key == SDLK_ESCAPE) {
                preview_media_cancel();
                play_error[0] = '\0';
                if (page == READER) {
                    page = BOOKS;
                    selection = book_index;
                } else if (page == PLAYING) {
                    page = MUSIC;
                } else {
                    page = HOME;
                    selection = home_selection;
                }
            } else if (page == HOME && direction) {
                home_selection = (home_selection + direction + 6) % 6;
                selection = home_selection;
            } else if (page == HOME && enter) {
                page = home_pages[home_selection];
                selection = 0;
                play_error[0] = '\0';
                if (page == BOOKS) preview_media_request_books();
                if (page == MUSIC) preview_media_request_tracks();
            } else if (page == BOOKS && direction && media.book_count) {
                selection = (selection + direction + media.book_count) % media.book_count;
            } else if (page == BOOKS && enter) {
                if (media.loading) continue;
                if (media.error[0] || !media.book_count) {
                    preview_media_request_books();
                } else {
                    book_index = selection;
                    page = READER;
                    preview_media_request_book_page(book_index, 0);
                }
            } else if (page == READER && direction) {
                const int next = media.page + direction;
                if (!media.loading && next >= 0 && next < media.pages)
                    preview_media_request_book_page(book_index, next);
            } else if (page == MUSIC && direction && media.track_count) {
                selection = (selection + direction + media.track_count) % media.track_count;
            } else if (page == MUSIC && enter) {
                if (media.loading) continue;
                if (media.error[0] || !media.track_count) {
                    preview_media_request_tracks();
                } else {
                    page = PLAYING;
                    if (!preview_media_request_play(selection))
                        snprintf(play_error, sizeof(play_error), "音频设备不可用");
                    else play_error[0] = '\0';
                }
            } else if (page == PLAYING && enter) {
                if (play_error[0] || media.playback == PREVIEW_PLAY_ENDED ||
                    media.playback == PREVIEW_PLAY_ERROR) {
                    if (preview_media_request_play(selection)) play_error[0] = '\0';
                } else {
                    preview_media_toggle_pause();
                }
            } else if (page == SETTINGS && direction) {
                setting = (setting + direction + 8) % 8;
            } else if (page == SETTINGS && enter && setting == 7) {
                page = HOME;
                selection = home_selection;
            }
            preview_media_snapshot(&media);
            show(page, selection, setting, book_index, play_error, &media);
            shown_revision = media.revision;
        }
        preview_media_snapshot(&media);
        if (media.revision != shown_revision) {
            show(page, selection, setting, book_index, play_error, &media);
            shown_revision = media.revision;
        }
        if (page == MONITOR) render_monitor();
        else lv_timer_handler();
        SDL_UpdateTexture(texture, NULL, pixels, WIDTH * 2);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    preview_media_shutdown();
    preview_audio_shutdown();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
