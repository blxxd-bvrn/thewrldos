/* Paint: pencil, brush, airbrush, eraser, fill, colour picker, text, line, rectangle,
 * rounded rectangle and ellipse (outline, outline and fill, or fill), rectangular
 * selections that move, cut, copy and paste, undo and redo, zoom with a grid, flip,
 * rotate, stretch, canvas size and crop; opens BMP, PNG, JPEG and GIF, saves BMP and PNG. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

void file_dialog(const char *title, const char *initial, bool save, const char *ext, void (*cb)(void *, const char *), void *ctx);

enum { T_SELECT, T_MAGNIFY, T_PENCIL, T_BRUSH, T_AIRBRUSH, T_ERASER, T_FILL, T_PICK, T_TEXT, T_LINE, T_RECT, T_RRECT, T_ELLIPSE, T_COUNT };
static const char *const tool_names[T_COUNT] = { "Select", "Magnifier", "Pencil", "Brush", "Airbrush", "Eraser", "Fill With Color", "Pick Color",
                                                 "Text", "Line", "Rectangle", "Rounded Rectangle", "Ellipse" };
#define UNDO_N 12
#define TEXT_MAX 1024
typedef struct {
    canvas_t *img, *snap, *view;             /* the picture; a copy while a shape is dragged; picture + selection + text, for showing */
    int tool; uint32_t fg, bg; uint32_t pal[28];
    bool drawing, right; int sx, sy, lx, ly;
    int size;                                 /* 0..3: the width choice of the tools that have one */
    int fillmode;                             /* shapes: 0 outline, 1 outline and fill, 2 fill */
    bool transparent;                         /* selections and text: the colour 2 pixels let the picture through */
    int font;                                 /* text: index in fonts[] */
    int zoom, scx, scy; bool grid;
    /* the selection, in picture coordinates; fl is its lifted pixels while it is moved or pasted */
    bool sel; rect_t sr; canvas_t *fl; int seldrag, offx, offy;
    /* text being typed at tx, ty */
    bool texting; int tx, ty; char text[TEXT_MAX]; int tlen;
    /* the canvas size handle and the scroll bar being dragged */
    bool resizing; int rw, rh; int hdrag, hdrag_off;
    int mx, my;                               /* the pointer over the picture, -1 when not */
    canvas_t *undo[UNDO_N], *redo[UNDO_N]; int nundo, nredo;
    char path[256]; bool modified, closing;
    int pal_edit;
    uint32_t rng;
} paint_t;
static canvas_t *paint_clip;                  /* Paint's clipboard, shared by its windows */

static const uint32_t def_palette[28] = { 0x000000, 0x808080, 0x800000, 0x808000, 0x008000, 0x008080, 0x000080, 0x800080, 0x808040, 0x004040, 0x0080FF, 0x004080, 0x8000FF, 0x804000,
                                          0xFFFFFF, 0xC0C0C0, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFF80, 0x00FF80, 0x80FFFF, 0x8080FF, 0xFF0080, 0xFF8040 };
static const int fonts[] = { FONT_SMALL, FONT_UI, FONT_UI_BOLD, FONT_LARGE, FONT_LARGE_BOLD, FONT_XL, FONT_HUGE };
static const char *const font_names[] = { "Small", "Normal", "Bold", "Large", "Large B", "X-Large", "Huge" };
#define NFONTS ((int)ARRAY_LEN(fonts))
static const int widths[4] = { 1, 2, 3, 5 };          /* lines and shape outlines */
static const int brushes[4] = { 1, 3, 5, 8 };
static const int erasers[4] = { 4, 6, 8, 10 };
static const int sprays[4] = { 4, 8, 12, 16 };
static const int zooms[4] = { 1, 2, 4, 8 };
#define PAL_H 46
#define TOOL_W 62
#define SB 16

static int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static uint32_t rnd(paint_t *p) { p->rng = p->rng * 1103515245u + 12345u; return p->rng >> 8; }
static int64_t isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t r = 0, bit = (int64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

/* ---------------- pictures ---------------- */
static canvas_t *clone(canvas_t *s) {
    canvas_t *c = canvas_new(s->w, s->h);
    if (c) for (int y = 0; y < s->h; y++) memcpy(c->px + y * c->stride, s->px + y * s->stride, s->w * 4);
    return c;
}
static canvas_t *crop(canvas_t *s, rect_t r) {
    canvas_t *c = canvas_new(r.w, r.h);
    if (c) for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
        int sx = r.x + x, sy = r.y + y;
        c->px[y * c->stride + x] = sx >= 0 && sy >= 0 && sx < s->w && sy < s->h ? s->px[sy * s->stride + sx] : WHITE;
    }
    return c;
}
/* the picture becomes c (Paint owns it); the old one goes to *old, or is freed */
static bool set_image(paint_t *p, canvas_t *c, canvas_t **old) {
    if (!c) return false;
    canvas_t *snap = canvas_new(c->w, c->h), *view = canvas_new(c->w, c->h);
    if (!snap || !view) { canvas_free(snap); canvas_free(view); canvas_free(c); msgbox("Paint", "There is not enough memory for a picture this size.", MBI_ERROR, MBX_OK, NULL, NULL); return false; }
    if (old) *old = p->img; else canvas_free(p->img);
    canvas_free(p->snap); canvas_free(p->view);
    p->img = c; p->snap = snap; p->view = view;
    return true;
}
static void stack_clear(canvas_t **s, int *n) { for (int i = 0; i < *n; i++) canvas_free(s[i]); *n = 0; }
static void stack_push(canvas_t **s, int *n, canvas_t *c) {
    if (!c) return;
    if (*n == UNDO_N) { canvas_free(s[0]); memmove(s, s + 1, (UNDO_N - 1) * sizeof *s); (*n)--; }
    s[(*n)++] = c;
}
/* call before every change of the picture */
static void push_undo(paint_t *p) {
    stack_push(p->undo, &p->nundo, clone(p->img));
    stack_clear(p->redo, &p->nredo);
    p->modified = true;
}
static void flatten_alpha(canvas_t *c) {
    for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) {
        uint32_t *q = &c->px[y * c->stride + x], a = C_A(*q);
        if (a != 255) *q = mix(WHITE, *q | 0xFF000000u, a);
    }
}

/* ---------------- the selection and the text ---------------- */
static void stamp_canvas(paint_t *p, canvas_t *dst, canvas_t *src, int dx, int dy) {
    for (int y = 0; y < src->h; y++) {
        int ty = dy + y; if (ty < 0 || ty >= dst->h) continue;
        for (int x = 0; x < src->w; x++) {
            int tx = dx + x; if (tx < 0 || tx >= dst->w) continue;
            uint32_t v = src->px[y * src->stride + x];
            if (p->transparent && (v & 0xFFFFFF) == (p->bg & 0xFFFFFF)) continue;
            dst->px[ty * dst->stride + tx] = v;
        }
    }
}
static void text_box(paint_t *p, int *w, int *h) {
    int f = fonts[p->font], lh = font_height(f), mw = 0, n = 1;
    for (const char *s = p->text, *e; ; s = e + 1) {
        e = strchr(s, '\n'); int len = e ? (int)(e - s) : (int)strlen(s);
        mw = MAX(mw, text_width_n(f, s, len));
        if (!e) break; n++;
    }
    *w = MAX(mw, 8) + 2; *h = n * lh + 2;
}
static void draw_typed(paint_t *p, canvas_t *c, bool caret) {
    int f = fonts[p->font], lh = font_height(f), bw, bh;
    text_box(p, &bw, &bh);
    if (!p->transparent) fill_rect(c, p->tx, p->ty, bw, bh, p->bg);
    int y = p->ty + 1, lastx = p->tx + 1;
    for (const char *s = p->text, *e; ; s = e + 1) {
        e = strchr(s, '\n'); int len = e ? (int)(e - s) : (int)strlen(s);
        draw_text_n(c, f, p->tx + 1, y, s, len, p->fg);
        lastx = p->tx + 1 + text_width_n(f, s, len);
        if (!e) break; y += lh;
    }
    if (caret) { vline(c, lastx, y, lh, p->fg); dotted_rect(c, p->tx - 1, p->ty - 1, bw + 2, bh + 2, HEX(0x808080)); }
}
static void text_commit(paint_t *p) {
    if (!p->texting) return;
    p->texting = false;
    if (!p->tlen) return;
    push_undo(p);
    draw_typed(p, p->img, false);
}
static void sel_commit(paint_t *p) {
    if (p->fl) { stamp_canvas(p, p->img, p->fl, p->sr.x, p->sr.y); canvas_free(p->fl); p->fl = NULL; }
    p->sel = false; p->seldrag = 0;
}
static void commit_all(paint_t *p) { text_commit(p); sel_commit(p); }
/* the selection's pixels come off the picture (which keeps colour 2 there) to be moved */
static void sel_lift(paint_t *p) {
    if (p->fl || !p->sel) return;
    push_undo(p);
    p->fl = crop(p->img, p->sr);
    if (p->fl) fill_rect(p->img, p->sr.x, p->sr.y, p->sr.w, p->sr.h, p->bg);
}

