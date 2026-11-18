/* More of user32, gdi32 and comctl32 (included by user32.c after user32_dlg.inc):
 * DIB sections and the DIB functions, bitmaps from resources and .bmp files, regions as
 * rectangles, viewport/window origins, SaveDC/RestoreDC, arcs and curves, flood fill, wide
 * font functions; menu check/enable state, menu item info; window properties; display and
 * keyboard queries; image lists. Entries are found before the older tables
 * (usermore_api, comctl_api via w32_extra_find), so several replace stubs above. */

static void um_w2a(const WCHAR *w, char *a, int cap) { int i = 0; if (w) for (; w[i] && i < cap - 1; i++) a[i] = w[i] < 256 ? (char)w[i] : '?'; a[i] = 0; }
static int um_a2w(const char *a, WCHAR *w, int cap) { int i = 0; for (; a && a[i] && i < cap - 1; i++) w[i] = (uint8_t)a[i]; if (cap > 0) w[i] = 0; return i; }
static uint32_t rgb_of(uint8_t r, uint8_t g, uint8_t b) { return 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b; }
static COLORREF colref(uint32_t px) { return ((px >> 16) & 0xFF) | (px & 0xFF00) | ((px & 0xFF) << 16); }

/* ================================================================ DIBs */
typedef struct PACKED { DWORD size; LONG w, h; WORD planes, bpp; DWORD comp, img, xppm, yppm, used, important; } BIH;
typedef struct dib { uint8_t *bits; int w, h, bpp, stride; bool bottomup, own; uint32_t pal[256]; int npal; uint32_t masks[3]; DWORD comp; } dib_t;
static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(uint32_t m) { int b = 0; m >>= mask_shift(m); while (m & 1) { m >>= 1; b++; } return b; }
static uint8_t mask_get(uint32_t v, uint32_t m) { if (!m) return 0; int s = mask_shift(m), b = mask_bits(m); uint32_t x = (v & m) >> s; return b >= 8 ? (uint8_t)(x >> (b - 8)) : (uint8_t)(x * 255 / ((1u << b) - 1)); }
/* palette and masks from a BITMAPINFO: the colour table follows the header */
static int dib_palette(const BIH *bi, UINT usage, uint32_t *pal, uint32_t *masks) {
    int n = 0;
    const uint8_t *tab = (const uint8_t *)bi + bi->size;
    if (bi->bpp <= 8) {
        n = bi->used ? (int)MIN(bi->used, 256u) : 1 << bi->bpp;
        for (int i = 0; i < n; i++) {
            if (usage == 1) { uint32_t v = 0xFF000000u | (uint32_t)(i * 255 / MAX(1, n - 1)) * 0x010101; pal[i] = v; }   /* DIB_PAL_COLORS: a gray ramp */
            else pal[i] = rgb_of(tab[i * 4 + 2], tab[i * 4 + 1], tab[i * 4]);
        }
    }
    if (masks) {
        if (bi->comp == 3 || bi->comp == 6) { const uint32_t *m = bi->size >= 52 ? (const uint32_t *)((const uint8_t *)bi + 40) : (const uint32_t *)tab; masks[0] = m[0]; masks[1] = m[1]; masks[2] = m[2]; }
        else if (bi->bpp == 16) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; }
        else { masks[0] = 0xFF0000; masks[1] = 0xFF00; masks[2] = 0xFF; }
    }
    return n;
}
static int dib_stride(int w, int bpp) { return ((w * bpp + 31) / 32) * 4; }
/* one pixel of a row */
static uint32_t dib_px(const uint8_t *row, int x, int bpp, const uint32_t *pal, const uint32_t *m) {
    switch (bpp) {
    case 1: return pal[(row[x >> 3] >> (7 - (x & 7))) & 1];
    case 4: return pal[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
    case 8: return pal[row[x]];
    case 16: { uint16_t v = ((const uint16_t *)row)[x]; return rgb_of(mask_get(v, m[0]), mask_get(v, m[1]), mask_get(v, m[2])); }
    case 24: return rgb_of(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
    case 32: { uint32_t v = ((const uint32_t *)row)[x]; return m[0] == 0xFF0000 && m[2] == 0xFF ? (v | 0xFF000000u) : rgb_of(mask_get(v, m[0]), mask_get(v, m[1]), mask_get(v, m[2])); }
    }
    return 0xFF000000u;
}
/* RLE8 / RLE4 to 8-bit indices */
static uint8_t *rle_decode(const uint8_t *s, uint32_t n, int w, int h, bool four) {
    uint8_t *o = kzalloc((size_t)w * h);
    int x = 0, y = 0; uint32_t i = 0;
    while (i + 1 < n && y < h) {
        uint8_t a = s[i++], b = s[i++];
        if (a) { for (int k = 0; k < a; k++, x++) if (x < w) o[y * w + x] = four ? ((k & 1) ? b & 15 : b >> 4) : b; }
        else if (b == 0) { x = 0; y++; }
        else if (b == 1) break;
        else if (b == 2) { if (i + 1 < n) { x += s[i]; y += s[i + 1]; i += 2; } }
        else {
            for (int k = 0; k < b; k++, x++) { uint8_t v = four ? ((k & 1) ? s[i + k / 2] & 15 : s[i + k / 2] >> 4) : s[i + k]; if (x < w) o[y * w + x] = v; }
            int used = four ? (b + 1) / 2 : b; i += used; if (used & 1) i++;
        }
    }
    return o;                                         /* rows bottom-up, as in the file */
}
/* decode a packed DIB (header, colours, bits) into a new canvas */
static canvas_t *dib_decode(const BIH *bi, const void *bits, UINT usage) {
    if (!bi || bi->size < 12) return NULL;
    int w, h, bpp; DWORD comp = 0;
    if (bi->size == 12) { const uint16_t *c = (const uint16_t *)((const uint8_t *)bi + 4); w = c[0]; h = (int16_t)c[1]; bpp = c[3]; }
    else { w = bi->w; h = bi->h; bpp = bi->bpp; comp = bi->comp; }
    bool bottomup = h > 0; if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    uint32_t pal[256] = { 0 }, m[3];
    if (bi->size == 12) { const uint8_t *t = (const uint8_t *)bi + 12; int n = bpp <= 8 ? 1 << bpp : 0; for (int i = 0; i < n; i++) pal[i] = rgb_of(t[i * 3 + 2], t[i * 3 + 1], t[i * 3]); m[0] = 0xFF0000; m[1] = 0xFF00; m[2] = 0xFF; }
    else dib_palette(bi, usage, pal, m);
    if (!bits) {                                       /* packed: the bits follow the colours */
        int ncol = bpp <= 8 ? (bi->size >= 40 && bi->used ? (int)bi->used : 1 << bpp) : 0;
        int extra = (bi->size == 40 && (comp == 3)) ? 12 : 0;
        bits = (const uint8_t *)bi + bi->size + ncol * (bi->size == 12 ? 3 : 4) + extra;
    }
    canvas_t *c = canvas_new(w, h);
    if (!c) return NULL;
    if (comp == 1 || comp == 2) {                      /* RLE8, RLE4 */
        uint8_t *idx = rle_decode(bits, bi->img ? bi->img : (uint32_t)w * h, w, h, comp == 2);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) c->px[(h - 1 - y) * c->stride + x] = pal[idx[y * w + x]];
        kfree(idx);
        return c;
    }
    int stride = dib_stride(w, bpp);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)bits + (size_t)(bottomup ? h - 1 - y : y) * stride;
        uint32_t *dst = c->px + (size_t)y * c->stride;
        for (int x = 0; x < w; x++) dst[x] = dib_px(row, x, bpp, pal, m);
    }
    return c;
}
/* DIB sections: the canvas mirrors the program's buffer around each GDI call (dc_begin/dc_end) */
static void dib_in(gdiobj_t *o) {
    dib_t *d = o->dib; canvas_t *c = o->bmp;
    if (d->bpp == 32 && d->masks[0] == 0xFF0000 && d->masks[2] == 0xFF) {
        for (int y = 0; y < d->h; y++) { const uint32_t *s = (const uint32_t *)(d->bits + (size_t)(d->bottomup ? d->h - 1 - y : y) * d->stride); uint32_t *t = c->px + (size_t)y * c->stride; for (int x = 0; x < d->w; x++) t[x] = s[x] | 0xFF000000u; }
        return;
    }
    for (int y = 0; y < d->h; y++) {
        const uint8_t *row = d->bits + (size_t)(d->bottomup ? d->h - 1 - y : y) * d->stride;
        uint32_t *t = c->px + (size_t)y * c->stride;
        for (int x = 0; x < d->w; x++) t[x] = dib_px(row, x, d->bpp, d->pal, d->masks);
    }
}
static int nearest_pal(const dib_t *d, uint32_t px) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < d->npal; i++) { int dr = (int)((px >> 16) & 255) - (int)((d->pal[i] >> 16) & 255), dg = (int)((px >> 8) & 255) - (int)((d->pal[i] >> 8) & 255), db = (int)(px & 255) - (int)(d->pal[i] & 255); int e = dr * dr + dg * dg + db * db; if (e < bd) { bd = e; best = i; if (!e) break; } }
    return best;
}
static void dib_out(gdiobj_t *o) {
    dib_t *d = o->dib; canvas_t *c = o->bmp;
    for (int y = 0; y < d->h; y++) {
        uint8_t *row = d->bits + (size_t)(d->bottomup ? d->h - 1 - y : y) * d->stride;
        const uint32_t *s = c->px + (size_t)y * c->stride;
        for (int x = 0; x < d->w; x++) {
            uint32_t p = s[x];
            switch (d->bpp) {
            case 32: ((uint32_t *)row)[x] = d->masks[0] == 0xFF0000 ? p : p; break;
            case 24: row[x * 3] = p & 255; row[x * 3 + 1] = (p >> 8) & 255; row[x * 3 + 2] = (p >> 16) & 255; break;
            case 16: { uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                ((uint16_t *)row)[x] = d->masks[1] == 0x07E0 ? (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3) : (uint16_t)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break; }
            case 8: row[x] = nearest_pal(d, p); break;
            case 4: { int v = nearest_pal(d, p); row[x >> 1] = (x & 1) ? (row[x >> 1] & 0xF0) | v : (row[x >> 1] & 0x0F) | v << 4; break; }
            case 1: { int v = nearest_pal(d, p); if (v) row[x >> 3] |= 0x80 >> (x & 7); else row[x >> 3] &= ~(0x80 >> (x & 7)); break; }
            }
        }
    }
}
static HGDIOBJ WINAPI CreateDIBSection(HDC hdc, const BIH *bi, UINT usage, void **bits, HANDLE sec, DWORD off) {
    if (bits) *bits = NULL;
    if (!bi || bi->size < 40 || !bi->w || !bi->h) return 0;
    int bpp = bi->bpp; if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    int w = bi->w, h = bi->h < 0 ? -bi->h : bi->h;
    gdiobj_t *o = newobj(OBJ_BITMAP);
    o->bmp = canvas_new(w, h);
    if (!o->bmp) { w32_free(P, o); return 0; }
    dib_t *d = kzalloc(sizeof *d);
    d->w = w; d->h = h; d->bpp = bpp; d->stride = dib_stride(w, bpp); d->bottomup = bi->h > 0; d->comp = bi->comp;
    d->npal = dib_palette(bi, usage, d->pal, d->masks);
    d->bits = w32_alloc(P, (size_t)d->stride * h); memset(d->bits, 0, (size_t)d->stride * h); d->own = true;
    o->dib = d;
    dib_in(o);
    if (bits) *bits = d->bits;
    return (HGDIOBJ)o;
}
static HGDIOBJ WINAPI CreateDIBitmap(HDC hdc, const BIH *bi, DWORD init, const void *bits, const BIH *info, UINT usage) {
    int w = bi->size == 12 ? ((const uint16_t *)bi)[2] : bi->w, h = bi->size == 12 ? ((const uint16_t *)bi)[3] : bi->h;
    if (h < 0) h = -h;
    gdiobj_t *o = (gdiobj_t *)CreateCompatibleBitmap(hdc, MAX(1, w), MAX(1, h));
    if ((init & 4) && bits && info) {                  /* CBM_INIT */
        canvas_t *c = dib_decode(info, bits, usage);
        if (c) { blit(o->bmp, 0, 0, c, 0, 0, MIN(c->w, o->bmp->w), MIN(c->h, o->bmp->h)); canvas_free(c); }
    }
    return (HGDIOBJ)o;
}
static int WINAPI SetDIBits(HDC hdc, HGDIOBJ hb, UINT start, UINT lines, const void *bits, const BIH *bi, UINT usage) {
    gdiobj_t *o = OBJ(hb); if (!o || o->type != OBJ_BITMAP || !o->bmp) return 0;
    if (o->dib) dib_in(o);
    canvas_t *c = dib_decode(bi, bits, usage); if (!c) return 0;
    int h = c->h; int y0 = bi->h > 0 ? h - (int)start - (int)lines : (int)start;
    for (int y = MAX(0, y0); y < MIN(h, y0 + (int)lines) && y < o->bmp->h; y++)
        memcpy(o->bmp->px + (size_t)y * o->bmp->stride, c->px + (size_t)y * c->stride, MIN(c->w, o->bmp->w) * 4);
    canvas_free(c);
    if (o->dib) dib_out(o);
    return lines;
}
/* GetDIBits: the header (bits NULL) or the pixels converted to 24 or 32 bits per pixel */
static int WINAPI GetDIBits2(HDC hdc, HGDIOBJ hb, UINT start, UINT lines, void *bits, BIH *bi, UINT usage) {
    gdiobj_t *o = OBJ(hb); if (!o || o->type != OBJ_BITMAP || !o->bmp || !bi) return 0;
    if (o->dib) dib_in(o);
    canvas_t *c = o->bmp;
    if (!bits || !bi->bpp) {
        bool keep_dir = bi->h < 0;
        bi->w = c->w; bi->h = keep_dir ? -c->h : c->h; bi->planes = 1;
        if (!bi->bpp) bi->bpp = o->dib ? o->dib->bpp : 32;
        bi->comp = 0; bi->img = dib_stride(c->w, bi->bpp) * c->h;
        return c->h;
    }
    int bpp = bi->bpp, h = bi->h < 0 ? -bi->h : bi->h, w = bi->w; bool bottomup = bi->h > 0;
    int stride = dib_stride(w, bpp), done = 0;
    for (UINT k = 0; k < lines; k++) {
        int row = start + k; if (row >= h) break;
        int sy = bottomup ? h - 1 - row : row; if (sy < 0 || sy >= c->h) continue;
        uint8_t *dst = (uint8_t *)bits + (size_t)k * stride;
        for (int x = 0; x < w && x < c->w; x++) {
            uint32_t p = c->px[(size_t)sy * c->stride + x];
            if (bpp == 32) ((uint32_t *)dst)[x] = p;
            else if (bpp == 24) { dst[x * 3] = p & 255; dst[x * 3 + 1] = (p >> 8) & 255; dst[x * 3 + 2] = (p >> 16) & 255; }
            else if (bpp == 16) ((uint16_t *)dst)[x] = (uint16_t)(((p >> 19) & 31) << 10 | ((p >> 11) & 31) << 5 | ((p >> 3) & 31));
        }
        done++;
    }
    return done;
}
static int draw_dib(dc_t *d, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const void *bits, const BIH *bi, UINT usage) {
    canvas_t *c = dib_decode(bi, bits, usage); if (!c) return 0;
    int sh = c->h;
    /* source rectangle in DIB coordinates: y counts from the bottom for bottom-up DIBs */
    int top = bi->h > 0 ? sh - ys - hs : ys;
    rect_t sv; dc_begin(d, &sv);
    bool flipx = wd < 0, flipy = hd < 0; if (flipx) wd = -wd; if (flipy) hd = -hd;
    if (wd == ws && hd == hs && !flipx && !flipy) blit(d->surf, d->ox + xd, d->oy + yd, c, xs, top, ws, hs);
    else for (int j = 0; j < hd; j++) {
        int sy = top + (flipy ? hd - 1 - j : j) * hs / hd; if (sy < 0 || sy >= sh) continue;
        for (int i = 0; i < wd; i++) { int sx = xs + (flipx ? wd - 1 - i : i) * ws / wd; if (sx < 0 || sx >= c->w) continue; put_pixel(d->surf, d->ox + xd + i, d->oy + yd + j, c->px[(size_t)sy * c->stride + sx]); }
    }
    dc_end(d, sv);
    canvas_free(c);
    return hs;
}
static int WINAPI StretchDIBits(HDC h, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const void *bits, const BIH *bi, UINT usage, DWORD rop) {
    dc_t *d = DC(h); if (!d || !bi) return 0;
    return draw_dib(d, xd, yd, wd, hd, xs, ys, ws, hs, bits, bi, usage);
}
static int WINAPI SetDIBitsToDevice(HDC h, int xd, int yd, DWORD w, DWORD ht, int xs, int ys, UINT start, UINT lines, const void *bits, const BIH *bi, UINT usage) {
    dc_t *d = DC(h); if (!d || !bi) return 0;
    draw_dib(d, xd, yd, w, ht, xs, ys, w, ht, bits, bi, usage);
    return lines;
}
static LONG WINAPI GetBitmapBits(HGDIOBJ hb, LONG n, void *out) {
    gdiobj_t *o = OBJ(hb); if (!o || !o->bmp) return 0;
    if (o->dib) dib_in(o);
    LONG need = o->bmp->w * o->bmp->h * 4; if (!out) return need;
    LONG k = 0; for (int y = 0; y < o->bmp->h && k < n; y++) { LONG l = MIN((LONG)(o->bmp->w * 4), n - k); memcpy((uint8_t *)out + k, o->bmp->px + (size_t)y * o->bmp->stride, l); k += l; }
    return k;
}
static LONG WINAPI SetBitmapBits(HGDIOBJ hb, DWORD n, const void *in) {
    gdiobj_t *o = OBJ(hb); if (!o || !o->bmp) return 0;
    DWORD k = 0; for (int y = 0; y < o->bmp->h && k < n; y++) { DWORD l = MIN((DWORD)(o->bmp->w * 4), n - k); memcpy(o->bmp->px + (size_t)y * o->bmp->stride, (const uint8_t *)in + k, l); k += l; }
    if (o->dib) dib_out(o);
    return k;
}
static UINT WINAPI GetDIBColorTable(HDC h, UINT start, UINT n, uint8_t *out) {
    dc_t *d = DC(h); if (!d || !d->bitmap || !d->bitmap->dib) return 0;
    dib_t *b = d->bitmap->dib; UINT k = 0;
    for (; k < n && start + k < (UINT)b->npal; k++) { uint32_t c = b->pal[start + k]; out[k * 4] = c & 255; out[k * 4 + 1] = (c >> 8) & 255; out[k * 4 + 2] = (c >> 16) & 255; out[k * 4 + 3] = 0; }
    return k;
}
static UINT WINAPI SetDIBColorTable(HDC h, UINT start, UINT n, const uint8_t *in) {
    dc_t *d = DC(h); if (!d || !d->bitmap || !d->bitmap->dib) return 0;
    dib_t *b = d->bitmap->dib; UINT k = 0;
    for (; k < n && start + k < 256; k++) b->pal[start + k] = rgb_of(in[k * 4 + 2], in[k * 4 + 1], in[k * 4]);
    if ((int)(start + k) > b->npal) b->npal = start + k;
    return k;
}
/* bitmaps from resources (RT_BITMAP: a packed DIB) and from .bmp files; OEM bitmaps as blanks */
static HGDIOBJ bitmap_from_packed(const uint8_t *d, uint32_t n) {
    canvas_t *c = dib_decode((const BIH *)d, NULL, 0); if (!c) return 0;
    gdiobj_t *o = newobj(OBJ_BITMAP); o->bmp = c;
    return (HGDIOBJ)o;
}
static HGDIOBJ load_bitmap(HINSTANCE inst, uintptr_t name, bool wide) {
    if (!inst && name < 0x10000) { gdiobj_t *o = (gdiobj_t *)CreateCompatibleBitmap(0, 16, 16); gfx_clear(o->bmp, T.face); return (HGDIOBJ)o; }   /* OBM_* */
    uintptr_t r = w32_find_resource_in(P, inst, 2, name, wide, 0);
    uint32_t sz = 0; const uint8_t *d = r ? w32_resource_data(P, r, &sz) : NULL;
    if (!d) { P->last_error = 1814; klog("w32: LoadBitmap(%p, %p): no such bitmap resource", (void *)inst, (void *)name); return 0; }
    HGDIOBJ b = bitmap_from_packed(d, sz);
    if (!b) klog("w32: LoadBitmap(%p): bitmap resource not understood", (void *)name);
    return b;
}
static HGDIOBJ WINAPI LoadBitmapA(HINSTANCE h, const char *n) { return load_bitmap(h, (uintptr_t)n, false); }
static HGDIOBJ WINAPI LoadBitmapW(HINSTANCE h, const WCHAR *n) { return load_bitmap(h, (uintptr_t)n, true); }
static HGDIOBJ bitmap_file(const char *path) {
    char full[300];
    if (isalpha(path[0]) && path[1] == ':') strlcpy(full, path, sizeof full); else path_join(full, sizeof full, P->cwd, path);
    vnode_t *v = vfs_resolve(full, NULL);
    if (!v || v->dir || v->size < 26 || v->data[0] != 'B' || v->data[1] != 'M') return 0;
    uint32_t off = *(const uint32_t *)(v->data + 10);
    canvas_t *c = dib_decode((const BIH *)(v->data + 14), off < v->size ? v->data + off : NULL, 0);
    if (!c) return 0;
    gdiobj_t *o = newobj(OBJ_BITMAP); o->bmp = c;
    return (HGDIOBJ)o;
}
static HANDLE WINAPI LoadImageA2(HINSTANCE h, const char *n, UINT type, int cx, int cy, UINT fl) {
    if (type != 0) return type == 1 || type == 2 ? 0x20003 : 0;   /* icons and cursors: the stock ones */
    HGDIOBJ b = (fl & 0x10) ? bitmap_file(n) : load_bitmap(h, (uintptr_t)n, false);
    if (b && (cx || cy) && (fl & 0x10) == 0) {}
    return b;
}
static HANDLE WINAPI LoadImageW2(HINSTANCE h, const WCHAR *n, UINT type, int cx, int cy, UINT fl) {
    if (type != 0) return type == 1 || type == 2 ? 0x20003 : 0;
    if (fl & 0x10) { char a[300]; um_w2a(n, a, sizeof a); return bitmap_file(a); }
    return load_bitmap(h, (uintptr_t)n, true);
}
static HANDLE WINAPI CreateIcon(HINSTANCE h, int w, int ht, BYTE planes, BYTE bpp, const BYTE *and, const BYTE *xor) { return 0x20002; }
static HANDLE WINAPI CreateIconFromResource(BYTE *d, DWORD n, BOOL icon, DWORD ver) { return 0x20002; }
static HANDLE WINAPI CreateIconFromResourceEx(BYTE *d, DWORD n, BOOL icon, DWORD ver, int cx, int cy, UINT f) { return 0x20002; }
static int WINAPI LookupIconIdFromDirectoryEx(BYTE *d, BOOL icon, int cx, int cy, UINT f) { return 1; }
static HANDLE WINAPI GetCursor(void) { return 0x20001; }
typedef struct { DWORD size, flags; HANDLE cur; LONG x, y; } CURSORINFO;
static BOOL WINAPI GetCursorInfo(CURSORINFO *c) { c->flags = 1; c->cur = 0x20001; c->x = wm_mouse_x; c->y = wm_mouse_y; return 1; }

