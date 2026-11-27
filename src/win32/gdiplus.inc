/* gdiplus.dll (included by user32.c, so it can reach GDI bitmaps): the flat GDI+ API for
 * images, the part programs use to load pictures, scale and turn them, draw them on one
 * another, swap them with GDI bitmaps and save them (BMP, PNG). Images are decoded by the
 * OS's own decoders (PNG, JPEG, GIF, BMP). Also ole32's CreateStreamOnHGlobal, the
 * memory stream programs hand GDI+ their pictures in. Drawing shapes and text through GDI+
 * (pens, brushes, paths) is not here. */
#include "apps.h"

typedef uint32_t ULONG;
typedef int GpStatus;
enum { GpOk = 0, GpGenericError = 1, GpInvalidParameter = 2, GpOutOfMemory = 3, GpNotImplemented = 6, GpWin32Error = 7, GpUnknownImageFormat = 13 };
typedef int32_t HRESULT_T;
#define S_OK_ 0
#define E_POINTER_ ((HRESULT_T)0x80004003)
#define E_NOINTERFACE_ ((HRESULT_T)0x80004002)
#define E_NOTIMPL_ ((HRESULT_T)0x80004001)
#define STG_E_INVALIDFUNCTION_ ((HRESULT_T)0x80030001)

/* ---------------- memory streams (IStream on an HGLOBAL) ---------------- */
typedef struct { void **vt; int refs; uint8_t *data; uint32_t size, cap, pos; bool own; bool free_on_release; } mstream_t;
static void *mstream_vt[14];
static HRESULT_T WINAPI ms_qi(mstream_t *s, const void *iid, void **out) { (void)iid; if (!out) return E_POINTER_; *out = s; s->refs++; return S_OK_; }
static ULONG WINAPI ms_addref(mstream_t *s) { return ++s->refs; }
static ULONG WINAPI ms_release(mstream_t *s) {
    if (--s->refs > 0) return s->refs;
    if (s->own) kfree(s->data);
    kfree(s);
    return 0;
}
static HRESULT_T WINAPI ms_read(mstream_t *s, void *pv, ULONG cb, ULONG *got) {
    uint32_t n = s->pos < s->size ? MIN(cb, s->size - s->pos) : 0;
    if (n && pv) memcpy(pv, s->data + s->pos, n);
    s->pos += n;
    if (got) *got = n;
    return n < cb ? 1 : S_OK_;                                     /* S_FALSE at the end */
}
static bool ms_grow(mstream_t *s, uint32_t need) {
    if (need <= s->cap) return true;
    uint32_t cap = MAX(need, s->cap * 2 + 256);
    uint8_t *d = kmalloc(cap);
    if (!d) return false;
    if (s->data) memcpy(d, s->data, s->size);
    if (s->own) kfree(s->data);
    s->data = d; s->cap = cap; s->own = true;
    return true;
}
static HRESULT_T WINAPI ms_write(mstream_t *s, const void *pv, ULONG cb, ULONG *put) {
    if (!ms_grow(s, s->pos + cb)) return (HRESULT_T)0x8007000E;
    if (pv) memcpy(s->data + s->pos, pv, cb);
    s->pos += cb; if (s->pos > s->size) s->size = s->pos;
    if (put) *put = cb;
    return S_OK_;
}
static HRESULT_T WINAPI ms_seek(mstream_t *s, int64_t move, DWORD origin, uint64_t *newpos) {
    int64_t base = origin == 0 ? 0 : origin == 1 ? (int64_t)s->pos : (int64_t)s->size;
    int64_t p = base + move;
    if (p < 0 || origin > 2) return STG_E_INVALIDFUNCTION_;
    s->pos = (uint32_t)p;
    if (newpos) *newpos = (uint64_t)p;
    return S_OK_;
}
static HRESULT_T WINAPI ms_setsize(mstream_t *s, uint64_t n) { if (!ms_grow(s, (uint32_t)n)) return (HRESULT_T)0x8007000E; s->size = (uint32_t)n; return S_OK_; }
static HRESULT_T WINAPI ms_copyto(mstream_t *s, void *dst, uint64_t cb, uint64_t *rd, uint64_t *wr) { (void)s; (void)dst; (void)cb; (void)rd; (void)wr; return E_NOTIMPL_; }
static HRESULT_T WINAPI ms_commit(mstream_t *s, DWORD f) { (void)s; (void)f; return S_OK_; }
static HRESULT_T WINAPI ms_revert(mstream_t *s) { (void)s; return S_OK_; }
static HRESULT_T WINAPI ms_lock(mstream_t *s, uint64_t o, uint64_t n, DWORD t) { (void)s; (void)o; (void)n; (void)t; return STG_E_INVALIDFUNCTION_; }
/* STATSTG: name, type, then the size (at 8 on 32-bit, 16 on 64-bit), times, mode, locks, clsid... */
typedef struct { void *name; DWORD type; uint64_t size; uint8_t rest[48]; } statstg_t;
static HRESULT_T WINAPI ms_stat(mstream_t *s, statstg_t *st, DWORD flag) {
    (void)flag; if (!st) return E_POINTER_;
    memset(st, 0, sizeof *st); st->type = 2; st->size = s->size;      /* STGTY_STREAM */
    return S_OK_;
}
static HRESULT_T WINAPI ms_clone(mstream_t *s, void **out) { (void)s; if (out) *out = NULL; return E_NOTIMPL_; }
static mstream_t *mstream_new(uint8_t *data, uint32_t size, bool own) {
    if (!mstream_vt[0]) {
        void *f[14] = { ms_qi, ms_addref, ms_release, ms_read, ms_write, ms_seek, ms_setsize, ms_copyto, ms_commit, ms_revert, ms_lock, ms_lock, ms_stat, ms_clone };
        memcpy(mstream_vt, f, sizeof f);
    }
    mstream_t *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->vt = mstream_vt; s->refs = 1; s->data = data; s->size = size; s->cap = own ? size : 0; s->own = own;
    return s;
}
/* the stream reads the HGLOBAL in place (here an HGLOBAL is its own pointer), or starts empty */
static HRESULT_T WINAPI CreateStreamOnHGlobal(void *h, BOOL del, void **out) {
    if (!out) return E_POINTER_;
    mstream_t *s = h ? mstream_new(h, w32_msize(h), false) : mstream_new(NULL, 0, true);
    if (!s) return (HRESULT_T)0x8007000E;
    (void)del;
    *out = s;
    return S_OK_;
}

