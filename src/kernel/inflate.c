/* DEFLATE / zlib decompressor (RFC 1950/1951) */
#include "kernel.h"
#include "fs.h"

typedef struct { uint16_t counts[16], symbols[288]; } huff_t;
typedef struct {
    const uint8_t *src, *end; uint32_t bitbuf; int bitcnt;
    uint8_t *dst; uint32_t pos, cap; bool err;
} inf_t;

static int getbit(inf_t *s) {
    if (!s->bitcnt) { if (s->src >= s->end) { s->err = true; return 0; } s->bitbuf = *s->src++; s->bitcnt = 8; }
    int b = s->bitbuf & 1; s->bitbuf >>= 1; s->bitcnt--; return b;
}
static uint32_t getbits(inf_t *s, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= getbit(s) << i; return v; }
static void build(huff_t *h, const uint8_t *lens, int n) {
    uint16_t offs[16];
    memset(h->counts, 0, sizeof h->counts);
    for (int i = 0; i < n; i++) h->counts[lens[i]]++;
    h->counts[0] = 0;
    offs[0] = 0; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->counts[i];
    for (int i = 0; i < n; i++) if (lens[i]) h->symbols[offs[lens[i]]++] = i;
}
static int decode(inf_t *s, huff_t *h) {
    int code = 0, first = 0, idx = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbit(s);
        int cnt = h->counts[len];
        if (code - cnt < first) return h->symbols[idx + (code - first)];
        idx += cnt; first += cnt; first <<= 1; code <<= 1;
        if (s->err) return -1;
    }
    s->err = true; return -1;
}
static const uint16_t lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t lextra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t dextra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int block(inf_t *s, huff_t *lt, huff_t *dt) {
    for (;;) {
        int sym = decode(s, lt);
        if (s->err || sym < 0) return -1;
        if (sym < 256) { if (s->pos >= s->cap) return -2; s->dst[s->pos++] = sym; }
        else if (sym == 256) return 0;
        else {
            sym -= 257; if (sym >= 29) return -1;
            int len = lbase[sym] + getbits(s, lextra[sym]);
            int ds = decode(s, dt); if (ds < 0 || ds >= 30) return -1;
            uint32_t dist = dbase[ds] + getbits(s, dextra[ds]);
            if (dist > s->pos) return -1;
            if (s->pos + len > s->cap) return -2;
            for (int i = 0; i < len; i++, s->pos++) s->dst[s->pos] = s->dst[s->pos - dist];
        }
    }
}
int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t cap, uint32_t *outlen) {
    inf_t s = { src, src + srclen, 0, 0, dst, 0, cap, false };
    static huff_t lt, dt;
    int final;
    do {
        final = getbit(&s);
        int type = getbits(&s, 2);
        if (type == 0) {
            s.bitcnt = 0;
            if (s.src + 4 > s.end) return -1;
            uint16_t len = s.src[0] | (s.src[1] << 8); s.src += 4;
            if (s.src + len > s.end || s.pos + len > s.cap) return -1;
            memcpy(s.dst + s.pos, s.src, len); s.src += len; s.pos += len;
        } else if (type == 1) {
            uint8_t l[288];
            for (int i = 0; i < 144; i++) l[i] = 8; for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7; for (int i = 280; i < 288; i++) l[i] = 8;
            build(&lt, l, 288);
            for (int i = 0; i < 30; i++) l[i] = 5;
            build(&dt, l, 30);
            if (block(&s, &lt, &dt)) return -1;
        } else if (type == 2) {
            static const uint8_t ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            int hlit = getbits(&s, 5) + 257, hdist = getbits(&s, 5) + 1, hclen = getbits(&s, 4) + 4;
            uint8_t lens[320] = { 0 };
            for (int i = 0; i < hclen; i++) lens[ord[i]] = getbits(&s, 3);
            huff_t ct; build(&ct, lens, 19);
            memset(lens, 0, sizeof lens);
            for (int n = 0; n < hlit + hdist;) {
                int sym = decode(&s, &ct);
                if (sym < 0) return -1;
                if (sym < 16) lens[n++] = sym;
                else {
                    int rep, val = 0;
                    if (sym == 16) { if (!n) return -1; val = lens[n - 1]; rep = 3 + getbits(&s, 2); }
                    else if (sym == 17) rep = 3 + getbits(&s, 3);
                    else rep = 11 + getbits(&s, 7);
                    while (rep-- && n < hlit + hdist) lens[n++] = val;
                }
            }
            build(&lt, lens, hlit);
            build(&dt, lens + hlit, hdist);
            if (block(&s, &lt, &dt)) return -1;
        } else return -1;
        if (s.err) return -1;
    } while (!final);
    *outlen = s.pos;
    return 0;
}
int inflate_zlib(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t cap, uint32_t *outlen) {
    if (srclen < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31) return -1;
    return inflate_raw(src + 2, srclen - 2, dst, cap, outlen);
}
