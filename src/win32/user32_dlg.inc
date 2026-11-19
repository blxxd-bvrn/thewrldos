/* user32, continued (included by user32.c): dialog boxes from resource templates, the
 * list box / combo box / tree view controls in full, Unicode window text, and the rest
 * of what real programs (PuTTY, installers, utilities) call.
 *
 * Dialogs: CreateDialog* / DialogBox* read a DLGTEMPLATE or DLGTEMPLATEEX, convert dialog
 * units to pixels from the dialog font, create the window (the template's class, or the
 * system's #32770) and its controls, and send WM_INITDIALOG. DefDlgProc is the dialog
 * engine: it calls the dialog procedure kept at DWLP_DLGPROC in the window's extra bytes,
 * returns DWLP_MSGRESULT for handled messages and does the default work otherwise.
 * IsDialogMessage moves the focus with Tab and turns Enter / Esc into IDOK / IDCANCEL. */

#define DWLP_MSGRESULT 0
#define DWLP_DLGPROC ((int)sizeof(LRESULT))
#define DWLP_USER ((int)(2 * sizeof(void *)))
#define WS_GROUP 0x00020000u
#define WS_TABSTOP 0x00010000u
#define BN_CLICKED 0
#define IDOK 1
#define IDCANCEL 2

/* ---------------------------------------------------------------- UTF-8 <-> UTF-16 */
static int w_len(const WCHAR *w) { int n = 0; while (w && w[n]) n++; return n; }
static void w2u(const WCHAR *w, char *a, int cap) {
    int o = 0;
    if (!w || (uintptr_t)w < 0x10000) { if (cap) a[0] = 0; return; }
    for (int i = 0; w[i] && o < cap - 4; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        if (c < 0x80) a[o++] = (char)c;
        else if (c < 0x800) { a[o++] = 0xC0 | (c >> 6); a[o++] = 0x80 | (c & 63); }
        else if (c < 0x10000) { a[o++] = 0xE0 | (c >> 12); a[o++] = 0x80 | ((c >> 6) & 63); a[o++] = 0x80 | (c & 63); }
        else { a[o++] = 0xF0 | (c >> 18); a[o++] = 0x80 | ((c >> 12) & 63); a[o++] = 0x80 | ((c >> 6) & 63); a[o++] = 0x80 | (c & 63); }
    }
    a[o] = 0;
}
static int u2w(const char *a, WCHAR *w, int cap) {
    int o = 0;
    if (cap <= 0) return 0;
    for (const uint8_t *s = (const uint8_t *)a; s && *s && o < cap - 1; ) {
        uint32_t c = *s++;
        if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63); s += 3; }
        else if (c >= 0xE0 && s[0] && s[1]) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
        else if (c >= 0xC0 && s[0]) { c = (c & 31) << 6 | (s[0] & 63); s += 1; }
        if (c >= 0x10000 && o < cap - 2) { c -= 0x10000; w[o++] = 0xD800 + (c >> 10); w[o++] = 0xDC00 + (c & 1023); }
        else w[o++] = (WCHAR)c;
    }
    w[o] = 0;
    return o;
}
static bool sync_call(w32win_t *w) { return w32_cur() == w->proc; }   /* running in the program's own task */

/* ---------------------------------------------------------------- list storage (list box, combo box) */
static int item_h(int f) { return font_height(f) + 3; }
static void list_insert(w32win_t *w, int at, const char *s) {
    if (at < 0 || at > w->lb_n) at = w->lb_n;
    w->lb_items = krealloc(w->lb_items, (w->lb_n + 1) * sizeof(char *));
    w->lb_data = krealloc(w->lb_data, (w->lb_n + 1) * sizeof(uintptr_t));
    w->lb_selbits = krealloc(w->lb_selbits, w->lb_n + 1);
    memmove(w->lb_items + at + 1, w->lb_items + at, (w->lb_n - at) * sizeof(char *));
    memmove(w->lb_data + at + 1, w->lb_data + at, (w->lb_n - at) * sizeof(uintptr_t));
    memmove(w->lb_selbits + at + 1, w->lb_selbits + at, w->lb_n - at);
    w->lb_items[at] = strdup(s ? s : ""); w->lb_data[at] = 0; w->lb_selbits[at] = 0;
    w->lb_n++;
    if (w->lb_sel >= at) w->lb_sel++;
}
static void list_delete(w32win_t *w, int i) {
    if (i < 0 || i >= w->lb_n) return;
    kfree(w->lb_items[i]);
    memmove(w->lb_items + i, w->lb_items + i + 1, (w->lb_n - i - 1) * sizeof(char *));
    memmove(w->lb_data + i, w->lb_data + i + 1, (w->lb_n - i - 1) * sizeof(uintptr_t));
    memmove(w->lb_selbits + i, w->lb_selbits + i + 1, w->lb_n - i - 1);
    w->lb_n--;
    if (w->lb_sel == i) w->lb_sel = -1; else if (w->lb_sel > i) w->lb_sel--;
}
static void list_clear(w32win_t *w) { while (w->lb_n) list_delete(w, w->lb_n - 1); w->lb_sel = -1; w->lb_top = 0; }
static int list_find(w32win_t *w, int start, const char *s, bool exact) {
    int n = w->lb_n, l = strlen(s);
    for (int k = 1; k <= n; k++) {
        int i = (start + k) % (n ? n : 1);
        if (i < 0 || i >= n) continue;
        if (exact ? !strcasecmp(w->lb_items[i], s) : !strncasecmp(w->lb_items[i], s, l)) return i;
    }
    return -1;
}
static int list_sorted_pos(w32win_t *w, const char *s) { int i = 0; while (i < w->lb_n && strcasecmp(w->lb_items[i], s) <= 0) i++; return i; }
static bool lb_multi(w32win_t *w) { return w->ctl == CTL_LISTBOX && (w->style & (0x8 | 0x800)); }   /* LBS_MULTIPLESEL / LBS_EXTENDEDSEL */
static void notify_cmd(w32win_t *w, int code) {
    if (!w->parent) return;
    WPARAM wp = (w->id & 0xFFFF) | ((uint32_t)code << 16);
    if (sync_call(w)) send(w->parent, WM_COMMAND, wp, (uintptr_t)w); else w32_post(w->proc, (uintptr_t)w->parent, WM_COMMAND, wp, (uintptr_t)w);
}
static void lb_ensure_visible(w32win_t *w, int f) {
    int rows = MAX(1, (w->h - 4) / item_h(f));
    if (w->lb_sel >= 0) { if (w->lb_sel < w->lb_top) w->lb_top = w->lb_sel; if (w->lb_sel >= w->lb_top + rows) w->lb_top = w->lb_sel - rows + 1; }
    w->lb_top = CLAMP(w->lb_top, 0, MAX(0, w->lb_n - rows));
}
static int ctl_font(w32win_t *c) { gdiobj_t *o = c->font ? OBJ(c->font) : NULL; return o ? o->font : FONT_UI; }

/* tab stops in list items: shown as spaces */
static void untab(const char *s, char *o, int cap) { int k = 0; for (; *s && k < cap - 4; s++) { if (*s == '\t') { o[k++] = ' '; o[k++] = ' '; } else o[k++] = *s; } o[k] = 0; }
static void draw_list_ext(w32win_t *c, canvas_t *s, rect_t r, int f) {
    int ih = item_h(f), rows = MAX(1, (r.h - 4) / ih);
    bool sb = c->lb_n > rows;
    rect_t o = canvas_push_clip(s, R(r.x + 2, r.y + 2, r.w - 4 - (sb ? 16 : 0), r.h - 4));
    for (int i = c->lb_top; i < c->lb_n && i < c->lb_top + rows + 1; i++) {
        int y = r.y + 2 + (i - c->lb_top) * ih;
        bool sel = lb_multi(c) ? c->lb_selbits[i] : i == c->lb_sel;
        if (sel) fill_rect(s, r.x + 2, y, r.w - 4, ih, focus_win == c || !lb_multi(c) ? T.select : T.face);
        char t[512]; untab(c->lb_items[i], t, sizeof t);
        draw_text(s, f, r.x + 4, y + 1, t, sel ? T.select_text : (c->enabled ? T.text : T.gray_text));
        if (lb_multi(c) && i == c->lb_sel && focus_win == c) dotted_rect(s, r.x + 2, y, r.w - 4 - (sb ? 16 : 0), ih, T.text);
    }
    s->clip = o;
    if (sb) draw_scrollbar(s, R(r.x + r.w - 18, r.y + 2, 16, r.h - 4), c->lb_n, rows, c->lb_top, true);
}

/* ---------------------------------------------------------------- tree view */
typedef struct tvitem { struct tvitem *parent, *child, *next; char text[160]; uintptr_t lparam; bool expanded; uint32_t state; int cchildren; } tvitem_t;
#define TVI_ROOT ((uintptr_t)(intptr_t)-0x10000)
#define TVI_FIRST ((uintptr_t)(intptr_t)-0xFFFF)
#define TVI_LAST ((uintptr_t)(intptr_t)-0xFFFE)
#define TVI_SORT ((uintptr_t)(intptr_t)-0xFFFD)
typedef struct { UINT mask; uintptr_t hItem; UINT state, stateMask; uintptr_t pszText; int cchTextMax, iImage, iSelectedImage, cChildren; LPARAM lParam; } TVITEM_;
typedef struct { uintptr_t hParent, hInsertAfter; TVITEM_ item; } TVINSERT_;
typedef struct { uintptr_t hwndFrom, idFrom; UINT code; } NMHDR_;
typedef struct { NMHDR_ hdr; UINT action; TVITEM_ itemOld, itemNew; LONG ptx, pty; } NMTREEVIEW_;
static bool tv_valid_in(tvitem_t *list, tvitem_t *it) { for (tvitem_t *x = list; x; x = x->next) { if (x == it || tv_valid_in(x->child, it)) return true; } return false; }
static tvitem_t *TVI(w32win_t *w, uintptr_t h) { tvitem_t *it = (tvitem_t *)h; return h && h < (uintptr_t)(intptr_t)-0x20000 && tv_valid_in(w->tv_root, it) ? it : NULL; }
static void tv_free_list(w32win_t *w, tvitem_t *it) { while (it) { tvitem_t *n = it->next; tv_free_list(w, it->child); if (w->tv_sel == it) w->tv_sel = NULL; kfree(it); it = n; } }
void w32_tv_free_all(w32win_t *w) { tv_free_list(w, w->tv_root); w->tv_root = NULL; w->tv_sel = NULL; if (w->tv_nm) { kfree(w->tv_nm); w->tv_nm = NULL; } }
static tvitem_t **tv_list_of(w32win_t *w, tvitem_t *parent) { return parent ? &parent->child : &w->tv_root; }
/* the visible items in order (expanded branches), with their depth */
static int tv_walk(tvitem_t *list, int depth, tvitem_t **out, int *deps, int n, int cap) {
    for (tvitem_t *x = list; x && n < cap; x = x->next) {
        out[n] = x; deps[n] = depth; n++;
        if (x->expanded) n = tv_walk(x->child, depth + 1, out, deps, n, cap);
    }
    return n;
}
#define TV_MAXROWS 512
static void tv_text_out(w32win_t *w, tvitem_t *it, TVITEM_ *ti) {
    if (ti->mask & 1) {                                                       /* TVIF_TEXT */
        if (ti->pszText && ti->cchTextMax > 0) {
            if (w->tv_unicode) u2w(it->text, (WCHAR *)ti->pszText, ti->cchTextMax);
            else strlcpy((char *)ti->pszText, it->text, ti->cchTextMax);
        }
    }
    if (ti->mask & 4) ti->lParam = it->lparam;                                /* TVIF_PARAM */
    if (ti->mask & 8) ti->state = it->state | (it->expanded ? 0x20 : 0) | (it == w->tv_sel ? 2 : 0);   /* TVIF_STATE */
    if (ti->mask & 0x40) ti->cChildren = it->child ? 1 : it->cchildren;       /* TVIF_CHILDREN */
    ti->hItem = (uintptr_t)it;
}
static void tv_text_in(w32win_t *w, tvitem_t *it, const TVITEM_ *ti, bool wide) {
    if ((ti->mask & 1) && ti->pszText && ti->pszText != (uintptr_t)-1) {
        if (wide) w2u((const WCHAR *)ti->pszText, it->text, sizeof it->text); else strlcpy(it->text, (const char *)ti->pszText, sizeof it->text);
    }
    if (ti->mask & 4) it->lparam = ti->lParam;
    if (ti->mask & 8) { it->state = (it->state & ~ti->stateMask) | (ti->state & ti->stateMask); if (ti->stateMask & 0x20) it->expanded = (ti->state & 0x20) != 0; }
    if (ti->mask & 0x40) it->cchildren = ti->cChildren;
}
/* change the selection; tell the parent (TVN_SELCHANGED, A or W by how items were inserted) */
static void tv_select(w32win_t *w, tvitem_t *it, int action) {
    if (it == w->tv_sel) return;
    tvitem_t *old = w->tv_sel;
    w->tv_sel = it;
    for (tvitem_t *p = it ? it->parent : NULL; p; p = p->parent) p->expanded = true;   /* make it visible */
    draw_control(w);
    if (!w->parent) return;
    if (!w->tv_nm) w->tv_nm = kzalloc(sizeof(NMTREEVIEW_));
    NMTREEVIEW_ *nm = w->tv_nm; memset(nm, 0, sizeof *nm);
    nm->hdr.hwndFrom = (uintptr_t)w; nm->hdr.idFrom = w->id;
    nm->hdr.code = w->tv_unicode ? (UINT)-451 : (UINT)-402;                   /* TVN_SELCHANGEDW / A */
    nm->action = action;
    nm->itemOld.mask = nm->itemNew.mask = 4 | 8 | 0x10;
    nm->itemOld.hItem = (uintptr_t)old; if (old) nm->itemOld.lParam = old->lparam;
    nm->itemNew.hItem = (uintptr_t)it; if (it) { nm->itemNew.lParam = it->lparam; nm->itemNew.state = 2; }
    if (sync_call(w)) send(w->parent, 0x4E, w->id, (uintptr_t)nm);           /* WM_NOTIFY */
    else w32_post(w->proc, (uintptr_t)w->parent, 0x4E, w->id, (uintptr_t)nm);
}
static void draw_tree(w32win_t *c, canvas_t *s, rect_t r, int f) {
    draw_sunken_box(s, r, T.window_bg);
    static tvitem_t *rows[TV_MAXROWS]; static int deps[TV_MAXROWS];
    int n = tv_walk(c->tv_root, 0, rows, deps, 0, TV_MAXROWS), ih = item_h(f), vis = MAX(1, (r.h - 4) / ih);
    bool sb = n > vis;
    c->tv_scroll = CLAMP(c->tv_scroll, 0, MAX(0, n - vis));
    bool rootbtn = (c->style & 4) != 0;                                       /* TVS_LINESATROOT */
    rect_t o = canvas_push_clip(s, R(r.x + 2, r.y + 2, r.w - 4 - (sb ? 16 : 0), r.h - 4));
    for (int i = c->tv_scroll; i < n && i < c->tv_scroll + vis + 1; i++) {
        tvitem_t *it = rows[i];
        int y = r.y + 2 + (i - c->tv_scroll) * ih, x = r.x + 4 + (deps[i] + (rootbtn ? 1 : 0)) * 16;
        bool kids = it->child || it->cchildren;
        if (kids && (deps[i] > 0 || rootbtn)) {                               /* the [+] / [-] box */
            int bx = x - 13, by = y + (ih - 9) / 2;
            fill_rect(s, bx, by, 9, 9, T.window_bg); draw_rect(s, bx, by, 9, 9, T.gray_text);
            hline(s, bx + 2, by + 4, 5, T.text);
            if (!it->expanded) vline(s, bx + 4, by + 2, 5, T.text);
        }
        int tw = text_width(f, it->text);
        bool sel = it == c->tv_sel;
        if (sel) fill_rect(s, x - 2, y, tw + 4, ih, focus_win == c || (c->style & 0x20) ? T.select : T.face);   /* TVS_SHOWSELALWAYS */
        draw_text(s, f, x, y + 1, it->text, sel && (focus_win == c || (c->style & 0x20)) ? T.select_text : (c->enabled ? T.text : T.gray_text));
    }
    s->clip = o;
    if (sb) draw_scrollbar(s, R(r.x + r.w - 18, r.y + 2, 16, r.h - 4), n, vis, c->tv_scroll, true);
}