/* ---------------- images ---------------- */
#define GPIMG_MAGIC 0x47504931
#define GPGFX_MAGIC 0x47504731
typedef struct { uint32_t magic; canvas_t *c; } gpimage_t;              /* 0xAARRGGBB pixels */
typedef struct { uint32_t magic; canvas_t *c; int interp; } gpgfx_t;
static gpimage_t *IMG(void *p) { gpimage_t *i = p; return i && i->magic == GPIMG_MAGIC && i->c ? i : NULL; }
static gpgfx_t *GFX(void *p) { gpgfx_t *g = p; return g && g->magic == GPGFX_MAGIC && g->c ? g : NULL; }
static gpimage_t *img_new(canvas_t *c) {
    if (!c) return NULL;
    gpimage_t *i = kzalloc(sizeof *i);
    if (!i) { canvas_free(c); return NULL; }
    i->magic = GPIMG_MAGIC; i->c = c;
    return i;
}

static GpStatus WINAPI GdiplusStartup(uintptr_t *token, const void *input, void *output) {
    (void)input;
    if (!token) return GpInvalidParameter;
    *token = 1;
    if (output) memset(output, 0, 2 * sizeof(void *));               /* no background thread hooks */
    return GpOk;
}
static void WINAPI GdiplusShutdown(uintptr_t token) { (void)token; }
static void *WINAPI GdipAlloc(size_t n) { return kmalloc(n ? n : 1); }
static void WINAPI GdipFree(void *p) { kfree(p); }

