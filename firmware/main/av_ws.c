#include "av_ws.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// SHA-1（FIPS 180-1），只用于校验 Sec-WebSocket-Accept
// ---------------------------------------------------------------------------
static uint32_t rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t b[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 |
               (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (bb & c) | (~bb & d);           k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = bb ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

void av_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t block[64];
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    size_t rest = len - i;
    memset(block, 0, sizeof(block));
    memcpy(block, data + i, rest);
    block[rest] = 0x80;
    if (rest >= 56) {
        sha1_block(h, block);
        memset(block, 0, sizeof(block));
    }
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) block[63 - j] = (uint8_t)(bits >> (8 * j));
    sha1_block(h, block);
    for (int j = 0; j < 5; j++) {
        out[j * 4] = (uint8_t)(h[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(h[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(h[j] >> 8);
        out[j * 4 + 3] = (uint8_t)h[j];
    }
}

size_t av_base64(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (len + 2) / 3 * 4;
    if (out_size < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

void av_ws_make_key(const uint8_t nonce[16], char key[AV_WS_KEY_LEN + 1])
{
    av_base64(nonce, 16, key, AV_WS_KEY_LEN + 1);
}

// ---------------------------------------------------------------------------
// 握手
// ---------------------------------------------------------------------------
int av_ws_handshake_request(char *buf, size_t size, const av_url_t *url, const char *key)
{
    char host[AV_URL_HOST_MAX + 8];
    if (url->port == 80) snprintf(host, sizeof(host), "%s", url->host);
    else snprintf(host, sizeof(host), "%s:%u", url->host, (unsigned)url->port);
    int n = snprintf(buf, size,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "User-Agent: ai-passport-astrbot-voice\r\n"
                     "\r\n",
                     url->path, host, key);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

static void expected_accept(const char *key, char out[29])
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char joined[AV_WS_KEY_LEN + sizeof(guid)];
    size_t klen = strlen(key);
    if (klen > AV_WS_KEY_LEN) klen = AV_WS_KEY_LEN;
    memcpy(joined, key, klen);
    memcpy(joined + klen, guid, sizeof(guid) - 1);
    uint8_t digest[20];
    av_sha1((const uint8_t *)joined, klen + sizeof(guid) - 1, digest);
    av_base64(digest, 20, out, 29);
}

static bool header_name_is(const char *line, size_t len, const char *name)
{
    size_t n = strlen(name);
    if (len <= n || line[n] != ':') return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) return false;
    }
    return true;
}

av_ws_hs_result_t av_ws_handshake_check(const char *resp, size_t len, const char *key,
                                        int *status, size_t *header_len)
{
    *status = 0;
    *header_len = 0;
    size_t end = 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            end = i + 4;
            break;
        }
    }
    if (end == 0) return AV_WS_HS_INCOMPLETE;
    *header_len = end;

    // 状态行：HTTP/1.1 101 Switching Protocols
    if (len < 12 || strncmp(resp, "HTTP/1.", 7) != 0 || resp[8] != ' ') return AV_WS_HS_MALFORMED;
    int code = 0;
    for (int i = 9; i < 12; i++) {
        if (resp[i] < '0' || resp[i] > '9') return AV_WS_HS_MALFORMED;
        code = code * 10 + (resp[i] - '0');
    }
    *status = code;
    if (code != 101) return AV_WS_HS_BAD_STATUS;

    char want[29];
    expected_accept(key, want);
    size_t pos = 0;
    while (pos < end && !(resp[pos] == '\r' && resp[pos + 1] == '\n')) pos++;
    pos += 2;  // 跳过状态行
    while (pos + 2 <= end) {
        size_t line_end = pos;
        while (line_end + 1 < end && !(resp[line_end] == '\r' && resp[line_end + 1] == '\n')) line_end++;
        size_t line_len = line_end - pos;
        if (line_len == 0) break;
        const char *line = resp + pos;
        if (header_name_is(line, line_len, "Sec-WebSocket-Accept")) {
            size_t v = strlen("Sec-WebSocket-Accept") + 1;
            while (v < line_len && (line[v] == ' ' || line[v] == '\t')) v++;
            size_t vend = line_len;
            while (vend > v && (line[vend - 1] == ' ' || line[vend - 1] == '\t')) vend--;
            if (vend - v == 28 && memcmp(line + v, want, 28) == 0) return AV_WS_HS_OK;
            return AV_WS_HS_BAD_ACCEPT;
        }
        pos = line_end + 2;
    }
    return AV_WS_HS_BAD_ACCEPT;
}

// ---------------------------------------------------------------------------
// 发送帧
// ---------------------------------------------------------------------------
size_t av_ws_frame_header(uint8_t hdr[AV_WS_HEADER_MAX], uint8_t opcode, size_t payload_len,
                          const uint8_t mask[4])
{
    size_t n = 0;
    hdr[n++] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (payload_len < 126) {
        hdr[n++] = (uint8_t)(0x80 | payload_len);
    } else if (payload_len <= 0xFFFF) {
        hdr[n++] = 0x80 | 126;
        hdr[n++] = (uint8_t)(payload_len >> 8);
        hdr[n++] = (uint8_t)payload_len;
    } else {
        hdr[n++] = 0x80 | 127;
        uint64_t l = payload_len;
        for (int i = 7; i >= 0; i--) hdr[n++] = (uint8_t)(l >> (8 * i));
    }
    memcpy(hdr + n, mask, 4);
    return n + 4;
}

void av_ws_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4], size_t offset)
{
    for (size_t i = 0; i < len; i++) data[i] ^= mask[(offset + i) & 3];
}

