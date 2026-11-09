// main/av_url.h —— 中继地址 ws://host[:port][/path][?query] 的解析与格式化（纯逻辑）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AV_URL_MAX       128
#define AV_URL_HOST_MAX  64
#define AV_URL_PATH_MAX  96

typedef struct {
    char host[AV_URL_HOST_MAX];
    uint16_t port;
    char path[AV_URL_PATH_MAX];  // 以 '/' 开头，含 ?query
} av_url_t;

// 只接受 ws://（中继是局域网明文 ws）。端口缺省 80，路径缺省 "/"。
bool av_url_parse(const char *url, av_url_t *out);
// 返回长度；空间不足返回 -1。端口为 80 时省略。
int av_url_format(char *buf, size_t size, const av_url_t *url);
// host 是点分十进制 IPv4 时返回 true 并填 out。
bool av_url_ipv4(const char *host, uint8_t out[4]);
// url 没有 ?query 而 fallback 有时，把 fallback 的 ?query（如 ?token=...）接到 url 后面。
// 用于设备上改过 IP、但存下的地址丢了 token 的情况。接上了返回 true；空间不足不改动。
bool av_url_inherit_query(char *url, size_t size, const char *fallback);
// 取出 url 的 ?query 里名为 key 的参数值（如 token）写入 out，值可以为空。
// 找到返回 true；没有这个参数或放不下（含 NUL）返回 false，out 置空。
bool av_url_query_param(const char *url, const char *key, char *out, size_t size);
// 拼出媒体请求地址：base + path；token 非空时追加 token 参数（path 已有 ? 则用 &）。
// 返回长度；空间不足返回 -1。
int av_media_url(char *buf, size_t size, const char *base, const char *path, const char *token);