/* ---------------- shapes ---------------- */
static void stamp(paint_t *p, int x, int y, uint32_t col) {
    int s = p->tool == T_BRUSH ? brushes[p->size] : p->tool == T_ERASER ? erasers[p->size] : 1;
    if (s <= 1) put_pixel(p->img, x, y, col);
    else if (p->tool == T_ERASER) fill_rect(p->img, x - s / 2, y - s / 2, s, s, col);
    else fill_circle(p->img, x, y, s / 2, col);
}
static void seg(paint_t *p, int x0, int y0, int x1, int y1, uint32_t col) {
    int n = MAX(ABSDIFF(x0, x1), ABSDIFF(y0, y1)); if (!n) n = 1;
    for (int i = 0; i <= n; i++) stamp(p, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, col);
}
static void spray(paint_t *p, int cx, int cy, uint32_t col) {
    int r = sprays[p->size];
    for (int i = 0; i < r * 2; i++) {
        int dx = (int)(rnd(p) % (2 * r + 1)) - r, dy = (int)(rnd(p) % (2 * r + 1)) - r;
        if (dx * dx + dy * dy <= r * r) put_pixel(p->img, cx + dx, cy + dy, col);
    }
}
/* the span of row dy2 (in half pixels from the centre) of an ellipse a by b pixels across */
static int half_span(int a, int b, int dy2) {
    if (b <= 0 || dy2 * dy2 > b * b) return -1;
    return (int)(a * isqrt64((int64_t)b * b - (int64_t)dy2 * dy2) / b);
}
static void ellipse(canvas_t *c, int x0, int y0, int x1, int y1, int t, int mode, uint32_t oc, uint32_t fc) {
    int cx2 = x0 + x1, cy2 = y0 + y1, a = x1 - x0, b = y1 - y0;
    for (int y = y0; y <= y1; y++) {
        int dy2 = 2 * y - cy2;
        int o = half_span(a, b, dy2); if (o < 0) continue;
        int xl = (cx2 - o) / 2, xr = (cx2 + o + 1) / 2;
        int in = a > 2 * t && b > 2 * t ? half_span(a - 2 * t, b - 2 * t, dy2) : -1;
        if (mode == 2) { hline(c, xl, y, xr - xl + 1, fc); continue; }
        if (in < 0) { hline(c, xl, y, xr - xl + 1, oc); continue; }
        int il = (cx2 - in) / 2, ir = (cx2 + in + 1) / 2;
        hline(c, xl, y, il - xl, oc); hline(c, ir + 1, y, xr - ir, oc);
        if (mode == 1 && ir >= il) hline(c, il, y, ir - il + 1, fc);
    }
}
static void shape(paint_t *p, int x0, int y0, int x1, int y1, bool right) {
    uint32_t fg = right ? p->bg : p->fg, bg = right ? p->fg : p->bg;
    bool shift = keyboard_mods() & MOD_SHIFT;
    if (p->tool == T_LINE) {
        if (shift) {      /* along 45-degree steps */
            int dx = x1 - x0, dy = y1 - y0, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            if (ax > 2 * ay) y1 = y0; else if (ay > 2 * ax) x1 = x0;
            else { int m = MAX(ax, ay); x1 = x0 + (dx < 0 ? -m : m); y1 = y0 + (dy < 0 ? -m : m); }
        }
        draw_thick_line(p->img, x0, y0, x1, y1, widths[p->size], fg);
        return;
    }
    if (shift) {          /* squares and circles */
        int m = MAX(ABSDIFF(x0, x1), ABSDIFF(y0, y1));
        x1 = x0 + (x1 < x0 ? -m : m); y1 = y0 + (y1 < y0 ? -m : m);
    }
    int x = MIN(x0, x1), y = MIN(y0, y1), w = ABSDIFF(x0, x1) + 1, h = ABSDIFF(y0, y1) + 1, t = widths[p->size];
    uint32_t oc = fg, fc = p->fillmode == 2 ? fg : bg;
    if (p->tool == T_ELLIPSE) { ellipse(p->img, x, y, x + w - 1, y + h - 1, t, p->fillmode, oc, fc); return; }
    if (p->tool == T_RECT) {
        if (p->fillmode) fill_rect(p->img, x, y, w, h, fc);
        if (p->fillmode != 2) for (int k = 0; k < t && 2 * k < w && 2 * k < h; k++) draw_rect(p->img, x + k, y + k, w - 2 * k, h - 2 * k, oc);
        return;
    }
    int r = MIN(16, MIN(w, h) / 4);
    if (p->fillmode == 2) { fill_round_rect(p->img, x, y, w, h, r, fc); return; }
    if (p->fillmode == 1) fill_round_rect(p->img, x, y, w, h, r, fc);
    for (int k = 0; k < t && 2 * k < w && 2 * k < h; k++) draw_round_rect(p->img, x + k, y + k, w - 2 * k, h - 2 * k, MAX(0, r - k), oc);
}

/* ---------------- whole-picture (or selection) operations ---------------- */
enum { OP_FLIPH, OP_FLIPV, OP_ROT_R, OP_ROT_L, OP_ROT_180, OP_INVERT, OP_GRAY };
static canvas_t *apply_op(canvas_t *s, int op) {
    bool turn = op == OP_ROT_R || op == OP_ROT_L;
    canvas_t *c = canvas_new(turn ? s->h : s->w, turn ? s->w : s->h);
    if (!c) return NULL;
    for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
        uint32_t v = s->px[y * s->stride + x];
        int tx = x, ty = y;
        switch (op) {
        case OP_FLIPH: tx = s->w - 1 - x; break;
        case OP_FLIPV: ty = s->h - 1 - y; break;
        case OP_ROT_R: tx = s->h - 1 - y; ty = x; break;
        case OP_ROT_L: tx = y; ty = s->w - 1 - x; break;
        case OP_ROT_180: tx = s->w - 1 - x; ty = s->h - 1 - y; break;
        case OP_INVERT: v = (v ^ 0xFFFFFF) | 0xFF000000u; break;
        case OP_GRAY: { uint32_t l = (C_R(v) * 77 + C_G(v) * 150 + C_B(v) * 29) >> 8; v = 0xFF000000u | l << 16 | l << 8 | l; break; }
        }
        c->px[ty * c->stride + tx] = v;
    }
    return c;
}
static void do_op(window_t *w, int op) {
    paint_t *p = w->data;
    text_commit(p);
    if (p->sel) {                          /* only the selection: lift it and change the lifted pixels */
        sel_lift(p);
        canvas_t *n = p->fl ? apply_op(p->fl, op) : NULL;
        if (n) { canvas_free(p->fl); p->fl = n; p->sr.w = n->w; p->sr.h = n->h; }
    } else {
        canvas_t *n = apply_op(p->img, op);
        if (n) { push_undo(p); set_image(p, n, NULL); }
    }
    w->dirty = true;
}
static canvas_t *resized(canvas_t *s, int w, int h, uint32_t bg, bool stretch) {
    canvas_t *c = canvas_new(w, h);
    if (!c) return NULL;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t v;
        if (stretch) v = s->px[(int)((int64_t)y * s->h / h) * s->stride + (int)((int64_t)x * s->w / w)];
        else v = x < s->w && y < s->h ? s->px[y * s->stride + x] : bg;
        c->px[y * c->stride + x] = v;
    }
    return c;
}
static void canvas_size(window_t *w, int nw, int nh, bool stretch) {
    paint_t *p = w->data;
    if (nw < 1 || nh < 1 || nw > 8192 || nh > 8192) { msgbox("Paint", "The width and height must be between 1 and 8192 pixels.", MBI_WARNING, MBX_OK, NULL, NULL); return; }
    commit_all(p);
    canvas_t *n = resized(p->img, nw, nh, p->bg, stretch);
    if (!n) { msgbox("Paint", "There is not enough memory for a picture this size.", MBI_ERROR, MBX_OK, NULL, NULL); return; }
    push_undo(p); set_image(p, n, NULL);
    w->dirty = true;
}

