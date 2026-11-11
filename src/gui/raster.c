/* Software path rasteriser. See raster.h.
 *
 * The path is a list of contours of 26.6 fixed-point points; curves are
 * flattened as they are added. Filling walks a scanline at eight vertical
 * subsamples per pixel, accumulates exact horizontal coverage in 1/64 pixel
 * units, and blends the resulting coverage. Strokes are built as a union of
 * one quad per segment plus a round disc at every vertex, then filled with the
 * nonzero rule. */
#include "raster.h"
#include "kernel.h"

typedef struct { int32_t *v; int n, cap; } pts_t;

struct raster_path {
    pts_t p;                    /* x,y pairs, 26.6 */
    int *start;                 /* contour start, in points */
    int ns, scap;
    int32_t pen_x, pen_y;
};

static void *xalloc(size_t n) { return kmalloc(n); }

static int pts_push(raster_path_t *r, int32_t x, int32_t y)
{
    if (r->p.n == r->p.cap) {
        int cap = r->p.cap ? r->p.cap * 2 : 64;
        int32_t *v = krealloc(r->p.v, (size_t)cap * 2 * sizeof *v);
        if (!v) return 0;
        r->p.v = v; r->p.cap = cap;
    }
    r->p.v[r->p.n * 2] = x;
    r->p.v[r->p.n * 2 + 1] = y;
    r->p.n++;
    return 1;
}

static int contour_start(raster_path_t *r)
{
    if (r->ns == r->scap) {
        int cap = r->scap ? r->scap * 2 : 8;
        int *s = krealloc(r->start, (size_t)cap * sizeof *s);
        if (!s) return 0;
        r->start = s; r->scap = cap;
    }
    r->start[r->ns] = r->p.n;
    r->ns++;
    return 1;
}

raster_path_t *raster_path_new(void)
{
    raster_path_t *r = kzalloc(sizeof *r);
    if (!r) return NULL;
    contour_start(r);
    return r;
}

void raster_path_free(raster_path_t *r)
{
    if (!r) return;
    kfree(r->p.v);
    kfree(r->start);
    kfree(r);
}

void raster_reset(raster_path_t *r)
{
    if (!r) return;
    r->p.n = 0; r->ns = 0; r->pen_x = r->pen_y = 0;
    contour_start(r);
}

void raster_move_to(raster_path_t *r, int32_t x, int32_t y)
{
    if (!r) return;
    if (r->p.n == 0 || (r->start[r->ns - 1] < r->p.n && r->p.n > r->start[r->ns - 1])) {
        contour_start(r);
    }
    pts_push(r, x, y);
    r->pen_x = x; r->pen_y = y;
}

void raster_line_to(raster_path_t *r, int32_t x, int32_t y)
{
    if (!r) return;
    if (r->p.n == 0) { raster_move_to(r, x, y); return; }
    pts_push(r, x, y);
    r->pen_x = x; r->pen_y = y;
}

static int32_t lerp(int32_t a, int32_t b, int t, int n)
{
    return a + (int32_t)(((int64_t)(b - a) * t) / n);
}

void raster_quad_to(raster_path_t *r, int32_t cx, int32_t cy, int32_t x, int32_t y)
{
    if (!r) return;
    int32_t x0 = r->pen_x, y0 = r->pen_y;
    int n = 12;
    for (int i = 1; i <= n; i++) {
        int32_t ax = lerp(x0, cx, i, n), ay = lerp(y0, cy, i, n);
        int32_t bx = lerp(cx, x, i, n), by = lerp(cy, y, i, n);
        int32_t px = lerp(ax, bx, i, n), py = lerp(ay, by, i, n);
        pts_push(r, px, py);
    }
    r->pen_x = x; r->pen_y = y;
}

void raster_cubic_to(raster_path_t *r, int32_t c1x, int32_t c1y,
                     int32_t c2x, int32_t c2y, int32_t x, int32_t y)
{
    if (!r) return;
    int32_t x0 = r->pen_x, y0 = r->pen_y;
    int n = 16;
    for (int i = 1; i <= n; i++) {
        int32_t ax = lerp(x0, c1x, i, n), ay = lerp(y0, c1y, i, n);
        int32_t bx = lerp(c1x, c2x, i, n), by = lerp(c1y, c2y, i, n);
        int32_t cx = lerp(c2x, x, i, n), cy = lerp(c2y, y, i, n);
        int32_t dx = lerp(ax, bx, i, n), dy = lerp(ay, by, i, n);
        int32_t ex = lerp(bx, cx, i, n), ey = lerp(by, cy, i, n);
        int32_t px = lerp(dx, ex, i, n), py = lerp(dy, ey, i, n);
        pts_push(r, px, py);
    }
    r->pen_x = x; r->pen_y = y;
}

