#include "av_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "av_app.h"
#include "av_adpcm.h"
#include "av_audio.h"
#include "av_proto.h"
#include "av_session.h"
#include "av_url.h"
#include "av_ws.h"

static const char *TAG = "av_net";

#define CONNECT_TIMEOUT_MS   5000
#define HANDSHAKE_TIMEOUT_MS 5000
#define IDLE_TIMEOUT_MS      45000   // 中继每 20s ping 一次；这么久没有任何数据视为断线
#define SEND_TIMEOUT_MS      3000
#define TEXT_MAX             2048
#define MAX_FRAME            (64 * 1024)

typedef struct {
    char url[AV_URL_MAX];
    char device_id[32];
    uint32_t gen;
} request_t;

static SemaphoreHandle_t s_lock;       // 保护 s_req / s_pending / s_sock
static SemaphoreHandle_t s_send_lock;  // 串行化发送
static TaskHandle_t s_task;
static request_t s_req;
static bool s_pending;
static volatile bool s_stop;           // 应用要求断开当前连接
static int s_sock = -1;

// 接收侧状态（只在网络任务里访问）
static char s_text[TEXT_MAX + 1];
static size_t s_text_len;
static bool s_text_overflow;
static uint8_t s_ctrl[125];
static size_t s_ctrl_len;
static bool s_closed_by_peer;
static bool s_ping_pending;
static bool s_audio_receiving;
static uint32_t s_audio_bytes;
static TickType_t s_audio_first_rx;
static TickType_t s_audio_last_rx;
static TickType_t s_audio_max_gap;
static bool s_adpcm;
static uint8_t s_audio_block[AV_ADPCM_MAX_BYTES];
static size_t s_audio_block_len;
static bool s_audio_block_bad;
static int16_t s_decoded_audio[AV_ADPCM_MAX_SAMPLES];

static uint8_t s_tx[AV_WS_HEADER_MAX + AV_AUDIO_FRAME_BYTES];

