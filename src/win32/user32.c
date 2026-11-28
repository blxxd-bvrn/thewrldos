/* user32.dll + gdi32.dll: Win32 windows are hosted as WRLD WM windows; messages are
 * queued per process and dispatched on the program's own thread. */
#include "w32.h"
#include "theme.h"

#define P w32_cur()

enum { WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_SETTEXT = 0xC, WM_GETTEXT = 0xD,
       WM_GETTEXTLENGTH = 0xE, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_SETFONT = 0x30, WM_GETFONT = 0x31, WM_NCCREATE = 0x81,
       WM_NCDESTROY = 0x82, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_COMMAND = 0x111, WM_SYSCOMMAND = 0x112, WM_TIMER = 0x113, WM_MOUSEMOVE = 0x200,
       WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MOUSEWHEEL = 0x20A, WM_USER = 0x400 };
#define BM_GETCHECK 0xF0
#define BM_SETCHECK 0xF1
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_THICKFRAME 0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define CW_USEDEFAULT 0x80000000u
#define W32MAGIC 0x4857494E
#define DCMAGIC 0x48444321
#define OBJMAGIC 0x4F424A21

enum { CTL_NONE, CTL_BUTTON, CTL_STATIC, CTL_EDIT, CTL_LISTBOX, CTL_COMBO, CTL_TREE, CTL_SCROLLBAR,
       CTL_TOOLBAR = 20, CTL_STATUS, CTL_REBAR, CTL_PROGRESS, CTL_TRACKBAR, CTL_UPDOWN, CTL_TAB, CTL_LISTVIEW, CTL_HEADER, CTL_QUIET, CTL_TOOLTIP };   /* comctl.inc */
typedef struct w32cls { char name[64]; void *proc; uintptr_t brush; uint32_t style; w32_proc_t *owner; struct w32cls *next; int wnd_extra; bool unicode;
                       uintptr_t menu_id; char menu_name[64]; HINSTANCE inst; } w32cls_t;   /* the class menu (lpszMenuName) */
struct tvitem;
typedef struct w32win {
    uint32_t magic;
    w32_proc_t *proc;
    w32cls_t *cls;
    int ctl;
    window_t *top;
    struct w32win *parent, *child, *next, *all_next;
    int x, y, w, h;                /* child: rect in parent client; top: client size */
    char text[4096];
    uint32_t style, exstyle; uintptr_t id, userdata;
    void *wndproc;
    bool need_paint, erase, visible, enabled, destroyed;
    bool pressed, hot, checked;
    int caret;
    uintptr_t font;
    uintptr_t menu;
    char **lb_items; int lb_n, lb_sel;
    /* added for real programs (dialog.c-style parts below) */
    uint8_t *extra; int nextra;          /* cbWndExtra: GetWindowLong(Ptr) with an index >= 0 */
    bool unicode;                        /* a Unicode window procedure (RegisterClassW) */
    bool is_dialog, dlg_modal, dlg_ended; intptr_t dlg_ret;
    uintptr_t *lb_data; uint8_t *lb_selbits; int lb_top;    /* listbox / combo box */
    int cb_droph; menu_item_t *cb_menu;  /* combo box: dropped-down list height, its popup */
    bool sent_size; struct tvitem *tv_root, *tv_sel; int tv_scroll; bool tv_unicode; void *tv_nm;   /* tree view */
    int limit;                           /* edit: EM_LIMITTEXT */
    void *cc;                            /* a common control's state (comctl.inc) */
} w32win_t;
/* comctl.inc */
static int cc_class(const char *n); static const char *cc_class_name(int ctl); static void cc_created(w32win_t *w);
static void cc_draw(w32win_t *c, canvas_t *s, rect_t r, int f); static bool cc_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res);
static bool cc_mouse(w32win_t *w, int type, int lx, int ly, int btn); static bool cc_key(w32win_t *w, key_event_t *e);
static w32win_t *all_wins;
static w32cls_t *classes;
typedef struct { uintptr_t hwnd, id; uint32_t ms, next; w32_proc_t *proc; void *fn; bool used; } w32timer_t;
static w32timer_t timers[64];
static uint8_t keystate[256];
/* per virtual key: the toggle (low bit of GetKeyState, flips on each press, as Caps Lock's
 * light does) and "pressed since GetAsyncKeyState last asked" (its low bit) */
static uint8_t keytoggle[256], keyhit[256];
static void key_set(int vk, bool down) {
    vk &= 0xFF;
    if (down && !keystate[vk]) { keytoggle[vk] ^= 1; keyhit[vk] = 1; }
    keystate[vk] = down;
}
static uint8_t key_char[256];                /* vk -> the character of its last key press */
static w32win_t *focus_win;

static w32win_t *W(uintptr_t h) { w32win_t *w = (w32win_t *)h; if (!h || h < 0x100000 || h & 3) return NULL; for (w32win_t *x = all_wins; x; x = x->all_next) if (x == w) return w->destroyed ? NULL : w; return NULL; }
static w32win_t *top_of(w32win_t *w) { while (w && w->parent) w = w->parent; return w; }
static void abs_origin(w32win_t *w, int *ox, int *oy) { *ox = 0; *oy = 0; for (w32win_t *c = w; c && c->parent; c = c->parent) { *ox += c->x; *oy += c->y; } }
static uint32_t cref(COLORREF c) { return RGB(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF); }
static LRESULT send(w32win_t *w, UINT msg, WPARAM wp, LPARAM lp);

/* ---------------- GDI objects ---------------- */
enum { OBJ_PEN = 1, OBJ_BRUSH, OBJ_FONT, OBJ_BITMAP, OBJ_REGION, OBJ_PALETTE };
struct dib; struct rgn;
typedef struct { uint32_t magic; int type; uint32_t color; int width, style; bool stock; int font; canvas_t *bmp; bool null;
                 struct dib *dib;          /* a DIB section: the program's own pixel buffer (user32_more.inc) */
                 struct rgn *rgn;          /* a region */
                 int font_h, font_weight; bool font_italic, font_underline; char face[32]; } gdiobj_t;
static gdiobj_t stock[20];
typedef struct {
    uint32_t magic; w32win_t *win; canvas_t *surf; int ox, oy; rect_t clip;
    gdiobj_t *pen, *brush, *font, *bitmap;
    uint32_t text, bk; int bkmode; int cx, cy; bool memdc; uint32_t align;
} dc_t;
static gdiobj_t default_bitmap;
static gdiobj_t *OBJ(uintptr_t h) {
    if (h >= 1 && h <= 31) { /* system colour brush (COLOR_x + 1) */
        static gdiobj_t sys[32];
        const uint32_t cols[32] = { T.sb_track, T.desktop_bg, T.title_top, T.ititle_top, T.menu_bg, T.window_bg,
            T.frame_out, T.text, T.text, T.title_text, T.frame_mid, T.iframe_mid, T.face_dark, T.select, T.select_text,
            T.face, T.face_dark, T.gray_text, T.text, T.ititle_text, T.face_light, mix(T.face_dark, BLACK, 128), T.face,
            T.text, dark_mode ? T.face : HEX(0xFFFFE1), T.face, T.link, T.title_bot, T.ititle_bot, T.hot, T.face, T.face };
        int i = h - 1;
        sys[i].magic = OBJMAGIC; sys[i].type = OBJ_BRUSH; sys[i].color = cols[i]; sys[i].stock = true;
        return &sys[i];
    }
    gdiobj_t *o = (gdiobj_t *)h;
    if (h < 0x100000 || (h & 3)) return NULL;
    for (int i = 0; i < 20; i++) if (o == &stock[i]) return o;
    if (o == &default_bitmap) return o;
    return o->magic == OBJMAGIC ? o : NULL;
}
static dc_t *DC(uintptr_t h) { dc_t *d = (dc_t *)h; if (h < 0x100000 || (h & 3)) return NULL; return d->magic == DCMAGIC ? d : NULL; }
/* the OS font a DC's selected font is drawn with (gl.c: wglUseFontBitmaps) */
int w32_dc_font(uintptr_t h) { dc_t *d = DC(h); return d && d->font ? d->font->font : FONT_UI; }
static void stock_init(void) {
    static const struct { int type; uint32_t col; bool null; } s[] = {
        { OBJ_BRUSH, 0xFFFFFF }, { OBJ_BRUSH, 0xC0C0C0 }, { OBJ_BRUSH, 0x808080 }, { OBJ_BRUSH, 0x404040 }, { OBJ_BRUSH, 0x000000 }, { OBJ_BRUSH, 0, true },
        { OBJ_PEN, 0xFFFFFF }, { OBJ_PEN, 0x000000 }, { OBJ_PEN, 0, true }, { 0 }, { OBJ_FONT }, { OBJ_FONT }, { OBJ_FONT }, { OBJ_FONT }, { OBJ_FONT }, { OBJ_FONT }, { OBJ_FONT }, { OBJ_FONT }, { OBJ_BRUSH, 0xD4D0C8 } };
    for (unsigned i = 0; i < ARRAY_LEN(s); i++) { stock[i].magic = OBJMAGIC; stock[i].type = s[i].type; stock[i].color = HEX(s[i].col); stock[i].null = s[i].null; stock[i].stock = true; stock[i].width = 1; stock[i].font = i == 16 ? FONT_MONO : i == 10 || i == 11 ? FONT_MONO : FONT_UI; }
}
static HGDIOBJ WINAPI GetStockObject(int i) { return (i >= 0 && i < 20) ? (HGDIOBJ)&stock[i] : 0; }
static gdiobj_t *newobj(int type) { gdiobj_t *o = w32_alloc(P, sizeof(gdiobj_t)); memset(o, 0, sizeof *o); o->magic = OBJMAGIC; o->type = type; o->width = 1; return o; }
static HGDIOBJ WINAPI CreatePen(int style, int width, COLORREF c) { gdiobj_t *o = newobj(OBJ_PEN); o->color = cref(c); o->width = MAX(1, width); o->style = style; o->null = style == 5; return (HGDIOBJ)o; }
static HGDIOBJ WINAPI CreateSolidBrush(COLORREF c) { gdiobj_t *o = newobj(OBJ_BRUSH); o->color = cref(c); return (HGDIOBJ)o; }
static HGDIOBJ WINAPI CreateHatchBrush(int s, COLORREF c) { return CreateSolidBrush(c); }
typedef struct { LONG style; COLORREF color; LONG hatch; } LOGBRUSH;
static HGDIOBJ WINAPI CreateBrushIndirect(const LOGBRUSH *b) { gdiobj_t *o = (gdiobj_t *)CreateSolidBrush(b->color); o->null = b->style == 1; return (HGDIOBJ)o; }
static HGDIOBJ WINAPI CreateFontA(int h, int w, int esc, int ori, int weight, DWORD it, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pitch, const char *face) {
    gdiobj_t *o = newobj(OBJ_FONT);
    int ah = h < 0 ? -h : h;
    bool bold = weight >= 600;
    bool mono = face && (strstr(face, "Courier") || strstr(face, "Consol") || strstr(face, "Mono") || strstr(face, "Fixed") || strstr(face, "Terminal"));
    if (mono) o->font = FONT_MONO;
    else if (ah >= 26) o->font = bold ? FONT_HUGE : FONT_XL;
    else if (ah >= 17) o->font = bold ? FONT_LARGE_BOLD : FONT_LARGE;
    else if (ah && ah <= 10) o->font = FONT_SMALL;
    else o->font = bold ? FONT_UI_BOLD : FONT_UI;
    return (HGDIOBJ)o;
}
typedef struct { LONG h, w, esc, ori, weight; BYTE it, ul, so, cs, op, cp, q, pf; char face[32]; } LOGFONTA;
static HGDIOBJ WINAPI CreateFontIndirectA(const LOGFONTA *l) { return CreateFontA(l->h, l->w, 0, 0, l->weight, l->it, 0, 0, 0, 0, 0, 0, 0, l->face); }
static BOOL WINAPI DeleteObject(HGDIOBJ h) { gdiobj_t *o = OBJ(h); if (!o || o->stock) return 1; o->magic = 0; if (o->bmp) canvas_free(o->bmp); w32_free(P, o); return 1; }
static HGDIOBJ WINAPI SelectObject(HDC h, HGDIOBJ obj) {
    dc_t *d = DC(h); gdiobj_t *o = OBJ(obj);
    if (!d || !o) return 0;
    gdiobj_t *old = NULL;
    switch (o->type) {
    case OBJ_PEN: old = d->pen; d->pen = o; break;
    case OBJ_BRUSH: old = d->brush; d->brush = o; break;
    case OBJ_FONT: old = d->font; d->font = o; break;
    case OBJ_BITMAP: old = d->bitmap; d->bitmap = o; if (d->memdc && o->bmp) { d->surf = o->bmp; d->clip = R(0, 0, o->bmp->w, o->bmp->h); } break;
    }
    return (HGDIOBJ)old;
}
static int WINAPI GetObjectA(HGDIOBJ h, int n, void *out) {
    gdiobj_t *o = OBJ(h); if (!o) return 0;
    if (o->type == OBJ_BITMAP && n >= 24) { LONG *b = out; memset(b, 0, n); b[1] = o->bmp->w; b[2] = o->bmp->h; b[3] = o->bmp->w * 4; ((WORD *)b)[8] = 1; ((WORD *)b)[9] = 32; return 24; }
    return 0;
}

