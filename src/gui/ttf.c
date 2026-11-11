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
void *memmove(void *d, const void *s, size_t n);

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
    uint32_t gsub, gsub_len;          /* OpenType layout tables */
    uint32_t gpos, gpos_len;
    uint32_t gdef, gdef_len;

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
        case TAG4('G', 'S', 'U', 'B'): f->gsub = off; f->gsub_len = len; break;
        case TAG4('G', 'P', 'O', 'S'): f->gpos = off; f->gpos_len = len; break;
        case TAG4('G', 'D', 'E', 'F'): f->gdef = off; f->gdef_len = len; break;
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

/* ---------------- OpenType layout ----------------
 *
 * GSUB and GPOS share a shape: a script list chooses features, a feature names
 * lookups, and a lookup holds the actual subtables. Only what text needs is
 * here - single, multiple, alternate, ligature, contextual (formats 1-3) and
 * chaining substitutions, pair, mark-to-base, mark-to-ligature and mark-to-mark
 * positioning - and every read is bounds checked; a malformed font loses that
 * substitution, not the kernel. */

#define LF_IGNORE_BASE  0x0002
#define LF_IGNORE_LIG   0x0004
#define LF_IGNORE_MARK  0x0008
#define LF_MARK_SET     0x0010

#define MAX_SHAPE_LOOKUPS 128

typedef struct { ttf_shaped_t *g; int n, cap; uint8_t *form; int forms; } gbuf_t;

static int coverage_index(const ttf_face_t *f, uint32_t off, int gid) {
    if (!f || !off || gid < 0 || !in_file(f, off, 4)) return -1;
    const uint8_t *d = f->data;
    uint16_t fmt = rd16(d + off);
    if (fmt == 1) {
        uint16_t n = rd16(d + off + 2);
        if (!in_file(f, off + 4, (uint32_t)n * 2)) return -1;
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            uint16_t g = rd16(d + off + 4 + (uint32_t)mid * 2);
            if (g < gid) lo = mid + 1; else hi = mid;
        }
        if (lo < n && rd16(d + off + 4 + (uint32_t)lo * 2) == gid) return lo;
        return -1;
    }
    if (fmt == 2) {
        uint16_t n = rd16(d + off + 2);
        if (!in_file(f, off + 4, (uint32_t)n * 6)) return -1;
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t r = off + 4 + (uint32_t)mid * 6;
            uint16_t e = rd16(d + r + 2);
            if (e < gid) lo = mid + 1; else hi = mid;
        }
        if (lo < n) {
            uint32_t r = off + 4 + (uint32_t)lo * 6;
            uint16_t s = rd16(d + r), e = rd16(d + r + 2);
            if (gid >= s && gid <= e) return rd16(d + r + 4) + gid - s;
        }
    }
    return -1;
}

static int classdef_get(const ttf_face_t *f, uint32_t off, int gid) {
    if (!f || !off || gid < 0 || !in_file(f, off, 4)) return 0;
    const uint8_t *d = f->data;
    uint16_t fmt = rd16(d + off);
    if (fmt == 1) {
        uint16_t start = rd16(d + off + 2), n = rd16(d + off + 4);
        if (gid < start || (uint32_t)(gid - start) >= n) return 0;
        uint32_t p = off + 6 + (uint32_t)(gid - start) * 2;
        return in_file(f, p, 2) ? rd16(d + p) : 0;
    }
    if (fmt == 2) {
        uint16_t n = rd16(d + off + 2);
        if (!in_file(f, off + 4, (uint32_t)n * 6)) return 0;
        for (uint16_t i = 0; i < n; i++) {
            uint32_t r = off + 4 + (uint32_t)i * 6;
            uint16_t s = rd16(d + r), e = rd16(d + r + 2);
            if (gid >= s && gid <= e) return rd16(d + r + 4);
            if (gid < s) break;
        }
    }
    return 0;
}

/* GDEF says whether a glyph is a base, a ligature or a mark; lookup flags use
 * that to ignore the right kind when matching a sequence. */
static int glyph_class(const ttf_face_t *f, int gid) {
    if (!f || !f->gdef || f->gdef_len < 12) return 0;
    return classdef_get(f, f->gdef + rd16(f->data + f->gdef + 4), gid);
}
static int mark_set_has(const ttf_face_t *f, int set, int gid) {
    if (!f || !f->gdef || f->gdef_len < 18 || set < 0) return 1;
    uint32_t ms = f->gdef + rd16(f->data + f->gdef + 12);
    if (!ms || !in_file(f, ms, 4)) return 1;
    uint16_t n = rd16(f->data + ms + 2);
    if ((uint32_t)set >= n) return 1;
    uint32_t cov = rd32(f->data + ms + 4 + (uint32_t)set * 4);
    return coverage_index(f, ms + cov, gid) >= 0;
}
static int ignore_glyph(const ttf_face_t *f, uint16_t flag, int set, int gid) {
    int cls = glyph_class(f, gid);
    if ((flag & LF_IGNORE_BASE) && cls == 1) return 1;
    if ((flag & LF_IGNORE_LIG) && cls == 2) return 1;
    if ((flag & LF_IGNORE_MARK) && cls == 3) return 1;
    if ((flag & LF_MARK_SET) && cls == 3 && !mark_set_has(f, set, gid)) return 1;
    if (!(flag & LF_MARK_SET)) {
        int attach = (flag >> 8) & 0xFF;
        if (cls == 3 && attach) {
            uint32_t mc = f->gdef + rd16(f->data + f->gdef + 10);
            if (classdef_get(f, mc, gid) != attach) return 1;
        }
    }
    return 0;
}
static int next_glyph(const ttf_face_t *f, const gbuf_t *b, uint16_t flag, int set, int from) {
    for (int i = from + 1; i < b->n; i++)
        if (!ignore_glyph(f, flag, set, b->g[i].gid)) return i;
    return -1;
}
static int prev_glyph(const ttf_face_t *f, const gbuf_t *b, uint16_t flag, int set, int from) {
    for (int i = from - 1; i >= 0; i--)
        if (!ignore_glyph(f, flag, set, b->g[i].gid)) return i;
    return -1;
}

