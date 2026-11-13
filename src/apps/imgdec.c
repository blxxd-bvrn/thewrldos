/* Image decoders for the browser and Image Viewer: PNG (all colour types,
 * bit depths and Adam7 interlacing), GIF (first frame, transparency),
 * baseline and progressive JPEG (Huffman, any sampling factors, restart
 * markers) and BMP. Output canvases hold 0xAARRGGBB pixels; alpha is kept for
 * blending. */
#include "kernel.h"
#include "gfx.h"
#include "fs.h"
#include "apps.h"

#define MAX_PIXELS (16 * 1024 * 1024)
static canvas_t *new_img(int w, int h) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (uint64_t)w * h > MAX_PIXELS) return NULL;
    return canvas_new(w, h);
}
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* ======================= PNG ======================= */
static int paeth(int a, int b, int c) { int p = a + b - c, pa = ABSDIFF(p, a), pb = ABSDIFF(p, b), pc = ABSDIFF(p, c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
static canvas_t *png_decode(const uint8_t *d, uint32_t n, char *err, int ecap) {
    uint32_t w = 0, h = 0; int depth = 0, ct = 0, inter = 0;
    uint32_t pal[256]; int npal = 0; for (int i = 0; i < 256; i++) pal[i] = 0xFF000000;
    int trns_g = -1, trns_r = -1, trns_gg = -1, trns_b = -1;
    uint8_t *idat = NULL; uint32_t idlen = 0, idcap = 0;
    for (uint32_t o = 8; o + 12 <= n;) {
        uint32_t len = be32(d + o); const uint8_t *type = d + o + 4, *cd = d + o + 8;
        if (o + 12 + len > n) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) { w = be32(cd); h = be32(cd + 4); depth = cd[8]; ct = cd[9]; inter = cd[12]; }
        else if (!memcmp(type, "PLTE", 4)) { npal = MIN(256u, len / 3); for (int i = 0; i < npal; i++) pal[i] = RGB(cd[i * 3], cd[i * 3 + 1], cd[i * 3 + 2]); }
        else if (!memcmp(type, "tRNS", 4)) {
            if (ct == 3) for (uint32_t i = 0; i < len && i < 256; i++) pal[i] = (pal[i] & 0xFFFFFF) | ((uint32_t)cd[i] << 24);
            else if (ct == 0 && len >= 2) trns_g = (cd[0] << 8) | cd[1];
            else if (ct == 2 && len >= 6) { trns_r = (cd[0] << 8) | cd[1]; trns_gg = (cd[2] << 8) | cd[3]; trns_b = (cd[4] << 8) | cd[5]; }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idlen + len > idcap) { idcap = (idlen + len) * 2; idat = krealloc(idat, idcap); }
            memcpy(idat + idlen, cd, len); idlen += len;
        } else if (!memcmp(type, "IEND", 4)) break;
        o += 12 + len;
    }
    int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (!w || !ch || !idat || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) { snprintf(err, ecap, "unsupported PNG"); kfree(idat); return NULL; }
    canvas_t *img = new_img(w, h);
    if (!img) { snprintf(err, ecap, "PNG too large"); kfree(idat); return NULL; }
    int bpp = MAX(1, ch * depth / 8);                       /* bytes per complete pixel for filtering */
    /* Adam7 passes (or one pass for non-interlaced images) */
    static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay[7] = { 0, 0, 4, 0, 2, 0, 1 }, dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    uint32_t total = 0;
    for (int p = 0; p < (inter ? 7 : 1); p++) {
        uint32_t pw = inter ? (w - ax[p] + dx[p] - 1) / dx[p] : w, ph = inter ? (h - ay[p] + dy[p] - 1) / dy[p] : h;
        if (ax[p] >= (int)w || ay[p] >= (int)h) pw = ph = 0;
        if (pw && ph) total += ph * (1 + (pw * ch * depth + 7) / 8);
    }
    uint8_t *raw = kmalloc(total + 1); uint32_t got = 0;
    if (inflate_zlib(idat, idlen, raw, total, &got) < 0 && got < total) { snprintf(err, ecap, "corrupt PNG data"); kfree(idat); kfree(raw); canvas_free(img); return NULL; }
    kfree(idat);
    const uint8_t *rp = raw;
    for (int p = 0; p < (inter ? 7 : 1); p++) {
        uint32_t pw = inter ? (w - ax[p] + dx[p] - 1) / dx[p] : w, ph = inter ? (h - ay[p] + dy[p] - 1) / dy[p] : h;
        if (inter && (ax[p] >= (int)w || ay[p] >= (int)h)) continue;
        if (!pw || !ph) continue;
        uint32_t stride = (pw * ch * depth + 7) / 8;
        uint8_t *prev = kzalloc(stride), *cur = kmalloc(stride);
        for (uint32_t y = 0; y < ph; y++) {
            if (rp + 1 + stride > raw + got) break;
            int f = *rp++;
            memcpy(cur, rp, stride); rp += stride;
            for (uint32_t i = 0; i < stride; i++) {
                int a = i >= (uint32_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (uint32_t)bpp ? prev[i - bpp] : 0;
                switch (f) { case 1: cur[i] += a; break; case 2: cur[i] += b; break; case 3: cur[i] += (a + b) / 2; break; case 4: cur[i] += paeth(a, b, c); break; }
            }
            for (uint32_t x = 0; x < pw; x++) {
                uint32_t px;
                int s16 = depth == 16;
                #define S(k) (s16 ? cur[(x * ch + (k)) * 2] : cur[x * ch + (k)])
                #define V(k) (s16 ? (cur[(x * ch + (k)) * 2] << 8 | cur[(x * ch + (k)) * 2 + 1]) : cur[x * ch + (k)])
                if (depth < 8) {
                    int bit = x * depth, v = (cur[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
                    if (ct == 3) px = pal[v];
                    else { int g = v * 255 / ((1 << depth) - 1); px = (trns_g == v ? 0 : 0xFF000000u) | (g << 16) | (g << 8) | g; }
                } else if (ct == 3) px = pal[cur[x]];
                else if (ct == 0) { int g = S(0); px = (trns_g == V(0) ? 0 : 0xFF000000u) | (g << 16) | (g << 8) | g; }
                else if (ct == 4) { int g = S(0); px = ((uint32_t)S(1) << 24) | (g << 16) | (g << 8) | g; }
                else if (ct == 2) px = ((trns_r == V(0) && trns_gg == V(1) && trns_b == V(2)) ? 0 : 0xFF000000u) | (S(0) << 16) | (S(1) << 8) | S(2);
                else px = ((uint32_t)S(3) << 24) | (S(0) << 16) | (S(1) << 8) | S(2);
                #undef S
                #undef V
                uint32_t ix = inter ? ax[p] + x * dx[p] : x, iy = inter ? ay[p] + y * dy[p] : y;
                img->px[iy * img->stride + ix] = px;
            }
            uint8_t *t = prev; prev = cur; cur = t;
        }
        kfree(prev); kfree(cur);
    }
    kfree(raw);
    return img;
}

/* ======================= GIF ======================= */
static canvas_t *gif_decode(const uint8_t *d, uint32_t n, char *err, int ecap) {
    if (n < 13) return NULL;
    int sw = d[6] | (d[7] << 8), sh = d[8] | (d[9] << 8), flags = d[10];
    uint32_t gpal[256] = { 0 }, o = 13;
    if (flags & 0x80) { int gn = 2 << (flags & 7); for (int i = 0; i < gn && o + 3 <= n; i++, o += 3) gpal[i] = RGB(d[o], d[o + 1], d[o + 2]); }
    int transp = -1;
    while (o < n) {
        uint8_t b = d[o++];
        if (b == 0x21) {                                   /* extension */
            uint8_t label = d[o++];
            if (label == 0xF9 && o + 5 < n && (d[o + 1] & 1)) transp = d[o + 4];
            while (o < n && d[o]) o += d[o] + 1;
            o++;
        } else if (b == 0x2C) {                            /* image */
            if (o + 9 > n) break;
            int ix = d[o] | (d[o + 1] << 8), iy = d[o + 2] | (d[o + 3] << 8), iw = d[o + 4] | (d[o + 5] << 8), ih = d[o + 6] | (d[o + 7] << 8), lf = d[o + 8];
            o += 9;
            uint32_t pal[256]; memcpy(pal, gpal, sizeof pal);
            if (lf & 0x80) { int ln = 2 << (lf & 7); for (int i = 0; i < ln && o + 3 <= n; i++, o += 3) pal[i] = RGB(d[o], d[o + 1], d[o + 2]); }
            if (transp >= 0) pal[transp] = 0;
            if (!sw || !sh) { sw = iw; sh = ih; }
            canvas_t *img = new_img(sw, sh);
            if (!img) { snprintf(err, ecap, "GIF too large"); return NULL; }
            memset(img->px, 0, sw * sh * 4);
            int minc = d[o++];
            if (minc < 2 || minc > 8) { canvas_free(img); return NULL; }
            /* gather sub-blocks */
            uint8_t *lzw = kmalloc(n); uint32_t ll = 0;
            while (o < n && d[o]) { uint32_t bl = d[o]; if (o + 1 + bl > n) break; memcpy(lzw + ll, d + o + 1, bl); ll += bl; o += bl + 1; }
            /* LZW */
            static uint16_t prefix[4096]; static uint8_t suffix[4096], stack[4097];
            int clear = 1 << minc, eoi = clear + 1, avail = clear + 2, csize = minc + 1, cmask = (1 << csize) - 1, old = -1, first = 0;
            for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = i; }
            uint32_t bits = 0; int nbits = 0, pix = 0, total = iw * ih;
            int pass = 0, row = 0, col = 0;
            static const int istart[4] = { 0, 4, 2, 1 }, istep[4] = { 8, 8, 4, 2 };
            for (uint32_t i = 0; i < ll && pix < total;) {
                while (nbits < csize && i < ll) { bits |= (uint32_t)lzw[i++] << nbits; nbits += 8; }
                if (nbits < csize) break;
                int code = bits & cmask; bits >>= csize; nbits -= csize;
                if (code == clear) { csize = minc + 1; cmask = (1 << csize) - 1; avail = clear + 2; old = -1; continue; }
                if (code == eoi) break;
                int sp = 0, c = code;
                if (old == -1) { stack[sp++] = suffix[code]; first = code; old = code; }
                else {
                    if (code >= avail) { stack[sp++] = first; c = old; }
                    while (c >= clear && sp < 4096) { stack[sp++] = suffix[c]; c = prefix[c]; }
                    stack[sp++] = first = suffix[c];
                    if (avail < 4096) { prefix[avail] = old; suffix[avail] = first; avail++; if ((avail & cmask) == 0 && avail < 4096) { csize++; cmask = (1 << csize) - 1; } }
                    old = code;
                }
                while (sp > 0 && pix < total) {
                    int v = stack[--sp];
                    int px = ix + col, py = iy + ((lf & 0x40) ? row : pix / iw);
                    if (px < sw && py < sh && pal[v]) img->px[py * img->stride + px] = pal[v];
                    pix++;
                    if (++col >= iw) {
                        col = 0;
                        if (lf & 0x40) { row += istep[pass]; while (row >= ih && pass < 3) { pass++; row = istart[pass]; } }
                    }
                }
            }
            kfree(lzw);
            return img;
        } else break;
    }
    snprintf(err, ecap, "no image in GIF");
    return NULL;
}

/* ======================= JPEG ======================= */
/* Baseline and progressive (the multiple-scan form). A progressive frame
 * transmits the coefficients in several scans (DC first, AC first, then
 * refinements); they are accumulated in a coefficient block per component and
 * turned into pixels once every scan has been read. */
typedef struct { uint8_t bits[17]; uint8_t vals[256]; int maxcode[18], valptr[17], mincode[17]; uint8_t look[256][2]; } jhuff_t;
typedef struct { int id, h, v, tq, td, ta, bw, bh, bwx, bhy, iw, ih; int pred; uint8_t *data; int16_t *coef; } jcomp_t;
typedef struct {
    const uint8_t *d; uint32_t n, pos; uint32_t bitbuf; int bitcnt; bool marker;
    uint16_t q[4][64]; jhuff_t hd[4], ha[4];
    int w, h, ncomp, hmax, vmax, mcux, mcuy, restart;
    jcomp_t c[4];
    bool progressive;
    int sc[4], nsc;                        /* components of the scan being read */
    int Ss, Se, Ah, Al, eobrun;
} jpeg_t;
static const uint8_t zz[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
static void huff_build(jhuff_t *t) {
    int code = 0, k = 0;
    memset(t->look, 0, sizeof t->look);
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k; t->mincode[l] = code;
        for (int i = 0; i < t->bits[l]; i++) {
            if (l <= 8) for (int f = 0; f < (1 << (8 - l)); f++) { int idx = (code << (8 - l)) | f; t->look[idx][0] = l; t->look[idx][1] = t->vals[k]; }
            code++; k++;
        }
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
}
static void fill(jpeg_t *j) {
    while (j->bitcnt <= 24) {
        uint32_t b = 0;
        if (!j->marker && j->pos < j->n) {
            b = j->d[j->pos++];
            if (b == 0xFF) { uint8_t nx = j->pos < j->n ? j->d[j->pos] : 0; if (nx == 0) j->pos++; else { j->marker = true; j->pos--; b = 0; } }
        }
        j->bitbuf |= b << (24 - j->bitcnt); j->bitcnt += 8;
    }
}
static int getbits(jpeg_t *j, int n) { if (!n) return 0; fill(j); int v = j->bitbuf >> (32 - n); j->bitbuf <<= n; j->bitcnt -= n; return v; }
static int decode_huff(jpeg_t *j, jhuff_t *t) {
    fill(j);
    int idx = j->bitbuf >> 24;
    if (t->look[idx][0]) { int l = t->look[idx][0]; j->bitbuf <<= l; j->bitcnt -= l; return t->look[idx][1]; }
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | (j->bitbuf >> 31); j->bitbuf <<= 1; j->bitcnt--;
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) return t->vals[t->valptr[l] + code - t->mincode[l]];
        if (j->bitcnt < 8) fill(j);
    }
    return 0;
}
static int extend(int v, int n) { return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }
static int32_t idct_cos[8][8];
static void idct_init(void) {
    /* cos((2x+1) u pi / 16) * C(u) / 2, scaled by 2^12; computed with integer CORDIC-free table */
    static const int32_t c[8] = { 4096, 4017, 3784, 3406, 2896, 2276, 1567, 799 };   /* 4096*cos(k*pi/16) */
    for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) {
        int k = ((2 * x + 1) * u) % 32, s = 1;
        if (k > 16) k = 32 - k;
        if (k > 8) { k = 16 - k; s = -1; }
        int32_t v = s * c[k];
        if (u == 0) v = 2896;                        /* 1/sqrt(2) */
        idct_cos[x][u] = v / 2;
    }
}
static void idct8x8(const int *in, uint8_t *out, int stride) {
    int32_t tmp[64];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        int64_t s = 0; for (int u = 0; u < 8; u++) s += (int64_t)idct_cos[x][u] * in[y * 8 + u];
        tmp[y * 8 + x] = (int32_t)(s >> 8);
    }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        int64_t s = 0; for (int v = 0; v < 8; v++) s += (int64_t)idct_cos[y][v] * tmp[v * 8 + x];
        int val = (int)(s >> 16) + 128;
        out[y * stride + x] = val < 0 ? 0 : val > 255 ? 255 : val;
    }
}
/* One baseline block: dequantised into natural order. */
static bool decode_block(jpeg_t *j, jcomp_t *c, int *blk) {
    memset(blk, 0, 64 * sizeof(int));
    int t = decode_huff(j, &j->hd[c->td]);
    int diff = t ? extend(getbits(j, t), t) : 0;
    c->pred += diff;
    blk[0] = c->pred * j->q[c->tq][0];
    for (int k = 1; k < 64;) {
        int rs = decode_huff(j, &j->ha[c->ta]), r = rs >> 4, s = rs & 15;
        if (!s) { if (r != 15) break; k += 16; continue; }
        k += r;
        if (k > 63) return false;
        blk[zz[k]] = extend(getbits(j, s), s) * j->q[c->tq][k];
        k++;
    }
    return true;
}
/* One progressive block of the scan being read. Coefficients are kept
 * dequantised later, so only the transmitted value is stored here. */