/* ---------------- layout ---------------- */
static rect_t paint_area(window_t *w) { return R(TOOL_W + 4, 4, w->surf->w - TOOL_W - 8, w->surf->h - PAL_H - 8); }
/* the part that shows the picture, and whether it needs scroll bars */
static rect_t view_rect(window_t *w, bool *hs, bool *vs) {
    paint_t *p = w->data; rect_t a = paint_area(w);
    int vw = a.w - 4, vh = a.h - 4, iw = p->img->w * p->zoom, ih = p->img->h * p->zoom;
    bool h = iw > vw, v = ih > vh;
    if (h && !v) v = ih > vh - SB;
    if (v && !h) h = iw > vw - SB;
    if (hs) *hs = h;
    if (vs) *vs = v;
    return R(a.x + 2, a.y + 2, vw - (v ? SB : 0), vh - (h ? SB : 0));
}
static void clamp_scroll(window_t *w) {
    paint_t *p = w->data; rect_t v = view_rect(w, NULL, NULL);
    p->scx = CLAMP(p->scx, 0, MAX(0, p->img->w - v.w / p->zoom));
    p->scy = CLAMP(p->scy, 0, MAX(0, p->img->h - v.h / p->zoom));
}
static void set_zoom(window_t *w, int z, int ax, int ay) {      /* keep picture point ax, ay where it is */
    paint_t *p = w->data;
    z = CLAMP(z, 1, 8);
    if (z == p->zoom) return;
    rect_t v = view_rect(w, NULL, NULL);
    if (ax < 0) { ax = p->scx + v.w / p->zoom / 2; ay = p->scy + v.h / p->zoom / 2; }
    int ox = (ax - p->scx) * p->zoom, oy = (ay - p->scy) * p->zoom;   /* on the screen */
    p->zoom = z;
    p->scx = ax - ox / z; p->scy = ay - oy / z;
    clamp_scroll(w);
    w->dirty = true;
}
static rect_t handle_rect(window_t *w) {
    paint_t *p = w->data; rect_t v = view_rect(w, NULL, NULL);
    return R(v.x + (p->img->w - p->scx) * p->zoom, v.y + (p->img->h - p->scy) * p->zoom, 6, 6);
}
static rect_t hbar_rect(window_t *w) { rect_t v = view_rect(w, NULL, NULL); return R(v.x, v.y + v.h, v.w, SB); }

