/* A small TrueType engine. See ttf.h for the interface.
 *
 * Layout: the sfnt table directory, the Unicode cmaps (format 4 and 12), the
 * horizontal metrics, and the 'glyf' outlines including composite glyphs. The
 * rasteriser flattens the quadratic curves and fills them with 8 horizontal
 * scanlines per pixel, measuring each span exactly, so a pixel ends up with
 * 512 levels of coverage before it is scaled to 0..255.
 *
 * Hinting instructions are ignored. That costs a little sharpness at small
 * sizes but keeps the engine small and deterministic, and the same source runs
 * in the kernel and on the host. */
#include "ttf.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

#define TAG4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                          ((uint32_t)(c) << 8) | (uint32_t)(d))

#define F2DOT14_ONE 16384
#define MAX_POINTS  (1 << 16)
#define MAX_EDGES   20000
#define SUBSAMPLES  8
#define MAX_PX      512

struct ttf_face {
    const uint8_t *data;
    uint32_t size;
    ttf_alloc_fn alloc;
    ttf_free_fn  free;
    void *user;

    uint32_t head, head_len;
    uint32_t maxp, maxp_len;
    uint32_t hhea, hhea_len;
    uint32_t hmtx, hmtx_len;
    uint32_t cmap, cmap_len;
    uint32_t loca, loca_len;
    uint32_t glyf, glyf_len;
    uint32_t kern, kern_len;

    uint32_t num_glyphs;
    uint32_t num_hmetrics;
    int      units_per_em;
    int      ascender, descender, line_gap;
    int      loca_long;

    uint32_t cmap_off;                 /* the chosen Unicode subtable */
    int      cmap_format;              /* 4 or 12 */

    uint32_t kern_off, kern_pairs;     /* format 0, horizontal */
};

/* ---------------- reading ---------------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  rs16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int in_file(const ttf_face_t *f, uint32_t off, uint32_t n) {
    return off <= f->size && n <= f->size - off;
}

/* a bounds-checked reader over one glyph */
typedef struct { const uint8_t *p, *end; int ok; } cur_t;
static uint8_t cur8(cur_t *c) {
    if (!c->ok || c->end - c->p < 1) { c->ok = 0; return 0; }
    return *c->p++;
}
static uint16_t cur16(cur_t *c) {
    if (!c->ok || c->end - c->p < 2) { c->ok = 0; return 0; }
    uint16_t v = rd16(c->p); c->p += 2; return v;
}
static int16_t curs16(cur_t *c) { return (int16_t)cur16(c); }

/* ---------------- opening ---------------- */
static void face_free(ttf_face_t *f) { f->free(f->user, f); }

