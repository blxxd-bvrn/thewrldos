/* Software rasteriser: framebuffer, primitives, anti-aliased fonts, icons, cursors */
#include "kernel.h"
#include "gfx.h"

canvas_t screen;
int SCREEN_W, SCREEN_H;
fb_info_t fb_info;
static uint32_t *fb;

/* ---------------- canvas ---------------- */
void canvas_init(canvas_t *c, uint32_t *px, int w, int h) { c->px = px; c->w = w; c->h = h; c->stride = w; c->clip = R(0, 0, w, h); }
canvas_t *canvas_new(int w, int h) {
    canvas_t *c = kzalloc(sizeof(canvas_t));
    if (w < 1) w = 1; if (h < 1) h = 1;
    canvas_init(c, kmalloc(w * h * 4), w, h);
    memset32(c->px, 0xFFFFFFFF, w * h);
    return c;
}
void canvas_free(canvas_t *c) { if (!c) return; kfree(c->px); kfree(c); }
void canvas_resize(canvas_t *c, int w, int h) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w == c->w && h == c->h) return;
    uint32_t *n = kmalloc(w * h * 4);
    memset32(n, 0xFFFFFFFF, w * h);
    int cw = MIN(w, c->w), ch = MIN(h, c->h);
    for (int y = 0; y < ch; y++) memcpy(n + y * w, c->px + y * c->stride, cw * 4);
    kfree(c->px);
    canvas_init(c, n, w, h);
}
rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return R(x0, y0, 0, 0);
    return R(x0, y0, x1 - x0, y1 - y0);
}
void canvas_set_clip(canvas_t *c, rect_t r) { c->clip = rect_intersect(r, R(0, 0, c->w, c->h)); }
void canvas_reset_clip(canvas_t *c) { c->clip = R(0, 0, c->w, c->h); }
rect_t canvas_push_clip(canvas_t *c, rect_t r) { rect_t old = c->clip; c->clip = rect_intersect(old, r); return old; }

void gfx_init(void) {
    fb = (uint32_t *)fb_info.addr;
    SCREEN_W = fb_info.width; SCREEN_H = fb_info.height;
    canvas_init(&screen, kmalloc(SCREEN_W * SCREEN_H * 4), SCREEN_W, SCREEN_H);
    memset32(screen.px, 0xFF000000, SCREEN_W * SCREEN_H);
}

/* ---------------- colour ---------------- */
uint32_t mix(uint32_t a, uint32_t b, int t) {
    int r = C_R(a) + ((C_R(b) - C_R(a)) * t) / 255;
    int g = C_G(a) + ((C_G(b) - C_G(a)) * t) / 255;
    int bl = C_B(a) + ((C_B(b) - C_B(a)) * t) / 255;
    return RGB(r, g, bl);
}
uint32_t lighten(uint32_t c, int amt) { return mix(c, WHITE, amt); }
uint32_t darken(uint32_t c, int amt) { return mix(c, BLACK, amt); }
void rgb_to_hsv(uint32_t c, int *h, int *s, int *v) {
    int r = C_R(c), g = C_G(c), b = C_B(c);
    int mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), d = mx - mn;
    *v = mx; *s = mx ? d * 255 / mx : 0;
    if (!d) { *h = 0; return; }
    int hh;
    if (mx == r) hh = 60 * (g - b) / d;
    else if (mx == g) hh = 120 + 60 * (b - r) / d;
    else hh = 240 + 60 * (r - g) / d;
    if (hh < 0) hh += 360;
    *h = hh;
}
uint32_t hsv_to_rgb(int h, int s, int v) {
    h %= 360; if (h < 0) h += 360;
    int region = h / 60, rem = (h % 60) * 255 / 60;
    int p = v * (255 - s) / 255, q = v * (255 - (s * rem) / 255) / 255, t = v * (255 - (s * (255 - rem)) / 255) / 255;
    switch (region) {
    case 0: return RGB(v, t, p); case 1: return RGB(q, v, p); case 2: return RGB(p, v, t);
    case 3: return RGB(p, q, v); case 4: return RGB(t, p, v); default: return RGB(v, p, q);
    }
}
uint32_t hue_shift(uint32_t c, int deg) {
    if (!deg) return c;
    int h, s, v; rgb_to_hsv(c, &h, &s, &v);
    if (s < 12) return c;
    return (c & 0xFF000000u) | (hsv_to_rgb(h + deg, s, v) & 0xFFFFFF);
}