/* ---------------- painting ---------------- */
static void tool_glyph(canvas_t *c, int t, int x, int y) {
    uint32_t k = T.text;
    switch (t) {
    case T_SELECT: dotted_rect(c, x + 2, y + 3, 12, 10, k); break;
    case T_MAGNIFY: draw_circle(c, x + 6, y + 6, 4, k); draw_thick_line(c, x + 9, y + 9, x + 14, y + 14, 2, HEX(0x8D5A2B)); break;
    case T_PENCIL: draw_thick_line(c, x + 3, y + 14, x + 13, y + 4, 3, HEX(0xE0A020)); draw_line(c, x + 2, y + 15, x + 4, y + 13, k); break;
    case T_BRUSH: draw_thick_line(c, x + 4, y + 12, x + 13, y + 3, 2, HEX(0x8D5A2B)); fill_circle(c, x + 4, y + 13, 3, HEX(0x2060E0)); break;
    case T_AIRBRUSH:
        fill_rect(c, x + 8, y + 7, 6, 8, HEX(0x707070)); fill_rect(c, x + 10, y + 4, 2, 3, k);
        for (int i = 0; i < 7; i++) put_pixel(c, x + 1 + (i * 5) % 6, y + 2 + (i * 3) % 7, HEX(0x2060E0));
        break;
    case T_ERASER: fill_rect(c, x + 3, y + 5, 11, 7, HEX(0xFFC0E0)); draw_rect(c, x + 3, y + 5, 11, 7, k); break;
    case T_FILL: fill_rect(c, x + 3, y + 6, 9, 8, HEX(0x2080FF)); draw_rect(c, x + 3, y + 6, 9, 8, k); draw_line(c, x + 12, y + 7, x + 15, y + 12, HEX(0x2080FF)); break;
    case T_PICK: draw_thick_line(c, x + 3, y + 13, x + 11, y + 5, 2, HEX(0x606060)); fill_circle(c, x + 12, y + 4, 3, k); break;
    case T_TEXT: draw_text(c, FONT_UI_BOLD, x + 4, y + 1, "A", k); break;
    case T_LINE: draw_thick_line(c, x + 2, y + 14, x + 14, y + 2, 2, k); break;
    case T_RECT: draw_rect(c, x + 2, y + 4, 13, 10, k); break;
    case T_RRECT: draw_round_rect(c, x + 2, y + 4, 13, 10, 3, k); break;
    case T_ELLIPSE: ellipse(c, x + 1, y + 3, x + 15, y + 13, 1, 0, k, k); break;
    }
}
static void show_scaled(canvas_t *c, rect_t v, canvas_t *s, int scx, int scy, int z, bool grid) {
    int w = MIN(v.w, (s->w - scx) * z), h = MIN(v.h, (s->h - scy) * z);
    if (z == 1) { blit(c, v.x, v.y, s, scx, scy, w, h); return; }
    for (int y = 0; y < h; y++) {
        int dy = v.y + y; if (dy < 0 || dy >= c->h) continue;
        const uint32_t *row = s->px + (scy + y / z) * s->stride + scx;
        uint32_t *out = c->px + dy * c->stride + v.x;
        bool gl = grid && y % z == z - 1;
        for (int x = 0; x < w; x++) out[x] = gl || (grid && x % z == z - 1) ? mix(row[x / z], HEX(0x808080), 140) : row[x / z];
    }
}
/* one of the choices in the options box under the tools */
static bool opt(window_t *w, rect_t r, bool on) {
    canvas_t *c = w->surf;
    bool k = ui_click_area(w, r);
    if (on) fill_rect(c, r.x, r.y, r.w, r.h, T.select);
    return k;
}
static void paint_options(window_t *w, paint_t *p, rect_t box) {
    canvas_t *c = w->surf;
    draw_sunken_box(c, box, T.window_bg);
    int x = box.x + 2, y = box.y + 2, bw = box.w - 4;
    uint32_t sel = T.select_text, txt = T.text;
    switch (p->tool) {
    case T_BRUSH: case T_ERASER: case T_LINE: case T_AIRBRUSH:
        for (int i = 0; i < 4; i++) {
            rect_t r = R(x, y + i * 16, bw, 16);
            if (opt(w, r, p->size == i)) p->size = i;
            uint32_t col = p->size == i ? sel : txt;
            if (p->tool == T_AIRBRUSH) { int rr = sprays[i] / 2 + 1; for (int k = 0; k < 10; k++) put_pixel(c, r.x + bw / 2 - rr + (k * 7) % (2 * rr + 1), r.y + 8 - rr / 2 + (k * 3) % (rr + 1), col); }
            else if (p->tool == T_ERASER) { int s = erasers[i]; fill_rect(c, r.x + bw / 2 - s / 2, r.y + 8 - s / 2, s, s, col); }
            else { int s = p->tool == T_BRUSH ? brushes[i] : widths[i]; fill_rect(c, r.x + 6, r.y + 8 - s / 2, bw - 12, MAX(1, s), col); }
        }
        break;
    case T_RECT: case T_RRECT: case T_ELLIPSE:
        for (int i = 0; i < 3; i++) {
            rect_t r = R(x, y + i * 18, bw, 18);
            if (opt(w, r, p->fillmode == i)) p->fillmode = i;
            rect_t s = R(r.x + 8, r.y + 3, bw - 16, 12);
            if (i) fill_rect(c, s.x, s.y, s.w, s.h, i == 2 ? HEX(0x808080) : HEX(0xC0C0C0));
            if (i != 2) draw_rect(c, s.x, s.y, s.w, s.h, p->fillmode == i ? sel : txt);
        }
        hline(c, x + 2, y + 57, bw - 4, T.face_dark);
        for (int i = 0; i < 4; i++) {
            rect_t r = R(x, y + 60 + i * 12, bw, 12);
            if (opt(w, r, p->size == i)) p->size = i;
            fill_rect(c, r.x + 6, r.y + 6 - widths[i] / 2, bw - 12, widths[i], p->size == i ? sel : txt);
        }
        break;
    case T_SELECT: case T_TEXT:
        for (int i = 0; i < 2; i++) {
            rect_t r = R(x, y + i * 20, bw, 20);
            if (opt(w, r, p->transparent == (i == 1))) { p->transparent = i == 1; w->dirty = true; }
            /* opaque: the shapes on white; see-through: on a checkerboard */
            rect_t s = R(r.x + bw / 2 - 12, r.y + 2, 24, 16);
            for (int yy = 0; yy < s.h; yy += 4) for (int xx = 0; xx < s.w; xx += 4)
                fill_rect(c, s.x + xx, s.y + yy, 4, 4, !i || ((xx + yy) / 4) % 2 ? WHITE : HEX(0xC0C0C0));
            draw_rect(c, s.x, s.y, s.w, s.h, txt);
            fill_circle(c, s.x + 8, s.y + 9, 5, HEX(0xD03030)); fill_rect(c, s.x + 12, s.y + 3, 9, 9, HEX(0x2060E0));
        }
        if (p->tool == T_TEXT) for (int i = 0; i < NFONTS; i++) {
            rect_t r = R(x, y + 44 + i * 14, bw, 14);
            if (opt(w, r, p->font == i)) { p->font = i; w->dirty = true; }
            draw_text(c, FONT_SMALL, r.x + 4, r.y + 1, font_names[i], p->font == i ? sel : txt);
        }
        break;
    case T_MAGNIFY:
        for (int i = 0; i < 4; i++) {
            rect_t r = R(x, y + i * 16, bw, 16);
            if (opt(w, r, p->zoom == zooms[i])) set_zoom(w, zooms[i], -1, -1);
            char b[8]; snprintf(b, sizeof b, "%dx", zooms[i]);
            draw_text(c, FONT_SMALL, r.x + bw / 2 - 6, r.y + 2, b, p->zoom == zooms[i] ? sel : txt);
        }
        break;
    }
}
static void menu_state(window_t *w);
static void paint_paint(window_t *w) {
    paint_t *p = w->data; canvas_t *c = w->surf;
    menu_state(w);
    gfx_clear(c, T.face);
    clamp_scroll(w);
    rect_t a = paint_area(w);
    bool hs, vs; rect_t v = view_rect(w, &hs, &vs);
    fill_rect(c, a.x, a.y, a.w, a.h, HEX(0x808080));
    bevel(c, a.x, a.y, a.w, a.h, false);
    /* the picture, with the lifted selection and the text on top */
    canvas_t *src = p->img;
    if (p->fl || p->texting) {
        for (int y = 0; y < p->img->h; y++) memcpy(p->view->px + y * p->view->stride, p->img->px + y * p->img->stride, p->img->w * 4);
        if (p->fl) stamp_canvas(p, p->view, p->fl, p->sr.x, p->sr.y);
        if (p->texting) draw_typed(p, p->view, true);
        src = p->view;
    }
    rect_t old = canvas_push_clip(c, v);
    show_scaled(c, v, src, p->scx, p->scy, p->zoom, p->grid && p->zoom >= 4);
    int z = p->zoom;
    if (p->sel) dotted_rect(c, v.x + (p->sr.x - p->scx) * z - 1, v.y + (p->sr.y - p->scy) * z - 1, p->sr.w * z + 2, p->sr.h * z + 2, HEX(0x000080));
    if (p->resizing) dotted_rect(c, v.x - p->scx * z, v.y - p->scy * z, p->rw * z, p->rh * z, BLACK);
    rect_t hr = handle_rect(w);
    fill_rect(c, hr.x, hr.y, hr.w, hr.h, HEX(0x000080));
    canvas_set_clip(c, old);
    /* scroll bars */
    if (vs) { int pos = p->scy; if (ui_vscroll(w, R(v.x + v.w, v.y, SB, v.h), &pos, p->img->h, v.h / z)) { p->scy = pos; w->dirty = true; } }
    if (hs) {
        rect_t hb = hbar_rect(w);
        int vis = v.w / z, total = p->img->w, maxp = MAX(1, total - vis);
        fill_rect(c, hb.x, hb.y, hb.w, hb.h, mix(T.face, WHITE, 120));
        int tl = MAX(14, hb.w * vis / MAX(1, total)), tp = (hb.w - tl) * p->scx / maxp;
        fill_rect(c, hb.x + tp, hb.y, tl, hb.h, T.face); bevel(c, hb.x + tp, hb.y, tl, hb.h, true);
        if (vs) fill_rect(c, hb.x + hb.w, hb.y, SB, SB, T.face);
    }
    /* tools */
    for (int i = 0; i < T_COUNT; i++) {
        rect_t r = R(7 + (i % 2) * 25, 6 + (i / 2) * 25, 23, 23);
        bool sel = p->tool == i;
        bool clicked = ui_click_area(w, r);
        fill_rect(c, r.x, r.y, r.w, r.h, sel ? T.window_bg : T.face);
        bevel_deep(c, r.x, r.y, r.w, r.h, !sel);
        tool_glyph(c, i, r.x + 3, r.y + 3);
        if (clicked && p->tool != i) {
            commit_all(p);
            p->tool = i;
            if (i == T_LINE || i == T_BRUSH || i == T_ERASER || i == T_AIRBRUSH || i >= T_RECT) p->size = MIN(p->size, 3);
            w->dirty = true;
        }
    }
    int oy = 6 + ((T_COUNT + 1) / 2) * 25 + 4;
    paint_options(w, p, R(5, oy, TOOL_W - 8, MIN(150, c->h - PAL_H - oy - 6)));
    /* colours */
    int py = c->h - PAL_H;
    fill_rect(c, 6, py + 4, 34, 34, WHITE); bevel_deep(c, 6, py + 4, 34, 34, false);
    fill_rect(c, 20, py + 18, 14, 14, p->bg); draw_rect(c, 20, py + 18, 14, 14, BLACK);
    fill_rect(c, 11, py + 9, 14, 14, p->fg); draw_rect(c, 11, py + 9, 14, 14, BLACK);
    for (int i = 0; i < 28; i++) {
        rect_t r = R(46 + (i % 14) * 18, py + 4 + (i / 14) * 18, 17, 17);
        fill_rect(c, r.x, r.y, r.w, r.h, HEX(p->pal[i])); bevel_deep(c, r.x, r.y, r.w, r.h, false);
    }
    /* the status line: the pointer, the selection or shape size, the zoom */
    char b[120], pos[32] = "", sz[40] = "";
    if (p->mx >= 0) snprintf(pos, sizeof pos, "%d, %dpx", p->mx, p->my);
    if (p->sel) snprintf(sz, sizeof sz, "Selection %d x %d", p->sr.w, p->sr.h);
    else if (p->drawing && p->tool >= T_LINE) snprintf(sz, sizeof sz, "%d x %d", ABSDIFF(p->lx, p->sx) + 1, ABSDIFF(p->ly, p->sy) + 1);
    else if (p->resizing) snprintf(sz, sizeof sz, "New size %d x %d", p->rw, p->rh);
    else snprintf(sz, sizeof sz, "%d x %d", p->img->w, p->img->h);
    int sx = 46 + 14 * 18 + 10;
    snprintf(b, sizeof b, "%s", tool_names[p->tool]);
    draw_text(c, FONT_SMALL, sx, py + 6, b, T.text);
    snprintf(b, sizeof b, "%s   %s   %d%%", pos, sz, p->zoom * 100);
    draw_text(c, FONT_SMALL, sx, py + 22, b, T.gray_text);
}

