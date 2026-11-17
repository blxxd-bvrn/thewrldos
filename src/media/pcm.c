/* Uncompressed and simply compressed audio files: WAV (PCM 8/16/24/32-bit,
 * 32/64-bit float, A-law, mu-law, IMA ADPCM, Microsoft ADPCM, and
 * WAVE_FORMAT_EXTENSIBLE around those), AIFF and AIFC (big- and little-endian
 * PCM, float, A-law, mu-law) and Sun/NeXT .au. Output is 16-bit stereo. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mediadec.h"

enum { E_INT, E_FLOAT, E_ULAW, E_ALAW, E_IMA, E_MSADPCM };
struct mfmt {
    const uint8_t *data; uint32_t len;  /* sample data */
    uint32_t frames, pos;               /* in frames */
    int ch, bits, enc, be;
    uint32_t block;                     /* ADPCM: bytes per block */
    uint32_t spb;                       /* ADPCM: frames per block */
    int16_t *blk; uint32_t blk_n, blk_pos, blk_index;   /* the decoded block */
};

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int16_t ulaw(uint8_t u) {
    u = ~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? 0x84 - t : t - 0x84);
}
static int16_t alaw(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4, seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8; else if (seg == 1) t += 0x108; else { t += 0x108; t <<= seg - 1; }
    return (int16_t)((a & 0x80) ? t : -t);
}
/* 80-bit IEEE extended (AIFF sample rate) */
static uint32_t ext80(const uint8_t *p) {
    int e = ((p[0] & 0x7F) << 8 | p[1]) - 16383 - 31;
    uint32_t m = be32(p + 2);
    if (e > 0) return 0;
    return e < -31 ? 0 : m >> -e;
}

static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* decode ADPCM block number `b` into f->blk (interleaved, f->ch channels) */
static void adpcm_block(struct mfmt *f, uint32_t b) {
    const uint8_t *p = f->data + (size_t)b * f->block;
    uint32_t avail = f->len - b * f->block; if (avail > f->block) avail = f->block;
    int ch = f->ch;
    memset(f->blk, 0, sizeof(int16_t) * f->spb * ch);
    f->blk_n = 0; f->blk_pos = 0; f->blk_index = b;
    if (f->enc == E_IMA) {
        if (avail < 4u * ch) return;
        int pred[8], idx[8];
        for (int c = 0; c < ch; c++) {
            pred[c] = (int16_t)le16(p + c * 4); idx[c] = p[c * 4 + 2]; if (idx[c] > 88) idx[c] = 88;
            f->blk[c] = (int16_t)pred[c];
        }
        uint32_t n = 1, o = 4 * ch;
        /* then groups of 4 bytes (8 samples) per channel, in turn */
        while (o + 4u * ch <= avail && n + 8 <= f->spb) {
            for (int c = 0; c < ch; c++) {
                for (int k = 0; k < 8; k++) {
                    int nib = (p[o + c * 4 + k / 2] >> ((k & 1) * 4)) & 15, step = ima_step[idx[c]];
                    int diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    pred[c] = clamp16(nib & 8 ? pred[c] - diff : pred[c] + diff);
                    idx[c] += ima_index[nib]; if (idx[c] < 0) idx[c] = 0; if (idx[c] > 88) idx[c] = 88;
                    f->blk[(n + k) * ch + c] = (int16_t)pred[c];
                }
            }
            o += 4 * ch; n += 8;
        }
        f->blk_n = n;
    } else {                                                       /* Microsoft ADPCM */
        static const int coef[7][2] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 } };
        static const int adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
        if (avail < 7u * ch) return;
        int pi[2], delta[2], s1[2], s2[2];
        for (int c = 0; c < ch; c++) {
            pi[c] = p[c] < 7 ? p[c] : 0;
            delta[c] = (int16_t)le16(p + ch + c * 2);
            s1[c] = (int16_t)le16(p + 3 * ch + c * 2);
            s2[c] = (int16_t)le16(p + 5 * ch + c * 2);
            f->blk[c] = (int16_t)s2[c]; f->blk[ch + c] = (int16_t)s1[c];
        }
        uint32_t n = 2, o = 7 * ch; int c = 0, half = 0;
        while (o < avail && n < f->spb) {
            int nib = half ? p[o] & 15 : p[o] >> 4;
            int sn = nib & 8 ? nib - 16 : nib;
            int pred = (s1[c] * coef[pi[c]][0] + s2[c] * coef[pi[c]][1]) / 256 + sn * delta[c];
            pred = clamp16(pred);
            s2[c] = s1[c]; s1[c] = pred;
            delta[c] = delta[c] * adapt[nib] / 256; if (delta[c] < 16) delta[c] = 16;
            f->blk[n * ch + c] = (int16_t)pred;
            if (++c == ch) { c = 0; n++; }
            if (half) o++;
            half ^= 1;
        }
        f->blk_n = n;
    }
}

