/* Immediate-mode widgets. Widgets are evaluated during a window's paint pass;
 * the WM re-runs paint synchronously for every click so widgets can react. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"

void ui_begin(window_t *w) {
    ui_t *u = &w->ui;
    u->next_id = 0; u->cursor = 0; u->hot = 0;
    memcpy(u->pf, u->fids, sizeof u->pf); u->npf = u->nf; u->nf = 0;
    /* Tab / Shift+Tab: the next or previous control of the last paint takes the focus */
    if (u->has_key && u->key.key == KEY_TAB && !(u->key.mods & (MOD_ALT | MOD_CTRL)) && u->npf) {
        int i = 0;
        while (i < u->npf && u->pf[i] != u->focus) i++;
        bool back = u->key.mods & MOD_SHIFT;
        if (i == u->npf) i = back ? u->npf - 1 : 0;
        else i = (i + (back ? u->npf - 1 : 1)) % u->npf;
        u->focus = u->pf[i]; u->kbd = true; u->has_key = false;
    }
}
void ui_end(window_t *w) {
    ui_t *u = &w->ui;
    if (u->released) u->active = 0;
    if (u->pressed && !u->hot) u->focus = 0;
    if (u->pressed) u->kbd = false;                   /* a click: no focus rectangle until the keyboard is used */
}
/* a widget that can take the keyboard focus; ring: draw the dotted focus rectangle around r */
static void focusable(window_t *w, int id, rect_t r, bool ring) {
    ui_t *u = &w->ui;
    if (u->nf < (int)ARRAY_LEN(u->fids)) u->fids[u->nf++] = id;
    if (ring && u->focus == id && u->kbd) dotted_rect(w->surf, r.x - 2, r.y - 1, r.w + 4, r.h + 2, T.text);
}
/* Enter or Space on the focused widget */
static bool key_press(window_t *w, int id) {
    ui_t *u = &w->ui;
    if (u->focus == id && u->has_key && (u->key.key == KEY_ENTER || u->key.key == ' ')) { u->has_key = false; return true; }
    return false;
}
static bool key_is(window_t *w, int id, int k) {
    ui_t *u = &w->ui;
    if (u->focus == id && u->has_key && u->key.key == k) { u->has_key = false; return true; }
    return false;
}
int ui_id(window_t *w) { return ++w->ui.next_id; }

static bool hover(window_t *w, rect_t r) { return w->ui.in_window && pt_in(r, w->ui.mx, w->ui.my) && (!w->ui.active || w->ui.active == w->ui.next_id); }
bool ui_hover(window_t *w, rect_t r) { return w->ui.in_window && pt_in(r, w->ui.mx, w->ui.my); }

/* generic press/release logic; returns true on click */
static bool behave(window_t *w, int id, rect_t r, bool *hot_out, bool *held_out) {
    ui_t *u = &w->ui;
    bool hot = u->in_window && pt_in(r, u->mx, u->my) && (!u->active || u->active == id);
    if (hot) u->hot = id;
    if (hot && u->pressed) { u->active = id; }
    bool clicked = false;
    if (u->released && u->active == id) { if (hot) clicked = true; }
    if (hot_out) *hot_out = hot;
    if (held_out) *held_out = u->active == id && u->down && hot;
    return clicked;
}
bool ui_click_area(window_t *w, rect_t r) { int id = ui_id(w); return behave(w, id, r, NULL, NULL); }

bool ui_button_ex(window_t *w, rect_t r, const char *label, bool enabled, bool is_default) {
    int id = ui_id(w);
    bool hot = false, held = false;
    bool clicked = enabled ? behave(w, id, r, &hot, &held) : false;
    if (enabled) focusable(w, id, r, false);
    bool focus = w->ui.focus == id || (is_default && !w->ui.focus);
    if (enabled && w->ui.has_key && focus && (w->ui.key.key == KEY_ENTER || w->ui.key.key == ' ')) { clicked = true; w->ui.has_key = false; }
    draw_button(w->surf, r, label, hot && enabled, held, focus && enabled, enabled);
    return clicked;
}
bool ui_button(window_t *w, rect_t r, const char *label) { return ui_button_ex(w, r, label, true, false); }

