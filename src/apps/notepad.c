/* Notepad text editor + shared Open/Save file dialog */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"

/* ======================= file dialog ======================= */
typedef void (*fdlg_cb)(void *ctx, const char *path);
typedef struct { char dir[256]; char name[128]; bool save; fdlg_cb cb; void *ctx; int sel, scroll; const char *ext; } fdlg_t;
static void fdlg_finish(window_t *w, const char *path) { fdlg_t *f = w->data; fdlg_cb cb = f->cb; void *ctx = f->ctx; char p[400]; strlcpy(p, path, sizeof p); wm_destroy(w); if (cb) cb(ctx, p); }
static void fdlg_paint(window_t *w) {
    fdlg_t *f = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    draw_text(c, FONT_UI, 10, 12, f->save ? "Save in:" : "Look in:", BLACK);
    draw_sunken_box(c, R(70, 8, c->w - 150, 22), WHITE);
    draw_icon(c, ICON_FOLDER_OPEN, 16, 74, 11);
    draw_text_ellipsis(c, FONT_UI, 94, 12, c->w - 180, f->dir, BLACK);
    if (ui_icon_button(w, R(c->w - 74, 7, 28, 24), ICON_UP, NULL)) { vnode_t *n = vfs_resolve(f->dir, NULL); if (n && n->parent) vfs_path(n->parent, f->dir, sizeof f->dir); f->sel = -1; f->scroll = 0; }
    if (ui_icon_button(w, R(c->w - 42, 7, 28, 24), ICON_MY_DOCUMENTS, NULL)) { strcpy(f->dir, USER_DOCS); f->sel = -1; }
    /* shortcuts */
    const char *pl[] = { "Desktop", "My Documents", "My Computer" }; const int pi[] = { ICON_DESKTOP, ICON_MY_DOCUMENTS, ICON_COMPUTER };
    const char *pp[] = { USER_DESKTOP, USER_DOCS, "C:\\" };
    fill_rect(c, 8, 40, 82, c->h - 90, T.classic ? HEX(0x808080) : lighten(T.select, 120));
    for (int i = 0; i < 3; i++) {
        rect_t r = R(10, 46 + i * 64, 78, 60);
        if (ui_click_area(w, r)) { strcpy(f->dir, pp[i]); f->sel = -1; f->scroll = 0; }
        draw_icon(c, pi[i], 32, r.x + 23, r.y + 4);
        draw_text_centered(c, FONT_UI, R(r.x, r.y + 40, r.w, 16), pl[i], WHITE);
    }
    /* listing */
    vnode_t *d = vfs_resolve(f->dir, NULL);
    vnode_t *items[256]; int n = 0;
    if (d && d->dir) { vfs_sort_children(d); for (vnode_t *k = d->child; k && n < 256; k = k->next) if (k->dir || !f->ext || !*f->ext || str_ends_with_ci(k->name, f->ext)) items[n++] = k; }
    rect_t lr = R(96, 40, c->w - 104, c->h - 104);
    draw_sunken_box(c, lr, WHITE);
    int ih = 20, vis = (lr.h - 4) / ih;
    rect_t old = canvas_push_clip(c, R(lr.x + 1, lr.y + 1, lr.w - 2, lr.h - 2));
    for (int i = f->scroll; i < n && i < f->scroll + vis + 1; i++) {
        rect_t r = R(lr.x + 2, lr.y + 2 + (i - f->scroll) * ih, lr.w - 22, ih);
        if (ui_click_area(w, r)) {
            if (w->ui.dblclick || (f->sel == i && items[i]->dir)) {}
            f->sel = i;
            if (!items[i]->dir) strlcpy(f->name, items[i]->name, sizeof f->name);
        }
        if (i == f->sel) fill_rect(c, r.x, r.y, r.w, r.h, T.select);
        draw_icon(c, file_icon(items[i]), 16, r.x + 3, r.y + 2);
        draw_text_ellipsis(c, FONT_UI, r.x + 24, r.y + 3, r.w - 30, items[i]->name, i == f->sel ? WHITE : BLACK);
    }
    c->clip = old;
    if (n > vis) draw_scrollbar(c, R(lr.x + lr.w - 18, lr.y + 1, 17, lr.h - 2), n, vis, f->scroll, true);
    if (w->ui.dblclick && f->sel >= 0 && f->sel < n) {
        vnode_t *k = items[f->sel];
        if (k->dir) { vfs_path(k, f->dir, sizeof f->dir); f->sel = -1; f->scroll = 0; w->dirty = true; }
        else { char p[400]; vfs_path(k, p, sizeof p); fdlg_finish(w, p); return; }
    }
    if (w->ui.wheel) { f->scroll = CLAMP(f->scroll + w->ui.wheel * 3, 0, MAX(0, n - vis)); }
    draw_text(c, FONT_UI, 96, c->h - 56, "File name:", BLACK);
    if (!w->ui.focus) w->ui.focus = 0;
    bool enter = ui_textbox(w, R(170, c->h - 60, c->w - 270, 22), f->name, sizeof f->name);
    bool ok = ui_button_ex(w, R(c->w - 88, c->h - 60, 78, 23), f->save ? "Save" : "Open", true, true);
    bool cancel = ui_button(w, R(c->w - 88, c->h - 32, 78, 23), "Cancel");
    if (enter || ok) {
        if (f->sel >= 0 && f->sel < n && items[f->sel]->dir && (!f->name[0] || !strcmp(f->name, items[f->sel]->name))) { vfs_path(items[f->sel], f->dir, sizeof f->dir); f->sel = -1; w->dirty = true; return; }
        if (!f->name[0]) return;
        char p[400];
        if (isalpha(f->name[0]) && f->name[1] == ':') strlcpy(p, f->name, sizeof p); else path_join(p, sizeof p, f->dir, f->name);
        path_normalize(p);
        vnode_t *t = vfs_resolve(p, NULL);
        if (t && t->dir) { vfs_path(t, f->dir, sizeof f->dir); f->name[0] = 0; return; }
        if (!f->save && !t) { msgbox("Open", "File not found. Please verify the correct file name was given.", MBI_WARNING, MBX_OK, NULL, NULL); return; }
        fdlg_finish(w, p);
    } else if (cancel) wm_destroy(w);
}
void file_dialog(const char *title, const char *initial, bool save, const char *ext, fdlg_cb cb, void *ctx) {
    window_t *w = wm_create(title, -1, -1, 520, 330, WF_NO_MAX | WF_NO_MIN);
    fdlg_t *f = kzalloc(sizeof *f);
    f->save = save; f->cb = cb; f->ctx = ctx; f->sel = -1; f->ext = ext;
    if (initial && *initial) {
        vnode_t *n = vfs_resolve(initial, NULL);
        if (n && n->dir) vfs_path(n, f->dir, sizeof f->dir);
        else { path_dirname(initial, f->dir, sizeof f->dir); strlcpy(f->name, path_basename(initial), sizeof f->name); if (!vfs_resolve(f->dir, NULL)) strcpy(f->dir, USER_DOCS); }
    } else strcpy(f->dir, USER_DOCS);
    w->data = f; w->paint = fdlg_paint; w->icon = save ? ICON_FOLDER : ICON_FOLDER_OPEN;
    if (wm_active && wm_active != w) { w->owner = wm_active; w->flags |= WF_MODAL; }
    wm_center(w);
}