/* ---------------------------------------------------------------- drawing */
static void draw_ext(w32win_t *c, canvas_t *s, rect_t r, int f) {
    if (c->ctl == CTL_TREE) { draw_tree(c, s, r, f); return; }
    if (c->ctl == CTL_SCROLLBAR) { if (c->style & 8) bevel_deep(s, r.x, r.y, r.w, r.h, false); else draw_scrollbar(s, r, 10, 10, 0, (c->style & 1) != 0); return; }
    /* combo box: a box with the current text, and the drop-down button */
    bool editable = (c->style & 3) != 3;
    draw_sunken_box(s, r, c->enabled ? T.window_bg : T.face);
    int bw = MIN(17, r.h - 4);
    const char *txt = editable ? c->text : (c->lb_sel >= 0 && c->lb_sel < c->lb_n ? c->lb_items[c->lb_sel] : "");
    rect_t o = canvas_push_clip(s, R(r.x + 2, r.y + 2, r.w - bw - 6, r.h - 4));
    bool hl = !editable && focus_win == c;
    if (hl) fill_rect(s, r.x + 3, r.y + 3, r.w - bw - 8, r.h - 6, T.select);
    draw_text(s, f, r.x + 5, r.y + (r.h - font_height(f)) / 2, txt, hl ? T.select_text : (c->enabled ? T.text : T.gray_text));
    if (editable && focus_win == c) vline(s, r.x + 5 + text_width(f, txt), r.y + 4, r.h - 8, T.text);
    s->clip = o;
    if ((c->style & 3) != 1) {
        rect_t b = R(r.x + r.w - bw - 2, r.y + 2, bw, r.h - 4);
        draw_button(s, b, "", c->hot, c->pressed, false, c->enabled);
        draw_arrow(s, b.x + b.w / 2, b.y + b.h / 2, 1, c->enabled ? T.text : T.gray_text);
    }
}

/* ---------------------------------------------------------------- mouse and keyboard */
static void combo_picked(void *ctx, int id) {
    w32win_t *c = W((uintptr_t)ctx);
    if (!c || id < 1 || id > c->lb_n) return;
    c->lb_sel = id - 1;
    if ((c->style & 3) != 3) strlcpy(c->text, c->lb_items[c->lb_sel], sizeof c->text);
    draw_control(c);
    notify_cmd(c, 1);                                                          /* CBN_SELCHANGE */
    notify_cmd(c, 9);                                                          /* CBN_SELENDOK */
}
static void combo_open(w32win_t *c, window_t *win) {
    if (!c->lb_n) return;
    int n = MIN(c->lb_n, 32);
    kfree(c->cb_menu);
    c->cb_menu = kzalloc(sizeof(menu_item_t) * n);
    for (int i = 0; i < n; i++) { c->cb_menu[i].label = c->lb_items[i][0] ? c->lb_items[i] : " "; c->cb_menu[i].id = i + 1; c->cb_menu[i].flags = i == c->lb_sel ? MI_CHECKED : 0; }
    rect_t cr = wm_client_rect(win);
    int ox, oy; abs_origin(c, &ox, &oy);
    notify_cmd(c, 7);                                                          /* CBN_DROPDOWN */
    popup_menu(cr.x + ox, cr.y + oy + c->h, c->cb_menu, n, combo_picked, c);
}
static void set_focus_ctl(w32win_t *c) {
    w32win_t *old = focus_win;
    if (old == c) return;
    focus_win = c;
    if (old && W((uintptr_t)old) && old->ctl) draw_control(old);
    draw_control(c);
}
static bool ctl_mouse_ext(w32win_t *c, window_t *win, int type, int lx, int ly, int btn) {
    if (!c->enabled) return true;
    int f = ctl_font(c);
    if (c->ctl == CTL_COMBO) {
        if (type == ME_DOWN && (btn & MB_LEFT)) {
            set_focus_ctl(c);
            bool on_button = lx >= c->w - 20 || (c->style & 3) == 3;
            if (on_button) combo_open(c, win);
        }
        return true;
    }
    if (c->ctl == CTL_LISTBOX) {
        int ih = item_h(f), rows = MAX(1, (c->h - 4) / ih);
        if (type == ME_WHEEL) { c->lb_top = CLAMP(c->lb_top + btn * 3, 0, MAX(0, c->lb_n - rows)); draw_control(c); return true; }
        if (c->lb_n > rows && lx >= c->w - 18 && (type == ME_DOWN || type == ME_DBLCLICK)) {   /* scroll bar: page by clicks */
            c->lb_top = CLAMP(c->lb_top + (ly < c->h / 2 ? -rows : rows), 0, MAX(0, c->lb_n - rows)); draw_control(c); return true;
        }
        if (type != ME_DOWN && type != ME_DBLCLICK) return true;
        set_focus_ctl(c);
        int i = c->lb_top + (ly - 2) / ih;
        if (i < 0 || i >= c->lb_n) return true;
        if (lb_multi(c)) { if (c->style & 0x8 || keystate[0x11]) c->lb_selbits[i] ^= 1; else { memset(c->lb_selbits, 0, c->lb_n); c->lb_selbits[i] = 1; } }
        c->lb_sel = i; draw_control(c);
        if (c->style & 1) notify_cmd(c, type == ME_DBLCLICK ? 2 : 1);         /* LBS_NOTIFY: LBN_DBLCLK / LBN_SELCHANGE */
        return true;
    }
    if (c->ctl == CTL_TREE) {
        static tvitem_t *rows[TV_MAXROWS]; static int deps[TV_MAXROWS];
        int n = tv_walk(c->tv_root, 0, rows, deps, 0, TV_MAXROWS), ih = item_h(f), vis = MAX(1, (c->h - 4) / ih);
        if (type == ME_WHEEL) { c->tv_scroll = CLAMP(c->tv_scroll + btn * 3, 0, MAX(0, n - vis)); draw_control(c); return true; }
        if (type != ME_DOWN && type != ME_DBLCLICK) return true;
        set_focus_ctl(c);
        if (n > vis && lx >= c->w - 18) { c->tv_scroll = CLAMP(c->tv_scroll + (ly < c->h / 2 ? -vis : vis), 0, MAX(0, n - vis)); draw_control(c); return true; }
        int i = c->tv_scroll + (ly - 2) / ih;
        if (i < 0 || i >= n) return true;
        tvitem_t *it = rows[i];
        int x = 4 + (deps[i] + ((c->style & 4) ? 1 : 0)) * 16;
        bool kids = it->child || it->cchildren;
        if (kids && (lx < x - 1 || type == ME_DBLCLICK)) { it->expanded = !it->expanded; draw_control(c); if (lx < x - 1) return true; }
        tv_select(c, it, 1);                                                    /* TVC_BYMOUSE */
        return true;
    }
    return false;
}
static bool ctl_key_ext(w32win_t *c, key_event_t *e) {
    if (!e->pressed) return false;
    int k = e->key;
    if (c->ctl == CTL_LISTBOX || (c->ctl == CTL_COMBO && (k == KEY_UP || k == KEY_DOWN))) {
        int d = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -8 : k == KEY_PGDN ? 8 : k == KEY_HOME ? -100000 : k == KEY_END ? 100000 : 0;
        if (!d) return false;
        if (!c->lb_n) return true;
        int ns = CLAMP((c->lb_sel < 0 ? 0 : c->lb_sel) + d, 0, c->lb_n - 1);
        if (ns == c->lb_sel) return true;
        c->lb_sel = ns;
        if (lb_multi(c)) { memset(c->lb_selbits, 0, c->lb_n); c->lb_selbits[ns] = 1; }
        if (c->ctl == CTL_COMBO && (c->style & 3) != 3) strlcpy(c->text, c->lb_items[ns], sizeof c->text);
        if (c->ctl == CTL_LISTBOX) lb_ensure_visible(c, ctl_font(c));
        draw_control(c);
        if (c->ctl == CTL_COMBO) notify_cmd(c, 1); else if (c->style & 1) notify_cmd(c, 1);
        return true;
    }
    if (c->ctl == CTL_COMBO) {                                                  /* typing into an editable combo box */
        if ((c->style & 3) == 3) return k != KEY_TAB && k != KEY_ENTER && k != KEY_ESC;
        int l = strlen(c->text);
        if (k == KEY_BACKSPACE) { if (l) c->text[l - 1] = 0; }
        else if (e->ch >= 32 && e->ch < 127 && l < (int)sizeof c->text - 2) { c->text[l] = e->ch; c->text[l + 1] = 0; }
        else return false;
        draw_control(c); notify_cmd(c, 5);                                     /* CBN_EDITCHANGE */
        return true;
    }
    if (c->ctl == CTL_TREE) {
        static tvitem_t *rows[TV_MAXROWS]; static int deps[TV_MAXROWS];
        int n = tv_walk(c->tv_root, 0, rows, deps, 0, TV_MAXROWS), i = 0;
        while (i < n && rows[i] != c->tv_sel) i++;
        tvitem_t *it = c->tv_sel;
        if (k == KEY_UP && i > 0 && i < n) tv_select(c, rows[i - 1], 2);
        else if (k == KEY_DOWN && i + 1 < n) tv_select(c, rows[i + 1], 2);
        else if (k == KEY_RIGHT && it && (it->child || it->cchildren)) { if (!it->expanded) { it->expanded = true; draw_control(c); } else if (it->child) tv_select(c, it->child, 2); }
        else if (k == KEY_LEFT && it) { if (it->expanded) { it->expanded = false; draw_control(c); } else if (it->parent) tv_select(c, it->parent, 2); }
        else if (k == KEY_HOME && n) tv_select(c, rows[0], 2);
        else if (k == KEY_END && n) tv_select(c, rows[n - 1], 2);
        else return false;
        return true;
    }
    return false;
}

/* radio buttons: one checked per group (from a WS_GROUP control to the next one) */
static void radio_group_select(w32win_t *b) {
    if (!b->parent) { b->checked = 1; draw_control(b); return; }
    w32win_t *start = b->parent->child;
    for (w32win_t *x = b->parent->child; x && x != b; x = x->next) if (x->style & WS_GROUP) start = x;
    for (w32win_t *x = start; x; x = x->next) {
        if (x != start && (x->style & WS_GROUP)) break;
        int bs = x->style & 0xF;
        if (x->ctl == CTL_BUTTON && (bs == 4 || bs == 9)) { bool on = x == b; if (x->checked != on) { x->checked = on; draw_control(x); } }
    }
}

