/* Software rasteriser: framebuffer, primitives, anti-aliased fonts, icons, cursors */
#include "kernel.h"
#include "fs.h"
#include "gfx.h"
#include "ttf.h"

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
    fb = (uint32_t *)(uintptr_t)fb_info.addr;
    SCREEN_W = fb_info.width; SCREEN_H = fb_info.height;
    canvas_init(&screen, kmalloc(SCREEN_W * SCREEN_H * 4), SCREEN_W, SCREEN_H);
    memset32(screen.px, 0xFF000000, SCREEN_W * SCREEN_H);
}

/* change the screen resolution: switch the adapter, then resize the back buffer */
void (*gfx_resized_hook)(void);
bool gfx_set_resolution(int w, int h) {
    if (w == SCREEN_W && h == SCREEN_H) return true;
    if (!video_set_mode(w, h)) return false;
    fb = (uint32_t *)(uintptr_t)fb_info.addr;
    kfree(screen.px);
    SCREEN_W = w; SCREEN_H = h;
    canvas_init(&screen, kmalloc(w * h * 4), w, h);
    memset32(screen.px, 0xFF000000, w * h);
    if (gfx_resized_hook) gfx_resized_hook();
    return true;
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

/* saturation and lightness, -100..100 each: 0 leaves the colour unchanged,
 * saturation -100 gives grey, lightness +-100 gives white / black */
uint32_t color_adjust(uint32_t c, int sat, int light) {
    if (!sat && !light) return c;
    int r = C_R(c), g = C_G(c), b = C_B(c);
    if (sat) {
        int y = (r * 77 + g * 150 + b * 29) >> 8, f = 100 + CLAMP(sat, -100, 100);
        r = CLAMP(y + (r - y) * f / 100, 0, 255); g = CLAMP(y + (g - y) * f / 100, 0, 255); b = CLAMP(y + (b - y) * f / 100, 0, 255);
    }
    if (light > 0) { light = MIN(light, 100); r += (255 - r) * light / 100; g += (255 - g) * light / 100; b += (255 - b) * light / 100; }
    else if (light < 0) { light = MIN(-light, 100); r -= r * light / 100; g -= g * light / 100; b -= b * light / 100; }
    return (c & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
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
void fill_round_rect_blend(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, int alpha) {
    if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int in = 0;
        if (j < r) in = corner_inset(j, r); else if (j >= h - r) in = corner_inset(h - 1 - j, r);
        blend_rect(c, x + in, y + j, w - 2 * in, 1, col, alpha);
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
        fonts[i].data = p + 8 + 224 * sizeof(glyph_t) + 4;     /* glyphs for 32..255 */
        p += len;
    }
    fonts_loaded = true;
}
static inline font_t *F(int f) { if (!fonts_loaded) fonts_load(); return &fonts[f]; }
int font_height(int f) { return F(f)->height; }
int font_ascent(int f) { return F(f)->ascent; }
static inline const glyph_t *glyph(font_t *f, char ch) {
    uint8_t c = (uint8_t)ch;
    if (c < 32 || (c > 126 && c < 160)) c = '?';    /* Latin-1 160..255 are drawn */
    return &f->g[c - 32];
}

/* ---------------- scalable fonts ---------------- */
/* The bitmaps above are the fallback: before the file system is up, and if the
 * font files are missing, text is drawn exactly as it always was. Once the VFS
 * is available, the same family the bitmaps were generated from (Liberation,
 * SIL OFL) is loaded from C:\WRLD\Fonts and drawn by the TrueType engine: any
 * size, anti-aliased, and every character the font covers, read as UTF-8. */
enum { TF_REGULAR, TF_BOLD, TF_ITALIC, TF_BOLD_ITALIC, TF_MONO, TF_COUNT };

static const char *const ttf_files[TF_COUNT] = {
    "LiberationSans-Regular.ttf",
    "LiberationSans-Bold.ttf",
    "LiberationSans-Italic.ttf",
    "LiberationSans-BoldItalic.ttf",
    "LiberationMono-Regular.ttf",
};

typedef struct { uint8_t face, px; } font_spec_t;
static const font_spec_t font_specs[FONT_COUNT] = {
    [FONT_UI]         = { TF_REGULAR, 12 },
    [FONT_UI_BOLD]    = { TF_BOLD, 12 },
    [FONT_TITLE]      = { TF_BOLD, 13 },
    [FONT_LARGE]      = { TF_REGULAR, 18 },
    [FONT_LARGE_BOLD] = { TF_BOLD, 18 },
    [FONT_HUGE]       = { TF_BOLD, 30 },
    [FONT_START]      = { TF_BOLD_ITALIC, 17 },
    [FONT_MONO]       = { TF_MONO, 13 },
    [FONT_SMALL]      = { TF_REGULAR, 10 },
    [FONT_XL]         = { TF_REGULAR, 24 },
    [FONT_LOGO]       = { TF_BOLD, 46 },
    [FONT_WEB]        = { TF_REGULAR, 14 },
    [FONT_WEB_B]      = { TF_BOLD, 14 },
    [FONT_WEB_I]      = { TF_ITALIC, 14 },
    [FONT_WEB_BI]     = { TF_BOLD_ITALIC, 14 },
    [FONT_H1]         = { TF_BOLD, 28 },
    [FONT_H2]         = { TF_BOLD, 22 },
    [FONT_H3]         = { TF_BOLD, 17 },
    [FONT_WEB_SMALL]  = { TF_REGULAR, 12 },
};

static ttf_face_t *ttf_face[TF_COUNT];
static uint8_t *ttf_bytes[TF_COUNT];        /* the copies ttf_face points into */
static uint8_t ttf_tries[TF_COUNT];
static uint32_t ttf_retry[TF_COUNT];

static void *ttf_mem_alloc(void *user, size_t n) { (void)user; return kmalloc(n); }
static void ttf_mem_free(void *user, void *p) { (void)user; kfree(p); }

/* Load a face on first use. Before the initrd has been unpacked the lookup
 * fails, and the caller draws with the bitmaps; the attempt is retried a few
 * times a second for the first eight seconds. */
static ttf_face_t *ttf_get(int which) {
    if (which < 0 || which >= TF_COUNT) return NULL;
    if (ttf_face[which]) return ttf_face[which];
    if (ttf_tries[which] >= 32) return NULL;
    if (!vfs_root('C')) return NULL;
    if ((int32_t)(timer_ticks() - ttf_retry[which]) < 0) return NULL;
    ttf_tries[which]++;
    ttf_retry[which] = timer_ticks() + 250;
    char path[128];
    snprintf(path, sizeof path, "C:\\WRLD\\Fonts\\%s", ttf_files[which]);
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir || !n->data || n->size < 12) return NULL;
    /* The face keeps pointers into the file, so give it a copy of its own: a
     * write to the vnode (or the file being replaced) must not move it. */
    uint8_t *copy = kmalloc(n->size);
    if (!copy) return NULL;
    memcpy(copy, n->data, n->size);
    ttf_face_t *f = ttf_open(copy, n->size, ttf_mem_alloc, ttf_mem_free, NULL);
    if (!f) { kfree(copy); return NULL; }
    ttf_face[which] = f;
    ttf_bytes[which] = copy;
    klog("gfx: %s loaded (%u bytes)", path, (unsigned)n->size);
    return f;
}
static ttf_face_t *font_face(int font) {
    if (font < 0 || font >= FONT_COUNT) return NULL;
    return ttf_get(font_specs[font].face);
}

static int scale_units(int units, int px, int upem) {
    if (upem <= 0) return 0;
    int64_t v = (int64_t)units * px;
    if (v >= 0) return (int)((v + upem / 2) / upem);
    return -(int)((-v + upem / 2) / upem);
}
static int face_advance(ttf_face_t *f, int gid, int px) {
    return scale_units(ttf_advance(f, gid), px, ttf_units_per_em(f));
}
static int face_kern(ttf_face_t *f, int left, int right, int px) {
    return scale_units(ttf_kerning(f, left, right), px, ttf_units_per_em(f));
}

/* one UTF-8 character, or one Latin-1 byte when the sequence is not valid */
static uint32_t utf8_next(const uint8_t **pp, const uint8_t *end) {
    const uint8_t *p = *pp;
    uint8_t c = *p;
    if (c < 0x80) { *pp = p + 1; return c; }
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { *pp = p + 1; return c; }
    if (p + n >= end) { *pp = p + 1; return c; }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = p + 1; return c; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) { *pp = p + 1; return c; }
    *pp = p + n + 1;
    return cp;
}
static int char_len(const char *s, const char *end) {
    const uint8_t *p = (const uint8_t *)s, *q = p;
    (void)utf8_next(&q, (const uint8_t *)end);
    return (int)(q - p);
}
static int font_glyph(ttf_face_t *f, uint32_t cp) {
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) cp = '?';
    return ttf_glyph_index(f, cp);
}