bool ui_icon_button(window_t *w, rect_t r, int icon, const char *label) {
    int id = ui_id(w);
    bool hot, held;
    bool clicked = behave(w, id, r, &hot, &held) || key_press(w, id);
    canvas_t *c = w->surf;
    focusable(w, id, r, true);
    if (hot || held) {
        if (T.classic) bevel(c, r.x, r.y, r.w, r.h, !held);
        else if (dark_mode) { fill_round_rect(c, r.x, r.y, r.w, r.h, 3, held ? T.btn_hot_b : T.face_light); draw_round_rect(c, r.x, r.y, r.w, r.h, 3, T.face_dark); }
        else { fill_round_rect(c, r.x, r.y, r.w, r.h, 3, held ? HEX(0xC1D2EE) : HEX(0xCEDBF2)); draw_round_rect(c, r.x, r.y, r.w, r.h, 3, HEX(0x8EA8D8)); }
    }
    int isz = r.h >= 40 ? 32 : r.h >= 24 && !label ? 24 : 16;
    if (isz == 24) isz = 16;
    int o = held ? 1 : 0;
    if (label && r.w > r.h + 10) {
        draw_icon(c, icon, isz, r.x + 4 + o, r.y + (r.h - isz) / 2 + o);
        draw_text(c, FONT_UI, r.x + isz + 8 + o, r.y + (r.h - font_height(FONT_UI)) / 2 + o, label, T.text);
    } else draw_icon(c, icon, isz, r.x + (r.w - isz) / 2 + o, r.y + (r.h - isz) / 2 + o);
    return clicked;
}

bool ui_link(window_t *w, int x, int y, int font, const char *label, uint32_t col) {
    int id = ui_id(w);
    rect_t r = R(x, y, text_width(font, label), font_height(font));
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL) || key_press(w, id);
    focusable(w, id, r, true);
    draw_text(w->surf, font, x, y, label, hot ? lighten(col, 60) : col);
    if (hot) { hline(w->surf, x, y + font_ascent(font) + 2, r.w, hot ? lighten(col, 60) : col); w->ui.cursor = CURSOR_HAND; }
    return clicked;
}
/* A link whose text wraps inside maxw instead of running past it.  The whole
 * wrapped block is clickable and highlights on hover. */