/* ---------------- DCs ---------------- */
static dc_t *newdc(w32win_t *w) {
    dc_t *d = w32_alloc(P, sizeof(dc_t)); memset(d, 0, sizeof *d);
    d->magic = DCMAGIC; d->win = w;
    d->pen = &stock[7]; d->brush = &stock[0]; d->font = &stock[13]; d->text = BLACK; d->bk = WHITE; d->bkmode = 2;
    if (w) {
        w32win_t *t = top_of(w);
        if (t->top) {
            d->surf = t->top->surf;
            abs_origin(w, &d->ox, &d->oy);
            d->clip = w->parent ? R(d->ox, d->oy, w->w, w->h) : R(0, 0, d->surf->w, d->surf->h);
        }
    }
    return d;
}
static HDC WINAPI GetDC(HWND h) { w32win_t *w = W(h); if (h && !w) return 0; if (!w) { dc_t *d = newdc(NULL); d->surf = &screen; d->clip = R(0, 0, SCREEN_W, SCREEN_H); return (HDC)d; } return (HDC)newdc(w); }
static HDC WINAPI GetWindowDC(HWND h) { return GetDC(h); }
static HWND WINAPI WindowFromDC(HDC hdc) { dc_t *d = DC(hdc); return d && d->win ? (HWND)d->win : 0; }
static int WINAPI ReleaseDC(HWND h, HDC hdc) { dc_t *d = DC(hdc); if (!d) return 0; d->magic = 0; w32_free(P, d); wm_need_redraw = true; return 1; }
static HDC WINAPI CreateCompatibleDC(HDC h) { dc_t *d = newdc(NULL); d->memdc = true; static canvas_t one; static uint32_t px; canvas_init(&one, &px, 1, 1); d->surf = &one; d->clip = R(0, 0, 1, 1); return (HDC)d; }
static BOOL WINAPI DeleteDC(HDC h) { dc_t *d = DC(h); if (!d) return 0; d->magic = 0; w32_free(P, d); return 1; }
static HGDIOBJ WINAPI CreateCompatibleBitmap(HDC h, int w, int ht) {
    gdiobj_t *o = newobj(OBJ_BITMAP);
    o->bmp = canvas_new(MAX(1, w), MAX(1, ht));
    if (!o->bmp) { o->magic = 0; w32_free(P, o); P->last_error = 8; return 0; }   /* ERROR_NOT_ENOUGH_MEMORY */
    gfx_clear(o->bmp, BLACK);
    return (HGDIOBJ)o;
}
static HGDIOBJ WINAPI CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits) {
    gdiobj_t *o = (gdiobj_t *)CreateCompatibleBitmap(0, w, h);
    if (o && bits && bpp == 32 && w > 0 && h > 0) memcpy(o->bmp->px, bits, (size_t)w * h * 4);
    return (HGDIOBJ)o;
}
static void dib_in(gdiobj_t *o);   /* user32_more.inc: a DIB section's bits to its canvas, and back */
static void dib_out(gdiobj_t *o);
static void dc_begin(dc_t *d, rect_t *save) { if (d->bitmap && d->bitmap->dib && d->memdc) dib_in(d->bitmap); *save = d->surf->clip; d->surf->clip = rect_intersect(R(0, 0, d->surf->w, d->surf->h), d->clip); }
static void dc_end(dc_t *d, rect_t save) { d->surf->clip = save; if (d->bitmap && d->bitmap->dib && d->memdc) dib_out(d->bitmap); if (!d->memdc) wm_need_redraw = true; }
static void dc_src(dc_t *s) { if (s && s->memdc && s->bitmap && s->bitmap->dib) dib_in(s->bitmap); }
static int WINAPI SetBkMode(HDC h, int m) { dc_t *d = DC(h); if (!d) return 0; int o = d->bkmode; d->bkmode = m; return o; }
static COLORREF WINAPI SetBkColor(HDC h, COLORREF c) { dc_t *d = DC(h); if (!d) return 0; d->bk = cref(c); return 0; }
static COLORREF WINAPI SetTextColor(HDC h, COLORREF c) { dc_t *d = DC(h); if (!d) return 0; d->text = cref(c); return 0; }
static UINT WINAPI SetTextAlign(HDC h, UINT a) { dc_t *d = DC(h); if (!d) return 0; UINT o = d->align; d->align = a; return o; }
static int WINAPI SetROP2(HDC h, int m) { return 13; }
static int WINAPI SetMapMode(HDC h, int m) { return 1; }
static int dcfont(dc_t *d) { return d->font ? d->font->font : FONT_UI; }
static BOOL WINAPI TextOutA(HDC h, int x, int y, const char *s, int n) {
    dc_t *d = DC(h); if (!d || !s) return 0;
    if (n < 0) n = strlen(s);
    rect_t sv; dc_begin(d, &sv);
    int f = dcfont(d), tw = text_width_n(f, s, n);
    if (d->align & 6) x -= (d->align & 6) == 6 ? tw / 2 : tw;
    if ((d->align & 24) == 24) y -= font_ascent(f);                 /* TA_BASELINE: y is the baseline */
    else if (d->align & 8) y -= font_height(f);                     /* TA_BOTTOM */
    if (d->bkmode == 2) fill_rect(d->surf, d->ox + x, d->oy + y, tw, font_height(f), d->bk);
    draw_text_n(d->surf, f, d->ox + x, d->oy + y, s, n, d->text);
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI ExtTextOutA(HDC h, int x, int y, UINT o, const LONG *r, const char *s, UINT n, const int *dx) {
    dc_t *d = DC(h); if (!d) return 0;
    if ((o & 4) && r) {                                             /* ETO_CLIPPED */
        rect_t save = d->clip;
        d->clip = rect_intersect(d->clip, R(d->ox + r[0], d->oy + r[1], r[2] - r[0], r[3] - r[1]));
        BOOL res = ExtTextOutA(h, x, y, o & ~4u, r, s, n, dx);
        d->clip = save;
        return res;
    }
    if (r && (o & 2)) { rect_t sv; dc_begin(d, &sv); fill_rect(d->surf, d->ox + r[0], d->oy + r[1], r[2] - r[0], r[3] - r[1], d->bk); dc_end(d, sv); }
    return s ? TextOutA(h, x, y, s, n) : 1;
}
typedef struct { LONG cx, cy; } SIZE32;
static BOOL WINAPI GetTextExtentPoint32A(HDC h, const char *s, int n, SIZE32 *sz) { dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI; sz->cx = text_width_n(f, s, n); sz->cy = font_height(f); return 1; }
typedef struct { LONG height, ascent, descent, intlead, extlead, avewidth, maxwidth, weight, overhang, dax, day; BYTE first, last, def, brk, it, ul, so, pitch, cs; } TEXTMETRICA;
static BOOL WINAPI GetTextMetricsA(HDC h, TEXTMETRICA *tm) {
    dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI;
    memset(tm, 0, sizeof *tm); tm->height = font_height(f); tm->ascent = font_ascent(f); tm->descent = tm->height - tm->ascent;
    tm->avewidth = text_width(f, "x"); tm->maxwidth = text_width(f, "W"); tm->weight = 400; tm->first = 32; tm->last = 126; tm->def = '?'; tm->dax = tm->day = 96;
    return 1;
}
static void fill_brush(dc_t *d, int x, int y, int w, int h, gdiobj_t *b) { if (b && !b->null) fill_rect(d->surf, d->ox + x, d->oy + y, w, h, b->color); }
static void pen_line(dc_t *d, int x0, int y0, int x1, int y1) {
    if (!d->pen || d->pen->null) return;
    draw_thick_line(d->surf, d->ox + x0, d->oy + y0, d->ox + x1, d->oy + y1, d->pen->width, d->pen->color);
}
static BOOL WINAPI MoveToEx(HDC h, int x, int y, LONG *old) { dc_t *d = DC(h); if (!d) return 0; if (old) { old[0] = d->cx; old[1] = d->cy; } d->cx = x; d->cy = y; return 1; }
static BOOL WINAPI LineTo(HDC h, int x, int y) { dc_t *d = DC(h); if (!d) return 0; rect_t sv; dc_begin(d, &sv); pen_line(d, d->cx, d->cy, x, y); dc_end(d, sv); d->cx = x; d->cy = y; return 1; }
static BOOL WINAPI Polyline(HDC h, const LONG *pts, int n) { dc_t *d = DC(h); if (!d) return 0; rect_t sv; dc_begin(d, &sv); for (int i = 1; i < n; i++) pen_line(d, pts[i * 2 - 2], pts[i * 2 - 1], pts[i * 2], pts[i * 2 + 1]); dc_end(d, sv); return 1; }
static BOOL WINAPI Polygon(HDC h, const LONG *pts, int n) {
    dc_t *d = DC(h); if (!d || n < 3) return 0;
    rect_t sv; dc_begin(d, &sv);
    if (d->brush && !d->brush->null) {
        int miny = pts[1], maxy = pts[1];
        for (int i = 1; i < n; i++) { miny = MIN(miny, pts[i * 2 + 1]); maxy = MAX(maxy, pts[i * 2 + 1]); }
        for (int y = miny; y <= maxy; y++) {
            int xs[64], nx = 0;
            for (int i = 0; i < n && nx < 64; i++) {
                int x0 = pts[i * 2], y0 = pts[i * 2 + 1], x1 = pts[((i + 1) % n) * 2], y1 = pts[((i + 1) % n) * 2 + 1];
                if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) xs[nx++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            }
            for (int a = 1; a < nx; a++) for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
            for (int k = 0; k + 1 < nx; k += 2) hline(d->surf, d->ox + xs[k], d->oy + y, xs[k + 1] - xs[k], d->brush->color);
        }
    }
    for (int i = 0; i < n; i++) pen_line(d, pts[i * 2], pts[i * 2 + 1], pts[((i + 1) % n) * 2], pts[((i + 1) % n) * 2 + 1]);
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI Rectangle(HDC h, int l, int t, int r, int b) {
    dc_t *d = DC(h); if (!d) return 0;
    rect_t sv; dc_begin(d, &sv);
    fill_brush(d, l, t, r - l, b - t, d->brush);
    if (d->pen && !d->pen->null) for (int i = 0; i < d->pen->width; i++) draw_rect(d->surf, d->ox + l + i, d->oy + t + i, r - l - 2 * i, b - t - 2 * i, d->pen->color);
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI RoundRect(HDC h, int l, int t, int r, int b, int ew, int eh) {
    dc_t *d = DC(h); if (!d) return 0;
    rect_t sv; dc_begin(d, &sv);
    if (d->brush && !d->brush->null) fill_round_rect(d->surf, d->ox + l, d->oy + t, r - l, b - t, ew / 2, d->brush->color);
    if (d->pen && !d->pen->null) draw_round_rect(d->surf, d->ox + l, d->oy + t, r - l, b - t, ew / 2, d->pen->color);
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI Ellipse(HDC h, int l, int t, int r, int b) {
    dc_t *d = DC(h); if (!d) return 0;
    rect_t sv; dc_begin(d, &sv);
    int cx2 = l + r, cy2 = t + b, a = (r - l) / 2, bb = (b - t) / 2;
    if (a <= 0 || bb <= 0) { dc_end(d, sv); return 1; }
    int64_t a2 = (int64_t)a * a, b2 = (int64_t)bb * bb;
    for (int y = -bb; y <= bb; y++) {
        int xw = 0; while ((int64_t)(xw + 1) * (xw + 1) * b2 + (int64_t)y * y * a2 <= a2 * b2) xw++;
        int yy = d->oy + cy2 / 2 + y;
        if (d->brush && !d->brush->null) hline(d->surf, d->ox + cx2 / 2 - xw, yy, 2 * xw + 1, d->brush->color);
        if (d->pen && !d->pen->null) { put_pixel(d->surf, d->ox + cx2 / 2 - xw, yy, d->pen->color); put_pixel(d->surf, d->ox + cx2 / 2 + xw, yy, d->pen->color); }
    }
    if (d->pen && !d->pen->null) for (int x = -a; x <= a; x++) {
        int yh = 0; while ((int64_t)(yh + 1) * (yh + 1) * a2 + (int64_t)x * x * b2 <= a2 * b2) yh++;
        put_pixel(d->surf, d->ox + cx2 / 2 + x, d->oy + cy2 / 2 - yh, d->pen->color); put_pixel(d->surf, d->ox + cx2 / 2 + x, d->oy + cy2 / 2 + yh, d->pen->color);
    }
    dc_end(d, sv);
    return 1;
}
static COLORREF WINAPI SetPixel(HDC h, int x, int y, COLORREF c) { dc_t *d = DC(h); if (!d) return 0; rect_t sv; dc_begin(d, &sv); put_pixel(d->surf, d->ox + x, d->oy + y, cref(c)); dc_end(d, sv); return c; }
static BOOL WINAPI SetPixelV(HDC h, int x, int y, COLORREF c) { SetPixel(h, x, y, c); return 1; }
static COLORREF WINAPI GetPixel(HDC h, int x, int y) { dc_t *d = DC(h); if (!d) return 0xFFFFFFFF; uint32_t p = get_pixel(d->surf, d->ox + x, d->oy + y); return C_R(p) | (C_G(p) << 8) | (C_B(p) << 16); }
static BOOL WINAPI BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
    dc_t *d = DC(dst), *s = DC(src); if (!d) return 0;
    if (s != d) dc_src(s);
    rect_t sv; dc_begin(d, &sv);
    if (rop == 0x00F00021 || !s) fill_brush(d, x, y, w, h, d->brush);       /* PATCOPY */
    else if (rop == 0x00000042) fill_rect(d->surf, d->ox + x, d->oy + y, w, h, BLACK);
    else if (rop == 0x00FF0062) fill_rect(d->surf, d->ox + x, d->oy + y, w, h, WHITE);
    else blit(d->surf, d->ox + x, d->oy + y, s->surf, s->ox + sx, s->oy + sy, w, h);
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop) {
    dc_t *d = DC(dst), *s = DC(src); if (!d || !s || !w || !h) return 0;
    if (s != d) dc_src(s);
    rect_t sv; dc_begin(d, &sv);
    /* negative sizes mirror; only the part inside the clip is visited, one row at a time */
    if (w < 0) { x += w; w = -w; sx += sw; sw = -sw; }
    if (h < 0) { y += h; h = -h; sy += sh; sh = -sh; }
    canvas_t *dc = d->surf, *sc = s->surf;
    rect_t cl = dc->clip;
    int x0 = MAX(d->ox + x, cl.x), x1 = MIN(d->ox + x + w, cl.x + cl.w), y0 = MAX(d->oy + y, cl.y), y1 = MIN(d->oy + y + h, cl.y + cl.h);
    /* the source column of each destination column, worked out once (no division per pixel) */
    int cols = x1 - x0;
    int *map = cols > 0 ? kmalloc(sizeof(int) * cols) : NULL;
    if (map) for (int px = x0; px < x1; px++) { int i = px - (d->ox + x); map[px - x0] = s->ox + sx + (int)((int64_t)i * sw / w) - (sw < 0 ? 1 : 0); }
    for (int py = y0; map && py < y1; py++) {
        int j = py - (d->oy + y);
        int syy = s->oy + sy + (int)((int64_t)j * sh / h) - (sh < 0 ? 1 : 0);
        if (syy < 0 || syy >= sc->h) continue;
        const uint32_t *srow = sc->px + (size_t)syy * sc->stride;
        uint32_t *drow = dc->px + (size_t)py * dc->stride;
        for (int px = x0; px < x1; px++) { int sxx = map[px - x0]; if ((unsigned)sxx < (unsigned)sc->w) drow[px] = srow[sxx]; }
    }
    kfree(map);
    dc_end(d, sv);
    return 1;
}
static BOOL WINAPI PatBlt(HDC h, int x, int y, int w, int ht, DWORD rop) { return BitBlt(h, x, y, w, ht, 0, 0, 0, 0x00F00021); }
static int WINAPI GetDeviceCaps(HDC h, int i) { switch (i) { case 8: return SCREEN_W; case 10: return SCREEN_H; case 12: return 32; case 14: return 1; case 88: case 90: return 96; } return 0; }

/* ---------------- rects ---------------- */
typedef struct { LONG l, t, r, b; } RECT32;
static BOOL WINAPI SetRect(RECT32 *r, int l, int t, int ri, int b) { r->l = l; r->t = t; r->r = ri; r->b = b; return 1; }
static BOOL WINAPI SetRectEmpty(RECT32 *r) { memset(r, 0, sizeof *r); return 1; }
static BOOL WINAPI CopyRect(RECT32 *d, const RECT32 *s) { *d = *s; return 1; }
static BOOL WINAPI OffsetRect(RECT32 *r, int dx, int dy) { r->l += dx; r->r += dx; r->t += dy; r->b += dy; return 1; }
static BOOL WINAPI InflateRect(RECT32 *r, int dx, int dy) { r->l -= dx; r->r += dx; r->t -= dy; r->b += dy; return 1; }
static BOOL WINAPI IsRectEmpty(const RECT32 *r) { return r->r <= r->l || r->b <= r->t; }
static BOOL WINAPI PtInRect(const RECT32 *r, LONG x, LONG y) { return x >= r->l && x < r->r && y >= r->t && y < r->b; }
static BOOL WINAPI EqualRect(const RECT32 *a, const RECT32 *b) { return !memcmp(a, b, sizeof *a); }
static BOOL WINAPI IntersectRect(RECT32 *d, const RECT32 *a, const RECT32 *b) { d->l = MAX(a->l, b->l); d->t = MAX(a->t, b->t); d->r = MIN(a->r, b->r); d->b = MIN(a->b, b->b); if (IsRectEmpty(d)) { SetRectEmpty(d); return 0; } return 1; }
static BOOL WINAPI UnionRect(RECT32 *d, const RECT32 *a, const RECT32 *b) { d->l = MIN(a->l, b->l); d->t = MIN(a->t, b->t); d->r = MAX(a->r, b->r); d->b = MAX(a->b, b->b); return 1; }
static int WINAPI FillRect(HDC h, const RECT32 *r, HGDIOBJ br) { dc_t *d = DC(h); gdiobj_t *b = OBJ(br); if (!d || !b) return 0; rect_t sv; dc_begin(d, &sv); fill_brush(d, r->l, r->t, r->r - r->l, r->b - r->t, b); dc_end(d, sv); return 1; }
static int WINAPI FrameRect(HDC h, const RECT32 *r, HGDIOBJ br) { dc_t *d = DC(h); gdiobj_t *b = OBJ(br); if (!d || !b) return 0; rect_t sv; dc_begin(d, &sv); draw_rect(d->surf, d->ox + r->l, d->oy + r->t, r->r - r->l, r->b - r->t, b->color); dc_end(d, sv); return 1; }
static BOOL WINAPI InvertRect(HDC h, const RECT32 *r) {
    dc_t *d = DC(h); if (!d) return 0; rect_t sv; dc_begin(d, &sv);
    for (int y = r->t; y < r->b; y++) for (int x = r->l; x < r->r; x++) { uint32_t p = get_pixel(d->surf, d->ox + x, d->oy + y); put_pixel(d->surf, d->ox + x, d->oy + y, (p ^ 0xFFFFFF) | 0xFF000000); }
    dc_end(d, sv); return 1;
}
static BOOL WINAPI DrawFocusRect(HDC h, const RECT32 *r) { dc_t *d = DC(h); if (!d) return 0; rect_t sv; dc_begin(d, &sv); dotted_rect(d->surf, d->ox + r->l, d->oy + r->t, r->r - r->l, r->b - r->t, BLACK); dc_end(d, sv); return 1; }
static BOOL WINAPI DrawEdge(HDC h, RECT32 *r, UINT e, UINT f) { dc_t *d = DC(h); if (!d) return 0; rect_t sv; dc_begin(d, &sv); bevel_deep(d->surf, d->ox + r->l, d->oy + r->t, r->r - r->l, r->b - r->t, (e & 5) != 0); dc_end(d, sv); return 1; }
#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_CALCRECT 0x400
static int WINAPI DrawTextA(HDC h, const char *s, int n, RECT32 *r, UINT fmt) {
    dc_t *d = DC(h); if (!d || !s) return 0;
    char buf[2048]; if (n < 0) n = strlen(s); n = MIN(n, 2047); memcpy(buf, s, n); buf[n] = 0;
    for (char *p = buf; *p; p++) if (*p == '&' && p[1] != '&') memmove(p, p + 1, strlen(p));
    int f = dcfont(d), fh = font_height(f), w = r->r - r->l;
    int th = (fmt & DT_WORDBREAK) && !(fmt & DT_SINGLELINE) ? text_wrapped_height(f, MAX(10, w), buf, 0) : fh;
    if (fmt & DT_CALCRECT) { if (!(fmt & DT_WORDBREAK)) r->r = r->l + text_width(f, buf); r->b = r->t + th; return th; }
    rect_t sv; dc_begin(d, &sv);
    rect_t old = canvas_push_clip(d->surf, R(d->ox + r->l, d->oy + r->t, w, r->b - r->t));
    int y = r->t;
    if (fmt & DT_VCENTER) y = r->t + (r->b - r->t - th) / 2; else if (fmt & DT_BOTTOM) y = r->b - th;
    if ((fmt & DT_WORDBREAK) && !(fmt & DT_SINGLELINE)) {
        if (d->bkmode == 2) fill_rect(d->surf, d->ox + r->l, d->oy + y, w, th, d->bk);
        draw_text_wrapped(d->surf, f, d->ox + r->l, d->oy + y, w, buf, d->text, 0);
    } else {
        int tw = text_width(f, buf), x = r->l;
        if (fmt & DT_CENTER) x = r->l + (w - tw) / 2; else if (fmt & DT_RIGHT) x = r->r - tw;
        if (d->bkmode == 2) fill_rect(d->surf, d->ox + x, d->oy + y, tw, fh, d->bk);
        draw_text(d->surf, f, d->ox + x, d->oy + y, buf, d->text);
    }
    d->surf->clip = old;
    dc_end(d, sv);
    return th;
}

/* ---------------- controls ---------------- */
static void draw_list_ext(w32win_t *c, canvas_t *s, rect_t r, int f);
static void draw_ext(w32win_t *c, canvas_t *s, rect_t r, int f);
static void draw_control(w32win_t *c) {
    w32win_t *t = top_of(c);
    if (!t || !t->top || !c->visible) return;
    canvas_t *s = t->top->surf;
    int ox, oy; abs_origin(c, &ox, &oy);
    rect_t old = canvas_push_clip(s, R(ox, oy, c->w, c->h));
    rect_t r = R(ox, oy, c->w, c->h);
    int f = c->font ? OBJ(c->font) ? OBJ(c->font)->font : FONT_UI : FONT_UI;
    char label[512]; strlcpy(label, c->text, sizeof label);
    for (char *p = label; *p; p++) if (*p == '&') memmove(p, p + 1, strlen(p));
    if (c->ctl == CTL_BUTTON) {
        int bs = c->style & 0xF;
        if (bs == 7) { draw_group(s, r, label); }
        else if (bs == 2 || bs == 3 || bs == 5 || bs == 6) { fill_rect(s, r.x, r.y, r.w, r.h, T.face); draw_check(s, r.x, r.y + (r.h - 13) / 2, c->checked, c->hot); draw_text(s, f, r.x + 18, r.y + (r.h - font_height(f)) / 2, label, c->enabled ? T.text : T.gray_text); }
        else if (bs == 4 || bs == 9) { fill_rect(s, r.x, r.y, r.w, r.h, T.face); draw_radio(s, r.x, r.y + (r.h - 13) / 2, c->checked, c->hot); draw_text(s, f, r.x + 18, r.y + (r.h - font_height(f)) / 2, label, c->enabled ? T.text : T.gray_text); }
        else draw_button(s, r, label, c->hot, c->pressed && c->hot, focus_win == c || bs == 1, c->enabled);
    } else if (c->ctl >= CTL_TOOLBAR) {
        cc_draw(c, s, r, f);
    } else if (c->ctl == CTL_COMBO || c->ctl == CTL_TREE || c->ctl == CTL_SCROLLBAR) {
        draw_ext(c, s, r, f);
    } else if (c->ctl == CTL_STATIC) {
        int ss = c->style & 0x1F;
        if (ss == 0x0E || ss == 0x0F) {}
        else if (ss == 0x10) hline(s, r.x, r.y, r.w, T.face_dark), hline(s, r.x, r.y + 1, r.w, T.face_light);      /* SS_ETCHEDHORZ */
        else if (ss == 0x11) vline(s, r.x, r.y, r.h, T.face_dark), vline(s, r.x + 1, r.y, r.h, T.face_light);      /* SS_ETCHEDVERT */
        else if (ss == 0x12 || ss == 0x07 || ss == 0x08 || ss == 0x09) bevel_deep(s, r.x, r.y, r.w, r.h, true);   /* frames */
        else {
            int x = r.x, tw = text_width(f, label);
            if (ss == 1) x = r.x + (r.w - tw) / 2; else if (ss == 2) x = r.x + r.w - tw;
            draw_text_wrapped(s, f, x, r.y, r.w, label, T.text, 0);
        }
    } else if (c->ctl == CTL_EDIT) {
        draw_sunken_box(s, r, c->enabled ? T.window_bg : T.face);
        rect_t o2 = canvas_push_clip(s, R(r.x + 2, r.y + 2, r.w - 4, r.h - 4));
        char shown[512]; strlcpy(shown, label, sizeof shown);
        if (c->style & 0x20) for (char *p = shown; *p; p++) *p = '*';
        if (c->style & 4) draw_text_wrapped(s, f, r.x + 4, r.y + 3, r.w - 8, shown, c->enabled ? T.text : T.gray_text, 0);
        else {
            int tw = text_width(f, shown), off = tw > r.w - 10 ? tw - (r.w - 10) : 0;
            draw_text(s, f, r.x + 4 - off, r.y + (r.h - font_height(f)) / 2, shown, c->enabled ? T.text : T.gray_text);
            if (focus_win == c) vline(s, r.x + 4 - off + tw, r.y + 3, r.h - 6, T.text);
        }
        s->clip = o2;
    } else if (c->ctl == CTL_LISTBOX) {
        draw_sunken_box(s, r, T.window_bg);
        int ih = font_height(FONT_UI) + 2;
        (void)ih; draw_list_ext(c, s, r, f);
    }
    s->clip = old;
    wm_need_redraw = true;
}
static void draw_children(w32win_t *w) {
    for (w32win_t *c = w->child; c; c = c->next) {
        if (!c->visible || c->destroyed) continue;
        if (c->ctl) draw_control(c); else c->need_paint = true;
        draw_children(c);
    }
}
void w32_theme_changed(void) {
    /* Repaint on each application's own message loop; do not call PE window
     * procedures on the shell's stack. Explicit GDI drawing colours stay put. */
    for (w32win_t *w = all_wins; w; w = w->all_next)
        if (!w->destroyed && !w->parent) { w->need_paint = true; w->erase = true; }
}
static LRESULT WINAPI ctl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static bool ctl_mouse_ext(w32win_t *c, window_t *win, int type, int lx, int ly, int btn);
static bool ctl_key_ext(w32win_t *c, key_event_t *e);
static bool ctl_msg_ext(w32win_t *c, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res);
static void radio_group_select(w32win_t *b);

/* ---------------- classes ---------------- */
typedef struct { UINT style; void *proc; int cls_extra, wnd_extra; HINSTANCE inst; HANDLE icon, cursor; HGDIOBJ bg; const char *menu, *name; } WNDCLASSA;
typedef struct { UINT size, style; void *proc; int cls_extra, wnd_extra; HINSTANCE inst; HANDLE icon, cursor; HGDIOBJ bg; const char *menu, *name; HANDLE sm; } WNDCLASSEXA;
static w32cls_t *find_class(const char *n) {
    if ((uintptr_t)n < 0x10000) { for (w32cls_t *c = classes; c; c = c->next) if (0xC000 + ((uintptr_t)c & 0x3FFF) == (uintptr_t)n) return c; return NULL; }   /* atom */
    for (w32cls_t *c = classes; c; c = c->next) if (!strcasecmp(c->name, n) && (!c->owner || c->owner == P)) return c;
    return NULL;
}
static ATOM reg_class_ex(const char *name, void *proc, uintptr_t bg, uint32_t style, int wnd_extra, bool unicode) {
    if (find_class(name)) { P->last_error = 1410; return 0; }
    w32cls_t *c = kzalloc(sizeof *c);
    strlcpy(c->name, name, sizeof c->name); c->proc = proc; c->brush = bg; c->style = style; c->owner = P;
    c->wnd_extra = CLAMP(wnd_extra, 0, 1024); c->unicode = unicode;
    c->next = classes; classes = c;
    return 0xC000 + ((uintptr_t)c & 0x3FFF);
}
static ATOM reg_class(const char *name, void *proc, uintptr_t bg, uint32_t style) { return reg_class_ex(name, proc, bg, style, 0, false); }
static w32cls_t *find_class(const char *n);
/* remember a class's menu: an ordinal or a name (ANSI or wide) */
static void class_menu(const char *cls, HINSTANCE inst, const void *menu, bool wide) {
    w32cls_t *c = find_class(cls); if (!c || !menu) return;
    c->inst = inst;
    if ((uintptr_t)menu < 0x10000) c->menu_id = (uintptr_t)menu;
    else if (wide) { const WCHAR *w = menu; int i = 0; for (; w[i] && i < 63; i++) c->menu_name[i] = (char)w[i]; c->menu_name[i] = 0; }
    else strlcpy(c->menu_name, menu, sizeof c->menu_name);
}
static ATOM WINAPI RegisterClassA(const WNDCLASSA *wc) { ATOM a = reg_class_ex(wc->name, wc->proc, wc->bg, wc->style, wc->wnd_extra, false); if (a && (uintptr_t)wc->name >= 0x10000) class_menu(wc->name, wc->inst, wc->menu, false); return a; }
static ATOM WINAPI RegisterClassExA(const WNDCLASSEXA *wc) { ATOM a = reg_class_ex(wc->name, wc->proc, wc->bg, wc->style, wc->wnd_extra, false); if (a && (uintptr_t)wc->name >= 0x10000) class_menu(wc->name, wc->inst, wc->menu, false); return a; }
static BOOL WINAPI UnregisterClassA(const char *n, HINSTANCE h) { return 1; }
static BOOL WINAPI GetClassInfoA(HINSTANCE h, const char *n, WNDCLASSA *wc) { w32cls_t *c = find_class(n); if (!c) return 0; memset(wc, 0, sizeof *wc); wc->proc = c->proc; wc->name = c->name; wc->bg = c->brush; return 1; }

/* ---------------- WM integration (runs on the GUI thread) ---------------- */
static UINT vk_scan(UINT vk);   /* user32_more.inc */
static int vk_of(key_event_t *e) {
    int k = e->key;
    if (k >= 'a' && k <= 'z') return k - 32;
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ') return k;
    if (k > ' ' && k < 127 && !e->ch) { key_event_t t = *e; t.ch = (char)k; t.key = 0; return vk_of(&t); }
    switch (k) {
    case KEY_ENTER: return 0x0D; case KEY_ESC: return 0x1B; case KEY_BACKSPACE: return 8; case KEY_TAB: return 9;
    case KEY_LEFT: return 0x25; case KEY_UP: return 0x26; case KEY_RIGHT: return 0x27; case KEY_DOWN: return 0x28;
    case KEY_LSHIFT: case KEY_RSHIFT: return 0x10; case KEY_LCTRL: return 0x11; case KEY_LALT: return 0x12;
    case KEY_DELETE: return 0x2E; case KEY_HOME: return 0x24; case KEY_END: return 0x23; case KEY_PGUP: return 0x21; case KEY_PGDN: return 0x22; case KEY_INSERT: return 0x2D;
    }
    if (k >= KEY_F1 && k <= KEY_F12) return 0x70 + (k - KEY_F1);
    if (e->ch) {
        /* punctuation: the US layout's VK_OEM_* keys (their ASCII values are other keys' codes,
         * '.' would be VK_DELETE); shifted characters map to their unshifted key */
        static const char oem[] = ";=,-./`[\\]'", oem_sh[] = ":+<_>?~{|}\"", dig_sh[] = ")!@#$%^&*(";
        static const uint8_t oem_vk[] = { 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE };
        for (int i = 0; oem[i]; i++) if (e->ch == oem[i] || e->ch == oem_sh[i]) return oem_vk[i];
        for (int i = 0; dig_sh[i]; i++) if (e->ch == dig_sh[i]) return '0' + i;
        return (uint8_t)toupper(e->ch);
    }
    return 0;
}
static w32win_t *child_at(w32win_t *w, int x, int y) {
    for (w32win_t *c = w->child; c; c = c->next) {
        if (!c->visible || c->destroyed) continue;
        if (x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h) {
            if (c->ctl == CTL_STATIC || (c->ctl == CTL_BUTTON && (c->style & 0xF) == 7)) continue;
            w32win_t *d = child_at(c, x - c->x, y - c->y);
            return d ? d : c;
        }
    }
    return NULL;
}
static w32win_t *hot_ctl;
static void wm_mouse_cb(window_t *win, int type, int x, int y, int btn) {
    w32win_t *t = win->w32; if (!t) return;
    w32win_t *target = child_at(t, x, y);
    if (!target) target = t;
    int ox, oy; abs_origin(target, &ox, &oy);
    int lx = x - ox, ly = y - oy;
    uint32_t lp = (lx & 0xFFFF) | ((ly & 0xFFFF) << 16);
    /* the button state, as GetAsyncKeyState and DirectInput see it (VK_LBUTTON, VK_RBUTTON, VK_MBUTTON) */
    if (type == ME_DOWN) { if (btn & MB_LEFT) key_set(1, true); if (btn & MB_RIGHT) key_set(2, true); if (btn & MB_MIDDLE) key_set(4, true); }
    if (type == ME_UP) { if (!btn || (btn & MB_LEFT)) key_set(1, false); if (!btn || (btn & MB_RIGHT)) key_set(2, false); if (!btn || (btn & MB_MIDDLE)) key_set(4, false); }
    if (type == ME_WHEEL) { extern int w32_wheel_acc; w32_wheel_acc += -btn * 120; }
    uint32_t mk = (win->ui.down ? 1 : 0) | (keystate[0x10] ? 4 : 0) | (keystate[0x11] ? 8 : 0);
    /* built-in control behaviour */
    if (hot_ctl && hot_ctl != target && W((uintptr_t)hot_ctl)) { hot_ctl->hot = false; if (type != ME_UP) hot_ctl->pressed = false; draw_control(hot_ctl); }
    if (target->ctl == CTL_BUTTON && target->enabled) {
        if (!target->hot) { target->hot = true; draw_control(target); }
        hot_ctl = target;
        if (type == ME_DOWN && (btn & MB_LEFT)) { target->pressed = true; focus_win = target; draw_control(target); }
        if (type == ME_UP && target->pressed) {
            target->pressed = false;
            int bs = target->style & 0xF;
            if (bs == 3) target->checked = !target->checked;
            if (bs == 9) radio_group_select(target);
            if (bs == 6) target->checked = (target->checked + 1) % 3;                              /* BS_AUTO3STATE */
            draw_control(target);
            if (target->parent) w32_post(target->proc, (uintptr_t)target->parent, WM_COMMAND, target->id & 0xFFFF, (uintptr_t)target);
        }
        return;
    } else if (hot_ctl && type == ME_UP) { hot_ctl->pressed = false; draw_control(hot_ctl); }
    if (type == ME_LEAVE && hot_ctl) { hot_ctl->hot = false; hot_ctl->pressed = false; draw_control(hot_ctl); hot_ctl = NULL; return; }
    if (target->ctl == CTL_EDIT && type == ME_DOWN) { w32win_t *old = focus_win; focus_win = target; if (old && old->ctl) draw_control(old); draw_control(target); return; }
    if (target->ctl >= CTL_TOOLBAR && cc_mouse(target, type, lx, ly, btn)) return;
    if ((target->ctl == CTL_LISTBOX || target->ctl == CTL_COMBO || target->ctl == CTL_TREE) && ctl_mouse_ext(target, win, type, lx, ly, btn)) return;
    if (target->ctl) return;
    uint32_t msg = 0, wp = mk;
    switch (type) {
    case ME_MOVE: msg = WM_MOUSEMOVE; break;
    case ME_DOWN: msg = (btn & MB_RIGHT) ? WM_RBUTTONDOWN : WM_LBUTTONDOWN; wp |= (btn & MB_RIGHT) ? 2 : 1; focus_win = NULL; break;
    case ME_UP: msg = (btn & MB_RIGHT) ? WM_RBUTTONUP : WM_LBUTTONUP; break;
    case ME_DBLCLICK: if (target->cls && (target->cls->style & 8)) msg = WM_LBUTTONDBLCLK; break;
    case ME_WHEEL: msg = WM_MOUSEWHEEL; wp |= ((uint32_t)(int16_t)(-btn * 120)) << 16; lp = ((wm_mouse_x) & 0xFFFF) | (wm_mouse_y << 16); break;
    }
    if (msg) w32_post(target->proc, (uintptr_t)target, msg, wp, lp);
}
static void wm_key_cb(window_t *win, key_event_t *e) {
    w32win_t *t = win->w32; if (!t) return;
    int vk = vk_of(e);
    if (vk) key_set(vk, e->pressed);
    if (vk != 0x10 && vk != 0x11 && vk != 0x12) {          /* modifiers held during this key, even if their own events did not reach us */
        keystate[0x10] = keystate[0xA0] = (e->mods & MOD_SHIFT) != 0;
        keystate[0x11] = keystate[0xA2] = (e->mods & MOD_CTRL) != 0;
        keystate[0x12] = keystate[0xA4] = (e->mods & MOD_ALT) != 0;
    }
    /* the character this key makes, for TranslateMessage to turn into WM_CHAR */
    if (vk && e->pressed) key_char[vk & 0xFF] = e->ch == '\n' ? 13 : e->ch ? (uint8_t)e->ch : vk == 0x0D ? 13 : vk == 8 ? 8 : vk == 9 ? 9 : vk == 0x1B ? 27 : 0;
    /* Ctrl with a letter makes a control character (Ctrl+O: 0x0F), as on Windows; editors ignore those */
    if (vk >= 'A' && vk <= 'Z' && e->pressed && keystate[0x11] && !keystate[0x12]) key_char[vk] = vk - 'A' + 1;
    w32win_t *f = focus_win && top_of(focus_win) == t && W((uintptr_t)focus_win) ? focus_win : t;
    if (f->ctl >= CTL_TOOLBAR && cc_key(f, e)) return;
    if ((f->ctl == CTL_LISTBOX || f->ctl == CTL_COMBO || f->ctl == CTL_TREE) && ctl_key_ext(f, e)) return;
    if (f->ctl == CTL_EDIT) {
        if (!e->pressed) return;
        int l = strlen(f->text);
        if (e->key == KEY_BACKSPACE) { if (l) f->text[l - 1] = 0; }
        else if (f->style & 0x800) return;                                                     /* ES_READONLY */
        else if (e->key == KEY_ENTER && (f->style & 4)) { if (l < (int)sizeof f->text - 2) { f->text[l] = '\n'; f->text[l + 1] = 0; } }
        else if (e->key == KEY_ENTER || e->key == KEY_TAB) { if (f->parent) w32_post(f->proc, (uintptr_t)top_of(f), WM_KEYDOWN, vk, 0); return; }
        else if (e->ch >= 32 && e->ch < 127 && l < (int)sizeof f->text - 2 && (!f->limit || l < f->limit)) { f->text[l] = e->ch; f->text[l + 1] = 0; }
        else return;
        draw_control(f);
        w32_post(f->proc, (uintptr_t)f->parent, WM_COMMAND, (f->id & 0xFFFF) | (0x300 << 16), (uintptr_t)f);
        return;
    }
    if (f->ctl == CTL_BUTTON && e->pressed && (e->key == ' ' || e->key == KEY_ENTER)) { w32_post(f->proc, (uintptr_t)f->parent, WM_COMMAND, f->id & 0xFFFF, (uintptr_t)f); return; }
    if (f->ctl) f = t;
    /* lParam: repeat count 1, the PC scan code (bits 16-23) and the extended-key bit (24), which
     * SDL and other games identify keys by, and on key-up the previous-state and transition bits
     * (30, 31), which programs that translate keys themselves (terminals) look at */
    if (vk) {
        bool ext = (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E || vk == 0x5B || vk == 0x5C || vk == 0x5D || vk == 0x6F;
        uint32_t lp = 1 | (vk_scan(vk) << 16) | (ext ? 1u << 24 : 0) | (e->pressed ? 0 : 0xC0000000u);
        w32_post(f->proc, (uintptr_t)f, e->pressed ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    }
}
static bool wm_close_cb(window_t *win) { w32win_t *t = win->w32; if (!t) return true; w32_post(t->proc, (uintptr_t)t, WM_CLOSE, 0, 0); return false; }
static void wm_resized_cb(window_t *win) {
    w32win_t *t = win->w32; if (!t) return;
    t->w = win->surf->w; t->h = win->surf->h;
    t->need_paint = true; t->erase = true;
    w32_post(t->proc, (uintptr_t)t, WM_SIZE, 0, (t->w & 0xFFFF) | (t->h << 16));
}
static void wm_destroy_cb(window_t *win) { w32win_t *t = win->w32; if (t && W((uintptr_t)t)) t->top = NULL; }
static void wm_activate_cb(window_t *win, bool a) { w32win_t *t = win->w32; if (t) w32_post(t->proc, (uintptr_t)t, WM_ACTIVATE, a ? 1 : 0, 0); }
static void wm_menu_cb(window_t *win, int id) { w32win_t *t = win->w32; if (t && id >= 0) w32_post(t->proc, (uintptr_t)t, WM_COMMAND, id & 0xFFFF, 0); }

/* ---------------- menus ---------------- */
typedef struct { uint32_t flags, id; char text[64]; } mitem_t;
typedef struct { uint32_t magic; mitem_t items[24]; int n; } w32menu_t;
#define MENUMAGIC 0x4D454E55
static w32menu_t *MENU(uintptr_t h) { w32menu_t *m = (w32menu_t *)h; return (h > 0x100000 && !(h & 3) && m->magic == MENUMAGIC) ? m : NULL; }
static HANDLE WINAPI CreateMenu(void) { w32menu_t *m = w32_alloc(P, sizeof *m); memset(m, 0, sizeof *m); m->magic = MENUMAGIC; return (HANDLE)m; }
static HANDLE WINAPI CreatePopupMenu(void) { return CreateMenu(); }
static BOOL WINAPI AppendMenuA(HANDLE hm, UINT flags, uintptr_t id, const char *text) {
    w32menu_t *m = MENU(hm); if (!m || m->n >= 24) return 0;
    mitem_t *it = &m->items[m->n++]; it->flags = flags; it->id = id;
    if (flags & 0x800) strcpy(it->text, "-"); else if (text) { strlcpy(it->text, text, 64); for (char *p = it->text; *p; p++) { if (*p == '&') memmove(p, p + 1, strlen(p)); if (*p == '\t') { *p = '\t'; } } }
    return 1;
}
static BOOL WINAPI DestroyMenu(HANDLE h) { return 1; }
static BOOL WINAPI DrawMenuBar(HWND h) { wm_need_redraw = true; return 1; }
static void apply_menu(w32win_t *w) {
    w32menu_t *m = MENU(w->menu);
    if (!w->top) return;
    if (!m) { if (w->top->menus) { kfree(w->top->menus); w->top->menus = NULL; w->top->nmenus = 0; } return; }
    rect_t cr = wm_client_rect(w->top);
    menu_t *mm = kzalloc(sizeof(menu_t) * m->n);
    for (int i = 0; i < m->n; i++) {
        mm[i].title = strdup(m->items[i].text);
        w32menu_t *sub = (m->items[i].flags & 0x10) ? MENU(m->items[i].id) : NULL;
        if (sub) for (int k = 0; k < sub->n && k < 19; k++) { mm[i].items[k].label = strdup(sub->items[k].text); mm[i].items[k].id = sub->items[k].id; mm[i].items[k].flags = (sub->items[k].flags & 8) ? MI_CHECKED : (sub->items[k].flags & 3) ? MI_DISABLED : 0; }
        else { mm[i].items[0].label = strdup(m->items[i].text); mm[i].items[0].id = m->items[i].id; }
    }
    w->top->menus = mm; w->top->nmenus = m->n;
    w->top->menu_cmd = wm_menu_cb;
    wm_resize_client(w->top, cr.w, cr.h);
}
static BOOL WINAPI SetMenu(HWND h, HANDLE m) { w32win_t *w = W(h); if (!w) return 0; w->menu = m; apply_menu(w); return 1; }
static HANDLE WINAPI GetMenu(HWND h) { w32win_t *w = W(h); return w ? w->menu : 0; }
static HANDLE WINAPI GetSubMenu(HANDLE hm, int i) { w32menu_t *m = MENU(hm); return m && i < m->n ? m->items[i].id : 0; }
static DWORD WINAPI CheckMenuItem(HANDLE hm, UINT id, UINT f) { return 0; }
static BOOL WINAPI EnableMenuItem(HANDLE hm, UINT id, UINT f) { return 1; }

/* ---------------- window creation ---------------- */
typedef struct { void *params; HINSTANCE inst; HANDLE menu; HWND parent; int cy, cx, y, x; LONG style; const char *name, *cls; DWORD exstyle; } CREATESTRUCTA;
static HWND WINAPI CreateWindowExA(DWORD ex, const char *clsname, const char *title, DWORD style, int x, int y, int w, int h, HWND parent, HANDLE menu, HINSTANCE inst, void *param) {
    w32_proc_t *p = P;
    w32cls_t *cls = find_class(clsname);
    int ctl = CTL_NONE;
    if (!cls && (uintptr_t)clsname > 0x10000) {
        int ccl = cc_class(clsname);
        if (ccl >= 0) ctl = ccl;
        else if (!strcasecmp(clsname, "BUTTON")) ctl = CTL_BUTTON; else if (!strcasecmp(clsname, "STATIC")) ctl = CTL_STATIC;
        else if (!strcasecmp(clsname, "EDIT") || !strcasecmp(clsname, "RichEdit20A") || !strcasecmp(clsname, "RichEdit20W")) ctl = CTL_EDIT;
        else if (!strcasecmp(clsname, "LISTBOX")) ctl = CTL_LISTBOX;
        else if (!strcasecmp(clsname, "COMBOBOX") || !strcasecmp(clsname, "ComboBoxEx32")) ctl = CTL_COMBO;
        else if (!strcasecmp(clsname, "SysTreeView32")) ctl = CTL_TREE;
        else if (!strcasecmp(clsname, "SCROLLBAR") || !strncasecmp(clsname, "msctls_", 7)) ctl = CTL_SCROLLBAR;
        else { p->last_error = 1407; klog("w32: CreateWindow: unknown class %s", clsname); return 0; }
    } else if (!cls) { p->last_error = 1407; return 0; }
    w32win_t *pw = W(parent);
    if ((style & WS_CHILD) && !pw) { p->last_error = 1406; return 0; }
    w32win_t *win = kzalloc(sizeof(w32win_t));
    win->magic = W32MAGIC; win->proc = p; win->cls = cls; win->ctl = ctl;
    win->style = style; win->exstyle = ex;
    win->wndproc = cls ? cls->proc : (void *)ctl_proc;
    win->enabled = !(style & WS_DISABLED);
    win->unicode = cls && cls->unicode;
    win->nextra = cls ? cls->wnd_extra : 0;
    if (cls && !strcmp(cls->name, "#32770") && win->nextra < 64) win->nextra = 64;   /* the system dialog class: DLGWINDOWEXTRA */
    if (win->nextra) win->extra = kzalloc(win->nextra);
    win->lb_sel = -1;
    if (title) strlcpy(win->text, (uintptr_t)title > 0x10000 ? title : "", sizeof win->text);
    win->all_next = all_wins; all_wins = win;
    if ((style & WS_CHILD) && pw) {
        win->parent = pw; win->id = (uint32_t)menu;
        win->x = x == (int)CW_USEDEFAULT ? 0 : x; win->y = y == (int)CW_USEDEFAULT ? 0 : y;
        win->w = w == (int)CW_USEDEFAULT ? 100 : w; win->h = h == (int)CW_USEDEFAULT ? 24 : h;
        if (ctl == CTL_COMBO && (style & 3) != 1) { win->cb_droph = win->h; win->h = MIN(win->h, font_height(FONT_UI) + 8); }
        w32win_t **pp = &pw->child; while (*pp) pp = &(*pp)->next; *pp = win;
        win->visible = (style & WS_VISIBLE) != 0;
    } else if (ctl == CTL_TOOLTIP || ctl == CTL_QUIET) {          /* tooltips and the like: a window that is never shown */
        win->w = 1; win->h = 1; win->visible = false;
    } else {
        int cw = w == (int)CW_USEDEFAULT ? 480 : w - 2 * T.border, ch = h == (int)CW_USEDEFAULT ? 340 : h - T.title_h - T.border;
        if (!(style & WS_CAPTION) && (style & 0x80000000u)) { cw = w; ch = h; }
        cw = MAX(cw, 40); ch = MAX(ch, 20);
        int flags = 0;
        if (style & WS_THICKFRAME) flags |= WF_RESIZABLE;
        if (!(style & WS_MAXIMIZEBOX) || !(style & WS_THICKFRAME)) flags |= WF_NO_MAX;
        if (!(style & WS_MINIMIZEBOX)) flags |= WF_NO_MIN;
        if (!(style & WS_CAPTION) && (style & 0x80000000u)) flags |= WF_POPUP;
        if (!(style & WS_VISIBLE)) flags |= WF_HIDDEN;
        window_t *tw = wm_create(win->text, x == (int)CW_USEDEFAULT ? -1 : x, y == (int)CW_USEDEFAULT ? -1 : y, cw, ch, flags);
        if (!tw) { win->destroyed = true; return 0; }
        tw->w32 = win; tw->icon = ICON_EXE; tw->pid = p->pid;
        tw->paint = NULL; tw->mouse = wm_mouse_cb; tw->key = wm_key_cb; tw->close = wm_close_cb; tw->resized = wm_resized_cb; tw->destroy = wm_destroy_cb; tw->activate = wm_activate_cb;
        gfx_clear(tw->surf, T.window_bg);
        win->top = tw; win->w = tw->surf->w; win->h = tw->surf->h;
        win->visible = (style & WS_VISIBLE) != 0;
        win->need_paint = true; win->erase = true;
        if (!menu && cls && (cls->menu_id || cls->menu_name[0])) {      /* the class's menu */
            extern HANDLE WINAPI w32_load_menu(HINSTANCE, uintptr_t);
            menu = w32_load_menu(cls->inst, cls->menu_id ? cls->menu_id : (uintptr_t)cls->menu_name);
        }
        if (menu) { win->menu = menu; apply_menu(win); }
    }
    CREATESTRUCTA cs = { param, inst, menu, parent, h, w, y, x, style, title, clsname, ex };
    if (!send(win, WM_NCCREATE, 0, (uintptr_t)&cs)) { /* DefWindowProc returns TRUE; app may refuse */ }
    if ((int32_t)send(win, WM_CREATE, 0, (uintptr_t)&cs) == -1) { extern BOOL WINAPI DestroyWindow(HWND); DestroyWindow((HWND)win); return 0; }
    if (win->destroyed) return 0;
    /* a hidden top-level window gets its first WM_SIZE when it is first shown (programs set
     * their window data between CreateWindow and ShowWindow and expect nothing before) */
    if (win->parent || (style & WS_VISIBLE)) { win->sent_size = true; send(win, WM_SIZE, 0, (win->w & 0xFFFF) | (win->h << 16)); }
    if (ctl >= CTL_TOOLBAR) cc_created(win);
    if (win->parent && win->ctl) draw_control(win);
    else if (win->parent) win->need_paint = true;
    return (HWND)win;
}
static void destroy_rec(w32win_t *w) {
    if (w->destroyed) return;
    send(w, WM_DESTROY, 0, 0);
    for (w32win_t *c = w->child; c; c = c->next) destroy_rec(c);
    send(w, WM_NCDESTROY, 0, 0);
    w->destroyed = true;
    if (focus_win == w) focus_win = NULL;
    if (hot_ctl == w) hot_ctl = NULL;
    for (int i = 0; i < 64; i++) if (timers[i].used && timers[i].hwnd == (uintptr_t)w) timers[i].used = false;
}
BOOL WINAPI DestroyWindow(HWND h) {
    w32win_t *w = W(h); if (!w) return 0;
    window_t *top = w->top;
    destroy_rec(w);
    if (w->parent) {
        for (w32win_t **pp = &w->parent->child; *pp; pp = &(*pp)->next) if (*pp == w) { *pp = w->next; break; }
        w32win_t *t = top_of(w->parent); if (t) t->need_paint = true;
    }
    if (top) { top->w32 = NULL; w->top = NULL; wm_destroy(top); }
    return 1;
}
static BOOL WINAPI ShowWindow(HWND h, int cmd) {
    w32win_t *w = W(h); if (!w) return 0;
    bool was = w->visible;
    w->visible = cmd != 0;
    if (cmd && !w->sent_size) { w->sent_size = true; send(w, WM_SIZE, 0, (w->w & 0xFFFF) | (w->h << 16)); if (!W(h)) return 0; }
    if (w->top) {
        if (cmd == 0) { w->top->flags |= WF_HIDDEN; wm_need_redraw = true; }
        else { wm_show(w->top); if (cmd == 3) wm_maximize(w->top, true); else if (cmd == 2 || cmd == 6 || cmd == 7) wm_minimize(w->top); else if (cmd == 9 && w->top->state == WS_MAXIMIZED) wm_maximize(w->top, false); }
    } else { w32win_t *t = top_of(w); if (t) t->need_paint = true; if (w->ctl) draw_control(w); }
    return was;
}
static BOOL WINAPI UpdateWindow(HWND h) { w32win_t *w = W(h); if (!w) return 0; if (w->need_paint) send(w, WM_PAINT, 0, 0); return 1; }
static BOOL WINAPI InvalidateRect(HWND h, const RECT32 *r, BOOL erase) {
    w32_proc_t *p = P;
    if (!h) { for (w32win_t *w = all_wins; w; w = w->all_next) if (w->proc == p && !w->destroyed && !w->parent) { w->need_paint = true; w->erase |= erase; } return 1; }
    w32win_t *w = W(h); if (!w) return 0;
    if (w->ctl) { draw_control(w); return 1; }
    w->need_paint = true; w->erase |= erase;
    return 1;
}
static BOOL WINAPI ValidateRect(HWND h, const RECT32 *r) { w32win_t *w = W(h); if (w) w->need_paint = false; return 1; }
static BOOL WINAPI RedrawWindow(HWND h, const RECT32 *r, HANDLE rgn, UINT f) { InvalidateRect(h, r, (f & 4) != 0); if (f & 0x100) UpdateWindow(h); return 1; }
static BOOL WINAPI GetClientRect(HWND h, RECT32 *r) {
    w32win_t *w = W(h);
    if (!w) { if (h == 0x10010) { r->l = r->t = 0; r->r = SCREEN_W; r->b = SCREEN_H; return 1; } return 0; }
    r->l = r->t = 0; r->r = w->w; r->b = w->h; return 1;
}
static BOOL WINAPI GetWindowRect(HWND h, RECT32 *r) {
    w32win_t *w = W(h); if (!w) { if (h == 0x10010) return GetClientRect(h, r); return 0; }
    w32win_t *t = top_of(w);
    if (w->top) { r->l = w->top->x; r->t = w->top->y; r->r = w->top->x + w->top->w; r->b = w->top->y + w->top->h; return 1; }
    rect_t cr = t && t->top ? wm_client_rect(t->top) : R(0, 0, 0, 0);
    int ox, oy; abs_origin(w, &ox, &oy);
    r->l = cr.x + ox; r->t = cr.y + oy; r->r = r->l + w->w; r->b = r->t + w->h; return 1;
}
static BOOL WINAPI ClientToScreen(HWND h, LONG *pt) { w32win_t *w = W(h); if (!w) return 0; w32win_t *t = top_of(w); rect_t cr = t->top ? wm_client_rect(t->top) : R(0, 0, 0, 0); int ox, oy; abs_origin(w, &ox, &oy); pt[0] += cr.x + ox; pt[1] += cr.y + oy; return 1; }
static BOOL WINAPI ScreenToClient(HWND h, LONG *pt) { w32win_t *w = W(h); if (!w) return 0; w32win_t *t = top_of(w); rect_t cr = t->top ? wm_client_rect(t->top) : R(0, 0, 0, 0); int ox, oy; abs_origin(w, &ox, &oy); pt[0] -= cr.x + ox; pt[1] -= cr.y + oy; return 1; }
static BOOL WINAPI AdjustWindowRect(RECT32 *r, DWORD style, BOOL menu) { r->l -= T.border; r->r += T.border; r->t -= T.title_h + (menu ? T.menubar_h : 0); r->b += T.border; return 1; }
static BOOL WINAPI AdjustWindowRectEx(RECT32 *r, DWORD style, BOOL menu, DWORD ex) { return AdjustWindowRect(r, style, menu); }
static BOOL WINAPI MoveWindow(HWND h, int x, int y, int w, int ht, BOOL repaint) {
    w32win_t *win = W(h); if (!win) return 0;
    if (win->top) {
        win->top->x = x; win->top->y = y;
        wm_resize_client(win->top, MAX(40, w - 2 * T.border), MAX(20, ht - T.title_h - T.border - (win->top->nmenus ? T.menubar_h : 0)));
        win->w = win->top->surf->w; win->h = win->top->surf->h;
    } else { win->x = x; win->y = y; win->w = w; win->h = ht; w32win_t *t = top_of(win); if (t) { t->need_paint = true; t->erase = true; } }
    send(win, WM_SIZE, 0, (win->w & 0xFFFF) | (win->h << 16));
    return 1;
}
static BOOL WINAPI SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f) {
    w32win_t *w = W(h); if (!w) return 0;
    RECT32 r; GetWindowRect(h, &r);
    if (f & 2) { x = w->top ? w->top->x : w->x; y = w->top ? w->top->y : w->y; }   /* SWP_NOMOVE */
    if (f & 1) { cx = r.r - r.l; cy = r.b - r.t; }                                   /* SWP_NOSIZE */
    if (!(f & 3) || (f & 3) != 3) MoveWindow(h, x, y, cx, cy, 1);
    if (f & 0x40) ShowWindow(h, 1);
    if (f & 0x80) ShowWindow(h, 0);
    return 1;
}
static BOOL WINAPI SetWindowTextA(HWND h, const char *t) {
    w32win_t *w = W(h); if (!w) return 0;
    strlcpy(w->text, t ? t : "", sizeof w->text);
    if (w->top) wm_set_title(w->top, w->text);
    if (w->ctl) draw_control(w);
    return 1;
}
static int WINAPI GetWindowTextA(HWND h, char *b, int n) { w32win_t *w = W(h); if (!w || n <= 0) return 0; strlcpy(b, w->text, n); return strlen(b); }
static int WINAPI GetWindowTextLengthA(HWND h) { w32win_t *w = W(h); return w ? strlen(w->text) : 0; }
static BOOL WINAPI EnableWindow(HWND h, BOOL e) { w32win_t *w = W(h); if (!w) return 0; bool o = !w->enabled; w->enabled = e; if (w->ctl) draw_control(w); return o; }
static BOOL WINAPI IsWindowEnabled(HWND h) { w32win_t *w = W(h); return w && w->enabled; }
static BOOL WINAPI IsWindow(HWND h) { return W(h) != NULL; }
static BOOL WINAPI IsWindowVisible(HWND h) { w32win_t *w = W(h); return w && w->visible; }
static BOOL WINAPI IsIconic(HWND h) { w32win_t *w = W(h); return w && w->top && w->top->state == WS_MINIMIZED; }
static BOOL WINAPI IsZoomed(HWND h) { w32win_t *w = W(h); return w && w->top && w->top->state == WS_MAXIMIZED; }
static HWND WINAPI GetParent(HWND h) { w32win_t *w = W(h); return w && w->parent ? (HWND)w->parent : 0; }
static HWND WINAPI GetDlgItem(HWND h, int id) { w32win_t *w = W(h); if (!w) return 0; for (w32win_t *c = w->child; c; c = c->next) if (!c->destroyed && (int)c->id == id) return (HWND)c; return 0; }
static int WINAPI GetDlgCtrlID(HWND h) { w32win_t *w = W(h); return w ? w->id : 0; }
static BOOL WINAPI SetDlgItemTextA(HWND h, int id, const char *t) { return SetWindowTextA(GetDlgItem(h, id), t); }
static UINT WINAPI GetDlgItemTextA(HWND h, int id, char *b, int n) { return GetWindowTextA(GetDlgItem(h, id), b, n); }
static BOOL WINAPI SetDlgItemInt(HWND h, int id, UINT v, BOOL s) { char b[16]; snprintf(b, sizeof b, s ? "%d" : "%u", v); return SetDlgItemTextA(h, id, b); }
static UINT WINAPI GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL s) { char b[32] = ""; GetDlgItemTextA(h, id, b, 32); if (ok) *ok = isdigit(b[0]) || b[0] == '-'; return atoi(b); }
static HWND WINAPI SetFocus(HWND h) { w32win_t *w = W(h); HWND o = (HWND)focus_win; w32win_t *of = focus_win; focus_win = w; if (of && W((uintptr_t)of) && of->ctl) draw_control(of); if (w && w->ctl) draw_control(w); return o; }
static HWND WINAPI GetFocus(void) { return (HWND)focus_win; }
static HWND WINAPI GetActiveWindow(void) { window_t *a = wm_active; return a && a->w32 ? (HWND)a->w32 : 0; }
static HWND WINAPI GetForegroundWindow(void) { return GetActiveWindow(); }
static BOOL WINAPI SetForegroundWindow(HWND h) { w32win_t *w = W(h); if (w && w->top) wm_focus(w->top); return 1; }
static HWND WINAPI SetActiveWindow(HWND h) { SetForegroundWindow(h); return h; }
static BOOL WINAPI BringWindowToTop(HWND h) { return SetForegroundWindow(h); }
static HWND WINAPI GetDesktopWindow(void) { return 0x10010; }
static HWND WINAPI SetCapture(HWND h) { return 0; }
static BOOL WINAPI ReleaseCapture(void) { return 1; }
static HWND WINAPI GetCapture(void) { return 0; }
static HWND WINAPI FindWindowA(const char *c, const char *t) { for (w32win_t *w = all_wins; w; w = w->all_next) if (!w->destroyed && !w->parent && t && !strcmp(w->text, t)) return (HWND)w; return 0; }
static HWND WINAPI GetWindow(HWND h, UINT c) { w32win_t *w = W(h); if (!w) return 0; if (c == 5) return (HWND)w->child; if (c == 2) return (HWND)w->next; if (c == 4) return (HWND)w->parent; return 0; }
/* the ...LongPtr forms are separate exports on x64 (macros for the 32-bit ones on x86) */
static LONG_PTR WINAPI GetWindowLongPtrA(HWND h, int i) {
    w32win_t *w = W(h); if (!w) return 0;
    if (i >= 0) { LONG_PTR v = 0; if (w->extra && i + (int)sizeof(LONG_PTR) <= w->nextra) memcpy(&v, w->extra + i, sizeof v); else w->proc->last_error = 1413; return v; }
    switch (i) { case -4: return (LONG_PTR)w->wndproc; case -21: return w->userdata; case -16: return w->style; case -20: return w->exstyle; case -12: return w->id; case -6: return (LONG_PTR)w->proc->base; case -8: return (LONG_PTR)w->parent; }
    return 0;
}
static LONG_PTR WINAPI SetWindowLongPtrA(HWND h, int i, LONG_PTR v) {
    w32win_t *w = W(h); if (!w) return 0;
    if (i >= 0) { LONG_PTR o = 0; if (w->extra && i + (int)sizeof(LONG_PTR) <= w->nextra) { memcpy(&o, w->extra + i, sizeof o); memcpy(w->extra + i, &v, sizeof v); } else w->proc->last_error = 1413; return o; }
    LONG_PTR o = GetWindowLongPtrA(h, i);
    switch (i) { case -4: w->wndproc = (void *)v; break; case -21: w->userdata = v; break; case -16: w->style = v; break; case -20: w->exstyle = v; break; case -12: w->id = v; break; }
    return o;
}
static LONG WINAPI GetWindowLongA(HWND h, int i) {
    w32win_t *w = W(h);
    if (w && i >= 0) { LONG v = 0; if (w->extra && i + 4 <= w->nextra) memcpy(&v, w->extra + i, 4); return v; }
    return (LONG)GetWindowLongPtrA(h, i);
}
static LONG WINAPI SetWindowLongA(HWND h, int i, LONG v) {
    w32win_t *w = W(h);
    if (w && i >= 0) { LONG o = 0; if (w->extra && i + 4 <= w->nextra) { memcpy(&o, w->extra + i, 4); memcpy(w->extra + i, &v, 4); } return o; }
    return (LONG)SetWindowLongPtrA(h, i, i == -4 || i == -21 ? (LONG_PTR)(uint32_t)v : v);
}
static uintptr_t WINAPI GetClassLongPtrA(HWND h, int i) { w32win_t *w = W(h); if (!w || !w->cls) return 0; if (i == -10) return w->cls->brush; if (i == -24) return (uintptr_t)w->cls->proc; return 0; }
static uintptr_t WINAPI SetClassLongPtrA(HWND h, int i, LONG_PTR v) { w32win_t *w = W(h); if (!w || !w->cls) return 0; uintptr_t o = GetClassLongPtrA(h, i); if (i == -10) w->cls->brush = v; if (i == -24) w->cls->proc = (void *)v; return o; }
static DWORD WINAPI GetClassLongA(HWND h, int i) { return (DWORD)GetClassLongPtrA(h, i); }
static DWORD WINAPI SetClassLongA(HWND h, int i, LONG v) { return (DWORD)SetClassLongPtrA(h, i, v); }
static int WINAPI GetClassNameA(HWND h, char *b, int n) { w32win_t *w = W(h); if (!w) return 0; strlcpy(b, w->cls ? w->cls->name : w->ctl >= CTL_TOOLBAR ? cc_class_name(w->ctl) : w->ctl == CTL_BUTTON ? "Button" : w->ctl == CTL_EDIT ? "Edit" : w->ctl == CTL_LISTBOX ? "ListBox" : w->ctl == CTL_COMBO ? "ComboBox" : w->ctl == CTL_TREE ? "SysTreeView32" : w->ctl == CTL_SCROLLBAR ? "ScrollBar" : "Static", n); return strlen(b); }
static BOOL WINAPI EnumWindows(void *fn, LPARAM lp) { return 1; }
static BOOL WINAPI FlashWindow(HWND h, BOOL b) { return 1; }

/* ---------------- messages ---------------- */
void w32_post(w32_proc_t *p, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!p) return;
    int n = (p->qh + 1) % MSGQ;
    if (n == p->qt) return;
    /* coalesce mouse moves */
    if (msg == WM_MOUSEMOVE && p->qh != p->qt) { int last = (p->qh + MSGQ - 1) % MSGQ; if (p->q[last].message == WM_MOUSEMOVE && p->q[last].hwnd == hwnd) { p->q[last].lParam = lp; p->q[last].wParam = wp; return; } }
    MSG32 *m = &p->q[p->qh];
    m->hwnd = hwnd; m->message = msg; m->wParam = wp; m->lParam = lp; m->time = timer_ticks(); m->ptx = wm_mouse_x; m->pty = wm_mouse_y;
    p->qh = n;
}
static LRESULT send(w32win_t *w, UINT msg, WPARAM wp, LPARAM lp) {
    if (!w || !w->wndproc) return 0;
    return call_wndproc(w->wndproc, (uintptr_t)w, msg, wp, lp);
}
static LRESULT WINAPI SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (h == 0xFFFF) return 0;
    w32win_t *w = W(h); if (!w) return 0;
    return send(w, msg, wp, lp);
}
static BOOL WINAPI PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { w32_proc_t *p = P; w32win_t *w = W(h); if (h && !w) return 0; w32_post(w ? w->proc : p, h, msg, wp, lp); return 1; }
static BOOL WINAPI PostThreadMessageA(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) { w32_post(P, 0, msg, wp, lp); return 1; }
static void WINAPI PostQuitMessage(int code) { w32_proc_t *p = P; p->quit_posted = true; p->quit_code = code; }
static bool find_paint(w32_proc_t *p, MSG32 *m) {
    for (w32win_t *w = all_wins; w; w = w->all_next)
        if (w->proc == p && w->need_paint && !w->destroyed && !w->ctl && (top_of(w)->top)) {
            m->hwnd = (uintptr_t)w; m->message = WM_PAINT; m->wParam = m->lParam = 0; m->time = timer_ticks(); return true;
        }
    return false;
}
bool w32_pump_timers(w32_proc_t *p) {
    uint32_t now = timer_ticks(); bool any = false;
    for (int i = 0; i < 64; i++) {
        w32timer_t *t = &timers[i];
        if (!t->used || t->proc != p || (int32_t)(now - t->next) < 0) continue;
        t->next = now + t->ms;
        /* don't flood: only post if no identical timer message is pending */
        bool pending = false;
        for (int k = p->qt; k != p->qh; k = (k + 1) % MSGQ) if (p->q[k].message == WM_TIMER && p->q[k].wParam == t->id && p->q[k].hwnd == t->hwnd) pending = true;
        if (!pending) { w32_post(p, t->hwnd, WM_TIMER, t->id, (uintptr_t)t->fn); any = true; }
    }
    return any;
}
static bool get_msg(w32_proc_t *p, MSG32 *m, bool remove) {
    w32_pump_timers(p);
    if (p->qt != p->qh) { *m = p->q[p->qt]; if (remove) p->qt = (p->qt + 1) % MSGQ; return true; }
    if (p->quit_posted) { memset(m, 0, sizeof *m); m->message = WM_QUIT; m->wParam = p->quit_code; if (remove) p->quit_posted = false; return true; }
    if (find_paint(p, m)) return true;
    return false;
}
static BOOL WINAPI GetMessageA(MSG32 *m, HWND h, UINT lo, UINT hi) {
    w32_proc_t *p = P;
    for (;;) {
        if (get_msg(p, m, true)) return m->message != WM_QUIT;
        task_sleep(8);
    }
}
static BOOL WINAPI PeekMessageA(MSG32 *m, HWND h, UINT lo, UINT hi, UINT rm) {
    w32_proc_t *p = P;
    if (get_msg(p, m, rm & 1)) return 1;
    task_yield();
    return 0;
}
static BOOL WINAPI WaitMessage(void) { w32_proc_t *p = P; MSG32 m; while (!get_msg(p, &m, false)) task_sleep(8); return 1; }
/* WM_CHAR comes from TranslateMessage, as on Windows: a key press that IsDialogMessage or an
 * accelerator consumed makes no character */
static BOOL WINAPI TranslateMessage(const MSG32 *m) {
    if (!m || m->message != WM_KEYDOWN) return 0;
    uint8_t c = key_char[m->wParam & 0xFF];
    w32win_t *w = W(m->hwnd);
    if (!c || !w) return 0;
    w32_post(w->proc, m->hwnd, WM_CHAR, c, m->lParam);
    return 1;
}
static LRESULT WINAPI DispatchMessageA(const MSG32 *m) {
    if (m->message == WM_TIMER && m->lParam) { void (WINAPI *cb)(HWND, UINT, uintptr_t, DWORD) = (void *)m->lParam; cb(m->hwnd, WM_TIMER, m->wParam, timer_ticks()); return 0; }
    w32win_t *w = W(m->hwnd); if (!w) return 0;
    return send(w, m->message, m->wParam, m->lParam);
}
static BOOL WINAPI TranslateAcceleratorA(HWND h, HANDLE a, MSG32 *m) { return 0; }
static HANDLE WINAPI LoadAcceleratorsA(HINSTANCE h, const char *n) { return 0; }
static LRESULT WINAPI CallWindowProcA(void *fn, HWND h, UINT msg, WPARAM wp, LPARAM lp) { return call_wndproc(fn, h, msg, wp, lp); }
static DWORD WINAPI GetMessageTime(void) { return timer_ticks(); }
static DWORD WINAPI GetMessagePos(void) { return (wm_mouse_x & 0xFFFF) | (wm_mouse_y << 16); }
static BOOL WINAPI InSendMessage(void) { return 0; }
static DWORD WINAPI MsgWaitForMultipleObjects(DWORD n, HANDLE *h, BOOL all, DWORD ms, DWORD mask) { w32_proc_t *p = P; uint32_t end = timer_ticks() + ms; MSG32 m; while (!get_msg(p, &m, false)) { if (ms != 0xFFFFFFFF && (int32_t)(timer_ticks() - end) >= 0) return 0x102; task_sleep(5); } return n; }

typedef struct { HDC hdc; BOOL erase; RECT32 rc; BOOL restore, incupdate; BYTE res[32]; } PAINTSTRUCT;
static HDC WINAPI BeginPaint(HWND h, PAINTSTRUCT *ps) {
    w32win_t *w = W(h); if (!w) return 0;
    dc_t *d = newdc(w);
    memset(ps, 0, sizeof *ps);
    ps->hdc = (HDC)d; ps->rc.r = w->w; ps->rc.b = w->h;
    bool erase = w->erase;
    w->need_paint = false; w->erase = false;
    if (erase) ps->erase = !send(w, WM_ERASEBKGND, (uintptr_t)d, 0);
    return (HDC)d;
}
static BOOL WINAPI EndPaint(HWND h, const PAINTSTRUCT *ps) {
    w32win_t *w = W(h);
    ReleaseDC(h, ps->hdc);
    if (w) draw_children(w);
    wm_need_redraw = true;
    return 1;
}
static LRESULT default_proc(w32win_t *w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: DestroyWindow((HWND)w); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint((HWND)w, &ps); EndPaint((HWND)w, &ps); return 0; }
    case WM_ERASEBKGND: {
        dc_t *d = DC(wp); if (!d) return 0;
        gdiobj_t *b = w->cls ? OBJ(w->cls->brush) : NULL;
        uint32_t col = b ? b->color : T.face;
        if (b && b->null) return 0;
        rect_t sv; dc_begin(d, &sv); fill_rect(d->surf, d->ox, d->oy, w->w, w->h, col); dc_end(d, sv);
        return 1;
    }
    case WM_SETTEXT: SetWindowTextA((HWND)w, (const char *)lp); return 1;
    case WM_GETTEXT: return GetWindowTextA((HWND)w, (char *)lp, wp);
    case WM_GETTEXTLENGTH: return strlen(w->text);
    case WM_SETFONT: w->font = wp; if (w->ctl) draw_control(w); return 0;
    case WM_GETFONT: return w->font;
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == 0xF060) w32_post(w->proc, (uintptr_t)w, WM_CLOSE, 0, 0); return 0;
    }
    return 0;
}
static LRESULT WINAPI DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { w32win_t *w = W(h); return w ? default_proc(w, msg, wp, lp) : 0; }
#define LB_ADDSTRING 0x180
#define LB_GETCURSEL 0x188
#define LB_SETCURSEL 0x186
#define LB_GETCOUNT 0x18B
#define LB_RESETCONTENT 0x184
#define LB_GETTEXT 0x189
static LRESULT WINAPI ctl_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    w32win_t *w = W(h); if (!w) return 0;
    LRESULT xr;
    if (ctl_msg_ext(w, msg, wp, lp, &xr)) return xr;
    switch (msg) {
    case BM_GETCHECK: return w->checked;
    case BM_SETCHECK: w->checked = wp == 1; draw_control(w); return 0;
    case WM_PAINT: draw_control(w); w->need_paint = false; return 0;
    }
    return default_proc(w, msg, wp, lp);
}

