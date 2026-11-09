#include "preview_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "preview_audio.h"
#include "preview_http.h"

enum { LIST_BODY_MAX = 8192 };

typedef enum { REQUEST_BOOKS, REQUEST_TRACKS, REQUEST_PAGE, REQUEST_PLAY } request_kind_t;
typedef struct {
    request_kind_t kind;
    int index;
    int page;
    uint64_t generation;
} media_request_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ready = PTHREAD_COND_INITIALIZER;
static pthread_t s_worker;
static bool s_started;
static bool s_shutdown;
static bool s_has_pending;
static uint64_t s_generation;
static media_request_t s_pending;
static preview_media_state_t s_state;

static bool is_current(uint64_t generation)
{
    pthread_mutex_lock(&s_lock);
    const bool current = !s_shutdown && generation == s_generation;
    pthread_mutex_unlock(&s_lock);
    return current;
}

static void finish_error(uint64_t generation, const char *error, bool playback)
{
    preview_audio_cancel(generation);
    pthread_mutex_lock(&s_lock);
    if (generation == s_generation) {
        s_state.loading = false;
        snprintf(s_state.error, sizeof(s_state.error), "%s", error);
        if (playback) s_state.playback = PREVIEW_PLAY_ERROR;
        s_state.revision++;
    }
    pthread_mutex_unlock(&s_lock);
}

static bool finish_http_error(uint64_t generation, preview_http_result_t response, bool playback)
{
    if (response.error == PREVIEW_HTTP_OK) return false;
    if (response.error == PREVIEW_HTTP_CANCELLED) return true;
    char error[32];
    switch (response.error) {
    case PREVIEW_HTTP_STATUS_ERROR:
        snprintf(error, sizeof(error), "HTTP %ld", response.status);
        break;
    case PREVIEW_HTTP_CONNECT_ERROR: snprintf(error, sizeof(error), "连接失败"); break;
    case PREVIEW_HTTP_TOO_LARGE: snprintf(error, sizeof(error), "响应过长"); break;
    case PREVIEW_HTTP_URL_ERROR: snprintf(error, sizeof(error), "地址无效"); break;
    case PREVIEW_HTTP_AUDIO_ERROR: snprintf(error, sizeof(error), "音频队列失败"); break;
    case PREVIEW_HTTP_PCM_ERROR: snprintf(error, sizeof(error), "PCM 数据不完整"); break;
    default: snprintf(error, sizeof(error), playback ? "播放连接中断" : "读取失败"); break;
    }
    finish_error(generation, error, playback);
    return true;
}

static size_t utf8_prefix(const char *text, size_t length, size_t capacity)
{
    if (length < capacity) return length;
    length = capacity - 1;
    while (length && ((unsigned char)text[length] & 0xC0) == 0x80) length--;
    return length;
}

static void parse_list(const char *body, char titles[][PREVIEW_MEDIA_TITLE_MAX], int *count)
{
    const char *line = body;
    *count = 0;
    while (*line && *count < PREVIEW_MEDIA_ITEMS_MAX) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length && line[length - 1] == '\r') length--;
        if (length) {
            length = utf8_prefix(line, length, PREVIEW_MEDIA_TITLE_MAX);
            memcpy(titles[*count], line, length);
            titles[*count][length] = '\0';
            (*count)++;
        }
        if (!end) break;
        line = end + 1;
    }
}

static void finish_text(const media_request_t *request, preview_http_result_t response,
                        const char *body)
{
    if (finish_http_error(request->generation, response, false)) return;
    pthread_mutex_lock(&s_lock);
    if (request->generation == s_generation) {
        s_state.error[0] = '\0';
        s_state.loading = false;
        if (request->kind == REQUEST_BOOKS) {
            parse_list(body, s_state.books, &s_state.book_count);
        } else if (request->kind == REQUEST_TRACKS) {
            parse_list(body, s_state.tracks, &s_state.track_count);
        } else {
            snprintf(s_state.body, sizeof(s_state.body), "%s", body);
            s_state.page = request->page;
            s_state.pages = response.page_count > 0 ? (int)response.page_count : 1;
        }
        s_state.revision++;
    }
    pthread_mutex_unlock(&s_lock);
}

static void run_text_request(const media_request_t *request)
{
    char body[LIST_BODY_MAX];
    char path[128];
    if (request->kind == REQUEST_BOOKS) {
        snprintf(path, sizeof(path), "/books");
    } else if (request->kind == REQUEST_TRACKS) {
        snprintf(path, sizeof(path), "/music");
    } else {
        snprintf(path, sizeof(path), "/books/%d?page=%d&cols=17&lines=10",
                 request->index, request->page);
    }
    const preview_http_result_t response = preview_http_get_text(
        request->generation, path, body, sizeof(body));
    finish_text(request, response, body);
}

static void report_audio_data(uint64_t generation, size_t bytes)
{
    pthread_mutex_lock(&s_lock);
    if (generation == s_generation) {
        if (s_state.playback == PREVIEW_PLAY_CONNECTING) s_state.playback = PREVIEW_PLAY_ACTIVE;
        s_state.playback_bytes += bytes;
        s_state.revision++;
    }
    pthread_mutex_unlock(&s_lock);
}

