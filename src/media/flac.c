/* FLAC and Ogg FLAC through dr_flac (third_party/dr_flac). */
/* built freestanding: not a Linux program (dr_flac would want <endian.h>) */
#undef __linux__
#undef linux
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../third_party/dr_flac/dr_flac.h"
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

struct mfmt { drflac *d; int16_t *tmp; uint32_t tmp_frames; };

static void copy_tag(char *dst, size_t cap, const char *v, uint32_t n) {
    if (dst[0]) return;
    size_t i = 0;
    for (uint32_t k = 0; k < n && i + 1 < cap; k++) if ((unsigned char)v[k] < 128) dst[i++] = v[k]; else if ((v[k] & 0xC0) == 0xC0) dst[i++] = '?';
    dst[i] = 0;
}
static void on_meta(void *user, drflac_metadata *m) {
    mdec_info_t *info = user;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
    drflac_uint32 len; const char *c;
    while ((c = drflac_next_vorbis_comment(&it, &len))) {
        if (len > 6 && !ci_n(c, "TITLE=", 6)) copy_tag(info->title, sizeof info->title, c + 6, len - 6);
        else if (len > 7 && !ci_n(c, "ARTIST=", 7)) copy_tag(info->artist, sizeof info->artist, c + 7, len - 7);
        else if (len > 6 && !ci_n(c, "ALBUM=", 6)) copy_tag(info->album, sizeof info->album, c + 6, len - 6);
    }
}
mfmt_t *flac_open(const uint8_t *data, uint32_t len, mdec_info_t *info) {
    drflac *d = drflac_open_memory_with_metadata(data, len, on_meta, info, NULL);
    if (!d) return NULL;
    info->rate = (int)d->sampleRate; info->channels = d->channels;
    info->ms = d->sampleRate ? (uint32_t)(d->totalPCMFrameCount * 1000 / d->sampleRate) : 0;
    info->kbps = info->ms ? (int)((uint64_t)len * 8 / info->ms) : 0;
    snprintf(info->format, sizeof info->format, "FLAC (%d-bit)", d->bitsPerSample);
    struct mfmt *f = calloc(1, sizeof *f);
    if (!f) { drflac_close(d); return NULL; }
    f->d = d;
    return f;
}
int flac_read(mfmt_t *f, int16_t *pcm, int max) {
    int ch = f->d->channels;
    if (ch == 2) return (int)drflac_read_pcm_frames_s16(f->d, (drflac_uint64)max, pcm);
    if (f->tmp_frames < (uint32_t)max) {
        free(f->tmp); f->tmp = malloc(sizeof(int16_t) * max * ch); f->tmp_frames = f->tmp ? max : 0;
        if (!f->tmp) return 0;
    }
    int n = (int)drflac_read_pcm_frames_s16(f->d, (drflac_uint64)max, f->tmp);
    for (int i = 0; i < n; i++) {
        int l = f->tmp[i * ch], r = ch > 1 ? f->tmp[i * ch + 1] : l;
        pcm[i * 2] = (int16_t)l; pcm[i * 2 + 1] = (int16_t)r;
    }
    return n;
}
void flac_close(mfmt_t *f) { if (f) { drflac_close(f->d); free(f->tmp); free(f); } }
