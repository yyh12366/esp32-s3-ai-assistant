// main/av_media.h —— 小说(TXT)与音乐(MP3→PCM)的媒体客户端。
//
// 数据源是媒体 HTTP 服务（本机 = Mac 上 astrbot-relay 的 8766 端口；la = 远端服务器的 /media）：
//   GET /books                  纯文本，每行一个书名，行号即索引
//   GET /books/<i>?page=&cols=&lines=  该页正文，响应头 X-Page-Count
//   GET /music                  纯文本，每行一个曲名
//   GET /music/<i>.adpcm        16kHz/单声道 IMA ADPCM 块流（中继用 ffmpeg 转码后编码）
//
// 所有网络操作都在独立 worker 任务里跑，绝不阻塞应用任务。结果用 av_media_take() 轮询。
// 音乐 PCM 直接喂 av_audio_feed()，复用对讲那套环形缓冲与播放任务，避免两路抢 I2S。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AV_MEDIA_ITEMS_MAX   16
#define AV_MEDIA_TITLE_MAX   64
#define AV_MEDIA_TEXT_MAX    900    // 一页正文的 UTF-8 字节上限（无 PSRAM，省内部 RAM）
#define AV_MEDIA_PORT_DEFAULT 8766
#define AV_MEDIA_BASE_MAX    128    // 媒体基址 http://host[:port][/prefix]
#define AV_MEDIA_TOKEN_MAX   96     // 鉴权 token（来自中继地址的 ?token=，服务端开启鉴权时必填）

typedef struct {
    char titles[AV_MEDIA_ITEMS_MAX][AV_MEDIA_TITLE_MAX];
    int count;
} av_media_list_t;

typedef enum {
    AV_MEDIA_RES_NONE = 0,
    AV_MEDIA_RES_BOOKS,      // list
    AV_MEDIA_RES_TRACKS,     // list
    AV_MEDIA_RES_BOOK_PAGE,  // text + page + pages
    AV_MEDIA_RES_PLAY_STARTED,
    AV_MEDIA_RES_PLAY_ENDED,
    AV_MEDIA_RES_ERROR,      // error
} av_media_res_kind_t;

typedef struct {
    av_media_res_kind_t kind;
    av_media_list_t list;
    char text[AV_MEDIA_TEXT_MAX];
    char error[64];
    int index;
    int page;
    int pages;
    int duration_s;
} av_media_result_t;

// base_url 是媒体基址（不含路径），如 http://<host>:8766 或 http://<host>/media；
// token 为空串时不带 token 参数。每个请求都会附加 token=<token>（Mac 的独立媒体服务忽略它）。
esp_err_t av_media_start(const char *base_url, const char *token);
// 服务器切换（本机 / la）后改用新的基址和 token。未启动时等同于 av_media_start。
// 只在媒体页不在前台时调用：正在进行的请求可能还用旧地址。
esp_err_t av_media_set_base(const char *base_url, const char *token);

// 以下请求都是异步的：排队后立刻返回，结果通过 av_media_take() 取。
bool av_media_request_books(void);
bool av_media_request_tracks(void);
bool av_media_request_book_page(int index, int page, int cols, int lines);
bool av_media_request_play(int index);
void av_media_request_stop(void);

bool av_media_playing(void);
int av_media_elapsed_s(void);

// 返回内部静态结果，取走后置空；无结果返回 NULL。只能在应用任务里调用。
const av_media_result_t *av_media_take(void);