ttf_face_t *ttf_open(const void *data, size_t size, ttf_alloc_fn alloc,
                     ttf_free_fn free_fn, void *user) {
    if (!data || size < 12 || !alloc || !free_fn) return NULL;
    const uint8_t *d = (const uint8_t *)data;
    uint32_t base = 0;
    uint32_t version = rd32(d);
    if (version == TAG4('t', 't', 'c', 'f')) {           /* collection: first face */
        if (size < 16) return NULL;
        uint32_t count = rd32(d + 8);
        base = rd32(d + 12);
        if (!count || base > size || size - base < 12) return NULL;
        version = rd32(d + base);
    }
    if (version != 0x00010000u && version != TAG4('t', 'r', 'u', 'e') &&
        version != TAG4('t', 'y', 'p', '1')) return NULL;            /* OTTO is CFF: unsupported */

    uint16_t num_tables = rd16(d + base + 4);

    ttf_face_t *f = (ttf_face_t *)alloc(user, sizeof *f);
    if (!f) return NULL;
    memset(f, 0, sizeof *f);
    f->data = d;
    f->size = (uint32_t)(size > 0xFFFFFFFFu ? 0xFFFFFFFFu : size);
    f->alloc = alloc;
    f->free = free_fn;
    f->user = user;

    for (uint16_t i = 0; i < num_tables; i++) {
        uint32_t rec = base + 12 + (uint32_t)i * 16;
        if (!in_file(f, rec, 16)) break;
        uint32_t tag = rd32(d + rec);
        uint32_t off = rd32(d + rec + 8);
        uint32_t len = rd32(d + rec + 12);
        if (!in_file(f, off, len)) continue;
        switch (tag) {
        case TAG4('h', 'e', 'a', 'd'): f->head = off; f->head_len = len; break;
        case TAG4('m', 'a', 'x', 'p'): f->maxp = off; f->maxp_len = len; break;
        case TAG4('h', 'h', 'e', 'a'): f->hhea = off; f->hhea_len = len; break;
        case TAG4('h', 'm', 't', 'x'): f->hmtx = off; f->hmtx_len = len; break;
        case TAG4('c', 'm', 'a', 'p'): f->cmap = off; f->cmap_len = len; break;
        case TAG4('l', 'o', 'c', 'a'): f->loca = off; f->loca_len = len; break;
        case TAG4('g', 'l', 'y', 'f'): f->glyf = off; f->glyf_len = len; break;
        case TAG4('k', 'e', 'r', 'n'): f->kern = off; f->kern_len = len; break;
        default: break;
        }
    }
    if (!f->head || f->head_len < 54 || !f->maxp || f->maxp_len < 6 ||
        !f->hhea || f->hhea_len < 36 || !f->cmap || !f->hmtx ||
        !f->loca || !f->glyf || !f->loca_len || !f->glyf_len) goto bad;

    f->units_per_em = rd16(d + f->head + 18);
    f->loca_long = rs16(d + f->head + 50) != 0;
    if (f->units_per_em < 16 || f->units_per_em > 16384) goto bad;
    f->num_glyphs = rd16(d + f->maxp + 4);
    f->num_hmetrics = rd16(d + f->hhea + 34);
    if (!f->num_glyphs || !f->num_hmetrics) goto bad;
    f->ascender = rs16(d + f->hhea + 4);
    f->descender = rs16(d + f->hhea + 6);
    f->line_gap = rs16(d + f->hhea + 8);

    /* the best Unicode cmap: full repertoire first, then the BMP */
    if (f->cmap_len >= 4) {
        uint16_t n = rd16(d + f->cmap + 2);
        int best = 0;
        for (uint16_t i = 0; i < n; i++) {
            uint32_t rec = f->cmap + 4 + (uint32_t)i * 8;
            if (!in_file(f, rec, 8)) break;
            uint16_t plat = rd16(d + rec), enc = rd16(d + rec + 2);
            uint32_t sub = f->cmap + rd32(d + rec + 4);
            if (!in_file(f, sub, 4)) continue;
            uint16_t fmt = rd16(d + sub);
            int score = 0;
            if (fmt == 12) score = (plat == 3 && enc == 10) ? 5 : (plat == 0) ? 4 : 0;
            else if (fmt == 4) score = (plat == 3 && enc == 1) ? 3 : (plat == 0) ? 2 :
                                      (plat == 3 && enc == 0) ? 1 : 0;
            if (score > best) { best = score; f->cmap_off = sub; f->cmap_format = fmt; }
        }
    }

    /* kerning pairs, format 0, horizontal only */
    if (f->kern && f->kern_len >= 4 && rd16(d + f->kern) == 0) {
        uint16_t nt = rd16(d + f->kern + 2);
        uint32_t p = f->kern + 4;
        for (uint16_t i = 0; i < nt; i++) {
            if (!in_file(f, p, 6)) break;
            uint16_t len = rd16(d + p + 2), cov = rd16(d + p + 4);
            if (len < 14 || !in_file(f, p, len)) break;
            if ((cov >> 8) == 0 && (cov & 1) && !(cov & 0x0E)) {
                uint32_t np = rd16(d + p + 6);
                if (in_file(f, p + 14, np * 6u)) { f->kern_off = p + 14; f->kern_pairs = np; }
                break;
            }
            p += len;
        }
    }
    return f;
bad:
    face_free(f);
    return NULL;
}

void ttf_close(ttf_face_t *f) { if (f) face_free(f); }

int ttf_units_per_em(const ttf_face_t *f) { return f ? f->units_per_em : 0; }
int ttf_ascender(const ttf_face_t *f) { return f ? f->ascender : 0; }
int ttf_descender(const ttf_face_t *f) { return f ? f->descender : 0; }
int ttf_line_gap(const ttf_face_t *f) { return f ? f->line_gap : 0; }
int ttf_num_glyphs(const ttf_face_t *f) { return f ? (int)f->num_glyphs : 0; }

