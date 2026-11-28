#ifndef GFX_H
#define GFX_H
#include "types.h"
#include "assets.h"

/* TrueType-only sizes after the generated fonts (bitmap fallback in gfx.c) */
enum { FONT_DISPLAY_BI = FONT_COUNT, FONT_DISPLAY, FONT_ALL };   /* bold italic 30 px; regular 40 px */
/* any size of a family, made on demand (TrueType): a font id for draw_text, text_width & co */
enum { FAM_SANS, FAM_SERIF, FAM_MONO };
int font_sized(int family, int px, bool bold, bool italic);

#define RGB(r,g,b)    (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGBA(r,g,b,a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define HEX(v)        (0xFF000000u | (uint32_t)(v))
#define C_R(c) (((c) >> 16) & 0xFF)
#define C_G(c) (((c) >> 8) & 0xFF)
#define C_B(c) ((c) & 0xFF)
#define C_A(c) ((c) >> 24)
#define WHITE HEX(0xFFFFFF)
#define BLACK HEX(0x000000)

typedef struct { int x, y, w, h; } rect_t;
typedef struct canvas {
    uint32_t *px;
    int w, h, stride;
    rect_t clip;
} canvas_t;

extern canvas_t screen;
extern int SCREEN_W, SCREEN_H;
bool gfx_set_resolution(int w, int h);   /* runtime mode change (display driver) */
void gfx_refresh_framebuffer(void);     /* driver handoff without resizing the canvas */
void gfx_emergency_present(canvas_t *c, int x, int y);
void gfx_display_diagnostics(char *out, size_t size);
extern void (*gfx_resized_hook)(void);

static inline rect_t R(int x, int y, int w, int h) { rect_t r = { x, y, w, h }; return r; }
static inline bool pt_in(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
rect_t rect_intersect(rect_t a, rect_t b);

void gfx_init(void);
void canvas_init(canvas_t *c, uint32_t *px, int w, int h);
canvas_t *canvas_new(int w, int h);
void canvas_free(canvas_t *c);
void canvas_resize(canvas_t *c, int w, int h);
void canvas_set_clip(canvas_t *c, rect_t r);
void canvas_reset_clip(canvas_t *c);
rect_t canvas_push_clip(canvas_t *c, rect_t r);   /* intersect, return old */

uint32_t mix(uint32_t a, uint32_t b, int t);       /* t 0..255: a->b */
uint32_t lighten(uint32_t c, int amt);
uint32_t darken(uint32_t c, int amt);
uint32_t hue_shift(uint32_t c, int deg);
uint32_t color_adjust(uint32_t c, int sat, int light);
void rgb_to_hsv(uint32_t c, int *h, int *s, int *v);
uint32_t hsv_to_rgb(int h, int s, int v);

void gfx_clear(canvas_t *c, uint32_t col);
void put_pixel(canvas_t *c, int x, int y, uint32_t col);
void blend_pixel(canvas_t *c, int x, int y, uint32_t col, int alpha);
uint32_t get_pixel(canvas_t *c, int x, int y);
void fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void blend_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col, int alpha);
void blur_rect(canvas_t *c, int x, int y, int w, int h, int radius);
void draw_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void hline(canvas_t *c, int x, int y, int w, uint32_t col);
void vline(canvas_t *c, int x, int y, int h, uint32_t col);
void gradient_v(canvas_t *c, int x, int y, int w, int h, uint32_t c1, uint32_t c2);
void gradient_h(canvas_t *c, int x, int y, int w, int h, uint32_t c1, uint32_t c2);
void gradient_v3(canvas_t *c, int x, int y, int w, int h, uint32_t c1, uint32_t c2, uint32_t c3, int mid);
void fill_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col);
void fill_round_rect_blend(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, int alpha);
void draw_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col);
void gradient_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t c1, uint32_t c2);
void draw_line(canvas_t *c, int x0, int y0, int x1, int y1, uint32_t col);
void draw_thick_line(canvas_t *c, int x0, int y0, int x1, int y1, int t, uint32_t col);
void fill_circle(canvas_t *c, int cx, int cy, int r, uint32_t col);
void draw_circle(canvas_t *c, int cx, int cy, int r, uint32_t col);
void bevel(canvas_t *c, int x, int y, int w, int h, bool raised);
void bevel_deep(canvas_t *c, int x, int y, int w, int h, bool raised);
void dotted_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void blit(canvas_t *dst, int dx, int dy, canvas_t *src, int sx, int sy, int w, int h);
void blit_alpha(canvas_t *dst, int dx, int dy, canvas_t *src, int alpha);
void blit_scaled_alpha(canvas_t *dst, rect_t r, canvas_t *src, int alpha);
void flood_fill(canvas_t *c, int x, int y, uint32_t col);

/* text */
/* Strings are UTF-8. `n` is a byte count, and -1 or a value past the end means
 * "up to the terminator". With the TrueType fonts loaded any size and any
 * character the font covers is drawn; before that, and for Latin-1 callers,
 * the bitmap fonts are used, one byte per character. */
int font_height(int font);
int font_ascent(int font);
int text_width(int font, const char *s);
int text_width_n(int font, const char *s, int n);
int draw_text(canvas_t *c, int font, int x, int y, const char *s, uint32_t col);
int draw_text_n(canvas_t *c, int font, int x, int y, const char *s, int n, uint32_t col);
void draw_text_ellipsis(canvas_t *c, int font, int x, int y, int maxw, const char *s, uint32_t col);
void draw_text_centered(canvas_t *c, int font, rect_t r, const char *s, uint32_t col);
int draw_text_wrapped(canvas_t *c, int font, int x, int y, int maxw, const char *s, uint32_t col, int line_gap);
int text_wrapped_height(int font, int maxw, const char *s, int line_gap);
int text_fit_chars(int font, const char *s, int maxw);   /* bytes that fit */

/* icons */
void draw_icon(canvas_t *c, int icon, int size, int x, int y);
void draw_icon_alpha(canvas_t *c, int icon, int size, int x, int y, int alpha);
void draw_icon_tinted(canvas_t *c, int icon, int size, int x, int y, uint32_t tint, int amt);
const uint32_t *icon_pixels(int icon, int size);

/* THE WRLD OS branding: spinning globe (sizes 160, 96, 64) and the wordmark */
void draw_globe(canvas_t *c, int x, int y, int size, int rot);   /* rot: 0..511 = one revolution */
enum { WORDMARK_LARGE, WORDMARK_MEDIUM, WORDMARK_SMALL };
int draw_wordmark(canvas_t *c, int x, int y, int style, uint32_t col);   /* returns width */
int wordmark_width(int style);

/* cursor + presentation */
enum { CURSOR_ARROW, CURSOR_IBEAM, CURSOR_HAND, CURSOR_SIZE_NS, CURSOR_SIZE_EW, CURSOR_SIZE_NWSE, CURSOR_SIZE_NESW, CURSOR_BUSY, CURSOR_MOVE, CURSOR_CROSS, CURSOR_COUNT };
#define CURSOR_NONE 0x7F                /* no pointer drawn: a program hid it (not 0, which is "unset" for windows) */
void gfx_present(int cursor, int mx, int my);
void gfx_present_region(int cursor, int mx, int my, rect_t r);   /* only r changed (and the cursor) */
void gfx_move_cursor(int cursor, int mx, int my);
/* cursor sets: folders under C:\WRLD\Cursors with Windows .cur files */
int cursor_set_count(void);
const char *cursor_set_name(int i);
int cursor_set_current(void);
const char *cursor_set_active(void);
bool cursor_set_load(const char *name);
#endif
