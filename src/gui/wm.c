/* Window manager: z-order, decorations, compositing, input routing */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "animation.h"
#include "usb.h"

#define MAX_WIN 64
static window_t *wins[MAX_WIN];
static int nwin;
static int next_wid = 1;
window_t *wm_active;
int wm_mouse_x, wm_mouse_y;
uint32_t wm_dblclick_ms = 500;
bool wm_need_redraw = true;
uint32_t gui_composes;

bool wm_animations = true;
int wm_animation_style = WINDOW_ANIM_BORDER;
static int prev_buttons;
static window_t *capture;
static int capture_btn;
static window_t *dying[MAX_WIN];
static int ndying;

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CAPBTN, DRAG_CLIENT, DRAG_FILES };
static struct { int mode; window_t *w; int edges; int mx, my; rect_t start; int btn; } drag;
static struct { window_t *w; int btn; } cap_hot;
static uint32_t last_click_time; static int last_click_x, last_click_y, last_click_btn;

/* Effects own pixels/IDs, never application data or a pointer to a destroyed
 * window. Logical state changes immediately; only composition is animated. */
enum { FX_OPEN, FX_CLOSE, FX_MINIMIZE, FX_RESTORE, FX_SIZE };
#define MAX_EFFECTS 12
#define EFFECT_BUDGET (32u * 1024 * 1024)
typedef struct {
    bool used, pending;
    int id, before, kind, style;
    rect_t from, to;
    animation_t time;
    canvas_t *image;
} window_effect_t;
static window_effect_t effects[MAX_EFFECTS];
static uint32_t effect_bytes, effect_frame;
static bool effects_suppressed;
static void paint_window_at(canvas_t *c, window_t *w, int x, int y);
static void paint_file_drag(canvas_t *c);
static void effect_begin(window_t *w, int kind, rect_t from, rect_t to, bool snapshot);
static void effects_cancel(void);
static void effect_cancel_window(window_t *w);
static window_effect_t *effect_for(window_t *w);
static rect_t effect_rect(window_effect_t *e);
static bool effect_mouse(int x, int y);
static bool effect_covers(int x, int y);

/* deferred calls */
typedef struct { wm_deferred_fn fn; void *arg; } defer_t;
static defer_t defq[64]; static int defn;
void wm_defer(wm_deferred_fn fn, void *arg) { if (defn < 64) { defq[defn].fn = fn; defq[defn].arg = arg; defn++; } }

/* ---------------- geometry ---------------- */
static int menubar_h(window_t *w) { return w->nmenus ? T.menubar_h : 0; }
static bool framed(window_t *w) { return !(w->flags & (WF_POPUP | WF_SKINNED)); }
rect_t wm_client_rect(window_t *w) {
    if (!framed(w)) return R(w->x, w->y, w->w, w->h);
    int b = T.border, th = T.title_h;
    if (T.classic) return R(w->x + b, w->y + th + menubar_h(w), w->w - 2 * b, w->h - th - b - menubar_h(w));
    return R(w->x + b, w->y + th + menubar_h(w), w->w - 2 * b, w->h - th - b - menubar_h(w));
}
rect_t wm_work_area(void) { return R(0, 0, SCREEN_W, SCREEN_H - T.taskbar_h); }
static void sync_surface(window_t *w) {
    rect_t c = wm_client_rect(w);
    if (!w->surf) {
        w->surf = canvas_new(MAX(1, c.w), MAX(1, c.h));
        /* out of memory: a 1x1 stand-in keeps the program's drawing harmless until the window
         * closes (wm_run) and says why */
        if (!w->surf) { w->surf = canvas_new(1, 1); w->flags |= WF_NOMEM; }
    } else if (w->surf->w != c.w || w->surf->h != c.h) {
        canvas_resize(w->surf, MAX(1, c.w), MAX(1, c.h));
        if (w->surf->w != MAX(1, c.w) || w->surf->h != MAX(1, c.h)) w->flags |= WF_NOMEM;
        if (w->resized) w->resized(w);
    }
    w->dirty = true;
}
static void frame_size_for_client(int cw, int ch, int flags, int nmenus, int *ow, int *oh) {
    if (flags & (WF_POPUP | WF_SKINNED)) { *ow = cw; *oh = ch; return; }
    *ow = cw + 2 * T.border;
    *oh = ch + T.title_h + T.border + (nmenus ? T.menubar_h : 0);
}
void wm_resize_client(window_t *w, int cw, int ch) {
    effect_cancel_window(w);
    frame_size_for_client(cw, ch, w->flags, w->nmenus, &w->w, &w->h);
    sync_surface(w); wm_need_redraw = true;
}
void wm_center(window_t *w) {
    rect_t wa = wm_work_area();
    w->x = (wa.w - w->w) / 2; w->y = MAX(0, (wa.h - w->h) / 2);
    wm_need_redraw = true;
}

