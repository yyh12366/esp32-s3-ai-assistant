// main/av_ota_manifest.h —— OTA 清单 version.json 的解析（纯逻辑，可在主机上测试）。
//
// 清单格式：{"version":"<版本标识>","size":<字节数>,"sha256":"<64 位十六进制>"}
// 版本标识 = esp_app_desc 的 version + " " + date + " " + time，每次编译都不同。
// 解析器只接受这三个字段，不做转义；遇到反斜杠直接失败，避免半截解析。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AV_OTA_VERSION_MAX 80   // 版本标识上限（含结尾 NUL）
#define AV_OTA_SHA256_LEN  32

typedef struct {
    char version[AV_OTA_VERSION_MAX];
    uint32_t size;
    uint8_t sha256[AV_OTA_SHA256_LEN];
} av_ota_manifest_t;

// 解析成功返回 true；缺字段、size 为 0 或超过 uint32、sha256 不是 64 位十六进制、
// 版本串过长或含反斜杠都返回 false，out 内容不保证有效。
bool av_ota_manifest_parse(const char *json, av_ota_manifest_t *out);
