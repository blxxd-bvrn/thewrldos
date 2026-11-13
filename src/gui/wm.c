/* Window manager: z-order, decorations, compositing, input routing */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
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
static int prev_buttons;
static window_t *capture;
static int capture_btn;
static window_t *dying[MAX_WIN];
static int ndying;

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CAPBTN, DRAG_CLIENT };
static struct { int mode; window_t *w; int edges; int mx, my; rect_t start; int btn; } drag;
static struct { window_t *w; int btn; } cap_hot;
static uint32_t last_click_time; static int last_click_x, last_click_y, last_click_btn;

/* deferred calls */
typedef struct { wm_deferred_fn fn; void *arg; } defer_t;
static defer_t defq[64]; static int defn;
void wm_defer(wm_deferred_fn fn, void *arg) { if (defn < 64) { defq[defn].fn = fn; defq[defn].arg = arg; defn++; } }

/* ---------------- geometry ---------------- */
static int menubar_h(window_t *w) { return w->nmenus ? T.menubar_h : 0; }
static bool framed(window_t *w) { return !(w->flags & WF_POPUP); }
rect_t wm_client_rect(window_t *w) {
    if (!framed(w)) return R(w->x, w->y, w->w, w->h);
    int b = T.border, th = T.title_h;
    if (T.classic) return R(w->x + b, w->y + th + menubar_h(w), w->w - 2 * b, w->h - th - b - menubar_h(w));
    return R(w->x + b, w->y + th + menubar_h(w), w->w - 2 * b, w->h - th - b - menubar_h(w));
}
rect_t wm_work_area(void) { return R(0, 0, SCREEN_W, SCREEN_H - T.taskbar_h); }
static void sync_surface(window_t *w) {
    rect_t c = wm_client_rect(w);
    if (!w->surf) w->surf = canvas_new(MAX(1, c.w), MAX(1, c.h));
    else if (w->surf->w != c.w || w->surf->h != c.h) {
        canvas_resize(w->surf, MAX(1, c.w), MAX(1, c.h));
        if (w->resized) w->resized(w);
    }
    w->dirty = true;
}
static void frame_size_for_client(int cw, int ch, int flags, int nmenus, int *ow, int *oh) {
    if (flags & WF_POPUP) { *ow = cw; *oh = ch; return; }
    *ow = cw + 2 * T.border;
    *oh = ch + T.title_h + T.border + (nmenus ? T.menubar_h : 0);
}
void wm_resize_client(window_t *w, int cw, int ch) {
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
    if (m) { if (w->state == WS_MINIMIZED) w->state = WS_NORMAL; raise(w); w = m; }
    if (w->state == WS_MINIMIZED) { w->state = w->restore.w && (w->flags & WF_RESIZABLE) && w->anim == 2 ? WS_MAXIMIZED : WS_NORMAL; w->anim = 0; w->dirty = true; }
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
    task_t *t = task_current();
    w->pid = t ? t->pid : 0;
    w->ui.focus = 0;
    wins[nwin++] = w;
    raise(w);
    sync_surface(w);
    if (!(flags & WF_HIDDEN)) set_active(w);
    wm_need_redraw = true;
    return w;
}
void wm_show(window_t *w) { w->flags &= ~WF_HIDDEN; wm_focus(w); }
void wm_destroy(window_t *w) {
    int i = index_of(w);
    if (i < 0) return;
    for (int j = 0; j < ndying; j++) if (dying[j] == w) return;
    /* destroy owned windows too */
    for (int k = nwin - 1; k >= 0; k--) if (wins[k]->owner == w) wm_destroy(wins[k]);
    i = index_of(w);
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
    wm_destroy(w);
}
void wm_minimize(window_t *w) {
    if (w->flags & WF_NO_MIN) return;
    w->anim = w->state == WS_MAXIMIZED ? 2 : 1;
    w->state = WS_MINIMIZED;
    w->dirty = false;                 /* a minimized window is not painted */
    if (wm_active == w) { wm_active = NULL; focus_topmost(); }
    wm_need_redraw = true;
}
void wm_maximize(window_t *w, bool on) {
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
    sync_surface(w); wm_need_redraw = true;
}
void wm_set_title(window_t *w, const char *t) { strlcpy(w->title, t, sizeof w->title); wm_need_redraw = true; }
void wm_invalidate(window_t *w) { if (w) w->dirty = true; }
void wm_redraw_all(void) { for (int i = 0; i < nwin; i++) wins[i]->dirty = true; wm_need_redraw = true; }
void wm_set_timer(window_t *w, uint32_t ms, void (*fn)(window_t *)) { w->timer = fn; w->timer_ms = ms; w->timer_next = timer_ticks() + ms; }
void wm_theme_changed(void) {
    for (int i = 0; i < nwin; i++) {
        window_t *w = wins[i];
        if (w->state == WS_MAXIMIZED) { rect_t r = w->restore; wm_maximize(w, true); w->restore = r; }
        sync_surface(w);
    }
    shell_wallpaper_changed();
    wm_need_redraw = true;
}