/* ---------------- list management ---------------- */
int wm_window_count(void) { return nwin; }
window_t *wm_window_at(int i) { return (i >= 0 && i < nwin) ? wins[i] : NULL; }
window_t *wm_focused(void) { return wm_active; }
window_t *wm_find(int id) { for (int i = 0; i < nwin; i++) if (wins[i]->id == id) return wins[i]; return NULL; }
bool wm_alive(window_t *w) { for (int i = 0; i < nwin; i++) if (wins[i] == w) return true; return false; }
static int index_of(window_t *w) { for (int i = 0; i < nwin; i++) if (wins[i] == w) return i; return -1; }
static void raise(window_t *w) {
    int i = index_of(w); if (i < 0) return;
    for (; i < nwin - 1; i++) {
        if (!(w->flags & WF_TOPMOST) && (wins[i + 1]->flags & WF_TOPMOST)) break;
        wins[i] = wins[i + 1]; wins[i + 1] = w;
    }
}
static void set_active(window_t *w) {
    if (wm_active == w) return;
    window_t *old = wm_active;
    wm_active = w;
    if (old && wm_alive(old)) { old->ui.focus = old->ui.focus; if (old->activate) old->activate(old, false); old->dirty = true; }
    if (w && w->activate) w->activate(w, true);
    wm_need_redraw = true;
}
static window_t *modal_child(window_t *w) {
    for (int i = nwin - 1; i >= 0; i--) if (wins[i]->owner == w && (wins[i]->flags & WF_MODAL)) return modal_child(wins[i]) ? modal_child(wins[i]) : wins[i];
    return NULL;
}
void wm_focus(window_t *w) {
    if (!w || !wm_alive(w)) { set_active(NULL); return; }
    window_t *m = modal_child(w);
    if (m) { if (w->state == WS_MINIMIZED) { effect_cancel_window(w); w->state = w->minimized_state; } raise(w); w = m; }
    if (w->state == WS_MINIMIZED) {
        rect_t from = shell_task_button_rect(w);
        window_effect_t *e = effect_for(w); if (e) from = effect_rect(e);
        w->state = w->minimized_state; w->dirty = true;
        effect_begin(w, FX_RESTORE, from, R(w->x, w->y, w->w, w->h), false);
    }
    w->flags &= ~WF_HIDDEN;
    raise(w);
    set_active(w);
    wm_need_redraw = true;
}
static void focus_topmost(void) {
    for (int i = nwin - 1; i >= 0; i--)
        if (wins[i]->state != WS_MINIMIZED && !(wins[i]->flags & WF_HIDDEN)) { set_active(wins[i]); return; }
    set_active(NULL);
}
window_t *wm_create(const char *title, int x, int y, int cw, int ch, int flags) {
    if (nwin >= MAX_WIN) return NULL;
    window_t *w = kzalloc(sizeof(window_t));
    w->id = next_wid++;
    strlcpy(w->title, title, sizeof w->title);
    w->icon = -1;
    w->flags = flags;
    w->min_w = 120; w->min_h = 60;
    frame_size_for_client(cw, ch, flags, 0, &w->w, &w->h);
    static int cascade = 0;
    if (x < 0) { rect_t wa = wm_work_area(); x = 60 + (cascade % 8) * 26; y = 40 + (cascade % 8) * 26; cascade++; if (x + w->w > wa.w) x = MAX(0, wa.w - w->w); if (y + w->h > wa.h) y = MAX(0, wa.h - w->h); }
    w->x = x; w->y = y;
    w->opening = !(flags & (WF_HIDDEN | WF_POPUP));
    task_t *t = task_current();
    w->pid = t ? t->pid : 0;
    w->ui.focus = 0;
    wins[nwin++] = w;
    for (int i = 0; i < MAX_EFFECTS; i++)
        if (effects[i].used && !effects[i].id && !effects[i].before) effects[i].before = w->id;
    raise(w);
    sync_surface(w);
    if (!(flags & WF_HIDDEN)) set_active(w);
    wm_need_redraw = true;
    return w;
}
void wm_show(window_t *w) { if (w->flags & WF_HIDDEN) w->opening = !(w->flags & WF_POPUP); w->flags &= ~WF_HIDDEN; wm_focus(w); }
void wm_destroy(window_t *w) {
    int i = index_of(w);
    if (i < 0) return;
    for (int j = 0; j < ndying; j++) if (dying[j] == w) return;
    window_effect_t *effect = effect_for(w);
    if (!w->opening && (!effect || effect->kind != FX_CLOSE)) {
        rect_t r = R(w->x, w->y, w->w, w->h);
        effect_begin(w, FX_CLOSE, r, R(r.x + r.w / 2, r.y + r.h / 2, 2, 2), true);
    }
    /* destroy owned windows too */
    for (int k = nwin - 1; k >= 0; k--) if (wins[k]->owner == w) wm_destroy(wins[k]);
    i = index_of(w);
    int before = i + 1 < nwin ? wins[i + 1]->id : 0;
    for (int j = 0; j < MAX_EFFECTS; j++) {
        window_effect_t *e = &effects[j];
        if (e->used && e->before == w->id) e->before = before;
        if (e->used && e->id == w->id && e->kind == FX_CLOSE) { e->id = 0; e->before = before; }
    }
    effect_cancel_window(w);        /* a close snapshot has already been detached */
    for (int j = i; j < nwin - 1; j++) wins[j] = wins[j + 1];
    nwin--;
    if (capture == w) capture = NULL;
    if (drag.w == w) drag.mode = DRAG_NONE;
    if (cap_hot.w == w) cap_hot.w = NULL;
    if (wm_active == w) { wm_active = NULL; if (w->owner && wm_alive(w->owner)) wm_focus(w->owner); else focus_topmost(); }
    dying[ndying++] = w;
    wm_need_redraw = true;
}
static void reap_windows(void) {
    for (int i = 0; i < ndying; i++) {
        window_t *w = dying[i];
        if (w->destroy) w->destroy(w);
        if (w->menus) kfree(w->menus);
        canvas_free(w->surf);
        kfree(w);
    }
    ndying = 0;
}
void wm_destroy_for_pid(int pid) {
    if (pid == 0) return;
    for (int i = nwin - 1; i >= 0; i--) if (i < nwin && wins[i]->pid == pid) wm_destroy(wins[i]);
}
void wm_close(window_t *w) {
    if (!wm_alive(w)) return;
    if (w->close && !w->close(w)) return;
    if (!wm_alive(w)) return;
    rect_t r = R(w->x, w->y, w->w, w->h);
    effect_begin(w, FX_CLOSE, r, R(r.x + r.w / 2, r.y + r.h / 2, 2, 2), true);
    wm_destroy(w);
}
void wm_minimize(window_t *w) {
    if (!w || (w->flags & WF_NO_MIN) || w->state == WS_MINIMIZED) return;
    w->opening = false;
    rect_t from = R(w->x, w->y, w->w, w->h);
    window_effect_t *e = effect_for(w); if (e) from = effect_rect(e);
    effect_begin(w, FX_MINIMIZE, from, shell_task_button_rect(w), true);
    w->minimized_state = w->state;
    w->state = WS_MINIMIZED;
    w->dirty = false;                 /* a minimized window is not painted */
    if (wm_active == w) { wm_active = NULL; focus_topmost(); }
    wm_need_redraw = true;
}
void wm_maximize(window_t *w, bool on) {
    if (!w || (on && (w->flags & WF_NO_MAX))) return;
    if (w->state == WS_MINIMIZED) { effect_cancel_window(w); w->state = w->minimized_state; }
    rect_t from = R(w->x, w->y, w->w, w->h);
    window_effect_t *e = effect_for(w); if (e) from = effect_rect(e);
    bool changed = on != (w->state == WS_MAXIMIZED);
    if (on) {
        if (w->state != WS_MAXIMIZED) w->restore = R(w->x, w->y, w->w, w->h);
        rect_t wa = wm_work_area();
        if (T.classic) { w->x = -T.border; w->y = -T.border; w->w = wa.w + 2 * T.border; w->h = wa.h + 2 * T.border; }
        else { w->x = -T.border; w->y = 0; w->w = wa.w + 2 * T.border; w->h = wa.h + T.border; }
        w->state = WS_MAXIMIZED;
    } else if (w->state == WS_MAXIMIZED) {
        w->x = w->restore.x; w->y = w->restore.y; w->w = w->restore.w; w->h = w->restore.h;
        w->state = WS_NORMAL;
    }
    sync_surface(w);
    if (changed && !w->opening) effect_begin(w, FX_SIZE, from, R(w->x, w->y, w->w, w->h), false);
    wm_need_redraw = true;
}
void wm_set_title(window_t *w, const char *t) { strlcpy(w->title, t, sizeof w->title); wm_need_redraw = true; }
void wm_invalidate(window_t *w) { if (w) w->dirty = true; }
void wm_redraw_all(void) { for (int i = 0; i < nwin; i++) wins[i]->dirty = true; wm_need_redraw = true; }
void wm_set_timer(window_t *w, uint32_t ms, void (*fn)(window_t *)) { w->timer = fn; w->timer_ms = ms; w->timer_next = timer_ticks() + ms; }
void wm_theme_changed(void) {
    wm_effects_changed();
    for (int i = 0; i < nwin; i++) {
        window_t *w = wins[i];
        if (w->state == WS_MAXIMIZED) { rect_t r = w->restore; wm_maximize(w, true); w->restore = r; }
        sync_surface(w);
    }
    shell_wallpaper_changed();
    extern void w32_theme_changed(void);
    w32_theme_changed();
    wm_need_redraw = true;
}

/* ---------------- popup menus ---------------- */
static struct {
    bool open; int x, y, w, h; menu_item_t items[32]; int n; int hot;
    popup_cb cb; void *ctx; window_t *menubar_win; int menubar_idx;
} pm;
#define PM_ITEM_H 22
#define PM_SEP_H 8
static int pm_item_y(int i) { int y = 3; for (int k = 0; k < i; k++) y += pm.items[k].label[0] == '-' ? PM_SEP_H : PM_ITEM_H; return y; }
void popup_menu(int x, int y, const menu_item_t *items, int n, popup_cb cb, void *ctx) {
    pm.n = MIN(n, 32);
    memcpy(pm.items, items, pm.n * sizeof(menu_item_t));
    int w = 120, h = 6;
    for (int i = 0; i < pm.n; i++) {
        if (pm.items[i].label[0] == '-') { h += PM_SEP_H; continue; }
        int tw = text_width(FONT_UI, pm.items[i].label) + 50;
        const char *tab = strchr(pm.items[i].label, '\t'); if (tab) tw += 30;
        if (tw > w) w = tw;
        h += PM_ITEM_H;
    }
    if (x + w > SCREEN_W) x = SCREEN_W - w - 2;
    if (y + h > SCREEN_H) y = y - h >= 0 ? y - h : MAX(0, SCREEN_H - h);   /* above, or as high as it fits */
    pm.x = x; pm.y = y; pm.w = w; pm.h = h; pm.hot = -1; pm.cb = cb; pm.ctx = ctx; pm.open = true;
    pm.menubar_win = NULL;
    wm_need_redraw = true;
}
void popup_close(void) { if (pm.open) { pm.open = false; pm.menubar_win = NULL; wm_need_redraw = true; } }
bool popup_open(void) { return pm.open; }
static void popup_paint(canvas_t *c) {
    if (!pm.open) return;
    draw_menu_panel(c, R(pm.x, pm.y, pm.w, pm.h));
    for (int i = 0; i < pm.n; i++) {
        int iy = pm.y + pm_item_y(i);
        menu_item_t *it = &pm.items[i];
        if (it->label[0] == '-') { hline(c, pm.x + (T.classic ? 2 : 1), iy + 3, pm.w - (T.classic ? 4 : 2), T.face_dark); if (T.classic) hline(c, pm.x + 2, iy + 4, pm.w - 4, T.face_light); continue; }
        bool hot = i == pm.hot && !(it->flags & MI_DISABLED);
        if (hot) fill_rect(c, pm.x + (T.classic ? 3 : 2), iy, pm.w - (T.classic ? 6 : 4), PM_ITEM_H, T.hot);
        uint32_t col = (it->flags & MI_DISABLED) ? T.gray_text : hot ? T.hot_text : T.text;
        char buf[96]; strlcpy(buf, it->label, sizeof buf);
        char *tab = strchr(buf, '\t'); if (tab) *tab = 0;
        draw_text(c, FONT_UI, pm.x + 24, iy + (PM_ITEM_H - font_height(FONT_UI)) / 2, buf, col);
        if (tab) draw_text(c, FONT_UI, pm.x + pm.w - 14 - text_width(FONT_UI, tab + 1), iy + (PM_ITEM_H - font_height(FONT_UI)) / 2, tab + 1, col);
        if (it->flags & MI_CHECKED) {
            int cx = pm.x + 9, cy = iy + 7;
            if (it->flags & MI_RADIO) fill_circle(c, cx + 3, cy + 4, 3, col);
            else { for (int k = 0; k < 3; k++) vline(c, cx + k, cy + 3 + k, 3, col); for (int k = 0; k < 4; k++) vline(c, cx + 3 + k, cy + 4 - k, 3, col); }
        }
    }
}
static int popup_hit(int x, int y) {
    if (!pt_in(R(pm.x, pm.y, pm.w, pm.h), x, y)) return -2;
    for (int i = 0; i < pm.n; i++) {
        int iy = pm.y + pm_item_y(i), ih = pm.items[i].label[0] == '-' ? PM_SEP_H : PM_ITEM_H;
        if (y >= iy && y < iy + ih) return pm.items[i].label[0] == '-' ? -1 : i;
    }
    return -1;
}
static void popup_activate(int i) {
    if (i < 0 || i >= pm.n || (pm.items[i].flags & MI_DISABLED)) return;
    popup_cb cb = pm.cb; void *ctx = pm.ctx; int id = pm.items[i].id;
    popup_close();
    if (cb) cb(ctx, id);
}
static void menubar_cb(void *ctx, int id) {
    window_t *w = ctx;
    if (wm_alive(w) && w->menu_cmd) { w->menu_cmd(w, id); w->dirty = true; }
}
static int menubar_item_x(window_t *w, int idx) {
    int x = 6;
    for (int i = 0; i < idx; i++) x += text_width(FONT_UI, w->menus[i].title) + 14;
    return x;
}
static void open_menubar(window_t *w, int idx) {
    menu_t *m = &w->menus[idx];
    int n = 0; while (n < 20 && m->items[n].label) n++;
    rect_t cr = wm_client_rect(w);
    int x = w->x + T.border + menubar_item_x(w, idx) - 6;
    popup_menu(x, cr.y, m->items, n, menubar_cb, w);
    pm.menubar_win = w; pm.menubar_idx = idx;
}
static int menubar_hit(window_t *w, int sx, int sy) {
    if (!w->nmenus) return -1;
    rect_t cr = wm_client_rect(w);
    int my = cr.y - T.menubar_h;
    if (sy < my || sy >= cr.y || sx < cr.x || sx >= cr.x + cr.w) return -1;
    int lx = sx - cr.x;
    for (int i = 0; i < w->nmenus; i++) {
        int x0 = menubar_item_x(w, i) - 6, x1 = x0 + text_width(FONT_UI, w->menus[i].title) + 14;
        if (lx >= x0 && lx < x1) return i;
    }
    return -2;
}

