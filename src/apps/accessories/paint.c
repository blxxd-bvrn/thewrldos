/* Paint */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

void file_dialog(const char *title, const char *initial, bool save, const char *ext, void (*cb)(void *, const char *), void *ctx);


/* ======================= Paint ======================= */
enum { T_PENCIL, T_BRUSH, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_ERASER, T_PICK, T_COUNT };
typedef struct { canvas_t *img, *snap; int tool; uint32_t fg, bg; bool drawing; int sx, sy, lx, ly; char path[256]; int size; bool modified; } paint_t;
static const uint32_t palette[28] = { 0x000000, 0x808080, 0x800000, 0x808000, 0x008000, 0x008080, 0x000080, 0x800080, 0x808040, 0x004040, 0x0080FF, 0x004080, 0x8000FF, 0x804000,
                                      0xFFFFFF, 0xC0C0C0, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFF80, 0x00FF80, 0x80FFFF, 0x8080FF, 0xFF0080, 0xFF8040 };
#define PAL_H 46
#define TOOL_W 56
static void tool_glyph(canvas_t *c, int t, int x, int y) {
    uint32_t k = T.text;
    switch (t) {
    case T_PENCIL: draw_thick_line(c, x + 3, y + 14, x + 13, y + 4, 3, HEX(0xE0A020)); draw_line(c, x + 2, y + 15, x + 4, y + 13, k); break;
    case T_BRUSH: draw_thick_line(c, x + 4, y + 12, x + 13, y + 3, 2, HEX(0x8D5A2B)); fill_circle(c, x + 4, y + 13, 3, HEX(0x2060E0)); break;
    case T_LINE: draw_thick_line(c, x + 2, y + 14, x + 14, y + 2, 2, k); break;
    case T_RECT: draw_rect(c, x + 2, y + 4, 13, 10, k); break;
    case T_ELLIPSE: draw_circle(c, x + 8, y + 8, 6, k); break;
    case T_FILL: fill_rect(c, x + 3, y + 6, 9, 8, HEX(0x2080FF)); draw_rect(c, x + 3, y + 6, 9, 8, k); draw_line(c, x + 12, y + 7, x + 15, y + 12, HEX(0x2080FF)); break;
    case T_ERASER: fill_rect(c, x + 3, y + 5, 11, 7, HEX(0xFFC0E0)); draw_rect(c, x + 3, y + 5, 11, 7, k); break;
    case T_PICK: draw_thick_line(c, x + 3, y + 13, x + 11, y + 5, 2, HEX(0x606060)); fill_circle(c, x + 12, y + 4, 3, k); break;
    }
}
static rect_t paint_area(window_t *w) { return R(TOOL_W + 4, 4, w->surf->w - TOOL_W - 8, w->surf->h - PAL_H - 8); }
static void paint_paint(window_t *w) {
    paint_t *p = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    rect_t a = paint_area(w);
    fill_rect(c, a.x, a.y, a.w, a.h, HEX(0x808080));
    blit(c, a.x + 2, a.y + 2, p->img, 0, 0, MIN(p->img->w, a.w - 4), MIN(p->img->h, a.h - 4));
    for (int i = 0; i < T_COUNT; i++) {
        rect_t r = R(6 + (i % 2) * 24, 6 + (i / 2) * 24, 22, 22);
        bool sel = p->tool == i;
        bool clicked = ui_click_area(w, r);
        fill_rect(c, r.x, r.y, r.w, r.h, sel ? T.window_bg : T.face);
        bevel_deep(c, r.x, r.y, r.w, r.h, !sel);
        tool_glyph(c, i, r.x + 3, r.y + 3);
        if (clicked) p->tool = i;
    }
    draw_sunken_box(c, R(6, 110, 46, 60), T.window_bg);
    static const int sizes[] = { 1, 3, 5, 8 };
    for (int i = 0; i < 4; i++) {
        rect_t r = R(8, 112 + i * 14, 42, 14);
        if (ui_click_area(w, r)) p->size = sizes[i];
        if (p->size == sizes[i]) fill_rect(c, r.x, r.y, r.w, r.h, T.select);
        fill_rect(c, r.x + 6, r.y + 7 - sizes[i] / 2, 30, MAX(1, sizes[i]), p->size == sizes[i] ? T.select_text : T.text);
    }
    int py = c->h - PAL_H;
    fill_rect(c, 6, py + 4, 34, 34, WHITE); bevel_deep(c, 6, py + 4, 34, 34, false);
    fill_rect(c, 20, py + 18, 14, 14, p->bg); draw_rect(c, 20, py + 18, 14, 14, BLACK);
    fill_rect(c, 11, py + 9, 14, 14, p->fg); draw_rect(c, 11, py + 9, 14, 14, BLACK);
    for (int i = 0; i < 28; i++) {
        rect_t r = R(46 + (i % 14) * 18, py + 4 + (i / 14) * 18, 17, 17);
        fill_rect(c, r.x, r.y, r.w, r.h, HEX(palette[i])); bevel_deep(c, r.x, r.y, r.w, r.h, false);
        if (ui_hover(w, r) && w->ui.pressed) p->fg = HEX(palette[i]);
        if (ui_hover(w, r) && w->ui.rpressed) p->bg = HEX(palette[i]);
    }
    char b[80]; snprintf(b, sizeof b, "%dx%d   %s", p->img->w, p->img->h, "Left-click palette: colour 1, right-click: colour 2");
    draw_text(c, FONT_SMALL, 46 + 14 * 18 + 10, py + 16, b, T.gray_text);
}
static void stamp(paint_t *p, int x, int y, uint32_t col) { int s = p->tool == T_BRUSH ? p->size + 2 : p->tool == T_ERASER ? p->size * 2 + 4 : p->size; if (s <= 1) put_pixel(p->img, x, y, col); else if (p->tool == T_ERASER) fill_rect(p->img, x - s / 2, y - s / 2, s, s, col); else fill_circle(p->img, x, y, s / 2, col); }
static void seg(paint_t *p, int x0, int y0, int x1, int y1, uint32_t col) {
    int n = MAX(ABSDIFF(x0, x1), ABSDIFF(y0, y1)); if (!n) n = 1;
    for (int i = 0; i <= n; i++) stamp(p, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, col);
}
static void shape(paint_t *p, int x0, int y0, int x1, int y1, uint32_t col) {
    if (p->tool == T_LINE) draw_thick_line(p->img, x0, y0, x1, y1, p->size, col);
    else if (p->tool == T_RECT) { int x = MIN(x0, x1), y = MIN(y0, y1), w = ABSDIFF(x0, x1) + 1, h = ABSDIFF(y0, y1) + 1; for (int k = 0; k < p->size; k++) draw_rect(p->img, x + k, y + k, w - 2 * k, h - 2 * k, col); }
    else if (p->tool == T_ELLIPSE) {
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, a = ABSDIFF(x0, x1) / 2, b = ABSDIFF(y0, y1) / 2;
        if (!a || !b) return;
        for (int i = 0; i < 720; i++) { extern int ext_isin(int); int ang = i * 1024 / 720; stamp(p, cx + ext_isin(ang) * a / 256, cy + ext_isin(ang + 256) * b / 256, col); }
    }
}
static void paint_mouse(window_t *w, int type, int x, int y, int btn) {
    paint_t *p = w->data; rect_t a = paint_area(w);
    int ix = x - a.x - 2, iy = y - a.y - 2;
    bool inside = ix >= 0 && iy >= 0 && ix < p->img->w && iy < p->img->h && pt_in(a, x, y);
    w->cursor = inside ? CURSOR_CROSS : CURSOR_ARROW;
    uint32_t col = (btn & MB_RIGHT) ? p->bg : p->fg;
    if (type == ME_DOWN && inside) {
        p->drawing = true; p->sx = p->lx = ix; p->sy = p->ly = iy; p->modified = true;
        if (p->tool == T_FILL) { flood_fill(p->img, ix, iy, col); p->drawing = false; }
        else if (p->tool == T_PICK) { uint32_t pc = get_pixel(p->img, ix, iy); if (btn & MB_RIGHT) p->bg = pc; else p->fg = pc; p->drawing = false; }
        else if (p->tool >= T_LINE && p->tool <= T_ELLIPSE) { memcpy(p->snap->px, p->img->px, p->img->w * p->img->h * 4); }
        else stamp(p, ix, iy, p->tool == T_ERASER ? p->bg : col);
        w->dirty = true;
    } else if (type == ME_MOVE && p->drawing) {
        uint32_t dc = w->ui.down ? p->fg : p->bg;
        if (p->tool >= T_LINE && p->tool <= T_ELLIPSE) { memcpy(p->img->px, p->snap->px, p->img->w * p->img->h * 4); shape(p, p->sx, p->sy, ix, iy, dc); }
        else seg(p, p->lx, p->ly, ix, iy, p->tool == T_ERASER ? p->bg : dc);
        p->lx = ix; p->ly = iy; w->dirty = true;
    } else if (type == ME_UP) p->drawing = false;
}
static void paint_save_cb(void *ctx, const char *path) {
    window_t *w = ctx; if (!wm_alive(w)) return; paint_t *p = w->data;
    char pp[300]; strlcpy(pp, path, sizeof pp); if (!str_ends_with_ci(pp, ".bmp")) strlcat(pp, ".bmp", sizeof pp);
    uint32_t sz; uint8_t *d = bmp_save(p->img, &sz);
    if (vfs_write_file(pp, d, sz)) { strlcpy(p->path, pp, sizeof p->path); p->modified = false; char t[300]; snprintf(t, sizeof t, "%s - Paint", path_basename(pp)); wm_set_title(w, t); }
    else msgbox("Paint", "Could not save the picture.", MBI_ERROR, MBX_OK, NULL, NULL);
    kfree(d);
}
static bool paint_load(window_t *w, const char *path) {
    paint_t *p = w->data; vnode_t *n = vfs_resolve(path, NULL);
    if (!n) return false;
    canvas_t *c = bmp_load(n->data, n->size);
    if (!c) { msgbox("Paint", "This is not a supported bitmap (24/32-bit uncompressed BMP), or there is not enough memory to open it.", MBI_ERROR, MBX_OK, NULL, NULL); return false; }
    canvas_t *snap = canvas_new(c->w, c->h);
    if (!snap) { canvas_free(c); msgbox("Paint", "There is not enough memory to open this picture.", MBI_ERROR, MBX_OK, NULL, NULL); return false; }
    canvas_free(p->img); canvas_free(p->snap); p->img = c; p->snap = snap;
    vfs_path(n, p->path, sizeof p->path);
    char t[300]; snprintf(t, sizeof t, "%s - Paint", n->name); wm_set_title(w, t);
    w->dirty = true; return true;
}
static void paint_open_cb(void *ctx, const char *path) { window_t *w = ctx; if (wm_alive(w)) paint_load(w, path); }
static void paint_menu(window_t *w, int id) {
    paint_t *p = w->data;
    switch (id) {
    case 1: gfx_clear(p->img, WHITE); p->path[0] = 0; wm_set_title(w, "untitled - Paint"); break;
    case 2: file_dialog("Open", USER_DOCS, false, ".bmp", paint_open_cb, w); break;
    case 3: if (p->path[0]) paint_save_cb(w, p->path); else file_dialog("Save As", USER_DOCS "\\untitled.bmp", true, ".bmp", paint_save_cb, w); break;
    case 4: file_dialog("Save As", p->path[0] ? p->path : USER_DOCS "\\untitled.bmp", true, ".bmp", paint_save_cb, w); break;
    case 5: wm_close(w); break;
    case 6: gfx_clear(p->img, p->bg); break;
    case 7: for (int i = 0; i < p->img->w * p->img->h; i++) p->img->px[i] = (p->img->px[i] ^ 0xFFFFFF) | 0xFF000000; break;
    case 8: about_open(); break;
    }
    w->dirty = true;
}
static void paint_destroy(window_t *w) { paint_t *p = w->data; canvas_free(p->img); canvas_free(p->snap); kfree(p); }
static const menu_t paint_menus[] = {
    { "File", { { "New", 1, 0 }, { "Open...", 2, 0 }, { "Save", 3, 0 }, { "Save As...", 4, 0 }, { "-", 0, 0 }, { "Exit", 5, 0 }, { NULL } } },
    { "Image", { { "Clear Image", 6, 0 }, { "Invert Colors", 7, 0 }, { NULL } } },
    { "Help", { { "About Paint", 8, 0 }, { NULL } } },
};
static bool pt_drop(window_t *w, int x, int y, const char *paths, bool copy) {
    (void)w; (void)x; (void)y; (void)copy;
    char p[512]; const char *e = strchr(paths, '\n'); size_t n = e ? (size_t)(e - paths) : strlen(paths);
    snprintf(p, sizeof p, "%.*s", (int)MIN(n, sizeof p - 1), paths);
    if (!p[0]) return false;
    paint_load(w, p);
    return true;
}
void paint_open(const char *path) {
    if (path && *path && vfs_resolve(path, NULL)) { char full_[256]; vfs_path(vfs_resolve(path, NULL), full_, sizeof full_); recent_add(full_); }   /* for The WRLD Search */
    window_t *w = wm_create("untitled - Paint", -1, -1, 680, 520, WF_RESIZABLE);
    paint_t *p = kzalloc(sizeof *p);
    if (p) { p->img = canvas_new(560, 400); p->snap = canvas_new(560, 400); }
    if (!p || !p->img || !p->snap) {
        if (p) { canvas_free(p->img); canvas_free(p->snap); kfree(p); }
        wm_destroy(w);
        msgbox("Paint", "There is not enough memory to start Paint. Close some programs or windows and try again.", MBI_ERROR, MBX_OK, NULL, NULL);
        return;
    }
    p->fg = BLACK; p->bg = WHITE; p->size = 1;
    w->data = p; w->paint = paint_paint; w->mouse = paint_mouse; w->menu_cmd = paint_menu; w->destroy = paint_destroy; w->icon = ICON_PAINT;
    w->drop = pt_drop;
    w->menus = kmalloc(sizeof paint_menus); memcpy(w->menus, paint_menus, sizeof paint_menus); w->nmenus = ARRAY_LEN(paint_menus);
    rect_t cr = wm_client_rect(w); wm_resize_client(w, cr.w, cr.h);
    if (path && *path) paint_load(w, path);
}