/* ---------------- cmap and metrics ---------------- */
static int cmap12(const ttf_face_t *f, uint32_t c) {
    const uint8_t *d = f->data;
    uint32_t p = f->cmap_off;
    if (!in_file(f, p, 16)) return 0;
    uint32_t n = rd32(d + p + 12);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t g = p + 16 + mid * 12;
        if (!in_file(f, g, 12)) return 0;
        uint32_t first = rd32(d + g), last = rd32(d + g + 4);
        if (c < first) hi = mid;
        else if (c > last) lo = mid + 1;
        else {
            uint32_t gid = rd32(d + g + 8) + (c - first);
            return gid < f->num_glyphs ? (int)gid : 0;
        }
    }
    return 0;
}
static int cmap4(const ttf_face_t *f, uint32_t c) {
    const uint8_t *d = f->data;
    uint32_t p = f->cmap_off;
    if (c > 0xFFFF || !in_file(f, p, 14)) return 0;
    uint32_t seg2 = rd16(d + p + 6), seg = seg2 / 2;
    uint32_t endp = p + 14, startp = endp + seg2 + 2;
    uint32_t deltap = startp + seg2, rangep = deltap + seg2;
    if (!seg || !in_file(f, endp, seg2 + 2 + seg2 * 3)) return 0;
    uint32_t lo = 0, hi = seg;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint16_t e = rd16(d + endp + mid * 2);
        if (c <= e) hi = mid; else lo = mid + 1;
    }
    if (lo >= seg) return 0;
    uint16_t start = rd16(d + startp + lo * 2);
    if (c < start) return 0;
    int16_t delta = rs16(d + deltap + lo * 2);
    uint16_t ro = rd16(d + rangep + lo * 2);
    if (!ro) return (uint16_t)(c + (uint16_t)delta) < f->num_glyphs ? (int)(uint16_t)(c + (uint16_t)delta) : 0;
    uint32_t g = rangep + lo * 2 + ro + (c - start) * 2;
    if (!in_file(f, g, 2)) return 0;
    uint16_t gid = rd16(d + g);
    if (!gid) return 0;
    gid = (uint16_t)(gid + (uint16_t)delta);
    return gid < f->num_glyphs ? (int)gid : 0;
}
int ttf_glyph_index(const ttf_face_t *f, uint32_t c) {
    if (!f) return 0;
    if (f->cmap_format == 12) return cmap12(f, c);
    if (f->cmap_format == 4) return cmap4(f, c);
    return 0;
}
int ttf_advance(const ttf_face_t *f, int gid) {
    if (!f || gid < 0 || (uint32_t)gid >= f->num_glyphs) return 0;
    uint32_t idx = (uint32_t)gid < f->num_hmetrics ? (uint32_t)gid : f->num_hmetrics - 1;
    uint32_t off = f->hmtx + idx * 4;
    if (!in_file(f, off, 2)) return 0;
    return rd16(f->data + off);
}
int ttf_kerning(const ttf_face_t *f, int left, int right) {
    if (!f || !f->kern_pairs || left < 0 || right < 0) return 0;
    uint32_t key = ((uint32_t)left << 16) | (uint32_t)right;
    uint32_t lo = 0, hi = f->kern_pairs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t p = f->kern_off + mid * 6;
        if (!in_file(f, p, 6)) return 0;
        uint32_t k = ((uint32_t)rd16(f->data + p) << 16) | rd16(f->data + p + 2);
        if (key < k) hi = mid;
        else if (key > k) lo = mid + 1;
        else return rs16(f->data + p + 4);
    }
    return 0;
}

/* ---------------- outlines ---------------- */
typedef struct { int32_t x, y; uint8_t on; } pt_t;
typedef struct {
    pt_t    *pts;  int n, cap;
    uint16_t *ends; int ne, ecap;
} outline_t;
typedef struct { int32_t a, b, c, d, e, f; } xform_t;   /* a..d F2Dot14, e,f font units */