/* ---------------- hit testing ---------------- */
enum { HT_NONE, HT_CLIENT, HT_TITLE, HT_EDGE, HT_BTN, HT_MENUBAR, HT_FRAME };
static int cap_btn_at(window_t *w, int sx, int sy) {
    static const int kinds[3] = { 3, 1, 0 };
    for (int i = 0; i < 3; i++) { rect_t r = caption_button_rect(R(w->x, w->y, w->w, w->h), w->flags, kinds[i]); if (r.w && pt_in(r, sx, sy)) return kinds[i]; }
    return -1;
}
static int hit_test(window_t *w, int sx, int sy, int *edges, int *btn) {
    if (!pt_in(R(w->x, w->y, w->w, w->h), sx, sy)) return HT_NONE;
    if (!framed(w)) return HT_CLIENT;
    *edges = 0;
    if ((w->flags & WF_RESIZABLE) && w->state != WS_MAXIMIZED) {
        int g = T.border + 1, cg = 14;
        int lx = sx - w->x, ly = sy - w->y;
        if (lx < g) *edges |= 1; if (lx >= w->w - g) *edges |= 2;
        if (ly < 4) *edges |= 4; if (ly >= w->h - g) *edges |= 8;
        if (*edges) {
            if ((*edges & 12) && lx < cg) *edges |= 1; if ((*edges & 12) && lx >= w->w - cg) *edges |= 2;
            if ((*edges & 3) && ly < cg) *edges |= 4; if ((*edges & 3) && ly >= w->h - cg) *edges |= 8;
            return HT_EDGE;
        }
    }
    int b = cap_btn_at(w, sx, sy);
    if (b >= 0) { *btn = b; return HT_BTN; }
    if (sy < w->y + T.title_h) return HT_TITLE;
    if (menubar_hit(w, sx, sy) != -1) return HT_MENUBAR;
    if (pt_in(wm_client_rect(w), sx, sy)) return HT_CLIENT;
    return HT_FRAME;
}
static int edge_cursor(int e) {
    if ((e & 5) == 5 || (e & 10) == 10) return CURSOR_SIZE_NWSE;
    if ((e & 6) == 6 || (e & 9) == 9) return CURSOR_SIZE_NESW;
    if (e & 3) return CURSOR_SIZE_EW;
    return CURSOR_SIZE_NS;
}
static window_t *window_at(int sx, int sy) {
    for (int i = nwin - 1; i >= 0; i--) {
        window_t *w = wins[i];
        if (w->state == WS_MINIMIZED || (w->flags & WF_HIDDEN)) continue;
        window_effect_t *e = effect_for(w);
        if (pt_in(e ? effect_rect(e) : R(w->x, w->y, w->w, w->h), sx, sy)) return w;
    }
    return NULL;
}
bool wm_window_at_point(int x, int y) { return window_at(x, y) != NULL || effect_covers(x, y); }

/* ---------------- painting ---------------- */
/* "guiprof on" in the Command Prompt: every 2 seconds, how long each window spent
 * painting and how long composing the screen took (serial port and boot log) */
bool gui_profile;
static uint64_t prof_tsc_ms;                      /* TSC ticks per millisecond, measured */
static struct { window_t *w; uint64_t t; int n; } prof_win[16];
static uint64_t prof_compose_t; static int prof_compose_n; static uint32_t prof_start;
static void prof_paint(window_t *w, uint64_t t) {
    for (int i = 0; i < 16; i++) if (prof_win[i].w == w || !prof_win[i].w) { prof_win[i].w = w; prof_win[i].t += t; prof_win[i].n++; return; }
}
static void prof_report(void) {
    uint32_t now = timer_ticks();
    static uint64_t tsc0; static uint32_t tick0;
    if (!tick0) { tick0 = now; tsc0 = rdtsc(); }
    else if (!prof_tsc_ms && now - tick0 >= 500) prof_tsc_ms = (rdtsc() - tsc0) / (now - tick0);
    if (!gui_profile || !prof_tsc_ms) { prof_start = now; return; }
    if (now - prof_start < 2000) return;
    char line[512]; int o = snprintf(line, sizeof line, "guiprof %u ms: compose %u ms (%d)", now - prof_start,
                                     (unsigned)(prof_compose_t / prof_tsc_ms), prof_compose_n);
    for (int i = 0; i < 16 && prof_win[i].w; i++)
        if (wm_alive(prof_win[i].w) && o < (int)sizeof line - 60)
            o += snprintf(line + o, sizeof line - o, ", '%.20s' %u ms (%d)", prof_win[i].w->title,
                          (unsigned)(prof_win[i].t / prof_tsc_ms), prof_win[i].n);
    klog("%s", line);
    memset(prof_win, 0, sizeof prof_win); prof_compose_t = 0; prof_compose_n = 0; prof_start = now;
}
static void run_paint(window_t *w) {
    if (!w->paint) { w->dirty = false; return; }
    sync_surface(w);              /* e.g. a menu bar was attached after the window was created */
    w->dirty = false;             /* preserve invalidations made during this paint */
    canvas_reset_clip(w->surf);
    uint64_t t0 = gui_profile ? rdtsc() : 0;
    ui_begin(w);
    w->paint(w);
    ui_end(w);
    if (gui_profile) prof_paint(w, rdtsc() - t0);
    wm_need_redraw = true;
}
static void paint_menubar(canvas_t *c, window_t *w, int dx, int dy) {
    rect_t cr = wm_client_rect(w);
    cr.x += dx; cr.y += dy;
    int y = cr.y - T.menubar_h;
    fill_rect(c, cr.x, y, cr.w, T.menubar_h, T.classic ? T.face : T.face);
    if (!T.classic) hline(c, cr.x, y + T.menubar_h - 1, cr.w, T.face_dark);
    for (int i = 0; i < w->nmenus; i++) {
        int x = cr.x + menubar_item_x(w, i);
        int tw = text_width(FONT_UI, w->menus[i].title);
        bool open = pm.open && pm.menubar_win == w && pm.menubar_idx == i;
        if (open) fill_rect(c, x - 6, y + 1, tw + 12, T.menubar_h - 2, T.classic ? T.face : T.hot);
        if (open && T.classic) bevel(c, x - 6, y + 1, tw + 12, T.menubar_h - 2, false);
        draw_text(c, FONT_UI, x, y + (T.menubar_h - font_height(FONT_UI)) / 2, w->menus[i].title, open && !T.classic ? T.hot_text : T.text);
    }
}
static void paint_window_at(canvas_t *c, window_t *w, int x, int y) {
    if (framed(w)) {
        int hot = cap_hot.w == w ? cap_hot.btn : -1;
        int pressed = (drag.mode == DRAG_CAPBTN && drag.w == w && hot == drag.btn) ? drag.btn : -1;
        draw_window_frame(c, R(x, y, w->w, w->h), w->title, w->icon, w == wm_active, w->flags, hot, pressed, w->state);
        if (w->nmenus) paint_menubar(c, w, x - w->x, y - w->y);
    }
    rect_t cr = wm_client_rect(w);
    cr.x += x - w->x; cr.y += y - w->y;
    if (!(w->flags & WF_SKINNED)) { blit(c, cr.x, cr.y, w->surf, 0, 0, cr.w, cr.h); return; }
    /* skinned: the app's pixels with alpha 0 (rounded corners, cut-outs) show what is behind */
    if (!w->surf) return;
    rect_t r = rect_intersect(rect_intersect(R(cr.x, cr.y, cr.w, cr.h), R(cr.x, cr.y, w->surf->w, w->surf->h)), c->clip);
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        const uint32_t *s = w->surf->px + (size_t)(yy - cr.y) * w->surf->stride + (r.x - cr.x);
        uint32_t *d = c->px + (size_t)yy * c->stride + r.x;
        for (int xx = 0; xx < r.w; xx++) if (s[xx] >> 24) d[xx] = s[xx] | 0xFF000000u;
    }
}
void wm_begin_move(window_t *w) {
    if (!wm_alive(w) || w->state == WS_MAXIMIZED) return;
    capture = NULL; w->ui.down = false;
    drag.mode = DRAG_MOVE; drag.w = w; drag.mx = wm_mouse_x; drag.my = wm_mouse_y; drag.start = R(w->x, w->y, w->w, w->h);
}
void wm_begin_resize(window_t *w, int edges) {
    if (!wm_alive(w) || w->state == WS_MAXIMIZED || !(w->flags & WF_RESIZABLE)) return;
    capture = NULL; w->ui.down = false;
    drag.mode = DRAG_RESIZE; drag.w = w; drag.edges = edges; drag.mx = wm_mouse_x; drag.my = wm_mouse_y; drag.start = R(w->x, w->y, w->w, w->h);
}
void wm_set_skinned(window_t *w, bool on) {
    if (!wm_alive(w) || !!(w->flags & WF_SKINNED) == on) return;
    if (on) w->flags |= WF_SKINNED; else w->flags &= ~WF_SKINNED;
    effect_cancel_window(w);
    sync_surface(w);                             /* same outer size, the client area follows */
    wm_need_redraw = true;
}