/* ---------------- the mouse ---------------- */
static void edit_colour_cb(void *ctx, const char *s);
static void airbrush_tick(window_t *w) {
    paint_t *p = w->data;
    if (!p->drawing || p->tool != T_AIRBRUSH) return;
    spray(p, p->lx, p->ly, p->right ? p->bg : p->fg);
    w->dirty = true;
}
static void palette_mouse(window_t *w, int type, int x, int y, int btn) {
    paint_t *p = w->data; int py = w->surf->h - PAL_H;
    for (int i = 0; i < 28; i++) {
        rect_t r = R(46 + (i % 14) * 18, py + 4 + (i / 14) * 18, 17, 17);
        if (!pt_in(r, x, y)) continue;
        if (type == ME_DBLCLICK) {
            p->pal_edit = i;
            char cur[16]; snprintf(cur, sizeof cur, "#%06X", p->pal[i] & 0xFFFFFF);
            input_box("Edit Colors", "The colour for this box, as #RRGGBB or R,G,B:", cur, edit_colour_cb, w);
        } else if (type == ME_DOWN) {
            if (btn & MB_RIGHT) p->bg = HEX(p->pal[i]); else p->fg = HEX(p->pal[i]);
        }
        w->dirty = true;
        return;
    }
    if (type == ME_DOWN && pt_in(R(6, py + 4, 34, 34), x, y)) { uint32_t t = p->fg; p->fg = p->bg; p->bg = t; w->dirty = true; }   /* swap */
}
static void paint_mouse(window_t *w, int type, int x, int y, int btn) {
    paint_t *p = w->data;
    bool hs, vs; rect_t v = view_rect(w, &hs, &vs);
    int z = p->zoom;
    int ix = fdiv(x - v.x, z) + p->scx, iy = fdiv(y - v.y, z) + p->scy;
    bool over = pt_in(v, x, y) && ix >= 0 && iy >= 0 && ix < p->img->w && iy < p->img->h;
    int pmx = p->mx, pmy = p->my;
    p->mx = over ? ix : -1; p->my = over ? iy : -1;
    if (pmx != p->mx || pmy != p->my) w->dirty = true;
    if (y >= w->surf->h - PAL_H) { palette_mouse(w, type, x, y, btn); if (type != ME_UP) return; }
    if (type == ME_WHEEL) {
        if (keyboard_mods() & MOD_CTRL) set_zoom(w, btn < 0 ? p->zoom * 2 : p->zoom / 2, over ? ix : -1, over ? iy : -1);
        else if (keyboard_mods() & MOD_SHIFT) p->scx += btn * 24 / z;
        else p->scy += btn * 24 / z;
        clamp_scroll(w); w->dirty = true;
        return;
    }
    /* the horizontal scroll bar */
    rect_t hb = hbar_rect(w);
    if (hs && (p->hdrag || (type == ME_DOWN && pt_in(hb, x, y)))) {
        int vis = v.w / z, total = p->img->w, maxp = MAX(1, total - vis);
        int tl = MAX(14, hb.w * vis / MAX(1, total)), tp = (hb.w - tl) * p->scx / maxp;
        if (type == ME_DOWN) {
            if (x >= hb.x + tp && x < hb.x + tp + tl) { p->hdrag = 1; p->hdrag_off = x - hb.x - tp; }
            else p->scx += x < hb.x + tp ? -vis : vis;
        } else if (type == ME_MOVE && hb.w > tl) p->scx = (x - hb.x - p->hdrag_off) * maxp / (hb.w - tl);
        else if (type == ME_UP) p->hdrag = 0;
        clamp_scroll(w); w->dirty = true;
        return;
    }
    /* the canvas size handle */
    rect_t hr = handle_rect(w);
    bool on_handle = pt_in(R(hr.x - 2, hr.y - 2, hr.w + 4, hr.h + 4), x, y) && pt_in(v, MIN(x, v.x + v.w - 1), MIN(y, v.y + v.h - 1));
    if (p->resizing) {
        p->rw = MAX(1, fdiv(x - v.x, z) + p->scx); p->rh = MAX(1, fdiv(y - v.y, z) + p->scy);
        w->cursor = CURSOR_SIZE_NWSE;
        if (type == ME_UP) { p->resizing = false; if (p->rw != p->img->w || p->rh != p->img->h) canvas_size(w, p->rw, p->rh, false); }
        w->dirty = true;
        return;
    }
    if (type == ME_DOWN && on_handle && !p->drawing) { commit_all(p); p->resizing = true; p->rw = p->img->w; p->rh = p->img->h; w->dirty = true; return; }

    bool in_sel = p->sel && ix >= p->sr.x && iy >= p->sr.y && ix < p->sr.x + p->sr.w && iy < p->sr.y + p->sr.h;
    w->cursor = on_handle ? CURSOR_SIZE_NWSE : !over && !p->drawing ? CURSOR_ARROW
              : p->tool == T_SELECT && (in_sel || p->seldrag == 2) ? CURSOR_MOVE : p->tool == T_TEXT ? CURSOR_IBEAM : CURSOR_CROSS;
    bool right = btn & MB_RIGHT;
    if (type == ME_DOWN && over) {
        p->drawing = true; p->right = right; p->sx = p->lx = ix; p->sy = p->ly = iy;
        uint32_t col = right ? p->bg : p->fg;
        switch (p->tool) {
        case T_SELECT:
            text_commit(p);
            if (in_sel) { sel_lift(p); p->seldrag = 2; p->offx = ix - p->sr.x; p->offy = iy - p->sr.y; }
            else { sel_commit(p); p->sel = true; p->seldrag = 1; p->sr = R(ix, iy, 1, 1); }
            break;
        case T_MAGNIFY: set_zoom(w, right ? p->zoom / 2 : p->zoom * 2, ix, iy); p->drawing = false; break;
        case T_TEXT:
            if (p->texting) { text_commit(p); p->drawing = false; break; }
            p->texting = true; p->tx = ix; p->ty = iy; p->tlen = 0; p->text[0] = 0; p->drawing = false;
            break;
        case T_FILL: push_undo(p); flood_fill(p->img, ix, iy, col); p->drawing = false; break;
        case T_PICK: { uint32_t pc = get_pixel(p->img, ix, iy); if (right) p->bg = pc; else p->fg = pc; p->drawing = false; break; }
        case T_LINE: case T_RECT: case T_RRECT: case T_ELLIPSE:
            push_undo(p);
            for (int yy = 0; yy < p->img->h; yy++) memcpy(p->snap->px + yy * p->snap->stride, p->img->px + yy * p->img->stride, p->img->w * 4);
            break;
        case T_AIRBRUSH: push_undo(p); spray(p, ix, iy, col); wm_set_timer(w, 30, airbrush_tick); break;
        default: push_undo(p); stamp(p, ix, iy, p->tool == T_ERASER ? p->bg : col); break;
        }
        w->dirty = true;
    } else if (type == ME_MOVE && p->drawing) {
        ix = CLAMP(ix, -4096, 12288); iy = CLAMP(iy, -4096, 12288);
        uint32_t col = p->right ? p->bg : p->fg;
        switch (p->tool) {
        case T_SELECT:
            if (p->seldrag == 1) {
                int x0 = CLAMP(MIN(p->sx, ix), 0, p->img->w - 1), y0 = CLAMP(MIN(p->sy, iy), 0, p->img->h - 1);
                int x1 = CLAMP(MAX(p->sx, ix), 0, p->img->w - 1), y1 = CLAMP(MAX(p->sy, iy), 0, p->img->h - 1);
                p->sr = R(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
            } else if (p->seldrag == 2) { p->sr.x = ix - p->offx; p->sr.y = iy - p->offy; }
            break;
        case T_LINE: case T_RECT: case T_RRECT: case T_ELLIPSE:
            for (int yy = 0; yy < p->img->h; yy++) memcpy(p->img->px + yy * p->img->stride, p->snap->px + yy * p->snap->stride, p->img->w * 4);
            shape(p, p->sx, p->sy, ix, iy, p->right);
            break;
        case T_AIRBRUSH: spray(p, ix, iy, col); break;
        default: seg(p, p->lx, p->ly, ix, iy, p->tool == T_ERASER ? p->bg : col); break;
        }
        p->lx = ix; p->ly = iy; w->dirty = true;
    } else if (type == ME_UP && p->drawing) {
        p->drawing = false;
        if (p->tool == T_AIRBRUSH) wm_set_timer(w, 0, NULL);
        if (p->tool == T_SELECT && p->seldrag == 1 && p->sr.w <= 1 && p->sr.h <= 1) p->sel = false;   /* a click: no selection */
        p->seldrag = 0;
        w->dirty = true;
    }
}

/* ---------------- files ---------------- */
static void paint_title(window_t *w) {
    paint_t *p = w->data; char t[300];
    snprintf(t, sizeof t, "%s - Paint", p->path[0] ? path_basename(p->path) : "untitled");
    wm_set_title(w, t);
}
static void paint_save_cb(void *ctx, const char *path) {
    window_t *w = ctx; if (!wm_alive(w)) return; paint_t *p = w->data;
    commit_all(p);
    char pp[300]; strlcpy(pp, path, sizeof pp);
    bool png = str_ends_with_ci(pp, ".png") != 0;
    if (!png && !str_ends_with_ci(pp, ".bmp")) strlcat(pp, ".bmp", sizeof pp);
    uint32_t sz = 0; uint8_t *d = png ? image_encode(p->img, true, &sz) : bmp_save(p->img, &sz);
    if (d && vfs_write_file(pp, d, sz)) {
        strlcpy(p->path, pp, sizeof p->path); p->modified = false; paint_title(w);
        recent_add(pp);
        if (p->closing) { kfree(d); wm_destroy(w); return; }
    } else msgbox("Paint", "Could not save the picture.", MBI_ERROR, MBX_OK, NULL, NULL);
    p->closing = false;
    kfree(d);
}
static canvas_t *load_picture(const char *path) {
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir) return NULL;
    char err[96];
    canvas_t *c = image_decode(n->data, n->size, err, sizeof err);
    if (!c) { char m[200]; snprintf(m, sizeof m, "Paint cannot read this file: %s.", err[0] ? err : "not a picture"); msgbox("Paint", m, MBI_ERROR, MBX_OK, NULL, NULL); return NULL; }
    flatten_alpha(c);
    return c;
}
static bool paint_load(window_t *w, const char *path) {
    paint_t *p = w->data;
    canvas_t *c = load_picture(path);
    if (!c || !set_image(p, c, NULL)) return false;
    p->sel = false; canvas_free(p->fl); p->fl = NULL; p->texting = false;
    stack_clear(p->undo, &p->nundo); stack_clear(p->redo, &p->nredo);
    p->scx = p->scy = 0; p->modified = false;
    vnode_t *n = vfs_resolve(path, NULL);
    vfs_path(n, p->path, sizeof p->path);
    /* only BMP and PNG are written: other pictures are saved as a PNG next to them */
    if (!str_ends_with_ci(p->path, ".bmp") && !str_ends_with_ci(p->path, ".png")) {
        char *dot = strrchr(p->path, '.');
        if (dot && !strchr(dot, '\\')) *dot = 0;
        strlcat(p->path, ".png", sizeof p->path);
        p->modified = true;
    }
    paint_title(w);
    w->dirty = true; return true;
}
static void paint_open_cb(void *ctx, const char *path) { window_t *w = ctx; if (wm_alive(w)) paint_load(w, path); }
static void paste_canvas(window_t *w, canvas_t *c) {
    paint_t *p = w->data;
    commit_all(p);
    push_undo(p);
    if (c->w > p->img->w || c->h > p->img->h) {    /* make room for it */
        canvas_t *n = resized(p->img, MAX(c->w, p->img->w), MAX(c->h, p->img->h), p->bg, false);
        if (!n || !set_image(p, n, NULL)) { canvas_free(c); return; }
    }
    p->fl = c; p->sel = true; p->sr = R(p->scx, p->scy, c->w, c->h);
    if (p->sr.x + c->w > p->img->w) p->sr.x = p->img->w - c->w;
    if (p->sr.y + c->h > p->img->h) p->sr.y = p->img->h - c->h;
    p->tool = T_SELECT;
    w->dirty = true;
}
static void paste_from_cb(void *ctx, const char *path) {
    window_t *w = ctx; if (!wm_alive(w)) return;
    canvas_t *c = load_picture(path);
    if (c) paste_canvas(w, c);
}
static void wallpaper_cb(void *ctx, int r) {
    window_t *w = ctx; if (!wm_alive(w) || r != IDOK) return; paint_t *p = w->data;
    char err[96];
    if (wallpaper_set_custom(p->path, err, sizeof err)) { settings_save(); wm_need_redraw = true; }
    else { char m[200]; snprintf(m, sizeof m, "The picture cannot be used as the background: %s", err); msgbox("Paint", m, MBI_WARNING, MBX_OK, NULL, NULL); }
}