/* The script table for `tag`, or the default one, or 0. */
static uint32_t find_script(const ttf_face_t *f, uint32_t table, uint32_t tag) {
    const uint8_t *d = f->data;
    if (!table || !in_file(f, table, 10)) return 0;
    uint32_t sl = table + rd16(d + table + 4);
    if (!in_file(f, sl, 2)) return 0;
    uint16_t n = rd16(d + sl);
    uint32_t dflt = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint32_t r = sl + 2 + (uint32_t)i * 6;
        if (!in_file(f, r, 6)) break;
        uint32_t t = rd32(d + r);
        uint32_t s = sl + rd16(d + r + 4);
        if (t == tag) return s;
        if (t == TAG4('D', 'F', 'L', 'T')) dflt = s;
    }
    return dflt;
}

/* Append the lookup indices of `feat` for `script` to `out`. */
static int feature_lookups(const ttf_face_t *f, uint32_t table, uint32_t script,
                           uint32_t feat, uint16_t *out, int max) {
    const uint8_t *d = f->data;
    uint32_t s = find_script(f, table, script);
    if (!s || !in_file(f, s, 6)) return 0;
    uint32_t ls = rd16(d + s);
    if (!ls) return 0;                                    /* no default language */
    ls += s;
    if (!in_file(f, ls, 6)) return 0;
    uint16_t nf = rd16(d + ls + 4);
    uint32_t fl = table + rd16(d + table + 6);
    if (!in_file(f, fl, 2)) return 0;
    uint16_t nfeature = rd16(d + fl);
    int got = 0;
    for (uint16_t i = 0; i < nf; i++) {
        uint32_t fr = ls + 6 + (uint32_t)i * 2;
        if (!in_file(f, fr, 2)) break;
        uint16_t fi = rd16(d + fr);
        if (fi >= nfeature) continue;
        uint32_t rec = fl + 2 + (uint32_t)fi * 6;
        if (!in_file(f, rec, 6) || rd32(d + rec) != feat) continue;
        uint32_t ft = fl + rd16(d + rec + 4);
        if (!in_file(f, ft, 4)) continue;
        uint16_t nl = rd16(d + ft + 2);
        for (uint16_t k = 0; k < nl && got < max; k++) {
            uint32_t lr = ft + 4 + (uint32_t)k * 2;
            if (!in_file(f, lr, 2)) break;
            out[got++] = rd16(d + lr);
        }
    }
    return got;
}

/* Grow or shrink the buffer at `at`, keeping the parallel form array in step. */
static int gbuf_insert(const ttf_face_t *f, gbuf_t *b, int at, int count) {
    if (count <= 0) return 0;
    if (b->n + count > b->cap) return 0;
    memmove(&b->g[at + count], &b->g[at], (size_t)(b->n - at) * sizeof *b->g);
    if (b->forms) {
        memmove(&b->form[at + count], &b->form[at], (size_t)(b->n - at));
        memset(&b->form[at], 0, (size_t)count);
    }
    b->n += count;
    return 1;
}
static void gbuf_remove(gbuf_t *b, int at, int count) {
    if (count <= 0 || at < 0 || at + count > b->n) return;
    memmove(&b->g[at], &b->g[at + count], (size_t)(b->n - at - count) * sizeof *b->g);
    if (b->forms) memmove(&b->form[at], &b->form[at + count], (size_t)(b->n - at - count));
    b->n -= count;
}

/* A lookup applied at one position. Returns 1 when it matched; *delta is how
 * many glyphs appeared (positive) or disappeared (negative). */
static int gsub_at(const ttf_face_t *f, uint32_t lookup, gbuf_t *b, int pos, int depth, int *delta);

static int gsub_single(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos, int *delta) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 6)) return 0;
    uint16_t fmt = rd16(d + sub);
    int idx = coverage_index(f, sub + rd16(d + sub + 2), b->g[pos].gid);
    if (idx < 0) return 0;
    if (fmt == 1) {
        b->g[pos].gid = (uint16_t)(b->g[pos].gid + (uint16_t)rd16(d + sub + 4));
    } else if (fmt == 2) {
        uint16_t n = rd16(d + sub + 4);
        if ((uint32_t)idx >= n || !in_file(f, sub + 6, (uint32_t)n * 2)) return 0;
        b->g[pos].gid = rd16(d + sub + 6 + (uint32_t)idx * 2);
    } else return 0;
    b->g[pos].advance = ttf_advance(f, b->g[pos].gid);
    *delta = 0;
    return 1;
}