static void effect_drop(window_effect_t *e) {
    if (e->image) { effect_bytes -= e->image->w * e->image->h * 4; canvas_free(e->image); }
    memset(e, 0, sizeof *e);
    wm_need_redraw = true;
}
static window_effect_t *effect_for(window_t *w) {
    if (w) for (int i = 0; i < MAX_EFFECTS; i++) if (effects[i].used && effects[i].id == w->id) return &effects[i];
    return NULL;
}
static void effect_cancel_window(window_t *w) {
    window_effect_t *e = effect_for(w); if (e) effect_drop(e);
}
static void effects_cancel(void) {
    for (int i = 0; i < MAX_EFFECTS; i++) if (effects[i].used) effect_drop(&effects[i]);
}
void wm_effects_changed(void) {
    effects_cancel();
    for (int i = 0; i < nwin; i++) wins[i]->opening = false;
    shell_effects_changed();
    wm_need_redraw = true;
}
static rect_t effect_rect(window_effect_t *e) {
    int p = animation_value(&e->time, timer_ticks());
    return R(animation_lerp(e->from.x, e->to.x, p), animation_lerp(e->from.y, e->to.y, p),
             animation_lerp(e->from.w, e->to.w, p), animation_lerp(e->from.h, e->to.h, p));
}
static bool effect_snapshot(window_effect_t *e, window_t *w) {
    uint64_t bytes = (uint64_t)w->w * w->h * 4;
    if (!w->surf || w->w <= 0 || w->h <= 0 || bytes > EFFECT_BUDGET - effect_bytes) return false;
    canvas_t *image = kzalloc(sizeof *image);
    if (!image) return false;
    uint32_t *pixels = kmalloc((size_t)bytes);
    if (!pixels) { kfree(image); return false; }
    canvas_init(image, pixels, w->w, w->h);
    e->image = image; effect_bytes += (uint32_t)bytes;
    /* Aero samples the backdrop before drawing glass; the application paints
     * only its client surface. Corners outside the frame remain transparent. */
    gfx_clear(e->image, T.face);
    rect_t visible = rect_intersect(R(w->x, w->y, w->w, w->h), R(0, 0, SCREEN_W, SCREEN_H));
    blit(e->image, visible.x - w->x, visible.y - w->y, &screen, visible.x, visible.y, visible.w, visible.h);
    paint_window_at(e->image, w, 0, 0);
    if (framed(w) && !T.classic && w->state != WS_MAXIMIZED) {
        for (int y = 0; y < MIN(7, w->h); y++) for (int x = 0; x < MIN(7, w->w / 2); x++) {
            if ((7 - x) * (7 - x) + (7 - y) * (7 - y) > 49) {
                e->image->px[y * e->image->stride + x] = 0;
                e->image->px[y * e->image->stride + w->w - 1 - x] = 0;
            }
        }
    }
    return true;
}
static void effect_begin(window_t *w, int kind, rect_t from, rect_t to, bool snapshot) {
    effect_cancel_window(w);
    if (effects_suppressed || !wm_animations || boot_safe_mode || (snapshot && w->opening) || wm_animation_style <= WINDOW_ANIM_NONE || wm_animation_style >= WINDOW_ANIM_COUNT ||
        (w->flags & (WF_HIDDEN | WF_POPUP)) || w->state == WS_MINIMIZED) return;
    window_effect_t *e = NULL;
    for (int i = 0; i < MAX_EFFECTS; i++) if (!effects[i].used) { e = &effects[i]; break; }
    if (!e) return;                /* a busy desktop still changes state immediately */
    e->used = true; e->id = w->id; e->kind = kind; e->style = wm_animation_style;
    e->from = from; e->to = to; e->pending = !snapshot;
    if (e->style == WINDOW_ANIM_AERO && (kind == FX_OPEN || kind == FX_CLOSE)) {
        rect_t full = kind == FX_OPEN ? to : from;
        rect_t small = R(full.x + full.w * 3 / 100, full.y + full.h * 3 / 100 + 12, MAX(1, full.w * 94 / 100), MAX(1, full.h * 94 / 100));
        if (kind == FX_OPEN) e->from = small; else e->to = small;
    }
    animation_set(&e->time, 0);
    if (snapshot) {
        if (e->style == WINDOW_ANIM_AERO && !effect_snapshot(e, w)) { effect_drop(e); return; }
        animation_start(&e->time, 255, timer_ticks(), e->style == WINDOW_ANIM_BORDER ? 180 : 220);
    }
    wm_need_redraw = true;
}
static void effect_paint(canvas_t *c, window_effect_t *e, window_t *w) {
    if (e->pending) {
        if (!w || (e->style == WINDOW_ANIM_AERO && !effect_snapshot(e, w))) { effect_drop(e); if (w) paint_window_at(c, w, w->x, w->y); return; }
        e->pending = false;
        animation_start(&e->time, 255, timer_ticks(), e->style == WINDOW_ANIM_BORDER ? 180 : 220);
    }
    rect_t r = effect_rect(e);
    if (e->style == WINDOW_ANIM_BORDER) {
        /* XP-style wireframe scaling: no repeatedly resized client paints. */
        draw_rect(c, r.x, r.y, r.w, r.h, T.select);
        if (r.w > 4 && r.h > 4) draw_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, mix(T.select, WHITE, 160));
        if (r.w > 8 && r.h > 8) draw_rect(c, r.x + 3, r.y + 3, r.w - 6, r.h - 6, T.select);
    } else {
        int p = animation_value(&e->time, timer_ticks());
        int alpha = e->kind == FX_CLOSE || e->kind == FX_MINIMIZE ? 255 - p : e->kind == FX_SIZE ? 205 + p * 50 / 255 : p;
        blit_scaled_alpha(c, r, e->image, alpha);
    }
}
static void effects_before(canvas_t *c, int id) {
    for (int i = 0; i < MAX_EFFECTS; i++)
        if (effects[i].used && !effects[i].id && effects[i].before == id) effect_paint(c, &effects[i], NULL);
}
static void effects_tick(uint32_t now) {
    if (now - effect_frame < 16) return;
    effect_frame = now;
    for (int i = 0; i < MAX_EFFECTS; i++) {
        window_effect_t *e = &effects[i];
        if (!e->used) continue;
        if (!wm_animations || (!e->pending && !animation_running(&e->time, now))) effect_drop(e);
        else wm_need_redraw = true;
    }
}
static bool effect_covers(int x, int y) {
    for (int i = 0; i < MAX_EFFECTS; i++) if (effects[i].used && pt_in(effect_rect(&effects[i]), x, y)) return true;
    return false;
}
static bool effect_mouse(int x, int y) {
    window_t *top = window_at(x, y);
    int top_z = top ? index_of(top) : -1;
    for (int i = MAX_EFFECTS - 1; i >= 0; i--) {
        window_effect_t *e = &effects[i];
        if (!e->used || !pt_in(effect_rect(e), x, y)) continue;
        window_t *w = wm_find(e->id), *before = wm_find(e->before);
        int z = w ? index_of(w) : before ? index_of(before) - 1 : nwin;
        if (top_z > z) continue;
        /* A click settles a moving visual, but must not activate a control at
         * a coordinate it has not reached yet, or click through to the sidebar. */
        effect_drop(e);
        if (w && w->state != WS_MINIMIZED) { wm_focus(w); w->dirty = true; }
        return true;
    }
    return false;
}
static int current_cursor(void) {
    if (drag.mode == DRAG_RESIZE) return edge_cursor(drag.edges);
    if (drag.mode == DRAG_CLIENT && capture) return capture->ui.cursor ? capture->ui.cursor : capture->cursor;
    if (pm.open) return CURSOR_ARROW;
    int sc = shell_cursor(wm_mouse_x, wm_mouse_y);
    if (sc >= 0) return sc;
    window_t *w = window_at(wm_mouse_x, wm_mouse_y);
    if (!w) return CURSOR_ARROW;
    if (effect_for(w)) return CURSOR_ARROW;
    int e = 0, b = -1;
    int ht = hit_test(w, wm_mouse_x, wm_mouse_y, &e, &b);
    if (ht == HT_EDGE) return edge_cursor(e);
    if (ht == HT_CLIENT) return w->ui.cursor ? w->ui.cursor : w->cursor;
    return CURSOR_ARROW;
}
/* for Flip 3D (flip3d.c): a window painted at (x, y) of another canvas */
void wm_paint_window_to(canvas_t *c, window_t *w, int x, int y) { paint_window_at(c, w, x, y); }
bool flip3d_start(window_t **list, int count, window_t *active, bool sticky);
bool flip3d_active(void);
bool flip3d_animating(void);
void flip3d_key(key_event_t *e);
void flip3d_mouse(int x, int y, int pressed, int wheel);
bool flip3d_paint(canvas_t *screen);
/* a repaint of part of the screen: composed with the screen clipped to it (desktop, windows and
 * taskbar each draw only what falls inside), then presented as usual */