/* Glyph cache: direct-mapped, keyed by font and glyph index (the font decides
 * the face and the size). 4096 entries are about 100 KiB of headers plus the
 * coverage bitmaps, and avoid rasterising the same letter every frame. */
#define GLYPH_CACHE 4096
typedef struct {
    uint16_t gid;
    uint8_t used, font;
    int16_t w, h, xoff, yoff, adv;
    uint8_t *cov;
} gcache_t;
static gcache_t gcache[GLYPH_CACHE];

static gcache_t *gcache_get(int font, ttf_face_t *f, int px, int gid) {
    uint32_t h = (uint32_t)gid * 2654435761u ^ (uint32_t)font * 2246822519u;
    h ^= h >> 15;
    gcache_t *e = &gcache[(h >> 4) & (GLYPH_CACHE - 1)];
    if (e->used && e->font == font && e->gid == (uint16_t)gid) return e;
    ttf_glyph_t g;
    if (ttf_render(f, gid, px, 0, &g) != 0) return NULL;
    if (e->cov) kfree(e->cov);
    e->cov = g.coverage;
    e->w = (int16_t)g.width; e->h = (int16_t)g.height;
    e->xoff = (int16_t)g.xoff; e->yoff = (int16_t)g.yoff;
    e->adv = (int16_t)g.advance;
    e->gid = (uint16_t)gid;
    e->font = (uint8_t)font;
    e->used = 1;
    return e;
}