/* ================================================================ GDI objects */
static HGDIOBJ WINAPI CreateFontW(int h, int w, int esc, int ori, int weight, DWORD it, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pitch, const WCHAR *face) {
    char a[64]; um_w2a(face, a, sizeof a);
    gdiobj_t *o = (gdiobj_t *)CreateFontA(h, w, esc, ori, weight, it, ul, so, cs, op, cp, q, pitch, a);
    o->font_h = h; o->font_weight = weight; o->font_italic = it; o->font_underline = ul; strlcpy(o->face, a, sizeof o->face);
    return (HGDIOBJ)o;
}
typedef struct { LONG h, w, esc, ori, weight; BYTE it, ul, so, cs, op, cp, q, pf; WCHAR face[32]; } LOGFONTW_;
static HGDIOBJ WINAPI CreateFontIndirectW(const LOGFONTW_ *l) { return CreateFontW(l->h, l->w, l->esc, l->ori, l->weight, l->it, l->ul, l->so, l->cs, l->op, l->cp, l->q, l->pf, l->face); }
static HGDIOBJ WINAPI CreateFontIndirectA2(const LOGFONTA *l) {
    gdiobj_t *o = (gdiobj_t *)CreateFontA(l->h, l->w, 0, 0, l->weight, l->it, l->ul, 0, 0, 0, 0, 0, 0, l->face);
    o->font_h = l->h; o->font_weight = l->weight; o->font_italic = l->it; o->font_underline = l->ul; strlcpy(o->face, l->face, sizeof o->face);
    return (HGDIOBJ)o;
}
static HGDIOBJ WINAPI CreateFontIndirectExW(const void *e) { return CreateFontIndirectW(e); }
typedef struct { LONG height, ascent, descent, intlead, extlead, avewidth, maxwidth, weight, overhang, dax, day; WCHAR first, last, def, brk; BYTE it, ul, so, pitch, cs; } TEXTMETRICW_;
static BOOL WINAPI GetTextMetricsW(HDC h, TEXTMETRICW_ *tm) {
    TEXTMETRICA a; GetTextMetricsA(h, &a);
    memset(tm, 0, sizeof *tm);
    tm->height = a.height; tm->ascent = a.ascent; tm->descent = a.descent; tm->avewidth = a.avewidth; tm->maxwidth = a.maxwidth;
    tm->weight = a.weight; tm->dax = a.dax; tm->day = a.day; tm->first = 32; tm->last = 0xFFFC; tm->def = '?'; tm->brk = ' ';
    dc_t *d = DC(h); if (d && d->font) { tm->weight = d->font->font_weight ? d->font->font_weight : 400; tm->it = d->font->font_italic; tm->ul = d->font->font_underline; tm->pitch = dcfont(d) == FONT_MONO ? 0x30 : 0x26; }
    return 1;
}
static BOOL WINAPI GetTextMetricsA2(HDC h, TEXTMETRICA *tm) {
    GetTextMetricsA(h, tm);
    dc_t *d = DC(h); if (d && d->font) { tm->weight = d->font->font_weight ? d->font->font_weight : 400; tm->it = d->font->font_italic; tm->ul = d->font->font_underline; tm->pitch = dcfont(d) == FONT_MONO ? 0x30 : 0x26; }
    return 1;
}
static int WINAPI GetTextFaceW(HDC h, int n, WCHAR *out) { dc_t *d = DC(h); const char *f = d && d->font && d->font->face[0] ? d->font->face : (d && dcfont(d) == FONT_MONO ? "Courier New" : "Tahoma"); if (!out) return strlen(f) + 1; return um_a2w(f, out, n); }
static int WINAPI GetTextFaceA(HDC h, int n, char *out) { dc_t *d = DC(h); const char *f = d && d->font && d->font->face[0] ? d->font->face : (d && dcfont(d) == FONT_MONO ? "Courier New" : "Tahoma"); if (!out) return strlen(f) + 1; strlcpy(out, f, n); return strlen(out); }
/* GetObject for every kind of object */
static int get_object(HGDIOBJ h, int n, void *out, bool wide) {
    gdiobj_t *o = OBJ(h); if (!o) return 0;
    switch (o->type) {
    case OBJ_BITMAP: {
        int need = o->dib ? (sizeof(uintptr_t) == 8 ? 104 : 84) : (sizeof(uintptr_t) == 8 ? 32 : 24);
        if (!out) return need;
        if (n < (sizeof(uintptr_t) == 8 ? 32 : 24)) return 0;
        uint8_t *b = out; memset(b, 0, MIN(n, need));
        int bpp = o->dib ? o->dib->bpp : 32;
        *(LONG *)(b + 4) = o->bmp->w; *(LONG *)(b + 8) = o->bmp->h; *(LONG *)(b + 12) = o->dib ? o->dib->stride : o->bmp->w * 4;
        *(WORD *)(b + 16) = 1; *(WORD *)(b + 18) = bpp;
        if (o->dib) {
            *(void **)(b + (sizeof(uintptr_t) == 8 ? 24 : 20)) = o->dib->bits;
            if (n >= need) {
                BIH *bi = (BIH *)(b + (sizeof(uintptr_t) == 8 ? 32 : 24));
                bi->size = 40; bi->w = o->dib->w; bi->h = o->dib->bottomup ? o->dib->h : -o->dib->h; bi->planes = 1; bi->bpp = bpp; bi->comp = o->dib->comp;
                bi->img = o->dib->stride * o->dib->h;
                memcpy((uint8_t *)bi + 40, o->dib->masks, 12);
                return need;
            }
        }
        return sizeof(uintptr_t) == 8 ? 32 : 24; }
    case OBJ_PEN: { if (!out) return 16; if (n < 16) return 0; LONG *p = out; p[0] = o->null ? 5 : o->style; p[1] = o->width; p[2] = 0; p[3] = colref(o->color); return 16; }
    case OBJ_BRUSH: { if (!out) return 12; if (n < 12) return 0; LONG *p = out; p[0] = o->null ? 1 : 0; p[1] = colref(o->color); p[2] = 0; return 12; }
    case OBJ_FONT: {
        int need = wide ? 92 : 60; if (!out) return need; if (n < 28) return 0;
        uint8_t *b = out; memset(b, 0, MIN(n, need));
        LONG fh = o->font_h ? o->font_h : -font_height(o->font);
        *(LONG *)b = fh; *(LONG *)(b + 16) = o->font_weight ? o->font_weight : 400; b[20] = o->font_italic; b[21] = o->font_underline; b[23] = 1; b[27] = o->font == FONT_MONO ? 0x31 : 0x22;
        const char *face = o->face[0] ? o->face : o->font == FONT_MONO ? "Courier New" : "Tahoma";
        if (n >= need) { if (wide) um_a2w(face, (WCHAR *)(b + 28), 32); else strlcpy((char *)(b + 28), face, 32); }
        return MIN(n, need); }
    case OBJ_PALETTE: { if (!out) return 2; if (n >= 2) *(WORD *)out = 256; return 2; }
    }
    return 0;
}
static int WINAPI GetObjectA2(HGDIOBJ h, int n, void *out) { return get_object(h, n, out, false); }
static int WINAPI GetObjectW(HGDIOBJ h, int n, void *out) { return get_object(h, n, out, true); }
static DWORD WINAPI GetObjectType(HGDIOBJ h) {
    if (DC(h)) { dc_t *d = DC(h); return d->memdc ? 10 : 3; }
    gdiobj_t *o = OBJ(h); if (!o) return 0;
    static const DWORD t[] = { 0, 1, 2, 6, 7, 8, 5 };
    return o->type < 7 ? t[o->type] : 0;
}
typedef struct { UINT style; LONG wx, wy; COLORREF color; } LOGPEN;
static HGDIOBJ WINAPI CreatePenIndirect(const LOGPEN *l) { return CreatePen(l->style, l->wx, l->color); }
static HGDIOBJ WINAPI ExtCreatePen(DWORD style, DWORD w, const LOGBRUSH *b, DWORD n, const DWORD *st) { return CreatePen(style & 0xF, w, b ? b->color : 0); }
static HGDIOBJ WINAPI CreatePatternBrush(HGDIOBJ hb) {
    gdiobj_t *b = OBJ(hb); uint32_t avg = T.face;
    if (b && b->bmp && b->bmp->w && b->bmp->h) {                  /* the pattern's average colour */
        if (b->dib) dib_in(b);
        uint64_t r = 0, g = 0, bl = 0; int n = 0;
        for (int y = 0; y < b->bmp->h && y < 8; y++) for (int x = 0; x < b->bmp->w && x < 8; x++) { uint32_t p = b->bmp->px[y * b->bmp->stride + x]; r += (p >> 16) & 255; g += (p >> 8) & 255; bl += p & 255; n++; }
        if (n) avg = rgb_of(r / n, g / n, bl / n);
    }
    gdiobj_t *o = newobj(OBJ_BRUSH); o->color = avg; return (HGDIOBJ)o;
}
static HGDIOBJ WINAPI CreateDIBPatternBrushPt(const void *dib, UINT usage) { canvas_t *c = dib_decode(dib, NULL, usage); uint32_t col = c ? c->px[0] : T.face; if (c) canvas_free(c); gdiobj_t *o = newobj(OBJ_BRUSH); o->color = col; return (HGDIOBJ)o; }
static COLORREF dc_brush_col = 0xFFFFFF, dc_pen_col = 0;
static COLORREF WINAPI SetDCBrushColor(HDC h, COLORREF c) { COLORREF o = dc_brush_col; dc_brush_col = c; stock[18].color = cref(c); return o; }
static COLORREF WINAPI SetDCPenColor(HDC h, COLORREF c) { COLORREF o = dc_pen_col; dc_pen_col = c; stock[7].color = cref(c); return o; }
static COLORREF WINAPI GetDCBrushColor(HDC h) { return dc_brush_col; }
static COLORREF WINAPI GetDCPenColor(HDC h) { return dc_pen_col; }
static COLORREF WINAPI GetNearestColor(HDC h, COLORREF c) { return c; }
static BOOL WINAPI GdiFlush(void) { return 1; }
static DWORD WINAPI GdiSetBatchLimit(DWORD n) { return 1; }
/* palettes: true colour, so they only hold entries */
static HGDIOBJ WINAPI CreateHalftonePalette(HDC h) { gdiobj_t *o = newobj(OBJ_PALETTE); return (HGDIOBJ)o; }
static BOOL WINAPI ResizePalette(HGDIOBJ p, UINT n) { return 1; }
static UINT WINAPI GetPaletteEntries(HGDIOBJ p, UINT s, UINT n, void *e) { if (e) memset(e, 0, n * 4); return n; }
static UINT WINAPI GetSystemPaletteEntries(HDC h, UINT s, UINT n, void *e) { return 0; }
static UINT WINAPI GetSystemPaletteUse(HDC h) { return 1; }
static BOOL WINAPI AnimatePalette(HGDIOBJ p, UINT s, UINT n, const void *e) { return 1; }

/* ================================================================ regions (bounding rectangles) */
typedef struct rgn { int l, t, r, b; bool complex; } rgn_t;
static gdiobj_t *new_rgn(int l, int t, int r, int b) {
    gdiobj_t *o = newobj(OBJ_REGION); o->rgn = kzalloc(sizeof(rgn_t));
    if (r < l) { int x = l; l = r; r = x; } if (b < t) { int x = t; t = b; b = x; }
    o->rgn->l = l; o->rgn->t = t; o->rgn->r = r; o->rgn->b = b;
    return o;
}
static rgn_t *RGN(HGDIOBJ h) { gdiobj_t *o = OBJ(h); return o && o->type == OBJ_REGION ? o->rgn : NULL; }
static int rgn_kind(const rgn_t *g) { return g->r <= g->l || g->b <= g->t ? 1 : g->complex ? 3 : 2; }
static HGDIOBJ WINAPI CreateRectRgn(int l, int t, int r, int b) { return (HGDIOBJ)new_rgn(l, t, r, b); }
static HGDIOBJ WINAPI CreateRectRgnIndirect(const RECT32 *r) { return CreateRectRgn(r->l, r->t, r->r, r->b); }
static HGDIOBJ WINAPI CreateEllipticRgn(int l, int t, int r, int b) { gdiobj_t *o = new_rgn(l, t, r, b); o->rgn->complex = true; return (HGDIOBJ)o; }
static HGDIOBJ WINAPI CreateEllipticRgnIndirect(const RECT32 *r) { return CreateEllipticRgn(r->l, r->t, r->r, r->b); }
static HGDIOBJ WINAPI CreateRoundRectRgn(int l, int t, int r, int b, int w, int h) { gdiobj_t *o = new_rgn(l, t, r, b); o->rgn->complex = true; return (HGDIOBJ)o; }
static HGDIOBJ WINAPI CreatePolygonRgn(const LONG *pts, int n, int mode) {
    if (n < 1) return CreateRectRgn(0, 0, 0, 0);
    int l = pts[0], t = pts[1], r = pts[0], b = pts[1];
    for (int i = 1; i < n; i++) { l = MIN(l, pts[2 * i]); r = MAX(r, pts[2 * i]); t = MIN(t, pts[2 * i + 1]); b = MAX(b, pts[2 * i + 1]); }
    gdiobj_t *o = new_rgn(l, t, r, b); o->rgn->complex = n != 4; return (HGDIOBJ)o;
}
static BOOL WINAPI SetRectRgn(HGDIOBJ h, int l, int t, int r, int b) { rgn_t *g = RGN(h); if (!g) return 0; g->l = l; g->t = t; g->r = r; g->b = b; g->complex = false; return 1; }
static int WINAPI CombineRgn(HGDIOBJ dst, HGDIOBJ a, HGDIOBJ b, int mode) {
    rgn_t *d = RGN(dst), *x = RGN(a), *y = RGN(b); if (!d || !x) return 0;
    rgn_t r = *x;
    switch (mode) {
    case 1: if (!y) return 0; r.l = MAX(x->l, y->l); r.t = MAX(x->t, y->t); r.r = MIN(x->r, y->r); r.b = MIN(x->b, y->b); if (r.r <= r.l || r.b <= r.t) r.l = r.t = r.r = r.b = 0; r.complex = x->complex || y->complex; break;   /* RGN_AND */
    case 2: case 3: if (!y) return 0;                                       /* RGN_OR, RGN_XOR: the bounding box */
        if (rgn_kind(y) == 1) break; if (rgn_kind(x) == 1) { r = *y; break; }
        r.l = MIN(x->l, y->l); r.t = MIN(x->t, y->t); r.r = MAX(x->r, y->r); r.b = MAX(x->b, y->b); r.complex = true; break;
    case 4: if (!y) return 0;                                               /* RGN_DIFF */
        if (y->l <= x->l && y->r >= x->r && y->t <= x->t && y->b >= x->b) r.l = r.t = r.r = r.b = 0;
        else if (y->l <= x->l && y->r >= x->r && y->t <= x->t && y->b > x->t) r.t = y->b;      /* a band off the top */
        else if (y->l <= x->l && y->r >= x->r && y->b >= x->b && y->t < x->b) r.b = y->t;      /* ... the bottom */
        else if (y->t <= x->t && y->b >= x->b && y->l <= x->l && y->r > x->l) r.l = y->r;
        else if (y->t <= x->t && y->b >= x->b && y->r >= x->r && y->l < x->r) r.r = y->l;
        else if (y->r > x->l && y->l < x->r && y->b > x->t && y->t < x->b) r.complex = true;
        break;
    case 5: break;                                                          /* RGN_COPY */
    default: return 0;
    }
    *d = r;
    return rgn_kind(d);
}
static int WINAPI OffsetRgn(HGDIOBJ h, int dx, int dy) { rgn_t *g = RGN(h); if (!g) return 0; g->l += dx; g->r += dx; g->t += dy; g->b += dy; return rgn_kind(g); }
static int WINAPI GetRgnBox(HGDIOBJ h, RECT32 *r) { rgn_t *g = RGN(h); if (!g) return 0; r->l = g->l; r->t = g->t; r->r = g->r; r->b = g->b; return rgn_kind(g); }
static BOOL WINAPI PtInRegion(HGDIOBJ h, int x, int y) { rgn_t *g = RGN(h); return g && x >= g->l && x < g->r && y >= g->t && y < g->b; }
static BOOL WINAPI RectInRegion(HGDIOBJ h, const RECT32 *r) { rgn_t *g = RGN(h); return g && r->l < g->r && r->r > g->l && r->t < g->b && r->b > g->t; }
static BOOL WINAPI EqualRgn(HGDIOBJ a, HGDIOBJ b) { rgn_t *x = RGN(a), *y = RGN(b); return x && y && x->l == y->l && x->t == y->t && x->r == y->r && x->b == y->b; }
static BOOL WINAPI FillRgn(HDC h, HGDIOBJ rg, HGDIOBJ br) { rgn_t *g = RGN(rg); if (!g) return 0; RECT32 r = { g->l, g->t, g->r, g->b }; return FillRect(h, &r, br); }
static BOOL WINAPI PaintRgn(HDC h, HGDIOBJ rg) { dc_t *d = DC(h); if (!d) return 0; return FillRgn(h, rg, (HGDIOBJ)d->brush); }
static BOOL WINAPI FrameRgn(HDC h, HGDIOBJ rg, HGDIOBJ br, int w, int ht) { rgn_t *g = RGN(rg); if (!g) return 0; RECT32 r = { g->l, g->t, g->r, g->b }; return FrameRect(h, &r, br); }
static BOOL WINAPI InvertRgn(HDC h, HGDIOBJ rg) { rgn_t *g = RGN(rg); if (!g) return 0; RECT32 r = { g->l, g->t, g->r, g->b }; return InvertRect(h, &r); }
static DWORD WINAPI GetRegionData(HGDIOBJ h, DWORD n, void *out) {
    rgn_t *g = RGN(h); if (!g) return 0;
    DWORD need = 32 + (rgn_kind(g) == 1 ? 0 : 16);
    if (!out || n < need) return need;
    DWORD *d = out; d[0] = 32; d[1] = 1; d[2] = rgn_kind(g) == 1 ? 0 : 1; d[3] = d[2] * 16;
    LONG *b = (LONG *)(d + 4); b[0] = g->l; b[1] = g->t; b[2] = g->r; b[3] = g->b;
    if (d[2]) memcpy(b + 4, b, 16);
    return need;
}
static HGDIOBJ WINAPI ExtCreateRegion(const void *xf, DWORD n, const DWORD *data) {
    if (!data || data[2] == 0) return CreateRectRgn(0, 0, 0, 0);
    const LONG *b = (const LONG *)(data + 4); gdiobj_t *o = new_rgn(b[0], b[1], b[2], b[3]); o->rgn->complex = data[2] > 1; return (HGDIOBJ)o;
}