static void prog_block(jpeg_t *j, jcomp_t *c, int16_t *blk)
{
    if (j->Ss == 0) {
        if (j->Ah == 0) {                                    /* DC first */
            int t = decode_huff(j, &j->hd[c->td]);
            int diff = t ? extend(getbits(j, t), t) : 0;
            c->pred += diff;
            blk[0] = (int16_t)(c->pred << j->Al);
        } else {                                             /* DC refinement */
            if (getbits(j, 1)) blk[0] |= (int16_t)(1 << j->Al);
        }
        return;
    }
    if (j->Ah == 0) {                                        /* AC first */
        if (j->eobrun > 0) { j->eobrun--; return; }
        for (int k = j->Ss; k <= j->Se; k++) {
            int rs = decode_huff(j, &j->ha[c->ta]), r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) {
                    j->eobrun = (1 << r) - 1;
                    if (r) j->eobrun += getbits(j, r);
                    break;
                }
                k += 15;
                continue;
            }
            k += r;
            if (k > j->Se) break;                            /* corrupt: stop this block */
            blk[zz[k]] = (int16_t)(extend(getbits(j, s), s) << j->Al);
        }
        return;
    }
    /* AC refinement: correct the coefficients already sent, then add the new
     * ones this scan carries. An end-of-band run stops the symbol loop, but
     * the block still carries correction bits for its remaining non-zero
     * coefficients; blocks inside the run carry only those bits. */
    int k = j->Ss;
    if (!j->eobrun) {
        for (; k <= j->Se; k++) {
            int rs = decode_huff(j, &j->ha[c->ta]), r = rs >> 4, s = rs & 15;
            if (s) {
                s = getbits(j, 1) ? (1 << j->Al) : -(1 << j->Al);
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += getbits(j, r);
                break;
            }
            while (k <= j->Se) {                             /* skip zeros, correct non-zeros */
                if (blk[zz[k]] != 0) {
                    if (getbits(j, 1) && (blk[zz[k]] & (1 << j->Al)) == 0)
                        blk[zz[k]] += (int16_t)(blk[zz[k]] > 0 ? (1 << j->Al) : -(1 << j->Al));
                } else {
                    if (--r < 0) break;
                }
                k++;
            }
            if (s && k <= j->Se) blk[zz[k]] = (int16_t)s;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= j->Se; k++)                              /* correction bits only */
            if (blk[zz[k]] != 0 && getbits(j, 1) && (blk[zz[k]] & (1 << j->Al)) == 0)
                blk[zz[k]] += (int16_t)(blk[zz[k]] > 0 ? (1 << j->Al) : -(1 << j->Al));
        j->eobrun--;
    }
}
static int16_t *prog_coef(jcomp_t *c, int bx, int by) { return c->coef + ((size_t)by * c->bwx + bx) * 64; }