static int gsub_multiple(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos, int *delta) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 6) || rd16(d + sub) != 1) return 0;
    int idx = coverage_index(f, sub + rd16(d + sub + 2), b->g[pos].gid);
    if (idx < 0) return 0;
    uint16_t nseq = rd16(d + sub + 4);
    if ((uint32_t)idx >= nseq || !in_file(f, sub + 6, (uint32_t)nseq * 2)) return 0;
    uint32_t sp = sub + rd16(d + sub + 6 + (uint32_t)idx * 2);
    if (!in_file(f, sp, 2)) return 0;
    uint16_t count = rd16(d + sp);
    if (count == 0) { gbuf_remove(b, pos, 1); *delta = -1; return 1; }
    if (count == 1) {
        b->g[pos].gid = rd16(d + sp + 2);
        b->g[pos].advance = ttf_advance(f, b->g[pos].gid);
        *delta = 0;
        return 1;
    }
    if (!gbuf_insert(f, b, pos + 1, count - 1)) return 0;
    ttf_shaped_t first = b->g[pos];
    for (int i = 0; i < count; i++) {
        ttf_shaped_t g = first;
        g.gid = rd16(d + sp + 2 + (uint32_t)i * 2);
        g.advance = ttf_advance(f, g.gid);
        g.x_offset = g.y_offset = 0;
        b->g[pos + i] = g;
    }
    *delta = count - 1;
    return 1;
}

static int gsub_alternate(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos, int *delta) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 6) || rd16(d + sub) != 1) return 0;
    int idx = coverage_index(f, sub + rd16(d + sub + 2), b->g[pos].gid);
    if (idx < 0) return 0;
    uint16_t nseq = rd16(d + sub + 4);
    if ((uint32_t)idx >= nseq || !in_file(f, sub + 6, (uint32_t)nseq * 2)) return 0;
    uint32_t sp = sub + rd16(d + sub + 6 + (uint32_t)idx * 2);
    if (!in_file(f, sp, 4)) return 0;
    if (!rd16(d + sp)) return 0;
    b->g[pos].gid = rd16(d + sp + 2);
    b->g[pos].advance = ttf_advance(f, b->g[pos].gid);
    *delta = 0;
    return 1;
}

static int gsub_ligature(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos,
                         uint16_t flag, int set, int *delta) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 6) || rd16(d + sub) != 1) return 0;
    int idx = coverage_index(f, sub + rd16(d + sub + 2), b->g[pos].gid);
    if (idx < 0) return 0;
    uint16_t nsets = rd16(d + sub + 4);
    if ((uint32_t)idx >= nsets || !in_file(f, sub + 6, (uint32_t)nsets * 2)) return 0;
    uint32_t lp = sub + rd16(d + sub + 6 + (uint32_t)idx * 2);
    if (!in_file(f, lp, 2)) return 0;
    uint16_t nlig = rd16(d + lp);
    for (uint16_t i = 0; i < nlig; i++) {
        uint32_t lr = lp + rd16(d + lp + 2 + (uint32_t)i * 2);
        if (!in_file(f, lr, 4)) break;
        int lig = rd16(d + lr);
        uint16_t comps = rd16(d + lr + 2);
        if (comps < 2 || comps > 16) continue;
        int p[17]; p[0] = pos;
        int ok = 1;
        for (int k = 1; k < comps && ok; k++) {
            p[k] = next_glyph(f, b, flag, set, p[k - 1]);
            if (p[k] < 0 || rd16(d + lr + 4 + (uint32_t)(k - 1) * 2) != b->g[p[k]].gid) ok = 0;
        }
        if (!ok) continue;
        b->g[pos].gid = lig;
        b->g[pos].advance = ttf_advance(f, lig);
        for (int k = comps - 1; k >= 1; k--) gbuf_remove(b, p[k], 1);
        *delta = -(comps - 1);
        return 1;
    }
    return 0;
}

/* format 3 contextual / chaining: coverage tables for the input, and records
 * that run other lookups at a chosen position */