/* ---------------- dialogs ---------------- */
static bool parse_colour(const char *s, uint32_t *out) {
    while (*s == ' ') s++;
    if (*s == '#') s++;
    int r, g, b;
    if (strchr(s, ',')) {
        r = atoi(s); s = strchr(s, ',') + 1; g = atoi(s);
        s = strchr(s, ','); if (!s) return false; b = atoi(s + 1);
        if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255) return false;
        *out = HEX((uint32_t)r << 16 | g << 8 | b); return true;
    }
    uint32_t v = 0; int n = 0;
    for (; *s && n < 6; s++, n++) {
        char ch = (char)tolower(*s);
        if (ch >= '0' && ch <= '9') v = v << 4 | (ch - '0'); else if (ch >= 'a' && ch <= 'f') v = v << 4 | (ch - 'a' + 10); else return false;
    }
    if (n != 6) return false;
    *out = HEX(v); return true;
}
static void edit_colour_cb(void *ctx, const char *s) {
    window_t *w = ctx; if (!wm_alive(w) || !s) return; paint_t *p = w->data;
    uint32_t c;
    if (!parse_colour(s, &c)) { msgbox("Paint", "Type a colour as #RRGGBB (for example #FF8000) or as R,G,B (for example 255,128,0).", MBI_WARNING, MBX_OK, NULL, NULL); return; }
    if (p->pal_edit >= 0) p->pal[p->pal_edit] = c & 0xFFFFFF;
    p->fg = c; p->pal_edit = -1;
    w->dirty = true;
}
static bool parse_size(const char *s, int *a, int *b) {
    while (*s == ' ') s++;
    *a = atoi(s);
    while (*s && (isdigit((unsigned char)*s) || *s == ' ')) s++;
    if (*s != 'x' && *s != 'X' && *s != ',' && *s != '*') return false;
    *b = atoi(s + 1);
    return *a > 0 && *b > 0;
}
static void size_cb(void *ctx, const char *s) {
    window_t *w = ctx; if (!wm_alive(w) || !s) return;
    int a, b;
    if (!parse_size(s, &a, &b)) { msgbox("Paint", "Type the size as width x height, for example 800 x 600.", MBI_WARNING, MBX_OK, NULL, NULL); return; }
    canvas_size(w, a, b, false);
}
static void stretch_cb(void *ctx, const char *s) {
    window_t *w = ctx; if (!wm_alive(w) || !s) return; paint_t *p = w->data;
    int a, b;
    if (!parse_size(s, &a, &b)) {
        a = b = atoi(s);
        if (a <= 0) { msgbox("Paint", "Type a percentage (for example 50) or both percentages (for example 200 x 100).", MBI_WARNING, MBX_OK, NULL, NULL); return; }
    }
    if (p->sel) {    /* stretch the selection */
        sel_lift(p);
        int nw = MAX(1, p->sr.w * a / 100), nh = MAX(1, p->sr.h * b / 100);
        canvas_t *n = p->fl && nw <= 8192 && nh <= 8192 ? resized(p->fl, nw, nh, p->bg, true) : NULL;
        if (n) { canvas_free(p->fl); p->fl = n; p->sr.w = nw; p->sr.h = nh; }
        w->dirty = true;
        return;
    }
    canvas_size(w, MAX(1, p->img->w * a / 100), MAX(1, p->img->h * b / 100), true);
}

/* ---------------- menus and keys ---------------- */
enum { PM_NEW = 1, PM_OPEN, PM_SAVE, PM_SAVEAS, PM_WALL, PM_EXIT, PM_UNDO, PM_REDO, PM_CUT, PM_COPY, PM_PASTE, PM_DEL, PM_ALL, PM_PASTEFROM,
       PM_ZIN, PM_ZOUT, PM_Z1, PM_GRID, PM_FLIPH, PM_FLIPV, PM_ROTR, PM_ROTL, PM_ROT180, PM_SIZE, PM_STRETCH, PM_CROP, PM_INVERT, PM_GRAY, PM_CLEAR,
       PM_EDITCOL, PM_SWAP, PM_ABOUT };