void raster_close(raster_path_t *r) { (void)r; }   /* contours are closed when filled */

void raster_add_rect(raster_path_t *r, int x, int y, int w, int h)
{
    if (!r || w <= 0 || h <= 0) return;
    raster_move_to(r, RASTER_FIX(x), RASTER_FIX(y));
    raster_line_to(r, RASTER_FIX(x + w), RASTER_FIX(y));
    raster_line_to(r, RASTER_FIX(x + w), RASTER_FIX(y + h));
    raster_line_to(r, RASTER_FIX(x), RASTER_FIX(y + h));
    raster_line_to(r, RASTER_FIX(x), RASTER_FIX(y));
}

void raster_add_round_rect(raster_path_t *r, int x, int y, int w, int h, int radius)
{
    if (!r || w <= 0 || h <= 0) return;
    int m = MIN(w, h) / 2;
    int rad = CLAMP(radius, 0, m);
    if (!rad) { raster_add_rect(r, x, y, w, h); return; }
    int k = rad * 55 / 100;                          /* cubic approximation of a circle */
    raster_move_to(r, RASTER_FIX(x + rad), RASTER_FIX(y));
    raster_line_to(r, RASTER_FIX(x + w - rad), RASTER_FIX(y));
    raster_cubic_to(r, RASTER_FIX(x + w - rad + k), RASTER_FIX(y),
                       RASTER_FIX(x + w), RASTER_FIX(y + rad - k),
                       RASTER_FIX(x + w), RASTER_FIX(y + rad));
    raster_line_to(r, RASTER_FIX(x + w), RASTER_FIX(y + h - rad));
    raster_cubic_to(r, RASTER_FIX(x + w), RASTER_FIX(y + h - rad + k),
                       RASTER_FIX(x + w - rad + k), RASTER_FIX(y + h),
                       RASTER_FIX(x + w - rad), RASTER_FIX(y + h));
    raster_line_to(r, RASTER_FIX(x + rad), RASTER_FIX(y + h));
    raster_cubic_to(r, RASTER_FIX(x + rad - k), RASTER_FIX(y + h),
                       RASTER_FIX(x), RASTER_FIX(y + h - rad + k),
                       RASTER_FIX(x), RASTER_FIX(y + h - rad));
    raster_line_to(r, RASTER_FIX(x), RASTER_FIX(y + rad));
    raster_cubic_to(r, RASTER_FIX(x), RASTER_FIX(y + rad - k),
                       RASTER_FIX(x + rad - k), RASTER_FIX(y),
                       RASTER_FIX(x + rad), RASTER_FIX(y));
}

/* 16 points around a circle, sin/cos scaled by 256 */
static const int16_t circle_pt[16][2] = {
    { 256, 0 }, { 237, 98 }, { 181, 181 }, { 98, 237 },
    { 0, 256 }, { -98, 237 }, { -181, 181 }, { -237, 98 },
    { -256, 0 }, { -237, -98 }, { -181, -181 }, { -98, -237 },
    { 0, -256 }, { 98, -237 }, { 181, -181 }, { 237, -98 },
};

void raster_add_circle(raster_path_t *r, int cx, int cy, int radius)
{
    if (!r || radius <= 0) return;
    raster_move_to(r, RASTER_FIX(cx) + radius * 64, RASTER_FIX(cy));
    for (int i = 1; i < 16; i++)
        raster_line_to(r, RASTER_FIX(cx) + (int32_t)circle_pt[i][0] * radius * 64 / 256,
                          RASTER_FIX(cy) + (int32_t)circle_pt[i][1] * radius * 64 / 256);
    raster_line_to(r, RASTER_FIX(cx) + radius * 64, RASTER_FIX(cy));
}

/* ---------------- filling ---------------- */
typedef struct { int32_t x0, y0, x1, y1; int8_t dir; } edge_t;
typedef struct { int32_t x; int8_t dir; } cross_t;

static void sort_cross(cross_t *c, int n)
{
    for (int i = 1; i < n; i++) {
        cross_t v = c[i]; int j = i - 1;
        while (j >= 0 && c[j].x > v.x) { c[j + 1] = c[j]; j--; }
        c[j + 1] = v;
    }
}