static int gsub_context3(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos,
                         uint16_t flag, int set, int chained, int depth, int *delta) {
    const uint8_t *d = f->data;
    uint32_t p;
    uint16_t records;
    int input[33];
    int nb = 0, ni = 0, na = 0;
    if (chained) {
        p = sub + 2;
        nb = rd16(d + p); p += 2;
        if (nb > 32 || (nb && !in_file(f, p, (uint32_t)nb * 2))) return 0;
        int at = pos;
        for (int i = 0; i < nb; i++) {
            at = prev_glyph(f, b, flag, set, at);
            if (at < 0 || coverage_index(f, sub + rd16(d + p + (uint32_t)i * 2), b->g[at].gid) < 0) return 0;
        }
        p += (uint32_t)nb * 2;
        ni = rd16(d + p); p += 2;
        if (ni < 1 || ni > 32 || !in_file(f, p, (uint32_t)ni * 2)) return 0;
        input[0] = pos;
        for (int i = 0; i < ni; i++) {
            if (i) input[i] = next_glyph(f, b, flag, set, input[i - 1]);
            if (input[i] < 0 || coverage_index(f, sub + rd16(d + p + (uint32_t)i * 2), b->g[input[i]].gid) < 0) return 0;
        }
        p += (uint32_t)ni * 2;
        na = rd16(d + p); p += 2;
        if (na > 32 || (na && !in_file(f, p, (uint32_t)na * 2))) return 0;
        int at2 = input[ni - 1];
        for (int i = 0; i < na; i++) {
            at2 = next_glyph(f, b, flag, set, at2);
            if (at2 < 0 || coverage_index(f, sub + rd16(d + p + (uint32_t)i * 2), b->g[at2].gid) < 0) return 0;
        }
        p += (uint32_t)na * 2;
        records = rd16(d + p); p += 2;
    } else {
        uint16_t glyphs = rd16(d + sub + 2);
        records = rd16(d + sub + 4);
        if (glyphs < 1 || glyphs > 32) return 0;
        p = sub + 6;
        ni = glyphs;
        input[0] = pos;
        for (int i = 0; i < ni; i++) {
            if (i) input[i] = next_glyph(f, b, flag, set, input[i - 1]);
            if (input[i] < 0 || coverage_index(f, sub + rd16(d + p + (uint32_t)i * 2), b->g[input[i]].gid) < 0) return 0;
        }
        p += (uint32_t)ni * 2;
    }
    if (records > 32) return 0;
    if (records > 0 && !in_file(f, p, (uint32_t)records * 4)) return 0;
    int changed = 0;
    for (uint16_t r = 0; r < records; r++) {
        uint16_t seq = rd16(d + p + (uint32_t)r * 4);
        uint16_t li = rd16(d + p + (uint32_t)r * 4 + 2);
        if (seq >= (uint16_t)ni) continue;
        int at = input[seq];
        if (at < 0 || at >= b->n) continue;
        int d2 = 0;
        uint32_t ll = f->gsub + rd16(f->data + f->gsub + 8);
        if (gsub_at(f, ll + rd16(f->data + ll + 2 + (uint32_t)li * 2), b, at, depth + 1, &d2) && d2) {
            for (int k = 0; k < ni; k++) if (input[k] > at) input[k] += d2;
            changed += d2;
        }
    }
    *delta = changed;
    return 1;
}

static int gsub_at(const ttf_face_t *f, uint32_t lookup, gbuf_t *b, int pos, int depth, int *delta) {
    const uint8_t *d = f->data;
    if (depth > 8 || pos < 0 || pos >= b->n || !f->gsub) return 0;
    if (!in_file(f, lookup, 6)) return 0;
    int type = rd16(d + lookup), nsub = rd16(d + lookup + 4);
    uint16_t flag = rd16(d + lookup + 2);
    int set = 0;
    if (flag & LF_MARK_SET) {
        if (!in_file(f, lookup + 6 + (uint32_t)nsub * 2, 2)) return 0;
        set = rd16(d + lookup + 6 + (uint32_t)nsub * 2);
    }
    if (ignore_glyph(f, flag, set, b->g[pos].gid)) return 0;
    for (int s = 0; s < nsub; s++) {
        uint32_t sub = lookup + rd16(d + lookup + 6 + (uint32_t)s * 2);
        int t = type;
        if (type == 7) {                                 /* extension */
            if (!in_file(f, sub, 8) || rd16(d + sub) != 1) continue;
            t = rd16(d + sub + 2);
            sub = sub + rd32(d + sub + 4);
        }
        int ok = 0;
        switch (t) {
        case 1: ok = gsub_single(f, sub, b, pos, delta); break;
        case 2: ok = gsub_multiple(f, sub, b, pos, delta); break;
        case 3: ok = gsub_alternate(f, sub, b, pos, delta); break;
        case 4: ok = gsub_ligature(f, sub, b, pos, flag, set, delta); break;
        case 5: ok = gsub_context3(f, sub, b, pos, flag, set, 0, depth, delta); break;
        case 6: ok = gsub_context3(f, sub, b, pos, flag, set, 1, depth, delta); break;
        default: break;
        }
        if (ok) return 1;
    }
    return 0;
}

static void gsub_lookup(const ttf_face_t *f, uint32_t lookup, gbuf_t *b, int depth) {
    for (int i = 0; i < b->n; ) {
        int delta = 0;
        if (gsub_at(f, lookup, b, i, depth, &delta)) {
            if (delta < 0 && i + delta < 0) i = 0;
            else i += 1 + (delta > 0 ? delta : 0);
        } else i++;
    }
}

static void gsub_feature(const ttf_face_t *f, uint32_t script, uint32_t feat, gbuf_t *b, int depth) {
    uint16_t lookups[MAX_SHAPE_LOOKUPS];
    int n = feature_lookups(f, f->gsub, script, feat, lookups, MAX_SHAPE_LOOKUPS);
    for (int i = 0; i < n; i++) {
        uint32_t ll = f->gsub + rd16(f->data + f->gsub + 8);
        gsub_lookup(f, ll + rd16(f->data + ll + 2 + (uint32_t)lookups[i] * 2), b, depth);
    }
}

/* the same feature, but only at the glyphs that ask for it (Arabic forms) */
static void gsub_form_feature(const ttf_face_t *f, uint32_t script, uint32_t feat,
                              int form, gbuf_t *b) {
    uint16_t lookups[MAX_SHAPE_LOOKUPS];
    int nl = feature_lookups(f, f->gsub, script, feat, lookups, MAX_SHAPE_LOOKUPS);
    if (!nl || !b->forms) return;
    for (int li = 0; li < nl; li++) {
        uint32_t ll = f->gsub + rd16(f->data + f->gsub + 8);
        uint32_t lookup = ll + rd16(f->data + ll + 2 + (uint32_t)lookups[li] * 2);
        for (int i = 0; i < b->n; ) {
            if (b->form[i] != form) { i++; continue; }
            int delta = 0;
            if (gsub_at(f, lookup, b, i, 0, &delta)) i += 1 + (delta > 0 ? delta : 0);
            else i++;
        }
    }
}