/* ---------------- primitives ---------------- */
void gfx_clear(canvas_t *c, uint32_t col) { for (int y = 0; y < c->h; y++) memset32(c->px + y * c->stride, col, c->w); }
void put_pixel(canvas_t *c, int x, int y, uint32_t col) { if (pt_in(c->clip, x, y)) c->px[y * c->stride + x] = col; }
uint32_t get_pixel(canvas_t *c, int x, int y) { if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0; return c->px[y * c->stride + x]; }
static inline uint32_t blend(uint32_t d, uint32_t s, int a) {
    uint32_t rb = ((s & 0xFF00FF) * a + (d & 0xFF00FF) * (255 - a)) >> 8;
    uint32_t g = ((s & 0x00FF00) * a + (d & 0x00FF00) * (255 - a)) >> 8;
    return 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
}
void blend_pixel(canvas_t *c, int x, int y, uint32_t col, int a) {
    if (!pt_in(c->clip, x, y) || a <= 0) return;
    uint32_t *p = &c->px[y * c->stride + x];
    *p = a >= 255 ? (col | 0xFF000000u) : blend(*p, col, a);
}
void fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col) {
    rect_t r = rect_intersect(R(x, y, w, h), c->clip);
    for (int j = 0; j < r.h; j++) memset32(c->px + (r.y + j) * c->stride + r.x, col, r.w);
}
void blend_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col, int a) {
    rect_t r = rect_intersect(R(x, y, w, h), c->clip);
    for (int j = 0; j < r.h; j++) { uint32_t *p = c->px + (r.y + j) * c->stride + r.x; for (int i = 0; i < r.w; i++) p[i] = blend(p[i], col, a); }
}
void hline(canvas_t *c, int x, int y, int w, uint32_t col) { fill_rect(c, x, y, w, 1, col); }
void vline(canvas_t *c, int x, int y, int h, uint32_t col) { fill_rect(c, x, y, 1, h, col); }
void draw_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col) {
    hline(c, x, y, w, col); hline(c, x, y + h - 1, w, col); vline(c, x, y, h, col); vline(c, x + w - 1, y, h, col);
}
void dotted_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col) {
    for (int i = 0; i < w; i += 2) { put_pixel(c, x + i, y, col); put_pixel(c, x + i, y + h - 1, col); }
    for (int i = 0; i < h; i += 2) { put_pixel(c, x, y + i, col); put_pixel(c, x + w - 1, y + i, col); }
}
void gradient_v(canvas_t *c, int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    for (int j = 0; j < h; j++) hline(c, x, y + j, w, mix(c1, c2, h > 1 ? j * 255 / (h - 1) : 0));
}
void gradient_v3(canvas_t *c, int x, int y, int w, int h, uint32_t c1, uint32_t c2, uint32_t c3, int mid) {
    if (mid < 1) mid = 1; if (mid > h - 1) mid = h - 1;
    gradient_v(c, x, y, w, mid, c1, c2);
    gradient_v(c, x, y + mid, w, h - mid, c2, c3);
}
void gradient_h(canvas_t *c, int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    for (int i = 0; i < w; i++) vline(c, x + i, y, h, mix(c1, c2, w > 1 ? i * 255 / (w - 1) : 0));
}
/* rounded-corner coverage: returns horizontal inset for row j of an r-radius corner */
static int corner_inset(int j, int r) {
    if (j >= r) return 0;
    int dy = r - j;
    int dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
    return r - dx;
}
void fill_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col) {
    if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int in = 0;
        if (j < r) in = corner_inset(j, r); else if (j >= h - r) in = corner_inset(h - 1 - j, r);
        hline(c, x + in, y + j, w - 2 * in, col);
    }
}
void gradient_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t c1, uint32_t c2) {
    if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int in = 0;
        if (j < r) in = corner_inset(j, r); else if (j >= h - r) in = corner_inset(h - 1 - j, r);
        hline(c, x + in, y + j, w - 2 * in, mix(c1, c2, h > 1 ? j * 255 / (h - 1) : 0));
    }
}
static int rr_inset(int j, int h, int r) {
    if (j < 0 || j >= h) return 1 << 20;
    if (j < r) return corner_inset(j, r);
    if (j >= h - r) return corner_inset(h - 1 - j, r);
    return 0;
}
void draw_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col) {
    if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int in = rr_inset(j, h, r);
        if (j == 0 || j == h - 1) { hline(c, x + in, y + j, w - 2 * in, col); continue; }
        int up = rr_inset(j - 1, h, r), dn = rr_inset(j + 1, h, r);
        int span = MAX(MAX(up, dn) - in, 1);
        hline(c, x + in, y + j, span, col);
        hline(c, x + w - in - span, y + j, span, col);
    }
}
void draw_line(canvas_t *c, int x0, int y0, int x1, int y1, uint32_t col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_pixel(c, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void fill_circle(canvas_t *c, int cx, int cy, int r, uint32_t col) {
    for (int y = -r; y <= r; y++) { int dx = 0; while ((dx + 1) * (dx + 1) + y * y <= r * r) dx++; hline(c, cx - dx, cy + y, dx * 2 + 1, col); }
}
void draw_thick_line(canvas_t *c, int x0, int y0, int x1, int y1, int t, uint32_t col) {
    if (t <= 1) { draw_line(c, x0, y0, x1, y1, col); return; }
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int n = MAX(dx, dy); if (!n) n = 1;
    for (int i = 0; i <= n; i++) fill_circle(c, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, t / 2, col);
}
void draw_circle(canvas_t *c, int cx, int cy, int r, uint32_t col) {
    int x = r, y = 0, e = 0;
    while (x >= y) {
        put_pixel(c, cx + x, cy + y, col); put_pixel(c, cx + y, cy + x, col); put_pixel(c, cx - y, cy + x, col); put_pixel(c, cx - x, cy + y, col);
        put_pixel(c, cx - x, cy - y, col); put_pixel(c, cx - y, cy - x, col); put_pixel(c, cx + y, cy - x, col); put_pixel(c, cx + x, cy - y, col);
        y++; e += 1 + 2 * y; if (2 * (e - x) + 1 > 0) { x--; e += 1 - 2 * x; }
    }
}
void bevel(canvas_t *c, int x, int y, int w, int h, bool raised) {
    uint32_t hi = raised ? WHITE : HEX(0x808080), lo = raised ? HEX(0x808080) : WHITE;
    hline(c, x, y, w, hi); vline(c, x, y, h, hi);
    hline(c, x, y + h - 1, w, lo); vline(c, x + w - 1, y, h, lo);
}
void bevel_deep(canvas_t *c, int x, int y, int w, int h, bool raised) {
    if (raised) {
        hline(c, x, y, w - 1, HEX(0xFFFFFF)); vline(c, x, y, h - 1, HEX(0xFFFFFF));
        hline(c, x, y + h - 1, w, HEX(0x404040)); vline(c, x + w - 1, y, h, HEX(0x404040));
        hline(c, x + 1, y + h - 2, w - 2, HEX(0x808080)); vline(c, x + w - 2, y + 1, h - 2, HEX(0x808080));
    } else {
        hline(c, x, y, w - 1, HEX(0x808080)); vline(c, x, y, h - 1, HEX(0x808080));
        hline(c, x + 1, y + 1, w - 3, HEX(0x404040)); vline(c, x + 1, y + 1, h - 3, HEX(0x404040));
        hline(c, x, y + h - 1, w, WHITE); vline(c, x + w - 1, y, h, WHITE);
        hline(c, x + 1, y + h - 2, w - 2, HEX(0xD4D0C8)); vline(c, x + w - 2, y + 1, h - 2, HEX(0xD4D0C8));
    }
}
void blit(canvas_t *dst, int dx, int dy, canvas_t *src, int sx, int sy, int w, int h) {
    rect_t r = rect_intersect(R(dx, dy, w, h), dst->clip);
    for (int j = 0; j < r.h; j++) {
        int syy = sy + (r.y - dy) + j, sxx = sx + (r.x - dx);
        if (syy < 0 || syy >= src->h) continue;
        int n = r.w; if (sxx + n > src->w) n = src->w - sxx;
        if (n <= 0 || sxx < 0) continue;
        memcpy(dst->px + (r.y + j) * dst->stride + r.x, src->px + syy * src->stride + sxx, n * 4);
    }
}
void blit_alpha(canvas_t *dst, int dx, int dy, canvas_t *src, int alpha) {
    rect_t r = rect_intersect(R(dx, dy, src->w, src->h), dst->clip);
    for (int j = 0; j < r.h; j++) {
        uint32_t *d = dst->px + (r.y + j) * dst->stride + r.x;
        uint32_t *s = src->px + (r.y - dy + j) * src->stride + (r.x - dx);
        for (int i = 0; i < r.w; i++) d[i] = blend(d[i], s[i], alpha);
    }
}
void flood_fill(canvas_t *c, int x, int y, uint32_t col) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    uint32_t target = c->px[y * c->stride + x];
    if (target == col) return;
    int cap = c->w * c->h;
    int *stack = kmalloc(cap * sizeof(int) < 4 * 1024 * 1024 ? cap * sizeof(int) : 4 * 1024 * 1024);
    int max = (cap * sizeof(int) < 4 * 1024 * 1024 ? cap : 1024 * 1024), sp = 0;
    stack[sp++] = y * c->w + x;
    while (sp) {
        int p = stack[--sp], px = p % c->w, py = p / c->w;
        int l = px; while (l > 0 && c->px[py * c->stride + l - 1] == target) l--;
        int r = px; while (r < c->w - 1 && c->px[py * c->stride + r + 1] == target) r++;
        for (int i = l; i <= r; i++) {
            c->px[py * c->stride + i] = col;
            if (py > 0 && c->px[(py - 1) * c->stride + i] == target && sp < max) stack[sp++] = (py - 1) * c->w + i;
            if (py < c->h - 1 && c->px[(py + 1) * c->stride + i] == target && sp < max) stack[sp++] = (py + 1) * c->w + i;
        }
    }
    kfree(stack);
}

/* ---------------- fonts ---------------- */
typedef struct PACKED { uint8_t w, h; int8_t xoff, yoff; uint8_t adv, pad; uint32_t off; } glyph_t;
typedef struct { int height, ascent, descent; const glyph_t *g; const uint8_t *data; } font_t;
extern const uint8_t asset_fonts[], asset_icons[];
static font_t fonts[FONT_COUNT];
static bool fonts_loaded;
static void fonts_load(void) {
    const uint8_t *p = asset_fonts;
    uint32_t n = *(const uint32_t *)p; p += 4;
    for (uint32_t i = 0; i < n && i < FONT_COUNT; i++) {
        uint32_t len = *(const uint32_t *)p; p += 4;
        const int16_t *h = (const int16_t *)p;
        fonts[i].height = h[0]; fonts[i].ascent = h[1]; fonts[i].descent = h[2];
        fonts[i].g = (const glyph_t *)(p + 8);
        fonts[i].data = p + 8 + 95 * sizeof(glyph_t) + 4;
        p += len;
    }
    fonts_loaded = true;
}
static inline font_t *F(int f) { if (!fonts_loaded) fonts_load(); return &fonts[f]; }
int font_height(int f) { return F(f)->height; }
int font_ascent(int f) { return F(f)->ascent; }
static inline const glyph_t *glyph(font_t *f, char ch) {
    uint8_t c = (uint8_t)ch;
    if (c < 32 || c > 126) c = '?';
    return &f->g[c - 32];
}
int text_width_n(int font, const char *s, int n) {
    font_t *f = F(font); int w = 0;
    for (int i = 0; i < n && s[i]; i++) { if (s[i] == '\t') w += glyph(f, ' ')->adv * 4; else w += glyph(f, s[i])->adv; }
    return w;
}
int text_width(int font, const char *s) { return text_width_n(font, s, 0x7FFFFFFF); }
int draw_text_n(canvas_t *c, int font, int x, int y, const char *s, int n, uint32_t col) {
    font_t *f = F(font);
    int base = y + f->ascent, x0 = x;
    for (int i = 0; i < n && s[i]; i++) {
        if (s[i] == '\t') { x += glyph(f, ' ')->adv * 4; continue; }
        const glyph_t *g = glyph(f, s[i]);
        if (g->w) {
            int gx = x + g->xoff, gy = base + g->yoff;
            const uint8_t *bm = f->data + g->off;
            if (gx + g->w > c->clip.x && gx < c->clip.x + c->clip.w && gy + g->h > c->clip.y && gy < c->clip.y + c->clip.h) {
                for (int j = 0; j < g->h; j++)
                    for (int k = 0; k < g->w; k++) {
                        int a = bm[j * g->w + k];
                        if (a) blend_pixel(c, gx + k, gy + j, col, a * C_A(col) / 255);
                    }
            }
        }
        x += g->adv;
    }
    return x - x0;
}
int draw_text(canvas_t *c, int font, int x, int y, const char *s, uint32_t col) { return draw_text_n(c, font, x, y, s, 0x7FFFFFFF, col); }
int text_fit_chars(int font, const char *s, int maxw) {
    font_t *f = F(font); int w = 0, i = 0;
    for (; s[i]; i++) { int a = glyph(f, s[i])->adv; if (w + a > maxw) break; w += a; }
    return i;
}
void draw_text_ellipsis(canvas_t *c, int font, int x, int y, int maxw, const char *s, uint32_t col) {
    if (text_width(font, s) <= maxw) { draw_text(c, font, x, y, s, col); return; }
    int ew = text_width(font, "...");
    int n = text_fit_chars(font, s, maxw - ew);
    int w = draw_text_n(c, font, x, y, s, n, col);
    draw_text(c, font, x + w, y, "...", col);
}
void draw_text_centered(canvas_t *c, int font, rect_t r, const char *s, uint32_t col) {
    int w = text_width(font, s);
    draw_text(c, font, r.x + (r.w - w) / 2, r.y + (r.h - font_height(font)) / 2, s, col);
}
static int wrap_lines(int font, int maxw, const char *s, canvas_t *c, int x, int y, uint32_t col, int gap) {
    int lh = font_height(font) + gap, lines = 0;
    while (*s) {
        int n = 0, last_space = -1, w = 0;
        while (s[n] && s[n] != '\n') {
            int a = text_width_n(font, s + n, 1);
            if (w + a > maxw && n > 0) break;
            if (s[n] == ' ') last_space = n;
            w += a; n++;
        }
        int brk = n;
        if (s[n] && s[n] != '\n' && last_space > 0) brk = last_space;
        if (c) draw_text_n(c, font, x, y + lines * lh, s, brk, col);
        lines++;
        s += brk;
        if (*s == ' ' || *s == '\n') s++;
    }
    return lines * lh;
}
int draw_text_wrapped(canvas_t *c, int font, int x, int y, int maxw, const char *s, uint32_t col, int gap) { return wrap_lines(font, maxw, s, c, x, y, col, gap); }
int text_wrapped_height(int font, int maxw, const char *s, int gap) { return wrap_lines(font, maxw, s, NULL, 0, 0, 0, gap); }

/* ---------------- icons ---------------- */
static const uint32_t *icon_ptr[ICON_COUNT][3];
static bool icons_loaded;
static void icons_load(void) {
    const uint8_t *p = asset_icons;
    uint32_t n = ((const uint32_t *)p)[0], ns = ((const uint32_t *)p)[1]; p += 8;
    for (uint32_t i = 0; i < n && i < ICON_COUNT; i++)
        for (uint32_t s = 0; s < ns; s++) {
            uint32_t sz = *(const uint32_t *)p; p += 4;
            if (s < 3) icon_ptr[i][s] = (const uint32_t *)p;
            p += sz * sz * 4;
        }
    icons_loaded = true;
}
const uint32_t *icon_pixels(int icon, int size) {
    if (!icons_loaded) icons_load();
    if (icon < 0 || icon >= ICON_COUNT) icon = ICON_FILE;
    return icon_ptr[icon][size >= 48 ? 0 : size >= 32 ? 1 : 2];
}
static void icon_draw(canvas_t *c, int icon, int size, int x, int y, int alpha, uint32_t tint, int tamt) {
    int base = size >= 48 ? 48 : size >= 32 ? 32 : 16;
    const uint32_t *px = icon_pixels(icon, size);
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            uint32_t p = px[(j * base / size) * base + (i * base / size)];
            int a = C_A(p) * alpha / 255;
            if (!a) continue;
            if (tamt) p = mix(p, tint, tamt);
            blend_pixel(c, x + i, y + j, p, a);
        }
}
void draw_icon(canvas_t *c, int icon, int size, int x, int y) { icon_draw(c, icon, size, x, y, 255, 0, 0); }
void draw_icon_alpha(canvas_t *c, int icon, int size, int x, int y, int alpha) { icon_draw(c, icon, size, x, y, alpha, 0, 0); }
void draw_icon_tinted(canvas_t *c, int icon, int size, int x, int y, uint32_t tint, int amt) { icon_draw(c, icon, size, x, y, 255, tint, amt); }