static int ol_pts(const ttf_face_t *f, outline_t *o, int need) {
    if (need <= o->cap) return 1;
    if (need > MAX_POINTS) return 0;
    int cap = o->cap ? o->cap : 128;
    while (cap < need) cap <<= 1;
    pt_t *p = (pt_t *)f->alloc(f->user, (size_t)cap * sizeof *p);
    if (!p) return 0;
    if (o->pts) { memcpy(p, o->pts, (size_t)o->n * sizeof *p); f->free(f->user, o->pts); }
    o->pts = p; o->cap = cap;
    return 1;
}
static int ol_ends(const ttf_face_t *f, outline_t *o, int need) {
    if (need <= o->ecap) return 1;
    if (need > MAX_POINTS) return 0;
    int cap = o->ecap ? o->ecap : 32;
    while (cap < need) cap <<= 1;
    uint16_t *p = (uint16_t *)f->alloc(f->user, (size_t)cap * sizeof *p);
    if (!p) return 0;
    if (o->ends) { memcpy(p, o->ends, (size_t)o->ne * sizeof *p); f->free(f->user, o->ends); }
    o->ends = p; o->ecap = cap;
    return 1;
}

static int glyph_slice(const ttf_face_t *f, int gid, const uint8_t **g, uint32_t *n) {
    *g = NULL; *n = 0;
    if (gid < 0 || (uint32_t)gid >= f->num_glyphs) return -1;
    uint32_t a, b;
    if (f->loca_long) {
        if (!in_file(f, f->loca + (uint32_t)gid * 4, 8)) return -1;
        a = rd32(f->data + f->loca + (uint32_t)gid * 4);
        b = rd32(f->data + f->loca + (uint32_t)gid * 4 + 4);
    } else {
        if (!in_file(f, f->loca + (uint32_t)gid * 2, 4)) return -1;
        a = (uint32_t)rd16(f->data + f->loca + (uint32_t)gid * 2) * 2;
        b = (uint32_t)rd16(f->data + f->loca + (uint32_t)gid * 2 + 2) * 2;
    }
    if (b <= a) return 0;
    if (a >= f->glyf_len || b > f->glyf_len) return -1;
    *g = f->data + f->glyf + a;
    *n = b - a;
    return 1;
}

static int emit_simple(const ttf_face_t *f, const uint8_t *g, uint32_t glen,
                       const xform_t *x, outline_t *o) {
    cur_t c = { g, g + glen, 1 };
    int16_t ncont = curs16(&c);
    if (!c.ok || ncont <= 0 || ncont > 1024) return ncont == 0 ? 1 : -1;
    if ((uint32_t)(c.p - g) + 8 > glen) return -1;
    c.p += 8;                                            /* the bounding box */
    uint32_t endp_off = (uint32_t)(c.p - g);
    if (endp_off + (uint32_t)ncont * 2 + 2 > glen) return -1;
    uint32_t last = rd16(g + endp_off + (uint32_t)(ncont - 1) * 2);
    uint32_t npoints = last + 1;
    uint32_t ilen_at = endp_off + (uint32_t)ncont * 2;
    uint32_t ilen = rd16(g + ilen_at);
    uint32_t flags_at = ilen_at + 2 + ilen;
    if (flags_at > glen || npoints > MAX_POINTS) return -1;

    int base = o->n;
    if (!ol_pts(f, o, base + (int)npoints) || !ol_ends(f, o, o->ne + ncont)) return -1;

    /* the flags, with repeats expanded; they ride in .on until the end */
    cur_t fc = { g + flags_at, g + glen, 1 };
    uint32_t i = 0;
    while (i < npoints) {
        uint8_t fl = cur8(&fc);
        if (!fc.ok) return -1;
        o->pts[base + i++].on = fl;
        if (fl & 0x08) {
            uint8_t rep = cur8(&fc);
            if (!fc.ok) return -1;
            while (rep-- && i < npoints) o->pts[base + i++].on = fl;
        }
    }
    if (i != npoints) return -1;

    /* x deltas, then y deltas */
    int32_t v = 0;
    for (i = 0; i < npoints; i++) {
        uint8_t fl = o->pts[base + i].on;
        if (fl & 0x02) { uint8_t dd = cur8(&fc); v += (fl & 0x10) ? dd : -dd; }
        else if (!(fl & 0x10)) { v += curs16(&fc); }
        if (!fc.ok) return -1;
        o->pts[base + i].x = v;
    }
    v = 0;
    for (i = 0; i < npoints; i++) {
        uint8_t fl = o->pts[base + i].on;
        if (fl & 0x04) { uint8_t dd = cur8(&fc); v += (fl & 0x20) ? dd : -dd; }
        else if (!(fl & 0x20)) { v += curs16(&fc); }
        if (!fc.ok) return -1;
        o->pts[base + i].y = v;
    }

    for (i = 0; i < npoints; i++) {
        pt_t *p = &o->pts[base + i];
        int32_t px = p->x, py = p->y;
        p->x = (int32_t)(((int64_t)x->a * px + (int64_t)x->c * py) >> 14) + x->e;
        p->y = (int32_t)(((int64_t)x->b * px + (int64_t)x->d * py) >> 14) + x->f;
        p->on &= 1;
    }
    for (int k = 0; k < ncont; k++)
        o->ends[o->ne++] = (uint16_t)(base + rd16(g + endp_off + (uint32_t)k * 2) + 1);
    o->n = base + (int)npoints;
    return 1;
}