bool ui_link_wrapped(window_t *w, int x, int y, int maxw, int font, const char *label, uint32_t col) {
    int id = ui_id(w);
    int h = text_wrapped_height(font, maxw, label, 2);
    rect_t r = R(x, y, maxw, h);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL) || key_press(w, id);
    focusable(w, id, r, true);
    draw_text_wrapped(w->surf, font, x, y, maxw, label, hot ? lighten(col, 60) : col, 2);
    if (hot) w->ui.cursor = CURSOR_HAND;
    return clicked;
}
bool ui_checkbox(window_t *w, int x, int y, const char *label, bool *v) {
    int id = ui_id(w);
    rect_t r = R(x, y, 18 + text_width(FONT_UI, label), 15);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL) || key_is(w, id, ' ');
    focusable(w, id, r, true);
    if (clicked) { *v = !*v; w->ui.focus = id; }
    draw_check(w->surf, x, y + 1, *v, hot);
    draw_text(w->surf, FONT_UI, x + 19, y + 1, label, T.text);
    return clicked;
}
bool ui_radio(window_t *w, int x, int y, const char *label, bool checked) {
    int id = ui_id(w);
    rect_t r = R(x, y, 18 + text_width(FONT_UI, label), 15);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL) || key_press(w, id);
    focusable(w, id, r, true);
    draw_radio(w->surf, x, y + 1, checked, hot);
    draw_text(w->surf, FONT_UI, x + 19, y + 1, label, T.text);
    return clicked;
}
bool ui_slider(window_t *w, rect_t r, int *v, int lo, int hi) {
    int id = ui_id(w);
    ui_t *u = &w->ui;
    bool hot;
    behave(w, id, r, &hot, NULL);
    bool changed = false;
    if (u->active == id && u->down) {
        int nv = lo + (u->mx - r.x - 5) * (hi - lo) / MAX(1, r.w - 10);
        nv = CLAMP(nv, lo, hi);
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (u->released && u->active == id) changed = true;
    if (hot && u->wheel) { *v = CLAMP(*v - u->wheel * MAX(1, (hi - lo) / 20), lo, hi); changed = true; }
    focusable(w, id, r, true);
    if (u->focus == id && u->has_key) {                   /* arrows step, Page Up/Down jump, Home/End */
        int k = u->key.key, step = MAX(1, (hi - lo) / 20), nv = *v;
        if (k == KEY_LEFT || k == KEY_DOWN) nv -= step; else if (k == KEY_RIGHT || k == KEY_UP) nv += step;
        else if (k == KEY_PGDN) nv -= step * 4; else if (k == KEY_PGUP) nv += step * 4;
        else if (k == KEY_HOME) nv = lo; else if (k == KEY_END) nv = hi;
        else k = 0;
        if (k) { nv = CLAMP(nv, lo, hi); changed = nv != *v; *v = nv; u->has_key = false; }
    }
    canvas_t *c = w->surf;
    int cy = r.y + r.h / 2;
    if (T.classic) { fill_rect(c, r.x, cy - 2, r.w, 4, T.window_bg); bevel_deep(c, r.x, cy - 2, r.w, 4, false); }
    else { fill_rect(c, r.x, cy - 2, r.w, 4, T.sb_track); draw_rect(c, r.x, cy - 2, r.w, 4, T.sb_border); }
    for (int i = 0; i <= 10; i++) vline(c, r.x + 5 + (r.w - 10) * i / 10, r.y + r.h - 3, 3, T.gray_text);
    int tx = r.x + 5 + (r.w - 10) * (*v - lo) / MAX(1, hi - lo);
    if (T.classic) { fill_rect(c, tx - 5, r.y + 2, 11, r.h - 7, T.face); bevel_deep(c, tx - 5, r.y + 2, 11, r.h - 7, true); }
    else {
        fill_round_rect(c, tx - 5, r.y + 2, 11, r.h - 7, 2, hot || u->active == id ? HEX(0xE5A03C) : T.check);
        gradient_round_rect(c, tx - 4, r.y + 3, 9, r.h - 9, 2, T.face_light, T.face);
    }
    return changed;
}
/* flat: the caller draws the box (text in fg); *focused_out reports keyboard focus */
bool ui_textbox_ex(window_t *w, rect_t r, char *buf, int cap, bool flat, uint32_t fg, bool *focused_out) {
    int id = ui_id(w);
    ui_t *u = &w->ui;
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    if (clicked || (u->pressed && hot)) u->focus = id;
    if (hot) u->cursor = CURSOR_IBEAM;
    focusable(w, id, r, false);
    bool focused = u->focus == id;
    if (focused_out) *focused_out = focused;
    bool enter = false;
    if (focused && u->has_key) {
        int len = strlen(buf);
        key_event_t *k = &u->key;
        if (k->key == KEY_BACKSPACE) { if (len) buf[len - 1] = 0; }
        else if (k->key == KEY_ENTER) enter = true;
        else if (k->ch >= 32 && k->ch < 127 && len < cap - 1 && !(k->mods & MOD_CTRL)) { buf[len] = k->ch; buf[len + 1] = 0; }
        if (k->key != KEY_TAB && k->key != KEY_ESC) u->has_key = false;
    }
    canvas_t *c = w->surf;
    if (!flat) { draw_sunken_box(c, r, T.window_bg); fg = T.text; }
    rect_t old = canvas_push_clip(c, R(r.x + 2, r.y + 1, r.w - 4, r.h - 2));
    int tw = text_width(FONT_UI, buf);
    int off = tw > r.w - 10 ? tw - (r.w - 10) : 0;
    int ty = r.y + (r.h - font_height(FONT_UI)) / 2;
    draw_text(c, FONT_UI, r.x + 4 - off, ty, buf, fg);
    if (focused && (timer_ticks() / 500) % 2 == 0) vline(c, r.x + 4 - off + tw, ty, font_height(FONT_UI), fg);
    c->clip = old;
    if (focused) { extern void ui_blink_request(window_t *w); ui_blink_request(w); }
    return enter;
}
bool ui_textbox(window_t *w, rect_t r, char *buf, int cap) { return ui_textbox_ex(w, r, buf, cap, false, 0, NULL); }
static void blink_tick(window_t *w) { w->dirty = true; }
void ui_blink_request(window_t *w) { if (!w->timer) wm_set_timer(w, 500, blink_tick); }
bool ui_password(window_t *w, rect_t r, char *buf, int cap) {
    int id = ui_id(w);
    ui_t *u = &w->ui;
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    if (clicked || (u->pressed && hot)) u->focus = id;
    if (hot) u->cursor = CURSOR_IBEAM;
    focusable(w, id, r, false);
    bool focused = u->focus == id;
    bool enter = false;
    if (focused && u->has_key) {
        int len = strlen(buf);
        key_event_t *k = &u->key;
        if (k->key == KEY_BACKSPACE) { if (len) buf[len - 1] = 0; }
        else if (k->key == KEY_ENTER) enter = true;
        else if (k->ch >= 32 && k->ch < 127 && len < cap - 1 && !(k->mods & MOD_CTRL)) { buf[len] = k->ch; buf[len + 1] = 0; }
        if (k->key != KEY_TAB && k->key != KEY_ESC) u->has_key = false;
    }
    char mask[64]; int n = MIN((int)strlen(buf), (int)sizeof mask - 1);
    for (int i = 0; i < n; i++) mask[i] = '*';
    mask[n] = 0;
    canvas_t *c = w->surf;
    draw_sunken_box(c, r, T.window_bg);
    rect_t old = canvas_push_clip(c, R(r.x + 2, r.y + 1, r.w - 4, r.h - 2));
    int tw = text_width(FONT_UI, mask);
    int off = tw > r.w - 10 ? tw - (r.w - 10) : 0;
    int ty = r.y + (r.h - font_height(FONT_UI)) / 2;
    draw_text(c, FONT_UI, r.x + 4 - off, ty, mask, T.text);
    if (focused && (timer_ticks() / 500) % 2 == 0) vline(c, r.x + 4 - off + tw, ty, font_height(FONT_UI), T.text);
    c->clip = old;
    if (focused) ui_blink_request(w);
    return enter;
}

int ui_list(window_t *w, rect_t r, const char **items, int n, int *sel, int *scroll) {
    int id = ui_id(w);
    ui_t *u = &w->ui;
    bool hot;
    behave(w, id, r, &hot, NULL);
    int ih = font_height(FONT_UI) + 4, vis = (r.h - 4) / ih;
    int result = -1;
    focusable(w, id, r, true);
    if (u->focus == id && u->has_key && *sel < 0 && n && (u->key.key == KEY_DOWN || u->key.key == KEY_UP)) { *sel = 0; u->has_key = false; }
    if (hot && u->wheel) *scroll = CLAMP(*scroll + u->wheel * 3, 0, MAX(0, n - vis));
    if (hot && u->pressed && u->mx < r.x + r.w - 17) {
        int i = *scroll + (u->my - r.y - 2) / ih;
        if (i >= 0 && i < n) { *sel = i; u->focus = id; }
    }
    if (hot && u->pressed && u->mx >= r.x + r.w - 17 && n > vis) {
        if (u->my < r.y + 17) (*scroll)--; else if (u->my > r.y + r.h - 17) (*scroll)++;
        else *scroll = (u->my - r.y - 17) * (n - vis) / MAX(1, r.h - 34);
        *scroll = CLAMP(*scroll, 0, MAX(0, n - vis));
    }
    if (hot && u->dblclick && *sel >= 0) result = *sel;
    if (u->focus == id && u->has_key) {
        if (u->key.key == KEY_DOWN && *sel < n - 1) (*sel)++;
        if (u->key.key == KEY_UP && *sel > 0) (*sel)--;
        if (u->key.key == KEY_ENTER && *sel >= 0) result = *sel;
        if (u->key.key == KEY_HOME && n) *sel = 0;
        if (u->key.key == KEY_END && n) *sel = n - 1;
        if (u->key.key == KEY_PGDN && n) *sel = MIN(n - 1, *sel + vis);
        if (u->key.key == KEY_PGUP && n) *sel = MAX(0, *sel - vis);
        if (*sel < *scroll) *scroll = *sel;
        if (*sel >= *scroll + vis) *scroll = *sel - vis + 1;
        u->has_key = false;
    }
    canvas_t *c = w->surf;
    draw_sunken_box(c, r, T.window_bg);
    rect_t old = canvas_push_clip(c, R(r.x + 2, r.y + 2, r.w - 4, r.h - 4));
    for (int i = *scroll; i < n && i < *scroll + vis + 1; i++) {
        int y = r.y + 2 + (i - *scroll) * ih;
        if (i == *sel) fill_rect(c, r.x + 2, y, r.w - 4, ih, T.select);
        draw_text_ellipsis(c, FONT_UI, r.x + 5, y + 2, r.w - 30, items[i], i == *sel ? T.select_text : T.text);
    }
    c->clip = old;
    if (n > vis) draw_scrollbar(c, R(r.x + r.w - 18, r.y + 1, 17, r.h - 2), n, vis, *scroll, true);
    return result;
}
/* A vertical scrollbar that works: the arrows step by one, the track pages, the thumb drags.
 * pos is the first visible row of total rows, visible of which fit. True when pos changed.
 * (The mouse wheel is left to the caller, which knows the area it scrolls.) */
static window_t *sb_drag_w; static int sb_drag_id, sb_drag_off;
bool ui_vscroll(window_t *w, rect_t r, int *pos, int total, int visible) {
    int id = ui_id(w);
    ui_t *u = &w->ui;
    bool hot;
    behave(w, id, r, &hot, NULL);
    int maxp = MAX(0, total - visible), thick = r.w, avail = r.h - 2 * thick;
    int tl = total > visible && total > 0 ? MAX(12, avail * visible / total) : avail;
    int old = *pos;
    *pos = CLAMP(*pos, 0, maxp);
    int tp = maxp ? (avail - tl) * *pos / maxp : 0;
    if (hot && u->pressed && maxp) {
        int y = u->my - r.y;
        if (y < thick) (*pos)--;
        else if (y >= r.h - thick) (*pos)++;
        else if (y < thick + tp) *pos -= MAX(1, visible - 1);
        else if (y >= thick + tp + tl) *pos += MAX(1, visible - 1);
        else { sb_drag_w = w; sb_drag_id = id; sb_drag_off = y - thick - tp; }
    }
    if (sb_drag_w == w && sb_drag_id == id) {
        if (!u->down) sb_drag_w = NULL;
        else if (avail > tl) *pos = (int)((int64_t)(u->my - r.y - thick - sb_drag_off) * maxp / (avail - tl));
    }
    *pos = CLAMP(*pos, 0, maxp);
    draw_scrollbar(w->surf, r, total, visible, *pos, true);
    return *pos != old;
}
bool ui_tab(window_t *w, rect_t r, const char *label, bool active) {
    int id = ui_id(w);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL) || key_press(w, id);
    draw_tab(w->surf, r, label, active, hot);
    focusable(w, id, R(r.x + 2, r.y + 2, r.w - 4, r.h - 4), true);
    return clicked;
}
static window_t *dd_win; static int *dd_sel;
static void dd_cb(void *ctx, int id) { (void)ctx; if (dd_sel) *dd_sel = id; if (dd_win && wm_alive(dd_win)) { dd_win->dirty = true; if (dd_win->menu_cmd) dd_win->menu_cmd(dd_win, -1000 - id); } }
bool ui_dropdown(window_t *w, rect_t r, const char **items, int n, int *sel) {
    int id = ui_id(w);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL) || key_press(w, id) || (w->ui.focus == id && w->ui.has_key && (w->ui.key.mods & MOD_ALT) && w->ui.key.key == KEY_DOWN);
    ui_t *u = &w->ui;
    if (u->focus == id && u->has_key && (u->key.key == KEY_DOWN || u->key.key == KEY_UP) && !(u->key.mods & MOD_ALT)) {   /* arrows pick without opening */
        int nv = CLAMP(*sel + (u->key.key == KEY_DOWN ? 1 : -1), 0, n - 1);
        if (nv != *sel) { *sel = nv; if (w->menu_cmd) w->menu_cmd(w, -1000 - nv); w->dirty = true; }
        u->has_key = false;
    }
    canvas_t *c = w->surf;
    focusable(w, id, r, true);
    draw_sunken_box(c, r, T.window_bg);
    if (*sel >= 0 && *sel < n) draw_text_ellipsis(c, FONT_UI, r.x + 4, r.y + (r.h - font_height(FONT_UI)) / 2, r.w - 24, items[*sel], T.text);
    rect_t b = R(r.x + r.w - 18, r.y + 1, 17, r.h - 2);
    if (T.classic) { fill_rect(c, b.x, b.y, b.w, b.h, T.face); bevel_deep(c, b.x, b.y, b.w, b.h, true); }
    else if (dark_mode) { fill_round_rect(c, b.x, b.y, b.w, b.h, 2, T.btn_border); gradient_round_rect(c, b.x + 1, b.y + 1, b.w - 2, b.h - 2, 2, T.btn_top, T.btn_bot); }
    else { fill_round_rect(c, b.x, b.y, b.w, b.h, 2, hot ? HEX(0xB8D0F8) : HEX(0xC4D6F8)); gradient_round_rect(c, b.x + 1, b.y + 1, b.w - 2, b.h - 2, 2, HEX(0xE6EEFC), HEX(0xB9CDF5)); }
    draw_arrow(c, b.x + 8, b.y + b.h / 2 - 1, 1, T.classic || dark_mode ? T.text : HEX(0x4D6185));
    if (clicked) {
        menu_item_t mi[32]; int k = MIN(n, 32);
        for (int i = 0; i < k; i++) { mi[i].label = items[i]; mi[i].id = i; mi[i].flags = i == *sel ? MI_CHECKED | MI_RADIO : 0; }
        rect_t cr = wm_client_rect(w);
        dd_win = w; dd_sel = sel;
        popup_menu(cr.x + r.x, cr.y + r.y + r.h, mi, k, dd_cb, NULL);
    }
    return clicked;
}