/* Add `xa..xb` (26.6) to the per-pixel accumulator. */
static void add_span(int32_t *acc, int w, int32_t ox, int32_t xa, int32_t xb)
{
    if (xb <= xa) return;
    int32_t c0 = (xa >> 6) - ox, c1 = ((xb + 63) >> 6) - ox;
    if (c0 < 0) c0 = 0;
    if (c1 > w) c1 = w;
    for (int32_t c = c0; c < c1; c++) {
        int32_t px = (ox + c) << 6;
        int32_t lo = xa > px ? xa : px;
        int32_t hi = xb < px + 64 ? xb : px + 64;
        if (hi > lo) acc[c] += hi - lo;
    }
}

static uint64_t isqrt64(uint64_t v);

static uint32_t grad_color(uint32_t c0, uint32_t c1, int x, int y,
                           int x0, int y0, int x1, int y1, int radial)
{
    int t = 0;
    if (radial) {
        int64_t dx = x - x0, dy = y - y0;
        int64_t r = (int64_t)x1 - x0; r = r * r + (int64_t)(y1 - y0) * (y1 - y0);
        if (r > 0) {
            int64_t d2 = dx * dx + dy * dy;
            t = (int)isqrt64((uint64_t)d2 * 65025 / (uint64_t)r);
        }
    } else {
        int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
        int64_t d2 = dx * dx + dy * dy;
        if (d2 > 0) t = (int)(((int64_t)(x - x0) * dx + (int64_t)(y - y0) * dy) * 255 / d2);
    }
    t = CLAMP(t, 0, 255);
    int r = C_R(c0) + (C_R(c1) - C_R(c0)) * t / 255;
    int g = C_G(c0) + (C_G(c1) - C_G(c0)) * t / 255;
    int b = C_B(c0) + (C_B(c1) - C_B(c0)) * t / 255;
    int a = C_A(c0) + (C_A(c1) - C_A(c0)) * t / 255;
    return RGBA(r, g, b, a);
}

static void fill_path(canvas_t *c, const raster_path_t *p, uint32_t color,
                      int has_grad, int x0, int y0, int x1, int y1,
                      uint32_t c0, uint32_t c1, int radial, bool evenodd)
{
    if (!c || !p || p->ns <= 0) return;
    int ne = 0, ecap = 256;
    edge_t *e = xalloc((size_t)ecap * sizeof *e);
    if (!e) return;
    int32_t minx = 0x7FFFFFFF, miny = 0x7FFFFFFF, maxx = -0x7FFFFFFF, maxy = -0x7FFFFFFF;
    for (int k = 0; k < p->ns; k++) {
        int a = p->start[k], b = (k + 1 < p->ns) ? p->start[k + 1] : p->p.n;
        if (b - a < 2) continue;
        for (int i = a; i < b; i++) {
            int j = (i + 1 < b) ? i + 1 : a;
            int32_t ax = p->p.v[i * 2], ay = p->p.v[i * 2 + 1];
            int32_t bx = p->p.v[j * 2], by = p->p.v[j * 2 + 1];
            if (ax < minx) minx = ax; if (bx < minx) minx = bx;
            if (ax > maxx) maxx = ax; if (bx > maxx) maxx = bx;
            if (ay < miny) miny = ay; if (by < miny) miny = by;
            if (ay > maxy) maxy = ay; if (by > maxy) maxy = by;
            if (ay == by) continue;
            if (ne == ecap) {
                int cap = ecap * 2;
                edge_t *n = krealloc(e, (size_t)cap * sizeof *n);
                if (!n) { kfree(e); return; }
                e = n; ecap = cap;
            }
            e[ne].x0 = ax; e[ne].y0 = ay; e[ne].x1 = bx; e[ne].y1 = by;
            e[ne].dir = by > ay ? 1 : -1;
            ne++;
        }
    }
    if (!ne) { kfree(e); return; }

    int32_t left = MAX(minx >> 6, c->clip.x), right = MIN((maxx + 63) >> 6, c->clip.x + c->clip.w);
    int32_t top = MAX(miny >> 6, c->clip.y), bottom = MIN((maxy + 63) >> 6, c->clip.y + c->clip.h);
    int w = (int)(right - left);
    if (w <= 0 || bottom <= top) { kfree(e); return; }

    int32_t *acc = xalloc((size_t)w * sizeof *acc);
    cross_t *cr = xalloc((size_t)ne * sizeof *cr);
    if (!acc || !cr) { if (acc) kfree(acc); if (cr) kfree(cr); kfree(e); return; }
    for (int row = (int)top; row < (int)bottom; row++) {
        memset(acc, 0, (size_t)w * sizeof *acc);
        for (int s = 0; s < 8; s++) {
            int32_t y = (int32_t)row * 64 + (2 * s + 1) * 4;
            int nc = 0;
            for (int i = 0; i < ne; i++) {
                int32_t ya = e[i].y0, yb = e[i].y1;
                int32_t lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
                if (y < lo || y >= hi) continue;
                int64_t num = (int64_t)(y - ya) * (e[i].x1 - e[i].x0);
                cr[nc].x = e[i].x0 + (int32_t)(num / (yb - ya));
                cr[nc].dir = e[i].dir;
                nc++;
            }
            sort_cross(cr, nc);
            if (evenodd) {
                for (int i = 0; i + 1 < nc; i += 2) add_span(acc, w, left, cr[i].x, cr[i + 1].x);
            } else {
                int wind = 0, open = 0; int32_t span = 0;
                for (int i = 0; i < nc; i++) {
                    wind += cr[i].dir;
                    if (!open && wind != 0) { span = cr[i].x; open = 1; }
                    else if (open && wind == 0) { add_span(acc, w, left, span, cr[i].x); open = 0; }
                }
            }
        }
        for (int col = 0; col < w; col++) {
            int32_t v = (acc[col] * 255 + 256) / 512;
            if (v <= 0) continue;
            if (v > 255) v = 255;
            uint32_t colr = has_grad ? grad_color(c0, c1, left + col, row, x0, y0, x1, y1, radial) : color;
            int a = (int)(v * C_A(colr) / 255);
            if (a) blend_pixel(c, (int)left + col, row, colr, a);
        }
    }
    kfree(acc); kfree(cr); kfree(e);
}

