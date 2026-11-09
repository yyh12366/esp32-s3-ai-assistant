#include "av_ipedit.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_PORT 8765

void av_ipedit_begin(av_ipedit_t *e, const char *current_url)
{
    static const uint16_t fallback[AV_IPEDIT_FIELDS] = {192, 168, 1, 100, DEFAULT_PORT};
    memset(e, 0, sizeof(*e));
    memcpy(e->value, fallback, sizeof(fallback));
    strcpy(e->path, "/ws");

    av_url_t url;
    if (!current_url || !av_url_parse(current_url, &url)) return;
    uint8_t ip[4];
    if (av_url_ipv4(url.host, ip)) {
        for (int i = 0; i < 4; i++) e->value[i] = ip[i];
    }
    e->value[4] = url.port;
    snprintf(e->path, sizeof(e->path), "%s", url.path);
}

static void step(av_ipedit_t *e, int delta)
{
    const bool port = e->field == 4;
    const int min = port ? 1 : 0;
    const int max = port ? 65535 : 255;
    const int span = max - min + 1;
    int v = (int)e->value[e->field] + delta - min;
    v %= span;
    if (v < 0) v += span;
    e->value[e->field] = (uint16_t)(v + min);
}

av_ipedit_result_t av_ipedit_key(av_ipedit_t *e, av_ipedit_key_t key)
{
    const int big = e->field == 4 ? 100 : 10;
    switch (key) {
    case AV_IPEDIT_KEY_UP: step(e, 1); break;
    case AV_IPEDIT_KEY_DOWN: step(e, -1); break;
    case AV_IPEDIT_KEY_UP_LONG: step(e, big); break;
    case AV_IPEDIT_KEY_DOWN_LONG: step(e, -big); break;
    case AV_IPEDIT_KEY_OK:
        if (e->field + 1 >= AV_IPEDIT_FIELDS) return AV_IPEDIT_SAVED;
        e->field++;
        break;
    case AV_IPEDIT_KEY_OK_LONG:
        return AV_IPEDIT_CANCELLED;
    }
    return AV_IPEDIT_EDITING;
}

int av_ipedit_url(const av_ipedit_t *e, char *buf, size_t size)
{
    int n = snprintf(buf, size, "ws://%u.%u.%u.%u:%u%s", e->value[0], e->value[1], e->value[2],
                     e->value[3], e->value[4], e->path);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}