static rect_t dmg;
static bool dmg_on;
void wm_redraw_rect(rect_t r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (dmg_on) {
        int x0 = MIN(dmg.x, r.x), y0 = MIN(dmg.y, r.y);
        int x1 = MAX(dmg.x + dmg.w, r.x + r.w), y1 = MAX(dmg.y + dmg.h, r.y + r.h);
        dmg = R(x0, y0, x1 - x0, y1 - y0);
    } else dmg = r;
    dmg_on = true;
}
static void compose_clipped(const rect_t *clip);
static void compose(void) { compose_clipped(NULL); }
static void compose_clipped(const rect_t *clip) {
    gui_composes++;
    if (clip && flip3d_active()) clip = NULL;
    if (flip3d_active()) {                       /* Flip 3D draws the whole screen */
        canvas_reset_clip(&screen);
        if (flip3d_paint(&screen)) {
            taskbar_paint(&screen);
            gfx_present(CURSOR_ARROW, wm_mouse_x, wm_mouse_y);
            wm_need_redraw = false;
            return;
        }
    }
    canvas_reset_clip(&screen);
    if (clip) canvas_push_clip(&screen, *clip);
    desktop_paint(&screen);
    for (int i = 0; i < nwin; i++) {
        window_t *w = wins[i];
        effects_before(&screen, w->id);
        if (w->flags & WF_HIDDEN) { effect_cancel_window(w); continue; }
        if (w->opening && w->state != WS_MINIMIZED) {
            w->opening = false;
            effect_begin(w, FX_OPEN, R(w->x + w->w / 2, w->y + w->h / 2, 2, 2), R(w->x, w->y, w->w, w->h), false);
        }
        window_effect_t *e = effect_for(w);
        if (e) effect_paint(&screen, e, w);
        else if (w->state != WS_MINIMIZED) paint_window_at(&screen, w, w->x, w->y);
    }
    effects_before(&screen, 0);
    taskbar_paint(&screen);
    /* a full-screen topmost popup (a video or visualisation shown full screen) covers the taskbar too */
    for (int i = 0; i < nwin; i++) {
        window_t *w = wins[i];
        if ((w->flags & (WF_POPUP | WF_TOPMOST)) == (WF_POPUP | WF_TOPMOST) && !(w->flags & WF_HIDDEN) &&
            w->x <= 0 && w->y <= 0 && w->w >= SCREEN_W && w->h >= SCREEN_H) paint_window_at(&screen, w, w->x, w->y);
    }
    shell_paint_overlays(&screen);
    popup_paint(&screen);
    paint_file_drag(&screen);
    canvas_reset_clip(&screen);
    if (clip) gfx_present_region(current_cursor(), wm_mouse_x, wm_mouse_y, *clip);
    else gfx_present(current_cursor(), wm_mouse_x, wm_mouse_y);
    wm_need_redraw = false;
}