static void new_picture_cb(void *ctx, int r);
static void paint_cmd(window_t *w, int id) {
    paint_t *p = w->data;
    switch (id) {
    case PM_NEW:
        if (p->modified) { msgbox("Paint", "Save the changes to this picture first?", MBI_WARNING, MBX_YESNOCANCEL, new_picture_cb, w); break; }
        new_picture_cb(w, IDNO);
        break;
    case PM_OPEN: file_dialog("Open", p->path[0] ? p->path : USER_DOCS, false, NULL, paint_open_cb, w); break;
    case PM_SAVE: if (p->path[0]) { paint_save_cb(w, p->path); break; } /* fall through */
    case PM_SAVEAS: file_dialog("Save As (.bmp or .png)", p->path[0] ? p->path : USER_DOCS "\\untitled.bmp", true, NULL, paint_save_cb, w); break;
    case PM_WALL:
        if (!p->path[0] || p->modified) { msgbox("Paint", "Save the picture first, then set it as the desktop background.", MBI_INFO, MBX_OK, NULL, NULL); break; }
        msgbox("Paint", "Use this picture as the desktop background?", MBI_QUESTION, MBX_OKCANCEL, wallpaper_cb, w);
        break;
    case PM_EXIT: wm_close(w); return;
    case PM_UNDO: case PM_REDO: {
        text_commit(p);
        canvas_t **from = id == PM_UNDO ? p->undo : p->redo, **to = id == PM_UNDO ? p->redo : p->undo;
        int *nf = id == PM_UNDO ? &p->nundo : &p->nredo, *nt = id == PM_UNDO ? &p->nredo : &p->nundo;
        if (!*nf) break;
        sel_commit(p);        /* a moved selection goes down first, so Redo brings the move back */
        canvas_t *c = from[--*nf], *old = NULL;
        if (set_image(p, c, &old)) stack_push(to, nt, old);
        p->modified = true;
        break;
    }
    case PM_CUT: case PM_COPY:
        if (!p->sel) break;
        canvas_free(paint_clip);
        paint_clip = p->fl ? clone(p->fl) : crop(p->img, p->sr);
        if (id == PM_COPY) break;
        /* fall through */
    case PM_DEL:
        if (!p->sel) break;
        if (p->fl) { canvas_free(p->fl); p->fl = NULL; }
        else { push_undo(p); fill_rect(p->img, p->sr.x, p->sr.y, p->sr.w, p->sr.h, p->bg); }
        p->sel = false;
        break;
    case PM_PASTE: { canvas_t *c = paint_clip ? clone(paint_clip) : NULL; if (c) paste_canvas(w, c); break; }
    case PM_ALL: commit_all(p); p->tool = T_SELECT; p->sel = true; p->sr = R(0, 0, p->img->w, p->img->h); break;
    case PM_PASTEFROM: file_dialog("Paste From", USER_DOCS, false, NULL, paste_from_cb, w); break;
    case PM_ZIN: set_zoom(w, p->zoom * 2, -1, -1); break;
    case PM_ZOUT: set_zoom(w, p->zoom / 2, -1, -1); break;
    case PM_Z1: set_zoom(w, 1, -1, -1); break;
    case PM_GRID: p->grid = !p->grid; break;
    case PM_FLIPH: do_op(w, OP_FLIPH); break;
    case PM_FLIPV: do_op(w, OP_FLIPV); break;
    case PM_ROTR: do_op(w, OP_ROT_R); break;
    case PM_ROTL: do_op(w, OP_ROT_L); break;
    case PM_ROT180: do_op(w, OP_ROT_180); break;
    case PM_INVERT: do_op(w, OP_INVERT); break;
    case PM_GRAY: do_op(w, OP_GRAY); break;
    case PM_SIZE: { char b[32]; snprintf(b, sizeof b, "%d x %d", p->img->w, p->img->h); input_box("Canvas Size", "Width x height in pixels (the picture keeps its top-left corner):", b, size_cb, w); break; }
    case PM_STRETCH: input_box("Stretch", p->sel ? "Stretch the selection by this percentage (or width % x height %):" : "Stretch the picture by this percentage (or width % x height %):", "100", stretch_cb, w); break;
    case PM_CROP:
        if (!p->sel) { msgbox("Paint", "Select the part of the picture to keep first.", MBI_INFO, MBX_OK, NULL, NULL); break; }
        text_commit(p);
        { canvas_t *c = p->fl ? clone(p->fl) : crop(p->img, p->sr);
          if (c) { push_undo(p); canvas_free(p->fl); p->fl = NULL; p->sel = false; set_image(p, c, NULL); p->scx = p->scy = 0; } }
        break;
    case PM_CLEAR: commit_all(p); push_undo(p); gfx_clear(p->img, p->bg); break;
    case PM_EDITCOL: { p->pal_edit = -1; char cur[16]; snprintf(cur, sizeof cur, "#%06X", p->fg & 0xFFFFFF); input_box("Edit Colors", "Colour 1, as #RRGGBB or R,G,B:", cur, edit_colour_cb, w); break; }
    case PM_SWAP: { uint32_t t = p->fg; p->fg = p->bg; p->bg = t; break; }
    case PM_ABOUT: about_open(); break;
    }
    if (p->closing) return;
    paint_title(w);
    w->dirty = true;
}
/* the menus show what applies now */
static void menu_state(window_t *w) {
    paint_t *p = w->data;
    for (int m = 0; m < w->nmenus; m++) for (menu_item_t *it = w->menus[m].items; it->label; it++) {
        int f = it->flags & ~(MI_CHECKED | MI_DISABLED);
        if (it->id == PM_GRID && p->grid) f |= MI_CHECKED;
        if ((it->id == PM_UNDO && !p->nundo) || (it->id == PM_REDO && !p->nredo) || (it->id == PM_PASTE && !paint_clip)
            || ((it->id == PM_CUT || it->id == PM_COPY || it->id == PM_DEL || it->id == PM_CROP) && !p->sel)) f |= MI_DISABLED;
        it->flags = f;
    }
}
static void new_picture_cb(void *ctx, int r) {
    window_t *w = ctx; if (!wm_alive(w)) return; paint_t *p = w->data;
    if (r == IDCANCEL) return;
    if (r == IDYES) { paint_cmd(w, PM_SAVE); return; }
    canvas_t *c = canvas_new(560, 400);
    if (!c || !set_image(p, c, NULL)) return;
    p->sel = false; canvas_free(p->fl); p->fl = NULL; p->texting = false;
    stack_clear(p->undo, &p->nundo); stack_clear(p->redo, &p->nredo);
    p->path[0] = 0; p->modified = false; p->scx = p->scy = 0;
    paint_cmd(w, 0);
}
static void paint_key(window_t *w, key_event_t *e) {
    paint_t *p = w->data;
    if (!e->pressed) return;
    bool ctrl = e->mods & MOD_CTRL, shift = e->mods & MOD_SHIFT;
    if (ctrl) {
        static const struct { char k; int id; } keys[] = { { 'n', PM_NEW }, { 'o', PM_OPEN }, { 's', PM_SAVE }, { 'z', PM_UNDO }, { 'y', PM_REDO }, { 'x', PM_CUT },
            { 'c', PM_COPY }, { 'v', PM_PASTE }, { 'a', PM_ALL }, { 'g', PM_GRID }, { 'e', PM_SIZE }, { 'w', PM_STRETCH }, { 'i', PM_INVERT },
            { '=', PM_ZIN }, { '+', PM_ZIN }, { '-', PM_ZOUT }, { '0', PM_Z1 } };
        int k = tolower(e->key);
        if (k == 'n' && shift) { paint_cmd(w, PM_CLEAR); return; }
        for (int i = 0; i < (int)ARRAY_LEN(keys); i++) if (keys[i].k == k) { paint_cmd(w, keys[i].id); return; }
        if (e->key == KEY_PGUP) { paint_cmd(w, PM_ZIN); return; }
        if (e->key == KEY_PGDN) { paint_cmd(w, PM_ZOUT); return; }
        return;
    }
    if (p->texting) {      /* typing on the picture */
        if (e->key == KEY_ESC) { p->texting = false; }
        else if (e->key == KEY_BACKSPACE) { if (p->tlen) { do p->tlen--; while (p->tlen && (p->text[p->tlen] & 0xC0) == 0x80); p->text[p->tlen] = 0; } }
        else if (e->key == KEY_ENTER) { if (p->tlen < TEXT_MAX - 1) { p->text[p->tlen++] = '\n'; p->text[p->tlen] = 0; } }
        else if (e->ch && (unsigned char)e->ch >= 32 && p->tlen < TEXT_MAX - 2) { p->text[p->tlen++] = e->ch; p->text[p->tlen] = 0; }
        w->dirty = true;
        return;
    }
    int dx = e->key == KEY_LEFT ? -1 : e->key == KEY_RIGHT ? 1 : 0, dy = e->key == KEY_UP ? -1 : e->key == KEY_DOWN ? 1 : 0;
    if (dx || dy) {
        if (p->sel) { sel_lift(p); p->sr.x += dx * (shift ? 10 : 1); p->sr.y += dy * (shift ? 10 : 1); }   /* nudge the selection */
        else { p->scx += dx * 16; p->scy += dy * 16; clamp_scroll(w); }
        w->dirty = true;
        return;
    }
    switch (e->key) {
    case KEY_DELETE: paint_cmd(w, PM_DEL); return;
    case KEY_ESC: if (p->sel) { if (p->fl) paint_cmd(w, PM_UNDO); else p->sel = false; w->dirty = true; } return;   /* drop a moved selection back */
    case KEY_ENTER: commit_all(p); w->dirty = true; return;
    case KEY_PGUP: p->scy -= 64; clamp_scroll(w); w->dirty = true; return;
    case KEY_PGDN: p->scy += 64; clamp_scroll(w); w->dirty = true; return;
    }
    /* single letters pick tools, as in other paint programs */
    static const struct { char k; int t; } tk[] = { { 's', T_SELECT }, { 'z', T_MAGNIFY }, { 'p', T_PENCIL }, { 'b', T_BRUSH }, { 'a', T_AIRBRUSH }, { 'e', T_ERASER },
        { 'f', T_FILL }, { 'k', T_PICK }, { 't', T_TEXT }, { 'l', T_LINE }, { 'r', T_RECT }, { 'o', T_ELLIPSE } };
    int k = tolower(e->ch);
    if (k == 'x') { paint_cmd(w, PM_SWAP); return; }
    if (k == '[' || k == ']') { p->size = CLAMP(p->size + (k == ']' ? 1 : -1), 0, 3); w->dirty = true; return; }
    for (int i = 0; i < (int)ARRAY_LEN(tk); i++) if (tk[i].k == k) { commit_all(p); p->tool = tk[i].t; w->dirty = true; return; }
}
static void close_ask_cb(void *ctx, int r) {
    window_t *w = ctx; if (!wm_alive(w)) return; paint_t *p = w->data;
    if (r == IDYES) { p->closing = true; if (p->path[0]) paint_save_cb(w, p->path); else file_dialog("Save As (.bmp or .png)", USER_DOCS "\\untitled.bmp", true, NULL, paint_save_cb, w); }
    else if (r == IDNO) wm_destroy(w);
}
static bool paint_close(window_t *w) {
    paint_t *p = w->data;
    if (p->texting && p->tlen) text_commit(p);
    if (!p->modified) return true;
    char m[300]; snprintf(m, sizeof m, "Save changes to %s?", p->path[0] ? path_basename(p->path) : "untitled");
    msgbox("Paint", m, MBI_WARNING, MBX_YESNOCANCEL, close_ask_cb, w);
    return false;
}
static void paint_destroy(window_t *w) {
    paint_t *p = w->data;
    canvas_free(p->img); canvas_free(p->snap); canvas_free(p->view); canvas_free(p->fl);
    stack_clear(p->undo, &p->nundo); stack_clear(p->redo, &p->nredo);
    kfree(p);
}
static const menu_t paint_menus[] = {
    { "File", { { "New\tCtrl+N", PM_NEW, 0 }, { "Open...\tCtrl+O", PM_OPEN, 0 }, { "Save\tCtrl+S", PM_SAVE, 0 }, { "Save As...", PM_SAVEAS, 0 }, { "-", 0, 0 },
                { "Set As Desktop Background", PM_WALL, 0 }, { "-", 0, 0 }, { "Exit", PM_EXIT, 0 }, { NULL } } },
    { "Edit", { { "Undo\tCtrl+Z", PM_UNDO, MI_DISABLED }, { "Redo\tCtrl+Y", PM_REDO, MI_DISABLED }, { "-", 0, 0 }, { "Cut\tCtrl+X", PM_CUT, MI_DISABLED },
                { "Copy\tCtrl+C", PM_COPY, MI_DISABLED }, { "Paste\tCtrl+V", PM_PASTE, 0 }, { "Clear Selection\tDel", PM_DEL, MI_DISABLED },
                { "Select All\tCtrl+A", PM_ALL, 0 }, { "-", 0, 0 }, { "Paste From...", PM_PASTEFROM, 0 }, { NULL } } },
    { "View", { { "Zoom In\tCtrl++", PM_ZIN, 0 }, { "Zoom Out\tCtrl+-", PM_ZOUT, 0 }, { "Actual Size\tCtrl+0", PM_Z1, 0 }, { "-", 0, 0 },
                { "Show Grid\tCtrl+G", PM_GRID, 0 }, { NULL } } },
    { "Image", { { "Flip Horizontal", PM_FLIPH, 0 }, { "Flip Vertical", PM_FLIPV, 0 }, { "Rotate Right", PM_ROTR, 0 }, { "Rotate Left", PM_ROTL, 0 },
                 { "Rotate 180", PM_ROT180, 0 }, { "-", 0, 0 }, { "Stretch...\tCtrl+W", PM_STRETCH, 0 }, { "Canvas Size...\tCtrl+E", PM_SIZE, 0 },
                 { "Crop to Selection", PM_CROP, MI_DISABLED }, { "-", 0, 0 }, { "Invert Colors\tCtrl+I", PM_INVERT, 0 }, { "Black and White", PM_GRAY, 0 },
                 { "Clear Image\tCtrl+Shift+N", PM_CLEAR, 0 }, { NULL } } },
    { "Colors", { { "Edit Colors...", PM_EDITCOL, 0 }, { "Swap Colors\tX", PM_SWAP, 0 }, { NULL } } },
    { "Help", { { "About Paint", PM_ABOUT, 0 }, { NULL } } },
};
static bool pt_drop(window_t *w, int x, int y, const char *paths, bool copy) {
    (void)x; (void)y; (void)copy;
    char p[512]; const char *e = strchr(paths, '\n'); size_t n = e ? (size_t)(e - paths) : strlen(paths);
    snprintf(p, sizeof p, "%.*s", (int)MIN(n, sizeof p - 1), paths);
    if (!p[0]) return false;
    paint_load(w, p);
    return true;
}
void paint_open(const char *path) {
    if (path && *path && vfs_resolve(path, NULL)) { char full_[256]; vfs_path(vfs_resolve(path, NULL), full_, sizeof full_); recent_add(full_); }   /* for The WRLD Search */
    window_t *w = wm_create("untitled - Paint", -1, -1, 720, 540, WF_RESIZABLE);
    paint_t *p = kzalloc(sizeof *p);
    if (p) { p->img = canvas_new(560, 400); p->snap = canvas_new(560, 400); p->view = canvas_new(560, 400); }
    if (!p || !p->img || !p->snap || !p->view) {
        if (p) { canvas_free(p->img); canvas_free(p->snap); canvas_free(p->view); kfree(p); }
        wm_destroy(w);
        msgbox("Paint", "There is not enough memory to start Paint. Close some programs or windows and try again.", MBI_ERROR, MBX_OK, NULL, NULL);
        return;
    }
    p->fg = BLACK; p->bg = WHITE; p->size = 0; p->zoom = 1; p->tool = T_PENCIL; p->font = 1; p->mx = p->my = -1; p->pal_edit = -1;
    p->rng = timer_ticks() | 1;
    memcpy(p->pal, def_palette, sizeof p->pal);
    w->data = p; w->paint = paint_paint; w->mouse = paint_mouse; w->key = paint_key; w->menu_cmd = paint_cmd; w->close = paint_close;
    w->destroy = paint_destroy; w->icon = ICON_PAINT; w->drop = pt_drop; w->min_w = 420; w->min_h = 380;
    w->menus = kmalloc(sizeof paint_menus); memcpy(w->menus, paint_menus, sizeof paint_menus); w->nmenus = ARRAY_LEN(paint_menus);
    rect_t cr = wm_client_rect(w); wm_resize_client(w, cr.w, cr.h);
    if (path && *path) paint_load(w, path);
}
