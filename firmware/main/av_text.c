#include "av_text.h"

#include <string.h>

#include "av_proto.h"  // av_utf8_encode

size_t av_utf8_decode(const char *s, size_t len, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (len == 0) { *cp = 0xFFFD; return 0; }
    unsigned char c = u[0];
    size_t n;
    uint32_t v, min;
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) { n = 2; v = c & 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; v = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; v = c & 0x07; min = 0x10000; }
    else { *cp = 0xFFFD; return 1; }
    if (len < n) { *cp = 0xFFFD; return 1; }
    for (size_t i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (u[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFD; return 1; }
    *cp = v;
    return n;
}

size_t av_utf8_prefix(const char *s, size_t len, size_t max)
{
    if (len <= max) return len;
    size_t i = max;
    // 回退到一个不是续字节的位置
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

size_t av_text_sanitize(char *dst, size_t dst_size, const char *src, size_t src_len,
                        av_glyph_fn has_glyph, void *user)
{
    if (dst_size == 0) return 0;
    size_t o = 0, i = 0;
    while (i < src_len && src[i] != '\0') {
        uint32_t cp;
        size_t used = av_utf8_decode(src + i, src_len - i, &cp);
        i += used;
        if (cp == '\t' || cp == '\r') cp = ' ';
        if (cp < 0x20 && cp != '\n') continue;
        if (cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) continue;
        if (cp == 0xFEFF || cp == 0x200B || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)) continue;
        if (cp == 0xFFFD || (cp != '\n' && has_glyph && !has_glyph(cp, user))) cp = AV_TEXT_REPLACEMENT;
        char enc[4];
        size_t n = av_utf8_encode(cp, enc);
        if (n == 0) continue;
        if (o + n + 1 > dst_size) break;
        memcpy(dst + o, enc, n);
        o += n;
    }
    dst[o] = '\0';
    return o;
}

void av_textbuf_clear(av_textbuf_t *t)
{
    t->len = 0;
    t->buf[0] = '\0';
    t->dropped = false;
}

void av_textbuf_append(av_textbuf_t *t, const char *s)
{
    size_t n = strlen(s);
    const size_t cap = sizeof(t->buf) - 1;
    if (n > cap) {
        // 新内容本身就放不下：只保留它的结尾
        size_t skip = n - cap;
        while (skip < n && ((unsigned char)s[skip] & 0xC0) == 0x80) skip++;
        s += skip;
        n -= skip;
        t->len = 0;
        t->dropped = true;
    }
    if (t->len + n > cap) {
        size_t drop = t->len + n - cap;
        while (drop < t->len && ((unsigned char)t->buf[drop] & 0xC0) == 0x80) drop++;
        memmove(t->buf, t->buf + drop, t->len - drop);
        t->len -= drop;
        t->dropped = true;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = '\0';
}