/* ---------------- cursors ---------------- */
typedef struct { int w, h, hx, hy; const char *art; } cursor_t;
static const cursor_t cursors[CURSOR_COUNT] = {
    [CURSOR_ARROW] = { 12, 19, 0, 0,
        "B..........."
        "BB.........."
        "BWB........."
        "BWWB........"
        "BWWWB......."
        "BWWWWB......"
        "BWWWWWB....."
        "BWWWWWWB...."
        "BWWWWWWWB..."
        "BWWWWWWWWB.."
        "BWWWWWWWWWB."
        "BWWWWWWBBBBB"
        "BWWWBWWB...."
        "BWWBBWWB...."
        "BWB..BWWB..."
        "BB...BWWB..."
        "B.....BWWB.."
        "......BWWB.."
        ".......BB..." },
    [CURSOR_IBEAM] = { 7, 16, 3, 8,
        "BBB.BBB" "...B..." "...B..." "...B..." "...B..." "...B..." "...B..." "...B..."
        "...B..." "...B..." "...B..." "...B..." "...B..." "...B..." "...B..." "BBB.BBB" },
    [CURSOR_HAND] = { 16, 18, 5, 0,
        ".....BB........."
        "....BWWB........"
        "....BWWB........"
        "....BWWB........"
        "....BWWBBB......"
        "....BWWBWWBBB..."
        "....BWWBWWBWWBB."
        ".BB.BWWBWWBWWBWB"
        "BWWBBWWWWWWWWBWB"
        "BWWWBWWWWWWWWWWB"
        ".BWWBWWWWWWWWWWB"
        "..BWWWWWWWWWWWWB"
        "..BWWWWWWWWWWWB."
        "...BWWWWWWWWWWB."
        "...BWWWWWWWWWB.."
        "....BWWWWWWWWB.."
        "....BWWWWWWWWB.."
        "....BBBBBBBBBB.." },
    [CURSOR_SIZE_NS] = { 9, 16, 4, 8,
        "....B...." "...BWB..." "..BWWWB.." ".BWWWWWB." "BBBBWBBBB" "...BWB..." "...BWB..." "...BWB..."
        "...BWB..." "...BWB..." "...BWB..." "BBBBWBBBB" ".BWWWWWB." "..BWWWB.." "...BWB..." "....B...." },
    [CURSOR_SIZE_EW] = { 16, 9, 8, 4,
        "....B......B...." "...BB......BB..." "..BWB......BWB.." ".BWWBBBBBBBBWWB." "BWWWWWWWWWWWWWWB"
        ".BWWBBBBBBBBWWB." "..BWB......BWB.." "...BB......BB..." "....B......B...." },
    [CURSOR_SIZE_NWSE] = { 11, 11, 5, 5,
        "BBBBBB....." "BWWWB......" "BWWB......." "BWBWB......" "BB.BWB....." "B...BWB...B"
        ".....BWB.BB" "......BWBWB" ".......BWWB" "......BWWWB" ".....BBBBBB" },
    [CURSOR_SIZE_NESW] = { 11, 11, 5, 5,
        ".....BBBBBB" "......BWWWB" ".......BWWB" "......BWBWB" ".....BWB.BB" "B...BWB...B"
        "BB.BWB....." "BWBWB......" "BWWB......." "BWWWB......" "BBBBBB....." },
    [CURSOR_BUSY] = { 11, 16, 5, 8,
        "BBBBBBBBBBB" "BWWWWWWWWWB" ".BWWWWWWWB." ".BWBWBWBWB." ".BWWBWBWWB." "..BWWBWWB.." "...BWBWB..." "....BWB...."
        "....BWB...." "...BWWWB..." "..BWWBWWB.." ".BWWWBWWWB." ".BWWBWBWWB." ".BWBWBWBWB." "BWWWWWWWWWB" "BBBBBBBBBBB" },
    [CURSOR_MOVE] = { 15, 15, 7, 7,
        ".......B......." "......BWB......" ".....BWWWB....." "....BBBWBBB...." "...B...W...B..." "..BB...W...BB.." ".BWBBBBWBBBBWB."
        "BWWWWWWWWWWWWWB" ".BWBBBBWBBBBWB." "..BB...W...BB.." "...B...W...B..." "....BBBWBBB...." ".....BWWWB....." "......BWB......" ".......B......." },
    [CURSOR_CROSS] = { 15, 15, 7, 7,
        ".......B......." ".......B......." ".......B......." ".......B......." ".......B......." "..............." "..............."
        "BBBBB.....BBBBB" "..............." "..............." ".......B......." ".......B......." ".......B......." ".......B......." ".......B......." },
};
static int cur_x = -100, cur_y = -100, cur_kind;

