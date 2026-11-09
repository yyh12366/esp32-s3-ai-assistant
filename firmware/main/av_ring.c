#include "av_ring.h"

#include <string.h>

void av_ring_init(av_ring_t *r, uint8_t *storage, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->buf = storage;
    r->cap = cap;
    r->gate_open = true;
}

size_t av_ring_free(const av_ring_t *r) { return r->cap - r->len; }

size_t av_ring_write(av_ring_t *r, const uint8_t *data, size_t len)
{
    if (!r->gate_open) {
        r->dropped += (uint32_t)len;
        return len;
    }
    size_t n = av_ring_free(r);
    if (n > len) n = len;
    size_t tail = (r->head + r->len) % r->cap;
    size_t first = r->cap - tail;
    if (first > n) first = n;
    memcpy(r->buf + tail, data, first);
    memcpy(r->buf, data + first, n - first);
    r->len += n;
    return n;
}

size_t av_ring_read(av_ring_t *r, uint8_t *out, size_t max)
{
    size_t n = r->len < max ? r->len : max;
    size_t first = r->cap - r->head;
    if (first > n) first = n;
    memcpy(out, r->buf + r->head, first);
    memcpy(out + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->len -= n;
    return n;
}

size_t av_ring_play_read(av_ring_t *r, uint8_t *out, size_t max, size_t prebuffer)
{
    max &= ~(size_t)1;
    if (!r->gate_open || !max) return 0;
    size_t avail = r->len & ~(size_t)1;
    if (!avail) {
        if (r->playing && r->receiving) r->underruns++;
        r->playing = false;
        return 0;
    }
    size_t threshold = prebuffer < r->cap ? prebuffer : r->cap;
    threshold &= ~(size_t)1;
    if (!r->playing && r->receiving && avail < threshold) return 0;
    r->playing = true;
    return av_ring_read(r, out, avail < max ? avail : max);
}

void av_ring_interrupt(av_ring_t *r)
{
    r->gate_open = false;
    r->head = 0;
    r->len = 0;
    r->playing = false;
    r->receiving = false;
}

bool av_ring_on_server_msg(av_ring_t *r, av_msg_type_t type, av_voice_t state)
{
    bool open = type == AV_MSG_TTS_END ||
                (type == AV_MSG_STATE && (state == AV_VOICE_IDLE || state == AV_VOICE_LISTENING));
    bool reopened = !r->gate_open && open;
    if (reopened) r->gate_open = true;
    if (type == AV_MSG_TTS_START && r->gate_open) {
        r->receiving = true;
        r->playing = false;
        r->underruns = 0;
    } else if (open) {
        r->receiving = false;
    }
    return reopened;
}
