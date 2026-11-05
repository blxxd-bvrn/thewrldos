/* Immediate-mode widgets. Widgets are evaluated during a window's paint pass;
 * the WM re-runs paint synchronously for every click so widgets can react. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"

void ui_begin(window_t *w) { w->ui.next_id = 0; w->ui.cursor = 0; w->ui.hot = 0; }
void ui_end(window_t *w) {
    ui_t *u = &w->ui;
    if (u->released) u->active = 0;
    if (u->pressed && !u->hot) u->focus = 0;
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
    bool focus = w->ui.focus == id || (is_default && !w->ui.focus);
    if (enabled && w->ui.has_key && focus && (w->ui.key.key == KEY_ENTER || w->ui.key.key == ' ')) { clicked = true; w->ui.has_key = false; }
    draw_button(w->surf, r, label, hot && enabled, held, focus && enabled, enabled);
    return clicked;
}
bool ui_button(window_t *w, rect_t r, const char *label) { return ui_button_ex(w, r, label, true, false); }

bool ui_icon_button(window_t *w, rect_t r, int icon, const char *label) {
    int id = ui_id(w);
    bool hot, held;
    bool clicked = behave(w, id, r, &hot, &held);
    canvas_t *c = w->surf;
    if (hot || held) {
        if (T.classic) bevel(c, r.x, r.y, r.w, r.h, !held);
        else { fill_round_rect(c, r.x, r.y, r.w, r.h, 3, held ? HEX(0xC1D2EE) : HEX(0xCEDBF2)); draw_round_rect(c, r.x, r.y, r.w, r.h, 3, HEX(0x8EA8D8)); }
    }
    int isz = r.h >= 40 ? 32 : r.h >= 24 && !label ? 24 : 16;
    if (isz == 24) isz = 16;
    int o = held ? 1 : 0;
    if (label && r.w > r.h + 10) {
        draw_icon(c, icon, isz, r.x + 4 + o, r.y + (r.h - isz) / 2 + o);
        draw_text(c, FONT_UI, r.x + isz + 8 + o, r.y + (r.h - font_height(FONT_UI)) / 2 + o, label, BLACK);
    } else draw_icon(c, icon, isz, r.x + (r.w - isz) / 2 + o, r.y + (r.h - isz) / 2 + o);
    return clicked;
}

bool ui_link(window_t *w, int x, int y, int font, const char *label, uint32_t col) {
    int id = ui_id(w);
    rect_t r = R(x, y, text_width(font, label), font_height(font));
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    draw_text(w->surf, font, x, y, label, hot ? lighten(col, 60) : col);
    if (hot) { hline(w->surf, x, y + font_ascent(font) + 2, r.w, hot ? lighten(col, 60) : col); w->ui.cursor = CURSOR_HAND; }
    return clicked;
}
bool ui_checkbox(window_t *w, int x, int y, const char *label, bool *v) {
    int id = ui_id(w);
    rect_t r = R(x, y, 18 + text_width(FONT_UI, label), 15);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    if (clicked) { *v = !*v; w->ui.focus = id; }
    draw_check(w->surf, x, y + 1, *v, hot);
    draw_text(w->surf, FONT_UI, x + 19, y + 1, label, BLACK);
    return clicked;
}
bool ui_radio(window_t *w, int x, int y, const char *label, bool checked) {
    int id = ui_id(w);
    rect_t r = R(x, y, 18 + text_width(FONT_UI, label), 15);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    draw_radio(w->surf, x, y + 1, checked, hot);
    draw_text(w->surf, FONT_UI, x + 19, y + 1, label, BLACK);
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
    canvas_t *c = w->surf;
    int cy = r.y + r.h / 2;
    if (T.classic) { fill_rect(c, r.x, cy - 2, r.w, 4, WHITE); bevel_deep(c, r.x, cy - 2, r.w, 4, false); }
    else { fill_rect(c, r.x, cy - 2, r.w, 4, HEX(0xF3F3EF)); draw_rect(c, r.x, cy - 2, r.w, 4, HEX(0x9D9C99)); }
    for (int i = 0; i <= 10; i++) vline(c, r.x + 5 + (r.w - 10) * i / 10, r.y + r.h - 3, 3, HEX(0xA1A192));
    int tx = r.x + 5 + (r.w - 10) * (*v - lo) / MAX(1, hi - lo);
    if (T.classic) { fill_rect(c, tx - 5, r.y + 2, 11, r.h - 7, T.face); bevel_deep(c, tx - 5, r.y + 2, 11, r.h - 7, true); }
    else {
        fill_round_rect(c, tx - 5, r.y + 2, 11, r.h - 7, 2, hot || u->active == id ? HEX(0xE5A03C) : HEX(0x3D8B3D));
        gradient_round_rect(c, tx - 4, r.y + 3, 9, r.h - 9, 2, WHITE, HEX(0xE3E3DB));
    }
    return changed;
}
bool ui_textbox(window_t *w, rect_t r, char *buf, int cap) {
    int id = ui_id(w);
    ui_t *u = &w->ui;
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    if (clicked || (u->pressed && hot)) u->focus = id;
    if (hot) u->cursor = CURSOR_IBEAM;
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
    canvas_t *c = w->surf;
    draw_sunken_box(c, r, WHITE);
    rect_t old = canvas_push_clip(c, R(r.x + 2, r.y + 1, r.w - 4, r.h - 2));
    int tw = text_width(FONT_UI, buf);
    int off = tw > r.w - 10 ? tw - (r.w - 10) : 0;
    int ty = r.y + (r.h - font_height(FONT_UI)) / 2;
    draw_text(c, FONT_UI, r.x + 4 - off, ty, buf, BLACK);
    if (focused && (timer_ticks() / 500) % 2 == 0) vline(c, r.x + 4 - off + tw, ty, font_height(FONT_UI), BLACK);
    c->clip = old;
    if (focused) { extern void ui_blink_request(window_t *w); ui_blink_request(w); }
    return enter;
}
static void blink_tick(window_t *w) { w->dirty = true; }
void ui_blink_request(window_t *w) { if (!w->timer) wm_set_timer(w, 500, blink_tick); }

int ui_list(window_t *w, rect_t r, const char **items, int n, int *sel, int *scroll) {
    int id = ui_id(w);
    ui_t *u = &w->ui;
    bool hot;
    behave(w, id, r, &hot, NULL);
    int ih = font_height(FONT_UI) + 4, vis = (r.h - 4) / ih;
    int result = -1;
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
        if (*sel < *scroll) *scroll = *sel;
        if (*sel >= *scroll + vis) *scroll = *sel - vis + 1;
        u->has_key = false;
    }
    canvas_t *c = w->surf;
    draw_sunken_box(c, r, WHITE);
    rect_t old = canvas_push_clip(c, R(r.x + 2, r.y + 2, r.w - 4, r.h - 4));
    for (int i = *scroll; i < n && i < *scroll + vis + 1; i++) {
        int y = r.y + 2 + (i - *scroll) * ih;
        if (i == *sel) fill_rect(c, r.x + 2, y, r.w - 4, ih, T.select);
        draw_text_ellipsis(c, FONT_UI, r.x + 5, y + 2, r.w - 30, items[i], i == *sel ? T.select_text : BLACK);
    }
    c->clip = old;
    if (n > vis) draw_scrollbar(c, R(r.x + r.w - 18, r.y + 1, 17, r.h - 2), n, vis, *scroll, true);
    return result;
}
bool ui_tab(window_t *w, rect_t r, const char *label, bool active) {
    int id = ui_id(w);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    draw_tab(w->surf, r, label, active, hot);
    return clicked;
}
static window_t *dd_win; static int *dd_sel;
static void dd_cb(void *ctx, int id) { (void)ctx; if (dd_sel) *dd_sel = id; if (dd_win && wm_alive(dd_win)) { dd_win->dirty = true; if (dd_win->menu_cmd) dd_win->menu_cmd(dd_win, -1000 - id); } }
bool ui_dropdown(window_t *w, rect_t r, const char **items, int n, int *sel) {
    int id = ui_id(w);
    bool hot;
    bool clicked = behave(w, id, r, &hot, NULL);
    canvas_t *c = w->surf;
    draw_sunken_box(c, r, WHITE);
    if (*sel >= 0 && *sel < n) draw_text_ellipsis(c, FONT_UI, r.x + 4, r.y + (r.h - font_height(FONT_UI)) / 2, r.w - 24, items[*sel], BLACK);
    rect_t b = R(r.x + r.w - 18, r.y + 1, 17, r.h - 2);
    if (T.classic) { fill_rect(c, b.x, b.y, b.w, b.h, T.face); bevel_deep(c, b.x, b.y, b.w, b.h, true); }
    else { fill_round_rect(c, b.x, b.y, b.w, b.h, 2, hot ? HEX(0xB8D0F8) : HEX(0xC4D6F8)); gradient_round_rect(c, b.x + 1, b.y + 1, b.w - 2, b.h - 2, 2, HEX(0xE6EEFC), HEX(0xB9CDF5)); }
    draw_arrow(c, b.x + 8, b.y + b.h / 2 - 1, 1, T.classic ? BLACK : HEX(0x4D6185));
    if (clicked) {
        menu_item_t mi[20]; int k = MIN(n, 20);
        for (int i = 0; i < k; i++) { mi[i].label = items[i]; mi[i].id = i; mi[i].flags = i == *sel ? MI_CHECKED | MI_RADIO : 0; }
        rect_t cr = wm_client_rect(w);
        dd_win = w; dd_sel = sel;
        popup_menu(cr.x + r.x, cr.y + r.y + r.h, mi, k, dd_cb, NULL);
    }
    return clicked;
}