/* ---------------- GPOS ---------------- */
typedef struct { int xpl, ypl, xadv, yadv; } value_t;

static int value_size(uint16_t fmt) {
    int n = 0;
    for (int i = 0; i < 8; i++) if (fmt & (1u << i)) n += 2;
    return n;
}
static int value_read(const ttf_face_t *f, uint32_t p, uint16_t fmt, value_t *v) {
    const uint8_t *d = f->data;
    int n = value_size(fmt);
    if (!in_file(f, p, (uint32_t)n)) return 0;
    v->xpl = v->ypl = v->xadv = v->yadv = 0;
    if (fmt & 0x0001) { v->xpl = rs16(d + p); p += 2; }
    if (fmt & 0x0002) { v->ypl = rs16(d + p); p += 2; }
    if (fmt & 0x0004) { v->xadv = rs16(d + p); p += 2; }
    if (fmt & 0x0008) { v->yadv = rs16(d + p); p += 2; }
    return 1;
}
static void value_apply(ttf_shaped_t *g, const value_t *v) {
    g->x_offset += v->xpl;
    g->y_offset += v->ypl;
    g->advance += v->xadv;
}
static int anchor_read(const ttf_face_t *f, uint32_t p, int *x, int *y) {
    if (!in_file(f, p, 6)) return 0;
    *x = rs16(f->data + p + 2);
    *y = rs16(f->data + p + 4);
    return 1;
}

static int gpos_pair(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos,
                     uint16_t flag, int set, int *changed) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 10)) return 0;
    uint16_t fmt = rd16(d + sub);
    int idx = coverage_index(f, sub + rd16(d + sub + 2), b->g[pos].gid);
    if (idx < 0) return 0;
    int j = next_glyph(f, b, flag, set, pos);
    if (j < 0) return 0;
    uint16_t vf1 = rd16(d + sub + 4), vf2 = rd16(d + sub + 6);
    value_t v1, v2;
    if (fmt == 1) {
        uint16_t nsets = rd16(d + sub + 8);
        if ((uint32_t)idx >= nsets || !in_file(f, sub + 10, (uint32_t)nsets * 2)) return 0;
        uint32_t ps = sub + rd16(d + sub + 10 + (uint32_t)idx * 2);
        if (!in_file(f, ps, 2)) return 0;
        uint16_t np = rd16(d + ps);
        uint32_t rec = ps + 2;
        for (uint16_t i = 0; i < np; i++) {
            uint32_t r = rec + (uint32_t)i * (4 + value_size(vf1) + value_size(vf2));
            if (!in_file(f, r, 2)) break;
            if (rd16(d + r) != b->g[j].gid) continue;
            uint32_t p = r + 2;
            if (!value_read(f, p, vf1, &v1)) return 0;
            p += (uint32_t)value_size(vf1);
            if (!value_read(f, p, vf2, &v2)) return 0;
            value_apply(&b->g[pos], &v1);
            value_apply(&b->g[j], &v2);
            *changed = 1;
            return 1;
        }
        return 0;
    }
    if (fmt == 2) {
        int c1 = classdef_get(f, sub + rd16(d + sub + 8), b->g[pos].gid);
        int c2 = classdef_get(f, sub + rd16(d + sub + 10), b->g[j].gid);
        uint16_t n1 = rd16(d + sub + 12), n2 = rd16(d + sub + 14);
        if (c1 >= n1 || c2 >= n2) return 0;
        uint32_t rec = sub + 16 + ((uint32_t)c1 * n2 + (uint32_t)c2) * (value_size(vf1) + value_size(vf2));
        uint32_t p = rec;
        if (!value_read(f, p, vf1, &v1)) return 0;
        p += (uint32_t)value_size(vf1);
        if (!value_read(f, p, vf2, &v2)) return 0;
        value_apply(&b->g[pos], &v1);
        value_apply(&b->g[j], &v2);
        *changed = 1;
        return 1;
    }
    return 0;
}

/* MarkArray / BaseArray / LigatureAttach / Mark2Array share the same shape:
 * a count followed by classCount anchor offsets per entry. */
static int anchor_pair(const ttf_face_t *f, uint32_t array, int entry, int cls,
                       int class_count, int *ax, int *ay) {
    uint32_t p = array + 2 + (uint32_t)entry * (uint32_t)class_count * 2 + (uint32_t)cls * 2;
    if (!in_file(f, p, 2)) return 0;
    uint16_t off = rd16(f->data + p);
    if (!off) return 0;
    return anchor_read(f, array + off, ax, ay);
}

static int mark_anchor(const ttf_face_t *f, uint32_t mark_array, int entry, int *cls, int *x, int *y) {
    uint32_t p = mark_array + 2 + (uint32_t)entry * 4;
    if (!in_file(f, p, 4)) return 0;
    *cls = rd16(f->data + p);
    uint16_t off = rd16(f->data + p + 2);
    if (!off) return 0;
    return anchor_read(f, mark_array + off, x, y);
}

