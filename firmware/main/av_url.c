#include "av_url.h"

#include <stdio.h>
#include <string.h>

static bool host_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_';
}

bool av_url_parse(const char *url, av_url_t *out)
{
    static const char scheme[] = "ws://";
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));
    if (strncmp(url, scheme, sizeof(scheme) - 1) != 0) return false;
    const char *p = url + sizeof(scheme) - 1;

    size_t host_len = 0;
    while (p[host_len] && p[host_len] != ':' && p[host_len] != '/' && p[host_len] != '?') {
        if (!host_char_ok(p[host_len])) return false;
        host_len++;
    }
    if (host_len == 0 || host_len >= sizeof(out->host)) return false;
    memcpy(out->host, p, host_len);
    p += host_len;

    out->port = 80;
    if (*p == ':') {
        p++;
        unsigned long port = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned long)(*p - '0');
            if (port > 65535) return false;
            p++;
            digits++;
        }
        if (digits == 0 || port == 0) return false;
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        strcpy(out->path, "/");
        return true;
    }
    if (*p != '/' && *p != '?') return false;
    size_t path_len = strlen(p);
    size_t prefix = (*p == '?') ? 1 : 0;  // "ws://h?x" → "/?x"
    if (path_len + prefix >= sizeof(out->path)) return false;
    for (size_t i = 0; i < path_len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c <= 0x20 || c >= 0x7F) return false;  // 不接受空格/控制字符/非 ASCII
    }
    if (prefix) out->path[0] = '/';
    memcpy(out->path + prefix, p, path_len);
    return true;
}

int av_url_format(char *buf, size_t size, const av_url_t *url)
{
    int n;
    if (url->port == 80) {
        n = snprintf(buf, size, "ws://%s%s", url->host, url->path);
    } else {
        n = snprintf(buf, size, "ws://%s:%u%s", url->host, (unsigned)url->port, url->path);
    }
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

bool av_url_ipv4(const char *host, uint8_t out[4])
{
    unsigned part = 0;
    int idx = 0, digits = 0;
    for (const char *p = host;; p++) {
        if (*p >= '0' && *p <= '9') {
            part = part * 10 + (unsigned)(*p - '0');
            if (part > 255 || ++digits > 3) return false;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || idx > 3) return false;
            out[idx++] = (uint8_t)part;
            part = 0;
            digits = 0;
            if (*p == '\0') break;
        } else {
            return false;
        }
    }
    return idx == 4;
}

bool av_url_inherit_query(char *url, size_t size, const char *fallback)
{
    if (!url || !fallback || strchr(url, '?')) return false;
    const char *query = strchr(fallback, '?');
    if (!query || !query[1]) return false;
    const size_t have = strlen(url), add = strlen(query);
    if (have + add + 1 > size) return false;
    memcpy(url + have, query, add + 1);
    return true;
}

bool av_url_query_param(const char *url, const char *key, char *out, size_t size)
{
    if (out && size) out[0] = '\0';
    if (!url || !key || !out || !size) return false;
    const char *q = strchr(url, '?');
    if (!q) return false;
    const size_t klen = strlen(key);
    const char *p = q + 1;
    while (*p) {
        const char *end = strchr(p, '&');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const size_t vlen = len - klen - 1;
            if (vlen >= size) return false;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}

int av_media_url(char *buf, size_t size, const char *base, const char *path, const char *token)
{
    if (!buf || !size || !base || !path) return -1;
    const int n = snprintf(buf, size, "%s%s", base, path);
    if (n < 0 || (size_t)n >= size) return -1;
    if (!token || !token[0]) return n;
    const char *sep = strchr(path, '?') ? "&" : "?";
    const int m = snprintf(buf + n, size - (size_t)n, "%stoken=%s", sep, token);
    if (m < 0 || (size_t)m >= size - (size_t)n) return -1;
    return n + m;
}