/* ---------------------------------------------------------------- control messages */
#define CB_ERR ((LRESULT)-1)
static LRESULT str_out(w32win_t *w, const char *s, LPARAM lp, bool wide, int cap) {
    if (!lp) return strlen(s);
    if (wide) return u2w(s, (WCHAR *)lp, cap > 0 ? cap : 4096);
    if (cap > 0) strlcpy((char *)lp, s, cap); else strcpy((char *)lp, s);
    return strlen(s);
}
static bool list_msg(w32win_t *w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res, bool wide) {
    bool combo = w->ctl == CTL_COMBO;
    /* the combo and list box messages differ by a constant: map CB_ to LB_ codes */
    static const struct { UINT cb, lb; } map[] = {
        { 0x143, 0x180 }, { 0x144, 0x182 }, { 0x146, 0x18B }, { 0x147, 0x188 }, { 0x148, 0x189 }, { 0x149, 0x18A }, { 0x14A, 0x181 },
        { 0x14B, 0x184 }, { 0x14C, 0x18F }, { 0x14D, 0x18C }, { 0x14E, 0x186 }, { 0x150, 0x199 }, { 0x151, 0x19A }, { 0x158, 0x1A2 },
    };
    UINT m = msg;
    if (combo) { m = 0; for (unsigned i = 0; i < ARRAY_LEN(map); i++) if (map[i].cb == msg) m = map[i].lb; }
    char tmp[1024];
    const char *str = NULL;
    if (m == 0x180 || m == 0x181 || m == 0x18F || m == 0x18C || m == 0x1A2) {
        if (wide) { w2u((const WCHAR *)lp, tmp, sizeof tmp); str = tmp; } else str = lp ? (const char *)lp : "";
    }
    switch (m) {
    case 0x180: { int at = (w->style & 2) || (combo && (w->style & 0x100)) ? list_sorted_pos(w, str) : w->lb_n; list_insert(w, at, str); draw_control(w); *res = at; return true; }   /* LB_ADDSTRING (LBS_SORT / CBS_SORT) */
    case 0x181: { int at = (int)wp < 0 ? w->lb_n : (int)wp; list_insert(w, at, str); draw_control(w); *res = at; return true; }   /* LB_INSERTSTRING */
    case 0x182: if ((int)wp >= w->lb_n) { *res = CB_ERR; return true; } list_delete(w, wp); draw_control(w); *res = w->lb_n; return true;
    case 0x184: list_clear(w); if (combo) w->text[0] = 0; draw_control(w); *res = 0; return true;
    case 0x18B: *res = w->lb_n; return true;
    case 0x188: *res = w->lb_sel >= 0 && w->lb_sel < w->lb_n ? w->lb_sel : CB_ERR; return true;
    case 0x186:                                                                         /* LB_SETCURSEL */
        w->lb_sel = (int)wp < w->lb_n ? (int)wp : -1;
        if (lb_multi(w)) { memset(w->lb_selbits, 0, w->lb_n); if (w->lb_sel >= 0) w->lb_selbits[w->lb_sel] = 1; }
        if (combo && (w->style & 3) != 3) strlcpy(w->text, w->lb_sel >= 0 ? w->lb_items[w->lb_sel] : "", sizeof w->text);
        if (!combo) lb_ensure_visible(w, ctl_font(w));
        draw_control(w); *res = w->lb_sel >= 0 ? w->lb_sel : CB_ERR; return true;
    case 0x189: if ((int)wp < 0 || (int)wp >= w->lb_n) { *res = CB_ERR; return true; } *res = str_out(w, w->lb_items[wp], lp, wide, 0); return true;   /* LB_GETTEXT */
    case 0x18A: *res = (int)wp >= 0 && (int)wp < w->lb_n ? (LRESULT)strlen(w->lb_items[wp]) : CB_ERR; return true;
    case 0x18F: case 0x1A2: *res = list_find(w, (int)wp, str, m == 0x1A2); return true;       /* LB_FINDSTRING(EXACT) */
    case 0x18C: { int i = list_find(w, (int)wp, str, false); if (i >= 0) { list_msg(w, combo ? 0x14E : 0x186, i, 0, res, false); } *res = i; return true; }
    case 0x199: *res = (int)wp >= 0 && (int)wp < w->lb_n ? (LRESULT)w->lb_data[wp] : CB_ERR; return true;
    case 0x19A: if ((int)wp >= 0 && (int)wp < w->lb_n) { w->lb_data[wp] = lp; *res = 0; } else *res = CB_ERR; return true;
    }
    if (combo) {
        switch (msg) {
        case 0x14F: *res = 1; return true;                                              /* CB_SHOWDROPDOWN */
        case 0x157: *res = 0; return true;                                              /* CB_GETDROPPEDSTATE */
        case 0x141: case 0x142: case 0x155: case 0x160: case 0x153: *res = 1; return true;
        case 0x140: *res = (LRESULT)((uint32_t)strlen(w->text) << 16); return true;      /* CB_GETEDITSEL */
        case 0x154: *res = item_h(ctl_font(w)); return true;                             /* CB_GETITEMHEIGHT */
        case 0x152: if (lp) { RECT32 *r = (RECT32 *)lp; GetWindowRect((HWND)w, r); r->b = r->t + w->cb_droph; } *res = 1; return true;
        }
        return false;
    }
    switch (msg) {                                                                       /* list box only */
    case 0x185: {                                                                        /* LB_SETSEL (wp: select, lp: index or -1) */
        int i = (int)lp;
        if (lb_multi(w)) { if (i < 0) memset(w->lb_selbits, wp ? 1 : 0, w->lb_n); else if (i < w->lb_n) w->lb_selbits[i] = wp ? 1 : 0; }
        else if (wp && i < w->lb_n) w->lb_sel = i;
        draw_control(w); *res = 0; return true;
    }
    case 0x187: *res = (int)wp >= 0 && (int)wp < w->lb_n ? (lb_multi(w) ? w->lb_selbits[wp] : (int)wp == w->lb_sel) : CB_ERR; return true;   /* LB_GETSEL */
    case 0x190: { int n = 0; if (lb_multi(w)) { for (int i = 0; i < w->lb_n; i++) n += w->lb_selbits[i]; } else n = w->lb_sel >= 0; *res = n; return true; }
    case 0x191: { int n = 0, *out = (int *)lp; for (int i = 0; i < w->lb_n && n < (int)wp; i++) if (lb_multi(w) ? w->lb_selbits[i] : i == w->lb_sel) out[n++] = i; *res = n; return true; }
    case 0x18E: *res = w->lb_top; return true;                                           /* LB_GETTOPINDEX */
    case 0x197: w->lb_top = CLAMP((int)wp, 0, MAX(0, w->lb_n - 1)); draw_control(w); *res = 0; return true;
    case 0x192: case 0x194: case 0x195: case 0x1A0: case 0x1A5: *res = 1; return true;   /* tab stops, extents, item height: accepted */
    case 0x1A1: *res = item_h(ctl_font(w)); return true;                                 /* LB_GETITEMHEIGHT */
    case 0x198: { RECT32 *r = (RECT32 *)lp; int ih = item_h(ctl_font(w)); if (r) { r->l = 2; r->r = w->w - 2; r->t = 2 + ((int)wp - w->lb_top) * ih; r->b = r->t + ih; } *res = 1; return true; }
    case 0x19F: *res = w->lb_sel; return true;                                           /* LB_GETCARETINDEX */
    case 0x19E: w->lb_sel = wp; draw_control(w); *res = 0; return true;                  /* LB_SETCARETINDEX */
    }
    return false;
}
static bool tree_msg(w32win_t *w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res) {
    switch (msg) {
    case 0x1100: case 0x1132: {                                                          /* TVM_INSERTITEMA / W */
        bool wide = msg == 0x1132; if (wide) w->tv_unicode = true;
        const TVINSERT_ *ins = (const TVINSERT_ *)lp;
        tvitem_t *parent = ins->hParent == TVI_ROOT || !ins->hParent ? NULL : TVI(w, ins->hParent);
        if (ins->hParent && ins->hParent != TVI_ROOT && !parent) { *res = 0; return true; }
        tvitem_t *it = kzalloc(sizeof *it);
        tv_text_in(w, it, &ins->item, wide);
        it->parent = parent;
        tvitem_t **list = tv_list_of(w, parent);
        if (ins->hInsertAfter == TVI_FIRST) { it->next = *list; *list = it; }
        else if (ins->hInsertAfter == TVI_SORT) { tvitem_t **pp = list; while (*pp && strcasecmp((*pp)->text, it->text) <= 0) pp = &(*pp)->next; it->next = *pp; *pp = it; }
        else {
            tvitem_t *after = ins->hInsertAfter == TVI_LAST ? NULL : TVI(w, ins->hInsertAfter);
            tvitem_t **pp = list;
            if (after && after->parent == parent) { it->next = after->next; after->next = it; }
            else { while (*pp) pp = &(*pp)->next; *pp = it; }
        }
        draw_control(w); *res = (LRESULT)it; return true;
    }
    case 0x1101: {                                                                       /* TVM_DELETEITEM */
        if (lp == TVI_ROOT || !lp) { w32_tv_free_all(w); draw_control(w); *res = 1; return true; }
        tvitem_t *it = TVI(w, lp); if (!it) { *res = 0; return true; }
        tvitem_t **pp = tv_list_of(w, it->parent);
        while (*pp && *pp != it) pp = &(*pp)->next;
        if (*pp) *pp = it->next;
        it->next = NULL; tv_free_list(w, it);
        draw_control(w); *res = 1; return true;
    }
    case 0x1102: {                                                                       /* TVM_EXPAND */
        tvitem_t *it = TVI(w, lp); if (!it) { *res = 0; return true; }
        int a = wp & 3; it->expanded = a == 2 ? true : a == 1 ? false : a == 3 ? !it->expanded : it->expanded;
        draw_control(w); *res = 1; return true;
    }
    case 0x1105: { static tvitem_t *rows[TV_MAXROWS]; static int deps[TV_MAXROWS]; int n = 0; for (tvitem_t *x = w->tv_root; x; x = x->next) n++; (void)rows; (void)deps; *res = n; return true; }
    case 0x110A: {                                                                       /* TVM_GETNEXTITEM */
        tvitem_t *it = TVI(w, lp); *res = 0;
        switch (wp) {
        case 0: *res = (LRESULT)w->tv_root; break;                                       /* TVGN_ROOT */
        case 1: *res = it ? (LRESULT)it->next : 0; break;                                /* NEXT */
        case 2: if (it) { tvitem_t *p = *tv_list_of(w, it->parent); tvitem_t *prev = NULL; while (p && p != it) { prev = p; p = p->next; } *res = (LRESULT)prev; } break;
        case 3: *res = it ? (LRESULT)it->parent : 0; break;                              /* PARENT */
        case 4: *res = it ? (LRESULT)it->child : (LRESULT)w->tv_root; break;             /* CHILD */
        case 5: *res = (LRESULT)w->tv_root; break;                                       /* FIRSTVISIBLE */
        case 9: *res = (LRESULT)w->tv_sel; break;                                        /* CARET */
        case 6: case 7: {
            static tvitem_t *rows[TV_MAXROWS]; static int deps[TV_MAXROWS];
            int n = tv_walk(w->tv_root, 0, rows, deps, 0, TV_MAXROWS);
            for (int i = 0; i < n; i++) if (rows[i] == it) { int j = wp == 6 ? i + 1 : i - 1; *res = j >= 0 && j < n ? (LRESULT)rows[j] : 0; }
            break;
        }
        }
        return true;
    }
    case 0x110B: {                                                                       /* TVM_SELECTITEM */
        tvitem_t *it = lp ? TVI(w, lp) : NULL;
        if (lp && !it) { *res = 0; return true; }
        if (wp == 9 || wp == 0x8009) tv_select(w, it, 0); else if (wp == 5 && it) { draw_control(w); }
        *res = 1; return true;
    }
    case 0x110C: case 0x113E: {                                                          /* TVM_GETITEMA / W */
        TVITEM_ *ti = (TVITEM_ *)lp; tvitem_t *it = ti ? TVI(w, ti->hItem) : NULL;
        if (!it) { *res = 0; return true; }
        bool save = w->tv_unicode; w->tv_unicode = msg == 0x113E; tv_text_out(w, it, ti); w->tv_unicode = save;
        *res = 1; return true;
    }
    case 0x110D: case 0x113F: {                                                          /* TVM_SETITEMA / W */
        TVITEM_ *ti = (TVITEM_ *)lp; tvitem_t *it = ti ? TVI(w, ti->hItem) : NULL;
        if (!it) { *res = 0; return true; }
        tv_text_in(w, it, ti, msg == 0x113F); draw_control(w); *res = 1; return true;
    }
    case 0x1114: { tvitem_t *it = TVI(w, lp); for (tvitem_t *p = it ? it->parent : NULL; p; p = p->parent) p->expanded = true; draw_control(w); *res = 1; return true; }   /* TVM_ENSUREVISIBLE */
    case 0x1104: { RECT32 *r = (RECT32 *)lp; if (r) { r->l = 0; r->t = 0; r->r = w->w; r->b = item_h(ctl_font(w)); } *res = 1; return true; }   /* TVM_GETITEMRECT */
    case 0x1110: *res = MAX(1, (w->h - 4) / item_h(ctl_font(w))); return true;             /* TVM_GETVISIBLECOUNT */
    case 0x1106: case 0x1107: case 0x1108: case 0x1109: case 0x1113: case 0x111B: case 0x111D: case 0x112C: *res = 0; return true;   /* indent, image lists, sorting, colours */
    }
    return false;
}
static bool ctl_msg_ext(w32win_t *w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *res) {
    if (w->ctl >= CTL_TOOLBAR && cc_msg(w, msg, wp, lp, res)) return true;
    if (w->ctl == CTL_LISTBOX || w->ctl == CTL_COMBO) {
        if (list_msg(w, msg, wp, lp, res, false)) return true;
        if (w->ctl == CTL_COMBO && msg == WM_SETTEXT) { strlcpy(w->text, lp ? (const char *)lp : "", sizeof w->text); draw_control(w); *res = 1; return true; }
    }
    if (w->ctl == CTL_TREE && tree_msg(w, msg, wp, lp, res)) return true;
    if (w->ctl == CTL_EDIT) {
        switch (msg) {
        case 0xCF: if (wp) w->style |= 0x800; else w->style &= ~0x800u; draw_control(w); *res = 1; return true;   /* EM_SETREADONLY */
        case 0xC5: w->limit = (int)wp; *res = 0; return true;                            /* EM_LIMITTEXT */
        case 0xD5: *res = w->limit ? w->limit : 30000; return true;                      /* EM_GETLIMITTEXT */
        case 0xBA: { int n = 1; for (char *p = w->text; *p; p++) n += *p == '\n'; *res = n; return true; }   /* EM_GETLINECOUNT */
        case 0xB1: case 0xB7: case 0xB6: case 0xCC: case 0xD3: case 0xB9: case 0xCB: *res = 1; return true;     /* selection, scrolling, margins, modify flag */
        case 0xB0: { uint32_t l = strlen(w->text); if (wp) *(DWORD *)wp = l; if (lp) *(DWORD *)lp = l; *res = (LRESULT)(l | (l << 16)); return true; }   /* EM_GETSEL */
        case 0xC2: strlcat(w->text, lp ? (const char *)lp : "", sizeof w->text); draw_control(w); *res = 0; return true;   /* EM_REPLACESEL: at the end */
        case 0xB8: *res = 0; return true;                                                /* EM_GETMODIFY */
        }
    }
    if (w->ctl == CTL_BUTTON) {
        switch (msg) {
        case 0xF5: {                                                                     /* BM_CLICK */
            int bs = w->style & 0xF;
            if (bs == 3) w->checked = !w->checked; else if (bs == 9) radio_group_select(w);
            draw_control(w); notify_cmd(w, BN_CLICKED); *res = 0; return true;
        }
        case 0xF4: w->style = (w->style & ~0xFFFFu) | (wp & 0xFFFF); draw_control(w); *res = 0; return true;   /* BM_SETSTYLE */
        case 0xF2: *res = (w->checked ? 1 : 0) | (w->pressed ? 4 : 0) | (focus_win == w ? 8 : 0); return true;   /* BM_GETSTATE */
        case 0xF3: w->pressed = wp != 0; draw_control(w); *res = 0; return true;          /* BM_SETSTATE */
        case 0xF7: case 0xF6: *res = 0; return true;                                      /* BM_SETIMAGE / GETIMAGE */
        }
    }
    if (msg == 0x0B) { *res = 0; return true; }                                          /* WM_SETREDRAW */
    if (msg == 0x87) { *res = w->ctl == CTL_EDIT ? 0x81 : w->ctl == CTL_LISTBOX || w->ctl == CTL_TREE ? 0x05 : w->ctl == CTL_BUTTON ? 0x2001 : 0; return true; }   /* WM_GETDLGCODE */
    return false;
}