static int gpos_mark_base(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 12) || rd16(d + sub) != 1) return 0;
    uint32_t mc = sub + rd16(d + sub + 2), bc = sub + rd16(d + sub + 4);
    int class_count = rd16(d + sub + 6);
    uint32_t ma = sub + rd16(d + sub + 8), ba = sub + rd16(d + sub + 10);
    int mi = coverage_index(f, mc, b->g[pos].gid);
    if (mi < 0 || class_count <= 0) return 0;
    int base = -1;
    for (int i = pos - 1, k = 0; i >= 0 && k < 64; i--, k++) {
        if (coverage_index(f, bc, b->g[i].gid) >= 0) { base = i; break; }
        if (glyph_class(f, b->g[i].gid) != 3) break;
    }
    if (base < 0) return 0;
    int bi = coverage_index(f, bc, b->g[base].gid);
    if (bi < 0) return 0;
    int cls, mx, my, ax, ay;
    if (!mark_anchor(f, ma, mi, &cls, &mx, &my)) return 0;
    if (cls >= class_count) return 0;
    if (!anchor_pair(f, ba, bi, cls, class_count, &ax, &ay)) return 0;
    b->g[pos].x_offset += ax - mx;
    b->g[pos].y_offset += ay - my;
    return 1;
}

static int gpos_mark_lig(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 12) || rd16(d + sub) != 1) return 0;
    uint32_t mc = sub + rd16(d + sub + 2), lc = sub + rd16(d + sub + 4);
    int class_count = rd16(d + sub + 6);
    uint32_t ma = sub + rd16(d + sub + 8), la = sub + rd16(d + sub + 10);
    int mi = coverage_index(f, mc, b->g[pos].gid);
    if (mi < 0 || class_count <= 0) return 0;
    int lig = -1;
    for (int i = pos - 1, k = 0; i >= 0 && k < 64; i--, k++) {
        if (coverage_index(f, lc, b->g[i].gid) >= 0) { lig = i; break; }
        if (glyph_class(f, b->g[i].gid) != 3) break;
    }
    if (lig < 0) return 0;
    int li = coverage_index(f, lc, b->g[lig].gid);
    if (li < 0 || !in_file(f, la, 2)) return 0;
    uint16_t nlig = rd16(d + la);
    if ((uint32_t)li >= nlig) return 0;
    uint32_t at = la + rd16(d + la + 2 + (uint32_t)li * 2);
    if (!in_file(f, at, 2)) return 0;
    int comps = rd16(d + at);
    if (comps <= 0) return 0;
    int cls, mx, my, ax, ay;
    if (!mark_anchor(f, ma, mi, &cls, &mx, &my) || cls >= class_count) return 0;
    int comp = comps - 1;                                  /* the mark follows the ligature */
    uint32_t p = at + 2 + (uint32_t)comp * (uint32_t)class_count * 2 + (uint32_t)cls * 2;
    if (!in_file(f, p, 2) || !rd16(d + p)) return 0;
    if (!anchor_read(f, at + rd16(d + p), &ax, &ay)) return 0;
    b->g[pos].x_offset += ax - mx;
    b->g[pos].y_offset += ay - my;
    return 1;
}

static int gpos_mark_mark(const ttf_face_t *f, uint32_t sub, gbuf_t *b, int pos) {
    const uint8_t *d = f->data;
    if (!in_file(f, sub, 12) || rd16(d + sub) != 1) return 0;
    uint32_t c1 = sub + rd16(d + sub + 2), c2 = sub + rd16(d + sub + 4);
    int class_count = rd16(d + sub + 6);
    uint32_t a1 = sub + rd16(d + sub + 8), a2 = sub + rd16(d + sub + 10);
    int m1 = coverage_index(f, c1, b->g[pos].gid);
    if (m1 < 0 || class_count <= 0) return 0;
    int base = -1;
    for (int i = pos - 1, k = 0; i >= 0 && k < 64; i--, k++) {
        if (coverage_index(f, c2, b->g[i].gid) >= 0) { base = i; break; }
        if (glyph_class(f, b->g[i].gid) != 3) break;      /* a base ends the mark stack */
    }
    if (base < 0) return 0;
    int m2 = coverage_index(f, c2, b->g[base].gid);
    if (m2 < 0) return 0;
    int cls, mx, my, ax, ay;
    if (!mark_anchor(f, a1, m1, &cls, &mx, &my) || cls >= class_count) return 0;
    if (!anchor_pair(f, a2, m2, cls, class_count, &ax, &ay)) return 0;
    b->g[pos].x_offset += ax - mx;
    b->g[pos].y_offset += ay - my;
    return 1;
}