/* The readable length of a string: at most n bytes, and never past the
 * terminator. Callers sometimes pass a one-byte buffer (a single character),
 * so this must not call strlen on it. */
static size_t text_span(const char *s, int n) {
    if (n < 0) return strlen(s);
    size_t len = 0;
    while (len < (size_t)n && s[len]) len++;
    return len;
}
static int ttf_width(int font, const char *s, int n) {
    ttf_face_t *f = font_face(font);
    if (!f) return -1;
    int px = font_specs[font].px;
    const uint8_t *p = (const uint8_t *)s, *end = p + text_span(s, n);
    int w = 0, prev = -1;
    while (p < end) {
        uint32_t cp = utf8_next(&p, end);
        if (cp == 0x00AD) continue;                     /* soft hyphen: invisible */
        if (cp == '\t') { w += face_advance(f, ttf_glyph_index(f, ' '), px) * 4; prev = -1; continue; }
        int gid = font_glyph(f, cp);
        if (prev >= 0) w += face_kern(f, prev, gid, px);
        w += face_advance(f, gid, px);
        prev = gid;
    }
    return w;
}
static int ttf_draw(canvas_t *c, int font, int x, int y, const char *s, int n, uint32_t col) {
    ttf_face_t *f = font_face(font);
    if (!f) return -1;
    int px = font_specs[font].px;
    const uint8_t *p = (const uint8_t *)s, *end = p + text_span(s, n);
    int base = y + F(font)->ascent, x0 = x, prev = -1;
    while (p < end) {
        uint32_t cp = utf8_next(&p, end);
        if (cp == 0x00AD) continue;                     /* soft hyphen: invisible */
        if (cp == '\t') { x += face_advance(f, ttf_glyph_index(f, ' '), px) * 4; prev = -1; continue; }
        int gid = font_glyph(f, cp);
        if (prev >= 0) x += face_kern(f, prev, gid, px);
        gcache_t *g = gcache_get(font, f, px, gid);
        if (g) {
            int gx = x + g->xoff, gy = base + g->yoff;
            if (g->w > 0 && gx + g->w > c->clip.x && gx < c->clip.x + c->clip.w &&
                gy + g->h > c->clip.y && gy < c->clip.y + c->clip.h) {
                for (int j = 0; j < g->h; j++) {
                    const uint8_t *row = g->cov + j * g->w;
                    for (int k = 0; k < g->w; k++) {
                        int a = row[k];
                        if (a) blend_pixel(c, gx + k, gy + j, col, a * C_A(col) / 255);
                    }
                }
            }
            x += g->adv;
        } else {
            x += face_advance(f, gid, px);
        }
        prev = gid;
    }
    return x - x0;
}
static int ttf_fit(int font, const char *s, int maxw) {
    ttf_face_t *f = font_face(font);
    if (!f) return -1;
    int px = font_specs[font].px;
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
    int w = 0, prev = -1;
    while (p < end) {
        const uint8_t *start = p;
        uint32_t cp = utf8_next(&p, end);
        int a;
        if (cp == 0x00AD) continue;                     /* soft hyphen: invisible */
        if (cp == '\t') { a = face_advance(f, ttf_glyph_index(f, ' '), px) * 4; prev = -1; }
        else {
            int gid = font_glyph(f, cp);
            a = face_advance(f, gid, px);
            if (prev >= 0) a += face_kern(f, prev, gid, px);
            prev = gid;
        }
        if (w + a > maxw) return (int)(start - (const uint8_t *)s);
        w += a;
    }
    return (int)(p - (const uint8_t *)s);
}
/* the advance of one character, for line breaking: no strlen, no kerning */
static int char_width_at(int font, const char *s, int cl) {
    ttf_face_t *f = font_face(font);
    if (!f) {
        if (s[0] == '\t') return glyph(F(font), ' ')->adv * 4;
        return glyph(F(font), s[0])->adv;
    }
    const uint8_t *p = (const uint8_t *)s, *end = p + cl;
    uint32_t cp = utf8_next(&p, end);
    if (cp == 0x00AD) return 0;                         /* soft hyphen: invisible */
    if (cp == '\t') return face_advance(f, ttf_glyph_index(f, ' '), font_specs[font].px) * 4;
    return face_advance(f, font_glyph(f, cp), font_specs[font].px);
}