/* ---------------- popup menus ---------------- */
static struct {
    bool open; int x, y, w, h; menu_item_t items[24]; int n; int hot;
    popup_cb cb; void *ctx; window_t *menubar_win; int menubar_idx;
} pm;
#define PM_ITEM_H 22
#define PM_SEP_H 8
static int pm_item_y(int i) { int y = 3; for (int k = 0; k < i; k++) y += pm.items[k].label[0] == '-' ? PM_SEP_H : PM_ITEM_H; return y; }
void popup_menu(int x, int y, const menu_item_t *items, int n, popup_cb cb, void *ctx) {
    pm.n = MIN(n, 24);
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
    if (y + h > SCREEN_H) y = MAX(0, y - h);
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
        if (it->label[0] == '-') { hline(c, pm.x + (T.classic ? 2 : 1), iy + 3, pm.w - (T.classic ? 4 : 2), HEX(0xACA899)); if (T.classic) hline(c, pm.x + 2, iy + 4, pm.w - 4, WHITE); continue; }
        bool hot = i == pm.hot && !(it->flags & MI_DISABLED);
        if (hot) fill_rect(c, pm.x + (T.classic ? 3 : 2), iy, pm.w - (T.classic ? 6 : 4), PM_ITEM_H, T.hot);
        uint32_t col = (it->flags & MI_DISABLED) ? HEX(0xACA899) : hot ? T.hot_text : T.text;
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
        if (pt_in(R(w->x, w->y, w->w, w->h), sx, sy)) return w;
    }
    return NULL;
}
bool wm_window_at_point(int x, int y) { return window_at(x, y) != NULL; }