/* ---------------- timers ---------------- */
static uintptr_t WINAPI SetTimer(HWND h, uintptr_t id, UINT ms, void *fn) {
    w32_proc_t *p = P;
    for (int i = 0; i < 64; i++) if (timers[i].used && timers[i].hwnd == h && timers[i].id == id && timers[i].proc == p) { timers[i].ms = MAX(ms, 10u); timers[i].next = timer_ticks() + timers[i].ms; timers[i].fn = fn; return id ? id : i + 1; }
    for (int i = 0; i < 64; i++) if (!timers[i].used) {
        timers[i].used = true; timers[i].hwnd = h; timers[i].id = h ? id : (uint32_t)i + 1; timers[i].ms = MAX(ms, 10u); timers[i].next = timer_ticks() + timers[i].ms; timers[i].proc = p; timers[i].fn = fn;
        return timers[i].id ? timers[i].id : 1;
    }
    return 0;
}
static BOOL WINAPI KillTimer(HWND h, uintptr_t id) { for (int i = 0; i < 64; i++) if (timers[i].used && timers[i].hwnd == h && timers[i].id == id) { timers[i].used = false; return 1; } return 0; }

/* ---------------- message box ---------------- */
typedef struct { volatile int result; } mbwait_t;
static void mb_done(void *ctx, int r) { ((mbwait_t *)ctx)->result = r; }
static int WINAPI MessageBoxA(HWND h, const char *text, const char *cap, UINT type) {
    int btn = type & 0xF, icon = type & 0xF0;
    if (cap && strstr(cap, "Runtime Library")) {                 /* a C runtime error (assertion): log where in the program it came from */
        uintptr_t *sp = __builtin_frame_address(0), base = P->base;
        char line[200] = ""; int k = 0, n = 0;
        for (int i = 0; i < 4096 && n < 16; i++) {
            
            uintptr_t v = sp[i];
            if (v >= base + 0x1000 && v < base + 0x200000 && (((uint8_t *)v)[-5] == 0xE8 || (((uint8_t *)v)[-6] == 0xFF && ((uint8_t *)v)[-5] == 0x15) || ((uint8_t *)v)[-2] == 0xFF || ((uint8_t *)v)[-3] == 0xFF)) { k += snprintf(line + k, sizeof line - k, " %lx", (unsigned long)sp[i]); n++; }
        }
        klog("w32: runtime error box; program addresses on the stack:%s", line);
    }
    int bt = btn == 1 ? MBX_OKCANCEL : btn == 3 ? MBX_YESNOCANCEL : btn == 4 ? MBX_YESNO : MBX_OK;
    int ic = icon == 0x10 ? MBI_ERROR : icon == 0x20 ? MBI_QUESTION : icon == 0x30 ? MBI_WARNING : icon == 0x40 ? MBI_INFO : MBI_NONE;
    mbwait_t *wt = kzalloc(sizeof *wt);
    w32win_t *ow = W(h);
    if (ow && top_of(ow)->top) wm_focus(top_of(ow)->top);
    window_t *mw = msgbox(cap ? cap : "Error", text ? text : "", ic, bt, mb_done, wt);
    if (mw) { mw->pid = P->pid; if (ow && top_of(ow)->top) { mw->owner = top_of(ow)->top; mw->flags |= WF_MODAL; } else { mw->owner = NULL; mw->flags &= ~WF_MODAL; } }
    w32_proc_t *p = P;
    while (!wt->result) {
        /* a modal loop, as on Windows: the program's timers, socket notifications and painting
         * go on behind the box; keyboard and mouse input for its windows is dropped */
        MSG32 m; bool any = false;
        while (!wt->result && get_msg(p, &m, true)) {
            any = true;
            if (m.message == WM_QUIT) { p->quit_posted = true; p->quit_code = m.wParam; continue; }
            if ((m.message >= 0x100 && m.message <= 0x109) || (m.message >= 0x200 && m.message <= 0x20E)) continue;
            DispatchMessageA(&m);
        }
        if (!any && find_paint(p, &m)) send(W(m.hwnd), WM_PAINT, 0, 0);
        task_sleep(15);
    }
    int r = wt->result; kfree(wt);
    return r;
}
static int WINAPI MessageBoxW(HWND h, const WCHAR *t, const WCHAR *c, UINT type) {
    char a[512], b[128]; int i;
    for (i = 0; t && t[i] && i < 511; i++) a[i] = t[i] < 128 ? t[i] : '?'; a[i] = 0;
    for (i = 0; c && c[i] && i < 127; i++) b[i] = c[i] < 128 ? c[i] : '?'; b[i] = 0;
    return MessageBoxA(h, a, b, type);
}
static int WINAPI MessageBoxExA(HWND h, const char *t, const char *c, UINT ty, WORD l) { return MessageBoxA(h, t, c, ty); }
static BOOL WINAPI MessageBeep(UINT t) { speaker_beep(t == 0x10 ? 440 : 880, 80); return 1; }

