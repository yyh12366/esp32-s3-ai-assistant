#include "av_proto.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// 足够解析中继消息的最小 JSON 读取器：顶层对象，值可以是任意 JSON（不需要的值被跳过）。
typedef struct {
    const char *p;
    const char *end;
} cursor_t;

#define JSON_MAX_DEPTH 16

static void skip_ws(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = s[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

size_t av_utf8_encode(unsigned cp, char out[4])
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

// 读一个字符串（c->p 指向开头的引号）。dst 为 NULL 时只跳过。
// 写入时整字符写入，放不下的部分丢弃并置 *truncated。
static int read_string(cursor_t *c, char *dst, size_t cap, bool *truncated)
{
    if (c->p >= c->end || *c->p != '"') return -1;
    c->p++;
    size_t n = 0;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;
        char buf[4];
        size_t blen;
        if (ch == '"') {
            if (dst) dst[n] = '\0';
            return 0;
        }
        if (ch < 0x20) return -1;
        if (ch == '\\') {
            if (c->p >= c->end) return -1;
            char e = *c->p++;
            switch (e) {
            case '"': buf[0] = '"'; blen = 1; break;
            case '\\': buf[0] = '\\'; blen = 1; break;
            case '/': buf[0] = '/'; blen = 1; break;
            case 'b': buf[0] = '\b'; blen = 1; break;
            case 'f': buf[0] = '\f'; blen = 1; break;
            case 'n': buf[0] = '\n'; blen = 1; break;
            case 'r': buf[0] = '\r'; blen = 1; break;
            case 't': buf[0] = '\t'; blen = 1; break;
            case 'u': {
                unsigned cp;
                if (c->end - c->p < 4 || hex4(c->p, &cp) != 0) return -1;
                c->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u' &&
                        hex4(c->p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        c->p += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = 0xFFFD;  // 孤立代理项
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                blen = av_utf8_encode(cp, buf);
                break;
            }
            default:
                return -1;
            }
        } else {
            // 原样复制一个完整的 UTF-8 字符（合法性由显示层再检查）
            size_t extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
            buf[0] = (char)ch;
            blen = 1;
            for (size_t k = 0; k < extra; k++) {
                if (c->p >= c->end || *c->p == '"') break;
                buf[blen++] = *c->p++;
            }
        }
        if (dst) {
            if (n + blen < cap) {
                memcpy(dst + n, buf, blen);
                n += blen;
            } else if (truncated) {
                *truncated = true;
            }
        }
    }
    return -1;
}

static int skip_value(cursor_t *c, int depth);

static int skip_container(cursor_t *c, int depth, char open, char close)
{
    if (depth > JSON_MAX_DEPTH) return -1;
    c->p++;  // open
    skip_ws(c);
    if (c->p < c->end && *c->p == close) { c->p++; return 0; }
    for (;;) {
        if (open == '{') {
            skip_ws(c);
            if (read_string(c, NULL, 0, NULL) != 0) return -1;
            skip_ws(c);
            if (c->p >= c->end || *c->p != ':') return -1;
            c->p++;
        }
        skip_ws(c);
        if (skip_value(c, depth + 1) != 0) return -1;
        skip_ws(c);
        if (c->p >= c->end) return -1;
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == close) { c->p++; return 0; }
        return -1;
    }
}

static int match_literal(cursor_t *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) return -1;
    c->p += n;
    return 0;
}

static int skip_value(cursor_t *c, int depth)
{
    if (c->p >= c->end) return -1;
    char ch = *c->p;
    if (ch == '"') return read_string(c, NULL, 0, NULL);
    if (ch == '{') return skip_container(c, depth, '{', '}');
    if (ch == '[') return skip_container(c, depth, '[', ']');
    if (ch == 't') return match_literal(c, "true");
    if (ch == 'f') return match_literal(c, "false");
    if (ch == 'n') return match_literal(c, "null");
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        const char *start = c->p;
        while (c->p < c->end && (strchr("+-.eE", *c->p) || (*c->p >= '0' && *c->p <= '9'))) c->p++;
        return c->p > start ? 0 : -1;
    }
    return -1;
}

static av_msg_type_t type_from(const char *s)
{
    if (strcmp(s, "state") == 0) return AV_MSG_STATE;
    if (strcmp(s, "asr") == 0) return AV_MSG_ASR;
    if (strcmp(s, "reply") == 0) return AV_MSG_REPLY;
    if (strcmp(s, "tts_start") == 0) return AV_MSG_TTS_START;
    if (strcmp(s, "tts_end") == 0) return AV_MSG_TTS_END;
    if (strcmp(s, "error") == 0) return AV_MSG_ERROR;
    return AV_MSG_UNKNOWN;
}