/* ---------------------------------------------------------------- dialog boxes */
static int dlg_bux, dlg_buy;                                    /* dialog base units, in pixels */
static void dlg_units(void) {
    if (dlg_bux) return;
    int f = FONT_UI;
    dlg_bux = (text_width(f, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") / 26 + 1) / 2;
    if (dlg_bux < 5) dlg_bux = 6;
    dlg_buy = font_height(f) + 1;
    if (dlg_buy < 12) dlg_buy = 13;
}
static int du_x(int v) { dlg_units(); return v * dlg_bux / 4; }
static int du_y(int v) { dlg_units(); return v * dlg_buy / 8; }
static const uint8_t *align4(const uint8_t *p, const uint8_t *base) { return base + (((p - base) + 3) & ~3); }
/* sz_Or_Ord: 0 (none), 0xFFFF and an ordinal, or a UTF-16 string */
static const uint8_t *sz_or_ord(const uint8_t *p, char *out, int cap, uint16_t *ord) {
    uint16_t v; memcpy(&v, p, 2);
    if (ord) *ord = 0;
    if (v == 0) { if (cap) out[0] = 0; return p + 2; }
    if (v == 0xFFFF) { uint16_t o; memcpy(&o, p + 2, 2); if (ord) *ord = o; if (cap) out[0] = 0; return p + 4; }
    const WCHAR *w = (const WCHAR *)p; int n = w_len(w);
    w2u(w, out, cap);
    return p + 2 * (n + 1);
}
static void dlg_set(w32win_t *d, int idx, LONG_PTR v) { if (d->extra && idx + (int)sizeof v <= d->nextra) memcpy(d->extra + idx, &v, sizeof v); }
static LONG_PTR dlg_get(w32win_t *d, int idx) { LONG_PTR v = 0; if (d->extra && idx + (int)sizeof v <= d->nextra) memcpy(&v, d->extra + idx, sizeof v); return v; }
static w32win_t *first_tabstop(w32win_t *d) { for (w32win_t *c = d->child; c; c = c->next) if (!c->destroyed && c->visible && c->enabled && (c->style & WS_TABSTOP)) return c; return NULL; }
static LRESULT WINAPI DefDlgProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static bool dlg_class_ready;
static HWND create_dialog(const uint8_t *tpl, HWND owner, void *dlgproc, LPARAM param, bool wide) {
    if (!tpl) return 0;
    w32_proc_t *p = P;
    if (!dlg_class_ready) {                                     /* the system dialog class, shared */
        w32cls_t *c = kzalloc(sizeof *c); strcpy(c->name, "#32770"); c->proc = (void *)DefDlgProcA; c->wnd_extra = 64; c->brush = 0;
        c->next = classes; classes = c; dlg_class_ready = true;
    }
    const uint8_t *q = tpl;
    uint16_t w0, w1; memcpy(&w0, q, 2); memcpy(&w1, q + 2, 2);
    bool ex = w0 == 1 && w1 == 0xFFFF;
    uint32_t style, exstyle; uint16_t n; int16_t x, y, cx, cy;
    if (ex) { memcpy(&exstyle, q + 8, 4); memcpy(&style, q + 12, 4); memcpy(&n, q + 16, 2); memcpy(&x, q + 18, 2); memcpy(&y, q + 20, 2); memcpy(&cx, q + 22, 2); memcpy(&cy, q + 24, 2); q += 26; }
    else { memcpy(&style, q, 4); memcpy(&exstyle, q + 4, 4); memcpy(&n, q + 8, 2); memcpy(&x, q + 10, 2); memcpy(&y, q + 12, 2); memcpy(&cx, q + 14, 2); memcpy(&cy, q + 16, 2); q += 18; }
    char menu[64], cls[64], title[256], face[64] = "";
    uint16_t menu_ord, cls_ord;
    q = sz_or_ord(q, menu, sizeof menu, &menu_ord);
    q = sz_or_ord(q, cls, sizeof cls, &cls_ord);
    q = sz_or_ord(q, title, sizeof title, NULL);
    if (style & 0x48) {                                         /* DS_SETFONT / DS_SHELLFONT: point size (weight, italic, charset), face */
        q += ex ? 6 : 2;
        q = sz_or_ord(q, face, sizeof face, NULL);
    }
    (void)face;
    const char *clsname = cls[0] ? cls : "#32770";
    if (!find_class(clsname)) clsname = "#32770";
    int cw = du_x(cx), ch = du_y(cy);
    uint32_t wstyle = (style & ~WS_VISIBLE & ~WS_CHILD) | 0x80000000u;   /* a top-level (popup) window */
    if (style & WS_CHILD) wstyle = style & ~WS_VISIBLE;                   /* a child dialog (property pages) */
    w32win_t *ow = W(owner);
    HWND h;
    if (style & WS_CHILD) h = CreateWindowExA(exstyle, clsname, title, wstyle, du_x(x), du_y(y), cw, ch, owner, 0, p->base, NULL);
    else h = CreateWindowExA(exstyle, clsname, title, wstyle | WS_CAPTION, (int)CW_USEDEFAULT, (int)CW_USEDEFAULT, cw + 2 * T.border, ch + T.title_h + T.border, owner, 0, p->base, NULL);
    w32win_t *d = W(h);
    if (!d) return 0;
    d->is_dialog = true;
    d->unicode = wide || d->unicode;
    if (d->nextra < DWLP_USER + (int)sizeof(LONG_PTR)) {                 /* DLGWINDOWEXTRA, whatever the class asked for */
        uint8_t *nx = kzalloc(64); if (d->extra) memcpy(nx, d->extra, d->nextra); kfree(d->extra); d->extra = nx; d->nextra = 64;
    }
    dlg_set(d, DWLP_DLGPROC, (LONG_PTR)dlgproc);
    gdiobj_t *font = newobj(OBJ_FONT); font->font = FONT_UI;
    d->font = (uintptr_t)font;
    if (d->top) {
        if (ow && top_of(ow)->top) { d->top->owner = top_of(ow)->top; }
        d->top->icon = ICON_EXE;
        wm_center(d->top);
    }
    /* the controls */
    for (int i = 0; i < n; i++) {
        q = align4(q, tpl);
        uint32_t cs, cex, id; int16_t ix, iy, icx, icy;
        if (ex) { memcpy(&cex, q + 4, 4); memcpy(&cs, q + 8, 4); memcpy(&ix, q + 12, 2); memcpy(&iy, q + 14, 2); memcpy(&icx, q + 16, 2); memcpy(&icy, q + 18, 2); memcpy(&id, q + 20, 4); q += 24; }
        else { uint16_t id16; memcpy(&cs, q, 4); memcpy(&cex, q + 4, 4); memcpy(&ix, q + 8, 2); memcpy(&iy, q + 10, 2); memcpy(&icx, q + 12, 2); memcpy(&icy, q + 14, 2); memcpy(&id16, q + 16, 2); id = id16; q += 18; }
        char ccls[64], ctext[1024]; uint16_t cord, tord;
        q = sz_or_ord(q, ccls, sizeof ccls, &cord);
        q = sz_or_ord(q, ctext, sizeof ctext, &tord);
        uint16_t extra; memcpy(&extra, q, 2); q += 2 + extra;
        if (cord) { static const char *atoms[] = { "BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX" }; strcpy(ccls, cord >= 0x80 && cord <= 0x85 ? atoms[cord - 0x80] : "STATIC"); }
        HWND c = CreateWindowExA(cex, ccls, ctext, cs | WS_CHILD, du_x(ix), du_y(iy), du_x(icx), du_y(icy), h, (HANDLE)(uintptr_t)id, p->base, NULL);
        if (c) send(W(c), WM_SETFONT, d->font, 0);
        else klog("w32: dialog control %s (id %u) could not be created", ccls, id);
    }
    /* WM_INITDIALOG: TRUE means "give the first control the focus" */
    w32win_t *ft = first_tabstop(d);
    LRESULT r = send(d, 0x110, (uintptr_t)ft, param);                    /* WM_INITDIALOG */
    if (!W(h)) return 0;
    if (r && ft && W((uintptr_t)ft)) { focus_win = ft; draw_control(ft); }
    if (style & WS_VISIBLE) ShowWindow(h, 5);
    return h;
}
static const uint8_t *dialog_template(HINSTANCE inst, uintptr_t name, bool wide) {
    w32_proc_t *p = P;
    uintptr_t r = w32_find_resource(p, 5, name, wide, 0);
    return r ? (const uint8_t *)w32_resource_data(p, r, NULL) : NULL;
}
static HWND WINAPI CreateDialogIndirectParamA(HINSTANCE i, const void *tpl, HWND owner, void *fn, LPARAM lp) { return create_dialog(tpl, owner, fn, lp, false); }
static HWND WINAPI CreateDialogIndirectParamW(HINSTANCE i, const void *tpl, HWND owner, void *fn, LPARAM lp) { return create_dialog(tpl, owner, fn, lp, true); }
static HWND WINAPI CreateDialogParamA(HINSTANCE i, const char *name, HWND owner, void *fn, LPARAM lp) {
    const uint8_t *t = dialog_template(i, (uintptr_t)name, false);
    if (!t) { P->last_error = 1814; klog("w32: CreateDialog: no dialog template %p", name); return 0; }
    return create_dialog(t, owner, fn, lp, false);
}
static HWND WINAPI CreateDialogParamW(HINSTANCE i, const WCHAR *name, HWND owner, void *fn, LPARAM lp) {
    const uint8_t *t = dialog_template(i, (uintptr_t)name, true);
    if (!t) { P->last_error = 1814; return 0; }
    return create_dialog(t, owner, fn, lp, true);
}
static BOOL WINAPI IsDialogMessageA(HWND hdlg, MSG32 *m);
static LRESULT WINAPI DispatchMessageA(const MSG32 *m);
/* a modal dialog: its own message loop until EndDialog */
static intptr_t run_modal(HWND h, HWND owner) {
    w32win_t *d = W(h); if (!d) return -1;
    w32_proc_t *p = P;
    d->dlg_modal = true;
    w32win_t *ow = W(owner);
    if (d->top && ow && top_of(ow)->top) { d->top->owner = top_of(ow)->top; d->top->flags |= WF_MODAL; }
    ShowWindow(h, 5);
    if (d->top) wm_focus(d->top);
    while (W(h) && !d->dlg_ended) {
        MSG32 m;
        if (!get_msg(p, &m, true)) { task_sleep(8); continue; }
        if (m.message == WM_QUIT) { p->quit_posted = true; p->quit_code = m.wParam; break; }
        if (!IsDialogMessageA(h, &m)) DispatchMessageA(&m);
    }
    intptr_t r = W(h) ? d->dlg_ret : -1;
    if (W(h)) DestroyWindow(h);
    if (ow && top_of(ow)->top) wm_focus(top_of(ow)->top);
    return r;
}
static intptr_t WINAPI DialogBoxIndirectParamA(HINSTANCE i, const void *tpl, HWND owner, void *fn, LPARAM lp) { HWND h = create_dialog(tpl, owner, fn, lp, false); return h ? run_modal(h, owner) : -1; }
static intptr_t WINAPI DialogBoxIndirectParamW(HINSTANCE i, const void *tpl, HWND owner, void *fn, LPARAM lp) { HWND h = create_dialog(tpl, owner, fn, lp, true); return h ? run_modal(h, owner) : -1; }
static intptr_t WINAPI DialogBoxParamA(HINSTANCE i, const char *name, HWND owner, void *fn, LPARAM lp) {
    const uint8_t *t = dialog_template(i, (uintptr_t)name, false);
    if (!t) { P->last_error = 1814; klog("w32: DialogBox: no dialog template %p", name); return -1; }
    return DialogBoxIndirectParamA(i, t, owner, fn, lp);
}
static intptr_t WINAPI DialogBoxParamW(HINSTANCE i, const WCHAR *name, HWND owner, void *fn, LPARAM lp) {
    const uint8_t *t = dialog_template(i, (uintptr_t)name, true);
    if (!t) { P->last_error = 1814; return -1; }
    return DialogBoxIndirectParamW(i, t, owner, fn, lp);
}
static BOOL WINAPI EndDialog(HWND h, intptr_t r) {
    w32win_t *d = W(h); if (!d) return 0;
    d->dlg_ended = true; d->dlg_ret = r;
    if (!d->dlg_modal) ShowWindow(h, 0);
    return 1;
}
/* the dialog engine */
static LRESULT dlg_default(w32win_t *d, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: { w32win_t *c = NULL; for (w32win_t *x = d->child; x; x = x->next) if ((int)x->id == IDCANCEL && !x->destroyed) c = x;
        if (c && !c->enabled) return 0;
        send(d, WM_COMMAND, IDCANCEL | ((uint32_t)BN_CLICKED << 16), (uintptr_t)c); return 0; }
    case WM_ERASEBKGND: {
        dc_t *dc = DC(wp); if (!dc) return 1;
        rect_t sv; dc_begin(dc, &sv); fill_rect(dc->surf, dc->ox, dc->oy, d->w, d->h, T.face); dc_end(dc, sv);
        return 1;
    }
    case 0x110: return 0;                                                       /* WM_INITDIALOG not handled */
    case 0x135: case 0x136: case 0x138: case 0x133: case 0x134: case 0x132: case 0x137: return (LRESULT)&stock[0];   /* WM_CTLCOLOR*: the white brush */
    case 0x28: {                                                                 /* WM_NEXTDLGCTL */
        w32win_t *t = lp ? W(wp) : NULL;
        if (!lp) { extern HWND WINAPI GetNextDlgTabItem(HWND, HWND, BOOL); t = W(GetNextDlgTabItem((HWND)d, (HWND)focus_win, wp != 0)); }
        if (t) set_focus_ctl(t);
        return 0;
    }
    }
    return default_proc(d, msg, wp, lp);
}
static LRESULT WINAPI DefDlgProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    w32win_t *d = W(h); if (!d) return 0;
    void *fn = (void *)dlg_get(d, DWLP_DLGPROC);
    if (fn) {
        dlg_set(d, DWLP_MSGRESULT, 0);
        LRESULT r = call_wndproc(fn, h, msg, wp, lp);
        d = W(h); if (!d) return r;
        if (r) {
            switch (msg) {
            case 0x110: case 0x132: case 0x133: case 0x134: case 0x135: case 0x136: case 0x137: case 0x138:
            case 0x39: case 0x2E: case 0x2F: case 0x37: return r;               /* WM_INITDIALOG, CTLCOLOR, COMPAREITEM, VKEYTOITEM, CHARTOITEM, QUERYDRAGICON */
            }
            return dlg_get(d, DWLP_MSGRESULT);
        }
    }
    return dlg_default(d, msg, wp, lp);
}
/* Tab moves the focus, Enter presses the default button, Esc is Cancel */
HWND WINAPI GetNextDlgTabItem(HWND hdlg, HWND hctl, BOOL prev) {
    w32win_t *d = W(hdlg); if (!d) return 0;
    w32win_t *list[256]; int n = 0, cur = -1;
    for (w32win_t *c = d->child; c && n < 256; c = c->next) if (!c->destroyed && c->visible && c->enabled && (c->style & WS_TABSTOP)) { if ((HWND)c == hctl) cur = n; list[n++] = c; }
    if (!n) return 0;
    int i = cur < 0 ? (prev ? n - 1 : 0) : (cur + (prev ? n - 1 : 1)) % n;
    return (HWND)list[i];
}
static HWND WINAPI GetNextDlgGroupItem(HWND hdlg, HWND hctl, BOOL prev) { return GetNextDlgTabItem(hdlg, hctl, prev); }
static bool is_inside(w32win_t *c, w32win_t *d) { for (; c; c = c->parent) if (c == d) return true; return false; }
static BOOL WINAPI IsDialogMessageA(HWND hdlg, MSG32 *m) {
    w32win_t *d = W(hdlg), *t = W(m->hwnd);
    if (!d || !t || !is_inside(t, d)) return 0;
    if (m->message == WM_KEYDOWN) {
        if (m->wParam == 9) {                                                    /* Tab */
            w32win_t *n = W(GetNextDlgTabItem(hdlg, (HWND)focus_win, keystate[0x10]));
            if (n) set_focus_ctl(n);
            return 1;
        }
        if (m->wParam == 0x0D) {                                                 /* Enter: the default push button, else IDOK */
            w32win_t *def = NULL;
            for (w32win_t *c = d->child; c; c = c->next) if (!c->destroyed && c->ctl == CTL_BUTTON && (c->style & 0xF) == 1) def = c;
            if (focus_win && focus_win->ctl == CTL_BUTTON && (focus_win->style & 0xF) <= 1 && is_inside(focus_win, d)) def = focus_win;
            int id = def ? (int)def->id : IDOK;
            if (def && !def->enabled) return 1;
            send(d, WM_COMMAND, (id & 0xFFFF) | ((uint32_t)BN_CLICKED << 16), (uintptr_t)def);
            return 1;
        }
        if (m->wParam == 0x1B) { send(d, WM_COMMAND, IDCANCEL | ((uint32_t)BN_CLICKED << 16), (uintptr_t)GetDlgItem(hdlg, IDCANCEL)); return 1; }
    }
    DispatchMessageA(m);
    return 1;
}
static BOOL WINAPI MapDialogRect(HWND h, RECT32 *r) { r->l = du_x(r->l); r->r = du_x(r->r); r->t = du_y(r->t); r->b = du_y(r->b); return 1; }
static LONG WINAPI GetDialogBaseUnits(void) { dlg_units(); return (LONG)(dlg_bux | (dlg_buy << 16)); }
static BOOL WINAPI CheckDlgButton(HWND h, int id, UINT st) { w32win_t *b = W(GetDlgItem(h, id)); if (!b) return 0; b->checked = st; draw_control(b); return 1; }
static UINT WINAPI IsDlgButtonChecked(HWND h, int id) { w32win_t *b = W(GetDlgItem(h, id)); return b ? b->checked : 0; }
static BOOL WINAPI CheckRadioButton(HWND h, int first, int last, int check) {
    w32win_t *d = W(h); if (!d) return 0;
    for (w32win_t *c = d->child; c; c = c->next) if (!c->destroyed && (int)c->id >= first && (int)c->id <= last) { c->checked = (int)c->id == check; draw_control(c); }
    return 1;
}
static LRESULT WINAPI SendMessageW_(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT WINAPI SendDlgItemMessageA(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp) { HWND c = GetDlgItem(h, id); return c ? SendMessageA(c, msg, wp, lp) : 0; }
static LRESULT WINAPI SendDlgItemMessageW(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp) { HWND c = GetDlgItem(h, id); return c ? SendMessageW_(c, msg, wp, lp) : 0; }

/* ---------------------------------------------------------------- Unicode window text and messages */
static BOOL WINAPI SetWindowTextW(HWND h, const WCHAR *t) { char a[4096]; w2u(t, a, sizeof a); return SetWindowTextA(h, a); }
static int WINAPI GetWindowTextW(HWND h, WCHAR *b, int n) { w32win_t *w = W(h); if (!w || n <= 0) return 0; return u2w(w->text, b, n); }
static int WINAPI GetWindowTextLengthW(HWND h) { w32win_t *w = W(h); if (!w) return 0; static WCHAR tmp[4096]; return u2w(w->text, tmp, 4096); }
static BOOL WINAPI SetDlgItemTextW(HWND h, int id, const WCHAR *t) { return SetWindowTextW(GetDlgItem(h, id), t); }
static UINT WINAPI GetDlgItemTextW(HWND h, int id, WCHAR *b, int n) { return GetWindowTextW(GetDlgItem(h, id), b, n); }
static int WINAPI GetClassNameW(HWND h, WCHAR *b, int n) { char a[64]; GetClassNameA(h, a, sizeof a); return u2w(a, b, n); }
/* the built-in controls keep UTF-8: wide strings in their messages are converted */
static bool text_message(UINT m) {
    switch (m) {
    case WM_SETTEXT: case WM_GETTEXT: case 0x180: case 0x181: case 0x189: case 0x18F: case 0x18C: case 0x1A2:
    case 0x143: case 0x14A: case 0x148: case 0x14C: case 0x14D: case 0x158: case 0xC2: return true;
    }
    return false;
}
static LRESULT WINAPI SendMessageW_(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    w32win_t *w = W(h); if (!w) return 0;
    if (w->ctl == CTL_TREE && (msg == 0x1100 || msg == 0x110C || msg == 0x110D)) msg += 0x32 + (msg == 0x110C || msg == 0x110D ? 0 : 0);   /* the A tree messages sent through W: as W */
    if (!w->ctl && !w->is_dialog) return send(w, msg, wp, lp);              /* the program's own (Unicode) window procedure */
    if (!text_message(msg)) return send(w, msg, wp, lp);
    if (msg == WM_GETTEXT) { return w->ctl || w->is_dialog ? u2w(w->text, (WCHAR *)lp, (int)wp) : 0; }
    if (msg == 0x189 || msg == 0x148) {                                         /* LB_GETTEXT / CB_GETLBTEXT */
        LRESULT r; if (list_msg(w, msg, wp, lp, &r, true)) return r; return -1;
    }
    char a[4096]; w2u((const WCHAR *)lp, a, sizeof a);
    return send(w, msg, wp, (uintptr_t)a);
}
static LRESULT WINAPI DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    w32win_t *w = W(h); if (!w) return 0;
    if (msg == WM_SETTEXT) { char a[4096]; w2u((const WCHAR *)lp, a, sizeof a); return default_proc(w, msg, wp, (uintptr_t)a); }
    if (msg == WM_GETTEXT) return lp ? u2w(w->text, (WCHAR *)lp, (int)wp) : 0;
    return default_proc(w, msg, wp, lp);
}
static LRESULT WINAPI DefDlgProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    w32win_t *d = W(h); if (!d) return 0;
    if (msg == WM_SETTEXT || msg == WM_GETTEXT) {
        void *fn = (void *)dlg_get(d, DWLP_DLGPROC);
        if (fn && call_wndproc(fn, h, msg, wp, lp)) return dlg_get(d, DWLP_MSGRESULT);
        return DefWindowProcW(h, msg, wp, lp);
    }
    return DefDlgProcA(h, msg, wp, lp);
}
typedef struct { UINT style; void *proc; int cls_extra, wnd_extra; HINSTANCE inst; HANDLE icon, cursor; HGDIOBJ bg; const WCHAR *menu, *name; } WNDCLASSW_;
typedef struct { UINT size, style; void *proc; int cls_extra, wnd_extra; HINSTANCE inst; HANDLE icon, cursor; HGDIOBJ bg; const WCHAR *menu, *name; HANDLE sm; } WNDCLASSEXW_;
static ATOM class_w(const WCHAR *name, void *proc, uintptr_t bg, uint32_t style, int extra) {
    char n[64];
    if ((uintptr_t)name < 0x10000) snprintf(n, sizeof n, "#%u", (unsigned)(uintptr_t)name); else w2u(name, n, sizeof n);
    return reg_class_ex(n, proc, bg, style, extra, true);
}
static void class_menu_w(const WCHAR *name, HINSTANCE inst, const WCHAR *menu) { char n[64]; if ((uintptr_t)name < 0x10000) snprintf(n, sizeof n, "#%u", (unsigned)(uintptr_t)name); else w2u(name, n, sizeof n); class_menu(n, inst, menu, true); }
static ATOM WINAPI RegisterClassW(const WNDCLASSW_ *wc) { ATOM a = class_w(wc->name, wc->proc, wc->bg, wc->style, wc->wnd_extra); if (a) class_menu_w(wc->name, wc->inst, wc->menu); return a; }
static ATOM WINAPI RegisterClassExW(const WNDCLASSEXW_ *wc) { ATOM a = class_w(wc->name, wc->proc, wc->bg, wc->style, wc->wnd_extra); if (a) class_menu_w(wc->name, wc->inst, wc->menu); return a; }
static BOOL WINAPI UnregisterClassW(const WCHAR *n, HINSTANCE h) { return 1; }
static BOOL WINAPI GetClassInfoW(HINSTANCE h, const WCHAR *n, WNDCLASSW_ *wc) {
    char a[64]; if ((uintptr_t)n < 0x10000) return 0; w2u(n, a, sizeof a);
    w32cls_t *c = find_class(a); if (!c) return 0;
    memset(wc, 0, sizeof *wc); wc->proc = c->proc; wc->bg = c->brush; wc->wnd_extra = c->wnd_extra; wc->name = n;
    return 1;
}
static HWND WINAPI CreateWindowExW2(DWORD ex, const WCHAR *c, const WCHAR *t, DWORD s, int x, int y, int w, int h, HWND p, HANDLE m, HINSTANCE i, void *pa) {
    char cn[64], tn[1024];
    const char *cls = cn;
    if ((uintptr_t)c < 0x10000) cls = (const char *)c; else w2u(c, cn, sizeof cn);   /* an atom stays an atom */
    w2u(t, tn, sizeof tn);
    return CreateWindowExA(ex, cls, tn, s, x, y, w, h, p, m, i, pa);
}