/* ---------------- misc user32 ---------------- */
static int WINAPI GetSystemMetrics(int i) {
    switch (i) {
    case 0: case 16: return SCREEN_W; case 1: return SCREEN_H; case 17: return SCREEN_H - T.taskbar_h - T.title_h;
    case 4: return T.title_h; case 5: case 6: case 32: case 33: return T.border; case 11: case 12: return 32; case 49: case 50: return 16;
    case 15: return T.menubar_h; case 2: case 3: case 20: case 21: return 17; case 43: return 3; case 80: return 1;
    }
    return 0;
}
static HANDLE WINAPI LoadCursorA(HINSTANCE h, const char *n) { return 0x20001; }
static HANDLE WINAPI LoadIconA(HINSTANCE h, const char *n) { return 0x20002; }
static HANDLE WINAPI LoadImageA(HINSTANCE h, const char *n, UINT t, int cx, int cy, UINT f) { return t == 0 ? 0 : 0x20003; }
/* A program hides the pointer over its windows with ShowCursor(FALSE) (a count below zero)
 * or SetCursor(NULL), as games do; over everything else it stays. Kept per process. */
static struct { w32_proc_t *proc; int count; bool none; } cursor_state[16];
static int cursor_slot(w32_proc_t *p) {
    int free = -1;
    for (int i = 0; i < 16; i++) { if (cursor_state[i].proc == p) return i; if (!cursor_state[i].proc && free < 0) free = i; }
    if (free < 0) return -1;
    cursor_state[free].proc = p; cursor_state[free].count = 0; cursor_state[free].none = false;
    return free;
}
static void cursor_apply(w32_proc_t *p, int i) {
    bool hide = cursor_state[i].count < 0 || cursor_state[i].none;
    for (w32win_t *w = all_wins; w; w = w->all_next)
        if (w->proc == p && !w->parent && w->top && !w->destroyed) w->top->cursor = hide ? CURSOR_NONE : CURSOR_ARROW;
}
/* a process that ends: its slot is free again */
static void cursor_forget(w32_proc_t *p) { for (int i = 0; i < 16; i++) if (cursor_state[i].proc == p) cursor_state[i].proc = NULL; }
static HANDLE WINAPI SetCursor(HANDLE c) {
    w32_proc_t *p = P; int i = cursor_slot(p);
    if (i < 0) return 0x20001;
    HANDLE old = cursor_state[i].none ? 0 : 0x20001;
    cursor_state[i].none = c == 0;
    cursor_apply(p, i);
    return old;
}
static int WINAPI ShowCursor(BOOL s) {
    w32_proc_t *p = P; int i = cursor_slot(p);
    if (i < 0) return s ? 0 : -1;
    cursor_state[i].count += s ? 1 : -1;
    cursor_apply(p, i);
    return cursor_state[i].count;
}
static BOOL WINAPI GetCursorPos(LONG *pt) { pt[0] = wm_mouse_x; pt[1] = wm_mouse_y; return 1; }
/* the pointer itself moves (games centre it each frame and turn by how far it then goes) */
static BOOL WINAPI SetCursorPos(int x, int y) {
    extern void mouse_warp(int x, int y);
    x = CLAMP(x, 0, SCREEN_W - 1); y = CLAMP(y, 0, SCREEN_H - 1);
    mouse_warp(x, y);
    wm_mouse_x = x; wm_mouse_y = y;
    return 1;
}
static BOOL WINAPI DestroyIcon(HANDLE h) { return 1; }
static BOOL WINAPI DrawIcon(HDC h, int x, int y, HANDLE i) { dc_t *d = DC(h); if (!d) return 0; draw_icon(d->surf, ICON_EXE, 32, d->ox + x, d->oy + y); return 1; }
/* as Windows returns them: GetKeyState 0xFF80 for a key that is down (programs test the
 * sign of the short or of its low byte), plus the toggle bit; GetAsyncKeyState 0x8000
 * when down, plus 1 when pressed since the last call */