/* ================================================================ DC state: origins, clipping, SaveDC */
typedef struct { dc_t *dc; int vpx, vpy, wx, wy, vex, vey, wex, wey, mapmode, stretch, brx, bry, depth;
                 struct { gdiobj_t *pen, *brush, *font, *bitmap; uint32_t text, bk; int bkmode, ox, oy, vpx, vpy, wx, wy; rect_t clip; uint32_t align; } saved[16]; } dcx_t;
static dcx_t dcx[64];
static dcx_t *DCX(dc_t *d, bool create) {
    for (int i = 0; i < 64; i++) if (dcx[i].dc == d) return &dcx[i];
    if (!create) return NULL;
    for (int i = 0; i < 64; i++) if (!dcx[i].dc) { memset(&dcx[i], 0, sizeof dcx[i]); dcx[i].dc = d; dcx[i].mapmode = 1; dcx[i].stretch = 1; dcx[i].vex = dcx[i].vey = dcx[i].wex = dcx[i].wey = 1; return &dcx[i]; }
    static int rr; dcx_t *x = &dcx[rr++ & 63]; memset(x, 0, sizeof *x); x->dc = d; x->mapmode = 1; x->stretch = 1; return x;
}
static void dcx_forget(dc_t *d) { for (int i = 0; i < 64; i++) if (dcx[i].dc == d) dcx[i].dc = NULL; }
static BOOL WINAPI ReleaseDC2(HWND h, HDC hdc) { dc_t *d = DC(hdc); if (d) dcx_forget(d); return ReleaseDC(h, hdc); }
static BOOL WINAPI DeleteDC2(HDC hdc) { dc_t *d = DC(hdc); if (d) dcx_forget(d); return DeleteDC(hdc); }
/* the device origin d->ox/oy already includes the window's place; viewport and window origins move it */
static BOOL WINAPI SetViewportOrgEx(HDC h, int x, int y, LONG *old) {
    dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true);
    if (old) { old[0] = e->vpx; old[1] = e->vpy; }
    d->ox += x - e->vpx; d->oy += y - e->vpy; e->vpx = x; e->vpy = y;
    return 1;
}
static BOOL WINAPI OffsetViewportOrgEx(HDC h, int dx, int dy, LONG *old) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true); return SetViewportOrgEx(h, e->vpx + dx, e->vpy + dy, old); }
static BOOL WINAPI GetViewportOrgEx(HDC h, LONG *p) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, false); p[0] = e ? e->vpx : 0; p[1] = e ? e->vpy : 0; return 1; }
static BOOL WINAPI SetWindowOrgEx(HDC h, int x, int y, LONG *old) {
    dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true);
    if (old) { old[0] = e->wx; old[1] = e->wy; }
    d->ox -= x - e->wx; d->oy -= y - e->wy; e->wx = x; e->wy = y;
    return 1;
}
static BOOL WINAPI OffsetWindowOrgEx(HDC h, int dx, int dy, LONG *old) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true); return SetWindowOrgEx(h, e->wx + dx, e->wy + dy, old); }
static BOOL WINAPI GetWindowOrgEx(HDC h, LONG *p) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, false); p[0] = e ? e->wx : 0; p[1] = e ? e->wy : 0; return 1; }
static BOOL WINAPI SetViewportExtEx(HDC h, int x, int y, LONG *old) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true); if (old) { old[0] = e->vex; old[1] = e->vey; } e->vex = x; e->vey = y; return 1; }
static BOOL WINAPI SetWindowExtEx(HDC h, int x, int y, LONG *old) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true); if (old) { old[0] = e->wex; old[1] = e->wey; } e->wex = x; e->wey = y; return 1; }
static BOOL WINAPI GetViewportExtEx(HDC h, LONG *p) { dc_t *d = DC(h); dcx_t *e = d ? DCX(d, false) : NULL; p[0] = e ? e->vex : 1; p[1] = e ? e->vey : 1; return 1; }
static BOOL WINAPI GetWindowExtEx(HDC h, LONG *p) { dc_t *d = DC(h); dcx_t *e = d ? DCX(d, false) : NULL; p[0] = e ? e->wex : 1; p[1] = e ? e->wey : 1; return 1; }
static BOOL WINAPI ScaleViewportExtEx(HDC h, int a, int b, int c, int dd, LONG *old) { return SetViewportExtEx(h, 1, 1, old); }
static BOOL WINAPI ScaleWindowExtEx(HDC h, int a, int b, int c, int dd, LONG *old) { return SetWindowExtEx(h, 1, 1, old); }
static int WINAPI SetMapMode2(HDC h, int m) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true); int o = e->mapmode; e->mapmode = m; return o; }
static int WINAPI GetMapMode(HDC h) { dc_t *d = DC(h); dcx_t *e = d ? DCX(d, false) : NULL; return e ? e->mapmode : 1; }
static BOOL WINAPI DPtoLP(HDC h, LONG *p, int n) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, false); if (!e) return 1; for (int i = 0; i < n; i++) { p[2 * i] += e->wx - e->vpx; p[2 * i + 1] += e->wy - e->vpy; } return 1; }
static BOOL WINAPI LPtoDP(HDC h, LONG *p, int n) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, false); if (!e) return 1; for (int i = 0; i < n; i++) { p[2 * i] += e->vpx - e->wx; p[2 * i + 1] += e->vpy - e->wy; } return 1; }
static int WINAPI SetStretchBltMode(HDC h, int m) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true); int o = e->stretch; e->stretch = m; return o; }
static int WINAPI GetStretchBltMode(HDC h) { dc_t *d = DC(h); dcx_t *e = d ? DCX(d, false) : NULL; return e ? e->stretch : 1; }
static BOOL WINAPI SetBrushOrgEx(HDC h, int x, int y, LONG *old) { dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true); if (old) { old[0] = e->brx; old[1] = e->bry; } e->brx = x; e->bry = y; return 1; }
static BOOL WINAPI GetBrushOrgEx(HDC h, LONG *p) { dc_t *d = DC(h); dcx_t *e = d ? DCX(d, false) : NULL; p[0] = e ? e->brx : 0; p[1] = e ? e->bry : 0; return 1; }
static UINT WINAPI GetTextAlign(HDC h) { dc_t *d = DC(h); return d ? d->align : 0; }
static BOOL WINAPI GetCurrentPositionEx(HDC h, LONG *p) { dc_t *d = DC(h); if (!d) return 0; p[0] = d->cx; p[1] = d->cy; return 1; }
static int WINAPI GetROP2(HDC h) { return 13; }
static int WINAPI GetPolyFillMode(HDC h) { return 1; }
static int WINAPI SetPolyFillMode(HDC h, int m) { return 1; }
static int WINAPI SetArcDirection(HDC h, int d) { return 1; }
static int WINAPI GetArcDirection(HDC h) { return 1; }
static int WINAPI SetGraphicsMode(HDC h, int m) { return 1; }
static int WINAPI GetGraphicsMode(HDC h) { return 1; }
static BOOL WINAPI SetWorldTransform(HDC h, const void *x) { return 0; }
static BOOL WINAPI ModifyWorldTransform(HDC h, const void *x, DWORD m) { return m == 1; }
static BOOL WINAPI GetWorldTransform(HDC h, float *x) { x[0] = 1; x[1] = 0; x[2] = 0; x[3] = 1; x[4] = 0; x[5] = 0; return 1; }
static int WINAPI SetTextCharacterExtra(HDC h, int e) { return 0; }
static int WINAPI GetTextCharacterExtra(HDC h) { return 0; }
static BOOL WINAPI SetTextJustification(HDC h, int e, int n) { return 1; }
static DWORD WINAPI SetLayout(HDC h, DWORD l) { return 0; }
static DWORD WINAPI GetLayout(HDC h) { return 0; }
/* the clip: one rectangle, in device pixels of the DC's canvas */
static rect_t default_clip(dc_t *d) {
    if (d->memdc) return R(0, 0, d->surf->w, d->surf->h);
    if (d->win && d->win->parent) { int ox, oy; abs_origin(d->win, &ox, &oy); return R(ox, oy, d->win->w, d->win->h); }
    return R(0, 0, d->surf->w, d->surf->h);
}
static int WINAPI SelectClipRgn2(HDC h, HGDIOBJ rg) {
    dc_t *d = DC(h); if (!d) return 0;
    rgn_t *g = RGN(rg);
    rect_t base = default_clip(d);
    if (!g) { d->clip = base; return 2; }
    dcx_t *e = DCX(d, false); int vx = e ? e->vpx : 0, vy = e ? e->vpy : 0;       /* region coordinates are device units */
    d->clip = rect_intersect(base, R(d->ox - vx + (e ? e->wx : 0) + g->l - (e ? e->wx : 0), d->oy - vy + g->t, g->r - g->l, g->b - g->t));
    (void)vy;
    return d->clip.w > 0 && d->clip.h > 0 ? 2 : 1;
}
static int WINAPI ExtSelectClipRgn(HDC h, HGDIOBJ rg, int mode) {
    dc_t *d = DC(h); if (!d) return 0;
    if (mode == 5 || !rg) return SelectClipRgn2(h, rg);
    rect_t old = d->clip; SelectClipRgn2(h, rg);
    if (mode == 1) d->clip = rect_intersect(old, d->clip);                          /* RGN_AND */
    else if (mode == 2) { int l = MIN(old.x, d->clip.x), t = MIN(old.y, d->clip.y), r = MAX(old.x + old.w, d->clip.x + d->clip.w), b = MAX(old.y + old.h, d->clip.y + d->clip.h); d->clip = R(l, t, r - l, b - t); }
    else if (mode == 4) d->clip = old;
    return 2;
}
static int WINAPI IntersectClipRect2(HDC h, int l, int t, int r, int b) { dc_t *d = DC(h); if (!d) return 0; d->clip = rect_intersect(d->clip, R(d->ox + l, d->oy + t, r - l, b - t)); return d->clip.w > 0 && d->clip.h > 0 ? 2 : 1; }
static int WINAPI GetClipBox(HDC h, RECT32 *r) {
    dc_t *d = DC(h); if (!d) return 0;
    r->l = d->clip.x - d->ox; r->t = d->clip.y - d->oy; r->r = r->l + d->clip.w; r->b = r->t + d->clip.h;
    return d->clip.w > 0 && d->clip.h > 0 ? 2 : 1;
}
static int WINAPI GetClipRgn(HDC h, HGDIOBJ rg) {
    dc_t *d = DC(h); rgn_t *g = RGN(rg); if (!d || !g) return -1;
    rect_t b = default_clip(d);
    if (d->clip.x == b.x && d->clip.y == b.y && d->clip.w == b.w && d->clip.h == b.h) return 0;
    g->l = d->clip.x - d->ox; g->t = d->clip.y - d->oy; g->r = g->l + d->clip.w; g->b = g->t + d->clip.h; g->complex = false;
    return 1;
}
static int WINAPI GetRandomRgn(HDC h, HGDIOBJ rg, int n) {
    dc_t *d = DC(h); rgn_t *g = RGN(rg); if (!d || !g) return -1;
    int ox = 0, oy = 0; if (d->win && !d->memdc) { w32win_t *t = top_of(d->win); if (t->top) { rect_t cr = wm_client_rect(t->top); ox = t->top->x + cr.x; oy = t->top->y + cr.y; } }
    g->l = d->clip.x + ox; g->t = d->clip.y + oy; g->r = g->l + d->clip.w; g->b = g->t + d->clip.h; g->complex = false;
    return 1;
}
static BOOL WINAPI PtVisible(HDC h, int x, int y) { dc_t *d = DC(h); if (!d) return 0; int px = d->ox + x, py = d->oy + y; return px >= d->clip.x && px < d->clip.x + d->clip.w && py >= d->clip.y && py < d->clip.y + d->clip.h; }
static BOOL WINAPI RectVisible(HDC h, const RECT32 *r) { dc_t *d = DC(h); if (!d) return 0; rect_t x = rect_intersect(d->clip, R(d->ox + r->l, d->oy + r->t, r->r - r->l, r->b - r->t)); return x.w > 0 && x.h > 0; }
static int WINAPI SaveDC(HDC h) {
    dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, true);
    if (e->depth >= 16) return 0;
    typeof(e->saved[0]) *s = &e->saved[e->depth++];
    s->pen = d->pen; s->brush = d->brush; s->font = d->font; s->bitmap = d->bitmap; s->text = d->text; s->bk = d->bk; s->bkmode = d->bkmode;
    s->ox = d->ox; s->oy = d->oy; s->vpx = e->vpx; s->vpy = e->vpy; s->wx = e->wx; s->wy = e->wy; s->clip = d->clip; s->align = d->align;
    return e->depth;
}
static BOOL WINAPI RestoreDC(HDC h, int n) {
    dc_t *d = DC(h); if (!d) return 0; dcx_t *e = DCX(d, false); if (!e || !e->depth) return 0;
    int to = n < 0 ? e->depth + n : n - 1;
    if (to < 0 || to >= e->depth) return 0;
    typeof(e->saved[0]) *s = &e->saved[to];
    d->pen = s->pen; d->brush = s->brush; d->font = s->font; d->text = s->text; d->bk = s->bk; d->bkmode = s->bkmode;
    if (s->bitmap != d->bitmap && s->bitmap) SelectObject(h, (HGDIOBJ)s->bitmap);
    d->ox = s->ox; d->oy = s->oy; e->vpx = s->vpx; e->vpy = s->vpy; e->wx = s->wx; e->wy = s->wy; d->clip = s->clip; d->align = s->align;
    e->depth = to;
    return 1;
}
/* SelectObject also takes regions (the clip) */
/* a memory DC starts with a 1x1 stock bitmap selected, so selecting a bitmap returns it */
static gdiobj_t *stock_bitmap(void) {
    if (!default_bitmap.magic) { default_bitmap.magic = OBJMAGIC; default_bitmap.type = OBJ_BITMAP; default_bitmap.stock = true; default_bitmap.bmp = canvas_new(1, 1); }
    return &default_bitmap;
}
static HGDIOBJ WINAPI SelectObject2(HDC h, HGDIOBJ obj) {
    gdiobj_t *o = OBJ(obj);
    if (o && o->type == OBJ_REGION) return (HGDIOBJ)(uintptr_t)SelectClipRgn2(h, obj);
    if (o && o->type == OBJ_PALETTE) return obj;
    dc_t *d = DC(h);
    if (o == &default_bitmap && d) { gdiobj_t *old = d->bitmap; d->bitmap = NULL; d->surf = default_bitmap.bmp; d->clip = R(0, 0, 1, 1); return (HGDIOBJ)(old ? old : stock_bitmap()); }
    HGDIOBJ r = SelectObject(h, obj);
    if (!r && o && o->type == OBJ_BITMAP && d && d->memdc) r = (HGDIOBJ)stock_bitmap();
    return r;
}
static BOOL WINAPI DeleteObject2(HGDIOBJ h) {
    gdiobj_t *o = OBJ(h); if (!o || o->stock) return 1;
    if (o->dib) { if (o->dib->own) w32_free(P, o->dib->bits); kfree(o->dib); o->dib = NULL; }
    if (o->rgn) { kfree(o->rgn); o->rgn = NULL; }
    return DeleteObject(h);
}