void raster_fill(canvas_t *c, const raster_path_t *p, uint32_t color, bool evenodd)
{
    fill_path(c, p, color, 0, 0, 0, 0, 0, 0, 0, 0, evenodd);
}

void raster_fill_gradient(canvas_t *c, const raster_path_t *p,
                          int x0, int y0, int x1, int y1,
                          uint32_t c0, uint32_t c1, bool radial, bool evenodd)
{
    fill_path(c, p, 0, 1, x0, y0, x1, y1, c0, c1, radial, evenodd);
}

/* ---------------- stroke ---------------- */
static uint64_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Add a quad contour, reversing it when needed so every stroke piece winds the
 * same way (the nonzero rule then takes their union). */
static void add_quad(raster_path_t *r, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                     int32_t cx, int32_t cy, int32_t dx, int32_t dy)
{
    int64_t area = (int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax);
    if (area >= 0) {
        raster_move_to(r, ax, ay); raster_line_to(r, bx, by);
        raster_line_to(r, cx, cy); raster_line_to(r, dx, dy);
        raster_line_to(r, ax, ay);
    } else {
        raster_move_to(r, ax, ay); raster_line_to(r, dx, dy);
        raster_line_to(r, cx, cy); raster_line_to(r, bx, by);
        raster_line_to(r, ax, ay);
    }
}

void raster_stroke(canvas_t *c, const raster_path_t *p, int width, uint32_t color)
{
    if (!c || !p || width <= 0) return;
    raster_path_t *s = raster_path_new();
    if (!s) return;
    int32_t w2 = RASTER_FIX(width) / 2;
    for (int k = 0; k < p->ns; k++) {
        int a = p->start[k], b = (k + 1 < p->ns) ? p->start[k + 1] : p->p.n;
        if (b - a < 2) continue;
        for (int i = a; i < b; i++) {
            int j = (i + 1 < b) ? i + 1 : a;
            int32_t ax = p->p.v[i * 2], ay = p->p.v[i * 2 + 1];
            int32_t bx = p->p.v[j * 2], by = p->p.v[j * 2 + 1];
            int32_t dx = bx - ax, dy = by - ay;
            uint64_t len = isqrt64((uint64_t)((int64_t)dx * dx + (int64_t)dy * dy));
            if (!len) continue;
            int32_t nx = (int32_t)((-(int64_t)dy * w2) / (int64_t)len);
            int32_t ny = (int32_t)(((int64_t)dx * w2) / (int64_t)len);
            add_quad(s, ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny);
            int rad = width / 2;
            if (rad > 0) {
                raster_add_circle(s, ax >> 6, ay >> 6, rad);
                raster_add_circle(s, bx >> 6, by >> 6, rad);
            }
        }
    }
    raster_fill(c, s, color, false);
    raster_path_free(s);
}
