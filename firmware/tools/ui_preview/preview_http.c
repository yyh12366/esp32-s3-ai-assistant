#include "preview_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "av_url.h"

enum { URL_MAX = 1024, CONNECT_TIMEOUT_MS = 2000, TEXT_TIMEOUT_MS = 8000 };

typedef struct {
    uint64_t generation;
    CURL *curl;
    char *body;
    size_t capacity;
    size_t length;
    long page_count;
    long status;
    preview_audio_stream_t *audio_stream;
    preview_http_audio_data_fn audio_data;
    bool overflow;
    bool audio_failed;
} transfer_t;

static char s_base[512];
static char s_token[256];
static preview_http_is_current_fn s_is_current;

static int build_url(CURL *curl, char *url, size_t size, const char *path)
{
    char *encoded = NULL;
    if (s_token[0]) {
        encoded = curl_easy_escape(curl, s_token, (int)strlen(s_token));
        if (!encoded) return -1;
    }
    const int length = av_media_url(url, size, s_base, path, encoded ? encoded : "");
    curl_free(encoded);
    return length;
}

static size_t receive_text(char *bytes, size_t size, size_t count, void *context)
{
    transfer_t *transfer = context;
    const size_t length = size * count;
    if (!s_is_current(transfer->generation)) return 0;
    if (length >= transfer->capacity - transfer->length) {
        transfer->overflow = true;
        return 0;
    }
    memcpy(transfer->body + transfer->length, bytes, length);
    transfer->length += length;
    transfer->body[transfer->length] = '\0';
    return length;
}

static size_t receive_header(char *bytes, size_t size, size_t count, void *context)
{
    transfer_t *transfer = context;
    const size_t length = size * count;
    static const char name[] = "X-Page-Count:";
    if (length > sizeof(name) - 1 && strncasecmp(bytes, name, sizeof(name) - 1) == 0) {
        transfer->page_count = strtol(bytes + sizeof(name) - 1, NULL, 10);
    }
    return length;
}

static size_t receive_pcm(char *bytes, size_t size, size_t count, void *context)
{
    transfer_t *transfer = context;
    const size_t length = size * count;
    if (!s_is_current(transfer->generation)) return 0;
    curl_easy_getinfo(transfer->curl, CURLINFO_RESPONSE_CODE, &transfer->status);
    if (transfer->status != 200) return 0;
    if (!preview_audio_write_pcm16le(transfer->generation, transfer->audio_stream,
                                     (const uint8_t *)bytes, length)) {
        transfer->audio_failed = true;
        return 0;
    }
    transfer->length += length;
    if (transfer->audio_data) transfer->audio_data(transfer->generation, length);
    return length;
}

static int cancel_transfer(void *context, curl_off_t total_down, curl_off_t now_down,
                           curl_off_t total_up, curl_off_t now_up)
{
    (void)total_down;
    (void)now_down;
    (void)total_up;
    (void)now_up;
    return s_is_current(((transfer_t *)context)->generation) ? 0 : 1;
}

static preview_http_result_t classify(transfer_t *transfer, CURLcode result)
{
    preview_http_result_t response = {
        .status = transfer->status,
        .page_count = transfer->page_count,
        .length = transfer->length,
    };
    if (!s_is_current(transfer->generation)) {
        response.error = PREVIEW_HTTP_CANCELLED;
    } else if (transfer->status != 0 && transfer->status != 200) {
        response.error = PREVIEW_HTTP_STATUS_ERROR;
    } else if (transfer->overflow) {
        response.error = PREVIEW_HTTP_TOO_LARGE;
    } else if (result == CURLE_URL_MALFORMAT) {
        response.error = PREVIEW_HTTP_URL_ERROR;
    } else if (transfer->status == 0 && result != CURLE_OK) {
        response.error = PREVIEW_HTTP_CONNECT_ERROR;
    } else if (result != CURLE_OK) {
        response.error = transfer->audio_failed ? PREVIEW_HTTP_AUDIO_ERROR : PREVIEW_HTTP_TRANSFER_ERROR;
    } else if (transfer->audio_stream && (transfer->audio_stream->has_low_byte || !transfer->length)) {
        response.error = PREVIEW_HTTP_PCM_ERROR;
    } else {
        response.error = PREVIEW_HTTP_OK;
    }
    return response;
}

static CURLcode perform(transfer_t *transfer, const char *path, bool pcm)
{
    transfer->curl = curl_easy_init();
    if (!transfer->curl) return CURLE_FAILED_INIT;
    char url[URL_MAX];
    if (build_url(transfer->curl, url, sizeof(url), path) < 0) {
        curl_easy_cleanup(transfer->curl);
        transfer->curl = NULL;
        return CURLE_URL_MALFORMAT;
    }
    curl_easy_setopt(transfer->curl, CURLOPT_URL, url);
    curl_easy_setopt(transfer->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(transfer->curl, CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TIMEOUT_MS);
    curl_easy_setopt(transfer->curl, CURLOPT_XFERINFOFUNCTION, cancel_transfer);
    curl_easy_setopt(transfer->curl, CURLOPT_XFERINFODATA, transfer);
    curl_easy_setopt(transfer->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(transfer->curl, CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(transfer->curl, CURLOPT_HEADERDATA, transfer);
    curl_easy_setopt(transfer->curl, CURLOPT_WRITEFUNCTION, pcm ? receive_pcm : receive_text);
    curl_easy_setopt(transfer->curl, CURLOPT_WRITEDATA, transfer);
    if (!pcm) {
        curl_easy_setopt(transfer->curl, CURLOPT_TIMEOUT_MS, TEXT_TIMEOUT_MS);
        curl_easy_setopt(transfer->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(transfer->curl, CURLOPT_LOW_SPEED_TIME, 3L);
    }
    const CURLcode result = curl_easy_perform(transfer->curl);
    curl_easy_getinfo(transfer->curl, CURLINFO_RESPONSE_CODE, &transfer->status);
    curl_easy_cleanup(transfer->curl);
    transfer->curl = NULL;
    return result;
}

bool preview_http_init(const char *base_url, const char *token, preview_http_is_current_fn is_current)
{
    if (!base_url || !base_url[0] || strlen(base_url) >= sizeof(s_base) ||
        (token && strlen(token) >= sizeof(s_token)) || !is_current) return false;
    snprintf(s_base, sizeof(s_base), "%s", base_url);
    snprintf(s_token, sizeof(s_token), "%s", token ? token : "");
    s_is_current = is_current;
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void preview_http_shutdown(void)
{
    curl_global_cleanup();
    s_is_current = NULL;
    s_token[0] = '\0';
    s_base[0] = '\0';
}

preview_http_result_t preview_http_get_text(uint64_t generation, const char *path,
                                            char *body, size_t capacity)
{
    body[0] = '\0';
    transfer_t transfer = {.generation = generation, .body = body, .capacity = capacity};
    return classify(&transfer, perform(&transfer, path, false));
}

preview_http_result_t preview_http_stream_pcm(uint64_t generation, const char *path,
                                              preview_audio_stream_t *stream,
                                              preview_http_audio_data_fn audio_data)
{
    transfer_t transfer = {.generation = generation, .audio_stream = stream,
                           .audio_data = audio_data};
    return classify(&transfer, perform(&transfer, path, true));
}