/* ================================================================ curves and fills */
static void ellipse_pt(int l, int t, int r, int b, double ang, int *x, int *y) {
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0, a = (r - l) / 2.0, bb = (b - t) / 2.0;
    /* the small-angle sine and cosine by the kernel's helpers would do; this is precise enough */
    double s = 0, c = 1, term; double x2 = ang;
    while (x2 > 3.14159265358979) x2 -= 6.28318530717959; while (x2 < -3.14159265358979) x2 += 6.28318530717959;
    term = x2; s = x2; for (int i = 1; i < 12; i++) { term *= -x2 * x2 / ((2 * i) * (2 * i + 1)); s += term; }
    term = 1; c = 1; for (int i = 1; i < 12; i++) { term *= -x2 * x2 / ((2 * i - 1) * (2 * i)); c += term; }
    *x = (int)(cx + a * c + (c >= 0 ? 0.5 : -0.5)); *y = (int)(cy - bb * s + (s >= 0 ? -0.5 : 0.5));
}
static double angle_of(int l, int t, int r, int b, int x, int y) {
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0, a = (r - l) / 2.0, bb = (b - t) / 2.0;
    double dx = (x - cx) / (a ? a : 1), dy = (cy - y) / (bb ? bb : 1);
    /* atan2 by series on the octant */
    double ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, z = ax > ay ? ay / (ax ? ax : 1) : ax / (ay ? ay : 1);
    double at = z - z * z * z / 3 + z * z * z * z * z / 5 - z * z * z * z * z * z * z / 7;
    at = z * (0.9998660 + z * z * (-0.3302995 + z * z * (0.1801410 + z * z * (-0.0851330 + z * z * 0.0208351))));
    if (ay > ax) at = 1.5707963267949 - at;
    if (dx < 0) at = 3.14159265358979 - at;
    if (dy < 0) at = -at;
    return at;
}
/* points along the arc, counter-clockwise from the start to the end radial */
static int arc_points(int l, int t, int r, int b, int xs, int ys, int xe, int ye, LONG *pts, int max) {
    double a0 = angle_of(l, t, r, b, xs, ys), a1 = angle_of(l, t, r, b, xe, ye);
    if (a1 <= a0) a1 += 6.28318530717959;
    int n = MIN(max - 1, MAX(4, (int)((a1 - a0) * MAX(r - l, b - t) / 8)));
    for (int i = 0; i <= n; i++) { int x, y; ellipse_pt(l, t, r, b, a0 + (a1 - a0) * i / n, &x, &y); pts[2 * i] = x; pts[2 * i + 1] = y; }
    return n + 1;
}
static BOOL WINAPI Arc(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { LONG p[2 * 362]; int n = arc_points(l, t, r, b, xs, ys, xe, ye, p, 362); return Polyline(h, p, n); }
static BOOL WINAPI ArcTo(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye) {
    dc_t *d = DC(h); if (!d) return 0; LONG p[2 * 362]; int n = arc_points(l, t, r, b, xs, ys, xe, ye, p, 362);
    LineTo(h, p[0], p[1]); Polyline(h, p, n); d->cx = p[2 * n - 2]; d->cy = p[2 * n - 1]; return 1;
}
static BOOL WINAPI Pie(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { LONG p[2 * 364]; int n = arc_points(l, t, r, b, xs, ys, xe, ye, p, 362); p[2 * n] = (l + r) / 2; p[2 * n + 1] = (t + b) / 2; return Polygon(h, p, n + 1); }
static BOOL WINAPI Chord(HDC h, int l, int t, int r, int b, int xs, int ys, int xe, int ye) { LONG p[2 * 362]; int n = arc_points(l, t, r, b, xs, ys, xe, ye, p, 362); return Polygon(h, p, n); }
static BOOL WINAPI AngleArc(HDC h, int x, int y, DWORD rad, float start, float sweep) {
    dc_t *d = DC(h); if (!d) return 0;
    int n = MAX(4, (int)((sweep < 0 ? -sweep : sweep) * rad / 400)); if (n > 360) n = 360;
    LONG p[2 * 362];
    for (int i = 0; i <= n; i++) { int px, py; ellipse_pt(x - rad, y - rad, x + rad, y + rad, (start + sweep * i / n) * 3.14159265358979 / 180, &px, &py); p[2 * i] = px; p[2 * i + 1] = py; }
    LineTo(h, p[0], p[1]); Polyline(h, p, n + 1); d->cx = p[2 * n]; d->cy = p[2 * n + 1];
    return 1;
}
static int bezier_flat(const LONG *pts, int n, LONG *out, int max) {
    int k = 0;
    if (n < 1) return 0;
    out[k++] = pts[0]; out[k++] = pts[1];
    for (int i = 0; i + 3 < n + 0 && i + 3 <= n - 1; i += 3) {
        const LONG *p = pts + 2 * i;
        for (int s = 1; s <= 16 && k + 2 <= max * 2; s++) {
            double t = s / 16.0, u = 1 - t, a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
            out[k++] = (LONG)(a * p[0] + b * p[2] + c * p[4] + d * p[6] + 0.5); out[k++] = (LONG)(a * p[1] + b * p[3] + c * p[5] + d * p[7] + 0.5);
        }
    }
    return k / 2;
}
static BOOL WINAPI PolyBezier(HDC h, const LONG *pts, DWORD n) { LONG *o = kmalloc(sizeof(LONG) * 2 * (n / 3 * 16 + 2)); int k = bezier_flat(pts, n, o, n / 3 * 16 + 2); BOOL r = Polyline(h, o, k); kfree(o); return r; }
static BOOL WINAPI PolyBezierTo(HDC h, const LONG *pts, DWORD n) {
    dc_t *d = DC(h); if (!d || n < 3) return 0;
    LONG *all = kmalloc(sizeof(LONG) * 2 * (n + 1)); all[0] = d->cx; all[1] = d->cy; memcpy(all + 2, pts, sizeof(LONG) * 2 * n);
    BOOL r = PolyBezier(h, all, n + 1); d->cx = pts[2 * n - 2]; d->cy = pts[2 * n - 1]; kfree(all); return r;
}
static BOOL WINAPI PolylineTo(HDC h, const LONG *pts, DWORD n) { dc_t *d = DC(h); if (!d) return 0; for (DWORD i = 0; i < n; i++) LineTo(h, pts[2 * i], pts[2 * i + 1]); return 1; }
static BOOL WINAPI PolyPolyline(HDC h, const LONG *pts, const DWORD *cnt, DWORD n) { for (DWORD i = 0; i < n; i++) { Polyline(h, pts, cnt[i]); pts += 2 * cnt[i]; } return 1; }
static BOOL WINAPI PolyPolygon(HDC h, const LONG *pts, const int *cnt, int n) { for (int i = 0; i < n; i++) { Polygon(h, pts, cnt[i]); pts += 2 * cnt[i]; } return 1; }
/* flood fill: FLOODFILLBORDER (0) stops at the colour, FLOODFILLSURFACE (1) fills where it is */
static BOOL WINAPI ExtFloodFill(HDC h, int x, int y, COLORREF col, UINT type) {
    dc_t *d = DC(h); if (!d || !d->brush || d->brush->null) return 0;
    rect_t sv; dc_begin(d, &sv);
    canvas_t *c = d->surf; rect_t cl = c->clip;
    int sx = d->ox + x, sy = d->oy + y;
    uint32_t ref = cref(col) & 0xFFFFFF, fill = d->brush->color;
    #define INSIDE(px) (type ? ((px) & 0xFFFFFF) == ref : ((px) & 0xFFFFFF) != ref)
    if (sx < cl.x || sy < cl.y || sx >= cl.x + cl.w || sy >= cl.y + cl.h || !INSIDE(c->px[sy * c->stride + sx]) || (c->px[sy * c->stride + sx] & 0xFFFFFF) == (fill & 0xFFFFFF)) { dc_end(d, sv); return 0; }
    int cap = 65536, n = 0; int *st = kmalloc(sizeof(int) * 2 * cap);
    st[n++] = sx; st[n++] = sy;
    while (n) {
        int py = st[--n], px = st[--n];
        int l = px, r = px;
        while (l > cl.x && INSIDE(c->px[py * c->stride + l - 1]) && c->px[py * c->stride + l - 1] != fill) l--;
        while (r + 1 < cl.x + cl.w && INSIDE(c->px[py * c->stride + r + 1]) && c->px[py * c->stride + r + 1] != fill) r++;
        for (int i = l; i <= r; i++) c->px[py * c->stride + i] = fill;
        for (int dy = -1; dy <= 1; dy += 2) {
            int ny = py + dy; if (ny < cl.y || ny >= cl.y + cl.h) continue;
            for (int i = l; i <= r; i++) if (INSIDE(c->px[ny * c->stride + i]) && c->px[ny * c->stride + i] != fill && (i == l || !INSIDE(c->px[ny * c->stride + i - 1]) || c->px[ny * c->stride + i - 1] == fill)) { if (n + 2 < 2 * cap) { st[n++] = i; st[n++] = ny; } }
        }
    }
    #undef INSIDE
    kfree(st);
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI FloodFill(HDC h, int x, int y, COLORREF c) { return ExtFloodFill(h, x, y, c, 0); }
static BOOL WINAPI MaskBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, HGDIOBJ mask, int mx, int my, DWORD rop) { return BitBlt(dst, x, y, w, h, src, sx, sy, rop & 0xFFFFFF); }
static BOOL WINAPI PlgBlt(HDC dst, const LONG *pts, HDC src, int sx, int sy, int w, int h, HGDIOBJ m, int mx, int my) { return BitBlt(dst, pts[0], pts[1], w, h, src, sx, sy, 0xCC0020); }
/* GetGlyphOutline and friends: no outlines, so callers fall back */
static DWORD WINAPI GetGlyphOutlineW(HDC h, UINT c, UINT f, void *gm, DWORD n, void *b, const void *m) { return 0xFFFFFFFF; }
static DWORD WINAPI GetFontData(HDC h, DWORD t, DWORD o, void *b, DWORD n) { return 0xFFFFFFFF; }
/* fonts the program can enumerate: the system's three families */
typedef struct { LONG h, w, esc, ori, weight; BYTE it, ul, so, cs, op, cp, q, pf; char face[32]; } LFA;
static int enum_fonts(void *fn, LPARAM lp, bool wide, const char *only) {
    static const char *faces[] = { "Tahoma", "Arial", "Times New Roman", "Courier New", "MS Sans Serif", "Segoe UI" };
    int r = 1;
    for (unsigned i = 0; i < ARRAY_LEN(faces); i++) {
        if (only && *only && strcasecmp(only, faces[i])) continue;
        uint8_t lf[92 + 128]; memset(lf, 0, sizeof lf);
        *(LONG *)lf = -13; *(LONG *)(lf + 16) = 400; lf[23] = 1; lf[27] = i == 3 ? 0x31 : 0x22;
        if (wide) um_a2w(faces[i], (WCHAR *)(lf + 28), 32); else strlcpy((char *)(lf + 28), faces[i], 32);
        uint8_t tm[64]; memset(tm, 0, sizeof tm); *(LONG *)tm = 16; *(LONG *)(tm + 4) = 13; *(LONG *)(tm + 28) = 400;
        r = ((int (WINAPI *)(const void *, const void *, DWORD, LPARAM))fn)(lf, tm, 4, lp);   /* TRUETYPE_FONTTYPE */
        if (!r) break;
    }
    return r;
}
static int WINAPI EnumFontFamiliesExW(HDC h, const LOGFONTW_ *lf, void *fn, LPARAM lp, DWORD f) { char a[64] = ""; if (lf) um_w2a(lf->face, a, sizeof a); return enum_fonts(fn, lp, true, a); }
static int WINAPI EnumFontFamiliesExA(HDC h, const LFA *lf, void *fn, LPARAM lp, DWORD f) { return enum_fonts(fn, lp, false, lf ? lf->face : NULL); }
static int WINAPI EnumFontFamiliesW(HDC h, const WCHAR *face, void *fn, LPARAM lp) { char a[64] = ""; um_w2a(face, a, sizeof a); return enum_fonts(fn, lp, true, a); }
static int WINAPI EnumFontFamiliesA(HDC h, const char *face, void *fn, LPARAM lp) { return enum_fonts(fn, lp, false, face); }
static int WINAPI EnumFontsW(HDC h, const WCHAR *face, void *fn, LPARAM lp) { return EnumFontFamiliesW(h, face, fn, lp); }
static int WINAPI EnumFontsA(HDC h, const char *face, void *fn, LPARAM lp) { return enum_fonts(fn, lp, false, face); }
static int WINAPI AddFontResourceW(const WCHAR *f) { return 1; }
static int WINAPI AddFontResourceExW(const WCHAR *f, DWORD fl, void *r) { return 1; }
static HANDLE WINAPI AddFontMemResourceEx(void *p, DWORD n, void *r, DWORD *num) { if (num) *num = 1; return 0x46524D; }
static BOOL WINAPI RemoveFontResourceW(const WCHAR *f) { return 1; }
static BOOL WINAPI RemoveFontMemResourceEx(HANDLE h) { return 1; }
static DWORD WINAPI GetFontLanguageInfo(HDC h) { return 0; }
/* devices other than the screen: none (no printers) */
static HDC WINAPI CreateDCW(const WCHAR *drv, const WCHAR *dev, const WCHAR *o, const void *dm) { char a[32]; um_w2a(drv, a, sizeof a); if (!drv || !strcasecmp(a, "DISPLAY")) return GetDC(0); return 0; }
static HDC WINAPI CreateDCA(const char *drv, const char *dev, const char *o, const void *dm) { if (!drv || !strcasecmp(drv, "DISPLAY")) return GetDC(0); return 0; }
static HDC WINAPI CreateICW(const WCHAR *drv, const WCHAR *dev, const WCHAR *o, const void *dm) { return CreateDCW(drv, dev, o, dm); }
static HDC WINAPI CreateICA(const char *drv, const char *dev, const char *o, const void *dm) { return CreateDCA(drv, dev, o, dm); }
static int WINAPI StartDocA(HDC h, const void *di) { return -1; }
static int WINAPI StartDocW(HDC h, const void *di) { return -1; }
static int WINAPI EndDoc(HDC h) { return -1; }
static int WINAPI AbortDoc(HDC h) { return -1; }
static int WINAPI StartPage(HDC h) { return -1; }
static int WINAPI EndPage(HDC h) { return -1; }
static int WINAPI SetAbortProc(HDC h, void *fn) { return -1; }
static int WINAPI Escape(HDC h, int esc, int n, const void *in, void *out) { return 0; }
static int WINAPI ExtEscape(HDC h, int esc, int n, const void *in, int no, void *out) { return 0; }
/* metafiles: not recorded or played */
static HANDLE WINAPI zero_h(void) { return 0; }
static UINT WINAPI zero_u(void) { return 0; }
static HANDLE WINAPI CopyEnhMetaFileW(HANDLE h, const WCHAR *f) { return 0; }
static BOOL WINAPI DeleteEnhMetaFile(HANDLE h) { return 1; }
static UINT WINAPI GetEnhMetaFileBits(HANDLE h, UINT n, BYTE *b) { return 0; }
static UINT WINAPI GetEnhMetaFileDescriptionW(HANDLE h, UINT n, WCHAR *b) { return 0; }
static UINT WINAPI GetEnhMetaFileHeader(HANDLE h, UINT n, void *b) { return 0; }
static UINT WINAPI GetEnhMetaFilePaletteEntries(HANDLE h, UINT n, void *b) { return 0; }
static UINT WINAPI GetWinMetaFileBits(HANDLE h, UINT n, BYTE *b, int m, HDC r) { return 0; }
static BOOL WINAPI PlayEnhMetaFile(HDC h, HANDLE m, const RECT32 *r) { return 0; }
static HANDLE WINAPI SetEnhMetaFileBits(UINT n, const BYTE *b) { return 0; }
static HANDLE WINAPI SetWinMetaFileBits(UINT n, const BYTE *b, HDC r, const void *p) { return 0; }
static HDC WINAPI CreateEnhMetaFileW(HDC r, const WCHAR *f, const RECT32 *rc, const WCHAR *d) { return 0; }
static HANDLE WINAPI CloseEnhMetaFile(HDC h) { return 0; }
static BOOL WINAPI GetDeviceGammaRamp(HDC h, void *r) { return 0; }
static BOOL WINAPI SetDeviceGammaRamp(HDC h, void *r) { return 0; }
static BOOL WINAPI GetICMProfileW(HDC h, DWORD *n, WCHAR *f) { return 0; }
static int WINAPI SetICMMode(HDC h, int m) { return 1; }
static BOOL WINAPI GdiAlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD bf) {
    dc_t *d = DC(dst), *s = DC(src); if (!d || !s || w <= 0 || h <= 0) return 0;
    dc_src(s);
    int sca = (bf >> 16) & 255; bool per_px = (bf >> 24) & 1;
    rect_t sv; dc_begin(d, &sv);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        uint32_t p = get_pixel(s->surf, s->ox + sx + i * sw / w, s->oy + sy + j * sh / h);
        int a = per_px ? (int)((p >> 24) & 255) * sca / 255 : sca;
        if (!a) continue;
        uint32_t q = get_pixel(d->surf, d->ox + x + i, d->oy + y + j);
        int r = per_px ? (int)((p >> 16) & 255) * sca / 255 + ((int)((q >> 16) & 255) * (255 - a)) / 255 : (((p >> 16) & 255) * a + ((q >> 16) & 255) * (255 - a)) / 255;
        int g = per_px ? (int)((p >> 8) & 255) * sca / 255 + ((int)((q >> 8) & 255) * (255 - a)) / 255 : (((p >> 8) & 255) * a + ((q >> 8) & 255) * (255 - a)) / 255;
        int b = per_px ? (int)(p & 255) * sca / 255 + ((int)(q & 255) * (255 - a)) / 255 : ((p & 255) * a + (q & 255) * (255 - a)) / 255;
        put_pixel(d->surf, d->ox + x + i, d->oy + y + j, rgb_of(MIN(r, 255), MIN(g, 255), MIN(b, 255)));
    }
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI GdiTransparentBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, UINT key) {
    dc_t *d = DC(dst), *s = DC(src); if (!d || !s || w <= 0 || h <= 0) return 0;
    dc_src(s);
    uint32_t k = cref(key) & 0xFFFFFF;
    rect_t sv; dc_begin(d, &sv);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        uint32_t p = get_pixel(s->surf, s->ox + sx + i * sw / w, s->oy + sy + j * sh / h);
        if ((p & 0xFFFFFF) != k) put_pixel(d->surf, d->ox + x + i, d->oy + y + j, p | 0xFF000000u);
    }
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI GdiGradientFill(HDC h, const void *v, ULONG_PTR nv, const void *m, ULONG_PTR nm, DWORD mode) {
    dc_t *d = DC(h); if (!d || nv < 2 || mode > 1) return 0;
    typedef struct { LONG x, y; uint16_t r, g, b, a; } TV;
    const TV *vx = v; const uint32_t *rc = m;
    rect_t sv; dc_begin(d, &sv);
    for (ULONG_PTR k = 0; k < nm; k++) {
        const TV *a = &vx[rc[2 * k]], *b = &vx[rc[2 * k + 1]];
        int l = MIN(a->x, b->x), t = MIN(a->y, b->y), r = MAX(a->x, b->x), bt = MAX(a->y, b->y);
        int len = mode ? bt - t : r - l; if (len <= 0) continue;
        for (int i = 0; i < len; i++) {
            uint32_t c = rgb_of((a->r + (b->r - a->r) * i / len) >> 8, (a->g + (b->g - a->g) * i / len) >> 8, (a->b + (b->b - a->b) * i / len) >> 8);
            if (mode) hline(d->surf, d->ox + l, d->oy + t + i, r - l, c); else fill_rect(d->surf, d->ox + l + i, d->oy + t, 1, bt - t, c);
        }
    }
    dc_end(d, sv);
    return 1;
}

