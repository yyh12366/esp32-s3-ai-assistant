#include "av_ota_manifest.h"

#include <string.h>

// 在 json 里找到 "key" 后面的值起点（跳过冒号和空白）。找不到返回 NULL。
static const char *find_value(const char *json, const char *key)
{
    char pattern[32];
    if (strlen(key) + 3 > sizeof(pattern)) return NULL;
    pattern[0] = '"';
    strcpy(pattern + 1, key);
    strcat(pattern, "\"");
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// 读取 "..." 字符串到 out（不含引号）。遇到反斜杠或超长失败。
static bool read_string(const char *p, char *out, size_t size)
{
    if (*p != '"') return false;
    p++;
    size_t len = 0;
    while (*p != '"') {
        if (*p == '\0' || *p == '\\') return false;
        if (len + 1 >= size) return false;
        out[len++] = *p++;
    }
    out[len] = '\0';
    return len > 0;
}

static bool read_u32(const char *p, uint32_t *out)
{
    uint64_t value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint64_t)(*p - '0');
        if (value > 0xFFFFFFFFu) return false;
        p++;
        digits++;
    }
    if (digits == 0) return false;
    if (*p != ',' && *p != '}' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') return false;
    *out = (uint32_t)value;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_sha256(const char *p, uint8_t out[AV_OTA_SHA256_LEN])
{
    if (*p != '"') return false;
    p++;
    for (int i = 0; i < AV_OTA_SHA256_LEN; i++) {
        const int hi = hex_value(p[0]), lo = hex_value(p[1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    return *p == '"';
}

bool av_ota_manifest_parse(const char *json, av_ota_manifest_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));

    const char *version = find_value(json, "version");
    const char *size = find_value(json, "size");
    const char *sha = find_value(json, "sha256");
    if (!version || !size || !sha) return false;

    if (!read_string(version, out->version, sizeof(out->version))) return false;
    if (!read_u32(size, &out->size) || out->size == 0) return false;
    if (!read_sha256(sha, out->sha256)) return false;
    return true;
}