static int16_t WINAPI GetKeyState(int vk) { vk &= 0xFF; return (int16_t)((keystate[vk] ? 0xFF80 : 0) | (keytoggle[vk] & 1)); }
static int16_t WINAPI GetAsyncKeyState(int vk) {
    vk &= 0xFF;
    int16_t r = (int16_t)((keystate[vk] ? 0x8000 : 0) | (keyhit[vk] ? 1 : 0));
    keyhit[vk] = 0;
    return r;
}
static BOOL WINAPI GetKeyboardState(BYTE *s) { for (int i = 0; i < 256; i++) s[i] = (keystate[i] ? 0x80 : 0) | (keytoggle[i] & 1); return 1; }
static UINT WINAPI MapVirtualKeyW(UINT c, UINT t);
static UINT WINAPI MapVirtualKeyA(UINT c, UINT t) { return MapVirtualKeyW(c, t); }
static DWORD WINAPI GetSysColor(int i) { gdiobj_t *o = OBJ(i + 1); return o ? (C_R(o->color) | (C_G(o->color) << 8) | (C_B(o->color) << 16)) : 0; }
static HGDIOBJ WINAPI GetSysColorBrush(int i) { return i + 1; }
static int CDECL wsprintfA(char *b, const char *f, ...) { w32_va_list ap; w32_va_start(ap, f); int r = w32_vformat(b, 1024, f, ap); w32_va_end(ap); return r; }
static int WINAPI wvsprintfA(char *b, const char *f, w32_va_list ap) { return w32_vformat(b, 1024, f, ap); }
static char *WINAPI CharUpperA(char *s) { if ((uintptr_t)s < 0x10000) return (char *)(uintptr_t)toupper((int)(uintptr_t)s); for (char *p = s; *p; p++) *p = toupper(*p); return s; }
static char *WINAPI CharLowerA(char *s) { if ((uintptr_t)s < 0x10000) return (char *)(uintptr_t)tolower((int)(uintptr_t)s); for (char *p = s; *p; p++) *p = tolower(*p); return s; }
static char *WINAPI CharNextA(const char *s) { return (char *)(*s ? s + 1 : s); }
static char *WINAPI CharPrevA(const char *a, const char *s) { return (char *)(s > a ? s - 1 : a); }
static BOOL WINAPI IsCharAlphaA(char c) { return isalpha(c); }
static BOOL WINAPI IsCharAlphaNumericA(char c) { return isalnum(c); }
static BOOL WINAPI CloseClipboard(void) { return 1; }
static HANDLE WINAPI SetWindowsHookExA(int a, void *b, HINSTANCE c, DWORD d) { return 0; }
static UINT WINAPI RegisterWindowMessageA(const char *s) { return 0xC100 + (strlen(s) & 0xFF); }
static BOOL WINAPI SystemParametersInfoA(UINT a, UINT b, void *p, UINT f) { if (a == 0x30 && p) { RECT32 *r = p; r->l = r->t = 0; r->r = SCREEN_W; r->b = SCREEN_H - T.taskbar_h; } return 1; }
static HANDLE WINAPI MonitorFromWindow(HWND h, DWORD f) { return 0x30001; }
static UINT WINAPI GetDoubleClickTime(void) { return wm_dblclick_ms; }
static BOOL WINAPI ScrollWindow(HWND h, int dx, int dy, const RECT32 *a, const RECT32 *b) { InvalidateRect(h, NULL, 1); return 1; }
static BOOL WINAPI SetScrollRange(HWND h, int b, int mn, int mx, BOOL r) { return 1; }
static int WINAPI SetScrollPos(HWND h, int b, int p, BOOL r) { return 0; }
static int WINAPI GetScrollPos(HWND h, int b) { return 0; }
static BOOL WINAPI ShowScrollBar(HWND h, int b, BOOL s) { return 1; }
static HANDLE WINAPI BeginDeferWindowPos(int n) { return 1; }
#include "user32_dlg.inc"
#include "user32_more.inc"
#include "comctl.inc"
#include "gdiplus.inc"