static xform_t xform_mul(const xform_t *p, const xform_t *c) {
    xform_t r;
    r.a = (int32_t)(((int64_t)p->a * c->a + (int64_t)p->c * c->b) >> 14);
    r.b = (int32_t)(((int64_t)p->b * c->a + (int64_t)p->d * c->b) >> 14);
    r.c = (int32_t)(((int64_t)p->a * c->c + (int64_t)p->c * c->d) >> 14);
    r.d = (int32_t)(((int64_t)p->b * c->c + (int64_t)p->d * c->d) >> 14);
    r.e = (int32_t)(((int64_t)p->a * c->e + (int64_t)p->c * c->f) >> 14) + p->e;
    r.f = (int32_t)(((int64_t)p->b * c->e + (int64_t)p->d * c->f) >> 14) + p->f;
    return r;
}

static int emit_glyph(const ttf_face_t *f, int gid, const xform_t *x, outline_t *o, int depth);

static int emit_composite(const ttf_face_t *f, const uint8_t *g, uint32_t glen,
                          const xform_t *x, outline_t *o, int depth) {
    if (depth >= 8) return -1;
    cur_t c = { g + 10, g + glen, 1 };
    for (;;) {
        uint16_t flags = cur16(&c);
        uint16_t comp = cur16(&c);
        if (!c.ok) return -1;
        int32_t dx = 0, dy = 0;
        if (flags & 0x0001) { dx = curs16(&c); dy = curs16(&c); }
        else if (flags & 0x0002) { dx = (int8_t)cur8(&c); dy = (int8_t)cur8(&c); }
        else { (void)cur8(&c); (void)cur8(&c); }         /* point matching: uncommon, treated as 0 */
        int32_t a = F2DOT14_ONE, b = 0, cc = 0, dd = F2DOT14_ONE;
        if (flags & 0x0008) { a = dd = curs16(&c); }
        else if (flags & 0x0040) { a = curs16(&c); dd = curs16(&c); }
        else if (flags & 0x0080) { a = curs16(&c); b = curs16(&c); cc = curs16(&c); dd = curs16(&c); }
        if (!c.ok) return -1;
        if ((flags & 0x0800) && !(flags & 0x1000)) {      /* scaled component offset */
            int32_t sx = (int32_t)(((int64_t)a * dx + (int64_t)cc * dy) >> 14);
            int32_t sy = (int32_t)(((int64_t)b * dx + (int64_t)dd * dy) >> 14);
            dx = sx; dy = sy;
        }
        xform_t cx = { a, b, cc, dd, dx, dy };
        xform_t full = xform_mul(x, &cx);
        int r = emit_glyph(f, comp, &full, o, depth + 1);
        if (r < 0) return r;
        if (!(flags & 0x0020)) break;
    }
    return 1;
}