/* ---------------------------------------------------------------- strings, menus, icons from resources */
static int load_string(HINSTANCE inst, UINT id, WCHAR **out) {
    uintptr_t r = w32_find_resource(P, 6, (id >> 4) + 1, false, 0);              /* RT_STRING: blocks of 16 */
    if (!r) return -1;
    WCHAR *p = w32_resource_data(P, r, NULL);
    for (UINT i = 0; i < (id & 15); i++) p += 1 + p[0];
    *out = p + 1;
    return p[0];
}
static int WINAPI LoadStringW(HINSTANCE inst, UINT id, WCHAR *buf, int cap) {
    WCHAR *s; int n = load_string(inst, id, &s);
    if (n < 0) { if (cap > 0) buf[0] = 0; return 0; }
    if (cap == 0) { *(WCHAR **)buf = s; return n; }                              /* a pointer to the read-only string */
    int k = MIN(n, cap - 1); memcpy(buf, s, k * 2); buf[k] = 0;
    return k;
}
static int WINAPI LoadStringA(HINSTANCE inst, UINT id, char *buf, int cap) {
    WCHAR *s; int n = load_string(inst, id, &s);
    if (n < 0 || cap <= 0) { if (cap > 0) buf[0] = 0; return 0; }
    WCHAR tmp[512]; int k = MIN(n, 511); memcpy(tmp, s, k * 2); tmp[k] = 0;
    w2u(tmp, buf, cap);
    return strlen(buf);
}
/* a MENU resource (the classic, non-EX format) into a menu handle */
static const uint8_t *menu_parse(const uint8_t *p, HANDLE hm) {
    for (;;) {
        uint16_t flags; memcpy(&flags, p, 2); p += 2;
        uint16_t id = 0;
        if (!(flags & 0x10)) { memcpy(&id, p, 2); p += 2; }
        char text[64]; const WCHAR *w = (const WCHAR *)p; w2u(w, text, sizeof text); p += 2 * (w_len(w) + 1);
        if (flags & 0x10) { HANDLE sub = CreatePopupMenu(); p = menu_parse(p, sub); AppendMenuA(hm, 0x10, sub, text); }
        else AppendMenuA(hm, text[0] ? (flags & 0x0B) : 0x800, id, text);
        if (flags & 0x80) return p;                                              /* MF_END */
    }
}
HANDLE WINAPI w32_load_menu(HINSTANCE inst, uintptr_t name);
static HANDLE WINAPI LoadMenuA2(HINSTANCE inst, const char *name) {
    uintptr_t r = w32_find_resource(P, 4, (uintptr_t)name, false, 0);
    if (!r) return 0;
    const uint8_t *d = w32_resource_data(P, r, NULL);
    uint16_t ver; memcpy(&ver, d, 2);
    if (ver != 0) return CreateMenu();                                           /* MENUEX: an empty menu */
    uint16_t hdr; memcpy(&hdr, d + 2, 2);
    HANDLE hm = CreateMenu(); menu_parse(d + 4 + hdr, hm);
    return hm;
}
static HANDLE WINAPI LoadMenuW(HINSTANCE inst, const WCHAR *name) {
    if ((uintptr_t)name < 0x10000) return LoadMenuA2(inst, (const char *)name);
    char a[64]; w2u(name, a, sizeof a); return LoadMenuA2(inst, a);
}
static HANDLE WINAPI LoadImageW(HINSTANCE h, const WCHAR *n, UINT t, int cx, int cy, UINT f) { return t == 0 ? 0 : 0x20003; }
static BOOL WINAPI DrawIconEx(HDC h, int x, int y, HANDLE i, int cx, int cy, UINT step, HGDIOBJ br, UINT f) {
    dc_t *d = DC(h); if (!d) return 0;
    rect_t sv; dc_begin(d, &sv); draw_icon(d->surf, ICON_EXE, cx >= 24 || cx == 0 ? 32 : 16, d->ox + x, d->oy + y); dc_end(d, sv);
    return 1;
}