static void run_audio_request(const media_request_t *request)
{
    char path[64];
    snprintf(path, sizeof(path), "/music/%d.pcm", request->index);
    preview_audio_stream_t stream = {0};
    const preview_http_result_t response = preview_http_stream_pcm(
        request->generation, path, &stream, report_audio_data);
    if (finish_http_error(request->generation, response, true)) return;
    preview_audio_wait_drained(request->generation);
    if (!is_current(request->generation)) return;
    preview_audio_cancel(request->generation);
    pthread_mutex_lock(&s_lock);
    if (request->generation == s_generation) {
        s_state.loading = false;
        s_state.playback = PREVIEW_PLAY_ENDED;
        s_state.revision++;
    }
    pthread_mutex_unlock(&s_lock);
}

static void *worker(void *unused)
{
    (void)unused;
    // One worker serializes requests; generation changes cancel in-flight work without blocking the UI.
    for (;;) {
        pthread_mutex_lock(&s_lock);
        while (!s_shutdown && !s_has_pending) pthread_cond_wait(&s_ready, &s_lock);
        if (s_shutdown) {
            pthread_mutex_unlock(&s_lock);
            return NULL;
        }
        const media_request_t request = s_pending;
        s_has_pending = false;
        pthread_mutex_unlock(&s_lock);
        if (request.kind == REQUEST_PLAY) run_audio_request(&request);
        else run_text_request(&request);
    }
}

bool preview_media_init(const char *base_url, const char *token)
{
    const char *base = base_url && base_url[0] ? base_url : getenv("AV_MEDIA_BASE_URL");
    const char *key = token && token[0] ? token : getenv("AV_MEDIA_TOKEN");
    if (!base || !base[0]) base = "http://127.0.0.1:8766";
    pthread_mutex_lock(&s_lock);
    memset(&s_state, 0, sizeof(s_state));
    s_generation = 0;
    s_shutdown = false;
    s_has_pending = false;
    pthread_mutex_unlock(&s_lock);
    if (!preview_http_init(base, key, is_current)) return false;
    if (pthread_create(&s_worker, NULL, worker, NULL) != 0) {
        preview_http_shutdown();
        return false;
    }
    s_started = true;
    return true;
}

static void submit(request_kind_t kind, int index, int page)
{
    pthread_mutex_lock(&s_lock);
    if (!s_started || s_shutdown) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    const uint64_t generation = ++s_generation;
    s_pending = (media_request_t){.kind = kind, .index = index, .page = page,
                                  .generation = generation};
    s_has_pending = false;
    s_state.loading = true;
    s_state.error[0] = '\0';
    if (kind == REQUEST_BOOKS) s_state.book_count = 0;
    if (kind == REQUEST_TRACKS) s_state.track_count = 0;
    if (kind == REQUEST_PAGE) {
        s_state.body[0] = '\0';
        s_state.page = page;
        s_state.pages = 1;
    }
    if (kind == REQUEST_PLAY) {
        s_state.playing_index = index;
        s_state.playback_bytes = 0;
        s_state.playback = PREVIEW_PLAY_CONNECTING;
    } else {
        s_state.playback = PREVIEW_PLAY_IDLE;
    }
    s_state.revision++;
    pthread_mutex_unlock(&s_lock);

    preview_audio_cancel(generation);
    if (kind == REQUEST_PLAY) preview_audio_begin(generation);
    pthread_mutex_lock(&s_lock);
    if (generation == s_generation && !s_shutdown) {
        s_has_pending = true;
        pthread_cond_signal(&s_ready);
    }
    pthread_mutex_unlock(&s_lock);
}

void preview_media_request_books(void) { submit(REQUEST_BOOKS, 0, 0); }
void preview_media_request_tracks(void) { submit(REQUEST_TRACKS, 0, 0); }
void preview_media_request_book_page(int index, int page) { submit(REQUEST_PAGE, index, page); }

bool preview_media_request_play(int index)
{
    if (!preview_audio_available()) return false;
    submit(REQUEST_PLAY, index, 0);
    return true;
}

void preview_media_toggle_pause(void)
{
    pthread_mutex_lock(&s_lock);
    const uint64_t generation = s_generation;
    const preview_play_state_t playback = s_state.playback;
    pthread_mutex_unlock(&s_lock);
    if (playback != PREVIEW_PLAY_ACTIVE && playback != PREVIEW_PLAY_PAUSED) return;
    const bool paused = playback == PREVIEW_PLAY_ACTIVE;
    if (!preview_audio_set_paused(generation, paused)) return;
    pthread_mutex_lock(&s_lock);
    if (generation == s_generation) {
        s_state.playback = paused ? PREVIEW_PLAY_PAUSED : PREVIEW_PLAY_ACTIVE;
        s_state.revision++;
    }
    pthread_mutex_unlock(&s_lock);
}

void preview_media_cancel(void)
{
    pthread_mutex_lock(&s_lock);
    const uint64_t generation = ++s_generation;
    s_has_pending = false;
    s_state.loading = false;
    s_state.error[0] = '\0';
    s_state.playback = PREVIEW_PLAY_IDLE;
    s_state.revision++;
    pthread_cond_broadcast(&s_ready);
    pthread_mutex_unlock(&s_lock);
    preview_audio_cancel(generation);
}

void preview_media_snapshot(preview_media_state_t *state)
{
    pthread_mutex_lock(&s_lock);
    *state = s_state;
    pthread_mutex_unlock(&s_lock);
}

void preview_media_shutdown(void)
{
    if (!s_started) return;
    preview_media_cancel();
    pthread_mutex_lock(&s_lock);
    s_shutdown = true;
    pthread_cond_signal(&s_ready);
    pthread_mutex_unlock(&s_lock);
    pthread_join(s_worker, NULL);
    s_started = false;
    preview_http_shutdown();
}