/* ---------------- input ---------------- */
static void deliver_mouse(window_t *w, int type, int sx, int sy, int btn) {
    rect_t cr = wm_client_rect(w);
    int lx = sx - cr.x, ly = sy - cr.y;
    ui_t *u = &w->ui;
    u->mx = lx; u->my = ly;
    u->in_window = pt_in(R(0, 0, cr.w, cr.h), lx, ly);
    bool sync = false;
    if (type == ME_DOWN && btn == MB_LEFT) { u->down = true; u->pressed = true; sync = true; }
    if (type == ME_DOWN && btn == MB_RIGHT) { u->rpressed = true; sync = true; }
    if (type == ME_UP && btn == MB_LEFT) { u->down = false; u->released = true; sync = true; }
    if (type == ME_DBLCLICK) { u->dblclick = true; }
    if (type == ME_WHEEL) { u->wheel = btn; sync = true; }
    if (type == ME_LEAVE) { u->in_window = false; u->mx = -10000; u->my = -10000; }
    if (w->mouse) w->mouse(w, type, lx, ly, btn);
    if (!wm_alive(w)) return;
    if (sync || type == ME_DBLCLICK) run_paint(w);
    else w->dirty = true;
    u->pressed = u->released = u->rpressed = u->dblclick = false; u->wheel = 0;
}
static window_t *hover_win;
/* ---- drag and drop of files ---- */
static struct { char *paths; int icon, count; } fdrag;
#ifdef __x86_64__
extern bool lxwin_drop(window_t *w, int x, int y, const char *paths);   /* Linux programs' windows (64-bit edition) */
#else
static bool lxwin_drop(window_t *w, int x, int y, const char *paths) { (void)w; (void)x; (void)y; (void)paths; return false; }
#endif
void wm_drag_files(const char *paths, int icon) {
    if (!paths || !*paths) return;
    kfree(fdrag.paths); fdrag.paths = strdup(paths);
    if (!fdrag.paths) return;
    fdrag.icon = icon; fdrag.count = 0;
    for (const char *p = paths; *p; p++) if (*p == '\n' && p[1]) fdrag.count++;
    fdrag.count++;
    capture = NULL;                            /* the drag has the mouse now */
    drag.mode = DRAG_FILES;
    wm_need_redraw = true;
}
static void files_dropped(int mx, int my) {
    bool copy = (keyboard_mods() & MOD_CTRL) != 0;
    window_t *w = window_at(mx, my);
    if (w) {
        rect_t cr = wm_client_rect(w);
        if (!pt_in(cr, mx, my)) return;
        int lx = mx - cr.x, ly = my - cr.y;
        bool ok = w->drop ? w->drop(w, lx, ly, fdrag.paths, copy) : lxwin_drop(w, lx, ly, fdrag.paths);
        if (ok && wm_alive(w)) { wm_focus(w); w->dirty = true; }
    } else if (my < SCREEN_H - T.taskbar_h) desktop_drop(mx, my, fdrag.paths, copy);
}
static void paint_file_drag(canvas_t *c) {
    if (drag.mode != DRAG_FILES || !fdrag.paths) return;
    int x = wm_mouse_x + 4, y = wm_mouse_y + 10;
    draw_icon_tinted(c, fdrag.icon, 32, x, y, T.select, 70);
    if (fdrag.count > 1) {
        char b[12]; snprintf(b, sizeof b, "%d", fdrag.count);
        int tw = text_width(FONT_UI_BOLD, b) + 8;
        fill_round_rect(c, x + 26, y - 4, tw, 16, 7, T.select);
        draw_text(c, FONT_UI_BOLD, x + 30, y - 3, b, T.select_text);
    }
    window_t *w = window_at(wm_mouse_x, wm_mouse_y);
    const char *hint = (keyboard_mods() & MOD_CTRL) ? "Copy" : w ? NULL : "Move";
    if (hint) { int tw = text_width(FONT_UI, hint) + 8; fill_rect(c, x + 34, y + 20, tw, 15, T.window_bg); draw_rect(c, x + 34, y + 20, tw, 15, T.text); draw_text(c, FONT_UI, x + 38, y + 21, hint, T.text); }
}
static void handle_mouse(mouse_state_t *ms) {
    int mx = ms->x, my = ms->y;
    bool moved = mx != wm_mouse_x || my != wm_mouse_y;
    wm_mouse_x = mx; wm_mouse_y = my;
    int changed = ms->buttons ^ prev_buttons;
    int pressed = changed & ms->buttons, released = changed & prev_buttons;
    prev_buttons = ms->buttons;
    if (flip3d_active()) { flip3d_mouse(mx, my, pressed, ms->wheel); if (moved) wm_need_redraw = true; return; }

    /* popup menus are modal */
    if (pm.open) {
        int h = popup_hit(mx, my);
        if (moved) {
            if (h >= -1 && h != pm.hot) { pm.hot = h; wm_need_redraw = true; }
            if (pm.menubar_win) { int mi = menubar_hit(pm.menubar_win, mx, my); if (mi >= 0 && mi != pm.menubar_idx) open_menubar(pm.menubar_win, mi); }
        }
        if (released & MB_LEFT) { if (h >= 0) popup_activate(h); }
        if (pressed) {
            if (h == -2) {
                window_t *mw = pm.menubar_win;
                int mi = mw ? menubar_hit(mw, mx, my) : -1;
                popup_close();
                if (mw && mi >= 0) return;
                if (!(pressed & MB_LEFT)) return;
                /* fall through so the click also lands */
            } else return;
        } else return;
    }

    /* active drag */
    if (drag.mode == DRAG_MOVE || drag.mode == DRAG_RESIZE) {
        window_t *w = drag.w;
        if (moved) {
            int dx = mx - drag.mx, dy = my - drag.my;
            if (drag.mode == DRAG_MOVE) {
                if (w->state == WS_MAXIMIZED) {
                    /* drag a maximised window: restore under the cursor */
                    int rw = w->restore.w; wm_maximize(w, false); effect_cancel_window(w);
                    w->x = mx - rw / 2; w->y = my - 10; drag.start = R(w->x, w->y, w->w, w->h); drag.mx = mx; drag.my = my;
                } else { w->x = drag.start.x + dx; w->y = MAX(0, MIN(drag.start.y + dy, SCREEN_H - T.taskbar_h - 10)); }
            } else {
                rect_t s = drag.start; int nx = s.x, ny = s.y, nw = s.w, nh = s.h;
                if (drag.edges & 2) nw = s.w + dx;
                if (drag.edges & 8) nh = s.h + dy;
                if (drag.edges & 1) { nw = s.w - dx; nx = s.x + dx; }
                if (drag.edges & 4) { nh = s.h - dy; ny = s.y + dy; }
                if (nw < w->min_w) { if (drag.edges & 1) nx -= w->min_w - nw; nw = w->min_w; }
                if (nh < w->min_h) { if (drag.edges & 4) ny -= w->min_h - nh; nh = w->min_h; }
                w->x = nx; w->y = ny; w->w = nw; w->h = nh;
                sync_surface(w);
            }
            wm_need_redraw = true;
        }
        if (released & MB_LEFT) drag.mode = DRAG_NONE;
        return;
    }
    if (drag.mode == DRAG_FILES) {
        if (moved) wm_need_redraw = true;
        if (pressed & MB_RIGHT) { drag.mode = DRAG_NONE; wm_need_redraw = true; return; }   /* right-click cancels */
        if (released & MB_LEFT) {
            drag.mode = DRAG_NONE;
            files_dropped(mx, my);
            kfree(fdrag.paths); fdrag.paths = NULL;
            wm_need_redraw = true;
        }
        return;
    }
    if (drag.mode == DRAG_CAPBTN) {
        window_t *w = drag.w;
        int b = cap_btn_at(w, mx, my);
        int nh = b == drag.btn ? b : -1;
        if (cap_hot.btn != nh) { cap_hot.w = w; cap_hot.btn = nh; wm_need_redraw = true; }
        if (released & MB_LEFT) {
            drag.mode = DRAG_NONE;
            if (b == drag.btn) {
                if (b == 3) wm_close(w);
                else if (b == 1) wm_maximize(w, w->state != WS_MAXIMIZED);
                else if (b == 0) wm_minimize(w);
            }
            cap_hot.w = NULL; wm_need_redraw = true;
        }
        return;
    }
    /* client capture */
    if (capture) {
        window_t *w = capture;
        if (moved) deliver_mouse(w, ME_MOVE, mx, my, 0);
        if (released & capture_btn) { capture = NULL; drag.mode = DRAG_NONE; if (wm_alive(w)) deliver_mouse(w, ME_UP, mx, my, released & capture_btn); }
        if (ms->wheel && wm_alive(w)) deliver_mouse(w, ME_WHEEL, mx, my, ms->wheel);
        return;
    }

    bool full_up = false;                      /* a full-screen popup takes the taskbar's place too */
    for (int i = 0; i < nwin; i++) if ((wins[i]->flags & (WF_POPUP | WF_TOPMOST)) == (WF_POPUP | WF_TOPMOST) && !(wins[i]->flags & WF_HIDDEN) &&
                                       wins[i]->w >= SCREEN_W && wins[i]->h >= SCREEN_H) full_up = true;
    if (!full_up && shell_mouse(moved && !pressed && !released ? ME_MOVE : pressed ? ME_DOWN : released ? ME_UP : ME_MOVE, mx, my, pressed ? pressed : released)) {
        if (hover_win && wm_alive(hover_win)) { deliver_mouse(hover_win, ME_LEAVE, mx, my, 0); hover_win = NULL; }
        return;
    }
    if ((pressed || released) && my < SCREEN_H - T.taskbar_h && effect_mouse(mx, my)) return;

    window_t *w = window_at(mx, my);
    if (w && effect_for(w)) return;       /* hover waits for the visual to settle */
    /* hover tracking */
    if (hover_win != w) {
        if (hover_win && wm_alive(hover_win)) deliver_mouse(hover_win, ME_LEAVE, mx, my, 0);
        hover_win = w;
    }
    int e = 0, b = -1;
    int ht = w ? hit_test(w, mx, my, &e, &b) : HT_NONE;
    /* caption button hover */
    int nb = ht == HT_BTN ? b : -1;
    if ((cap_hot.w != (ht == HT_BTN ? w : NULL)) || cap_hot.btn != nb) { cap_hot.w = ht == HT_BTN ? w : NULL; cap_hot.btn = nb; wm_need_redraw = true; }

    if (!w) {
        if (pressed || released || ms->wheel) desktop_mouse(pressed ? ME_DOWN : released ? ME_UP : ME_WHEEL, mx, my, pressed ? pressed : released);
        if (pressed) {
            bool dbl = (pressed & MB_LEFT) && timer_ticks() - last_click_time < wm_dblclick_ms && ABSDIFF(mx, last_click_x) < 5 && ABSDIFF(my, last_click_y) < 5 && last_click_btn == MB_LEFT;
            if (dbl) { desktop_mouse(ME_DBLCLICK, mx, my, MB_LEFT); last_click_time = 0; }
            else { last_click_time = timer_ticks(); last_click_x = mx; last_click_y = my; last_click_btn = pressed & MB_LEFT ? MB_LEFT : MB_RIGHT; }
        } else if (moved) desktop_mouse(ME_MOVE, mx, my, ms->buttons);
        return;
    }
    if (ht == HT_CLIENT) {
        if (moved && !pressed) deliver_mouse(w, ME_MOVE, mx, my, ms->buttons);
        if (ms->wheel) deliver_mouse(w, ME_WHEEL, mx, my, ms->wheel);
    }
    if (pressed) {
        window_t *m = modal_child(w);
        if (m) { wm_focus(m); speaker_beep(0, 0); return; }
        if (w != wm_active) wm_focus(w);
        bool dbl = timer_ticks() - last_click_time < wm_dblclick_ms && ABSDIFF(mx, last_click_x) < 5 && ABSDIFF(my, last_click_y) < 5 && last_click_btn == (pressed & MB_LEFT ? MB_LEFT : MB_RIGHT);
        if (dbl) last_click_time = 0; else { last_click_time = timer_ticks(); last_click_x = mx; last_click_y = my; last_click_btn = pressed & MB_LEFT ? MB_LEFT : MB_RIGHT; }
        if (pressed & MB_LEFT) {
            switch (ht) {
            case HT_TITLE:
                if (dbl && !(w->flags & WF_NO_MAX) && (w->flags & WF_RESIZABLE)) { wm_maximize(w, w->state != WS_MAXIMIZED); break; }
                drag.mode = DRAG_MOVE; drag.w = w; drag.mx = mx; drag.my = my; drag.start = R(w->x, w->y, w->w, w->h); break;
            case HT_EDGE:
                drag.mode = DRAG_RESIZE; drag.w = w; drag.edges = e; drag.mx = mx; drag.my = my; drag.start = R(w->x, w->y, w->w, w->h); break;
            case HT_BTN:
                drag.mode = DRAG_CAPBTN; drag.w = w; drag.btn = b; cap_hot.w = w; cap_hot.btn = b; wm_need_redraw = true; break;
            case HT_MENUBAR: { int mi = menubar_hit(w, mx, my); if (mi >= 0) open_menubar(w, mi); break; }
            case HT_CLIENT:
                capture = w; capture_btn = MB_LEFT; drag.mode = DRAG_CLIENT;
                deliver_mouse(w, ME_DOWN, mx, my, MB_LEFT);
                if (dbl && wm_alive(w)) deliver_mouse(w, ME_DBLCLICK, mx, my, MB_LEFT);
                break;
            }
        } else if (pressed & MB_RIGHT) {
            if (ht == HT_CLIENT) { capture = w; capture_btn = MB_RIGHT; drag.mode = DRAG_CLIENT; deliver_mouse(w, ME_DOWN, mx, my, MB_RIGHT); }
            else if (ht == HT_TITLE) {
                static const menu_item_t sysm[] = { { "Restore", 1, 0 }, { "Minimize", 2, 0 }, { "Maximize", 3, 0 }, { "-", 0, 0 }, { "Close\tAlt+F4", 4, 0 } };
                extern void wm_sysmenu_cb(void *ctx, int id);
                popup_menu(mx, my, sysm, 5, wm_sysmenu_cb, w);
            }
        }
    } else if (released && ht == HT_CLIENT) {
        deliver_mouse(w, ME_UP, mx, my, released);
    }
    if (moved && !pressed && !released) wm_need_redraw = wm_need_redraw || false;
}
void wm_sysmenu_cb(void *ctx, int id) {
    window_t *w = ctx;
    if (!wm_alive(w)) return;
    if (id == 1) wm_maximize(w, false);
    else if (id == 2) wm_minimize(w);
    else if (id == 3 && !(w->flags & WF_NO_MAX)) wm_maximize(w, true);
    else if (id == 4) wm_close(w);
}