/* ---------------------------------------------------------------- menus: more of the API */
static BOOL WINAPI InsertMenuA(HANDLE hm, UINT pos, UINT flags, uintptr_t id, const char *text) {
    w32menu_t *m = MENU(hm); if (!m || m->n >= 24) return 0;
    int at = (flags & 0x400) ? -1 : (int)pos;                                    /* MF_BYPOSITION */
    if (at < 0) { at = m->n; for (int i = 0; i < m->n; i++) if (m->items[i].id == pos) { at = i; break; } }
    if (at > m->n) at = m->n;
    memmove(&m->items[at + 1], &m->items[at], (m->n - at) * sizeof(mitem_t));
    m->n++;
    mitem_t *it = &m->items[at]; memset(it, 0, sizeof *it); it->flags = flags & ~0x400u; it->id = id;
    if (flags & 0x800) strcpy(it->text, "-"); else if (text && (uintptr_t)text > 0x10000) { strlcpy(it->text, text, 64); for (char *p = it->text; *p; p++) if (*p == '&') memmove(p, p + 1, strlen(p)); }
    return 1;
}
static BOOL WINAPI InsertMenuW(HANDLE hm, UINT pos, UINT flags, uintptr_t id, const WCHAR *text) { char a[64] = ""; if (text && (uintptr_t)text > 0x10000) w2u(text, a, sizeof a); return InsertMenuA(hm, pos, flags, id, a); }
static BOOL WINAPI AppendMenuW(HANDLE hm, UINT flags, uintptr_t id, const WCHAR *text) { char a[64] = ""; if (text && (uintptr_t)text > 0x10000) w2u(text, a, sizeof a); return AppendMenuA(hm, flags, id, (flags & 0x800) ? NULL : a); }
static BOOL WINAPI DeleteMenu(HANDLE hm, UINT pos, UINT flags) {
    w32menu_t *m = MENU(hm); if (!m) return 0;
    int at = -1;
    if (flags & 0x400) at = (int)pos; else for (int i = 0; i < m->n; i++) if (m->items[i].id == pos) { at = i; break; }
    if (at < 0 || at >= m->n) return 0;
    memmove(&m->items[at], &m->items[at + 1], (m->n - at - 1) * sizeof(mitem_t));
    m->n--;
    return 1;
}
static BOOL WINAPI RemoveMenu(HANDLE hm, UINT pos, UINT flags) { return DeleteMenu(hm, pos, flags); }
static int WINAPI GetMenuItemCount(HANDLE hm) { w32menu_t *m = MENU(hm); return m ? m->n : -1; }
static UINT WINAPI GetMenuItemID(HANDLE hm, int i) { w32menu_t *m = MENU(hm); return m && i >= 0 && i < m->n && !(m->items[i].flags & 0x10) ? m->items[i].id : (UINT)-1; }
static BOOL WINAPI ModifyMenuA(HANDLE hm, UINT pos, UINT flags, uintptr_t id, const char *text) { DeleteMenu(hm, pos, flags & 0x400); return InsertMenuA(hm, pos, flags, id, text); }
/* TrackPopupMenu: the OS's popup menu; the choice comes back as WM_COMMAND (or as the result with TPM_RETURNCMD) */
typedef struct { volatile int picked; volatile bool done; } trackwait_t;
static void track_cb(void *ctx, int id) { trackwait_t *t = ctx; t->picked = id; t->done = true; }
static BOOL WINAPI TrackPopupMenu(HANDLE hm, UINT flags, int x, int y, int r, HWND owner, const void *rc) {
    w32menu_t *m = MENU(hm); if (!m || !m->n) return 0;
    static menu_item_t items[24];
    for (int i = 0; i < m->n; i++) { items[i].label = m->items[i].text; items[i].id = m->items[i].id ? (int)m->items[i].id : -1; items[i].flags = (m->items[i].flags & 8) ? MI_CHECKED : (m->items[i].flags & 3) ? MI_DISABLED : 0; }
    trackwait_t *t = kzalloc(sizeof *t);
    popup_menu(x, y, items, m->n, track_cb, t);
    while (!t->done && popup_open()) task_sleep(15);
    int id = t->done ? t->picked : 0;
    kfree(t);
    if (id <= 0) return 0;
    if (flags & 0x100) return id;                                                /* TPM_RETURNCMD */
    w32win_t *o = W(owner); if (o) w32_post(o->proc, owner, WM_COMMAND, id & 0xFFFF, 0);
    return 1;
}
static BOOL WINAPI TrackPopupMenuEx(HANDLE hm, UINT flags, int x, int y, HWND owner, void *p) { return TrackPopupMenu(hm, flags, x, y, 0, owner, NULL); }
static HANDLE WINAPI GetSystemMenu2(HWND h, BOOL revert) {
    static uintptr_t sysmenu;
    if (!sysmenu) sysmenu = (uintptr_t)CreatePopupMenu();
    return sysmenu;
}

/* ---------------------------------------------------------------- caret, clipboard, keyboard, scrolling, placement */
static struct { HWND hwnd; int x, y, w, h; bool shown; } caret;
static BOOL WINAPI CreateCaret(HWND h, HGDIOBJ bmp, int w, int ht) { caret.hwnd = h; caret.w = w ? w : 1; caret.h = ht; caret.shown = false; return 1; }
static BOOL WINAPI DestroyCaret(void) { caret.hwnd = 0; return 1; }
static BOOL WINAPI SetCaretPos(int x, int y) { caret.x = x; caret.y = y; return 1; }
static BOOL WINAPI GetCaretPos(LONG *pt) { pt[0] = caret.x; pt[1] = caret.y; return 1; }
static BOOL WINAPI ShowCaret(HWND h) { caret.shown = true; return 1; }
static BOOL WINAPI HideCaret(HWND h) { caret.shown = false; return 1; }
static UINT WINAPI GetCaretBlinkTime(void) { return 530; }
static BOOL WINAPI SetCaretBlinkTime(UINT ms) { return 1; }
/* the system clipboard (src/apps/launcher.c), as CF_TEXT (1), CF_OEMTEXT (7) and CF_UNICODETEXT (13):
 * shared with WordPad, Notepad and every other program */
static HWND clip_owner;
static BOOL WINAPI OpenClipboard2(HWND h) { return 1; }
static BOOL WINAPI EmptyClipboard(void) { clip_set_text(NULL); clip_owner = 0; return 1; }
static HANDLE WINAPI SetClipboardData(UINT fmt, HANDLE mem) {
    if (!mem) return 0;
    if (fmt == 13) { char *a = kmalloc(65536); w2u((const WCHAR *)mem, a, 65536); clip_set_text(a); kfree(a); }
    else if (fmt == 1 || fmt == 7) clip_set_text((const char *)mem);
    return mem;
}
static HANDLE WINAPI GetClipboardData(UINT fmt) {
    const char *t = clip_text();
    if (!t) return 0;
    w32_proc_t *p = P;
    if (fmt == 13) { int n = strlen(t) + 1; WCHAR *w = w32_alloc(p, n * 2); u2w(t, w, n); return (HANDLE)w; }
    if (fmt == 1 || fmt == 7) { char *a = w32_alloc(p, strlen(t) + 1); strcpy(a, t); return (HANDLE)a; }
    return 0;
}
static BOOL WINAPI IsClipboardFormatAvailable(UINT fmt) { return clip_text() && (fmt == 1 || fmt == 7 || fmt == 13); }
static UINT WINAPI RegisterClipboardFormatA(const char *n) { return 0xC000 + (strlen(n) & 0xFFF); }
static UINT WINAPI RegisterClipboardFormatW(const WCHAR *n) { return 0xC000 + (w_len(n) & 0xFFF); }
static HWND WINAPI GetClipboardOwner2(void) { return clip_owner; }
static UINT WINAPI EnumClipboardFormats(UINT f) { return f == 0 && clip_text() ? 13 : 0; }
static HANDLE WINAPI GetKeyboardLayout(DWORD tid) { return 0x04090409; }
static int WINAPI GetKeyboardLayoutList(int n, HANDLE *l) { if (n && l) l[0] = 0x04090409; return 1; }
static BOOL WINAPI SetKeyboardState(BYTE *s) { for (int i = 0; i < 256; i++) keystate[i] = (s[i] & 0x80) != 0; return 1; }
/* ToAscii(Ex): a virtual key to its character, US layout */
static int WINAPI ToAsciiEx(UINT vk, UINT scan, const BYTE *ks, WORD *out, UINT f, HANDLE layout) {
    if (scan & 0x8000) return 0;                                                 /* a key release makes no character */
    bool shift = ks && (ks[0x10] & 0x80), ctrl = ks && (ks[0x11] & 0x80), caps = ks && (ks[0x14] & 1);
    int c = 0;
    if (vk >= 'A' && vk <= 'Z') { c = (shift ^ caps) ? vk : vk + 32; if (ctrl) c = vk - 'A' + 1; }
    else if (vk >= '0' && vk <= '9') { static const char sh[] = ")!@#$%^&*("; c = shift ? sh[vk - '0'] : (int)vk; }
    else if (vk == 0x20) c = ' ';
    else if (vk == 0x0D) c = '\r'; else if (vk == 0x08) c = 8; else if (vk == 0x09) c = 9; else if (vk == 0x1B) c = 27;
    else {
        static const struct { UINT vk; char n, s; } oem[] = { { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' }, { 0xBD, '-', '_' }, { 0xBE, '.', '>' },
            { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' }, { 0xDC, '\\', '|' }, { 0xDD, ']', '}' }, { 0xDE, '\'', '"' } };
        for (unsigned i = 0; i < ARRAY_LEN(oem); i++) if (oem[i].vk == vk) c = shift ? oem[i].s : oem[i].n;
        if (vk >= 0x60 && vk <= 0x69) c = '0' + (vk - 0x60);
    }
    if (!c) return 0;
    out[0] = (WORD)c;
    return 1;
}
static int WINAPI ToAscii(UINT vk, UINT scan, const BYTE *ks, WORD *out, UINT f) { return ToAsciiEx(vk, scan, ks, out, f, 0); }
static int WINAPI ToUnicode(UINT vk, UINT scan, const BYTE *ks, WCHAR *out, int n, UINT f) { WORD c; int r = ToAsciiEx(vk, scan, ks, &c, f, 0); if (r && n > 0) out[0] = c; return r; }
static int WINAPI ToUnicodeEx(UINT vk, UINT scan, const BYTE *ks, WCHAR *out, int n, UINT f, HANDLE l) { return ToUnicode(vk, scan, ks, out, n, f); }
static UINT WINAPI MapVirtualKeyW(UINT c, UINT t);
static UINT WINAPI MapVirtualKeyExA(UINT c, UINT t, HANDLE l) { return MapVirtualKeyW(c, t); }
static int16_t WINAPI VkKeyScanA(char c) { if (c >= 'a' && c <= 'z') return c - 32; if (c >= 'A' && c <= 'Z') return (c) | 0x100; return (uint8_t)c; }
/* scroll bars of a window: remembered, not drawn */
typedef struct { UINT size, mask; int min, max; UINT page; int pos, track; } SCROLLINFO_;
static SCROLLINFO_ sb_info[2];
static int WINAPI SetScrollInfo(HWND h, int bar, const SCROLLINFO_ *si, BOOL redraw) {
    SCROLLINFO_ *s = &sb_info[bar == 1 ? 1 : 0];
    if (si->mask & 1) { s->min = si->min; s->max = si->max; }
    if (si->mask & 2) s->page = si->page;
    if (si->mask & 4) s->pos = si->pos;
    return s->pos;
}
static BOOL WINAPI GetScrollInfo(HWND h, int bar, SCROLLINFO_ *si) {
    SCROLLINFO_ *s = &sb_info[bar == 1 ? 1 : 0];
    if (si->mask & 1) { si->min = s->min; si->max = s->max; }
    if (si->mask & 2) si->page = s->page;
    if (si->mask & 4) si->pos = s->pos;
    if (si->mask & 0x10) si->track = s->pos;
    return 1;
}
typedef struct { UINT len, flags, show; LONG minx, miny, maxx, maxy; RECT32 normal; } WINDOWPLACEMENT_;
static BOOL WINAPI GetWindowPlacement(HWND h, WINDOWPLACEMENT_ *wp) {
    w32win_t *w = W(h); if (!w) return 0;
    wp->flags = 0; wp->show = !w->top ? 1 : w->top->state == WS_MAXIMIZED ? 3 : w->top->state == WS_MINIMIZED ? 2 : 1;
    GetWindowRect(h, &wp->normal);
    return 1;
}
static BOOL WINAPI SetWindowPlacement(HWND h, const WINDOWPLACEMENT_ *wp) {
    MoveWindow(h, wp->normal.l, wp->normal.t, wp->normal.r - wp->normal.l, wp->normal.b - wp->normal.t, 1);
    ShowWindow(h, wp->show);
    return 1;
}
typedef struct { UINT size; HWND owner; HINSTANCE inst; const WCHAR *text, *caption; DWORD style; const WCHAR *icon; uintptr_t ctx; void *cb; DWORD lang; } MSGBOXPARAMSW_;
static int WINAPI MessageBoxIndirectW(const MSGBOXPARAMSW_ *mb) {
    char t[2048], c[256]; w2u(mb->text, t, sizeof t); w2u(mb->caption, c, sizeof c);
    return MessageBoxA(mb->owner, t, c, mb->style);
}
typedef struct { UINT size; HWND owner; HINSTANCE inst; const char *text, *caption; DWORD style; const char *icon; uintptr_t ctx; void *cb; DWORD lang; } MSGBOXPARAMSA_;
static int WINAPI MessageBoxIndirectA(const MSGBOXPARAMSA_ *mb) { return MessageBoxA(mb->owner, mb->text, mb->caption, mb->style); }
static BOOL WINAPI EnumChildWindows(HWND h, BOOL (WINAPI *fn)(HWND, LPARAM), LPARAM lp) {
    w32win_t *w = W(h); if (!w) return 0;
    for (w32win_t *c = w->child; c; c = c->next) if (!c->destroyed && !fn((HWND)c, lp)) break;
    return 1;
}
static HWND WINAPI ChildWindowFromPoint(HWND h, LONG x, LONG y) { w32win_t *w = W(h); if (!w) return 0; w32win_t *c = child_at(w, x, y); return (HWND)(c ? c : w); }
static HWND WINAPI WindowFromPoint(LONG x, LONG y) { return 0; }
static BOOL WINAPI IsChild(HWND parent, HWND h) { w32win_t *p = W(parent), *c = W(h); return p && c && c != p && is_inside(c, p); }
static HWND WINAPI GetAncestor(HWND h, UINT f) { w32win_t *w = W(h); if (!w) return 0; if (f == 1) return (HWND)w->parent; return (HWND)top_of(w); }
static BOOL WINAPI LockWindowUpdate(HWND h) { return 1; }
static HWND WINAPI SetParent(HWND h, HWND np) { w32win_t *w = W(h); return w ? (HWND)w->parent : 0; }
static int WINAPI MapWindowPoints(HWND from, HWND to, LONG *pts, UINT n) {
    for (UINT i = 0; i < n; i++) {
        LONG pt[2] = { pts[2 * i], pts[2 * i + 1] };
        if (from) ClientToScreen(from, pt);
        if (to) ScreenToClient(to, pt);
        pts[2 * i] = pt[0]; pts[2 * i + 1] = pt[1];
    }
    return 0;
}
static BOOL WINAPI EnableScrollBar(HWND h, UINT bar, UINT arrows) { return 1; }
static BOOL WINAPI IsWindowUnicode(HWND h) { w32win_t *w = W(h); return w && w->unicode; }
static LRESULT WINAPI CallWindowProcW(void *fn, HWND h, UINT msg, WPARAM wp, LPARAM lp) { return call_wndproc(fn, h, msg, wp, lp); }
static LRESULT WINAPI SendMessageTimeoutA(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT f, UINT t, uintptr_t *r) { LRESULT x = SendMessageA(h, m, wp, lp); if (r) *r = x; return 1; }
static BOOL WINAPI SendNotifyMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) { SendMessageA(h, m, wp, lp); return 1; }
static UINT WINAPI RegisterWindowMessageW(const WCHAR *s) { return 0xC100 + (w_len(s) & 0xFF); }
static BOOL WINAPI SystemParametersInfoW(UINT a, UINT b, void *p, UINT f) { return SystemParametersInfoA(a, b, p, f); }
static HWND WINAPI FindWindowW(const WCHAR *c, const WCHAR *t) { char a[256]; if (!t) return 0; w2u(t, a, sizeof a); return FindWindowA(NULL, a); }
static DWORD WINAPI GetWindowThreadProcessId(HWND h, DWORD *pid) { w32win_t *w = W(h); if (pid) *pid = w ? (DWORD)w->proc->pid : 0; return w ? (DWORD)w->proc->pid : 0; }
static BOOL WINAPI GetMonitorInfoA(HANDLE m, void *mi) { RECT32 *r = (RECT32 *)((uint8_t *)mi + 4); r->l = r->t = 0; r->r = SCREEN_W; r->b = SCREEN_H; r[1] = r[0]; r[1].b = SCREEN_H - T.taskbar_h; *(DWORD *)((uint8_t *)mi + 36) = 1; return 1; }
static HANDLE WINAPI MonitorFromPoint(LONG x, LONG y, DWORD f) { return 0x30001; }
static HANDLE WINAPI MonitorFromRect(const RECT32 *r, DWORD f) { return 0x30001; }
static BOOL WINAPI MessageBeep2(UINT t) { return 1; }
static BOOL WINAPI GetUpdateRect(HWND h, RECT32 *r, BOOL erase) { w32win_t *w = W(h); if (r) { r->l = r->t = 0; r->r = w ? w->w : 0; r->b = w ? w->h : 0; } return w && w->need_paint; }
static int WINAPI GetUpdateRgn(HWND h, HANDLE rgn, BOOL erase) { return 1; }
static BOOL WINAPI InvalidateRgn(HWND h, HANDLE rgn, BOOL erase) { return InvalidateRect(h, NULL, erase); }
static BOOL WINAPI ValidateRgn(HWND h, HANDLE rgn) { return ValidateRect(h, NULL); }
static BOOL WINAPI EndDeferWindowPos(HANDLE h) { return 1; }
static HANDLE WINAPI DeferWindowPos(HANDLE d, HWND h, HWND a, int x, int y, int cx, int cy, UINT f) { SetWindowPos(h, a, x, y, cx, cy, f); return d; }
static BOOL WINAPI OpenIcon(HWND h) { return ShowWindow(h, 9); }
static BOOL WINAPI CloseWindow(HWND h) { return ShowWindow(h, 6); }
static BOOL WINAPI ShowWindowAsync(HWND h, int c) { return ShowWindow(h, c); }
static BOOL WINAPI SetWindowTextA_alias(HWND h, const char *t) { return SetWindowTextA(h, t); }
static BOOL WINAPI DestroyCursor(HANDLE c) { return 1; }
static HANDLE WINAPI CopyIcon(HANDLE i) { return i; }
static HANDLE WINAPI CreateIconIndirect(void *ii) { return 0x20002; }
static BOOL WINAPI GetIconInfo(HANDLE i, void *ii) { memset(ii, 0, sizeof(void *) == 8 ? 32 : 20); return 1; }
static HANDLE WINAPI CopyImage(HANDLE h, UINT t, int cx, int cy, UINT f) { return h; }
static BOOL WINAPI ClipCursor(const RECT32 *r) { return 1; }
static BOOL WINAPI GetClipCursor(RECT32 *r) { r->l = r->t = 0; r->r = SCREEN_W; r->b = SCREEN_H; return 1; }
static int WINAPI GetSystemMetricsForDpi(int i, UINT dpi) { return GetSystemMetrics(i); }
static UINT WINAPI GetDpiForWindow(HWND h) { return 96; }
static UINT WINAPI GetDpiForSystem(void) { return 96; }
static BOOL WINAPI SetProcessDPIAware(void) { return 1; }
static BOOL WINAPI SetProcessDpiAwarenessContext(HANDLE c) { return 1; }
static BOOL WINAPI IsProcessDPIAware(void) { return 1; }
static BOOL WINAPI AdjustWindowRectExForDpi(RECT32 *r, DWORD s, BOOL m, DWORD ex, UINT dpi) { return AdjustWindowRect(r, s, m); }
static BOOL WINAPI UnhookWindowsHookEx(HANDLE h) { return 1; }
static LRESULT WINAPI CallNextHookEx(HANDLE h, int c, WPARAM wp, LPARAM lp) { return 0; }
static BOOL WINAPI TranslateMDISysAccel(HWND h, MSG32 *m) { return 0; }
static BOOL WINAPI DestroyAcceleratorTable(HANDLE h) { return 1; }
static HANDLE WINAPI CreateAcceleratorTableA(void *a, int n) { return 0x40001; }
static BOOL WINAPI DragDetect(HWND h, LONG x, LONG y) { return 0; }
static BOOL WINAPI GetLastInputInfo(void *li) { ((DWORD *)li)[1] = timer_ticks(); return 1; }
static BOOL WINAPI AllowSetForegroundWindow(DWORD pid) { return 1; }
static BOOL WINAPI FlashWindowEx(void *fi) { return 1; }
static int WINAPI DrawTextW(HDC h, const WCHAR *s, int n, RECT32 *r, UINT fmt) {
    char a[4096]; WCHAR tmp[2048];
    if (n >= 0) { int k = MIN(n, 2047); memcpy(tmp, s, k * 2); tmp[k] = 0; s = tmp; }
    w2u(s, a, sizeof a);
    return DrawTextA(h, a, -1, r, fmt);
}
static int WINAPI DrawTextExW(HDC h, WCHAR *s, int n, RECT32 *r, UINT fmt, void *p) { return DrawTextW(h, s, n, r, fmt); }
static int WINAPI DrawTextExA(HDC h, char *s, int n, RECT32 *r, UINT fmt, void *p) { return DrawTextA(h, s, n, r, fmt); }
static int CDECL wsprintfW(WCHAR *b, const WCHAR *f, ...) {
    char fa[1024], out[2048]; w2u(f, fa, sizeof fa);
    w32_va_list ap; w32_va_start(ap, f); w32_vformat(out, sizeof out, fa, ap); w32_va_end(ap);
    return u2w(out, b, 1024);
}
static BOOL WINAPI IsDialogMessageW(HWND h, MSG32 *m) { return IsDialogMessageA(h, m); }
static BOOL WINAPI EnumThreadWindows(DWORD tid, void *fn, LPARAM lp) { return 1; }
static BOOL WINAPI SetWindowContextHelpId(HWND h, DWORD id) { return 1; }
static BOOL WINAPI WinHelpA(HWND h, const char *f, UINT c, uintptr_t d) { return 0; }
static HANDLE WINAPI LoadCursorFromFileA(const char *f) { return 0x20001; }
static int WINAPI CopyAcceleratorTableA(HANDLE h, void *a, int n) { return 0; }