/* Read one scan's entropy-coded data. A scan with one component is laid out
 * block by block; one with several is laid out in MCUs like a baseline frame. */
static void prog_scan(jpeg_t *j)
{
    j->bitcnt = 0; j->bitbuf = 0; j->marker = false;      /* a scan starts byte-aligned */
    j->eobrun = 0;
    for (int i = 0; i < j->nsc; i++) j->c[j->sc[i]].pred = 0;
    int mcus = 0;
    if (j->nsc == 1) {
        jcomp_t *c = &j->c[j->sc[0]];
        for (int by = 0; by < c->ih; by++) for (int bx = 0; bx < c->iw; bx++) {
            if (j->restart && mcus && mcus % j->restart == 0) {
                j->bitcnt = j->bitbuf = 0; j->marker = false;
                while (j->pos + 1 < j->n && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) j->pos++;
                j->pos += 2;
                j->eobrun = 0;
                for (int i = 0; i < j->nsc; i++) j->c[j->sc[i]].pred = 0;
            }
            prog_block(j, c, prog_coef(c, bx, by));
            mcus++;
        }
        return;
    }
    for (int my = 0; my < j->mcuy; my++) for (int mx = 0; mx < j->mcux; mx++) {
        if (j->restart && mcus && mcus % j->restart == 0) {
            j->bitcnt = j->bitbuf = 0; j->marker = false;
            while (j->pos + 1 < j->n && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) j->pos++;
            j->pos += 2;
            j->eobrun = 0;
            for (int i = 0; i < j->nsc; i++) j->c[j->sc[i]].pred = 0;
        }
        for (int i = 0; i < j->nsc; i++) {
            jcomp_t *c = &j->c[j->sc[i]];
            for (int by = 0; by < c->v; by++) for (int bx = 0; bx < c->h; bx++)
                prog_block(j, c, prog_coef(c, mx * c->h + bx, my * c->v + by));
        }
        mcus++;
    }
}
/* Turn the component planes (or the progressive coefficients) into a canvas. */
static canvas_t *jpeg_output(jpeg_t *j)
{
    if (j->progressive) {
        for (int k = 0; k < j->ncomp; k++) {
            jcomp_t *c = &j->c[k];
            if (!c->coef) continue;
            int blk[64];
            for (int by = 0; by < c->ih; by++) for (int bx = 0; bx < c->iw; bx++) {
                int16_t *cf = prog_coef(c, bx, by);
                for (int k = 0; k < 64; k++) blk[zz[k]] = cf[zz[k]] * j->q[c->tq][k];
                idct8x8(blk, c->data + (by * 8) * c->bw + bx * 8, c->bw);
            }
        }
    }
    canvas_t *img = new_img(j->w, j->h);
    if (img) for (int y = 0; y < j->h; y++) for (int x = 0; x < j->w; x++) {
        jcomp_t *c0 = &j->c[0];
        int Y = c0->data[(y * c0->v / j->vmax) * c0->bw + x * c0->h / j->hmax];
        if (j->ncomp == 1) { img->px[y * img->stride + x] = RGB(Y, Y, Y); continue; }
        jcomp_t *c1 = &j->c[1], *c2 = &j->c[2];
        int cb = c1->data[(y * c1->v / j->vmax) * c1->bw + x * c1->h / j->hmax] - 128;
        int cr = c2->data[(y * c2->v / j->vmax) * c2->bw + x * c2->h / j->hmax] - 128;
        int r = Y + ((91881 * cr) >> 16), g = Y - ((22554 * cb + 46802 * cr) >> 16), b = Y + ((116130 * cb) >> 16);
        img->px[y * img->stride + x] = RGB(CLAMP(r, 0, 255), CLAMP(g, 0, 255), CLAMP(b, 0, 255));
    }
    return img;
}
static canvas_t *jpeg_decode(const uint8_t *d, uint32_t n, char *err, int ecap) {
    static bool inited; if (!inited) { idct_init(); inited = true; }
    jpeg_t *j = kzalloc(sizeof *j);
    j->d = d; j->n = n; j->pos = 2;
    canvas_t *img = NULL;
    bool frame = false, coefs = false;
    while (j->pos + 2 <= n) {
        if (d[j->pos] != 0xFF) { j->pos++; continue; }
        uint8_t m = d[j->pos + 1];
        if (m == 0xFF) { j->pos++; continue; }
        j->pos += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) { if (j->progressive && coefs) img = jpeg_output(j); break; }
        if (j->pos + 2 > n) break;
        uint32_t len = (d[j->pos] << 8) | d[j->pos + 1];
        const uint8_t *s = d + j->pos + 2;
        if (j->pos + len > n) break;
        if (m == 0xDB) {
            for (uint32_t o = 0; o + 1 < len - 2;) {
                int pq = s[o] >> 4, tq = s[o] & 3; o++;
                for (int k = 0; k < 64; k++) { j->q[tq][k] = pq ? (s[o] << 8 | s[o + 1]) : s[o]; o += pq ? 2 : 1; }
            }
        } else if (m == 0xC4) {
            for (uint32_t o = 0; o < len - 2;) {
                int tc = s[o] >> 4, th = s[o] & 3; o++;
                jhuff_t *t = tc ? &j->ha[th] : &j->hd[th];
                int total = 0; t->bits[0] = 0;
                for (int k = 1; k <= 16; k++) { t->bits[k] = s[o++]; total += t->bits[k]; }
                for (int k = 0; k < total && k < 256; k++) t->vals[k] = s[o++];
                huff_build(t);
            }
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            j->progressive = (m == 0xC2);
            j->h = (s[1] << 8) | s[2]; j->w = (s[3] << 8) | s[4]; j->ncomp = s[5];
            if (j->ncomp != 1 && j->ncomp != 3) break;
            for (int i = 0; i < j->ncomp; i++) { j->c[i].id = s[6 + i * 3]; j->c[i].h = s[7 + i * 3] >> 4; j->c[i].v = s[7 + i * 3] & 15; j->c[i].tq = s[8 + i * 3] & 3;
                j->hmax = MAX(j->hmax, j->c[i].h); j->vmax = MAX(j->vmax, j->c[i].v); }
            frame = true;
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax); j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int k = 0; k < j->ncomp; k++) {
                jcomp_t *c = &j->c[k];
                c->bw = j->mcux * c->h * 8; c->bh = j->mcuy * c->v * 8;
                c->bwx = j->mcux * c->h; c->bhy = j->mcuy * c->v;
                /* A scan carrying one component codes only the blocks that
                 * cover the component itself, without the MCU padding. */
                c->iw = (j->w * c->h + 8 * j->hmax - 1) / (8 * j->hmax);
                c->ih = (j->h * c->v + 8 * j->vmax - 1) / (8 * j->vmax);
                c->data = kmalloc((size_t)c->bw * c->bh);
                if (j->progressive) {
                    c->coef = kzalloc((size_t)c->bwx * c->bhy * 64 * sizeof(int16_t));
                    if (c->coef) coefs = true;
                }
            }
        } else if (m == 0xDD) j->restart = (s[0] << 8) | s[1];
        else if (m == 0xDA) {
            if (!frame) break;
            j->nsc = s[0];
            if (j->nsc < 1 || j->nsc > j->ncomp) break;
            for (int i = 0; i < j->nsc; i++) {
                j->sc[i] = 0;
                for (int k = 0; k < j->ncomp; k++) if (j->c[k].id == s[1 + i * 2]) { j->sc[i] = k; j->c[k].td = s[2 + i * 2] >> 4; j->c[k].ta = s[2 + i * 2] & 15; }
            }
            j->Ss = s[1 + j->nsc * 2];
            j->Se = s[2 + j->nsc * 2];
            j->Ah = s[3 + j->nsc * 2] >> 4;
            j->Al = s[3 + j->nsc * 2] & 15;
            j->pos += len;
            if (j->progressive) {
                if (!coefs) break;
                prog_scan(j);
                /* Step over the entropy-coded bytes to the next real marker;
                 * stuffed 0xFF 0x00 and restart markers are part of the data. */
                while (j->pos + 1 < n) {
                    if (d[j->pos] != 0xFF) { j->pos++; continue; }
                    uint8_t nx = d[j->pos + 1];
                    if (nx == 0xFF) { j->pos++; continue; }
                    if (nx == 0 || (nx >= 0xD0 && nx <= 0xD7)) { j->pos += 2; continue; }
                    break;
                }
                continue;
            } else {
                int blk[64], mcus = 0;
                for (int my = 0; my < j->mcuy; my++) for (int mx = 0; mx < j->mcux; mx++) {
                    if (j->restart && mcus && mcus % j->restart == 0) {
                        j->bitcnt = 0; j->bitbuf = 0; j->marker = false;
                        while (j->pos + 1 < n && !(d[j->pos] == 0xFF && d[j->pos + 1] >= 0xD0 && d[j->pos + 1] <= 0xD7)) j->pos++;
                        j->pos += 2;
                        for (int k = 0; k < j->ncomp; k++) j->c[k].pred = 0;
                    }
                    for (int k = 0; k < j->ncomp; k++) {
                        jcomp_t *c = &j->c[k];
                        for (int by = 0; by < c->v; by++) for (int bx = 0; bx < c->h; bx++) {
                            decode_block(j, c, blk);
                            idct8x8(blk, c->data + ((my * c->v + by) * 8) * c->bw + (mx * c->h + bx) * 8, c->bw);
                        }
                    }
                    mcus++;
                }
                img = jpeg_output(j);
            }
            if (!j->progressive) break;
        }
        j->pos += len;
    }
    for (int k = 0; k < j->ncomp; k++) { kfree(j->c[k].data); kfree(j->c[k].coef); }
    if (!img) snprintf(err, ecap, j->progressive ? "incomplete progressive JPEG" : "unsupported JPEG");
    kfree(j);
    return img;
}

canvas_t *image_decode(const uint8_t *d, uint32_t n, char *err, int ecap) {
    err[0] = 0;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return png_decode(d, n, err, ecap);
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return gif_decode(d, n, err, ecap);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return jpeg_decode(d, n, err, ecap);
    { extern bool svg_sniff(const uint8_t *, uint32_t); extern canvas_t *svg_decode(const uint8_t *, uint32_t);
      if (svg_sniff(d, n)) { canvas_t *c = svg_decode(d, n); if (!c) snprintf(err, ecap, "unreadable SVG"); return c; } }
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') { canvas_t *c = bmp_load(d, n); if (!c) snprintf(err, ecap, "unsupported BMP"); return c; }
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) snprintf(err, ecap, "WebP images are not supported");
    else if (n >= 5 && (!memcmp(d, "<svg", 4) || !memcmp(d, "<?xml", 5))) snprintf(err, ecap, "SVG images are not supported");
    else snprintf(err, ecap, "unknown image format");
    return NULL;
}