static void cycle_windows(void) {
    if (nwin < 2) { if (nwin == 1) wm_focus(wins[0]); return; }
    /* bring the bottom-most normal window to the front */
    for (int i = 0; i < nwin; i++) {
        window_t *w = wins[i];
        if (w != wm_active && !(w->flags & WF_HIDDEN)) { wm_focus(w); return; }
    }
}
extern void app_launch(const char *name, const char *arg);
extern bool mousekeys_key(key_event_t *e);
static void handle_key(key_event_t *e) {
    /* the Alt-tap state (see below), also used to close an open menu with Alt */
    static bool alt_alone, alt_held;
    static uint32_t alt_down_at;
    if (mousekeys_key(e)) return;                       /* the keypad moves the pointer (Mouse Keys) */
    if (flip3d_active()) { flip3d_key(e); return; }
    if (e->pressed && wm_active) effect_cancel_window(wm_active);
    if (e->pressed) {
        if (e->key == KEY_F12 && (e->mods & MOD_CTRL) && (e->mods & MOD_ALT) && (e->mods & MOD_SHIFT)) { extern void task_dump_stacks(void); task_dump_stacks(); return; }
        if ((e->key == KEY_DELETE && (e->mods & MOD_CTRL) && (e->mods & MOD_ALT)) || (e->key == KEY_ESC && (e->mods & MOD_CTRL) && (e->mods & MOD_SHIFT))) { app_launch("taskmgr", NULL); return; }
        if (e->key == KEY_TAB && (e->mods & MOD_ALT)) {
            /* with 3D acceleration on: Flip 3D (Ctrl+Alt+Tab keeps it open) */
            if (!boot_safe_mode && flip3d_start(wins, nwin, wm_active, (e->mods & MOD_CTRL) != 0)) return;
            cycle_windows(); return;
        }
        if (e->key == KEY_F4 && (e->mods & MOD_ALT)) { if (wm_active) wm_close(wm_active); else shell_show_shutdown(); return; }
        if (pm.open) {
            if (e->key == KEY_ESC) { popup_close(); return; }
            /* a menu-bar menu: Left and Right move to the neighbouring menu */
            if ((e->key == KEY_LEFT || e->key == KEY_RIGHT) && pm.menubar_win && wm_alive(pm.menubar_win) && pm.menubar_win->nmenus > 1) {
                window_t *mw = pm.menubar_win;
                int idx = (pm.menubar_idx + (e->key == KEY_RIGHT ? 1 : mw->nmenus - 1)) % mw->nmenus;
                popup_close();
                open_menubar(mw, idx);
                pm.hot = 0; wm_need_redraw = true; return;
            }
            if (e->key == KEY_DOWN || e->key == KEY_UP) {
                int d = e->key == KEY_DOWN ? 1 : -1;
                for (int k = 0; k < pm.n; k++) { pm.hot = (pm.hot + d + pm.n) % pm.n; if (pm.items[pm.hot].label[0] != '-') break; }
                wm_need_redraw = true; return;
            }
            if (e->key == KEY_ENTER) { popup_activate(pm.hot); return; }
            /* Alt or F10 closes the menu, as on Windows (the Alt's release must not reopen it) */
            if (e->key == KEY_LALT || e->key == KEY_F10) { popup_close(); alt_held = true; alt_alone = false; wm_need_redraw = true; return; }
            /* a letter picks the next item starting with it; other typing closes the menu and
             * goes on to the window, so the keyboard never seems dead behind an open menu */
            if (e->ch > ' ' && !(e->mods & (MOD_CTRL | MOD_ALT))) {
                char want = e->ch >= 'a' && e->ch <= 'z' ? e->ch - 32 : e->ch;
                for (int k = 1; k <= pm.n; k++) {
                    int i = (pm.hot + k + pm.n) % pm.n;
                    char c = pm.items[i].label[0]; if (c >= 'a' && c <= 'z') c -= 32;
                    if (c == want && pm.items[i].label[0] != '-' && !(pm.items[i].flags & MI_DISABLED)) { pm.hot = i; popup_activate(i); return; }
                }
            }
            if (e->ch || e->key == KEY_BACKSPACE || e->key == KEY_DELETE || e->key == KEY_TAB) { popup_close(); wm_need_redraw = true; }
            else return;
        }
    }
    /* Alt pressed and let go on its own, or F10: the active window's menu bar, first menu */
    /* Only a quick tap counts, as on Windows: not with Ctrl, Shift or the Win key held (a VM's
     * Ctrl+Alt "release the mouse" chord used to open the File menu), not a release with no
     * press seen, and not an Alt held down for more than a second. */
    if (e->key == KEY_LALT) {
        if (e->pressed) {
            if (!alt_held) { alt_held = true; alt_alone = !((e->mods | keyboard_mods()) & (MOD_CTRL | MOD_SHIFT)); alt_down_at = timer_ticks(); }
            return;
        }
        bool tap = alt_held && alt_alone && timer_ticks() - alt_down_at < 1000 && !(keyboard_mods() & (MOD_CTRL | MOD_SHIFT));
        alt_alone = alt_held = false;
        if (tap && wm_active && wm_active->nmenus && !pm.open) { open_menubar(wm_active, 0); pm.hot = 0; wm_need_redraw = true; return; }
    } else if (e->pressed || e->key == KEY_LCTRL || e->key == KEY_LSHIFT || e->key == KEY_RSHIFT || e->key == KEY_WIN) alt_alone = false;
    if (e->pressed && e->key == KEY_F10 && !(e->mods & (MOD_SHIFT | MOD_CTRL | MOD_ALT)) && wm_active && wm_active->nmenus && !pm.open) {
        open_menubar(wm_active, 0); pm.hot = 0; wm_need_redraw = true; return;
    }
    if (shell_key(e)) return;
    window_t *w = wm_active;
    if (!w) return;
    if (w->key) w->key(w, e);
    if (!wm_alive(w)) return;
    if (e->pressed) {
        w->ui.key = *e; w->ui.has_key = true;
        run_paint(w);
        bool unused = w->ui.has_key;
        w->ui.has_key = false;
        /* Esc that nothing in the window used closes a dialog, as Cancel would */
        if (unused && e->key == KEY_ESC && !w->key && (w->flags & WF_NO_MIN) && !(w->flags & WF_NOCLOSE) && wm_alive(w)) wm_close(w);
    }
}

/* ---------------- main loop ---------------- */
static void screen_resized(void) {
    effects_cancel();
    rect_t wa = wm_work_area();
    for (int i = 0; i < nwin; i++) {
        window_t *w = wins[i];
        if (w->state == WS_MAXIMIZED) { w->state = WS_NORMAL; wm_maximize(w, true); continue; }
        if (w->x + 60 > wa.w) w->x = MAX(0, wa.w - w->w);
        if (w->y + 30 > wa.h) w->y = MAX(0, wa.h - w->h);
        if (w->w > wa.w) w->w = wa.w;
        if (w->h > wa.h) w->h = wa.h;
        sync_surface(w);
    }
    if (wm_mouse_x >= SCREEN_W) wm_mouse_x = SCREEN_W - 1;
    if (wm_mouse_y >= SCREEN_H) wm_mouse_y = SCREEN_H - 1;
    mouse_set_bounds(SCREEN_W, SCREEN_H);
    wm_effects_changed();
    shell_wallpaper_changed();
    desktop_refresh();
    wm_redraw_all();
}
void wm_init(void) {
    wm_animations = !boot_safe_mode;
    gfx_resized_hook = screen_resized; styles_load(); theme_apply(boot_safe_mode ? THEME_CLASSIC : THEME_LUNA_BLUE, 0); }