// ---------------------------------------------------------------------------
// 接收解析
// ---------------------------------------------------------------------------
void av_ws_parser_init(av_ws_parser_t *p, uint64_t max_frame)
{
    memset(p, 0, sizeof(*p));
    p->max_frame = max_frame;
    p->hdr_need = 2;
}

static av_ws_parse_err_t fail(av_ws_parser_t *p, av_ws_parse_err_t e)
{
    p->err = e;
    return e;
}

static void frame_done(av_ws_parser_t *p, const av_ws_sink_t *sink, void *user)
{
    uint8_t op = p->opcode;
    bool control = op & 0x08;
    if (sink->end) sink->end(user, op, p->fin);
    if (!control && p->fin) p->msg_opcode = 0;
    p->in_payload = false;
    p->hdr_len = 0;
    p->hdr_need = 2;
}

av_ws_parse_err_t av_ws_parse(av_ws_parser_t *p, const uint8_t *data, size_t len,
                              const av_ws_sink_t *sink, void *user)
{
    if (p->err) return p->err;
    size_t i = 0;
    while (i < len) {
        if (!p->in_payload) {
            while (p->hdr_len < p->hdr_need && i < len) p->hdr[p->hdr_len++] = data[i++];
            if (p->hdr_len < p->hdr_need) break;

            if (p->hdr_len == 2) {
                if (p->hdr[0] & 0x70) return fail(p, AV_WS_PARSE_ERR_RSV);
                if (p->hdr[1] & 0x80) return fail(p, AV_WS_PARSE_ERR_MASKED);
                uint8_t l7 = p->hdr[1] & 0x7F;
                if (l7 == 126) p->hdr_need = 4;
                else if (l7 == 127) p->hdr_need = 10;
                if (p->hdr_need > 2) continue;
            }

            uint8_t op = p->hdr[0] & 0x0F;
            bool fin = p->hdr[0] & 0x80;
            uint64_t flen = p->hdr[1] & 0x7F;
            if (flen == 126) {
                flen = (uint64_t)p->hdr[2] << 8 | p->hdr[3];
            } else if (flen == 127) {
                flen = 0;
                for (int k = 2; k < 10; k++) flen = flen << 8 | p->hdr[k];
            }

            if (op & 0x08) {
                if (op != AV_WS_OP_CLOSE && op != AV_WS_OP_PING && op != AV_WS_OP_PONG) {
                    return fail(p, AV_WS_PARSE_ERR_OPCODE);
                }
                if (!fin || flen > 125) return fail(p, AV_WS_PARSE_ERR_CONTROL);
            } else if (op == AV_WS_OP_CONT) {
                if (!p->msg_opcode) return fail(p, AV_WS_PARSE_ERR_OPCODE);
                op = p->msg_opcode;
            } else if (op == AV_WS_OP_TEXT || op == AV_WS_OP_BIN) {
                if (p->msg_opcode) return fail(p, AV_WS_PARSE_ERR_OPCODE);
                p->msg_opcode = op;
            } else {
                return fail(p, AV_WS_PARSE_ERR_OPCODE);
            }
            if (flen > p->max_frame) return fail(p, AV_WS_PARSE_ERR_TOO_LARGE);

            bool first_fragment = (p->hdr[0] & 0x0F) != AV_WS_OP_CONT;
            p->opcode = op;
            p->fin = fin;
            p->remaining = flen;
            p->in_payload = true;
            if (first_fragment && sink->begin) sink->begin(user, op, flen);
            if (flen == 0) frame_done(p, sink, user);
            continue;
        }

        size_t n = len - i;
        if (n > p->remaining) n = (size_t)p->remaining;
        if (n && sink->data) sink->data(user, p->opcode, data + i, n);
        i += n;
        p->remaining -= n;
        if (p->remaining == 0) frame_done(p, sink, user);
    }
    return AV_WS_PARSE_OK;
}