static bool state_from(const char *s, av_voice_t *out)
{
    static const struct { const char *name; av_voice_t v; } map[] = {
        {"idle", AV_VOICE_IDLE},
        {"listening", AV_VOICE_LISTENING},
        {"thinking", AV_VOICE_THINKING},
        {"speaking", AV_VOICE_SPEAKING},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(s, map[i].name) == 0) { *out = map[i].v; return true; }
    }
    return false;
}

int av_proto_parse(const char *json, size_t len, av_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    cursor_t c = {json, json + len};
    char type[16] = "", value[16] = "";
    bool have_text = false, have_msg = false;
    // text 与 msg 都写到 out->text：error 用 msg，asr/reply 用 text。先各存一份标志。
    char msg[AV_MSG_TEXT_MAX];
    bool msg_trunc = false;
    msg[0] = '\0';

    skip_ws(&c);
    if (c.p >= c.end || *c.p != '{') return -1;
    c.p++;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') {
        c.p++;
    } else {
        for (;;) {
            char key[16];
            bool key_trunc = false;
            skip_ws(&c);
            if (read_string(&c, key, sizeof(key), &key_trunc) != 0) return -1;
            if (key_trunc) key[0] = '\0';  // 太长的键不会是我们关心的
            skip_ws(&c);
            if (c.p >= c.end || *c.p != ':') return -1;
            c.p++;
            skip_ws(&c);
            if (c.p >= c.end) return -1;
            bool is_str = *c.p == '"';
            bool trunc = false;
            if (is_str && strcmp(key, "type") == 0) {
                if (read_string(&c, type, sizeof(type), &trunc) != 0) return -1;
                if (trunc) type[0] = '\0';
            } else if (is_str && strcmp(key, "value") == 0) {
                if (read_string(&c, value, sizeof(value), &trunc) != 0) return -1;
                if (trunc) value[0] = '\0';
            } else if (is_str && strcmp(key, "audio_codec") == 0) {
                if (read_string(&c, out->audio_codec, sizeof(out->audio_codec), &trunc) != 0) return -1;
                if (trunc) return -1;
            } else if (is_str && strcmp(key, "text") == 0) {
                if (read_string(&c, out->text, sizeof(out->text), &out->truncated) != 0) return -1;
                have_text = true;
            } else if (is_str && strcmp(key, "msg") == 0) {
                if (read_string(&c, msg, sizeof(msg), &msg_trunc) != 0) return -1;
                have_msg = true;
            } else if (strcmp(key, "final") == 0 && (*c.p == 't' || *c.p == 'f')) {
                out->final = *c.p == 't';
                if (skip_value(&c, 1) != 0) return -1;
            } else {
                if (skip_value(&c, 1) != 0) return -1;
            }
            skip_ws(&c);
            if (c.p >= c.end) return -1;
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return -1;
        }
    }
    skip_ws(&c);
    if (c.p != c.end) return -1;

    out->type = type_from(type);
    if (out->type == AV_MSG_STATE) out->state_known = state_from(value, &out->state);
    if (out->type == AV_MSG_ERROR) {
        if (have_msg) {
            memcpy(out->text, msg, sizeof(out->text));
            out->truncated = msg_trunc;
        } else if (!have_text) {
            out->text[0] = '\0';
        }
    } else if (out->type != AV_MSG_ASR && out->type != AV_MSG_REPLY) {
        out->text[0] = '\0';
        out->truncated = false;
    }
    return 0;
}

static int put(char *buf, size_t size, const char *s)
{
    int n = snprintf(buf, size, "%s", s);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

int av_proto_hello(char *buf, size_t size, const char *device_id)
{
    size_t n = 0;
    const char head[] = "{\"type\":\"hello\",\"device_id\":\"";
    const char tail[] = "\",\"audio_codec\":\"ima_adpcm\"}";
    if (size < sizeof(head)) return -1;
    memcpy(buf, head, sizeof(head) - 1);
    n = sizeof(head) - 1;
    for (const unsigned char *p = (const unsigned char *)device_id; *p; p++) {
        char esc[8];
        size_t elen;
        if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = (char)*p; elen = 2; }
        else if (*p < 0x20) { elen = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", *p); }
        else { esc[0] = (char)*p; elen = 1; }
        if (n + elen + sizeof(tail) > size) return -1;
        memcpy(buf + n, esc, elen);
        n += elen;
    }
    if (n + sizeof(tail) > size) return -1;
    memcpy(buf + n, tail, sizeof(tail));
    return (int)(n + sizeof(tail) - 1);
}

int av_proto_listen_start(char *buf, size_t size) { return put(buf, size, "{\"type\":\"listen_start\"}"); }
int av_proto_listen_stop(char *buf, size_t size) { return put(buf, size, "{\"type\":\"listen_stop\"}"); }
int av_proto_abort(char *buf, size_t size) { return put(buf, size, "{\"type\":\"abort\"}"); }