static void fb_copy_rect(int x, int y, int w, int h) {
    rect_t r = rect_intersect(R(x, y, w, h), R(0, 0, SCREEN_W, SCREEN_H));
    uint32_t pitch = fb_info.pitch / 4;
    for (int j = 0; j < r.h; j++) memcpy(fb + (r.y + j) * pitch + r.x, screen.px + (r.y + j) * SCREEN_W + r.x, r.w * 4);
}
static void fb_draw_cursor(int kind, int mx, int my) {
    const cursor_t *cu = &cursors[kind];
    uint32_t pitch = fb_info.pitch / 4;
    int ox = mx - cu->hx, oy = my - cu->hy;
    for (int j = 0; j < cu->h; j++)
        for (int i = 0; i < cu->w; i++) {
            char ch = cu->art[j * cu->w + i];
            int x = ox + i, y = oy + j;
            if (ch == '.' || x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) continue;
            uint32_t col = ch == 'B' ? 0 : 0xFFFFFF;
            if (kind == CURSOR_IBEAM || kind == CURSOR_CROSS) col = (screen.px[y * SCREEN_W + x] ^ 0xFFFFFF) & 0xFFFFFF;
            fb[y * pitch + x] = col;
        }
}
void gfx_present(int kind, int mx, int my) {
    fb_copy_rect(0, 0, SCREEN_W, SCREEN_H);
    fb_draw_cursor(kind, mx, my);
    cur_x = mx; cur_y = my; cur_kind = kind;
}
void gfx_move_cursor(int kind, int mx, int my) {
    if (mx == cur_x && my == cur_y && kind == cur_kind) return;
    const cursor_t *oc = &cursors[cur_kind];
    fb_copy_rect(cur_x - oc->hx, cur_y - oc->hy, oc->w, oc->h);
    fb_draw_cursor(kind, mx, my);
    cur_x = mx; cur_y = my; cur_kind = kind;
}