/* for opengl32/d3d9 (gl.c, d3d9.c): the window behind a DC, and a window's client pixels */
uintptr_t w32_dc_hwnd(uintptr_t hdc) { dc_t *d = DC(hdc); return d && d->win ? (uintptr_t)d->win : 0; }
bool w32_hwnd_client(uintptr_t hwnd, canvas_t **surf, rect_t *client) {
    w32win_t *w = W(hwnd);
    if (!w) return false;
    w32win_t *t = top_of(w);
    if (!t || !t->top || !t->top->surf) return false;
    int ox, oy; abs_origin(w, &ox, &oy);
    *surf = t->top->surf;
    *client = w->parent ? R(ox, oy, w->w, w->h) : R(0, 0, t->top->surf->w, t->top->surf->h);
    return true;
}
void w32_user_cleanup(w32_proc_t *p) {
    for (int i = 0; i < 64; i++) if (timers[i].proc == p) timers[i].used = false;
    cursor_forget(p);
    for (w32win_t **pp = &all_wins; *pp;) {
        w32win_t *w = *pp;
        if (w->proc == p) {
            if (w->top) { w->top->w32 = NULL; w->top->destroy = NULL; w->top->mouse = NULL; w->top->key = NULL; w->top->close = NULL; w->top->resized = NULL; w->top->activate = NULL; wm_destroy(w->top); }
            if (focus_win == w) focus_win = NULL;
            if (hot_ctl == w) hot_ctl = NULL;
            for (int i = 0; i < w->lb_n; i++) kfree(w->lb_items[i]);
            kfree(w->lb_items); kfree(w->lb_data); kfree(w->lb_selbits); kfree(w->extra); kfree(w->cb_menu);
            extern void w32_tv_free_all(w32win_t *w); w32_tv_free_all(w);
            *pp = w->all_next; w->magic = 0; kfree(w);
        } else pp = &w->all_next;
    }
    for (w32cls_t **pp = &classes; *pp;) { w32cls_t *c = *pp; if (c->owner == p) { *pp = c->next; kfree(c); } else pp = &c->next; }
}