static int emit_glyph(const ttf_face_t *f, int gid, const xform_t *x, outline_t *o, int depth) {
    const uint8_t *g;
    uint32_t n;
    int s = glyph_slice(f, gid, &g, &n);
    if (s <= 0) return s;
    if (n < 10) return -1;
    int16_t ncont = (int16_t)rd16(g);
    if (ncont >= 0) return emit_simple(f, g, n, x, o);
    return emit_composite(f, g, n, x, o, depth);
}

/* ---------------- flattening ---------------- */
typedef struct { int32_t x0, y0, x1, y1; } edge_t;
typedef struct { edge_t *e; int n, cap; } edges_t;
typedef struct { int32_t minx, miny, maxx, maxy; int any; } bbox_t;

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

static int edge_add(const ttf_face_t *f, edges_t *e, bbox_t *bb,
                    int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (!bb->any) {
        bb->minx = x0 < x1 ? x0 : x1; bb->maxx = x0 > x1 ? x0 : x1;
        bb->miny = y0 < y1 ? y0 : y1; bb->maxy = y0 > y1 ? y0 : y1;
        bb->any = 1;
    } else {
        if (x0 < bb->minx) bb->minx = x0;
        if (x1 < bb->minx) bb->minx = x1;
        if (x0 > bb->maxx) bb->maxx = x0;
        if (x1 > bb->maxx) bb->maxx = x1;
        if (y0 < bb->miny) bb->miny = y0;
        if (y1 < bb->miny) bb->miny = y1;
        if (y0 > bb->maxy) bb->maxy = y0;
        if (y1 > bb->maxy) bb->maxy = y1;
    }
    if (x0 == x1 && y0 == y1) return 1;
    if (e->n >= MAX_EDGES) return 0;
    if (e->n == e->cap) {
        int cap = e->cap ? e->cap * 2 : 64;
        if (cap > MAX_EDGES) cap = MAX_EDGES;
        edge_t *p = (edge_t *)f->alloc(f->user, (size_t)cap * sizeof *p);
        if (!p) return 0;
        if (e->e) { memcpy(p, e->e, (size_t)e->n * sizeof *p); f->free(f->user, e->e); }
        e->e = p; e->cap = cap;
    }
    e->e[e->n].x0 = x0; e->e[e->n].y0 = y0;
    e->e[e->n].x1 = x1; e->e[e->n].y1 = y1;
    e->n++;
    return 1;
}

static void to_screen(int32_t x, int32_t y, int scale_q, int slant, int32_t *sx, int32_t *sy) {
    int64_t px = ((int64_t)x * scale_q) >> 10;
    int64_t py = -(((int64_t)y * scale_q) >> 10);
    if (slant) px += ((int64_t)y * scale_q * slant) / (100LL * 1024LL);
    *sx = (int32_t)px;
    *sy = (int32_t)py;
}

static int flatten_quad(const ttf_face_t *f, edges_t *e, bbox_t *bb,
                        int32_t x0, int32_t y0, int32_t cx, int32_t cy,
                        int32_t x1, int32_t y1) {
    int32_t d = iabs32(cx - x0) + iabs32(cy - y0) + iabs32(x1 - cx) + iabs32(y1 - cy);
    int n = (int)(d >> 5);                              /* about half a pixel per piece */
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    int32_t px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        int64_t t = ((int64_t)i << 16) / n;
        int64_t u = 65536 - t;
        int32_t x = (int32_t)((u * u * x0 + 2 * u * t * cx + t * t * x1) >> 32);
        int32_t y = (int32_t)((u * u * y0 + 2 * u * t * cy + t * t * y1) >> 32);
        if (!edge_add(f, e, bb, px, py, x, y)) return 0;
        px = x; py = y;
    }
    return 1;
}

/* one contour, as a cyclic sequence with an on-curve first point */
typedef struct {
    const outline_t *o;
    int first, last, mode;          /* mode 0: start at first, 1: at last, 2: inserted midpoint */
    int m;
    int32_t mx, my;
} cont_t;

static pt_t cont_get(const cont_t *c, int i) {
    if (c->mode == 2) {
        if (i == 0) { pt_t p = { c->mx, c->my, 1 }; return p; }
        return c->o->pts[c->first + i - 1];
    }
    if (c->mode == 1) {
        if (i == 0) return c->o->pts[c->last];
        return c->o->pts[c->first + i - 1];
    }
    return c->o->pts[c->first + i];
}