bool gfx_ready_for_watchdog = true;
/* ---------------- watchdog ----------------
 * The desktop loop stamps a heartbeat. If it stops for several seconds while
 * timer interrupts still arrive, some task is running without yielding: say
 * which one, in the log and on the screen, so the hang can be reported. */
static volatile bool wd_reported;
static uint32_t wd_eips[4]; static int wd_neips;
#ifdef __x86_64__
extern void shell_restart_trampoline(void);
#endif
void video_update(int x, int y, int w, int h);
static void wd_banner(const char *a, const char *b) {
    static uint32_t pixels[760 * 50];
    canvas_t c; int bw = MIN((int)fb_info.width, 760), bx = ((int)fb_info.width - bw) / 2;
    canvas_init(&c, pixels, bw, 50);
    fill_rect(&c, 0, 0, bw, 50, HEX(0x9C1C1C));
    draw_text(&c, FONT_UI_BOLD, 10, 6, a, WHITE);
    draw_text(&c, FONT_UI, 10, 27, b, WHITE);
    gfx_emergency_present(&c, bx, 0);
}
static void watchdog(regs_t *r, uint32_t now) {
    /* Text-mode Setup runs on the GUI task and deliberately blocks it while it
     * formats or copies files, painting its own progress bar.  That is not a
     * hung desktop, so keep the heartbeat fresh and say nothing. */
    if (setup_text_active) { gui_heartbeat = now; wd_neips = 0; return; }
    uint32_t idle = now - gui_heartbeat;
    /* If the shell task itself is stuck, restart just the shell manager (not
     * the machine).  The timer interrupt returns to a tiny assembly trampoline
     * that switches to a private recovery stack, closes the windows and enters
     * a fresh wm_run loop. */
#ifdef __x86_64__
    if (idle >= 8000 && gui_task && task_current() == gui_task) {
        klog("watchdog: shellmanager.exe is not responding (%u ms); restarting it", idle);
        gui_heartbeat = now; wd_neips = 0; wd_reported = false;
        r->eip = (uint64_t)(uintptr_t)shell_restart_trampoline;
        r->eflags |= 0x200;
        return;
    }
#endif
    if (idle < 4000) {
        if (wd_reported) { klog("watchdog: the desktop is running again after %u ms", idle); wd_reported = false; }
        wd_neips = 0;
        return;
    }
    if (wd_neips < 4) wd_eips[wd_neips++] = r->eip;
    if (wd_reported || wd_neips < 4) return;
    wd_reported = true;
    task_t *t = task_current();
    char a[160], b[200];
    snprintf(a, sizeof a, "The desktop has stopped responding for %u seconds.", idle / 1000);
    snprintf(b, sizeof b, "Busy task: %s (pid %d), code at %08x %08x %08x %08x. Please report this line.",
             t ? t->name : "?", t ? t->pid : 0, wd_eips[0], wd_eips[1], wd_eips[2], wd_eips[3]);
    klog("watchdog: %s %s", a, b);
#ifdef __x86_64__
    { extern bool lx_task_syscall(task_t *t, char *out, int cap); char c[120]; if (lx_task_syscall(t, c, sizeof c)) klog("watchdog: in %s", c); }
#endif
    if (gfx_ready_for_watchdog) wd_banner(a, b);
}
static volatile bool shell_restart_req;
void wm_recover_shell(void) {
    effects_cancel();
    popup_close();
    pm.open = false;
    drag.mode = DRAG_NONE;
    capture = NULL;
    hover_win = NULL;
    cap_hot.w = NULL;
    prev_buttons = 0;
    last_click_time = 0;
    effects_suppressed = true;
    while (nwin > 0) wm_destroy(wins[nwin - 1]);
    effects_suppressed = false;
    wm_need_redraw = true;
}
void shellmanager_restart(void) { klog("shellmanager: restart requested"); shell_restart_req = true; wm_need_redraw = true; }
void shell_recover(void) {
    klog("shellmanager.exe: restarting the shell loop");
    wm_recover_shell();
    shell_reset();
    gui_heartbeat = timer_ticks();
    wm_run();
    for (;;) hlt();
}
static void shellmanager_entry(void *arg) {
    if (arg) { wm_recover_shell(); shell_reset(); }
    wm_run();
}
static int shell_pid;
static void shellwatch_entry(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep(1000);
        if (shell_pid && !task_find(shell_pid)) {
            klog("shellmanager.exe: process exited; starting it again");
            task_t *t = task_create("shellmanager.exe", shellmanager_entry, (void *)1, 512 * 1024);
            if (t) { shell_pid = t->pid; task_set_gui_task(t); }
        }
    }
}
void shellmanager_start(void) {
    task_t *t = task_create("shellmanager.exe", shellmanager_entry, NULL, 512 * 1024);
    if (t) { shell_pid = t->pid; task_set_gui_task(t); }
    task_create("shellwatch", shellwatch_entry, NULL, 32 * 1024);
}

void wm_run(void) {
    mouse_state_t ms;
    uint32_t last_tick = 0;
    gui_heartbeat = timer_ticks();
    watchdog_hook = watchdog;
    for (;;) {
        uint32_t loop_t0 = timer_ticks();
        gui_heartbeat = timer_ticks();
        if (shell_restart_req) {
            klog("shellmanager: restarting windows");
            shell_restart_req = false;
            wm_recover_shell();
            shell_reset();
            gui_heartbeat = timer_ticks();
            wm_need_redraw = true;
        }
        bool activity = false;
        key_event_t ke;
        while (keyboard_poll(&ke)) { handle_key(&ke); activity = true; }
        if (mouse_poll(&ms)) {
            int ob = prev_buttons;
            int ox = wm_mouse_x, oy = wm_mouse_y;
            handle_mouse(&ms);
            activity = true;
            if (!wm_need_redraw && (ox != wm_mouse_x || oy != wm_mouse_y || ob != ms.buttons)) {
                bool any_dirty = false;
                for (int i = 0; i < nwin; i++)
                    if (wins[i]->dirty && wins[i]->state != WS_MINIMIZED && !(wins[i]->flags & WF_HIDDEN))
                        any_dirty = true;
                if (!any_dirty) gfx_move_cursor(current_cursor(), wm_mouse_x, wm_mouse_y);
            }
        }
        uint32_t now = timer_ticks();
        for (int i = 0; i < nwin; i++) {
            window_t *w = wins[i];
            if (w->timer && (int32_t)(now - w->timer_next) >= 0) { w->timer_next = now + w->timer_ms; w->timer(w); }
        }
        for (int i = 0; i < nwin; i++) {
            window_t *w = wins[i];
            if (!(w->flags & WF_NOMEM)) continue;
            static window_t *oom_box;                     /* the message below, while it is open */
            w->flags &= ~WF_NOMEM;
            char msg[220];
            snprintf(msg, sizeof msg, "There is not enough memory to show \"%s\". Close some programs or windows and try again.", w->title);
            klog("wm: not enough memory for window '%s'; closing it", w->title);
            bool is_box = w == oom_box;
            if (is_box) { oom_box = NULL; wm_destroy(w); break; }     /* the message itself did not fit */
            wm_close(w);
            if (wm_alive(w)) wm_destroy(w);
            if (!oom_box || !wm_alive(oom_box)) oom_box = msgbox("Not enough memory", msg, MBI_ERROR, MBX_OK, NULL, NULL);
            break;
        }
        while (defn) { defer_t d = defq[0]; memmove(defq, defq + 1, (--defn) * sizeof(defer_t)); d.fn(d.arg); }
        now = timer_ticks();
        effects_tick(now);
        if (flip3d_animating()) wm_need_redraw = true;
        shell_animate(now);
        if (now - last_tick >= 250) { last_tick = now; shell_tick(); }
        for (int i = 0; i < nwin; i++) {
            window_t *w = wins[i];
            if (!w->dirty) continue;
            /* A minimized or hidden window is never painted, so its dirty flag
             * is cleared rather than left set: a flag that stays set keeps the
             * pointer from being moved without a full repaint. */
            if (w->state == WS_MINIMIZED || (w->flags & WF_HIDDEN)) { w->dirty = false; continue; }
            run_paint(w);
        }
        if (ndying) reap_windows();
        if (wm_need_redraw || dmg_on) {
            uint64_t t0 = gui_profile ? rdtsc() : 0;
            bool part = !wm_need_redraw;
            rect_t r = dmg;
            dmg_on = false;
            compose_clipped(part ? &r : NULL);
            if (gui_profile) { prof_compose_t += rdtsc() - t0; prof_compose_n++; }
        }
        prof_report();
        if ((int32_t)(timer_ticks() - loop_t0) >= 150)
            usb_diag("gui: %u ms in one frame", timer_ticks() - loop_t0);
        task_yield();
        if (!activity && !wm_need_redraw) {
            extern volatile bool cpu_idle_flag;
            cpu_idle_flag = true; hlt(); cpu_idle_flag = false;
        }
    }
}
