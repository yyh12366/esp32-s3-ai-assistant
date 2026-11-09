// main/av_text.h —— 显示文字的 UTF-8 处理（纯逻辑）。
//
// 动态文字契约：识别结果、回复、中继错误信息来自网络，字符集不受控。
// 显示前统一经过 av_text_sanitize()：
//   * 非法 UTF-8 → U+25A1 □
//   * 字体里没有的码点（由回调判断）→ U+25A1 □（字体保证包含 □）
//   * 控制字符丢弃（保留 \n；\t、\r 转为空格）
// 字体覆盖 ASCII + 全部 GB2312（见 tools/gen_av_font.py）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AV_TEXT_REPLACEMENT 0x25A1u

typedef bool (*av_glyph_fn)(uint32_t codepoint, void *user);

// 解码一个 UTF-8 字符，返回消耗的字节数（>=1）；非法序列返回 1 且 *cp = 0xFFFD。
size_t av_utf8_decode(const char *s, size_t len, uint32_t *cp);

// 返回 s 中不超过 max 字节、且以完整字符结尾的最长前缀长度。
size_t av_utf8_prefix(const char *s, size_t len, size_t max);

// 清洗 src 写入 dst（总是 NUL 结尾，按字符边界截断）。has_glyph 为 NULL 时不做覆盖检查。
// 返回写入的字节数。
size_t av_text_sanitize(char *dst, size_t dst_size, const char *src, size_t src_len,
                        av_glyph_fn has_glyph, void *user);

// 回复文字缓冲：追加句子，超出容量时丢弃最旧的完整字符（保留最新内容）。
#define AV_TEXTBUF_SIZE 2048
typedef struct {
    char buf[AV_TEXTBUF_SIZE];
    size_t len;
    bool dropped;  // 曾经丢弃过开头内容
} av_textbuf_t;

void av_textbuf_clear(av_textbuf_t *t);
void av_textbuf_append(av_textbuf_t *t, const char *s);