static int contour_edges(const ttf_face_t *f, const outline_t *o, int first, int last,
                         int scale_q, int slant, edges_t *e, bbox_t *bb) {
    cont_t c;
    c.o = o; c.first = first; c.last = last;
    if (o->pts[first].on) { c.mode = 0; c.m = last - first + 1; }
    else if (o->pts[last].on) { c.mode = 1; c.m = last - first + 1; }
    else {
        c.mode = 2; c.m = last - first + 2;
        c.mx = (o->pts[first].x + o->pts[last].x) / 2;
        c.my = (o->pts[first].y + o->pts[last].y) / 2;
    }
    pt_t p0 = cont_get(&c, 0);
    int32_t sx0, sy0;
    to_screen(p0.x, p0.y, scale_q, slant, &sx0, &sy0);
    int32_t px = sx0, py = sy0;
    int i = 1;
    while (i < c.m) {
        pt_t p = cont_get(&c, i);
        if (p.on) {
            int32_t x, y;
            to_screen(p.x, p.y, scale_q, slant, &x, &y);
            if (!edge_add(f, e, bb, px, py, x, y)) return 0;
            px = x; py = y; i++;
        } else {
            int ni = i + 1;
            int32_t cx, cy;
            to_screen(p.x, p.y, scale_q, slant, &cx, &cy);
            if (ni < c.m && !cont_get(&c, ni).on) {
                pt_t nn = cont_get(&c, ni);
                int32_t ex, ey;
                to_screen((p.x + nn.x) / 2, (p.y + nn.y) / 2, scale_q, slant, &ex, &ey);
                if (!flatten_quad(f, e, bb, px, py, cx, cy, ex, ey)) return 0;
                px = ex; py = ey; i++;
            } else {
                int32_t ex, ey;
                if (ni < c.m) {
                    pt_t nn = cont_get(&c, ni);
                    to_screen(nn.x, nn.y, scale_q, slant, &ex, &ey);
                    i += 2;
                } else {
                    ex = sx0; ey = sy0;
                    i = c.m;
                }
                if (!flatten_quad(f, e, bb, px, py, cx, cy, ex, ey)) return 0;
                px = ex; py = ey;
            }
        }
    }
    if (px != sx0 || py != sy0)
        if (!edge_add(f, e, bb, px, py, sx0, sy0)) return 0;
    return 1;
}

/* ---------------- rasterising ---------------- */
typedef struct { int32_t x; int8_t dir; } cross_t;

static void sort_cross(cross_t *c, int n) {
    for (int i = 1; i < n; i++) {
        cross_t v = c[i];
        int j = i - 1;
        while (j >= 0 && c[j].x > v.x) { c[j + 1] = c[j]; j--; }
        c[j + 1] = v;
    }
}

static void add_span(int32_t *acc, int w, int32_t ox, int32_t xa, int32_t xb) {
    if (xb <= xa) return;
    int32_t c0 = (xa >> 6) - ox;
    int32_t c1 = ((xb + 63) >> 6) - ox;
    if (c0 < 0) c0 = 0;
    if (c1 > w) c1 = w;
    for (int32_t c = c0; c < c1; c++) {
        int32_t px = (ox + c) << 6;
        int32_t lo = xa > px ? xa : px;
        int32_t hi = xb < px + 64 ? xb : px + 64;
        if (hi > lo) acc[c] += hi - lo;
    }
}