/* ======================= notepad ======================= */
typedef struct {
    char *buf; int len, cap;
    int cur, anchor;        /* anchor -1 = no selection */
    int top, left;          /* first visible line, horizontal scroll px */
    bool wrap, modified, dragging;
    char path[256];
    int *lines; int nlines, lcap;   /* visual line starts */
    window_t *win;
    bool closing;
} np_t;
static char *np_clip; static int np_clip_len;
#define NP_FONT FONT_MONO
static int cw(void) { return text_width(NP_FONT, "M"); }
static int lh(void) { return font_height(NP_FONT) + 1; }
static void np_title(np_t *n) {
    char t[160]; snprintf(t, sizeof t, "%s%s - Notepad", n->modified ? "*" : "", n->path[0] ? path_basename(n->path) : "Untitled");
    wm_set_title(n->win, t);
}
static void np_reserve(np_t *n, int extra) { if (n->len + extra + 1 > n->cap) { n->cap = (n->len + extra + 1) * 2; n->buf = krealloc(n->buf, n->cap); } }
static void np_layout(np_t *n) {
    int cols = n->wrap ? MAX(10, (n->win->surf->w - 24) / cw()) : 1 << 30;
    n->nlines = 0;
    int start = 0, col = 0;
    for (int i = 0; i <= n->len; i++) {
        if (n->nlines + 2 >= n->lcap) { n->lcap = n->lcap ? n->lcap * 2 : 256; n->lines = krealloc(n->lines, n->lcap * sizeof(int)); }
        if (i == n->len || n->buf[i] == '\n') { n->lines[n->nlines++] = start; start = i + 1; col = 0; continue; }
        if (++col > cols) { n->lines[n->nlines++] = start; start = i; col = 1; }
    }
    n->lines[n->nlines] = n->len + 1;
}
static int line_of(np_t *n, int pos) { int lo = 0, hi = n->nlines - 1; while (lo < hi) { int m = (lo + hi + 1) / 2; if (n->lines[m] <= pos) lo = m; else hi = m - 1; } return lo; }
static int line_end(np_t *n, int l) { int e = n->lines[l + 1] - 1; if (e > n->len) e = n->len; if (l + 1 < n->nlines && e >= n->lines[l] && e < n->len && n->buf[e] != '\n') e++; return MIN(e, n->len); }
static void sel_range(np_t *n, int *a, int *b) { if (n->anchor < 0) { *a = *b = n->cur; return; } *a = MIN(n->anchor, n->cur); *b = MAX(n->anchor, n->cur); }
static void del_range(np_t *n, int a, int b) { if (b <= a) return; memmove(n->buf + a, n->buf + b, n->len - b); n->len -= b - a; n->buf[n->len] = 0; n->cur = a; n->anchor = -1; n->modified = true; }
static void del_sel(np_t *n) { int a, b; sel_range(n, &a, &b); del_range(n, a, b); }
static void insert(np_t *n, const char *s, int l) {
    del_sel(n);
    np_reserve(n, l);
    memmove(n->buf + n->cur + l, n->buf + n->cur, n->len - n->cur);
    memcpy(n->buf + n->cur, s, l);
    n->len += l; n->cur += l; n->buf[n->len] = 0; n->modified = true;
}
static void ensure_visible(np_t *n) {
    np_layout(n);
    int vis = MAX(1, (n->win->surf->h - 4) / lh());
    int l = line_of(n, n->cur);
    if (l < n->top) n->top = l;
    if (l >= n->top + vis) n->top = l - vis + 1;
    if (!n->wrap) {
        int x = (n->cur - n->lines[l]) * cw();
        if (x < n->left) n->left = MAX(0, x - 40);
        if (x > n->left + n->win->surf->w - 40) n->left = x - n->win->surf->w + 80;
    } else n->left = 0;
}
static bool np_load(np_t *n, const char *path) {
    vnode_t *v = vfs_resolve(path, NULL);
    if (!v || v->dir) return false;
    n->len = 0; np_reserve(n, v->size);
    for (uint32_t i = 0; i < v->size; i++) if (v->data[i] != '\r') n->buf[n->len++] = v->data[i] ? v->data[i] : ' ';
    n->buf[n->len] = 0;
    vfs_path(v, n->path, sizeof n->path);
    n->cur = 0; n->anchor = -1; n->top = 0; n->modified = false;
    np_title(n);
    return true;
}
static bool np_save_to(np_t *n, const char *path) {
    vnode_t *v = vfs_write_file(path, n->buf, n->len);
    if (!v) { msgbox("Notepad", "Cannot save the file. The location may be read-only.", MBI_ERROR, MBX_OK, NULL, NULL); return false; }
    vfs_path(v, n->path, sizeof n->path);
    n->modified = false; np_title(n);
    return true;
}
static void save_as_cb(void *ctx, const char *p) { window_t *w = ctx; if (!wm_alive(w)) return; np_t *n = w->data; if (np_save_to(n, p) && n->closing) wm_destroy(w); }
static void open_cb(void *ctx, const char *p) { window_t *w = ctx; if (!wm_alive(w)) return; np_t *n = w->data; if (!np_load(n, p)) msgbox("Notepad", "Cannot open the file.", MBI_ERROR, MBX_OK, NULL, NULL); w->dirty = true; }
static void np_save(window_t *w, bool as) {
    np_t *n = w->data;
    vnode_t *v = n->path[0] ? vfs_resolve(n->path, NULL) : NULL;
    if (!as && n->path[0] && !(v && v->readonly)) { if (np_save_to(n, n->path) && n->closing) wm_destroy(w); return; }
    file_dialog("Save As", n->path[0] && !(v && v->readonly) ? n->path : USER_DOCS "\\Untitled.txt", true, NULL, save_as_cb, w);
}
static void close_ask_cb(void *ctx, int r) {
    window_t *w = ctx; if (!wm_alive(w)) return; np_t *n = w->data;
    if (r == IDYES) { n->closing = true; np_save(w, false); }
    else if (r == IDNO) wm_destroy(w);
}
static bool np_close(window_t *w) {
    np_t *n = w->data;
    if (!n->modified) return true;
    char m[300]; snprintf(m, sizeof m, "The text in the %s file has changed.\n\nDo you want to save the changes?", n->path[0] ? path_basename(n->path) : "Untitled");
    msgbox("Notepad", m, MBI_WARNING, MBX_YESNOCANCEL, close_ask_cb, w);
    return false;
}
enum { NP_NEW = 1, NP_OPEN, NP_SAVE, NP_SAVEAS, NP_EXIT, NP_CUT, NP_COPY, NP_PASTE, NP_DEL, NP_ALL, NP_TIME, NP_WRAP, NP_ABOUT, NP_RUN };
static void np_cmd(window_t *w, int id) {
    np_t *n = w->data;
    int a, b; sel_range(n, &a, &b);
    switch (id) {
    case NP_NEW: if (n->modified) { np_close(w); break; } n->len = 0; n->buf[0] = 0; n->cur = 0; n->anchor = -1; n->path[0] = 0; n->modified = false; np_title(n); break;
    case NP_OPEN: file_dialog("Open", n->path[0] ? n->path : USER_DOCS, false, NULL, open_cb, w); break;
    case NP_SAVE: np_save(w, false); break;
    case NP_SAVEAS: np_save(w, true); break;
    case NP_EXIT: wm_close(w); break;
    case NP_CUT: case NP_COPY: if (b > a) { kfree(np_clip); np_clip = strndup(n->buf + a, b - a); np_clip_len = b - a; if (id == NP_CUT) del_sel(n); } break;
    case NP_PASTE: if (np_clip) insert(n, np_clip, np_clip_len); break;
    case NP_DEL: if (b > a) del_sel(n); else if (n->cur < n->len) del_range(n, n->cur, n->cur + 1); break;
    case NP_ALL: n->anchor = 0; n->cur = n->len; break;
    case NP_TIME: { rtc_time_t t; rtc_now(&t); char s[40]; snprintf(s, sizeof s, "%d:%02d %s %d/%d/%d", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.hour >= 12 ? "PM" : "AM", t.month, t.day, t.year); insert(n, s, strlen(s)); break; }
    case NP_WRAP: n->wrap = !n->wrap; w->menus[2].items[0].flags = n->wrap ? MI_CHECKED : 0; break;
    case NP_ABOUT: about_open(); break;
    case NP_RUN: if (n->path[0]) { if (n->modified) np_save_to(n, n->path); shell_open(n->path, NULL); } break;
    }
    np_title(n);
    ensure_visible(n);
    w->dirty = true;
}
static void np_key(window_t *w, key_event_t *e) {
    np_t *n = w->data;
    if (!e->pressed) return;
    bool shift = e->mods & MOD_SHIFT, ctrl = e->mods & MOD_CTRL;
    if (ctrl) {
        switch (tolower(e->key)) {
        case 's': np_cmd(w, NP_SAVE); return; case 'o': np_cmd(w, NP_OPEN); return; case 'n': np_cmd(w, NP_NEW); return;
        case 'x': np_cmd(w, NP_CUT); return; case 'c': np_cmd(w, NP_COPY); return; case 'v': np_cmd(w, NP_PASTE); return; case 'a': np_cmd(w, NP_ALL); return;
        }
    }
    np_layout(n);
    int l = line_of(n, n->cur), col = n->cur - n->lines[l];
    int vis = MAX(1, (w->surf->h - 4) / lh());
    int old = n->cur;
    bool move = true;
    switch (e->key) {
    case KEY_LEFT: if (n->anchor >= 0 && !shift) { int a, b; sel_range(n, &a, &b); n->cur = a; } else if (n->cur > 0) n->cur--; break;
    case KEY_RIGHT: if (n->anchor >= 0 && !shift) { int a, b; sel_range(n, &a, &b); n->cur = b; } else if (n->cur < n->len) n->cur++; break;
    case KEY_UP: if (l > 0) n->cur = MIN(n->lines[l - 1] + col, line_end(n, l - 1)); break;
    case KEY_DOWN: if (l < n->nlines - 1) n->cur = MIN(n->lines[l + 1] + col, line_end(n, l + 1)); else n->cur = n->len; break;
    case KEY_HOME: n->cur = ctrl ? 0 : n->lines[l]; break;
    case KEY_END: n->cur = ctrl ? n->len : line_end(n, l); break;
    case KEY_PGUP: { int t = MAX(0, l - vis); n->cur = MIN(n->lines[t] + col, line_end(n, t)); break; }
    case KEY_PGDN: { int t = MIN(n->nlines - 1, l + vis); n->cur = MIN(n->lines[t] + col, line_end(n, t)); break; }
    case KEY_F5: np_cmd(w, NP_TIME); return;
    default: move = false;
    }
    if (move) {
        if (shift) { if (n->anchor < 0) n->anchor = old; } else n->anchor = -1;
        if (n->anchor == n->cur) n->anchor = -1;
        ensure_visible(n); w->dirty = true; return;
    }
    if (e->key == KEY_BACKSPACE) { int a, b; sel_range(n, &a, &b); if (b > a) del_sel(n); else if (n->cur > 0) del_range(n, n->cur - 1, n->cur); }
    else if (e->key == KEY_DELETE) np_cmd(w, NP_DEL);
    else if (e->key == KEY_ENTER) insert(n, "\n", 1);
    else if (e->key == KEY_TAB) insert(n, "\t", 1);
    else if (e->ch >= 32 && e->ch < 127 && !ctrl) insert(n, &e->ch, 1);
    else return;
    np_title(n);
    ensure_visible(n);
    w->dirty = true;
}
static int pos_at(np_t *n, int x, int y) {
    np_layout(n);
    int l = CLAMP(n->top + (y - 2) / lh(), 0, n->nlines - 1);
    int start = n->lines[l], end = line_end(n, l);
    int px = x - 4 + n->left;
    int col = 0, cx = 0;
    for (int i = start; i < end; i++) { int cwid = n->buf[i] == '\t' ? cw() * 4 : cw(); if (cx + cwid / 2 > px) break; cx += cwid; col++; }
    return start + col;
}
static void np_mouse(window_t *w, int type, int x, int y, int btn) {
    np_t *n = w->data;
    int sbx = w->surf->w - 17;
    if (type == ME_WHEEL) { n->top = CLAMP(n->top + btn * 3, 0, MAX(0, n->nlines - 1)); w->dirty = true; return; }
    if (type == ME_DOWN && x >= sbx) { int vis = (w->surf->h - 4) / lh(); if (y < 17) n->top--; else if (y > w->surf->h - 17) n->top++; else n->top = (y - 17) * MAX(0, n->nlines - vis) / MAX(1, w->surf->h - 34); n->top = CLAMP(n->top, 0, MAX(0, n->nlines - 1)); w->dirty = true; return; }
    if (type == ME_DOWN && (btn & MB_LEFT)) { n->cur = pos_at(n, x, y); n->anchor = n->cur; n->dragging = true; w->dirty = true; }
    else if (type == ME_MOVE && n->dragging) { n->cur = pos_at(n, x, y); if (y < 0 && n->top > 0) n->top--; w->dirty = true; }
    else if (type == ME_UP) { n->dragging = false; if (n->anchor == n->cur) n->anchor = -1; }
    else if (type == ME_DBLCLICK) { int a = n->cur, b = n->cur; while (a > 0 && isalnum(n->buf[a - 1])) a--; while (b < n->len && isalnum(n->buf[b])) b++; n->anchor = a; n->cur = b; n->dragging = false; w->dirty = true; }
    else if (type == ME_DOWN && (btn & MB_RIGHT)) {
        static const menu_item_t m[] = { { "Cut", NP_CUT, 0 }, { "Copy", NP_COPY, 0 }, { "Paste", NP_PASTE, 0 }, { "Delete", NP_DEL, 0 }, { "-", 0, 0 }, { "Select All", NP_ALL, 0 } };
        rect_t cr = wm_client_rect(w);
        extern void np_popup_cb(void *ctx, int id);
        popup_menu(cr.x + x, cr.y + y, m, 6, np_popup_cb, w);
    }
}
void np_popup_cb(void *ctx, int id) { window_t *w = ctx; if (wm_alive(w)) np_cmd(w, id); }
static void np_paint(window_t *w) {
    np_t *n = w->data; canvas_t *c = w->surf;
    gfx_clear(c, WHITE);
    np_layout(n);
    int vis = (c->h - 4) / lh() + 1, a, b;
    sel_range(n, &a, &b);
    bool focused = wm_active == w;
    rect_t old = canvas_push_clip(c, R(0, 0, c->w - 17, c->h));
    for (int k = 0; k < vis && n->top + k < n->nlines; k++) {
        int l = n->top + k, s = n->lines[l], e = line_end(n, l), y = 2 + k * lh();
        int x = 4 - n->left;
        for (int i = s; i <= e; i++) {
            bool in_sel = i >= a && i < b;
            int cwid = (i < e && n->buf[i] == '\t') ? cw() * 4 : cw();
            if (in_sel && i < e) fill_rect(c, x, y, cwid, lh(), T.select);
            if (i == n->cur && focused && (timer_ticks() / 500) % 2 == 0) vline(c, x, y, lh(), BLACK);
            if (i < e && n->buf[i] != '\t' && x > -cw() && x < c->w) { char ch = n->buf[i]; draw_text_n(c, NP_FONT, x, y, &ch, 1, in_sel ? WHITE : BLACK); }
            x += cwid;
        }
    }
    c->clip = old;
    draw_scrollbar(c, R(c->w - 17, 0, 17, c->h), n->nlines + vis - 1, vis, n->top, true);
}
static void np_blink(window_t *w) { if (wm_active == w) w->dirty = true; }
static void np_destroy(window_t *w) { np_t *n = w->data; kfree(n->buf); kfree(n->lines); kfree(n); }
static const menu_t np_menus[] = {
    { "File", { { "New\tCtrl+N", NP_NEW, 0 }, { "Open...\tCtrl+O", NP_OPEN, 0 }, { "Save\tCtrl+S", NP_SAVE, 0 }, { "Save As...", NP_SAVEAS, 0 }, { "-", 0, 0 }, { "Run this file", NP_RUN, 0 }, { "-", 0, 0 }, { "Exit", NP_EXIT, 0 }, { NULL } } },
    { "Edit", { { "Cut\tCtrl+X", NP_CUT, 0 }, { "Copy\tCtrl+C", NP_COPY, 0 }, { "Paste\tCtrl+V", NP_PASTE, 0 }, { "Delete\tDel", NP_DEL, 0 }, { "-", 0, 0 }, { "Select All\tCtrl+A", NP_ALL, 0 }, { "Time/Date\tF5", NP_TIME, 0 }, { NULL } } },
    { "Format", { { "Word Wrap", NP_WRAP, 0 }, { NULL } } },
    { "Help", { { "About Notepad", NP_ABOUT, 0 }, { NULL } } },
};
void notepad_open(const char *path) {
    window_t *w = wm_create("Untitled - Notepad", -1, -1, 600, 420, WF_RESIZABLE);
    np_t *n = kzalloc(sizeof *n);
    n->win = w; n->anchor = -1; n->cap = 256; n->buf = kmalloc(n->cap); n->buf[0] = 0;
    w->data = n; w->paint = np_paint; w->key = np_key; w->mouse = np_mouse; w->close = np_close; w->destroy = np_destroy; w->menu_cmd = np_cmd;
    w->icon = ICON_NOTEPAD; w->cursor = CURSOR_IBEAM;
    w->menus = kmalloc(sizeof np_menus); memcpy(w->menus, np_menus, sizeof np_menus); w->nmenus = ARRAY_LEN(np_menus);
    rect_t cr = wm_client_rect(w); wm_resize_client(w, cr.w, cr.h);
    wm_set_timer(w, 500, np_blink);
    if (path && *path) {
        if (!np_load(n, path)) { strlcpy(n->path, path, sizeof n->path); np_title(n); }
    }
    np_title(n);
}