static esp_err_t send_all(int sock, const uint8_t *data, size_t len)
{
    while (len) {
        int n = send(sock, data, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return ESP_FAIL;
        }
        data += n;
        len -= (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t send_frame(uint8_t opcode, const uint8_t *payload, size_t len)
{
    if (len > AV_AUDIO_FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    int sock = s_sock;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (sock >= 0) {
        uint8_t mask[4];
        esp_fill_random(mask, sizeof(mask));
        size_t h = av_ws_frame_header(s_tx, opcode, len, mask);
        memcpy(s_tx + h, payload, len);
        av_ws_apply_mask(s_tx + h, len, mask, 0);
        err = send_all(sock, s_tx, h + len);
    }
    xSemaphoreGive(s_send_lock);
    return err;
}

esp_err_t av_net_send_text(const char *json, size_t len)
{
    return send_frame(AV_WS_OP_TEXT, (const uint8_t *)json, len);
}

esp_err_t av_net_send_binary(const uint8_t *data, size_t len)
{
    return send_frame(AV_WS_OP_BIN, data, len);
}

// ---- 接收回调 ----
static void on_begin(void *user, uint8_t op, uint64_t len)
{
    (void)user;
    (void)len;
    if (op == AV_WS_OP_TEXT) {
        s_text_len = 0;
        s_text_overflow = false;
    } else if (op == AV_WS_OP_BIN) {
        s_audio_block_len = 0;
        s_audio_block_bad = false;
    } else if (op & 0x08) {
        s_ctrl_len = 0;
    }
}

static void feed_audio(const uint8_t *data, size_t len)
{
    if (s_audio_receiving) {
        TickType_t now = xTaskGetTickCount();
        if (!s_audio_bytes) {
            s_audio_first_rx = now;
        } else if (now - s_audio_last_rx > s_audio_max_gap) {
            s_audio_max_gap = now - s_audio_last_rx;
        }
        s_audio_last_rx = now;
        s_audio_bytes += (uint32_t)len;
    }
    av_audio_feed(data, len);
}

static void on_data(void *user, uint8_t op, const uint8_t *data, size_t len)
{
    (void)user;
    if (op == AV_WS_OP_BIN) {
        if (!s_adpcm) {
            feed_audio(data, len);
        } else if (len > sizeof(s_audio_block) - s_audio_block_len) {
            s_audio_block_bad = true;
        } else if (!s_audio_block_bad) {
            memcpy(s_audio_block + s_audio_block_len, data, len);
            s_audio_block_len += len;
        }
    } else if (op == AV_WS_OP_TEXT) {
        if (s_text_len + len > TEXT_MAX) {
            s_text_overflow = true;
            return;
        }
        memcpy(s_text + s_text_len, data, len);
        s_text_len += len;
    } else if (op & 0x08) {
        size_t n = len;
        if (s_ctrl_len + n > sizeof(s_ctrl)) n = sizeof(s_ctrl) - s_ctrl_len;
        memcpy(s_ctrl + s_ctrl_len, data, n);
        s_ctrl_len += n;
    }
}

static void on_end(void *user, uint8_t op, bool fin)
{
    (void)user;
    if (!fin) return;
    if (op == AV_WS_OP_TEXT) {
        if (s_text_overflow) {
            ESP_LOGW(TAG, "丢弃超长文本消息");
            return;
        }
        av_msg_t *msg = malloc(sizeof(*msg));
        if (!msg) {
            ESP_LOGE(TAG, "内存不足，丢弃消息");
            return;
        }
        if (av_proto_parse(s_text, s_text_len, msg) != 0) {
            ESP_LOGW(TAG, "无法解析的消息: %.*s", (int)(s_text_len > 80 ? 80 : s_text_len), s_text);
            free(msg);
            return;
        }
        if (msg->type == AV_MSG_STATE && msg->audio_codec[0]) {
            s_adpcm = strcmp(msg->audio_codec, "ima_adpcm") == 0;
        }
        if (msg->type == AV_MSG_TTS_START) {
            s_audio_receiving = true;
            s_audio_bytes = 0;
            s_audio_max_gap = 0;
        } else if (msg->type == AV_MSG_TTS_END && s_audio_receiving) {
            s_audio_receiving = false;
            uint32_t elapsed_ms = s_audio_bytes ?
                pdTICKS_TO_MS(xTaskGetTickCount() - s_audio_first_rx) : 0;
            ESP_LOGI(TAG, "音频接收: 字节=%u, 耗时=%ums, 最大间隔=%ums",
                     (unsigned)s_audio_bytes, (unsigned)elapsed_ms,
                     (unsigned)pdTICKS_TO_MS(s_audio_max_gap));
        }
        // 先按到达顺序更新播放闸门，再交给应用（应用负责 free）
        av_audio_on_server_msg(msg->type, msg->state);
        av_app_post_msg(msg);
    } else if (op == AV_WS_OP_BIN && s_adpcm) {
        size_t count = s_audio_block_bad ? 0 : av_adpcm_decode(
            s_audio_block, s_audio_block_len, s_decoded_audio, AV_ADPCM_MAX_SAMPLES);
        if (count) {
            feed_audio((const uint8_t *)s_decoded_audio, count * sizeof(int16_t));
        } else {
            ESP_LOGW(TAG, "Invalid ADPCM audio block");
        }
    } else if (op == AV_WS_OP_PING) {
        s_ping_pending = true;
    } else if (op == AV_WS_OP_CLOSE) {
        s_closed_by_peer = true;
    }
}

static const av_ws_sink_t SINK = {on_begin, on_data, on_end};

// ---- 连接 ----
static int wait_socket(int sock, bool write, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    return select(sock + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
}

static av_ws_err_t open_socket(const av_url_t *url, int *out)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)url->port);
    if (getaddrinfo(url->host, port, &hints, &res) != 0 || !res) return AV_WS_ERR_DNS;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return AV_WS_ERR_CONNECT;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) {
        close(sock);
        return AV_WS_ERR_CONNECT;
    }
    if (rc != 0) {
        if (wait_socket(sock, true, CONNECT_TIMEOUT_MS) <= 0) {
            close(sock);
            return AV_WS_ERR_CONNECT;
        }
        int so_err = 0;
        socklen_t sl = sizeof(so_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &sl);
        if (so_err != 0) {
            close(sock);
            return AV_WS_ERR_CONNECT;
        }
    }
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval stv = {.tv_sec = SEND_TIMEOUT_MS / 1000, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    *out = sock;
    return AV_WS_ERR_NONE;
}

// 握手；成功时把响应头之后多出来的字节交给 parser。
static av_ws_err_t handshake(int sock, const av_url_t *url, av_ws_parser_t *parser, int *http)
{
    uint8_t nonce[16];
    char key[AV_WS_KEY_LEN + 1];
    esp_fill_random(nonce, sizeof(nonce));
    av_ws_make_key(nonce, key);

    char buf[640];
    int n = av_ws_handshake_request(buf, sizeof(buf), url, key);
    if (n < 0) return AV_WS_ERR_BAD_URL;
    if (send_all(sock, (const uint8_t *)buf, (size_t)n) != ESP_OK) return AV_WS_ERR_IO;

    size_t got = 0;
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        int left = HANDSHAKE_TIMEOUT_MS - (int)pdTICKS_TO_MS(xTaskGetTickCount() - start);
        if (left <= 0 || wait_socket(sock, false, left) <= 0) return AV_WS_ERR_TIMEOUT;
        int r = recv(sock, buf + got, sizeof(buf) - 1 - got, 0);
        if (r <= 0) return AV_WS_ERR_CLOSED;
        got += (size_t)r;
        size_t header_len;
        av_ws_hs_result_t hs = av_ws_handshake_check(buf, got, key, http, &header_len);
        if (hs == AV_WS_HS_INCOMPLETE) {
            if (got >= sizeof(buf) - 1) return AV_WS_ERR_HANDSHAKE;
            continue;
        }
        if (hs == AV_WS_HS_BAD_STATUS) return AV_WS_ERR_HTTP;
        if (hs != AV_WS_HS_OK) return AV_WS_ERR_HANDSHAKE;
        if (got > header_len &&
            av_ws_parse(parser, (const uint8_t *)buf + header_len, got - header_len, &SINK, NULL) != AV_WS_PARSE_OK) {
            return AV_WS_ERR_PROTOCOL;
        }
        return AV_WS_ERR_NONE;
    }
}

static void set_sock(int sock)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    s_sock = sock;
    xSemaphoreGive(s_send_lock);
}