static int rasterise(const ttf_face_t *f, const edges_t *e, const bbox_t *bb, ttf_glyph_t *out) {
    int32_t x0 = bb->minx >> 6, y0 = bb->miny >> 6;
    int32_t x1 = (bb->maxx + 63) >> 6, y1 = (bb->maxy + 63) >> 6;
    int w = (int)(x1 - x0), h = (int)(y1 - y0);
    if (w <= 0 || h <= 0) return 0;
    if (w > 4096 || h > 4096) return -1;
    uint8_t *cov = (uint8_t *)f->alloc(f->user, (size_t)w * (size_t)h);
    int32_t *acc = (int32_t *)f->alloc(f->user, (size_t)w * sizeof *acc);
    cross_t *cr = (cross_t *)f->alloc(f->user, (size_t)e->n * sizeof *cr);
    if (!cov || !acc || !cr) {
        if (cov) f->free(f->user, cov);
        if (acc) f->free(f->user, acc);
        if (cr) f->free(f->user, cr);
        return -1;
    }
    memset(cov, 0, (size_t)w * (size_t)h);
    for (int row = 0; row < h; row++) {
        memset(acc, 0, (size_t)w * sizeof *acc);
        for (int s = 0; s < SUBSAMPLES; s++) {
            int32_t y = (y0 + row) * 64 + (2 * s + 1) * (64 / (2 * SUBSAMPLES));
            int nc = 0;
            for (int i = 0; i < e->n; i++) {
                int32_t ya = e->e[i].y0, yb = e->e[i].y1;
                if (ya == yb) continue;
                int32_t lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
                if (y < lo || y >= hi) continue;
                int64_t num = (int64_t)(y - ya) * (e->e[i].x1 - e->e[i].x0);
                cr[nc].x = e->e[i].x0 + (int32_t)(num / (yb - ya));
                cr[nc].dir = yb > ya ? 1 : -1;
                nc++;
            }
            sort_cross(cr, nc);
            int wind = 0, open = 0;
            int32_t span = 0;
            for (int i = 0; i < nc; i++) {
                wind += cr[i].dir;
                if (!open && wind != 0) { span = cr[i].x; open = 1; }
                else if (open && wind == 0) { add_span(acc, w, x0, span, cr[i].x); open = 0; }
            }
        }
        uint8_t *dst = cov + (size_t)row * (size_t)w;
        for (int c = 0; c < w; c++) {
            int32_t v = (acc[c] * 255 + 256) / 512;
            dst[c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    f->free(f->user, acc);
    f->free(f->user, cr);
    out->width = w;
    out->height = h;
    out->xoff = (int)x0;
    out->yoff = (int)y0;
    out->coverage = cov;
    return 0;
}

/* ---------------- the entry point ---------------- */
int ttf_render(const ttf_face_t *f, int gid, int px, int slant, ttf_glyph_t *out) {
    if (!f || !out) return -1;
    memset(out, 0, sizeof *out);
    if (px < 1) px = 1;
    if (px > MAX_PX) px = MAX_PX;
    int adv = ttf_advance(f, gid);
    out->advance = (int)(((int64_t)adv * px + f->units_per_em / 2) / f->units_per_em);
    if (gid < 0 || (uint32_t)gid >= f->num_glyphs) return 0;

    int scale_q = (int)(((int64_t)px << 16) / f->units_per_em);
    outline_t o;
    memset(&o, 0, sizeof o);
    xform_t id = { F2DOT14_ONE, 0, 0, F2DOT14_ONE, 0, 0 };
    int r = emit_glyph(f, gid, &id, &o, 0);
    if (r <= 0) {
        if (o.pts) f->free(f->user, o.pts);
        if (o.ends) f->free(f->user, o.ends);
        return r < 0 ? -1 : 0;
    }

    edges_t e;
    memset(&e, 0, sizeof e);
    bbox_t bb;
    memset(&bb, 0, sizeof bb);
    int first = 0, ok = 1;
    for (int i = 0; i < o.ne && ok; i++) {
        int last = (int)o.ends[i] - 1;
        if (last < first || last >= o.n) { ok = 0; break; }
        ok = contour_edges(f, &o, first, last, scale_q, slant, &e, &bb);
        first = last + 1;
    }
    f->free(f->user, o.pts);
    f->free(f->user, o.ends);
    if (!ok) { if (e.e) f->free(f->user, e.e); return -1; }
    if (!bb.any || !e.n) { if (e.e) f->free(f->user, e.e); return 0; }

    r = rasterise(f, &e, &bb, out);
    f->free(f->user, e.e);
    return r;
}

void ttf_glyph_release(const ttf_face_t *f, ttf_glyph_t *g) {
    if (!f || !g) return;
    if (g->coverage) f->free(f->user, g->coverage);
    g->coverage = NULL;
    g->width = g->height = 0;
}