static int sample(struct mfmt *f, const uint8_t *p) {
    int b = f->bits / 8;
    if (f->enc == E_ULAW) return ulaw(*p);
    if (f->enc == E_ALAW) return alaw(*p);
    if (f->enc == E_FLOAT) {
        double v;
        if (b == 4) { uint32_t u = f->be ? be32(p) : le32(p); float fl; memcpy(&fl, &u, 4); v = fl; }
        else { uint64_t u = f->be ? ((uint64_t)be32(p) << 32 | be32(p + 4)) : ((uint64_t)le32(p + 4) << 32 | le32(p)); memcpy(&v, &u, 8); }
        if (v != v) v = 0;
        return clamp16((int)(v * 32767));
    }
    switch (b) {
    case 1: return f->be ? (int8_t)p[0] << 8 : (p[0] - 128) << 8;   /* WAV 8-bit is unsigned, AIFF signed */
    case 2: return (int16_t)(f->be ? be16(p) : le16(p));
    case 3: return (int16_t)(f->be ? be16(p) : le16(p + 1));
    default: return (int16_t)(f->be ? be16(p) : le16(p + 2));
    }
}

mfmt_t *pcm_open(const uint8_t *d, uint32_t n, mdec_info_t *info, char *err, int errcap) {
    struct mfmt f; memset(&f, 0, sizeof f);
    uint32_t rate = 0;
    const char *what = "";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {
        int fmt = 0; uint32_t ba = 0;
        for (uint32_t o = 12; o + 8 <= n;) {
            uint32_t cl = le32(d + o + 4);
            if (!memcmp(d + o, "fmt ", 4) && cl >= 16 && o + 8 + 16 <= n) {
                fmt = le16(d + o + 8); f.ch = le16(d + o + 10); rate = le32(d + o + 12); ba = le16(d + o + 20); f.bits = le16(d + o + 22);
                if (fmt == 0xFFFE && cl >= 40 && o + 8 + 26 <= n) fmt = le16(d + o + 32);
            } else if (!memcmp(d + o, "data", 4)) { f.data = d + o + 8; f.len = cl < n - o - 8 ? cl : n - o - 8; }
            if (cl > n) break;
            o += 8 + cl + (cl & 1);
        }
        switch (fmt) {
        case 1: f.enc = E_INT; what = "WAV (PCM)"; break;
        case 3: f.enc = E_FLOAT; what = "WAV (float)"; break;
        case 6: f.enc = E_ALAW; f.bits = 8; what = "WAV (A-law)"; break;
        case 7: f.enc = E_ULAW; f.bits = 8; what = "WAV (mu-law)"; break;
        case 0x11: f.enc = E_IMA; what = "WAV (IMA ADPCM)"; break;
        case 2: f.enc = E_MSADPCM; what = "WAV (MS ADPCM)"; break;
        default: snprintf(err, errcap, "WAV format 0x%x is not supported", fmt); return NULL;
        }
        if (f.enc == E_IMA || f.enc == E_MSADPCM) {
            if (!ba || f.ch < 1 || f.ch > 2) { snprintf(err, errcap, "bad ADPCM header"); return NULL; }
            f.block = ba;
            f.spb = f.enc == E_IMA ? (ba - 4 * f.ch) * 8 / (4 * f.ch) + 1 : (ba - 7 * f.ch) * 8 / (4 * f.ch) + 2;
            uint32_t blocks = f.len / ba + (f.len % ba ? 1 : 0);
            f.frames = blocks * f.spb;
            f.bits = 4;
        }
    } else if (n >= 12 && !memcmp(d, "FORM", 4) && (!memcmp(d + 8, "AIFF", 4) || !memcmp(d + 8, "AIFC", 4))) {
        int aifc = !memcmp(d + 8, "AIFC", 4);
        f.be = 1; f.enc = E_INT; what = aifc ? "AIFC" : "AIFF";
        for (uint32_t o = 12; o + 8 <= n;) {
            uint32_t cl = be32(d + o + 4);
            if (!memcmp(d + o, "COMM", 4) && cl >= 18 && o + 26 <= n) {
                f.ch = be16(d + o + 8); f.bits = be16(d + o + 14); rate = ext80(d + o + 16);
                if (aifc && cl >= 22 && o + 30 <= n) {
                    const uint8_t *c = d + o + 26;
                    if (!memcmp(c, "sowt", 4)) f.be = 0;
                    else if (!memcmp(c, "fl32", 4) || !memcmp(c, "FL32", 4)) { f.enc = E_FLOAT; f.bits = 32; }
                    else if (!memcmp(c, "fl64", 4) || !memcmp(c, "FL64", 4)) { f.enc = E_FLOAT; f.bits = 64; }
                    else if (!memcmp(c, "ulaw", 4) || !memcmp(c, "ULAW", 4)) { f.enc = E_ULAW; f.bits = 8; }
                    else if (!memcmp(c, "alaw", 4) || !memcmp(c, "ALAW", 4)) { f.enc = E_ALAW; f.bits = 8; }
                    else if (memcmp(c, "NONE", 4) && memcmp(c, "twos", 4)) { snprintf(err, errcap, "AIFC compression %.4s is not supported", (const char *)c); return NULL; }
                }
            } else if (!memcmp(d + o, "SSND", 4) && cl >= 8 && o + 16 <= n) {
                uint32_t off = be32(d + o + 8);
                f.data = d + o + 16 + off; f.len = cl - 8 - off;
                if (o + 16 + off > n) f.len = 0; else if (f.len > n - (o + 16 + off)) f.len = n - (o + 16 + off);
            }
            if (cl > n) break;
            o += 8 + cl + (cl & 1);
        }
    } else if (n >= 24 && !memcmp(d, ".snd", 4)) {
        uint32_t off = be32(d + 4), size = be32(d + 8), enc = be32(d + 12);
        rate = be32(d + 16); f.ch = (int)be32(d + 20); f.be = 1;
        if (off > n) off = n;
        f.data = d + off; f.len = (size == 0xFFFFFFFFu || size > n - off) ? n - off : size;
        what = "Sun audio";
        switch (enc) {
        case 1: f.enc = E_ULAW; f.bits = 8; break;
        case 2: f.enc = E_INT; f.bits = 8; break;
        case 3: f.enc = E_INT; f.bits = 16; break;
        case 4: f.enc = E_INT; f.bits = 24; break;
        case 5: f.enc = E_INT; f.bits = 32; break;
        case 6: f.enc = E_FLOAT; f.bits = 32; break;
        case 7: f.enc = E_FLOAT; f.bits = 64; break;
        case 27: f.enc = E_ALAW; f.bits = 8; break;
        default: snprintf(err, errcap, ".au encoding %u is not supported", (unsigned)enc); return NULL;
        }
    } else { snprintf(err, errcap, "not a WAV, AIFF or .au file"); return NULL; }
    if (!f.data || f.ch < 1 || f.ch > 8 || rate < 1000 || rate > 384000) { snprintf(err, errcap, "the file's audio header is damaged"); return NULL; }
    if (f.enc == E_INT && f.bits != 8 && f.bits != 16 && f.bits != 24 && f.bits != 32) { snprintf(err, errcap, "%d-bit samples are not supported", f.bits); return NULL; }
    if (f.enc == E_FLOAT && f.bits != 32 && f.bits != 64) { snprintf(err, errcap, "%d-bit float is not supported", f.bits); return NULL; }
    if (f.enc != E_IMA && f.enc != E_MSADPCM) f.frames = f.len / (uint32_t)(f.ch * (f.bits / 8));
    struct mfmt *m = malloc(sizeof *m);
    if (!m) { snprintf(err, errcap, "not enough memory"); return NULL; }
    *m = f;
    if (f.spb) {
        m->blk = malloc(sizeof(int16_t) * f.spb * f.ch);
        if (!m->blk) { free(m); snprintf(err, errcap, "not enough memory"); return NULL; }
        m->blk_index = 0xFFFFFFFFu;
    }
    info->rate = (int)rate; info->channels = f.ch;
    info->ms = (uint32_t)((uint64_t)f.frames * 1000 / rate);
    info->kbps = f.spb ? (int)((uint64_t)f.len * 8 / (info->ms ? info->ms : 1)) : (int)((uint64_t)rate * f.ch * f.bits / 1000);
    snprintf(info->format, sizeof info->format, "%s", what);
    return m;
}

int pcm_read(mfmt_t *f, int16_t *out, int max) {
    int n = 0;
    while (n < max && f->pos < f->frames) {
        int l, r;
        if (f->spb) {
            uint32_t b = f->pos / f->spb, k = f->pos % f->spb;
            if (b != f->blk_index) adpcm_block(f, b);
            if (k >= f->blk_n) { f->pos = (b + 1) * f->spb; continue; }
            l = f->blk[k * f->ch]; r = f->ch > 1 ? f->blk[k * f->ch + 1] : l;
        } else {
            int bps = f->bits / 8;
            const uint8_t *p = f->data + (size_t)f->pos * f->ch * bps;
            l = sample(f, p); r = f->ch > 1 ? sample(f, p + bps) : l;
            if (f->ch > 2) {                                         /* fold the centre and surrounds in */
                int c = f->ch > 2 ? sample(f, p + 2 * bps) : 0;
                l = clamp16((l * 3 + c * 2) / 4); r = clamp16((r * 3 + c * 2) / 4);
            }
        }
        out[n * 2] = (int16_t)l; out[n * 2 + 1] = (int16_t)r;
        n++; f->pos++;
    }
    return n;
}
void pcm_close(mfmt_t *f) { if (f) { free(f->blk); free(f); } }