/* ---------------- painting ---------------- */
static void run_paint(window_t *w) {
    if (!w->paint) { w->dirty = false; return; }
    sync_surface(w);              /* e.g. a menu bar was attached after the window was created */
    canvas_reset_clip(w->surf);
    ui_begin(w);
    w->paint(w);
    ui_end(w);
    w->dirty = false;
    wm_need_redraw = true;
}
static void paint_menubar(canvas_t *c, window_t *w) {
    rect_t cr = wm_client_rect(w);
    int y = cr.y - T.menubar_h;
    fill_rect(c, cr.x, y, cr.w, T.menubar_h, T.classic ? T.face : T.face);
    if (!T.classic) hline(c, cr.x, y + T.menubar_h - 1, cr.w, HEX(0xD8D2BD));
    for (int i = 0; i < w->nmenus; i++) {
        int x = cr.x + menubar_item_x(w, i);
        int tw = text_width(FONT_UI, w->menus[i].title);
        bool open = pm.open && pm.menubar_win == w && pm.menubar_idx == i;
        if (open) fill_rect(c, x - 6, y + 1, tw + 12, T.menubar_h - 2, T.classic ? T.face : T.hot);
        if (open && T.classic) bevel(c, x - 6, y + 1, tw + 12, T.menubar_h - 2, false);
        draw_text(c, FONT_UI, x, y + (T.menubar_h - font_height(FONT_UI)) / 2, w->menus[i].title, open && !T.classic ? WHITE : T.text);
    }
}
static void paint_window(canvas_t *c, window_t *w) {
    if (framed(w)) {
        int hot = cap_hot.w == w ? cap_hot.btn : -1;
        int pressed = (drag.mode == DRAG_CAPBTN && drag.w == w && hot == drag.btn) ? drag.btn : -1;
        draw_window_frame(c, R(w->x, w->y, w->w, w->h), w->title, w->icon, w == wm_active, w->flags, hot, pressed, w->state);
        if (w->nmenus) paint_menubar(c, w);
    }
    rect_t cr = wm_client_rect(w);
    blit(c, cr.x, cr.y, w->surf, 0, 0, cr.w, cr.h);
}
static int current_cursor(void) {
    if (drag.mode == DRAG_RESIZE) return edge_cursor(drag.edges);
    if (drag.mode == DRAG_CLIENT && capture) return capture->ui.cursor ? capture->ui.cursor : capture->cursor;
    if (pm.open) return CURSOR_ARROW;
    int sc = shell_cursor(wm_mouse_x, wm_mouse_y);
    if (sc >= 0) return sc;
    window_t *w = window_at(wm_mouse_x, wm_mouse_y);
    if (!w) return CURSOR_ARROW;
    int e = 0, b = -1;
    int ht = hit_test(w, wm_mouse_x, wm_mouse_y, &e, &b);
    if (ht == HT_EDGE) return edge_cursor(e);
    if (ht == HT_CLIENT) return w->ui.cursor ? w->ui.cursor : w->cursor;
    return CURSOR_ARROW;
}
static void compose(void) {
    gui_composes++;
    canvas_reset_clip(&screen);
    desktop_paint(&screen);
    for (int i = 0; i < nwin; i++) {
        window_t *w = wins[i];
        if (w->state == WS_MINIMIZED || (w->flags & WF_HIDDEN)) continue;
        paint_window(&screen, w);
    }
    taskbar_paint(&screen);
    shell_paint_overlays(&screen);
    popup_paint(&screen);
    gfx_present(current_cursor(), wm_mouse_x, wm_mouse_y);
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
static void handle_mouse(mouse_state_t *ms) {
    int mx = ms->x, my = ms->y;
    bool moved = mx != wm_mouse_x || my != wm_mouse_y;
    wm_mouse_x = mx; wm_mouse_y = my;
    int changed = ms->buttons ^ prev_buttons;
    int pressed = changed & ms->buttons, released = changed & prev_buttons;
    prev_buttons = ms->buttons;

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
                    int rw = w->restore.w; wm_maximize(w, false);
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

    if (shell_mouse(moved && !pressed && !released ? ME_MOVE : pressed ? ME_DOWN : released ? ME_UP : ME_MOVE, mx, my, pressed ? pressed : released)) {
        if (hover_win && wm_alive(hover_win)) { deliver_mouse(hover_win, ME_LEAVE, mx, my, 0); hover_win = NULL; }
        return;
    }

    window_t *w = window_at(mx, my);
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
static void handle_key(key_event_t *e) {
    if (e->pressed) {
        if ((e->key == KEY_DELETE && (e->mods & MOD_CTRL) && (e->mods & MOD_ALT)) || (e->key == KEY_ESC && (e->mods & MOD_CTRL) && (e->mods & MOD_SHIFT))) { app_launch("taskmgr", NULL); return; }
        if (e->key == KEY_TAB && (e->mods & MOD_ALT)) { cycle_windows(); return; }
        if (e->key == KEY_F4 && (e->mods & MOD_ALT)) { if (wm_active) wm_close(wm_active); else shell_show_shutdown(); return; }
        if (pm.open) {
            if (e->key == KEY_ESC) { popup_close(); return; }
            if (e->key == KEY_DOWN || e->key == KEY_UP) {
                int d = e->key == KEY_DOWN ? 1 : -1;
                for (int k = 0; k < pm.n; k++) { pm.hot = (pm.hot + d + pm.n) % pm.n; if (pm.items[pm.hot].label[0] != '-') break; }
                wm_need_redraw = true; return;
            }
            if (e->key == KEY_ENTER) { popup_activate(pm.hot); return; }
            return;
        }
    }
    if (shell_key(e)) return;
    window_t *w = wm_active;
    if (!w) return;
    if (w->key) w->key(w, e);
    if (!wm_alive(w)) return;
    if (e->pressed) {
        w->ui.key = *e; w->ui.has_key = true;
        run_paint(w);
        w->ui.has_key = false;
    }
}

/* ---------------- main loop ---------------- */
static void screen_resized(void) {
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
    shell_wallpaper_changed();
    desktop_refresh();
    wm_redraw_all();
}
void wm_init(void) {
    gfx_resized_hook = screen_resized; styles_load(); theme_apply(boot_safe_mode ? THEME_CLASSIC : THEME_LUNA_BLUE, 0); }
bool gfx_ready_for_watchdog = true;
/* ---------------- watchdog ----------------
 * The desktop loop stamps a heartbeat. If it stops for several seconds while
 * timer interrupts still arrive, some task is running without yielding: say
 * which one, in the log and on the screen, so the hang can be reported. */
static volatile bool wd_reported;
static uint32_t wd_eips[4]; static int wd_neips;
void video_update(int x, int y, int w, int h);
static void wd_banner(const char *a, const char *b) {
    canvas_t c; c.px = (uint32_t *)(uintptr_t)fb_info.addr; c.w = fb_info.width; c.h = fb_info.height;
    c.stride = fb_info.pitch / 4; c.clip = R(0, 0, c.w, c.h);
    int bw = MIN(c.w, 760), bx = (c.w - bw) / 2;
    fill_rect(&c, bx, 0, bw, 50, HEX(0x9C1C1C));
    draw_text(&c, FONT_UI_BOLD, bx + 10, 6, a, WHITE);
    draw_text(&c, FONT_UI, bx + 10, 27, b, WHITE);
    video_update(bx, 0, bw, 50);
}
static void watchdog(regs_t *r, uint32_t now) {
    /* Text-mode Setup runs on the GUI task and deliberately blocks it while it
     * formats or copies files, painting its own progress bar.  That is not a
     * hung desktop, so keep the heartbeat fresh and say nothing. */
    if (setup_text_active) { gui_heartbeat = now; wd_neips = 0; return; }
    uint32_t idle = now - gui_heartbeat;
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
    if (gfx_ready_for_watchdog) wd_banner(a, b);
}
void wm_run(void) {
    mouse_state_t ms;
    uint32_t last_tick = 0;
    gui_heartbeat = timer_ticks();
    watchdog_hook = watchdog;
    for (;;) {
        uint32_t loop_t0 = timer_ticks();
        gui_heartbeat = timer_ticks();
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
        while (defn) { defer_t d = defq[0]; memmove(defq, defq + 1, (--defn) * sizeof(defer_t)); d.fn(d.arg); }
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
        if (wm_need_redraw) compose();
        if ((int32_t)(timer_ticks() - loop_t0) >= 150)
            usb_diag("gui: %u ms in one frame", timer_ticks() - loop_t0);
        task_yield();
        if (!activity && !wm_need_redraw) {
            extern volatile bool cpu_idle_flag;
            cpu_idle_flag = true; hlt(); cpu_idle_flag = false;
        }
    }
}
