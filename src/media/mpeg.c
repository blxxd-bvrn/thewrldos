/* MPEG-1 program streams (.mpg, .mpeg) through PL_MPEG (third_party/pl_mpeg):
 * the MP2 audio for the player's sound, and the video pictures on demand. */
#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../third_party/pl_mpeg/pl_mpeg.h"
#include "mediadec.h"

struct mfmt { plm_t *p; plm_samples_t *cur; int at; uint32_t silent; int rate; };

mfmt_t *mpeg_open(const uint8_t *data, uint32_t len, mdec_info_t *info) {
    plm_t *p = plm_create_with_memory((uint8_t *)data, len, 0);
    if (!p) return NULL;
    if (!plm_probe(p, 5000 * 1024)) { plm_destroy(p); return NULL; }
    plm_set_video_enabled(p, 0);
    plm_set_loop(p, 0);
    double dur = plm_get_duration(p);
    info->video = plm_get_num_video_streams(p) > 0;
    info->width = plm_get_width(p); info->height = plm_get_height(p); info->fps = (float)plm_get_framerate(p);
    int audio = plm_get_num_audio_streams(p) > 0;
    info->rate = audio ? plm_get_samplerate(p) : 44100; info->channels = 2;
    info->ms = dur > 0 ? (uint32_t)(dur * 1000) : 0;
    info->kbps = info->ms ? (int)((uint64_t)len * 8 / info->ms) : 0;
    snprintf(info->format, sizeof info->format, info->video ? "MPEG-1 video%s" : "MPEG audio (MP2)", info->video && audio ? " + MP2" : "");
    struct mfmt *f = calloc(1, sizeof *f);
    if (!f) { plm_destroy(p); return NULL; }
    f->p = p; f->rate = info->rate;
    if (!audio) { plm_set_audio_enabled(p, 0); f->silent = (uint32_t)((uint64_t)info->ms * info->rate / 1000); }
    else plm_set_audio_enabled(p, 1);
    return f;
}
static int16_t s16(float v) { int x = (int)(v * 32767.0f); return (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x); }
int mpeg_read(mfmt_t *f, int16_t *pcm, int max) {
    if (f->silent) {                                   /* a video without sound: silence for its length */
        int n = (uint32_t)max < f->silent ? max : (int)f->silent;
        memset(pcm, 0, sizeof(int16_t) * 2 * n);
        f->silent -= n;
        return n;
    }
    int n = 0;
    while (n < max) {
        if (!f->cur || f->at >= (int)f->cur->count) {
            f->cur = plm_decode_audio(f->p); f->at = 0;
            if (!f->cur) break;
        }
        int take = (int)f->cur->count - f->at;
        if (take > max - n) take = max - n;
        for (int i = 0; i < take; i++) {
            pcm[(n + i) * 2] = s16(f->cur->interleaved[(f->at + i) * 2]);
            pcm[(n + i) * 2 + 1] = s16(f->cur->interleaved[(f->at + i) * 2 + 1]);
        }
        n += take; f->at += take;
    }
    return n;
}
void mpeg_close(mfmt_t *f) { if (f) { plm_destroy(f->p); free(f); } }

/* ---------------- pictures ---------------- */
struct mvid { plm_t *p; plm_frame_t *frame; double shown; uint8_t *bgra; int w, h; };
mvid_t *mvid_open(const uint8_t *data, uint32_t len, int *w, int *h, float *fps) {
    plm_t *p = plm_create_with_memory((uint8_t *)data, len, 0);
    if (!p) return NULL;
    if (!plm_probe(p, 5000 * 1024) || plm_get_num_video_streams(p) < 1) { plm_destroy(p); return NULL; }
    plm_set_audio_enabled(p, 0);
    plm_set_loop(p, 0);
    struct mvid *v = calloc(1, sizeof *v);
    if (!v) { plm_destroy(p); return NULL; }
    v->p = p; v->w = plm_get_width(p); v->h = plm_get_height(p); v->shown = -1;
    v->bgra = malloc((size_t)v->w * v->h * 4);
    if (!v->bgra) { plm_destroy(p); free(v); return NULL; }
    *w = v->w; *h = v->h; *fps = (float)plm_get_framerate(p);
    return v;
}
int mvid_frame(mvid_t *v, double t, uint32_t *px, int stride) {
    double step = 1.0 / (plm_get_framerate(v->p) > 0 ? plm_get_framerate(v->p) : 25);
    /* far behind or a jump backwards: seek; otherwise decode forward to t */
    if (!v->frame || t < v->frame->time - step || t > v->frame->time + 2.0) {
        v->frame = plm_seek_frame(v->p, t, 0);
        if (!v->frame) return 0;
    } else {
        while (v->frame->time + step <= t) {
            plm_frame_t *f = plm_decode_video(v->p);
            if (!f) break;
            v->frame = f;
        }
    }
    if (v->frame->time == v->shown) return 0;
    v->shown = v->frame->time;
    plm_frame_to_bgra(v->frame, v->bgra, v->w * 4);
    for (int y = 0; y < v->h; y++) {
        const uint32_t *s = (const uint32_t *)(v->bgra + (size_t)y * v->w * 4);
        uint32_t *d = px + (size_t)y * stride;
        for (int x = 0; x < v->w; x++) d[x] = s[x] | 0xFF000000u;
    }
    return 1;
}
void mvid_close(mvid_t *v) { if (v) { plm_destroy(v->p); free(v->bgra); free(v); } }