int text_width_n(int font, const char *s, int n) {
    int w = ttf_width(font, s, n);
    if (w >= 0) return w;
    font_t *f = F(font); int bw = 0;
    for (int i = 0; i < n && s[i]; i++) { if (s[i] == '\t') bw += glyph(f, ' ')->adv * 4; else bw += glyph(f, s[i])->adv; }
    return bw;
}
int text_width(int font, const char *s) { return text_width_n(font, s, 0x7FFFFFFF); }
int draw_text_n(canvas_t *c, int font, int x, int y, const char *s, int n, uint32_t col) {
    int w = ttf_draw(c, font, x, y, s, n, col);
    if (w >= 0) return w;
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
    int n = ttf_fit(font, s, maxw);
    if (n >= 0) return n;
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
        const char *end = s + strlen(s);
        int n = 0, last_space = -1, w = 0;
        while (s + n < end && s[n] != '\n') {
            int cl = char_len(s + n, end);
            int a = char_width_at(font, s + n, cl);
            if (w + a > maxw && n > 0) break;
            if (s[n] == ' ') last_space = n;
            w += a; n += cl;
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
    video_update(r.x, r.y, r.w, r.h);
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
    rect_t r = rect_intersect(R(ox, oy, cu->w, cu->h), R(0, 0, SCREEN_W, SCREEN_H));
    video_update(r.x, r.y, r.w, r.h);
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

/* ---------------- THE WRLD OS globe + wordmark ---------------- */
extern const uint8_t asset_globe[];
typedef struct { int size; const uint32_t *lut, *ov; } globe_size_t;
static const uint16_t *globe_tex;
static int globe_tw, globe_th, globe_n;
static globe_size_t globe_sizes[4];
static void globe_load(void) {
    const uint32_t *h = (const uint32_t *)asset_globe;
    if (h[0] != 0x31424C47) return;     /* "GLB1" */
    globe_tw = h[1]; globe_th = h[2]; globe_n = MIN((int)h[3], 4);
    globe_tex = (const uint16_t *)(h + 4);
    const uint32_t *p = (const uint32_t *)(globe_tex + globe_tw * globe_th);
    for (int i = 0; i < globe_n; i++) {
        int s = *p++;
        globe_sizes[i].size = s; globe_sizes[i].lut = p; p += s * s;
        globe_sizes[i].ov = p; p += s * s;
    }
}
void draw_globe(canvas_t *c, int x, int y, int size, int rot) {
    if (!globe_tex) globe_load();
    if (!globe_n) { draw_icon(c, ICON_LOGO, 48, x, y); return; }
    globe_size_t *g = &globe_sizes[0];
    for (int i = 0; i < globe_n; i++) if (globe_sizes[i].size <= size) { g = &globe_sizes[i]; break; }
    int S = g->size, tw = globe_tw;
    rot = ((rot % tw) + tw) % tw;
    for (int j = 0; j < S; j++) {
        int py = y + j;
        if (py < c->clip.y || py >= c->clip.y + c->clip.h) continue;
        for (int i = 0; i < S; i++) {
            int px = x + i;
            if (px < c->clip.x || px >= c->clip.x + c->clip.w) continue;
            uint32_t e = g->lut[j * S + i], ov = g->ov[j * S + i];
            uint32_t *d = &c->px[py * c->stride + px];
            int a = (e >> 25) * 2 + ((e >> 25) >> 6);
            if (a) {
                int col = ((e & 511) + rot) % tw, row = (e >> 9) & 255, sh = ((e >> 17) & 255) * 276 >> 8;
                uint16_t t = globe_tex[row * tw + col];
                int r = ((t >> 11) << 3) * sh >> 8, gg = (((t >> 5) & 63) << 2) * sh >> 8, b = ((t & 31) << 3) * sh >> 8;
                uint32_t p = RGB(MIN(r, 255), MIN(gg, 255), MIN(b, 255));
                *d = a >= 255 ? p : mix(*d, p, a);
            }
            int oa = ov >> 24;
            if (oa) *d = mix(*d, ov | 0xFF000000, oa);
        }
    }
}
static const struct { int f_the, f_wrld, f_os, the_space, gap; } wm_styles[3] = {
    { FONT_UI_BOLD, FONT_LOGO, FONT_HUGE, 5, 8 },
    { FONT_SMALL, FONT_HUGE, FONT_LARGE_BOLD, 3, 5 },
    { FONT_SMALL, FONT_LARGE_BOLD, FONT_UI_BOLD, 2, 3 },
};
static int spaced_width(int f, const char *s, int sp) { int w = 0; for (; *s; s++) { char b[2] = { *s, 0 }; w += text_width(f, b) + sp; } return w - sp; }
int wordmark_width(int st) { return text_width(wm_styles[st].f_wrld, "WRLD") + wm_styles[st].gap + text_width(wm_styles[st].f_os, "OS"); }
int draw_wordmark(canvas_t *c, int x, int y, int st, uint32_t col) {
    int fw = wm_styles[st].f_wrld, fo = wm_styles[st].f_os, ft = wm_styles[st].f_the;
    int th = font_height(ft);
    /* "THE" letter-spaced over the left part of WRLD */
    int sx = x + 2;
    for (const char *s = "THE"; *s; s++) { char b[2] = { *s, 0 }; draw_text(c, ft, sx, y, b, mix(col, HEX(0x808080), 70)); sx += text_width(ft, b) + wm_styles[st].the_space; }
    (void)spaced_width;
    int wy = y + th - font_height(fw) / 7;
    draw_text(c, fw, x, wy, "WRLD", col);
    int ww = text_width(fw, "WRLD");
    int oy = wy + font_ascent(fw) - font_ascent(fo);
    draw_text(c, fo, x + ww + wm_styles[st].gap, oy, "OS", HEX(0xF08A24));
    return wordmark_width(st);
}

/* ---------------- blur (used for glass) ----------------
 * Box blur of a rectangle. Reads pixels up to `rad` outside the rectangle (so
 * strips blur smoothly into their surroundings) but only writes inside it. */
void blur_rect(canvas_t *c, int x, int y, int w, int h, int rad) {
    rect_t cl = c->clip;
    int x0 = MAX(x, cl.x), y0 = MAX(y, cl.y), x1 = MIN(x + w, cl.x + cl.w), y1 = MIN(y + h, cl.y + cl.h);
    if (rad <= 0 || x1 <= x0 || y1 <= y0) return;
    w = x1 - x0; h = y1 - y0;
    int ey0 = MAX(0, y0 - rad), ey1 = MIN(c->h, y1 + rad), eh = ey1 - ey0;
    static uint32_t *tmp; static int cap;
    if (w * eh > cap) { kfree(tmp); cap = w * eh; tmp = kmalloc(cap * 4); }
    /* horizontal pass into tmp (rows ey0..ey1, columns x0..x1) */
    for (int yy = ey0; yy < ey1; yy++) {
        const uint32_t *row = c->px + yy * c->stride;
        uint32_t r = 0, g = 0, b = 0; int n = 0;
        for (int k = x0 - rad; k <= x0 + rad; k++) { int kk = CLAMP(k, 0, c->w - 1); uint32_t p = row[kk]; r += C_R(p); g += C_G(p); b += C_B(p); n++; }
        uint32_t *out = tmp + (yy - ey0) * w;
        for (int xx = x0; xx < x1; xx++) {
            out[xx - x0] = RGB(r / n, g / n, b / n);
            uint32_t pa = row[CLAMP(xx - rad, 0, c->w - 1)], pb = row[CLAMP(xx + rad + 1, 0, c->w - 1)];
            r += C_R(pb) - C_R(pa); g += C_G(pb) - C_G(pa); b += C_B(pb) - C_B(pa);
        }
    }
    /* vertical pass from tmp back into the canvas */
    for (int xx = 0; xx < w; xx++) {
        uint32_t r = 0, g = 0, b = 0; int n = 0;
        for (int k = y0 - rad; k <= y0 + rad; k++) { int kk = CLAMP(k, ey0, ey1 - 1) - ey0; uint32_t p = tmp[kk * w + xx]; r += C_R(p); g += C_G(p); b += C_B(p); n++; }
        for (int yy = y0; yy < y1; yy++) {
            c->px[yy * c->stride + x0 + xx] = RGB(r / n, g / n, b / n);
            uint32_t pa = tmp[(CLAMP(yy - rad, ey0, ey1 - 1) - ey0) * w + xx], pb = tmp[(CLAMP(yy + rad + 1, ey0, ey1 - 1) - ey0) * w + xx];
            r += C_R(pb) - C_R(pa); g += C_G(pb) - C_G(pa); b += C_B(pb) - C_B(pa);
        }
    }
}
