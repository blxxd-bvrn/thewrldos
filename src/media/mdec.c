/* One interface over every decoder (src/include/mediadec.h): formats are told
 * apart by their contents, so a misnamed file still plays. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mediadec.h"
#include "mp3glue.h"
#include <stddef.h>

/* The layout of mdec_info_t for the Rust copy in rust/src/pcm.rs (WAV, AIFF and .au),
 * which rust_init checks at boot. */
const uint32_t rust_mdec_layout[4] = { offsetof(mdec_info_t, ms), offsetof(mdec_info_t, kbps), offsetof(mdec_info_t, format), sizeof ((mdec_info_t *)0)->format };

enum { K_MP3, K_VORBIS, K_FLAC, K_MPEG, K_PCM };
struct mdec { int kind; void *h; int16_t buf[1152 * 2]; int buf_n, buf_at; };

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ext_is(const char *ext, const char *e) {
    if (!ext) return 0;
    while (*ext && *e) if (lower(*ext++) != *e++) return 0;
    return !*ext && !*e;
}
int mdec_known_ext(const char *ext) {
    static const char *exts[] = { "mp3", "mp2", "mp1", "mpa", "ogg", "oga", "flac", "fla", "wav", "wave", "aif", "aiff", "aifc", "au", "snd", "mpg", "mpeg", "m1v", "mpe" };
    for (unsigned i = 0; i < sizeof exts / sizeof *exts; i++) if (ext_is(ext, exts[i])) return 1;
    return 0;
}
static int ogg_has(const uint8_t *d, uint32_t n, const char *codec, int clen) {
    if (n < 64 || memcmp(d, "OggS", 4)) return 0;
    uint32_t segs = d[26], body = 27 + segs;
    return body + (uint32_t)clen <= n && !memcmp(d + body, codec, (size_t)clen);
}
static int sniff(const uint8_t *d, uint32_t n, const char *ext) {
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) return K_PCM;
    if (n >= 12 && !memcmp(d, "FORM", 4) && (!memcmp(d + 8, "AIFF", 4) || !memcmp(d + 8, "AIFC", 4))) return K_PCM;
    if (n >= 4 && !memcmp(d, ".snd", 4)) return K_PCM;
    if (n >= 4 && !memcmp(d, "fLaC", 4)) return K_FLAC;
    if (ogg_has(d, n, "\x01vorbis", 7)) return K_VORBIS;
    if (ogg_has(d, n, "\x7f" "FLAC", 5)) return K_FLAC;
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && (d[3] == 0xBA || d[3] == 0xB3)) return K_MPEG;
    if (ext_is(ext, "mpg") || ext_is(ext, "mpeg") || ext_is(ext, "m1v") || ext_is(ext, "mpe")) return K_MPEG;
    return K_MP3;                                   /* MPEG audio: frames can start anywhere */
}

mdec_t *mdec_open(const uint8_t *d, uint32_t n, const char *ext, mdec_info_t *info, char *err, int errcap) {
    mdec_info_t tmp; if (!info) info = &tmp;
    memset(info, 0, sizeof *info);
    struct mdec *m = calloc(1, sizeof *m);
    if (!m) { snprintf(err, errcap, "not enough memory"); return NULL; }
    m->kind = sniff(d, n, ext);
    switch (m->kind) {
    case K_PCM: m->h = pcm_open(d, n, info, err, errcap); if (!m->h) { free(m); return NULL; } break;
    case K_FLAC: m->h = flac_open(d, n, info); break;
    case K_VORBIS: m->h = vorbis_open(d, n, info); break;
    case K_MPEG: m->h = mpeg_open(d, n, info); break;
    case K_MP3: {
        uint32_t ms; int rate, kbps;
        if (mp3_probe(d, n, &ms, &rate, &kbps)) break;
        m->h = mp3s_open(d, n);
        info->rate = rate; info->channels = 2; info->ms = ms; info->kbps = kbps;
        /* the layer from the first valid frame header, after any ID3v2 tag */
        uint32_t o = 0;
        if (n >= 10 && !memcmp(d, "ID3", 3)) o = 10 + (((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F));
        snprintf(info->format, sizeof info->format, "MP3");
        for (; o + 4 < n; o++) {
            const uint8_t *h = d + o;
            int layer = (h[1] >> 1) & 3, br = h[2] >> 4, sr = (h[2] >> 2) & 3;
            if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || !layer || br == 15 || br == 0 || sr == 3) continue;
            snprintf(info->format, sizeof info->format, "MP%d", 4 - layer);
            break;
        }
        break;
    }
    }
    if (!m->h) {
        static const char *names[] = { "an MPEG audio (MP3) file", "an Ogg Vorbis file", "a FLAC file", "an MPEG-1 file", "" };
        snprintf(err, errcap, "the file is damaged or is not %s", names[m->kind]);
        free(m); return NULL;
    }
    return m;
}
int mdec_read(mdec_t *m, int16_t *pcm, int max) {
    switch (m->kind) {
    case K_PCM: return pcm_read(m->h, pcm, max);
    case K_FLAC: return flac_read(m->h, pcm, max);
    case K_VORBIS: return vorbis_read(m->h, pcm, max);
    case K_MPEG: return mpeg_read(m->h, pcm, max);
    }
    /* MP3: whole frames, mono made stereo */
    int out = 0;
    while (out + 1152 <= max) {
        int ch, rate;
        int nf = mp3s_read(m->h, m->buf, &ch, &rate);
        if (nf <= 0) break;
        for (int i = 0; i < nf; i++) {
            pcm[(out + i) * 2] = m->buf[i * ch];
            pcm[(out + i) * 2 + 1] = m->buf[i * ch + (ch > 1)];
        }
        out += nf;
    }
    return out;
}
void mdec_close(mdec_t *m) {
    if (!m) return;
    switch (m->kind) {
    case K_PCM: pcm_close(m->h); break;
    case K_FLAC: flac_close(m->h); break;
    case K_VORBIS: vorbis_close(m->h); break;
    case K_MPEG: mpeg_close(m->h); break;
    default: mp3s_close(m->h); break;
    }
    free(m);
}
int mdec_probe(const uint8_t *d, uint32_t n, const char *ext, mdec_info_t *info) {
    char err[8];
    mdec_t *m = mdec_open(d, n, ext, info, err, sizeof err);
    if (!m) return -1;
    mdec_close(m);
    return 0;
}