#define E(n) { #n, (void *)n }
/* ---------------------------------------------------------------- GDI: more of the API (terminal programs such as PuTTY) */
static int wchar_width(int f, WCHAR c) { WCHAR w[2] = { c, 0 }; char a[8]; w2u(w, a, sizeof a); return text_width(f, a); }
static BOOL WINAPI ExtTextOutW(HDC h, int x, int y, UINT o, const LONG *r, const WCHAR *s, UINT n, const int *dx) {
    dc_t *d = DC(h); if (!d) return 0;
    if (o & 4 && r) {                                                            /* ETO_CLIPPED: draw inside the rectangle only */
        rect_t save = d->clip;
        d->clip = rect_intersect(d->clip, R(d->ox + r[0], d->oy + r[1], r[2] - r[0], r[3] - r[1]));
        BOOL res = ExtTextOutW(h, x, y, o & ~4u, r, s, n, dx);
        d->clip = save;
        return res;
    }
    if (r && (o & 2)) { rect_t sv; dc_begin(d, &sv); fill_rect(d->surf, d->ox + r[0], d->oy + r[1], r[2] - r[0], r[3] - r[1], d->bk); dc_end(d, sv); }
    if (!s || !n) return 1;
    if (o & 0x10) return 1;                                                      /* ETO_GLYPH_INDEX: no glyph tables here */
    int f = dcfont(d);
    if (dx) {                                                                    /* character cells at given advances (terminals) */
        rect_t sv; dc_begin(d, &sv);
        int cx = x;
        if ((d->align & 24) == 24) y -= font_ascent(f); else if (d->align & 8) y -= font_height(f);
        if (d->bkmode == 2 && !(o & 2)) { int tw = 0; for (UINT i = 0; i < n; i++) tw += dx[i]; fill_rect(d->surf, d->ox + x, d->oy + y, tw, font_height(f), d->bk); }
        for (UINT i = 0; i < n; i++) {
            WCHAR w[2] = { s[i], 0 }; char a[8]; w2u(w, a, sizeof a);
            if (s[i] != ' ') draw_text(d->surf, f, d->ox + cx, d->oy + y, a, d->text);
            cx += dx[i];
        }
        dc_end(d, sv);
        return 1;
    }
    WCHAR tmp[1024]; int k = MIN((int)n, 1023); memcpy(tmp, s, k * 2); tmp[k] = 0;
    char a[4096]; w2u(tmp, a, sizeof a);
    return TextOutA(h, x, y, a, -1);
}
static BOOL WINAPI TextOutW(HDC h, int x, int y, const WCHAR *s, int n) { if (n < 0) n = w_len(s); return ExtTextOutW(h, x, y, 0, NULL, s, n, NULL); }
static BOOL WINAPI GetTextExtentPoint32W(HDC h, const WCHAR *s, int n, SIZE32 *sz) {
    dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI;
    WCHAR tmp[1024]; int k = MIN(n, 1023); memcpy(tmp, s, k * 2); tmp[k] = 0;
    char a[4096]; w2u(tmp, a, sizeof a);
    sz->cx = text_width(f, a); sz->cy = font_height(f);
    return 1;
}
static int WINAPI GetBkMode(HDC h) { dc_t *d = DC(h); return d ? d->bkmode : 0; }
static COLORREF WINAPI GetBkColor(HDC h) { dc_t *d = DC(h); if (!d) return 0; uint32_t c = d->bk; return ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16); }
static COLORREF WINAPI GetTextColor(HDC h) { dc_t *d = DC(h); if (!d) return 0; uint32_t c = d->text; return ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16); }
static BOOL WINAPI GetCharWidth32A(HDC h, UINT first, UINT last, int *out) {
    dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI;
    for (UINT c = first; c <= last && c - first < 65536; c++) { char a[2] = { (char)c, 0 }; out[c - first] = c >= 32 && c < 127 ? text_width(f, a) : text_width(f, "?"); }
    return 1;
}
static BOOL WINAPI GetCharWidth32W(HDC h, UINT first, UINT last, int *out) {
    dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI;
    for (UINT c = first; c <= last && c - first < 65536; c++) out[c - first] = wchar_width(f, (WCHAR)c);
    return 1;
}
static BOOL WINAPI GetCharABCWidthsFloatA(HDC h, UINT first, UINT last, void *abc) { return 0; }   /* no float metrics: callers fall back */
static BOOL WINAPI GetCharABCWidthsA(HDC h, UINT first, UINT last, void *abc) { return 0; }
static BOOL WINAPI GetCharABCWidthsW(HDC h, UINT first, UINT last, void *abc) { return 0; }
typedef struct { DWORD size; WCHAR *out; UINT *order; int *dx, *caret; char *cls; WCHAR *glyphs; UINT nglyphs; int maxfit; } GCP_RESULTSW_;
static DWORD WINAPI GetCharacterPlacementW(HDC h, const WCHAR *s, int n, int maxext, GCP_RESULTSW_ *r, DWORD flags) {
    dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI, tw = 0;
    for (int i = 0; i < n; i++) {
        int cw = wchar_width(f, s[i]);
        if (r) {
            if (r->dx) r->dx[i] = cw;
            if (r->order) r->order[i] = i;
            if (r->caret) r->caret[i] = tw;
            if (r->glyphs && i < (int)r->nglyphs) r->glyphs[i] = s[i];
            if (r->out) r->out[i] = s[i];
        }
        tw += cw;
    }
    if (r) { r->nglyphs = n; r->maxfit = n; }
    return (DWORD)(tw & 0xFFFF) | ((DWORD)font_height(f) << 16);
}
static BOOL WINAPI GetTextExtentExPointA(HDC h, const char *s, int n, int maxext, int *fit, int *dx, SIZE32 *sz) {
    dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI, tw = 0, k = 0;
    for (int i = 0; i < n; i++) {
        tw += text_width_n(f, s + i, 1);
        if (dx) dx[i] = tw;
        if (tw <= maxext) k = i + 1;
    }
    if (fit) *fit = k;
    if (sz) { sz->cx = tw; sz->cy = font_height(f); }
    return 1;
}
static BOOL WINAPI GetTextExtentExPointW(HDC h, const WCHAR *s, int n, int maxext, int *fit, int *dx, SIZE32 *sz) {
    dc_t *d = DC(h); int f = d ? dcfont(d) : FONT_UI, tw = 0, k = 0;
    for (int i = 0; i < n; i++) { tw += wchar_width(f, s[i]); if (dx) dx[i] = tw; if (tw <= maxext) k = i + 1; }
    if (fit) *fit = k;
    if (sz) { sz->cx = tw; sz->cy = font_height(f); }
    return 1;
}
static UINT WINAPI GetOutlineTextMetricsA(HDC h, UINT n, void *otm) { return 0; }   /* bitmap fonts only */
static BOOL WINAPI TranslateCharsetInfo(DWORD *src, void *cs, DWORD flags) { return 0; }
static HGDIOBJ WINAPI GetCurrentObject(HDC h, UINT type) {
    dc_t *d = DC(h); if (!d) return 0;
    switch (type) { case 1: return (HGDIOBJ)d->pen; case 2: return (HGDIOBJ)d->brush; case 6: return (HGDIOBJ)d->font; case 7: return (HGDIOBJ)d->bitmap; }
    return 0;
}
static int WINAPI IntersectClipRect(HDC h, int l, int t, int r, int b) {
    dc_t *d = DC(h); if (!d) return 0;
    d->clip = rect_intersect(d->clip, R(d->ox + l, d->oy + t, r - l, b - t));
    return 2;                                                                    /* SIMPLEREGION */
}
static int WINAPI ExcludeClipRect(HDC h, int l, int t, int r, int b) { return DC(h) ? 3 : 0; }   /* COMPLEXREGION; drawing is not held back */
static int WINAPI SelectClipRgn(HDC h, HANDLE rgn) { dc_t *d = DC(h); if (!d) return 0; if (!rgn && d->win) { w32win_t *w = d->win; d->clip = w->parent ? R(d->ox, d->oy, w->w, w->h) : R(0, 0, d->surf->w, d->surf->h); } return 2; }
static int WINAPI GetDIBits(HDC h, HGDIOBJ bmp, UINT start, UINT lines, void *bits, void *bi, UINT usage) { return 0; }
/* palettes: true colour only, so these are bookkeeping */
static HGDIOBJ WINAPI CreatePalette(const void *lp) { gdiobj_t *o = newobj(OBJ_BRUSH); o->null = true; return (HGDIOBJ)o; }
static HGDIOBJ WINAPI SelectPalette(HDC h, HGDIOBJ pal, BOOL bg) { return (HGDIOBJ)&stock[15]; }
static UINT WINAPI RealizePalette(HDC h) { return 0; }
static UINT WINAPI SetPaletteEntries(HGDIOBJ pal, UINT start, UINT n, const void *e) { return n; }
static BOOL WINAPI UnrealizeObject(HGDIOBJ h) { return 1; }
static BOOL WINAPI UpdateColors(HDC h) { return 1; }
static UINT WINAPI GetNearestPaletteIndex(HGDIOBJ pal, COLORREF c) { return 0; }