void w32_crt_init(void);
void w32_init(void) {
    stock_init();
    w32_crt_init();
    klog("w32: Win32 subsystem ready (%d kernel32, %d msvcrt, %d user32, %d gdi32 exports)", kernel32_count, msvcrt_count, user32_count, gdi32_count);
}

#define E(n) { #n, (void *)n }
const w32_export_t user32_exports[] = {
    E(RegisterClassA), E(RegisterClassExA), E(UnregisterClassA), E(GetClassInfoA), E(CreateWindowExA), { "CreateWindowExW", (void *)CreateWindowExW2 }, E(DestroyWindow), E(ShowWindow), E(UpdateWindow),
    E(InvalidateRect), E(ValidateRect), E(RedrawWindow), E(GetClientRect), E(GetWindowRect), E(ClientToScreen), E(ScreenToClient), E(AdjustWindowRect), E(AdjustWindowRectEx),
    E(MoveWindow), E(SetWindowPos), E(SetWindowTextA), E(GetWindowTextA), E(GetWindowTextLengthA), E(EnableWindow), E(IsWindowEnabled), E(IsWindow), E(IsWindowVisible),
    E(IsIconic), E(IsZoomed), E(GetParent), E(GetDlgItem), E(GetDlgCtrlID), E(SetDlgItemTextA), E(GetDlgItemTextA), E(SetDlgItemInt), E(GetDlgItemInt), E(SetFocus), E(GetFocus),
    E(GetActiveWindow), E(GetForegroundWindow), E(SetForegroundWindow), E(SetActiveWindow), E(BringWindowToTop), E(GetDesktopWindow), E(SetCapture), E(ReleaseCapture), E(GetCapture),
    E(FindWindowA), E(GetWindow), E(GetWindowLongA), E(SetWindowLongA), E(GetWindowLongPtrA), E(SetWindowLongPtrA), E(GetClassLongPtrA), E(SetClassLongPtrA),
    E(GetClassLongA), E(SetClassLongA), E(GetClassNameA), E(EnumWindows), E(FlashWindow),
    E(SendMessageA), E(PostMessageA), E(PostThreadMessageA), E(PostQuitMessage), E(GetMessageA), E(PeekMessageA), E(WaitMessage), E(TranslateMessage), E(DispatchMessageA),
    { "GetMessageW", (void *)GetMessageA }, { "PeekMessageW", (void *)PeekMessageA }, { "DispatchMessageW", (void *)DispatchMessageA }, E(DefWindowProcW),
    { "SendMessageW", (void *)SendMessageW_ }, { "PostMessageW", (void *)PostMessageA },
    E(IsDialogMessageA), E(TranslateAcceleratorA), E(LoadAcceleratorsA), E(CallWindowProcA), E(GetMessageTime), E(GetMessagePos), E(InSendMessage), E(MsgWaitForMultipleObjects),
    E(BeginPaint), E(EndPaint), E(DefWindowProcA), E(GetDC), E(GetWindowDC), E(WindowFromDC), E(ReleaseDC), E(FillRect), E(FrameRect), E(InvertRect), E(DrawFocusRect), E(DrawEdge), E(DrawTextA),
    E(SetRect), E(SetRectEmpty), E(CopyRect), E(OffsetRect), E(InflateRect), E(IsRectEmpty), E(PtInRect), E(EqualRect), E(IntersectRect), E(UnionRect),
    E(SetTimer), E(KillTimer), E(MessageBoxA), E(MessageBoxW), E(MessageBoxExA), { "MessageBeep", (void *)MessageBeep }, E(DialogBoxParamA), E(CreateDialogParamA), E(EndDialog),
    E(CreateMenu), E(CreatePopupMenu), E(AppendMenuA), E(DestroyMenu), E(DrawMenuBar), E(SetMenu), E(GetMenu), E(GetSubMenu), E(CheckMenuItem), E(EnableMenuItem), { "LoadMenuA", (void *)LoadMenuA2 }, { "GetSystemMenu", (void *)GetSystemMenu2 },
    E(GetSystemMetrics), E(LoadCursorA), E(LoadIconA), { "LoadCursorW", (void *)LoadCursorA }, { "LoadIconW", (void *)LoadIconA }, E(LoadImageA), E(SetCursor), E(ShowCursor), E(GetCursorPos),
    E(SetCursorPos), E(DestroyIcon), E(DrawIcon), E(GetKeyState), E(GetAsyncKeyState), E(GetKeyboardState), E(MapVirtualKeyA), E(GetSysColor), E(GetSysColorBrush),
    E(wsprintfA), E(wvsprintfA), E(CharUpperA), E(CharLowerA), E(CharNextA), E(CharPrevA), E(IsCharAlphaA), E(IsCharAlphaNumericA), { "OpenClipboard", (void *)OpenClipboard2 }, E(CloseClipboard),
    E(SetWindowsHookExA), E(RegisterWindowMessageA), E(SystemParametersInfoA), E(MonitorFromWindow), E(GetDoubleClickTime), E(ScrollWindow), E(SetScrollRange), E(SetScrollPos),
    E(GetScrollPos), E(ShowScrollBar), E(BeginDeferWindowPos),
    /* dialogs, controls, Unicode and the rest (user32_dlg.inc) */
    E(DefDlgProcA), E(DefDlgProcW), E(CreateDialogParamW), E(CreateDialogIndirectParamA), E(CreateDialogIndirectParamW), E(DialogBoxParamW),
    E(DialogBoxIndirectParamA), E(DialogBoxIndirectParamW), E(IsDialogMessageW), E(GetNextDlgTabItem), E(GetNextDlgGroupItem),
    E(MapDialogRect), E(GetDialogBaseUnits), E(CheckDlgButton), E(IsDlgButtonChecked), E(CheckRadioButton), E(SendDlgItemMessageA), E(SendDlgItemMessageW),
    E(SetWindowTextW), E(GetWindowTextW), E(GetWindowTextLengthW), E(SetDlgItemTextW), E(GetDlgItemTextW), E(GetClassNameW), E(RegisterClassW), E(RegisterClassExW),
    E(UnregisterClassW), E(GetClassInfoW), E(LoadStringA), E(LoadStringW), E(LoadMenuW), E(LoadImageW), E(DrawIconEx), E(InsertMenuA), E(InsertMenuW), E(AppendMenuW),
    E(DeleteMenu), E(RemoveMenu), E(GetMenuItemCount), E(GetMenuItemID), E(ModifyMenuA), E(TrackPopupMenu), E(TrackPopupMenuEx),
    E(CreateCaret), E(DestroyCaret), E(SetCaretPos), E(GetCaretPos), E(ShowCaret), E(HideCaret), E(GetCaretBlinkTime), E(SetCaretBlinkTime),
    E(EmptyClipboard), E(SetClipboardData), E(GetClipboardData), E(IsClipboardFormatAvailable), E(RegisterClipboardFormatA), E(RegisterClipboardFormatW), E(EnumClipboardFormats),
    E(GetKeyboardLayout), E(GetKeyboardLayoutList), E(SetKeyboardState), E(ToAsciiEx), E(ToAscii), E(ToUnicode), E(ToUnicodeEx), E(MapVirtualKeyExA), E(VkKeyScanA),
    E(SetScrollInfo), E(GetScrollInfo), E(GetWindowPlacement), E(SetWindowPlacement), E(MessageBoxIndirectW), E(MessageBoxIndirectA), E(EnumChildWindows),
    E(ChildWindowFromPoint), E(WindowFromPoint), E(IsChild), E(GetAncestor), E(LockWindowUpdate), E(SetParent), E(MapWindowPoints), E(EnableScrollBar), E(IsWindowUnicode),
    E(CallWindowProcW), E(SendMessageTimeoutA), E(SendNotifyMessageA), E(RegisterWindowMessageW), E(SystemParametersInfoW), E(FindWindowW), E(GetWindowThreadProcessId),
    E(GetMonitorInfoA), { "GetMonitorInfoW", (void *)GetMonitorInfoA }, E(MonitorFromPoint), E(MonitorFromRect), E(GetUpdateRect), E(GetUpdateRgn), E(InvalidateRgn), E(ValidateRgn),
    E(EndDeferWindowPos), E(DeferWindowPos), E(OpenIcon), E(CloseWindow), E(ShowWindowAsync), E(DestroyCursor), E(CopyIcon), E(CreateIconIndirect), E(GetIconInfo), E(CopyImage),
    E(ClipCursor), E(GetClipCursor), E(GetSystemMetricsForDpi), E(GetDpiForWindow), E(GetDpiForSystem), E(SetProcessDPIAware), E(SetProcessDpiAwarenessContext), E(IsProcessDPIAware),
    E(AdjustWindowRectExForDpi), E(UnhookWindowsHookEx), E(CallNextHookEx), E(TranslateMDISysAccel), E(DestroyAcceleratorTable), E(CreateAcceleratorTableA), E(DragDetect),
    E(GetLastInputInfo), E(AllowSetForegroundWindow), E(FlashWindowEx), E(DrawTextW), E(DrawTextExW), E(DrawTextExA), E(wsprintfW), E(EnumThreadWindows), E(SetWindowContextHelpId),
    E(WinHelpA), E(LoadCursorFromFileA), E(CopyAcceleratorTableA),
    { "SetWindowLongPtrW", (void *)SetWindowLongPtrA }, { "GetWindowLongPtrW", (void *)GetWindowLongPtrA }, { "SetWindowLongW", (void *)SetWindowLongA }, { "GetWindowLongW", (void *)GetWindowLongA },
    { "LoadAcceleratorsW", (void *)LoadAcceleratorsA }, { "TranslateAcceleratorW", (void *)TranslateAcceleratorA },
    { "SetWindowsHookExW", (void *)SetWindowsHookExA }, { "PostThreadMessageW", (void *)PostThreadMessageA }, { "CallMsgFilterW", (void *)TranslateMDISysAccel },
};
const int user32_count = ARRAY_LEN(user32_exports);
int WINAPI w32_ChoosePixelFormat(HDC dc, const void *p);
BOOL WINAPI w32_SetPixelFormat(HDC dc, int fmt, const void *p);
int WINAPI w32_GetPixelFormat(HDC dc);
int WINAPI w32_DescribePixelFormat(HDC dc, int fmt, UINT bytes, void *p);
BOOL WINAPI w32_SwapBuffers(HDC dc);
const w32_export_t gdi32_exports[] = {
    E(GetStockObject), E(CreatePen), E(CreateSolidBrush), E(CreateHatchBrush), E(CreateBrushIndirect), E(CreateFontA), E(CreateFontIndirectA), E(DeleteObject), E(SelectObject), E(GetObjectA),
    E(CreateCompatibleDC), E(DeleteDC), E(CreateCompatibleBitmap), E(CreateBitmap), E(SetBkMode), E(SetBkColor), E(SetTextColor), E(SetTextAlign), E(SetROP2), E(SetMapMode),
    E(TextOutA), E(ExtTextOutA), E(GetTextExtentPoint32A), { "GetTextExtentPointA", (void *)GetTextExtentPoint32A }, E(GetTextMetricsA), E(MoveToEx), E(LineTo), E(Polyline), E(Polygon),
    E(Rectangle), E(RoundRect), E(Ellipse), E(SetPixel), E(SetPixelV), E(GetPixel), E(BitBlt), E(StretchBlt), E(PatBlt), E(GetDeviceCaps),
    /* pixel formats for OpenGL (gl.c) */
    { "ChoosePixelFormat", (void *)w32_ChoosePixelFormat }, { "SetPixelFormat", (void *)w32_SetPixelFormat }, { "GetPixelFormat", (void *)w32_GetPixelFormat },
    { "DescribePixelFormat", (void *)w32_DescribePixelFormat }, { "SwapBuffers", (void *)w32_SwapBuffers },
    E(ExtTextOutW), E(TextOutW), E(GetTextExtentPoint32W), { "GetTextExtentPointW", (void *)GetTextExtentPoint32W }, E(GetBkMode), E(GetBkColor), E(GetTextColor),
    E(GetCharWidth32A), E(GetCharWidth32W), { "GetCharWidthA", (void *)GetCharWidth32A }, { "GetCharWidthW", (void *)GetCharWidth32W }, E(GetCharABCWidthsFloatA),
    E(GetCharABCWidthsA), E(GetCharABCWidthsW), E(GetCharacterPlacementW), E(GetTextExtentExPointA), E(GetTextExtentExPointW), E(GetOutlineTextMetricsA),
    E(TranslateCharsetInfo), E(GetCurrentObject), E(IntersectClipRect), E(ExcludeClipRect), E(SelectClipRgn), E(GetDIBits), E(CreatePalette), E(SelectPalette),
    E(RealizePalette), E(SetPaletteEntries), E(UnrealizeObject), E(UpdateColors), E(GetNearestPaletteIndex),
};
const int gdi32_count = ARRAY_LEN(gdi32_exports);