/* a whole stream (from where it stands) through its own Read */
typedef HRESULT_T (WINAPI *stream_read_t)(void *, void *, ULONG, ULONG *);
static GpStatus WINAPI GdipCreateBitmapFromStream(void *stream, void **out) {
    if (!stream || !out) return GpInvalidParameter;
    *out = NULL;
    stream_read_t rd = (stream_read_t)(*(void ***)stream)[3];
    uint32_t cap = 65536, n = 0;
    uint8_t *buf = kmalloc(cap);
    if (!buf) return GpOutOfMemory;
    for (;;) {
        if (n == cap) { uint8_t *b2 = kmalloc(cap * 2); if (!b2) { kfree(buf); return GpOutOfMemory; } memcpy(b2, buf, n); kfree(buf); buf = b2; cap *= 2; }
        ULONG got = 0;
        HRESULT_T r = rd(stream, buf + n, cap - n, &got);
        n += got;
        if (r < 0 || got == 0) break;
    }
    char e[96];
    canvas_t *c = image_decode(buf, n, e, sizeof e);
    kfree(buf);
    if (!c) { klog("gdiplus: cannot decode a %u-byte image: %s", n, e); return GpUnknownImageFormat; }
    gpimage_t *i = img_new(c);
    if (!i) return GpOutOfMemory;
    *out = i;
    return GpOk;
}
static GpStatus WINAPI GdipCreateBitmapFromStreamICM(void *stream, void **out) { return GdipCreateBitmapFromStream(stream, out); }
static GpStatus WINAPI GdipLoadImageFromStream(void *stream, void **out) { return GdipCreateBitmapFromStream(stream, out); }