/* ---------------------------------------------------------------- imm32, ole32, comdlg32, comctl32 */
static HANDLE WINAPI ImmGetContext(HWND h) { return 0; }
static BOOL WINAPI ImmReleaseContext(HWND h, HANDLE c) { return 1; }
static LONG WINAPI ImmGetCompositionStringW(HANDLE c, DWORD i, void *buf, DWORD n) { return 0; }
static BOOL WINAPI ImmSetCompositionFontA(HANDLE c, void *lf) { return 0; }
static BOOL WINAPI ImmSetCompositionFontW(HANDLE c, void *lf) { return 0; }
static BOOL WINAPI ImmSetCompositionWindow(HANDLE c, void *cf) { return 0; }
static BOOL WINAPI ImmSetOpenStatus(HANDLE c, BOOL o) { return 1; }
static BOOL WINAPI ImmGetOpenStatus(HANDLE c) { return 0; }
static BOOL WINAPI ImmAssociateContext(HWND h, HANDLE c) { return 0; }
static LONG WINAPI CoInitialize(void *r) { return 0; }
static LONG WINAPI CoInitializeEx(void *r, DWORD f) { return 0; }
static void WINAPI CoUninitialize(void) {}
static LONG WINAPI CoCreateInstance(const void *clsid, void *outer, DWORD ctx, const void *iid, void **out) { if (out) *out = NULL; return (LONG)0x80040154; }   /* REGDB_E_CLASSNOTREG */
static void *WINAPI CoTaskMemAlloc(uintptr_t n) { return w32_alloc(P, n ? n : 1); }
static void WINAPI CoTaskMemFree(void *p) { if (p) w32_free(P, p); }
static LONG WINAPI OleInitialize(void *r) { return 0; }
static void WINAPI OleUninitialize(void) {}
static LONG WINAPI CoGetMalloc(DWORD c, void **out) { if (out) *out = NULL; return (LONG)0x80004001; }
/* the common dialogs. Open and Save As use the system's file dialog, run modally: the
 * program's timers and painting go on behind it, as in MessageBox. The filter's selected
 * pattern ("*.txt") narrows the list; one file is returned (no multiple selection). */
typedef struct {
    DWORD size; HWND owner; HINSTANCE inst; const void *filter; void *cust; DWORD ncust, findex; void *file; DWORD nfile;
    void *title_out; DWORD ntitle; const void *initdir, *title; DWORD flags; WORD fileoff, extoff; const void *defext; LPARAM cust_data; void *hook; const void *tpl;
} OFN;
static DWORD cdlg_err;
typedef struct { char path[400]; bool done; } ofn_wait_t;
static void ofn_cb(void *ctx, const char *path) { ofn_wait_t *w = ctx; if (path) strlcpy(w->path, path, sizeof w->path); w->done = true; }
static void ofn_str(const void *s, bool wide, char *out, int cap) {
    out[0] = 0; if (!s) return;
    if (wide) { const WCHAR *w = s; int i = 0; for (; w[i] && i < cap - 1; i++) out[i] = w[i] < 256 ? (char)w[i] : '?'; out[i] = 0; }
    else strlcpy(out, s, cap);
}
/* the n-th (1-based) filter's first pattern, as ".ext", or "" for all files */
static void ofn_filter_ext(const void *filter, bool wide, DWORD index, char *ext, int cap) {
    ext[0] = 0; if (!filter) return;
    char buf[1024]; int k = 0;
    if (wide) { const WCHAR *w = filter; for (int i = 0; k < (int)sizeof buf - 2 && (w[i] || w[i + 1]); i++) buf[k++] = w[i] < 256 ? (char)w[i] : '?'; }
    else { const char *a = filter; for (int i = 0; k < (int)sizeof buf - 2 && (a[i] || a[i + 1]); i++) buf[k++] = a[i]; }
    buf[k] = 0; buf[k + 1] = 0;
    const char *p = buf; DWORD n = 1; if (!index) index = 1;
    while (*p) {
        const char *pat = p + strlen(p) + 1;
        if (n == index) {
            const char *dot = strstr(pat, "*.");
            if (dot && dot[2] && dot[2] != '*' && dot[2] != ';') { int l = 0; ext[l++] = '.'; for (const char *c = dot + 2; *c && *c != ';' && l < cap - 1; c++) ext[l++] = *c; ext[l] = 0; }
            return;
        }
        if (!*pat) return;
        p = pat + strlen(pat) + 1; n++;
    }
}
static BOOL file_name_dialog(OFN *o, bool wide, bool save) {
    cdlg_err = 0;
    if (!o || !o->file || !o->nfile) { cdlg_err = 0xFFFF; return 0; }
    char init[400], dir[300], cur[300], title[128], ext[32], defext[32];
    ofn_str(o->file, wide, cur, sizeof cur); ofn_str(o->initdir, wide, dir, sizeof dir); ofn_str(o->title, wide, title, sizeof title);
    ofn_str(o->defext, wide, defext, sizeof defext);
    ofn_filter_ext(o->filter, wide, o->findex, ext, sizeof ext);
    if (cur[0] && (cur[1] == ':' || cur[0] == '\\')) strlcpy(init, cur, sizeof init);
    else if (dir[0]) { if (cur[0]) path_join(init, sizeof init, dir, cur); else strlcpy(init, dir, sizeof init); }
    else if (cur[0]) path_join(init, sizeof init, P->cwd, cur);
    else strlcpy(init, P->cwd, sizeof init);
    w32_proc_t *p = P;
    ofn_wait_t *wt = kzalloc(sizeof *wt);
    w32win_t *ow = W(o->owner);
    window_t *fw = file_dialog_win(title[0] ? title : save ? "Save As" : "Open", init, save, ext[0] ? ext : NULL, ofn_cb, wt);
    if (fw) { fw->pid = p->pid; if (ow && top_of(ow)->top) { fw->owner = top_of(ow)->top; fw->flags |= WF_MODAL; } }
    while (!wt->done && fw && wm_alive(fw)) {
        MSG32 m; bool any = false;
        while (!wt->done && get_msg(p, &m, true)) {
            any = true;
            if (m.message == WM_QUIT) { p->quit_posted = true; p->quit_code = m.wParam; continue; }
            if ((m.message >= 0x100 && m.message <= 0x109) || (m.message >= 0x200 && m.message <= 0x20E)) continue;
            DispatchMessageA(&m);
        }
        if (!any && find_paint(p, &m)) send(W(m.hwnd), WM_PAINT, 0, 0);
        task_sleep(15);
    }
    char path[420]; strlcpy(path, wt->path, sizeof path);
    bool ok = wt->done && path[0];
    kfree(wt);
    if (ow && top_of(ow)->top) wm_focus(top_of(ow)->top);
    if (!ok) return 0;                                     /* cancelled: CommDlgExtendedError() is 0 */
    if (save && defext[0] && !strchr(path_basename(path), '.')) { strlcat(path, ".", sizeof path); strlcat(path, defext, sizeof path); }
    if (save && (o->flags & 0x2) && vfs_resolve(path, NULL)) {          /* OFN_OVERWRITEPROMPT */
        char q[500]; snprintf(q, sizeof q, "%s already exists.\nDo you want to replace it?", path_basename(path));
        if (MessageBoxA(o->owner, q, "Confirm Save As", 0x34) != 6) return 0;
    }
    if (!save && (o->flags & 0x1000) && !vfs_resolve(path, NULL)) return 0;   /* OFN_FILEMUSTEXIST */
    size_t len = strlen(path);
    if (len + 1 > o->nfile) { cdlg_err = 0x3003; if (o->nfile >= 2) { if (wide) ((WCHAR *)o->file)[0] = (WCHAR)(len + 1); else *(WORD *)o->file = (WORD)(len + 1); } return 0; }
    if (wide) { WCHAR *w = o->file; for (size_t i = 0; i <= len; i++) w[i] = (uint8_t)path[i]; } else memcpy(o->file, path, len + 1);
    const char *base = path_basename(path), *dot = strrchr(base, '.');
    o->fileoff = (WORD)(base - path); o->extoff = dot ? (WORD)(dot + 1 - path) : (WORD)len;
    if (o->title_out && o->ntitle) {
        if (wide) { WCHAR *w = o->title_out; DWORD i = 0; for (; base[i] && i < o->ntitle - 1; i++) w[i] = (uint8_t)base[i]; w[i] = 0; }
        else strlcpy(o->title_out, base, o->ntitle);
    }
    return 1;
}
static BOOL WINAPI GetOpenFileNameA(void *ofn) { return file_name_dialog(ofn, false, false); }
static BOOL WINAPI GetOpenFileNameW(void *ofn) { return file_name_dialog(ofn, true, false); }
static BOOL WINAPI GetSaveFileNameA(void *ofn) { return file_name_dialog(ofn, false, true); }
static BOOL WINAPI GetSaveFileNameW(void *ofn) { return file_name_dialog(ofn, true, true); }
static short WINAPI GetFileTitleA(const char *f, char *b, WORD n) { const char *s = path_basename(f); size_t l = strlen(s); if (!b || n <= l) return (short)(l + 1); memcpy(b, s, l + 1); return 0; }
static short WINAPI GetFileTitleW(const WCHAR *f, WCHAR *b, WORD n) { int i = 0, s = 0; for (; f[i]; i++) if (f[i] == '\\' || f[i] == '/' || f[i] == ':') s = i + 1; int l = i - s; if (!b || n <= l) return (short)(l + 1); for (int k = 0; k <= l; k++) b[k] = f[s + k]; return 0; }
static HWND WINAPI FindTextA(void *fr) { return 0; }
static HWND WINAPI FindTextW(void *fr) { return 0; }
static HWND WINAPI ReplaceTextA(void *fr) { return 0; }
static HWND WINAPI ReplaceTextW(void *fr) { return 0; }
static BOOL WINAPI PrintDlgW(void *pd) { return 0; }
static BOOL WINAPI PageSetupDlgA(void *pd) { return 0; }
static BOOL WINAPI PageSetupDlgW(void *pd) { return 0; }
static BOOL WINAPI ChooseColorA(void *cc) { return 0; }
static BOOL WINAPI ChooseColorW(void *cc) { return 0; }
static BOOL WINAPI ChooseFontA(void *cf) { return 0; }
static BOOL WINAPI ChooseFontW(void *cf) { return 0; }
static BOOL WINAPI PrintDlgA(void *pd) { return 0; }
static DWORD WINAPI CommDlgExtendedError(void) { return cdlg_err; }
static void WINAPI InitCommonControls(void) {}
static BOOL WINAPI InitCommonControlsEx(const void *icc) { return 1; }

const w32_export_t imm32_exports[] = {
    E(ImmGetContext), E(ImmReleaseContext), E(ImmGetCompositionStringW), E(ImmSetCompositionFontA), E(ImmSetCompositionFontW), E(ImmSetCompositionWindow),
    E(ImmSetOpenStatus), E(ImmGetOpenStatus), E(ImmAssociateContext),
};
const int imm32_count = ARRAY_LEN(imm32_exports);
const w32_export_t ole32_exports[] = {
    E(CoInitialize), E(CoInitializeEx), E(CoUninitialize), E(CoCreateInstance), E(CoTaskMemAlloc), E(CoTaskMemFree), E(OleInitialize), E(OleUninitialize), E(CoGetMalloc),
};
const int ole32_count = ARRAY_LEN(ole32_exports);
const w32_export_t comdlg32_exports[] = {
    E(GetOpenFileNameA), E(GetOpenFileNameW), E(GetSaveFileNameA), E(GetSaveFileNameW), E(GetFileTitleA), E(GetFileTitleW), E(FindTextA), E(FindTextW), E(ReplaceTextA), E(ReplaceTextW), E(PrintDlgW), E(PageSetupDlgA), E(PageSetupDlgW), E(ChooseColorA), E(ChooseColorW), E(ChooseFontA), E(ChooseFontW), E(PrintDlgA),
    E(CommDlgExtendedError),
};
const int comdlg32_count = ARRAY_LEN(comdlg32_exports);
const w32_export_t comctl32_exports[] = { E(InitCommonControls), E(InitCommonControlsEx) };
const int comctl32_count = ARRAY_LEN(comctl32_exports);

/* ---------------------------------------------------------------- winspool.drv: no printers yet */
static BOOL WINAPI EnumPrintersA(DWORD flags, char *name, DWORD level, BYTE *buf, DWORD cb, DWORD *needed, DWORD *returned) {
    if (needed) *needed = 0;
    if (returned) *returned = 0;
    return 1;
}
static BOOL WINAPI EnumPrintersW(DWORD flags, WCHAR *name, DWORD level, BYTE *buf, DWORD cb, DWORD *needed, DWORD *returned) { return EnumPrintersA(flags, NULL, level, buf, cb, needed, returned); }
static BOOL WINAPI OpenPrinterA(char *name, HANDLE *h, void *def) { if (h) *h = 0; P->last_error = 1801; return 0; }   /* ERROR_INVALID_PRINTER_NAME */
static BOOL WINAPI OpenPrinterW(WCHAR *name, HANDLE *h, void *def) { return OpenPrinterA(NULL, h, def); }
static BOOL WINAPI ClosePrinter(HANDLE h) { return 1; }
static DWORD WINAPI StartDocPrinterA(HANDLE h, DWORD level, BYTE *info) { return 0; }
static BOOL WINAPI EndDocPrinter(HANDLE h) { return 0; }
static BOOL WINAPI StartPagePrinter(HANDLE h) { return 0; }
static BOOL WINAPI EndPagePrinter(HANDLE h) { return 0; }
static BOOL WINAPI WritePrinter(HANDLE h, void *buf, DWORD n, DWORD *written) { if (written) *written = 0; return 0; }
static BOOL WINAPI GetDefaultPrinterA(char *buf, DWORD *n) { P->last_error = 1168; return 0; }   /* ERROR_NOT_FOUND */
static BOOL WINAPI GetDefaultPrinterW(WCHAR *buf, DWORD *n) { P->last_error = 1168; return 0; }
const w32_export_t winspool_exports[] = {
    E(EnumPrintersA), E(EnumPrintersW), E(OpenPrinterA), E(OpenPrinterW), E(ClosePrinter), E(StartDocPrinterA), { "StartDocPrinterW", (void *)StartDocPrinterA },
    E(EndDocPrinter), E(StartPagePrinter), E(EndPagePrinter), E(WritePrinter), E(GetDefaultPrinterA), E(GetDefaultPrinterW),
};
const int winspool_count = ARRAY_LEN(winspool_exports);
/* a class menu, by ordinal or by its (ANSI) name */
HANDLE WINAPI w32_load_menu(HINSTANCE inst, uintptr_t name) { return LoadMenuA2(inst, (const char *)name); }