static int gpos_at(const ttf_face_t *f, uint32_t lookup, gbuf_t *b, int pos, uint16_t flag, int set) {
    const uint8_t *d = f->data;
    int type = rd16(d + lookup), nsub = rd16(d + lookup + 4);
    for (int s = 0; s < nsub; s++) {
        uint32_t sub = lookup + rd16(d + lookup + 6 + (uint32_t)s * 2);
        int t = type;
        if (type == 9) {
            if (!in_file(f, sub, 8) || rd16(d + sub) != 1) continue;
            t = rd16(d + sub + 2);
            sub = sub + rd32(d + sub + 4);
        }
        int changed = 0, ok = 0;
        switch (t) {
        case 1: {                                        /* single adjustment */
            if (rd16(d + sub) != 1) break;
            int idx = coverage_index(f, sub + rd16(d + sub + 2), b->g[pos].gid);
            if (idx < 0) break;
            uint16_t vf = rd16(d + sub + 4);
            uint32_t p = sub + 6 + (uint32_t)idx * (uint32_t)value_size(vf);
            value_t v;
            if (value_read(f, p, vf, &v)) { value_apply(&b->g[pos], &v); ok = changed = 1; }
            break;
        }
        case 2: ok = gpos_pair(f, sub, b, pos, flag, set, &changed); break;
        case 4: ok = gpos_mark_base(f, sub, b, pos) ? 1 : 0; changed = ok; break;
        case 5: ok = gpos_mark_lig(f, sub, b, pos) ? 1 : 0; changed = ok; break;
        case 6: ok = gpos_mark_mark(f, sub, b, pos) ? 1 : 0; changed = ok; break;
        default: break;
        }
        if (ok) return changed;
    }
    return 0;
}

static void gpos_feature(const ttf_face_t *f, uint32_t script, uint32_t feat, gbuf_t *b) {
    uint16_t lookups[MAX_SHAPE_LOOKUPS];
    int n = feature_lookups(f, f->gpos, script, feat, lookups, MAX_SHAPE_LOOKUPS);
    for (int k = 0; k < n; k++) {
        uint32_t ll = f->gpos + rd16(f->data + f->gpos + 8);
        uint32_t lookup = ll + rd16(f->data + ll + 2 + (uint32_t)lookups[k] * 2);
        if (!in_file(f, lookup, 6)) continue;
        uint16_t flag = rd16(f->data + lookup + 2);
        int set = 0;
        if (flag & LF_MARK_SET) {
            uint16_t ns = rd16(f->data + lookup + 4);
            if (in_file(f, lookup + 6 + (uint32_t)ns * 2, 2))
                set = rd16(f->data + lookup + 6 + (uint32_t)ns * 2);
        }
        for (int i = 0; i < b->n; i++)
            gpos_at(f, lookup, b, i, flag, set);
    }
}

/* ---------------- Arabic joining ---------------- */
enum { JOIN_U, JOIN_R, JOIN_D, JOIN_L, JOIN_C, JOIN_T };
enum { FORM_NONE, FORM_ISOL, FORM_INIT, FORM_MEDI, FORM_FINA };

static int in_range(uint32_t c, uint32_t lo, uint32_t hi) { return c >= lo && c <= hi; }

static int join_type(uint32_t c) {
    /* transparent: combining marks never break a join */
    if (in_range(c, 0x0300, 0x036F) || in_range(c, 0x0483, 0x0489) ||
        in_range(c, 0x0591, 0x05BD) || c == 0x05BF || in_range(c, 0x05C1, 0x05C2) ||
        in_range(c, 0x05C4, 0x05C5) || c == 0x05C7 ||
        in_range(c, 0x0610, 0x061A) || in_range(c, 0x064B, 0x065F) || c == 0x0670 ||
        in_range(c, 0x06D6, 0x06DC) || in_range(c, 0x06DF, 0x06E4) ||
        in_range(c, 0x06E7, 0x06E8) || in_range(c, 0x06EA, 0x06ED) ||
        c == 0x0711 || in_range(c, 0x0730, 0x074A) || in_range(c, 0x07A6, 0x07B0) ||
        in_range(c, 0x07EB, 0x07F3) || in_range(c, 0x0816, 0x0819) ||
        in_range(c, 0x081B, 0x0823) || in_range(c, 0x0825, 0x0827) ||
        in_range(c, 0x0829, 0x082D) || in_range(c, 0x0859, 0x085B) ||
        in_range(c, 0x0898, 0x089F) || in_range(c, 0x08CA, 0x08E1) ||
        in_range(c, 0x08E3, 0x08FF) || in_range(c, 0x1AB0, 0x1AFF) ||
        in_range(c, 0x1DC0, 0x1DFF) || in_range(c, 0x20D0, 0x20FF) ||
        in_range(c, 0xFE00, 0xFE0F) || in_range(c, 0xFE20, 0xFE2F))
        return JOIN_T;
    if (c == 0x0640 || c == 0x200D) return JOIN_C;          /* tatweel, ZWJ */
    if (c == 0x200C || c == 0x200B) return JOIN_U;          /* ZWNJ, ZWSP */
    if (c == 0x0710 || c == 0x07FA) return JOIN_R;          /* Syriac alaph, Mandaic */
    if (in_range(c, 0x0620, 0x064A) || in_range(c, 0x066E, 0x06D3) ||
        in_range(c, 0x06D5, 0x06D5) || in_range(c, 0x06EE, 0x06EF) ||
        in_range(c, 0x06FA, 0x06FC) || c == 0x06FF ||
        in_range(c, 0x0750, 0x077F) || in_range(c, 0x08A0, 0x08C8)) {
        /* right-joining: alef, dal, thal, reh, zain, waw and friends */
        if (c == 0x0622 || c == 0x0623 || c == 0x0624 || c == 0x0625 ||
            c == 0x0627 || c == 0x0629 || in_range(c, 0x062F, 0x0632) ||
            c == 0x0648 || c == 0x0671 || c == 0x0672 || c == 0x0673 ||
            in_range(c, 0x0675, 0x0677) || in_range(c, 0x0688, 0x0699) ||
            c == 0x06C0 || in_range(c, 0x06C3, 0x06CB) || c == 0x06CD ||
            c == 0x06CF || in_range(c, 0x06D2, 0x06D3) || c == 0x06D5 ||
            c == 0x06EE || c == 0x06EF || c == 0x06FE || c == 0x08AA ||
            c == 0x08AE || c == 0x08B1 || c == 0x08B2 || c == 0x08B9)
            return JOIN_R;
        return JOIN_D;
    }
    if (in_range(c, 0x0700, 0x070D) || in_range(c, 0x0800, 0x0815) ||
        in_range(c, 0x0840, 0x0858) || in_range(c, 0x0860, 0x086A) ||
        in_range(c, 0x0870, 0x0887) || in_range(c, 0x0889, 0x088E))
        return JOIN_D;
    if (in_range(c, 0x1807, 0x1807) || in_range(c, 0x1820, 0x1878) ||
        in_range(c, 0xA840, 0xA871)) return JOIN_D;        /* Mongolian, Phags-pa */
    return JOIN_U;
}