static void close_sock(void)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
    }
    xSemaphoreGive(s_send_lock);
}

static av_ws_err_t run_connection(const request_t *req, int *http)
{
    av_url_t url;
    if (!av_url_parse(req->url, &url)) return AV_WS_ERR_BAD_URL;
    int sock;
    av_ws_err_t err = open_socket(&url, &sock);
    if (err) return err;
    set_sock(sock);

    static av_ws_parser_t parser;
    av_ws_parser_init(&parser, MAX_FRAME);
    s_closed_by_peer = false;
    s_ping_pending = false;
    s_text_len = 0;
    s_audio_receiving = false;
    s_adpcm = false;
    av_audio_reset();

    err = handshake(sock, &url, &parser, http);
    if (err) return err;

    char hello[96];
    int n = av_proto_hello(hello, sizeof(hello), req->device_id);
    if (n < 0 || av_net_send_text(hello, (size_t)n) != ESP_OK) return AV_WS_ERR_IO;
    // 日志不打印 ?token=：复制一份并在 '?' 处截断
    char shown[sizeof(req->url)];
    snprintf(shown, sizeof(shown), "%s", req->url);
    char *query = strchr(shown, '?');
    if (query) *query = '\0';
    ESP_LOGI(TAG, "已连接 %s", shown);
    av_app_post_ws(AV_EV_WS_CONNECTED, 0, 0, req->gen);

    static uint8_t rx[1460];
    TickType_t last_rx = xTaskGetTickCount();
    while (!s_stop) {
        int ready = wait_socket(sock, false, 200);
        if (ready < 0) return AV_WS_ERR_IO;
        if (ready == 0) {
            if (pdTICKS_TO_MS(xTaskGetTickCount() - last_rx) > IDLE_TIMEOUT_MS) return AV_WS_ERR_TIMEOUT;
            continue;
        }
        int r = recv(sock, rx, sizeof(rx), 0);
        if (r == 0) return AV_WS_ERR_CLOSED;
        if (r < 0) return errno == EINTR ? AV_WS_ERR_NONE : AV_WS_ERR_IO;
        last_rx = xTaskGetTickCount();
        if (av_ws_parse(&parser, rx, (size_t)r, &SINK, NULL) != AV_WS_PARSE_OK) return AV_WS_ERR_PROTOCOL;
        if (s_ping_pending) {
            s_ping_pending = false;
            if (send_frame(AV_WS_OP_PONG, s_ctrl, s_ctrl_len) != ESP_OK) return AV_WS_ERR_IO;
        }
        if (s_closed_by_peer) {
            send_frame(AV_WS_OP_CLOSE, s_ctrl, s_ctrl_len >= 2 ? 2 : 0);
            return AV_WS_ERR_CLOSED;
        }
    }
    // 应用主动断开：礼貌地发 close（1000），不报告失败
    const uint8_t normal[2] = {0x03, 0xE8};
    send_frame(AV_WS_OP_CLOSE, normal, sizeof(normal));
    return AV_WS_ERR_NONE;
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            request_t req;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool pending = s_pending;
            if (pending) {
                req = s_req;
                s_pending = false;
                s_stop = false;
            }
            xSemaphoreGive(s_lock);
            if (!pending) break;

            int http = 0;
            av_ws_err_t err = run_connection(&req, &http);
            bool stopped = s_stop;
            close_sock();
            if (!stopped && err != AV_WS_ERR_NONE) {
                ESP_LOGW(TAG, "连接失败/断开: err=%d http=%d", err, http);
                av_app_post_ws(AV_EV_WS_FAILED, err, http, req.gen);
            }
        }
    }
}

esp_err_t av_net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_send_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(net_task, "av_net", 6144, NULL, 5, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void av_net_connect(const char *url, const char *device_id, uint32_t gen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_req.url, sizeof(s_req.url), "%s", url);
    snprintf(s_req.device_id, sizeof(s_req.device_id), "%s", device_id);
    s_req.gen = gen;
    s_pending = true;
    s_stop = true;  // 若有旧连接，让它退出；新请求开始时会清掉
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void av_net_disconnect(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending = false;
    s_stop = true;
    xSemaphoreGive(s_lock);
}