/* ================================================================ user32: text and characters */
static WCHAR wup(WCHAR c) { return c < 128 ? toupper(c) : (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 32 : c; }
static WCHAR wdn(WCHAR c) { return c < 128 ? tolower(c) : (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c; }
static DWORD WINAPI CharUpperBuffW(WCHAR *s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = wup(s[i]); return n; }
static DWORD WINAPI CharLowerBuffW(WCHAR *s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = wdn(s[i]); return n; }
static DWORD WINAPI CharUpperBuffA(char *s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = toupper(s[i]); return n; }
static DWORD WINAPI CharLowerBuffA(char *s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = tolower(s[i]); return n; }
static WCHAR *WINAPI CharUpperW2(WCHAR *s) { if ((uintptr_t)s < 0x10000) return (WCHAR *)(uintptr_t)wup((WCHAR)(uintptr_t)s); for (WCHAR *p = s; *p; p++) *p = wup(*p); return s; }
static WCHAR *WINAPI CharLowerW2(WCHAR *s) { if ((uintptr_t)s < 0x10000) return (WCHAR *)(uintptr_t)wdn((WCHAR)(uintptr_t)s); for (WCHAR *p = s; *p; p++) *p = wdn(*p); return s; }
static WCHAR *WINAPI CharNextW(const WCHAR *s) { return (WCHAR *)(*s ? s + 1 : s); }
static WCHAR *WINAPI CharPrevW(const WCHAR *start, const WCHAR *s) { return (WCHAR *)(s > start ? s - 1 : start); }
static char *WINAPI CharNextA2(const char *s) { return (char *)(*s ? s + 1 : s); }
static char *WINAPI CharPrevA2(const char *start, const char *s) { return (char *)(s > start ? s - 1 : start); }
static BOOL WINAPI IsCharAlphaW(WCHAR c) { return (c < 128 && isalpha(c)) || (c >= 0xC0 && c != 0xD7 && c != 0xF7); }
static BOOL WINAPI IsCharAlphaNumericW(WCHAR c) { return IsCharAlphaW(c) || (c >= '0' && c <= '9'); }
static BOOL WINAPI IsCharLowerW(WCHAR c) { return wup(c) != c; }
static BOOL WINAPI IsCharUpperW(WCHAR c) { return wdn(c) != c; }
static BOOL WINAPI IsCharLowerA(char c) { return (c >= 'a' && c <= 'z') || (uint8_t)c >= 0xDF; }
static BOOL WINAPI IsCharUpperA(char c) { return (c >= 'A' && c <= 'Z') || ((uint8_t)c >= 0xC0 && (uint8_t)c <= 0xDE); }
static int WINAPI wvsprintfW(WCHAR *b, const WCHAR *f, w32_va_list ap) { void *fn = NULL; w32_lookup("msvcrt.dll", "_vsnwprintf", &fn); return ((int (CDECL *)(WCHAR *, size_t, const WCHAR *, w32_va_list))fn)(b, 1024, f, ap); }
static LONG tabbed(HDC h, int x, int y, const char *s, int n, int ntab, const int *tabs, int org, bool draw) {
    dc_t *d = DC(h); if (!d) return 0;
    int f = dcfont(d), cx = x, start = 0;
    int deftab = 8 * text_width(f, "x");
    if (n < 0) n = strlen(s);
    for (int i = 0; i <= n; i++) {
        if (i == n || s[i] == '\t') {
            if (draw && i > start) TextOutA(h, cx, y, s + start, i - start);
            cx += text_width_n(f, s + start, i - start);
            if (i < n) { int t = 0; if (ntab > 0) { for (int k = 0; k < ntab; k++) if (org + tabs[k] > cx) { t = org + tabs[k]; break; } if (!t) t = cx + (ntab == 1 ? tabs[0] : deftab); } else t = org + ((cx - org) / deftab + 1) * deftab; cx = t; }
            start = i + 1;
        }
    }
    return (cx - x) | (LONG)font_height(f) << 16;
}
static LONG WINAPI TabbedTextOutA(HDC h, int x, int y, const char *s, int n, int nt, const int *t, int org) { return tabbed(h, x, y, s, n, nt, t, org, true); }
static LONG WINAPI TabbedTextOutW(HDC h, int x, int y, const WCHAR *s, int n, int nt, const int *t, int org) { char a[1024]; if (n < 0) { n = 0; while (s[n]) n++; } int k = MIN(n, 1023); for (int i = 0; i < k; i++) a[i] = s[i] < 256 ? (char)s[i] : '?'; a[k] = 0; return tabbed(h, x, y, a, k, nt, t, org, true); }
static DWORD WINAPI GetTabbedTextExtentA(HDC h, const char *s, int n, int nt, const int *t) { return tabbed(h, 0, 0, s, n, nt, t, 0, false); }
static DWORD WINAPI GetTabbedTextExtentW(HDC h, const WCHAR *s, int n, int nt, const int *t) { char a[1024]; int k = MIN(n, 1023); for (int i = 0; i < k; i++) a[i] = s[i] < 256 ? (char)s[i] : '?'; return tabbed(h, 0, 0, a, k, nt, t, 0, false); }
static BOOL WINAPI GrayStringA(HDC h, HGDIOBJ br, void *fn, LPARAM data, int n, int x, int y, int w, int ht) {
    dc_t *d = DC(h); if (!d) return 0;
    uint32_t old = d->text; d->text = T.gray_text;
    BOOL r = fn ? ((BOOL (WINAPI *)(HDC, LPARAM, int))fn)(h, data, n) : TextOutA(h, x, y, (const char *)data, n ? n : -1);
    d->text = old; return r;
}
static BOOL WINAPI GrayStringW(HDC h, HGDIOBJ br, void *fn, LPARAM data, int n, int x, int y, int w, int ht) {
    if (fn) return GrayStringA(h, br, fn, data, n, x, y, w, ht);
    char a[512]; um_w2a((const WCHAR *)data, a, sizeof a); return GrayStringA(h, br, NULL, (LPARAM)a, n, x, y, w, ht);
}
/* DrawState: text (DST_TEXT 1 / DST_PREFIXTEXT 2), bitmaps (4), complex drawing by callback (0) */
static BOOL draw_state(HDC h, HGDIOBJ br, void *fn, LPARAM lp, WPARAM wp, int x, int y, int cx, int cy, UINT fl, bool wide) {
    dc_t *d = DC(h); if (!d) return 0;
    UINT kind = fl & 0xF; bool disabled = fl & 0x20;
    uint32_t old = d->text; if (disabled) d->text = T.gray_text;
    BOOL r = 1;
    if (kind == 1 || kind == 2) {
        char a[512]; if (wide) um_w2a((const WCHAR *)lp, a, sizeof a); else strlcpy(a, (const char *)lp, sizeof a);
        for (char *p = a; *p; p++) if (*p == '&' && kind == 2) memmove(p, p + 1, strlen(p));
        int om = d->bkmode; d->bkmode = 1; TextOutA(h, x, y, a, -1); d->bkmode = om;
    } else if (kind == 4) {
        HDC m = CreateCompatibleDC(h); HGDIOBJ o = SelectObject(m, (HGDIOBJ)lp);
        gdiobj_t *b = OBJ((HGDIOBJ)lp);
        if (b && b->bmp) BitBlt(h, x, y, cx ? cx : b->bmp->w, cy ? cy : b->bmp->h, m, 0, 0, 0xCC0020);
        SelectObject(m, o); DeleteDC(m);
    } else if (kind == 0 && fn) r = ((BOOL (WINAPI *)(HDC, LPARAM, WPARAM, int, int))fn)(h, lp, wp, cx, cy);
    d->text = old;
    return r;
}
static BOOL WINAPI DrawStateA(HDC h, HGDIOBJ br, void *fn, LPARAM lp, WPARAM wp, int x, int y, int cx, int cy, UINT fl) { return draw_state(h, br, fn, lp, wp, x, y, cx, cy, fl, false); }
static BOOL WINAPI DrawStateW(HDC h, HGDIOBJ br, void *fn, LPARAM lp, WPARAM wp, int x, int y, int cx, int cy, UINT fl) { return draw_state(h, br, fn, lp, wp, x, y, cx, cy, fl, true); }
/* DrawFrameControl: buttons, check boxes, radio buttons, scroll arrows and caption buttons */
static BOOL WINAPI DrawFrameControl(HDC h, RECT32 *r, UINT type, UINT st) {
    dc_t *d = DC(h); if (!d) return 0;
    rect_t sv; dc_begin(d, &sv);
    int x = d->ox + r->l, y = d->oy + r->t, w = r->r - r->l, ht = r->b - r->t;
    bool pushed = st & 0x200, checked = st & 0x400, inactive = st & 0x100;
    uint32_t ink = inactive ? T.gray_text : T.text;
    if (type == 4 && (st & 0xF) <= 1) {                       /* DFC_BUTTON: check box (0) or radio (4)... */
    }
    if (type == 4) {
        int kind = st & 0x1F;
        if (kind == 0 || kind == 0x10) { fill_rect(d->surf, x, y, w, ht, T.face); bevel_deep(d->surf, x, y, w, ht, !pushed); }    /* DFCS_BUTTONPUSH */
        else if (kind == 4) { int s = MIN(w, ht); fill_round_rect(d->surf, x, y, s, s, s / 2, WHITE); draw_round_rect(d->surf, x, y, s, s, s / 2, T.frame_out); if (checked) fill_round_rect(d->surf, x + s / 3, y + s / 3, s / 3 + 1, s / 3 + 1, s / 6, ink); }
        else { int s = MIN(w, ht); fill_rect(d->surf, x, y, s, s, WHITE); draw_rect(d->surf, x, y, s, s, T.frame_out);
            if (checked) for (int i = 0; i < 3; i++) { draw_line(d->surf, x + 3, y + s / 2 - 1 + i, x + s / 2 - 1, y + s - 4 + i, ink); draw_line(d->surf, x + s / 2 - 1, y + s - 4 + i, x + s - 4, y + 3 + i, ink); } }
    } else if (type == 3 || type == 1) {                      /* DFC_SCROLL, DFC_CAPTION */
        fill_rect(d->surf, x, y, w, ht, T.face); bevel_deep(d->surf, x, y, w, ht, !pushed);
        int k = st & 0xF, cx = x + w / 2, cy = y + ht / 2, s = MIN(w, ht) / 4;
        if (type == 3 && k <= 3) for (int i = 0; i <= s; i++) {
            if (k == 0) hline(d->surf, cx - i, cy - s / 2 + i, 2 * i + 1, ink); else if (k == 1) hline(d->surf, cx - (s - i), cy - s / 2 + i, 2 * (s - i) + 1, ink);
            else if (k == 2) fill_rect(d->surf, cx - s / 2 + i, cy - i, 1, 2 * i + 1, ink); else fill_rect(d->surf, cx - s / 2 + i, cy - (s - i), 1, 2 * (s - i) + 1, ink);
        } else if (type == 1 && k == 0) { draw_line(d->surf, cx - s, cy - s, cx + s, cy + s, ink); draw_line(d->surf, cx + s, cy - s, cx - s, cy + s, ink); }
    } else if (type == 2) {                                   /* DFC_MENU: check mark, bullet, arrow */
        int k = st & 0xF, cx = x + w / 2, cy = y + ht / 2;
        if (k == 1) { draw_line(d->surf, cx - 3, cy, cx - 1, cy + 2, ink); draw_line(d->surf, cx - 1, cy + 2, cx + 3, cy - 2, ink); }
        else if (k == 2) fill_round_rect(d->surf, cx - 2, cy - 2, 5, 5, 2, ink);
        else for (int i = 0; i < 4; i++) fill_rect(d->surf, cx - 1 + i, cy - 3 + i, 1, 7 - 2 * i, ink);
    }
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI DrawAnimatedRects(HWND h, int id, const RECT32 *a, const RECT32 *b) { return 1; }
static BOOL WINAPI SubtractRect(RECT32 *d, const RECT32 *a, const RECT32 *b) {
    *d = *a;
    if (b->l <= a->l && b->r >= a->r) { if (b->t <= a->t && b->b > a->t) d->t = MIN(b->b, a->b); else if (b->b >= a->b && b->t < a->b) d->b = MAX(b->t, a->t); }
    else if (b->t <= a->t && b->b >= a->b) { if (b->l <= a->l && b->r > a->l) d->l = MIN(b->r, a->r); else if (b->r >= a->r && b->l < a->r) d->r = MAX(b->l, a->l); }
    if (d->r <= d->l || d->b <= d->t) { memset(d, 0, sizeof *d); return 0; }
    return 1;
}
static HDC WINAPI GetDCEx(HWND h, HGDIOBJ rg, DWORD f) { return GetDC(h); }
static BOOL WINAPI ScrollDC(HDC h, int dx, int dy, const RECT32 *sc, const RECT32 *clip, HGDIOBJ upd, RECT32 *ur) {
    dc_t *d = DC(h); if (!d) return 0;
    RECT32 r; if (sc) r = *sc; else { r.l = d->clip.x - d->ox; r.t = d->clip.y - d->oy; r.r = r.l + d->clip.w; r.b = r.t + d->clip.h; }
    int w = r.r - r.l, ht = r.b - r.t; if (w <= 0 || ht <= 0) return 1;
    canvas_t *c = canvas_new(w, ht); blit(c, 0, 0, d->surf, d->ox + r.l, d->oy + r.t, w, ht);
    rect_t sv; dc_begin(d, &sv); blit(d->surf, d->ox + r.l + dx, d->oy + r.t + dy, c, 0, 0, w, ht); dc_end(d, sv);
    canvas_free(c);
    if (ur) *ur = r;
    return 1;
}
static int WINAPI ScrollWindowEx(HWND h, int dx, int dy, const RECT32 *sc, const RECT32 *clip, HGDIOBJ upd, RECT32 *ur, UINT f) { if (ur) { GetClientRect(h, ur); } InvalidateRect(h, NULL, (f & 4) != 0); return 2; }

/* ================================================================ menus: state and item info */
/* find an item by command (MF_BYCOMMAND) in a menu and its submenus, or by position */
static mitem_t *menu_item(HANDLE hm, UINT item, bool bypos, w32menu_t **owner) {
    w32menu_t *m = MENU(hm); if (!m) return NULL;
    if (bypos) { if (owner) *owner = m; return item < (UINT)m->n ? &m->items[item] : NULL; }
    for (int i = 0; i < m->n; i++) {
        if (!(m->items[i].flags & 0x10) && m->items[i].id == item) { if (owner) *owner = m; return &m->items[i]; }
        if (m->items[i].flags & 0x10) { mitem_t *r = menu_item(m->items[i].id, item, false, owner); if (r) return r; }
    }
    return NULL;
}
/* the shown menu bar mirrors the item flags: refresh it in place for every window using hm */
static void menu_sync(void) {
    for (w32win_t *w = all_wins; w; w = w->all_next) {
        if (w->destroyed || !w->top || !w->top->menus) continue;
        w32menu_t *m = MENU(w->menu); if (!m) continue;
        for (int i = 0; i < m->n && i < w->top->nmenus; i++) {
            w32menu_t *sub = (m->items[i].flags & 0x10) ? MENU(m->items[i].id) : NULL;
            if (!sub) continue;
            for (int k = 0; k < sub->n && k < 19; k++) {
                int fl = 0; uint32_t f = sub->items[k].flags;
                if (f & 8) fl |= (f & 0x200) ? MI_RADIO | MI_CHECKED : MI_CHECKED;
                if (f & 3) fl |= MI_DISABLED;
                w->top->menus[i].items[k].flags = fl;
                if (w->top->menus[i].items[k].label && strcmp(w->top->menus[i].items[k].label, sub->items[k].text) && !(f & 0x800)) { kfree((void *)w->top->menus[i].items[k].label); w->top->menus[i].items[k].label = strdup(sub->items[k].text); }
            }
        }
    }
    wm_need_redraw = true;
}
static DWORD WINAPI CheckMenuItem2(HANDLE hm, UINT id, UINT f) {
    mitem_t *it = menu_item(hm, id, f & 0x400, NULL); if (!it) return (DWORD)-1;
    DWORD old = it->flags & 8;
    it->flags = (it->flags & ~8u) | (f & 8);
    menu_sync();
    return old;
}
static BOOL WINAPI EnableMenuItem2(HANDLE hm, UINT id, UINT f) {
    mitem_t *it = menu_item(hm, id, f & 0x400, NULL); if (!it) return -1;
    BOOL old = it->flags & 3;
    it->flags = (it->flags & ~3u) | (f & 3);
    menu_sync();
    return old;
}
static BOOL WINAPI CheckMenuRadioItem(HANDLE hm, UINT first, UINT last, UINT check, UINT f) {
    bool bypos = f & 0x400; w32menu_t *owner = NULL;
    if (!menu_item(hm, check, bypos, &owner) || !owner) return 0;
    for (int i = 0; i < owner->n; i++) {
        uint32_t key = bypos ? (uint32_t)i : owner->items[i].id;
        if (key < first || key > last || (owner->items[i].flags & 0x10)) continue;
        if (key == check) owner->items[i].flags |= 8 | 0x200; else owner->items[i].flags &= ~(8u | 0x200u);
    }
    menu_sync();
    return 1;
}
static UINT WINAPI GetMenuState(HANDLE hm, UINT id, UINT f) {
    w32menu_t *o = NULL; mitem_t *it = menu_item(hm, id, f & 0x400, &o); if (!it) return (UINT)-1;
    if (it->flags & 0x10) { w32menu_t *s = MENU(it->id); return (s ? s->n : 0) << 8 | (it->flags & 0xFF); }
    return it->flags & 0xFFFF;
}
static int WINAPI GetMenuStringA(HANDLE hm, UINT id, char *b, int n, UINT f) { mitem_t *it = menu_item(hm, id, f & 0x400, NULL); if (!it) return 0; if (!b || !n) return strlen(it->text); strlcpy(b, it->text, n); return strlen(b); }
static int WINAPI GetMenuStringW(HANDLE hm, UINT id, WCHAR *b, int n, UINT f) { mitem_t *it = menu_item(hm, id, f & 0x400, NULL); if (!it) return 0; if (!b || !n) return strlen(it->text); return um_a2w(it->text, b, n); }
/* MENUITEMINFO: cbSize, fMask, fType, fState, wID, hSubMenu, hbmpChecked, hbmpUnchecked, dwItemData, dwTypeData, cch, hbmpItem */
typedef struct { UINT size, mask, type, state, id; HANDLE sub; HGDIOBJ chk, unchk; ULONG_PTR data; void *text; UINT cch; HGDIOBJ bmp; } MII;
static BOOL menu_info_get(HANDLE hm, UINT item, BOOL bypos, MII *mi, bool wide) {
    mitem_t *it = menu_item(hm, item, bypos, NULL); if (!it) { P->last_error = 1456; return 0; }
    UINT m = mi->mask;
    if (m & 0x100) mi->type = it->flags & 0x800;                             /* MIIM_FTYPE */
    if (m & 0x10) { mi->type = it->flags & 0x800; if (mi->text && mi->cch) { if (wide) um_a2w(it->text, mi->text, mi->cch); else strlcpy(mi->text, it->text, mi->cch); } mi->cch = strlen(it->text); }   /* MIIM_TYPE */
    if (m & 0x40) { if (mi->text && mi->cch) { if (wide) um_a2w(it->text, mi->text, mi->cch); else strlcpy(mi->text, it->text, mi->cch); } mi->cch = strlen(it->text); }   /* MIIM_STRING */
    if (m & 1) mi->state = (it->flags & 8) | (it->flags & 3);                /* MIIM_STATE */
    if (m & 2) mi->id = (it->flags & 0x10) ? 0 : it->id;                     /* MIIM_ID */
    if (m & 4) mi->sub = (it->flags & 0x10) ? it->id : 0;                    /* MIIM_SUBMENU */
    if (m & 0x20) mi->data = 0;                                              /* MIIM_DATA */
    return 1;
}
static BOOL menu_info_set(HANDLE hm, UINT item, BOOL bypos, const MII *mi, bool wide) {
    mitem_t *it = menu_item(hm, item, bypos, NULL); if (!it) { P->last_error = 1456; return 0; }
    UINT m = mi->mask;
    if ((m & 0x50) && mi->text && !(mi->type & 0x800)) { if (wide) { char a[64]; um_w2a(mi->text, a, sizeof a); strlcpy(it->text, a, 64); } else strlcpy(it->text, mi->text, 64); for (char *p = it->text; *p; p++) if (*p == '&') memmove(p, p + 1, strlen(p)); }
    if (m & 1) it->flags = (it->flags & ~0xBu) | (mi->state & 0xB);
    if (m & 2) { if (!(it->flags & 0x10)) it->id = mi->id; }
    if (m & 4) { if (mi->sub) { it->flags |= 0x10; it->id = mi->sub; } }
    if (m & 0x100) it->flags = (it->flags & ~0x800u) | (mi->type & 0x800);
    menu_sync();
    return 1;
}
static BOOL WINAPI GetMenuItemInfoA(HANDLE hm, UINT i, BOOL bp, MII *mi) { return menu_info_get(hm, i, bp, mi, false); }
static BOOL WINAPI GetMenuItemInfoW(HANDLE hm, UINT i, BOOL bp, MII *mi) { return menu_info_get(hm, i, bp, mi, true); }
static BOOL WINAPI SetMenuItemInfoA(HANDLE hm, UINT i, BOOL bp, const MII *mi) { return menu_info_set(hm, i, bp, mi, false); }
static BOOL WINAPI SetMenuItemInfoW(HANDLE hm, UINT i, BOOL bp, const MII *mi) { return menu_info_set(hm, i, bp, mi, true); }
static BOOL insert_item(HANDLE hm, UINT item, BOOL bypos, const MII *mi, bool wide) {
    w32menu_t *m = MENU(hm); if (!m || m->n >= 24) return 0;
    int pos = m->n;
    if (bypos) pos = item < (UINT)m->n ? (int)item : m->n;
    else for (int i = 0; i < m->n; i++) if (!(m->items[i].flags & 0x10) && m->items[i].id == item) { pos = i; break; }
    memmove(&m->items[pos + 1], &m->items[pos], (m->n - pos) * sizeof(mitem_t)); m->n++;
    mitem_t *it = &m->items[pos]; memset(it, 0, sizeof *it);
    if ((mi->mask & 0x110) && (mi->type & 0x800)) { it->flags = 0x800; strcpy(it->text, "-"); }
    else if ((mi->mask & 0x50) && mi->text) { if (wide) um_w2a(mi->text, it->text, 64); else strlcpy(it->text, mi->text, 64); for (char *p = it->text; *p; p++) if (*p == '&') memmove(p, p + 1, strlen(p)); }
    if (mi->mask & 2) it->id = mi->id;
    if ((mi->mask & 4) && mi->sub) { it->flags |= 0x10; it->id = mi->sub; }
    if (mi->mask & 1) it->flags |= mi->state & 0xB;
    for (w32win_t *w = all_wins; w; w = w->all_next) if (!w->destroyed && w->menu == hm) apply_menu(w);
    return 1;
}
static BOOL WINAPI InsertMenuItemA(HANDLE hm, UINT i, BOOL bp, const MII *mi) { return insert_item(hm, i, bp, mi, false); }
static BOOL WINAPI InsertMenuItemW(HANDLE hm, UINT i, BOOL bp, const MII *mi) { return insert_item(hm, i, bp, mi, true); }
static BOOL WINAPI SetMenuDefaultItem(HANDLE hm, UINT i, UINT bp) { return 1; }
static UINT WINAPI GetMenuDefaultItem(HANDLE hm, UINT bp, UINT f) { return (UINT)-1; }
static BOOL WINAPI SetMenuInfo(HANDLE hm, const void *mi) { return 1; }
static BOOL WINAPI GetMenuInfo(HANDLE hm, void *mi) { return MENU(hm) != NULL; }
static BOOL WINAPI SetMenuItemBitmaps(HANDLE hm, UINT i, UINT f, HGDIOBJ a, HGDIOBJ b) { return 1; }
static LONG WINAPI GetMenuCheckMarkDimensions(void) { return 13 | 13 << 16; }
static BOOL WINAPI GetMenuItemRect(HWND h, HANDLE hm, UINT i, RECT32 *r) { memset(r, 0, sizeof *r); return 0; }
static BOOL WINAPI GetMenuBarInfo(HWND h, LONG obj, LONG item, void *mbi) { return 0; }
static BOOL WINAPI EndMenu(void) { return 1; }
static int WINAPI MenuItemFromPoint(HWND h, HANDLE hm, LONG pt) { return -1; }
static HANDLE WINAPI LoadMenuIndirectW(const void *tpl) { return 0; }
static HANDLE WINAPI LoadMenuIndirectA(const void *tpl) { return 0; }

/* ================================================================ window properties */
typedef struct { HWND h; char name[64]; uintptr_t atom; HANDLE val; } prop_t;
static prop_t props[512];
static prop_t *prop_find(HWND h, const void *name, bool wide, bool create) {
    char n[64] = ""; uintptr_t atom = (uintptr_t)name < 0x10000 ? (uintptr_t)name : 0;
    if (!atom) { if (wide) um_w2a(name, n, sizeof n); else strlcpy(n, name, sizeof n); }
    prop_t *free_slot = NULL;
    for (int i = 0; i < 512; i++) {
        if (!props[i].h) { if (!free_slot) free_slot = &props[i]; continue; }
        if (props[i].h == h && props[i].atom == atom && (atom || !strcasecmp(props[i].name, n))) return &props[i];
    }
    if (!create || !free_slot) return NULL;
    free_slot->h = h; free_slot->atom = atom; strlcpy(free_slot->name, n, sizeof free_slot->name); free_slot->val = 0;
    return free_slot;
}
static BOOL WINAPI SetPropA(HWND h, const char *n, HANDLE v) { if (!W(h)) return 0; prop_t *p = prop_find(h, n, false, true); if (!p) return 0; p->val = v; return 1; }
static BOOL WINAPI SetPropW(HWND h, const WCHAR *n, HANDLE v) { if (!W(h)) return 0; prop_t *p = prop_find(h, n, true, true); if (!p) return 0; p->val = v; return 1; }
static HANDLE WINAPI GetPropA(HWND h, const char *n) { prop_t *p = prop_find(h, n, false, false); return p ? p->val : 0; }
static HANDLE WINAPI GetPropW(HWND h, const WCHAR *n) { prop_t *p = prop_find(h, n, true, false); return p ? p->val : 0; }
static HANDLE WINAPI RemovePropA(HWND h, const char *n) { prop_t *p = prop_find(h, n, false, false); if (!p) return 0; HANDLE v = p->val; p->h = 0; return v; }
static HANDLE WINAPI RemovePropW(HWND h, const WCHAR *n) { prop_t *p = prop_find(h, n, true, false); if (!p) return 0; HANDLE v = p->val; p->h = 0; return v; }
static int WINAPI EnumPropsExW(HWND h, void *fn, LPARAM lp) {
    int r = -1;
    for (int i = 0; i < 512; i++) if (props[i].h == h) { WCHAR w[64]; um_a2w(props[i].name, w, 64); r = ((BOOL (WINAPI *)(HWND, const void *, HANDLE, LPARAM))fn)(h, props[i].atom ? (const void *)props[i].atom : w, props[i].val, lp); if (!r) break; }
    return r;
}

/* ================================================================ classes, windows, input */
typedef struct { UINT size, style; void *proc; int cls_extra, wnd_extra; HINSTANCE inst; HANDLE icon, cursor; HGDIOBJ bg; const void *menu, *name; HANDLE sm; } WCEX;
static BOOL WINAPI GetClassInfoExA(HINSTANCE h, const char *n, WCEX *wc) {
    WNDCLASSA a; if (!GetClassInfoA(h, n, &a)) return 0;
    wc->style = a.style; wc->proc = a.proc; wc->cls_extra = a.cls_extra; wc->wnd_extra = a.wnd_extra; wc->inst = a.inst; wc->icon = a.icon; wc->cursor = a.cursor; wc->bg = a.bg; wc->menu = a.menu; wc->name = n; wc->sm = a.icon;
    return 1;
}
static BOOL WINAPI GetClassInfoExW(HINSTANCE h, const WCHAR *n, WCEX *wc) {
    WNDCLASSW_ a; if (!GetClassInfoW(h, n, &a)) return 0;
    wc->style = a.style; wc->proc = a.proc; wc->cls_extra = a.cls_extra; wc->wnd_extra = a.wnd_extra; wc->inst = a.inst; wc->icon = a.icon; wc->cursor = a.cursor; wc->bg = a.bg; wc->menu = a.menu; wc->name = n; wc->sm = a.icon;
    return 1;
}
static DWORD WINAPI GetClassLongW(HWND h, int i) { return GetClassLongA(h, i); }
static DWORD WINAPI SetClassLongW(HWND h, int i, LONG v) { return SetClassLongA(h, i, v); }
static HWND WINAPI FindWindowExA(HWND parent, HWND after, const char *cls, const char *title) {
    w32win_t *p = W(parent);
    w32win_t *start = p ? p->child : all_wins;
    bool past = !after;
    for (w32win_t *w = start; w; w = p ? w->next : w->all_next) {
        if (w->destroyed || (!p && w->parent)) continue;
        if (!past) { if ((HWND)w == after) past = true; continue; }
        if (title && strcmp(w->text, title)) continue;
        if (cls && (uintptr_t)cls >= 0x10000 && w->cls && strcasecmp(w->cls->name, cls)) continue;
        return (HWND)w;
    }
    return 0;
}
static HWND WINAPI FindWindowExW(HWND parent, HWND after, const WCHAR *cls, const WCHAR *title) {
    char c[64], t[256]; um_w2a(cls, c, sizeof c); um_w2a(title, t, sizeof t);
    return FindWindowExA(parent, after, cls && (uintptr_t)cls >= 0x10000 ? c : NULL, title ? t : NULL);
}
static HWND WINAPI GetTopWindow(HWND h) { w32win_t *w = W(h); if (w) return (HWND)w->child; for (w32win_t *x = all_wins; x; x = x->all_next) if (!x->destroyed && !x->parent && x->proc == P) return (HWND)x; return 0; }
static HWND WINAPI GetLastActivePopup(HWND h) { return h; }
static BOOL WINAPI ShowOwnedPopups(HWND h, BOOL s) { return 1; }
static BOOL WINAPI SetLayeredWindowAttributes(HWND h, COLORREF k, BYTE a, DWORD f) { return W(h) != NULL; }
static BOOL WINAPI GetLayeredWindowAttributes(HWND h, COLORREF *k, BYTE *a, DWORD *f) { if (a) *a = 255; if (f) *f = 2; return 1; }
static BOOL WINAPI UpdateLayeredWindow(HWND h, HDC d, void *p, void *s, HDC src, void *sp, COLORREF k, void *bl, DWORD f) { return 1; }
static int WINAPI SetWindowRgn(HWND h, HGDIOBJ r, BOOL redraw) { if (r) DeleteObject2(r); return 1; }
static int WINAPI GetWindowRgn(HWND h, HGDIOBJ r) { return 0; }
static int WINAPI GetWindowRgnBox(HWND h, RECT32 *r) { return 0; }
static BOOL WINAPI AttachThreadInput(DWORD a, DWORD b, BOOL f) { return 1; }
static DWORD WINAPI WaitForInputIdle(HANDLE h, DWORD ms) { return 0; }
static LPARAM WINAPI GetMessageExtraInfo(void) { return 0; }
static LPARAM WINAPI SetMessageExtraInfo(LPARAM l) { return 0; }
static void WINAPI NotifyWinEvent(DWORD e, HWND h, LONG o, LONG c) {}
static HANDLE WINAPI SetWinEventHook(DWORD a, DWORD b, HINSTANCE m, void *fn, DWORD p, DWORD t, DWORD f) { return 0x57454831; }
static BOOL WINAPI UnhookWinEvent(HANDLE h) { return 1; }
static void WINAPI SetDebugErrorLevel(DWORD l) {}
static HANDLE WINAPI RegisterDeviceNotificationW(HANDLE r, void *f, DWORD fl) { return 0x44455631; }
static HANDLE WINAPI RegisterDeviceNotificationA(HANDLE r, void *f, DWORD fl) { return 0x44455631; }
static BOOL WINAPI UnregisterDeviceNotification(HANDLE h) { return 1; }
static HANDLE WINAPI GetProcessWindowStation(void) { return 0x57535431; }
static HANDLE WINAPI GetThreadDesktop(DWORD t) { return 0x44534B31; }
static BOOL WINAPI GetUserObjectInformationW(HANDLE h, int i, void *b, DWORD n, DWORD *need) {
    if (i == 1) { if (need) *need = 12; if (n < 12) return 0; DWORD *f = b; f[0] = 0; f[1] = 0; f[2] = 1; return 1; }   /* UOI_FLAGS: WSF_VISIBLE */
    if (i == 2 || i == 3) { const char *s = i == 2 ? "WinSta0" : "WindowStation"; DWORD l = (strlen(s) + 1) * 2; if (need) *need = l; if (n < l) return 0; um_a2w(s, b, n / 2); return 1; }
    return 0;
}
static BOOL WINAPI GetUserObjectInformationA(HANDLE h, int i, void *b, DWORD n, DWORD *need) { return GetUserObjectInformationW(h, i, b, n, need); }
static UINT WINAPI GetRawInputData(HANDLE h, UINT cmd, void *d, UINT *sz, UINT hs) { if (sz) *sz = 0; return 0; }
static UINT WINAPI GetRawInputDeviceList(void *l, UINT *n, UINT sz) { if (n) *n = 0; return 0; }
static UINT WINAPI GetRawInputDeviceInfoA(HANDLE h, UINT c, void *d, UINT *sz) { return (UINT)-1; }
static UINT WINAPI GetRawInputDeviceInfoW(HANDLE h, UINT c, void *d, UINT *sz) { return (UINT)-1; }
static BOOL WINAPI RegisterRawInputDevices(const void *d, UINT n, UINT sz) { return 1; }
static UINT WINAPI GetRegisteredRawInputDevices(void *d, UINT *n, UINT sz) { if (n) *n = 0; return 0; }
static LPARAM WINAPI ReuseDDElParam(LPARAM l, UINT a, UINT b, uintptr_t lo, uintptr_t hi) { return (lo & 0xFFFF) | (hi << 16); }
static BOOL WINAPI UnpackDDElParam(UINT m, LPARAM l, uintptr_t *lo, uintptr_t *hi) { if (lo) *lo = l & 0xFFFF; if (hi) *hi = (l >> 16) & 0xFFFF; return 1; }
static LPARAM WINAPI PackDDElParam(UINT m, uintptr_t lo, uintptr_t hi) { return (lo & 0xFFFF) | (hi << 16); }
static BOOL WINAPI FreeDDElParam(UINT m, LPARAM l) { return 1; }
/* scroll bars: the range and the bar's geometry */
static BOOL WINAPI GetScrollRange(HWND h, int bar, int *mn, int *mx) {
    SCROLLINFO_ si; memset(&si, 0, sizeof si); si.size = sizeof si; si.mask = 1;
    if (!GetScrollInfo(h, bar, &si)) { *mn = *mx = 0; return 1; }
    *mn = si.min; *mx = si.max; return 1;
}
static BOOL WINAPI GetScrollBarInfo(HWND h, LONG obj, void *sbi) {
    uint8_t *b = sbi; DWORD sz = *(DWORD *)b; memset(b + 4, 0, sz - 4);
    RECT32 r = { 0 }; GetClientRect(h, &r);
    LONG *rc = (LONG *)(b + 4);
    if (obj == (LONG)0xFFFFFFFB) { rc[0] = r.r; rc[1] = r.t; rc[2] = r.r + 17; rc[3] = r.b; } else { rc[0] = r.l; rc[1] = r.b; rc[2] = r.r; rc[3] = r.b + 17; }
    return 1;
}
typedef struct { DWORD size; RECT32 item, btn; DWORD state; HWND combo, item_w, list; } CBI;
static BOOL WINAPI GetComboBoxInfo(HWND h, CBI *c) {
    w32win_t *w = W(h); if (!w) return 0;
    memset((uint8_t *)c + 4, 0, sizeof *c - 4);
    c->item.l = 2; c->item.t = 2; c->item.r = w->w - 20; c->item.b = w->h - 2; c->btn.l = w->w - 18; c->btn.t = 2; c->btn.r = w->w - 2; c->btn.b = w->h - 2;
    c->combo = h; c->item_w = h; c->list = h;
    return 1;
}
/* keyboard */
static const char *vk_name(UINT vk) {
    static char b[8];
    switch (vk) { case 8: return "Backspace"; case 9: return "Tab"; case 13: return "Enter"; case 16: return "Shift"; case 17: return "Ctrl"; case 18: return "Alt"; case 19: return "Pause";
    case 20: return "Caps Lock"; case 27: return "Esc"; case 32: return "Space"; case 33: return "Page Up"; case 34: return "Page Down"; case 35: return "End"; case 36: return "Home";
    case 37: return "Left"; case 38: return "Up"; case 39: return "Right"; case 40: return "Down"; case 45: return "Insert"; case 46: return "Delete"; case 0x5B: return "Left Windows";
    case 0x90: return "Num Lock"; case 0x91: return "Scroll Lock"; case 0xBA: return ";"; case 0xBB: return "="; case 0xBC: return ","; case 0xBD: return "-"; case 0xBE: return "."; case 0xBF: return "/"; case 0xC0: return "`";
    case 0xDB: return "["; case 0xDC: return "\\"; case 0xDD: return "]"; case 0xDE: return "'"; }
    if (vk >= 0x70 && vk <= 0x87) { snprintf(b, sizeof b, "F%u", vk - 0x6F); return b; }
    if (vk >= 0x60 && vk <= 0x69) { snprintf(b, sizeof b, "Num %u", vk - 0x60); return b; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { b[0] = vk; b[1] = 0; return b; }
    return "";
}
/* scan codes of a US keyboard for the keys programs ask about */
static UINT vk_scan(UINT vk) {
    static const struct { uint8_t vk, sc; } t[] = { {8,14},{9,15},{13,28},{16,42},{17,29},{18,56},{20,58},{27,1},{32,57},{33,73},{34,81},{35,79},{36,71},{37,75},{38,72},{39,77},{40,80},{45,82},{46,83},
        {'1',2},{'2',3},{'3',4},{'4',5},{'5',6},{'6',7},{'7',8},{'8',9},{'9',10},{'0',11},{'Q',16},{'W',17},{'E',18},{'R',19},{'T',20},{'Y',21},{'U',22},{'I',23},{'O',24},{'P',25},
        {'A',30},{'S',31},{'D',32},{'F',33},{'G',34},{'H',35},{'J',36},{'K',37},{'L',38},{'Z',44},{'X',45},{'C',46},{'V',47},{'B',48},{'N',49},{'M',50},
        {0x70,59},{0x71,60},{0x72,61},{0x73,62},{0x74,63},{0x75,64},{0x76,65},{0x77,66},{0x78,67},{0x79,68},{0x7A,87},{0x7B,88},{0xBD,12},{0xBB,13},{0xDB,26},{0xDD,27},{0xBA,39},{0xDE,40},{0xC0,41},{0xDC,43},{0xBC,51},{0xBE,52},{0xBF,53} };
    for (unsigned i = 0; i < ARRAY_LEN(t); i++) if (t[i].vk == vk) return t[i].sc;
    return 0;
}
static UINT scan_vk(UINT sc) { for (UINT vk = 1; vk < 256; vk++) if (vk_scan(vk) == sc) return vk; return 0; }
static UINT WINAPI MapVirtualKeyW(UINT c, UINT t) {
    switch (t) {
    case 0: return vk_scan(c);                                           /* MAPVK_VK_TO_VSC */
    case 1: case 3: return scan_vk(c & 0xFF);                            /* MAPVK_VSC_TO_VK(_EX) */
    case 2: return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ? c : c == 0xBD ? '-' : c == 0xBB ? '=' : c == 0xBC ? ',' : c == 0xBE ? '.' : c == 0xBF ? '/' : c == 0xBA ? ';' : 0;   /* MAPVK_VK_TO_CHAR */
    }
    return 0;
}
static UINT WINAPI MapVirtualKeyExW(UINT c, UINT t, HANDLE l) { return MapVirtualKeyW(c, t); }
static UINT WINAPI MapVirtualKeyA2(UINT c, UINT t) { return MapVirtualKeyW(c, t); }
static int WINAPI GetKeyNameTextA(LONG lp, char *b, int n) { UINT vk = scan_vk((lp >> 16) & 0xFF); strlcpy(b, vk_name(vk), n); return strlen(b); }
static int WINAPI GetKeyNameTextW(LONG lp, WCHAR *b, int n) { char a[32]; GetKeyNameTextA(lp, a, sizeof a); return um_a2w(a, b, n); }
static BOOL WINAPI GetKeyboardLayoutNameA(char *b) { strcpy(b, "00000409"); return 1; }
static BOOL WINAPI GetKeyboardLayoutNameW(WCHAR *b) { um_a2w("00000409", b, 9); return 1; }
static HANDLE WINAPI LoadKeyboardLayoutW(const WCHAR *n, UINT f) { return 0x04090409; }
static HANDLE WINAPI LoadKeyboardLayoutA(const char *n, UINT f) { return 0x04090409; }
static HANDLE WINAPI ActivateKeyboardLayout(HANDLE h, UINT f) { return 0x04090409; }
static int WINAPI GetKeyboardType(int t) { return t == 0 ? 4 : t == 1 ? 0 : t == 2 ? 12 : 0; }
static void WINAPI keybd_event(BYTE vk, BYTE sc, DWORD f, ULONG_PTR x) {
    w32win_t *w = focus_win; if (!w || w->destroyed) return;
    bool up = f & 2;
    keystate[vk] = up ? 0 : 0x80;
    w32_post(w->proc, (uintptr_t)w, up ? WM_KEYUP : WM_KEYDOWN, vk, 1 | (LPARAM)vk_scan(vk) << 16 | (up ? 0xC0000000 : 0));
    if (!up) { char c = (vk >= 'A' && vk <= 'Z') ? (keystate[16] ? vk : vk + 32) : (vk >= '0' && vk <= '9') || vk == ' ' || vk == 13 || vk == 8 ? vk : 0; if (c) w32_post(w->proc, (uintptr_t)w, WM_CHAR, c, 1); }
}
typedef struct { DWORD type; union { struct { LONG dx, dy; DWORD data, flags, time; ULONG_PTR x; } mi; struct { WORD vk, sc; DWORD flags, time; ULONG_PTR x; } ki; }; } INPUT32;
static UINT WINAPI SendInput(UINT n, INPUT32 *in, int sz) { for (UINT i = 0; i < n; i++) { INPUT32 *e = (INPUT32 *)((uint8_t *)in + i * sz); if (e->type == 1) keybd_event((BYTE)e->ki.vk, (BYTE)e->ki.sc, e->ki.flags, 0); } return n; }
static void WINAPI mouse_event(DWORD f, DWORD dx, DWORD dy, DWORD d, ULONG_PTR x) {}
static BOOL WINAPI TrackMouseEvent(void *tme) { return 1; }
static BOOL WINAPI BlockInput(BOOL b) { return 0; }
/* the clipboard's bookkeeping */
static UINT clip_seq = 1;
static int WINAPI CountClipboardFormats(void) { const char *t = clip_text(); return t && *t ? 3 : 0; }
static int WINAPI GetClipboardFormatNameA(UINT f, char *b, int n) { (void)f; (void)b; (void)n; return 0; }   /* registered formats keep no names */
static int WINAPI GetClipboardFormatNameW(UINT f, WCHAR *b, int n) { char a[128]; int l = GetClipboardFormatNameA(f, a, sizeof a); if (!l) return 0; return um_a2w(a, b, n); }
static DWORD WINAPI GetClipboardSequenceNumber(void) { const char *t = clip_text(); return clip_seq + (t ? (DWORD)strlen(t) : 0); }
static HWND clip_viewer;
static HWND WINAPI SetClipboardViewer(HWND h) { HWND o = clip_viewer; clip_viewer = h; return o; }
static BOOL WINAPI ChangeClipboardChain(HWND rm, HWND next) { if (clip_viewer == rm) clip_viewer = next; return 1; }
static HWND WINAPI GetClipboardViewer(void) { return clip_viewer; }
static BOOL WINAPI AddClipboardFormatListener(HWND h) { return 1; }
static BOOL WINAPI RemoveClipboardFormatListener(HWND h) { return 1; }
/* the display */
typedef struct { DWORD cb; char name[32]; char str[128]; DWORD flags; char id[128]; char key[128]; } DDA;
typedef struct { DWORD cb; WCHAR name[32]; WCHAR str[128]; DWORD flags; WCHAR id[128]; WCHAR key[128]; } DDW;
static BOOL WINAPI EnumDisplayDevicesA(const char *dev, DWORD i, DDA *d, DWORD f) {
    if (i > 0) return 0;
    DWORD cb = d->cb; memset(d, 0, cb); d->cb = cb;
    strcpy(d->name, dev ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1"); strcpy(d->str, dev ? "Default Monitor" : "WRLD Display Adapter"); d->flags = dev ? 3 : 5;
    return 1;
}
static BOOL WINAPI EnumDisplayDevicesW(const WCHAR *dev, DWORD i, DDW *d, DWORD f) {
    DDA a; a.cb = sizeof a; if (!EnumDisplayDevicesA(dev ? "x" : NULL, i, &a, f)) return 0;
    DWORD cb = d->cb; memset(d, 0, cb); d->cb = cb;
    um_a2w(a.name, d->name, 32); um_a2w(a.str, d->str, 128); d->flags = a.flags;
    return 1;
}
/* DEVMODE: the A and W layouts differ in the name fields (32 chars each) */
static void fill_devmode(uint8_t *dm, bool wide, int w, int h) {
    int base = wide ? 64 : 32;                       /* after dmDeviceName */
    WORD size = wide ? 220 : 156;
    *(WORD *)(dm + base + 4) = size;                 /* dmSize */
    *(DWORD *)(dm + base + 8) = 0x5C0000;            /* dmFields: bits per pel, width, height, frequency */
    int after = base + 8 + 4 + 16 + 2 * 5 + (wide ? 64 : 32);   /* ... dmFormName ... then dmLogPixels */
    *(WORD *)(dm + after) = 96;
    *(DWORD *)(dm + after + 2) = 32; *(DWORD *)(dm + after + 6) = w; *(DWORD *)(dm + after + 10) = h; *(DWORD *)(dm + after + 18) = 60;
    if (wide) um_a2w("WRLD Display", (WCHAR *)dm, 32); else strcpy((char *)dm, "WRLD Display");
}
static BOOL WINAPI EnumDisplaySettingsA(const char *dev, DWORD mode, void *dm) { if (mode != (DWORD)-1 && mode != (DWORD)-2 && mode > 0) return 0; memset(dm, 0, 156); fill_devmode(dm, false, SCREEN_W, SCREEN_H); return 1; }
static BOOL WINAPI EnumDisplaySettingsW(const WCHAR *dev, DWORD mode, void *dm) { if (mode != (DWORD)-1 && mode != (DWORD)-2 && mode > 0) return 0; memset(dm, 0, 220); fill_devmode(dm, true, SCREEN_W, SCREEN_H); return 1; }
static BOOL WINAPI EnumDisplaySettingsExW(const WCHAR *dev, DWORD mode, void *dm, DWORD f) { return EnumDisplaySettingsW(dev, mode, dm); }
static LONG WINAPI ChangeDisplaySettingsExW(const WCHAR *dev, void *dm, HWND h, DWORD f, void *p) {
    if (!dm) return 0;
    DWORD w = *(DWORD *)((uint8_t *)dm + 172 + 0), ht = *(DWORD *)((uint8_t *)dm + 176);
    return ((int)w == SCREEN_W && (int)ht == SCREEN_H) || !(f & 0) ? 0 : -2;          /* DISP_CHANGE_SUCCESSFUL: the mode stays */
}
static LONG WINAPI ChangeDisplaySettingsExA(const char *dev, void *dm, HWND h, DWORD f, void *p) { return 0; }
static LONG WINAPI ChangeDisplaySettingsW(void *dm, DWORD f) { return 0; }
static LONG WINAPI ChangeDisplaySettingsA(void *dm, DWORD f) { return 0; }
static BOOL WINAPI EnumDisplayMonitors(HDC h, const RECT32 *clip, void *fn, LPARAM lp) {
    RECT32 r = { 0, 0, SCREEN_W, SCREEN_H };
    return ((BOOL (WINAPI *)(HANDLE, HDC, RECT32 *, LPARAM))fn)(0x4D4F4E31, h, &r, lp) || 1;
}
static int WINAPI MessageBoxExW(HWND h, const WCHAR *t, const WCHAR *c, UINT ty, WORD l) { return MessageBoxW(h, t, c, ty); }
static int WINAPI MessageBoxTimeoutA(HWND h, const char *t, const char *c, UINT ty, WORD l, DWORD ms) { return MessageBoxA(h, t, c, ty); }
static DWORD WINAPI MsgWaitForMultipleObjectsEx(DWORD n, HANDLE *h, DWORD ms, DWORD mask, DWORD f) { return MsgWaitForMultipleObjects(n, h, (f & 1) != 0, ms, mask); }
static LRESULT WINAPI DefFrameProcA(HWND h, HWND client, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
static LRESULT WINAPI DefFrameProcW(HWND h, HWND client, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static LRESULT WINAPI DefMDIChildProcA(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
static LRESULT WINAPI DefMDIChildProcW(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static BOOL WINAPI LockWorkStation(void) { return 0; }
static BOOL WINAPI ExitWindowsEx(UINT f, DWORD r) { return 0; }
static BOOL WINAPI IsHungAppWindow(HWND h) { return 0; }
static BOOL WINAPI ChangeWindowMessageFilter(UINT m, DWORD f) { return 1; }
static BOOL WINAPI ChangeWindowMessageFilterEx(HWND h, UINT m, DWORD a, void *s) { return 1; }

/* ================================================================ comctl32: image lists */
#define ILMAGIC 0x494D474C
typedef struct { uint32_t magic; int cx, cy, n, cap, grow; canvas_t *strip; uint32_t bk; bool masked; int overlay[16]; } imglist_t;
static imglist_t *IL(HANDLE h) { imglist_t *l = (imglist_t *)h; return h > 0x100000 && !(h & 3) && l->magic == ILMAGIC ? l : NULL; }
static void il_reserve(imglist_t *l, int n) {
    if (n <= l->cap) return;
    int cap = MAX(n, l->cap + MAX(l->grow, 4));
    canvas_t *s = canvas_new(l->cx * cap, l->cy);
    memset(s->px, 0, (size_t)s->stride * s->h * 4);
    if (l->strip) { blit(s, 0, 0, l->strip, 0, 0, l->strip->w, l->strip->h); canvas_free(l->strip); }
    l->strip = s; l->cap = cap;
}
static HANDLE WINAPI ImageList_Create(int cx, int cy, UINT flags, int initial, int grow) {
    if (cx <= 0 || cy <= 0) return 0;
    imglist_t *l = kzalloc(sizeof *l);
    l->magic = ILMAGIC; l->cx = cx; l->cy = cy; l->grow = MAX(grow, 4); l->masked = flags & 1; l->bk = 0xFFFFFFFF;
    il_reserve(l, MAX(initial, 4));
    return (HANDLE)l;
}
static BOOL WINAPI ImageList_Destroy(HANDLE h) { imglist_t *l = IL(h); if (!l) return 0; l->magic = 0; if (l->strip) canvas_free(l->strip); kfree(l); return 1; }
/* copy one image from a bitmap strip; transparent pixels get alpha 0 */
static void il_put(imglist_t *l, int idx, canvas_t *src, int sx, int key_on, uint32_t key, canvas_t *mask) {
    for (int y = 0; y < l->cy; y++) for (int x = 0; x < l->cx; x++) {
        uint32_t p = sx + x < src->w && y < src->h ? src->px[(size_t)y * src->stride + sx + x] : 0;
        bool transparent = (key_on && (p & 0xFFFFFF) == key) || (mask && sx + x < mask->w && y < mask->h && (mask->px[(size_t)y * mask->stride + sx + x] & 0xFFFFFF) == 0xFFFFFF);
        l->strip->px[(size_t)y * l->strip->stride + idx * l->cx + x] = transparent ? 0 : (p | 0xFF000000u);
    }
}
static int add_strip(imglist_t *l, gdiobj_t *b, gdiobj_t *m, bool key_on, uint32_t key) {
    if (!b || !b->bmp) return -1;
    if (b->dib) dib_in(b);
    int count = MAX(1, b->bmp->w / l->cx), first = l->n;
    il_reserve(l, l->n + count);
    for (int i = 0; i < count; i++) il_put(l, l->n + i, b->bmp, i * l->cx, key_on, key, m && l->masked ? m->bmp : NULL);
    l->n += count;
    return first;
}
static int WINAPI ImageList_Add(HANDLE h, HGDIOBJ img, HGDIOBJ mask) { imglist_t *l = IL(h); if (!l) return -1; return add_strip(l, OBJ(img), OBJ(mask), false, 0); }
static int WINAPI ImageList_AddMasked(HANDLE h, HGDIOBJ img, COLORREF key) { imglist_t *l = IL(h); if (!l) return -1; return add_strip(l, OBJ(img), NULL, true, cref(key) & 0xFFFFFF); }
static BOOL WINAPI ImageList_Replace(HANDLE h, int i, HGDIOBJ img, HGDIOBJ mask) { imglist_t *l = IL(h); gdiobj_t *b = OBJ(img); if (!l || i < 0 || i >= l->n || !b || !b->bmp) return 0; if (b->dib) dib_in(b); gdiobj_t *m = OBJ(mask); il_put(l, i, b->bmp, 0, 0, 0, m && l->masked ? m->bmp : NULL); return 1; }
/* icons have no pixels here: a blank image keeps the indices right */
static int WINAPI ImageList_ReplaceIcon(HANDLE h, int i, HANDLE icon) {
    imglist_t *l = IL(h); if (!l) return -1;
    if (i < 0 || i >= l->n) { il_reserve(l, l->n + 1); i = l->n++; }
    for (int y = 0; y < l->cy; y++) for (int x = 0; x < l->cx; x++) l->strip->px[(size_t)y * l->strip->stride + i * l->cx + x] = 0;
    return i;
}
static BOOL WINAPI ImageList_Remove(HANDLE h, int i) {
    imglist_t *l = IL(h); if (!l) return 0;
    if (i < 0) { l->n = 0; return 1; }
    if (i >= l->n) return 0;
    for (int k = i; k + 1 < l->n; k++) blit(l->strip, k * l->cx, 0, l->strip, (k + 1) * l->cx, 0, l->cx, l->cy);
    l->n--;
    return 1;
}
static int WINAPI ImageList_GetImageCount(HANDLE h) { imglist_t *l = IL(h); return l ? l->n : 0; }
static BOOL WINAPI ImageList_SetImageCount(HANDLE h, UINT n) { imglist_t *l = IL(h); if (!l) return 0; il_reserve(l, n); l->n = n; return 1; }
static BOOL WINAPI ImageList_GetIconSize(HANDLE h, int *cx, int *cy) { imglist_t *l = IL(h); if (!l) return 0; if (cx) *cx = l->cx; if (cy) *cy = l->cy; return 1; }
static BOOL WINAPI ImageList_SetIconSize(HANDLE h, int cx, int cy) {
    imglist_t *l = IL(h); if (!l || cx <= 0 || cy <= 0) return 0;
    if (l->strip) canvas_free(l->strip);
    l->strip = NULL; l->cap = 0; l->n = 0; l->cx = cx; l->cy = cy; il_reserve(l, 4);
    return 1;
}
static COLORREF WINAPI ImageList_SetBkColor(HANDLE h, COLORREF c) { imglist_t *l = IL(h); if (!l) return 0xFFFFFFFF; COLORREF o = l->bk == 0xFFFFFFFF ? 0xFFFFFFFF : colref(l->bk); l->bk = c == 0xFFFFFFFF ? 0xFFFFFFFF : cref(c); return o; }
static COLORREF WINAPI ImageList_GetBkColor(HANDLE h) { imglist_t *l = IL(h); return !l || l->bk == 0xFFFFFFFF ? 0xFFFFFFFF : colref(l->bk); }
static BOOL WINAPI ImageList_SetOverlayImage(HANDLE h, int i, int ov) { imglist_t *l = IL(h); if (!l || ov < 1 || ov > 15) return 0; l->overlay[ov] = i; return 1; }
/* ILD_NORMAL 0, ILD_TRANSPARENT 1, ILD_MASK 0x10, ILD_BLEND25/50 2/4, the overlay index in bits 8-11 */
static void il_draw(imglist_t *l, int i, dc_t *d, int x, int y, int w, int h, uint32_t bg, UINT style) {
    if (!l || i < 0 || i >= l->n || !d) return;
    if (!w) w = l->cx; if (!h) h = l->cy;
    rect_t sv; dc_begin(d, &sv);
    bool blend = style & 6;
    for (int yy = 0; yy < MIN(h, l->cy); yy++) for (int xx = 0; xx < MIN(w, l->cx); xx++) {
        uint32_t p = l->strip->px[(size_t)yy * l->strip->stride + i * l->cx + xx];
        if (!(p >> 24)) { if (!(style & 1) && bg != 0xFFFFFFFF && !l->masked) put_pixel(d->surf, d->ox + x + xx, d->oy + y + yy, bg); continue; }
        if (style & 0x10) { put_pixel(d->surf, d->ox + x + xx, d->oy + y + yy, BLACK); continue; }
        if (blend) p = mix(p, T.select, 128);
        put_pixel(d->surf, d->ox + x + xx, d->oy + y + yy, p | 0xFF000000u);
    }
    dc_end(d, sv);
    int ov = (style >> 8) & 15;
    if (ov && l->overlay[ov] != i) il_draw(l, l->overlay[ov], d, x, y, w, h, bg, style & 0xFF);
}
static BOOL WINAPI ImageList_Draw(HANDLE h, int i, HDC hdc, int x, int y, UINT style) { imglist_t *l = IL(h); if (!l) return 0; il_draw(l, i, DC(hdc), x, y, 0, 0, l->bk, style); return 1; }
static BOOL WINAPI ImageList_DrawEx(HANDLE h, int i, HDC hdc, int x, int y, int dx, int dy, COLORREF bg, COLORREF fg, UINT style) {
    imglist_t *l = IL(h); if (!l) return 0;
    uint32_t b = bg == 0xFFFFFFFF ? 0xFFFFFFFF : bg == 0xFF000000 ? l->bk : cref(bg);
    il_draw(l, i, DC(hdc), x, y, dx, dy, b, style); return 1;
}
typedef struct { DWORD size; HANDLE himl; int i; HDC hdc; int x, y, cx, cy, xbm, ybm; COLORREF bk, fg; UINT style; DWORD rop, state, frame; COLORREF effect; } ILDP;
static BOOL WINAPI ImageList_DrawIndirect(ILDP *p) { return ImageList_DrawEx(p->himl, p->i, p->hdc, p->x, p->y, p->cx, p->cy, p->bk, p->fg, p->style); }
static HANDLE WINAPI ImageList_GetIcon(HANDLE h, int i, UINT f) { return IL(h) ? 0x20002 : 0; }
typedef struct { HGDIOBJ img, mask; int u1, u2; RECT32 rc; } IMAGEINFO;
static BOOL WINAPI ImageList_GetImageInfo(HANDLE h, int i, IMAGEINFO *ii) { imglist_t *l = IL(h); if (!l || i < 0 || i >= l->n) return 0; memset(ii, 0, sizeof *ii); ii->rc.l = i * l->cx; ii->rc.r = ii->rc.l + l->cx; ii->rc.b = l->cy; return 1; }
static HANDLE WINAPI ImageList_Duplicate(HANDLE h) {
    imglist_t *l = IL(h); if (!l) return 0;
    imglist_t *c = (imglist_t *)ImageList_Create(l->cx, l->cy, l->masked, l->n, l->grow);
    il_reserve(c, l->n); blit(c->strip, 0, 0, l->strip, 0, 0, l->cx * l->n, l->cy); c->n = l->n; c->bk = l->bk;
    return (HANDLE)c;
}
static BOOL WINAPI ImageList_Copy(HANDLE dst, int id, HANDLE src, int is, UINT f) {
    imglist_t *d = IL(dst), *s = IL(src); if (!d || !s || is >= s->n || id >= d->n) return 0;
    blit(d->strip, id * d->cx, 0, s->strip, is * s->cx, 0, MIN(d->cx, s->cx), MIN(d->cy, s->cy));
    return 1;
}
static HANDLE WINAPI ImageList_LoadImageA(HINSTANCE h, const char *n, int cx, int grow, COLORREF key, UINT type, UINT f) {
    HGDIOBJ b = LoadImageA2(h, n, 0, 0, 0, f); gdiobj_t *o = OBJ(b); if (!o || !o->bmp) return 0;
    HANDLE l = ImageList_Create(cx, o->bmp->h, key != 0xFFFFFFFF, 4, grow);
    if (key != 0xFFFFFFFF) ImageList_AddMasked(l, b, key); else ImageList_Add(l, b, 0);
    DeleteObject2(b); return l;
}
static HANDLE WINAPI ImageList_LoadImageW(HINSTANCE h, const WCHAR *n, int cx, int grow, COLORREF key, UINT type, UINT f) {
    HGDIOBJ b = LoadImageW2(h, n, 0, 0, 0, f); gdiobj_t *o = OBJ(b); if (!o || !o->bmp) return 0;
    HANDLE l = ImageList_Create(cx, o->bmp->h, key != 0xFFFFFFFF, 4, grow);
    if (key != 0xFFFFFFFF) ImageList_AddMasked(l, b, key); else ImageList_Add(l, b, 0);
    DeleteObject2(b); return l;
}
/* drag images: nothing is drawn while dragging, but the calls succeed */
static BOOL WINAPI il_true(void) { return 1; }
static BOOL WINAPI ImageList_BeginDrag(HANDLE h, int i, int x, int y) { return IL(h) != NULL; }
static BOOL WINAPI ImageList_DragEnter(HWND h, int x, int y) { return 1; }
static BOOL WINAPI ImageList_DragMove(int x, int y) { return 1; }
static BOOL WINAPI ImageList_DragLeave(HWND h) { return 1; }
static BOOL WINAPI ImageList_DragShowNolock(BOOL s) { return 1; }
static void WINAPI ImageList_EndDrag(void) {}
static HANDLE WINAPI ImageList_GetDragImage(void *pt, void *hot) { return 0; }
static BOOL WINAPI ImageList_SetDragCursorImage(HANDLE h, int i, int x, int y) { return 1; }
static HANDLE WINAPI ImageList_Read(void *stream) { return 0; }
static BOOL WINAPI ImageList_Write(HANDLE h, void *stream) { return 0; }
static HANDLE WINAPI ImageList_Merge(HANDLE a, int i, HANDLE b, int j, int dx, int dy) { return ImageList_Duplicate(a); }
/* flat scroll bars are ordinary scroll bars */
static BOOL WINAPI InitializeFlatSB(HWND h) { return 1; }
static LONG WINAPI UninitializeFlatSB(HWND h) { return 0; }
static int WINAPI FlatSB_SetScrollInfo(HWND h, int bar, SCROLLINFO_ *si, BOOL r) { return SetScrollInfo(h, bar, si, r); }
static BOOL WINAPI FlatSB_GetScrollInfo(HWND h, int bar, SCROLLINFO_ *si) { return GetScrollInfo(h, bar, si); }
static int WINAPI FlatSB_GetScrollPos(HWND h, int bar) { return GetScrollPos(h, bar); }
static int WINAPI FlatSB_SetScrollPos(HWND h, int bar, int pos, BOOL r) { SCROLLINFO_ si; memset(&si, 0, sizeof si); si.size = sizeof si; si.mask = 4; si.pos = pos; return SetScrollInfo(h, bar, &si, r); }
static BOOL WINAPI FlatSB_SetScrollProp(HWND h, UINT i, intptr_t v, BOOL r) { return 1; }
static BOOL WINAPI FlatSB_ShowScrollBar(HWND h, int bar, BOOL s) { return 1; }
static BOOL WINAPI FlatSB_EnableScrollBar(HWND h, int bar, UINT a) { return 1; }
static BOOL WINAPI _TrackMouseEvent(void *t) { return 1; }
static HWND WINAPI CreateStatusWindowA(LONG style, const char *text, HWND parent, UINT id) {
    RECT32 r = { 0 }; GetClientRect(parent, &r);
    return CreateWindowExA(0, "STATIC", text, (style | WS_CHILD | WS_VISIBLE) & ~0xFFu, 0, r.b - 22, r.r, 22, parent, (HANDLE)(uintptr_t)id, P->base, NULL);
}
static HWND WINAPI CreateStatusWindowW(LONG style, const WCHAR *text, HWND parent, UINT id) { char a[256]; um_w2a(text, a, sizeof a); return CreateStatusWindowA(style, a, parent, id); }
static void WINAPI DrawStatusTextA(HDC h, RECT32 *r, const char *t, UINT f) { dc_t *d = DC(h); if (!d) return; rect_t sv; dc_begin(d, &sv); fill_rect(d->surf, d->ox + r->l, d->oy + r->t, r->r - r->l, r->b - r->t, T.face); dc_end(d, sv); RECT32 rr = *r; rr.l += 4; DrawTextA(h, t ? t : "", -1, &rr, DT_SINGLELINE | DT_VCENTER); }
static void WINAPI DrawStatusTextW(HDC h, RECT32 *r, const WCHAR *t, UINT f) { char a[256]; um_w2a(t, a, sizeof a); DrawStatusTextA(h, r, a, f); }
static LONG WINAPI DllGetVersion_cc(DWORD *info) { if (info && info[0] >= 20) { info[1] = 6; info[2] = 0; info[3] = 7601; info[4] = 2; } return 0; }
static LONG WINAPI LoadIconMetric(HINSTANCE h, const WCHAR *n, int m, HANDLE *out) { *out = 0x20002; return 0; }
static LONG WINAPI TaskDialogIndirect(const void *c, int *b, int *r, BOOL *v) { if (b) *b = 1; if (r) *r = 0; if (v) *v = 0; return 0; }
static LONG WINAPI TaskDialog(HWND h, HINSTANCE i, const WCHAR *t, const WCHAR *m, const WCHAR *c, DWORD btn, const WCHAR *icon, int *res) {
    char a[256], b[512], cc[512]; um_w2a(t, a, sizeof a); um_w2a(m, b, sizeof b); um_w2a(c, cc, sizeof cc);
    char msg[1100]; snprintf(msg, sizeof msg, "%s%s%s", b, cc[0] ? "\n\n" : "", cc);
    int r = MessageBoxA(h, msg, a[0] ? a : "Message", (btn & 6) == 6 ? 4 : (btn & 8) ? 1 : 0);
    if (res) *res = r; return 0;
}
/* subclassing helpers (comctl32 6): the window procedure chain kept per window */
typedef struct { HWND h; void *fn; uintptr_t id, data; void *prev; } subcls_t;
static subcls_t subs[64];
static LRESULT WINAPI DefSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l);
static LRESULT WINAPI subclass_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    for (int i = 63; i >= 0; i--) if (subs[i].h == h && subs[i].fn) {
        return ((LRESULT (WINAPI *)(HWND, UINT, WPARAM, LPARAM, uintptr_t, uintptr_t))subs[i].fn)(h, m, w, l, subs[i].id, subs[i].data);
    }
    return DefWindowProcA(h, m, w, l);
}
static BOOL WINAPI SetWindowSubclass(HWND h, void *fn, uintptr_t id, uintptr_t data) {
    w32win_t *w = W(h); if (!w) return 0;
    for (int i = 0; i < 64; i++) if (subs[i].h == h && subs[i].fn == fn && subs[i].id == id) { subs[i].data = data; return 1; }
    for (int i = 0; i < 64; i++) if (!subs[i].h) {
        subs[i].h = h; subs[i].fn = fn; subs[i].id = id; subs[i].data = data; subs[i].prev = w->wndproc;
        w->wndproc = (void *)subclass_proc;
        return 1;
    }
    return 0;
}
static BOOL WINAPI GetWindowSubclass(HWND h, void *fn, uintptr_t id, uintptr_t *data) { for (int i = 0; i < 64; i++) if (subs[i].h == h && subs[i].fn == fn && subs[i].id == id) { if (data) *data = subs[i].data; return 1; } return 0; }
static BOOL WINAPI RemoveWindowSubclass(HWND h, void *fn, uintptr_t id) {
    for (int i = 0; i < 64; i++) if (subs[i].h == h && subs[i].fn == fn && subs[i].id == id) {
        w32win_t *w = W(h); bool last = true;
        for (int k = 0; k < 64; k++) if (k != i && subs[k].h == h) last = false;
        if (w && last) w->wndproc = subs[i].prev;
        subs[i].h = 0; return 1;
    }
    return 0;
}
/* the next procedure in the chain: the one installed before the newest that is running */
static LRESULT WINAPI DefSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    void *prev = NULL;
    for (int i = 0; i < 64; i++) if (subs[i].h == h) { prev = subs[i].prev; break; }
    if (prev && prev != (void *)subclass_proc) return call_wndproc(prev, h, m, w, l);
    return DefWindowProcA(h, m, w, l);
}

#define U(n) { "user32", #n, (void *)n }
#define UA(n, f) { "user32", n, (void *)f }
#define GD(n) { "gdi32", #n, (void *)n }
#define GDA(n, f) { "gdi32", n, (void *)f }
const w32_api_t usermore_api[] = {
    /* gdi32 */
    GD(CreateDIBSection), GD(CreateDIBitmap), GD(SetDIBits), GDA("GetDIBits", GetDIBits2), GD(StretchDIBits), GD(SetDIBitsToDevice), GD(GetBitmapBits), GD(SetBitmapBits),
    GD(GetDIBColorTable), GD(SetDIBColorTable), GD(CreateFontW), GD(CreateFontIndirectW), GDA("CreateFontIndirectA", CreateFontIndirectA2), GD(CreateFontIndirectExW),
    GD(GetTextMetricsW), GDA("GetTextMetricsA", GetTextMetricsA2), GD(GetTextFaceW), GD(GetTextFaceA), GDA("GetObjectA", GetObjectA2), GD(GetObjectW), GD(GetObjectType),
    GD(CreatePenIndirect), GD(ExtCreatePen), GD(CreatePatternBrush), GD(CreateDIBPatternBrushPt), GD(SetDCBrushColor), GD(SetDCPenColor), GD(GetDCBrushColor),
    GD(GetDCPenColor), GD(GetNearestColor), GD(GdiFlush), GD(GdiSetBatchLimit), GD(CreateHalftonePalette), GD(ResizePalette), GD(GetPaletteEntries), 
    GD(GetSystemPaletteEntries), GD(GetSystemPaletteUse), GD(AnimatePalette),
    GD(CreateRectRgn), GD(CreateRectRgnIndirect), GD(CreateEllipticRgn), GD(CreateEllipticRgnIndirect), GD(CreateRoundRectRgn), GD(CreatePolygonRgn), GD(SetRectRgn),
    GD(CombineRgn), GD(OffsetRgn), GD(GetRgnBox), GD(PtInRegion), GD(RectInRegion), GD(EqualRgn), GD(FillRgn), GD(PaintRgn), GD(FrameRgn), GD(InvertRgn), GD(GetRegionData),
    GD(ExtCreateRegion),
    GD(SetViewportOrgEx), GD(OffsetViewportOrgEx), GD(GetViewportOrgEx), GD(SetWindowOrgEx), GD(OffsetWindowOrgEx), GD(GetWindowOrgEx), GD(SetViewportExtEx),
    GD(SetWindowExtEx), GD(GetViewportExtEx), GD(GetWindowExtEx), GD(ScaleViewportExtEx), GD(ScaleWindowExtEx), GDA("SetMapMode", SetMapMode2), GD(GetMapMode), GD(DPtoLP),
    GD(LPtoDP), GD(SetStretchBltMode), GD(GetStretchBltMode), GD(SetBrushOrgEx), GD(GetBrushOrgEx), GD(GetTextAlign), GD(GetCurrentPositionEx), GD(GetROP2),
    GD(GetPolyFillMode), GD(SetPolyFillMode), GD(SetArcDirection), GD(GetArcDirection), GD(SetGraphicsMode), GD(GetGraphicsMode), GD(SetWorldTransform),
    GD(ModifyWorldTransform), GD(GetWorldTransform), GD(SetTextCharacterExtra), GD(GetTextCharacterExtra), GD(SetTextJustification), GD(SetLayout), GD(GetLayout),
    GDA("SelectClipRgn", SelectClipRgn2), GD(ExtSelectClipRgn), GDA("IntersectClipRect", IntersectClipRect2), GD(GetClipBox), GD(GetClipRgn), GD(GetRandomRgn), GD(PtVisible),
    GD(RectVisible), GD(SaveDC), GD(RestoreDC), GDA("SelectObject", SelectObject2), GDA("DeleteObject", DeleteObject2), GDA("DeleteDC", DeleteDC2),
    GD(Arc), GD(ArcTo), GD(Pie), GD(Chord), GD(AngleArc), GD(PolyBezier), GD(PolyBezierTo), GD(PolylineTo), GD(PolyPolyline), GD(PolyPolygon), GD(ExtFloodFill), GD(FloodFill),
    GD(MaskBlt), GD(PlgBlt), GD(GetGlyphOutlineW), GDA("GetGlyphOutlineA", GetGlyphOutlineW), GD(GetFontData), 
    
    GD(EnumFontFamiliesExW), GD(EnumFontFamiliesExA), GD(EnumFontFamiliesW), GD(EnumFontFamiliesA), GD(EnumFontsW), GD(EnumFontsA), GD(AddFontResourceW),
    GD(AddFontResourceExW), GD(AddFontMemResourceEx), GD(RemoveFontResourceW), GD(RemoveFontMemResourceEx), GD(GetFontLanguageInfo), 
    GD(CreateDCW), GD(CreateDCA), GD(CreateICW), GD(CreateICA), GD(StartDocA), GD(StartDocW), GD(EndDoc), GD(AbortDoc), GD(StartPage), GD(EndPage), GD(SetAbortProc),
    GD(Escape), GD(ExtEscape), GD(CopyEnhMetaFileW), GD(DeleteEnhMetaFile), GD(GetEnhMetaFileBits), GD(GetEnhMetaFileDescriptionW), GD(GetEnhMetaFileHeader),
    GD(GetEnhMetaFilePaletteEntries), GD(GetWinMetaFileBits), GD(PlayEnhMetaFile), GD(SetEnhMetaFileBits), GD(SetWinMetaFileBits), GD(CreateEnhMetaFileW),
    GD(CloseEnhMetaFile), GD(GetDeviceGammaRamp), GD(SetDeviceGammaRamp), GD(GetICMProfileW), GD(SetICMMode), GD(GdiAlphaBlend), GD(GdiTransparentBlt), GD(GdiGradientFill),
    { "msimg32", "AlphaBlend", (void *)GdiAlphaBlend }, { "msimg32", "TransparentBlt", (void *)GdiTransparentBlt }, { "msimg32", "GradientFill", (void *)GdiGradientFill },
    GDA("CreateMetaFileW", zero_h), GDA("CloseMetaFile", zero_h), GDA("DeleteMetaFile", zero_u),
    /* user32 */
    U(LoadBitmapA), U(LoadBitmapW), UA("LoadImageA", LoadImageA2), UA("LoadImageW", LoadImageW2), U(CreateIcon), U(CreateIconFromResource), U(CreateIconFromResourceEx),
    U(LookupIconIdFromDirectoryEx), U(GetCursor), U(GetCursorInfo), U(CharUpperBuffW), U(CharLowerBuffW), U(CharUpperBuffA), U(CharLowerBuffA), UA("CharUpperW", CharUpperW2),
    UA("CharLowerW", CharLowerW2), U(CharNextW), U(CharPrevW), UA("CharNextA", CharNextA2), UA("CharPrevA", CharPrevA2), U(IsCharAlphaW), U(IsCharAlphaNumericW),
    U(IsCharLowerW), U(IsCharUpperW), U(IsCharLowerA), U(IsCharUpperA), U(wvsprintfW), U(TabbedTextOutA),
    U(TabbedTextOutW), U(GetTabbedTextExtentA), U(GetTabbedTextExtentW), U(GrayStringA), U(GrayStringW), U(DrawStateA), U(DrawStateW), U(DrawFrameControl),
    U(DrawAnimatedRects), U(SubtractRect), U(GetDCEx), U(ScrollDC), U(ScrollWindowEx), UA("ReleaseDC", ReleaseDC2),
    UA("CheckMenuItem", CheckMenuItem2), UA("EnableMenuItem", EnableMenuItem2), U(CheckMenuRadioItem), U(GetMenuState), U(GetMenuStringA), U(GetMenuStringW),
    U(GetMenuItemInfoA), U(GetMenuItemInfoW), U(SetMenuItemInfoA), U(SetMenuItemInfoW), U(InsertMenuItemA), U(InsertMenuItemW), U(SetMenuDefaultItem), U(GetMenuDefaultItem),
    U(SetMenuInfo), U(GetMenuInfo), U(SetMenuItemBitmaps), U(GetMenuCheckMarkDimensions), U(GetMenuItemRect), U(GetMenuBarInfo), U(EndMenu), U(MenuItemFromPoint),
    U(LoadMenuIndirectW), U(LoadMenuIndirectA), 
    U(SetPropA), U(SetPropW), U(GetPropA), U(GetPropW), U(RemovePropA), U(RemovePropW), U(EnumPropsExW),
    U(GetClassInfoExA), U(GetClassInfoExW), U(GetClassLongW), U(SetClassLongW), U(FindWindowExA), U(FindWindowExW), U(GetTopWindow), U(GetLastActivePopup),
    U(ShowOwnedPopups), U(SetLayeredWindowAttributes), U(GetLayeredWindowAttributes), U(UpdateLayeredWindow), U(SetWindowRgn), U(GetWindowRgn), U(GetWindowRgnBox),
    U(AttachThreadInput), U(WaitForInputIdle), U(GetMessageExtraInfo), U(SetMessageExtraInfo), U(NotifyWinEvent), U(SetWinEventHook), U(UnhookWinEvent),
    U(SetDebugErrorLevel), U(RegisterDeviceNotificationW), U(RegisterDeviceNotificationA), U(UnregisterDeviceNotification), U(GetProcessWindowStation),
    U(GetThreadDesktop), U(GetUserObjectInformationW), U(GetUserObjectInformationA), U(GetRawInputData), U(GetRawInputDeviceList), U(GetRawInputDeviceInfoA),
    U(GetRawInputDeviceInfoW), U(RegisterRawInputDevices), U(GetRegisteredRawInputDevices), U(ReuseDDElParam), U(UnpackDDElParam), U(PackDDElParam), U(FreeDDElParam),
    U(GetScrollRange), U(GetScrollBarInfo), U(GetComboBoxInfo), U(MapVirtualKeyW), U(MapVirtualKeyExW), UA("MapVirtualKeyA", MapVirtualKeyA2), UA("MapVirtualKeyExA", MapVirtualKeyExW),
    U(GetKeyNameTextA), U(GetKeyNameTextW), U(GetKeyboardLayoutNameA), U(GetKeyboardLayoutNameW), U(LoadKeyboardLayoutW), U(LoadKeyboardLayoutA), U(ActivateKeyboardLayout),
    U(GetKeyboardType), U(keybd_event), U(SendInput), U(mouse_event), U(TrackMouseEvent), U(BlockInput), U(CountClipboardFormats), U(GetClipboardFormatNameA),
    U(GetClipboardFormatNameW), U(GetClipboardSequenceNumber), U(SetClipboardViewer), U(ChangeClipboardChain), U(GetClipboardViewer), U(AddClipboardFormatListener),
    U(RemoveClipboardFormatListener), U(EnumDisplayDevicesA), U(EnumDisplayDevicesW), U(EnumDisplaySettingsA), U(EnumDisplaySettingsW), U(EnumDisplaySettingsExW),
    U(ChangeDisplaySettingsExW), U(ChangeDisplaySettingsExA), U(ChangeDisplaySettingsW), U(ChangeDisplaySettingsA), U(EnumDisplayMonitors), U(MessageBoxExW),
    U(MessageBoxTimeoutA), U(MsgWaitForMultipleObjectsEx), U(DefFrameProcA), U(DefFrameProcW), U(DefMDIChildProcA), U(DefMDIChildProcW), 
    U(LockWorkStation), U(ExitWindowsEx), U(IsHungAppWindow), 
    U(ChangeWindowMessageFilter), U(ChangeWindowMessageFilterEx),
    { NULL, NULL, NULL }
};
#define CC(n) { "comctl32", #n, (void *)n }
#define CCA(n, f) { "comctl32", n, (void *)f }
const w32_api_t comctl_api[] = {
    CC(ImageList_Create), CC(ImageList_Destroy), CC(ImageList_Add), CC(ImageList_AddMasked), CC(ImageList_Replace), CC(ImageList_ReplaceIcon), CC(ImageList_Remove),
    CC(ImageList_GetImageCount), CC(ImageList_SetImageCount), CC(ImageList_GetIconSize), CC(ImageList_SetIconSize), CC(ImageList_SetBkColor), CC(ImageList_GetBkColor),
    CC(ImageList_SetOverlayImage), CC(ImageList_Draw), CC(ImageList_DrawEx), CC(ImageList_DrawIndirect), CC(ImageList_GetIcon), CC(ImageList_GetImageInfo),
    CC(ImageList_Duplicate), CC(ImageList_Copy), CC(ImageList_LoadImageA), CC(ImageList_LoadImageW), CCA("ImageList_LoadImage", ImageList_LoadImageA),
    CC(ImageList_BeginDrag), CC(ImageList_DragEnter), CC(ImageList_DragMove), CC(ImageList_DragLeave), CC(ImageList_DragShowNolock), CC(ImageList_EndDrag),
    CC(ImageList_GetDragImage), CC(ImageList_SetDragCursorImage), CC(ImageList_Read), CC(ImageList_Write), CC(ImageList_Merge),
    CC(InitializeFlatSB), CC(UninitializeFlatSB), CC(FlatSB_SetScrollInfo), CC(FlatSB_GetScrollInfo), CC(FlatSB_GetScrollPos), CC(FlatSB_SetScrollPos),
    CC(FlatSB_SetScrollProp), CC(FlatSB_ShowScrollBar), CC(FlatSB_EnableScrollBar), CC(_TrackMouseEvent), CC(CreateStatusWindowA), CC(CreateStatusWindowW),
    CCA("CreateStatusWindow", CreateStatusWindowA), CC(DrawStatusTextA), CC(DrawStatusTextW), CCA("DllGetVersion", DllGetVersion_cc), CC(LoadIconMetric),
    CC(TaskDialogIndirect), CC(TaskDialog), CC(SetWindowSubclass), CC(GetWindowSubclass), CC(RemoveWindowSubclass), CC(DefSubclassProc),
    CCA("Ordinal410", SetWindowSubclass), CCA("Ordinal411", GetWindowSubclass), CCA("Ordinal412", RemoveWindowSubclass), CCA("Ordinal413", DefSubclassProc),
    CCA("Ordinal17", il_true),                       /* InitCommonControls by ordinal */
    { NULL, NULL, NULL }
};