static void arabic_forms(const uint32_t *cps, int n, uint8_t *form) {
    for (int i = 0; i < n; i++) {
        int t = join_type(cps[i]);
        form[i] = FORM_NONE;
        if (t != JOIN_R && t != JOIN_D && t != JOIN_L && t != JOIN_C) continue;
        int p = i - 1;
        while (p >= 0 && join_type(cps[p]) == JOIN_T) p--;
        int q = i + 1;
        while (q < n && join_type(cps[q]) == JOIN_T) q++;
        int pt = p >= 0 ? join_type(cps[p]) : JOIN_U;
        int qt = q < n ? join_type(cps[q]) : JOIN_U;
        int prev_joins = (pt == JOIN_D || pt == JOIN_L || pt == JOIN_C) &&
                         (t == JOIN_D || t == JOIN_R || t == JOIN_C);
        int next_joins = (t == JOIN_D || t == JOIN_L || t == JOIN_C) &&
                         (qt == JOIN_D || qt == JOIN_R || qt == JOIN_C);
        form[i] = prev_joins && next_joins ? FORM_MEDI :
                  prev_joins ? FORM_FINA : next_joins ? FORM_INIT : FORM_ISOL;
    }
}

ttf_shaped_t *ttf_shape(const ttf_face_t *f, const uint32_t *cps, int n,
                        uint32_t script, int rtl, int *out_n) {
    if (out_n) *out_n = 0;
    if (!f || !cps || n <= 0 || n > 65536) return NULL;
    int cap = n * 3 + 16;
    ttf_shaped_t *g = f->alloc(f->user, (size_t)cap * sizeof *g);
    if (!g) return NULL;
    uint8_t *forms = f->alloc(f->user, (size_t)cap);
    if (!forms) { f->free(f->user, g); return NULL; }
    uint8_t *orig = f->alloc(f->user, (size_t)n);
    if (!orig) { f->free(f->user, forms); f->free(f->user, g); return NULL; }
    arabic_forms(cps, n, orig);
    gbuf_t b = { g, 0, cap, forms, 1 };
    for (int i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 || cp == 0x00AD)
            continue;                                     /* invisible format characters */
        int gid = ttf_glyph_index(f, cp);
        b.g[b.n].gid = gid;
        b.g[b.n].advance = ttf_advance(f, gid);
        b.g[b.n].x_offset = b.g[b.n].y_offset = 0;
        b.g[b.n].cluster = i;
        b.form[b.n] = orig[i];
        b.n++;
    }
    f->free(f->user, orig);
    if (!b.n) { f->free(f->user, forms); f->free(f->user, g); return NULL; }
    b.cap = cap;

    if (f->gsub) {
        gsub_feature(f, script, TAG4('c', 'c', 'm', 'p'), &b, 0);
        gsub_feature(f, script, TAG4('l', 'o', 'c', 'l'), &b, 0);
        if (script == TAG4('a', 'r', 'a', 'b')) {
            gsub_form_feature(f, script, TAG4('i', 's', 'o', 'l'), FORM_ISOL, &b);
            gsub_form_feature(f, script, TAG4('i', 'n', 'i', 't'), FORM_INIT, &b);
            gsub_form_feature(f, script, TAG4('m', 'e', 'd', 'i'), FORM_MEDI, &b);
            gsub_form_feature(f, script, TAG4('f', 'i', 'n', 'a'), FORM_FINA, &b);
        }
        gsub_feature(f, script, TAG4('r', 'l', 'i', 'g'), &b, 0);
        gsub_feature(f, script, TAG4('c', 'a', 'l', 't'), &b, 0);
        gsub_feature(f, script, TAG4('l', 'i', 'g', 'a'), &b, 0);
        gsub_feature(f, script, TAG4('c', 'l', 'i', 'g'), &b, 0);
    }
    if (f->gpos) {
        gpos_feature(f, script, TAG4('k', 'e', 'r', 'n'), &b);
        gpos_feature(f, script, TAG4('m', 'a', 'r', 'k'), &b);
        gpos_feature(f, script, TAG4('m', 'k', 'm', 'k'), &b);
    }
    f->free(f->user, forms);
    if (out_n) *out_n = b.n;
    return g;
}

void ttf_shape_free(const ttf_face_t *f, ttf_shaped_t *shaped) {
    if (f && shaped) f->free(f->user, shaped);
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