static GpStatus WINAPI GdipCreateBitmapFromHBITMAP(uintptr_t hbm, uintptr_t hpal, void **out) {
    (void)hpal;
    if (!out) return GpInvalidParameter;
    *out = NULL;
    gdiobj_t *o = OBJ(hbm);
    if (!o || o->type != OBJ_BITMAP || !o->bmp) return GpInvalidParameter;
    if (o->dib) dib_in(o);                                            /* the program's bits into the picture */
    canvas_t *c = canvas_new(o->bmp->w, o->bmp->h);
    if (!c) return GpOutOfMemory;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) c->px[y * c->stride + x] = o->bmp->px[y * o->bmp->stride + x] | 0xFF000000u;
    gpimage_t *i = img_new(c);
    if (!i) return GpOutOfMemory;
    *out = i;
    return GpOk;
}
/* PixelFormat32bppARGB 0x26200A, 32bppPARGB 0xE200B, 32bppRGB 0x22009, 24bppRGB 0x21808 */
static GpStatus WINAPI GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT fmt, const uint8_t *scan0, void **out) {
    if (!out || w <= 0 || h <= 0) return GpInvalidParameter;
    *out = NULL;
    canvas_t *c = canvas_new(w, h);
    if (!c) return GpOutOfMemory;
    bool alpha = fmt == 0x26200A || fmt == 0xE200B;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = alpha ? 0 : 0xFF000000u;
            if (scan0) {
                const uint8_t *row = stride >= 0 ? scan0 + (size_t)y * stride : scan0 - (size_t)y * (size_t)(-stride);
                if (fmt == 0x21808) v = 0xFF000000u | row[x * 3 + 2] << 16 | row[x * 3 + 1] << 8 | row[x * 3];
                else { const uint8_t *p = row + x * 4; v = (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; if (!alpha) v |= 0xFF000000u; }
            }
            c->px[y * c->stride + x] = v;
        }
    gpimage_t *i = img_new(c);
    if (!i) return GpOutOfMemory;
    *out = i;
    return GpOk;
}
static GpStatus WINAPI GdipGetImageWidth(void *img, UINT *w) { gpimage_t *i = IMG(img); if (!i || !w) return GpInvalidParameter; *w = i->c->w; return GpOk; }
static GpStatus WINAPI GdipGetImageHeight(void *img, UINT *h) { gpimage_t *i = IMG(img); if (!i || !h) return GpInvalidParameter; *h = i->c->h; return GpOk; }
static GpStatus WINAPI GdipCloneImage(void *img, void **out) {
    gpimage_t *i = IMG(img);
    if (!i || !out) return GpInvalidParameter;
    canvas_t *c = canvas_new(i->c->w, i->c->h);
    if (!c) return GpOutOfMemory;
    for (int y = 0; y < c->h; y++) memcpy(c->px + y * c->stride, i->c->px + y * i->c->stride, c->w * 4);
    *out = img_new(c);
    return *out ? GpOk : GpOutOfMemory;
}
static GpStatus WINAPI GdipDisposeImage(void *img) {
    gpimage_t *i = IMG(img);
    if (!i) return GpInvalidParameter;
    canvas_free(i->c); i->magic = 0; kfree(i);
    return GpOk;
}
/* RotateFlipType: 0 none, 1 90, 2 180, 3 270 (clockwise), 4..7 the same then flipped left-right */
static GpStatus WINAPI GdipImageRotateFlip(void *img, INT type) {
    gpimage_t *i = IMG(img);
    if (!i || type < 0 || type > 7) return GpInvalidParameter;
    if (type == 0) return GpOk;
    int rot = type & 3; bool flip = type >= 4;
    int w = i->c->w, h = i->c->h, nw = rot & 1 ? h : w, nh = rot & 1 ? w : h;
    canvas_t *c = canvas_new(nw, nh);
    if (!c) return GpOutOfMemory;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            int dx = flip ? nw - 1 - x : x;
            int sx, sy;
            switch (rot) {
            case 0: sx = dx; sy = y; break;
            case 1: sx = y; sy = h - 1 - dx; break;
            case 2: sx = w - 1 - dx; sy = h - 1 - y; break;
            default: sx = w - 1 - y; sy = dx; break;
            }
            c->px[y * c->stride + x] = i->c->px[sy * i->c->stride + sx];
        }
    canvas_free(i->c); i->c = c;
    return GpOk;
}
/* an HBITMAP of the picture, with what is transparent over the background colour */
static GpStatus WINAPI GdipCreateHBITMAPFromBitmap(void *img, uintptr_t *out, uint32_t bg) {
    gpimage_t *i = IMG(img);
    if (!i || !out) return GpInvalidParameter;
    *out = 0;
    gdiobj_t *o = newobj(OBJ_BITMAP);
    o->bmp = canvas_new(i->c->w, i->c->h);
    if (!o->bmp) { w32_free(P, o); return GpOutOfMemory; }
    for (int y = 0; y < i->c->h; y++)
        for (int x = 0; x < i->c->w; x++) {
            uint32_t s = i->c->px[y * i->c->stride + x], a = s >> 24;
            uint32_t r = (((s >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
            uint32_t g = (((s >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
            uint32_t b = ((s & 255) * a + (bg & 255) * (255 - a)) / 255;
            o->bmp->px[y * o->bmp->stride + x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    *out = (uintptr_t)o;
    return GpOk;
}

/* ---------------- drawing one picture on another ---------------- */
static GpStatus WINAPI GdipGetImageGraphicsContext(void *img, void **out) {
    gpimage_t *i = IMG(img);
    if (!i || !out) return GpInvalidParameter;
    gpgfx_t *g = kzalloc(sizeof *g);
    if (!g) return GpOutOfMemory;
    g->magic = GPGFX_MAGIC; g->c = i->c; g->interp = 3;
    *out = g;
    return GpOk;
}
static GpStatus WINAPI GdipDeleteGraphics(void *gfx) { gpgfx_t *g = GFX(gfx); if (!g) return GpInvalidParameter; g->magic = 0; kfree(g); return GpOk; }
static GpStatus WINAPI GdipSetInterpolationMode(void *gfx, INT mode) { gpgfx_t *g = GFX(gfx); if (!g) return GpInvalidParameter; g->interp = mode; return GpOk; }
static GpStatus WINAPI GdipSetCompositingQuality(void *gfx, INT q) { (void)q; return GFX(gfx) ? GpOk : GpInvalidParameter; }
static GpStatus WINAPI GdipSetSmoothingMode(void *gfx, INT m) { (void)m; return GFX(gfx) ? GpOk : GpInvalidParameter; }
static GpStatus WINAPI GdipSetPixelOffsetMode(void *gfx, INT m) { (void)m; return GFX(gfx) ? GpOk : GpInvalidParameter; }
/* the picture scaled into the rectangle (bilinear unless nearest neighbour was asked for),
 * blended over what is there by its alpha */
static GpStatus WINAPI GdipDrawImageRectI(void *gfx, void *img, INT x, INT y, INT w, INT h) {
    gpgfx_t *g = GFX(gfx); gpimage_t *i = IMG(img);
    if (!g || !i) return GpInvalidParameter;
    if (w <= 0 || h <= 0) return GpOk;
    canvas_t *d = g->c, *s = i->c;
    bool nearest = g->interp == 5;
    for (int yy = MAX(0, y); yy < MIN(d->h, y + h); yy++)
        for (int xx = MAX(0, x); xx < MIN(d->w, x + w); xx++) {
            uint32_t p;
            float fx = ((xx - x) + 0.5f) * s->w / w - 0.5f, fy = ((yy - y) + 0.5f) * s->h / h - 0.5f;
            if (nearest) p = s->px[CLAMP((int)(fy + 0.5f), 0, s->h - 1) * s->stride + CLAMP((int)(fx + 0.5f), 0, s->w - 1)];
            else {
                int x0 = (int)(fx < 0 ? fx - 1 : fx), y0 = (int)(fy < 0 ? fy - 1 : fy);
                float ax = fx - x0, ay = fy - y0;
                uint32_t q[4]; int sx[2] = { CLAMP(x0, 0, s->w - 1), CLAMP(x0 + 1, 0, s->w - 1) }, sy[2] = { CLAMP(y0, 0, s->h - 1), CLAMP(y0 + 1, 0, s->h - 1) };
                for (int k = 0; k < 4; k++) q[k] = s->px[sy[k >> 1] * s->stride + sx[k & 1]];
                p = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    float a = ((q[0] >> sh) & 255) * (1 - ax) + ((q[1] >> sh) & 255) * ax;
                    float b = ((q[2] >> sh) & 255) * (1 - ax) + ((q[3] >> sh) & 255) * ax;
                    p |= (uint32_t)(a * (1 - ay) + b * ay + 0.5f) << sh;
                }
            }
            uint32_t a = p >> 24;
            if (a == 255) { d->px[yy * d->stride + xx] = p; continue; }
            if (!a) continue;
            uint32_t o = d->px[yy * d->stride + xx], oa = o >> 24;
            uint32_t r = (((p >> 16) & 255) * a + ((o >> 16) & 255) * (255 - a)) / 255;
            uint32_t gg = (((p >> 8) & 255) * a + ((o >> 8) & 255) * (255 - a)) / 255;
            uint32_t b = ((p & 255) * a + (o & 255) * (255 - a)) / 255;
            uint32_t na = a + oa * (255 - a) / 255;
            d->px[yy * d->stride + xx] = na << 24 | r << 16 | gg << 8 | b;
        }
    return GpOk;
}
static GpStatus WINAPI GdipDrawImageI(void *gfx, void *img, INT x, INT y) { gpimage_t *i = IMG(img); if (!i) return GpInvalidParameter; return GdipDrawImageRectI(gfx, img, x, y, i->c->w, i->c->h); }

/* ---------------- encoders and saving: BMP and PNG ---------------- */
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } guid_t;
typedef struct {
    guid_t clsid, format;
    const uint16_t *codec, *dll, *desc, *ext, *mime;
    DWORD flags, version, sigcount, sigsize;
    const uint8_t *sigpattern, *sigmask;
} codecinfo_t;
static const guid_t clsid_bmp = { 0x557CF400, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
static const guid_t clsid_png = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
static const guid_t fmt_bmp = { 0xB96B3CAB, 0x0728, 0x11D3, { 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
static const guid_t fmt_png = { 0xB96B3CAF, 0x0728, 0x11D3, { 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
static const char *const enc_text[2][4] = {
    { "Built-in BMP Codec", "BMP", "*.BMP;*.DIB;*.RLE", "image/bmp" },
    { "Built-in PNG Codec", "PNG", "*.PNG", "image/png" },
};
/* the size the encoder list takes: the records, then their strings (UTF-16) */
static UINT enc_bytes(void) {
    UINT n = 2 * sizeof(codecinfo_t);
    for (int e = 0; e < 2; e++) for (int k = 0; k < 4; k++) n += (strlen(enc_text[e][k]) + 1) * 2;
    return n;
}
static GpStatus WINAPI GdipGetImageEncodersSize(UINT *num, UINT *size) {
    if (!num || !size) return GpInvalidParameter;
    *num = 2; *size = enc_bytes();
    return GpOk;
}
static GpStatus WINAPI GdipGetImageEncoders(UINT num, UINT size, codecinfo_t *out) {
    if (!out || num < 2 || size < enc_bytes()) return GpInvalidParameter;
    uint16_t *str = (uint16_t *)(out + 2);
    for (int e = 0; e < 2; e++) {
        codecinfo_t *c = &out[e];
        memset(c, 0, sizeof *c);
        c->clsid = e ? clsid_png : clsid_bmp; c->format = e ? fmt_png : fmt_bmp;
        const uint16_t **slots[4] = { &c->codec, &c->desc, &c->ext, &c->mime };
        for (int k = 0; k < 4; k++) {
            *slots[k] = str;
            for (const char *t = enc_text[e][k]; *t; t++) *str++ = (uint8_t)*t;
            *str++ = 0;
        }
        c->flags = 0x10007; c->version = 1;                            /* encoder, decoder, bitmap, built in */
    }
    return GpOk;
}
static GpStatus WINAPI GdipGetImageDecodersSize(UINT *num, UINT *size) { return GdipGetImageEncodersSize(num, size); }
static GpStatus WINAPI GdipGetImageDecoders(UINT num, UINT size, codecinfo_t *out) { return GdipGetImageEncoders(num, size, out); }

static uint32_t png_crc(const uint8_t *d, size_t n, uint32_t c) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; t[i] = v; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void put32be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
/* a PNG (RGBA, stored zlib blocks) or a BMP (32-bit) of the picture, in a new buffer */
static uint8_t *encode_image(canvas_t *c, bool png, uint32_t *len) {
    int w = c->w, h = c->h;
    if (!png) {
        uint32_t n = 54 + (uint32_t)w * h * 4;
        uint8_t *b = kzalloc(n);
        if (!b) return NULL;
        b[0] = 'B'; b[1] = 'M'; memcpy(b + 2, &n, 4); b[10] = 54;
        int32_t bw = w, bh = -h; uint16_t planes = 1, bpp = 32; uint32_t hs = 40;
        memcpy(b + 14, &hs, 4); memcpy(b + 18, &bw, 4); memcpy(b + 22, &bh, 4); memcpy(b + 26, &planes, 2); memcpy(b + 28, &bpp, 2);
        for (int y = 0; y < h; y++) memcpy(b + 54 + (size_t)y * w * 4, c->px + y * c->stride, w * 4);
        *len = n;
        return b;
    }
    uint32_t raw = (uint32_t)h * (1 + w * 4);
    uint32_t blocks = (raw + 65534) / 65535;
    uint32_t zlen = 2 + raw + blocks * 5 + 4;
    uint32_t n = 8 + 25 + 12 + zlen + 12;
    uint8_t *b = kmalloc(n), *p = b;
    uint8_t *rows = kmalloc(raw);
    if (!b || !rows) { kfree(b); kfree(rows); return NULL; }
    for (int y = 0; y < h; y++) {
        uint8_t *r = rows + (size_t)y * (1 + w * 4);
        r[0] = 0;
        for (int x = 0; x < w; x++) { uint32_t v = c->px[y * c->stride + x]; r[1 + x * 4] = v >> 16; r[2 + x * 4] = v >> 8; r[3 + x * 4] = v; r[4 + x * 4] = v >> 24; }
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    memcpy(p, sig, 8); p += 8;
    put32be(p, 13); memcpy(p + 4, "IHDR", 4); put32be(p + 8, w); put32be(p + 12, h); p[16] = 8; p[17] = 6; p[18] = p[19] = p[20] = 0;
    put32be(p + 21, png_crc(p + 4, 17, 0)); p += 25;
    put32be(p, zlen); memcpy(p + 4, "IDAT", 4);
    uint8_t *z = p + 8, *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, bsum = 0;
    for (uint32_t off = 0; off < raw; ) {
        uint32_t k = MIN(65535u, raw - off);
        *q++ = off + k >= raw ? 1 : 0; *q++ = k; *q++ = k >> 8; *q++ = ~k; *q++ = (~k) >> 8;
        memcpy(q, rows + off, k); q += k;
        for (uint32_t i = 0; i < k; i++) { a = (a + rows[off + i]) % 65521; bsum = (bsum + a) % 65521; }
        off += k;
    }
    put32be(q, bsum << 16 | a); q += 4;
    put32be(q, png_crc(p + 4, 4 + zlen, 0)); q += 4;
    p = q;
    put32be(p, 0); memcpy(p + 4, "IEND", 4); put32be(p + 8, png_crc(p + 4, 4, 0)); p += 12;
    kfree(rows);
    *len = (uint32_t)(p - b);
    return b;
}
extern void *w32_builtin(const char *dll, const char *name);
typedef HANDLE (WINAPI *createfilew_t)(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *writefile_t)(HANDLE, const void *, DWORD, DWORD *, void *);
typedef BOOL (WINAPI *closehandle_t)(HANDLE);
static GpStatus WINAPI GdipSaveImageToFile(void *img, const WCHAR *name, const guid_t *clsid, const void *params) {
    (void)params;
    gpimage_t *i = IMG(img);
    if (!i || !name || !clsid) return GpInvalidParameter;
    bool png = !memcmp(clsid, &clsid_png, sizeof(guid_t)), bmp = !memcmp(clsid, &clsid_bmp, sizeof(guid_t));
    if (!png && !bmp) return GpUnknownImageFormat;
    uint32_t len = 0;
    uint8_t *data = encode_image(i->c, png, &len);
    if (!data) return GpOutOfMemory;
    createfilew_t cf = (createfilew_t)w32_builtin("kernel32.dll", "CreateFileW");
    writefile_t wf = (writefile_t)w32_builtin("kernel32.dll", "WriteFile");
    closehandle_t ch = (closehandle_t)w32_builtin("kernel32.dll", "CloseHandle");
    GpStatus st = GpWin32Error;
    if (cf && wf && ch) {
        HANDLE f = cf(name, 0x40000000, 0, NULL, 2, 0x80, 0);         /* GENERIC_WRITE, CREATE_ALWAYS */
        if (f && f != (HANDLE)(intptr_t)-1) {
            DWORD put = 0;
            if (wf(f, data, len, &put, NULL) && put == len) st = GpOk;
            ch(f);
        }
    }
    kfree(data);
    return st;
}
static GpStatus WINAPI GdipSaveImageToStream(void *img, void *stream, const guid_t *clsid, const void *params) {
    (void)params;
    gpimage_t *i = IMG(img);
    if (!i || !stream || !clsid) return GpInvalidParameter;
    bool png = !memcmp(clsid, &clsid_png, sizeof(guid_t));
    if (!png && memcmp(clsid, &clsid_bmp, sizeof(guid_t))) return GpUnknownImageFormat;
    uint32_t len = 0;
    uint8_t *data = encode_image(i->c, png, &len);
    if (!data) return GpOutOfMemory;
    typedef HRESULT_T (WINAPI *write_t)(void *, const void *, ULONG, ULONG *);
    ULONG put = 0;
    HRESULT_T r = ((write_t)(*(void ***)stream)[4])(stream, data, len, &put);
    kfree(data);
    return r >= 0 && put == len ? GpOk : GpWin32Error;
}

#define GP(n) { "gdiplus", #n, (void *)n }
const w32_api_t gdiplus_api[] = {
    GP(GdiplusStartup), GP(GdiplusShutdown), GP(GdipAlloc), GP(GdipFree),
    GP(GdipCreateBitmapFromStream), GP(GdipCreateBitmapFromStreamICM), GP(GdipLoadImageFromStream), GP(GdipCreateBitmapFromHBITMAP),
    GP(GdipCreateBitmapFromScan0), GP(GdipGetImageWidth), GP(GdipGetImageHeight), GP(GdipCloneImage), GP(GdipDisposeImage),
    GP(GdipImageRotateFlip), GP(GdipCreateHBITMAPFromBitmap), GP(GdipGetImageGraphicsContext), GP(GdipDeleteGraphics),
    GP(GdipSetInterpolationMode), GP(GdipSetCompositingQuality), GP(GdipSetSmoothingMode), GP(GdipSetPixelOffsetMode),
    GP(GdipDrawImageRectI), GP(GdipDrawImageI), GP(GdipGetImageEncodersSize), GP(GdipGetImageEncoders),
    GP(GdipGetImageDecodersSize), GP(GdipGetImageDecoders), GP(GdipSaveImageToFile), GP(GdipSaveImageToStream),
    { "ole32", "CreateStreamOnHGlobal", (void *)CreateStreamOnHGlobal },
    { NULL, NULL, NULL }
};
