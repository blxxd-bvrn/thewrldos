/* Ogg Vorbis through stb_vorbis (third_party/stb_vorbis). */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../third_party/stb_vorbis/stb_vorbis.c"
#include "mediadec.h"

static int ci_n(const char *a, const char *b, size_t n) {   /* strncasecmp for ASCII */
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
    return 0;
}

struct mfmt { stb_vorbis *v; int ch; };

/* "TITLE=..." style comments */
static void comment(const char *c, mdec_info_t *info) {
    const char *eq = strchr(c, '=');
    if (!eq) return;
    size_t k = (size_t)(eq - c);
    char *dst = NULL; size_t cap = 0;
    if (k == 5 && !ci_n(c, "TITLE", 5)) { dst = info->title; cap = sizeof info->title; }
    else if (k == 6 && !ci_n(c, "ARTIST", 6)) { dst = info->artist; cap = sizeof info->artist; }
    else if (k == 5 && !ci_n(c, "ALBUM", 5)) { dst = info->album; cap = sizeof info->album; }
    if (!dst || dst[0]) return;
    size_t i = 0;
    for (const char *p = eq + 1; *p && i + 1 < cap; p++) if ((unsigned char)*p < 128) dst[i++] = *p; else if ((*p & 0xC0) == 0xC0) dst[i++] = '?';
    dst[i] = 0;
}
mfmt_t *vorbis_open(const uint8_t *data, uint32_t len, mdec_info_t *info) {
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(data, (int)len, &err, NULL);
    if (!v) return NULL;
    stb_vorbis_info vi = stb_vorbis_get_info(v);
    unsigned int frames = stb_vorbis_stream_length_in_samples(v);
    info->rate = (int)vi.sample_rate; info->channels = vi.channels;
    info->ms = vi.sample_rate ? (uint32_t)((uint64_t)frames * 1000 / vi.sample_rate) : 0;
    info->kbps = info->ms ? (int)((uint64_t)len * 8 / info->ms) : 0;
    snprintf(info->format, sizeof info->format, "Ogg Vorbis");
    stb_vorbis_comment cm = stb_vorbis_get_comment(v);
    for (int i = 0; i < cm.comment_list_length; i++) comment(cm.comment_list[i], info);
    stb_vorbis_seek_start(v);
    struct mfmt *f = malloc(sizeof *f);
    if (!f) { stb_vorbis_close(v); return NULL; }
    f->v = v; f->ch = vi.channels;
    return f;
}
int vorbis_read(mfmt_t *f, int16_t *pcm, int max) { return stb_vorbis_get_samples_short_interleaved(f->v, 2, pcm, max * 2); }
void vorbis_close(mfmt_t *f) { if (f) { stb_vorbis_close(f->v); free(f); } }
