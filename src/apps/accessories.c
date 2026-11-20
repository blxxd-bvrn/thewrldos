/* Task Manager, Calculator, Paint, Snake, About, Image Viewer, Properties */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

void file_dialog(const char *title, const char *initial, bool save, const char *ext, void (*cb)(void *, const char *), void *ctx);

static void free_data(window_t *w) { kfree(w->data); }

/* ======================= BMP ======================= */
/* bmp_load is in Rust (rust/src/imgdec.rs) */
uint8_t *bmp_save(canvas_t *c, uint32_t *size) {
    uint32_t stride = (c->w * 3 + 3) & ~3u, img = stride * c->h, total = 54 + img;
    uint8_t *d = kzalloc(total);
    d[0] = 'B'; d[1] = 'M'; *(uint32_t *)(d + 2) = total; *(uint32_t *)(d + 10) = 54; *(uint32_t *)(d + 14) = 40;
    *(int32_t *)(d + 18) = c->w; *(int32_t *)(d + 22) = c->h; *(uint16_t *)(d + 26) = 1; *(uint16_t *)(d + 28) = 24; *(uint32_t *)(d + 34) = img;
    for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) {
        uint32_t p = c->px[y * c->stride + x]; uint8_t *o = d + 54 + stride * (c->h - 1 - y) + x * 3;
        o[0] = C_B(p); o[1] = C_G(p); o[2] = C_R(p);
    }
    *size = total;
    return d;
}

/* ======================= Task Manager ======================= */
typedef struct { int tab, sel, scroll; uint8_t cpu[120], mem[120]; uint32_t last_idle, last_tick; int cpu_now; } tm_t;
static void tm_sample(window_t *w) {
    tm_t *t = w->data;
    uint32_t now = timer_ticks(), idle = idle_ticks;
    uint32_t dt = now - t->last_tick, di = idle - t->last_idle;
    int pct = dt ? 100 - (int)(di * 100 / dt) : 0;
    t->cpu_now = CLAMP(pct, 0, 100);
    memmove(t->cpu, t->cpu + 1, 119); t->cpu[119] = t->cpu_now;
    memmove(t->mem, t->mem + 1, 119); t->mem[119] = mm_used() * 100 / mm_total();
    t->last_idle = idle; t->last_tick = now;
    w->dirty = true;
}
static void graph(canvas_t *c, rect_t r, uint8_t *data, uint32_t col) {
    fill_rect(c, r.x, r.y, r.w, r.h, BLACK);
    for (int x = 0; x < r.w; x += 12) vline(c, r.x + x, r.y, r.h, HEX(0x008040));
    for (int y = 0; y < r.h; y += 12) hline(c, r.x, r.y + y, r.w, HEX(0x008040));
    int px = -1, py = 0;
    for (int i = 0; i < 120; i++) {
        int x = r.x + i * (r.w - 1) / 119, y = r.y + r.h - 1 - data[i] * (r.h - 2) / 100;
        if (px >= 0) draw_line(c, px, py, x, y, col);
        px = x; py = y;
    }
}
static void tm_paint(window_t *w) {
    tm_t *t = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    const char *tabs[] = { "Applications", "Processes", "Performance" };
    int tx = 6;
    for (int i = 0; i < 3; i++) { int tw = text_width(FONT_UI, tabs[i]) + 20; if (ui_tab(w, R(tx, 6, tw, 22), tabs[i], t->tab == i)) { t->tab = i; t->sel = -1; } tx += tw + 1; }
    rect_t body = R(6, 28, c->w - 12, c->h - 28 - 56);
    if (T.classic) bevel_deep(c, body.x, body.y, body.w, body.h, true); else draw_rect(c, body.x, body.y, body.w, body.h, T.face_dark);
    fill_rect(c, body.x + 1, body.y + 1, body.w - 2, body.h - 2, T.classic ? T.face : T.window_bg);
    rect_t lr = R(body.x + 10, body.y + 10, body.w - 20, body.h - 54);
    if (t->tab == 0) {
        draw_sunken_box(c, lr, T.window_bg);
        fill_rect(c, lr.x + 1, lr.y + 1, lr.w - 2, 20, T.face); draw_text(c, FONT_UI, lr.x + 8, lr.y + 4, "Task", T.text); draw_text(c, FONT_UI, lr.x + lr.w - 100, lr.y + 4, "Status", T.text);
        int n = 0; window_t *list[64];
        for (int i = 0; i < wm_window_count(); i++) { window_t *x = wm_window_at(i); if (x != w && !(x->flags & (WF_POPUP | WF_HIDDEN))) list[n++] = x; }
        for (int i = 0; i < n; i++) {
            rect_t r = R(lr.x + 2, lr.y + 22 + i * 20, lr.w - 4, 20);
            if (ui_click_area(w, r)) t->sel = i;
            if (w->ui.dblclick && t->sel == i) wm_focus(list[i]);
            if (t->sel == i) fill_rect(c, r.x, r.y, r.w, r.h, T.select);
            draw_icon(c, list[i]->icon >= 0 ? list[i]->icon : ICON_EXE, 16, r.x + 4, r.y + 2);
            draw_text_ellipsis(c, FONT_UI, r.x + 26, r.y + 3, r.w - 140, list[i]->title, t->sel == i ? T.select_text : T.text);
            draw_text(c, FONT_UI, lr.x + lr.w - 100, r.y + 3, list[i]->state == WS_MINIMIZED ? "Minimized" : "Running", t->sel == i ? T.select_text : T.text);
        }
        bool has = t->sel >= 0 && t->sel < n;
        if (ui_button_ex(w, R(body.x + body.w - 260, body.y + body.h - 36, 80, 24), "End Task", has, false) && has) wm_close(list[t->sel]);
        if (ui_button_ex(w, R(body.x + body.w - 172, body.y + body.h - 36, 80, 24), "Switch To", has, false) && has) wm_focus(list[t->sel]);
        if (ui_button(w, R(body.x + body.w - 84, body.y + body.h - 36, 74, 24), "New Task...")) shell_show_run();
    } else if (t->tab == 1) {
        draw_sunken_box(c, lr, T.window_bg);
        static const char *kinds[] = { "System", "Shell", "Win32", "App" };
        const char *hd[] = { "Image Name", "PID", "Type", "CPU time", "Mem Usage" }; int hx[] = { 8, 190, 240, 310, 400 };
        fill_rect(c, lr.x + 1, lr.y + 1, lr.w - 2, 20, T.face);
        for (int k = 0; k < 5; k++) draw_text(c, FONT_UI, lr.x + hx[k], lr.y + 4, hd[k], T.text);
        int i = 0; task_t *selt = NULL;
        for (task_t *k = task_list(); k; k = k->next, i++) {
            rect_t r = R(lr.x + 2, lr.y + 22 + i * 20, lr.w - 4, 20);
            if (r.y + 20 > lr.y + lr.h) break;
            if (ui_click_area(w, r)) t->sel = i;
            bool s = t->sel == i; if (s) selt = k;
            if (s) fill_rect(c, r.x, r.y, r.w, r.h, T.select);
            uint32_t col = s ? T.select_text : T.text; char b[48];
            draw_text_ellipsis(c, FONT_UI, lr.x + hx[0], r.y + 3, 175, k->name, col);
            snprintf(b, sizeof b, "%d", k->pid); draw_text(c, FONT_UI, lr.x + hx[1], r.y + 3, b, col);
            draw_text(c, FONT_UI, lr.x + hx[2], r.y + 3, kinds[k->kind & 3], col);
            uint32_t cs = k->pid == 0 ? 0 : k->cpu_ms / 1000; snprintf(b, sizeof b, "%u:%02u:%02u", cs / 3600, (cs / 60) % 60, cs % 60); draw_text(c, FONT_UI, lr.x + hx[3], r.y + 3, b, col);
            snprintf(b, sizeof b, "%u K", k->mem_bytes / 1024); draw_text(c, FONT_UI, lr.x + hx[4], r.y + 3, b, col);
        }
        bool can = selt && selt->pid != 0;
        if (ui_button_ex(w, R(body.x + body.w - 104, body.y + body.h - 36, 94, 24), "End Process", can, false) && can) { task_kill(selt->pid); t->sel = -1; }
    } else {
        char b[64];
        int gw = (body.w - 40) / 4;
        draw_group(c, R(body.x + 10, body.y + 8, gw, 120), "CPU Usage");
        fill_rect(c, body.x + 20, body.y + 28, gw - 20, 90, BLACK);
        int bh = t->cpu_now * 70 / 100;
        for (int y = 0; y < 70; y += 3) fill_rect(c, body.x + 30, body.y + 34 + y, gw - 40, 2, (70 - y) <= bh ? HEX(0x00FF00) : HEX(0x004000));
        snprintf(b, sizeof b, "%d %%", t->cpu_now); draw_text_centered(c, FONT_UI, R(body.x + 20, body.y + 104, gw - 20, 14), b, HEX(0x00FF00));
        draw_group(c, R(body.x + 20 + gw, body.y + 8, body.w - gw - 30, 120), "CPU Usage History");
        graph(c, R(body.x + 30 + gw, body.y + 28, body.w - gw - 50, 90), t->cpu, HEX(0x00FF00));
        draw_group(c, R(body.x + 10, body.y + 136, gw, 120), "Heap Usage");
        fill_rect(c, body.x + 20, body.y + 156, gw - 20, 90, BLACK);
        snprintf(b, sizeof b, "%u MB", mm_used() / (1024 * 1024)); draw_text_centered(c, FONT_UI, R(body.x + 20, body.y + 230, gw - 20, 14), b, HEX(0x00FF00));
        int mh = t->mem[119] * 70 / 100;
        for (int y = 0; y < 70; y += 3) fill_rect(c, body.x + 30, body.y + 160 + y, gw - 40, 2, (70 - y) <= mh ? HEX(0xFFFF00) : HEX(0x404000));
        draw_group(c, R(body.x + 20 + gw, body.y + 136, body.w - gw - 30, 120), "Memory Usage History");
        graph(c, R(body.x + 30 + gw, body.y + 156, body.w - gw - 50, 90), t->mem, HEX(0xFFFF00));
        int y = body.y + 266;
        snprintf(b, sizeof b, "Physical memory: %llu MB", (unsigned long long)(mm_phys_total() / (1024 * 1024))); draw_text(c, FONT_UI, body.x + 14, y, b, T.text);
        snprintf(b, sizeof b, "Kernel heap: %u / %u KB", mm_used() / 1024, mm_total() / 1024); draw_text(c, FONT_UI, body.x + 14, y + 18, b, T.text);
        uint32_t up = timer_ticks() / 1000; snprintf(b, sizeof b, "Up time: %u:%02u:%02u   Tasks: %d   Open windows: %d", up / 3600, (up / 60) % 60, up % 60, task_count(), wm_window_count());
        draw_text(c, FONT_UI, body.x + 260, y, b, T.text);
    }
    char st[96]; snprintf(st, sizeof st, "Processes: %d     CPU Usage: %d%%     Heap: %u MB / %u MB", task_count(), t->cpu_now, mm_used() >> 20, mm_total() >> 20);
    hline(c, 0, c->h - 22, c->w, T.face_dark);
    draw_text(c, FONT_UI, 8, c->h - 18, st, T.text);
}
static window_t *tm_win;
static void tm_destroy(window_t *w) { kfree(w->data); tm_win = NULL; }
void taskmgr_open(void) {
    if (tm_win && wm_alive(tm_win)) { wm_focus(tm_win); return; }
    window_t *w = wm_create(OS_NAME " Task Manager", -1, -1, 540, 470, WF_RESIZABLE);
    tm_t *t = kzalloc(sizeof *t); t->sel = -1; t->tab = 1; t->last_idle = idle_ticks; t->last_tick = timer_ticks();
    w->data = t; w->paint = tm_paint; w->destroy = tm_destroy; w->icon = ICON_TASKMGR; w->min_w = 460; w->min_h = 400;
    wm_set_timer(w, 1000, tm_sample);
    tm_win = w;
}

/* ======================= Calculator ======================= */
typedef struct { double acc, mem; char disp[40]; char op; bool fresh, err; } calc_t;
static void fmt_num(double v, char *b, int n) {
    if (v != v || v > 1e15 || v < -1e15) { strlcpy(b, "Error", n); return; }
    int64_t iv = (int64_t)v;
    if ((double)iv == v) { snprintf(b, n, "%lld", iv); return; }
    char t[64]; int neg = v < 0; if (neg) v = -v;
    uint64_t ip = (uint64_t)v; double fp = v - (double)ip;
    int k = snprintf(t, sizeof t, "%s%llu.", neg ? "-" : "", ip);
    for (int i = 0; i < 12 - (ip > 999 ? 4 : 0); i++) { fp *= 10; int d = (int)fp; t[k++] = '0' + d; fp -= d; }
    t[k] = 0;
    while (k > 0 && t[k - 1] == '0') t[--k] = 0;
    if (k > 0 && t[k - 1] == '.') t[--k] = 0;
    strlcpy(b, t, n);
}
static double parse_d(const char *s) { double v = 0, f = 0.1; int neg = 0; if (*s == '-') { neg = 1; s++; } while (isdigit(*s)) v = v * 10 + (*s++ - '0'); if (*s == '.') { s++; while (isdigit(*s)) { v += (*s++ - '0') * f; f /= 10; } } return neg ? -v : v; }
static double c_sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static void calc_apply(calc_t *c) {
    double v = parse_d(c->disp);
    switch (c->op) { case '+': c->acc += v; break; case '-': c->acc -= v; break; case '*': c->acc *= v; break; case '/': if (v == 0) { c->err = true; strcpy(c->disp, "Cannot divide by zero"); return; } c->acc /= v; break; default: c->acc = v; }
    fmt_num(c->acc, c->disp, sizeof c->disp);
}
static void calc_press(calc_t *c, const char *k) {
    if (c->err && strcmp(k, "C")) return;
    if (isdigit(k[0]) || !strcmp(k, ".")) {
        if (c->fresh) { strcpy(c->disp, "0"); c->fresh = false; }
        if (k[0] == '.' && strchr(c->disp, '.')) return;
        if (!strcmp(c->disp, "0") && k[0] != '.') c->disp[0] = 0;
        if (strlen(c->disp) < 16) strlcat(c->disp, k, sizeof c->disp);
    } else if (!strcmp(k, "C")) { c->acc = 0; c->op = 0; strcpy(c->disp, "0"); c->fresh = true; c->err = false; }
    else if (!strcmp(k, "CE")) { strcpy(c->disp, "0"); }
    else if (!strcmp(k, "Back")) { int l = strlen(c->disp); if (l > 1) c->disp[l - 1] = 0; else strcpy(c->disp, "0"); }
    else if (!strcmp(k, "+/-")) { double v = -parse_d(c->disp); fmt_num(v, c->disp, sizeof c->disp); }
    else if (!strcmp(k, "sqrt")) { double v = parse_d(c->disp); if (v < 0) { c->err = true; strcpy(c->disp, "Invalid input"); } else fmt_num(c_sqrt(v), c->disp, sizeof c->disp); c->fresh = true; }
    else if (!strcmp(k, "1/x")) { double v = parse_d(c->disp); if (v == 0) { c->err = true; strcpy(c->disp, "Cannot divide by zero"); } else fmt_num(1 / v, c->disp, sizeof c->disp); c->fresh = true; }
    else if (!strcmp(k, "%")) { double v = parse_d(c->disp); fmt_num(c->acc * v / 100, c->disp, sizeof c->disp); }
    else if (!strcmp(k, "MC")) c->mem = 0; else if (!strcmp(k, "MR")) { fmt_num(c->mem, c->disp, sizeof c->disp); c->fresh = true; }
    else if (!strcmp(k, "MS")) c->mem = parse_d(c->disp); else if (!strcmp(k, "M+")) c->mem += parse_d(c->disp);
    else if (!strcmp(k, "=")) { if (!c->fresh || c->op) calc_apply(c); c->op = 0; c->fresh = true; }
    else { if (!c->fresh) calc_apply(c); else if (!c->op) c->acc = parse_d(c->disp); c->op = k[0]; c->fresh = true; }
}
static void calc_paint(window_t *w) {
    calc_t *c = w->data; canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    draw_sunken_box(s, R(10, 10, s->w - 20, 28), T.window_bg);
    draw_text(s, FONT_LARGE, s->w - 18 - text_width(FONT_LARGE, c->disp), 13, c->disp, T.text);
    fill_rect(s, 12, 48, 30, 26, T.face); bevel_deep(s, 12, 48, 30, 26, false);
    if (c->mem != 0) draw_text_centered(s, FONT_UI, R(12, 48, 30, 26), "M", T.text);
    static const char *top[] = { "Back", "CE", "C" };
    for (int i = 0; i < 3; i++) if (ui_button(w, R(62 + i * 70, 48, 64, 26), top[i])) calc_press(c, top[i]);
    static const char *keys[4][6] = { { "MC", "7", "8", "9", "/", "sqrt" }, { "MR", "4", "5", "6", "*", "%" }, { "MS", "1", "2", "3", "-", "1/x" }, { "M+", "0", "+/-", ".", "+", "=" } };
    for (int r = 0; r < 4; r++) for (int k = 0; k < 6; k++) {
        int x = k == 0 ? 12 : 62 + (k - 1) * 42, y = 84 + r * 34;
        if (ui_button(w, R(x, y, k == 0 ? 36 : 38, 30), keys[r][k])) calc_press(c, keys[r][k]);
    }
}
static void calc_key(window_t *w, key_event_t *e) {
    if (!e->pressed) return;
    calc_t *c = w->data; char k[2] = { e->ch, 0 };
    if (isdigit(e->ch) || strchr("+-*/.%", e->ch)) calc_press(c, k);
    else if (e->key == KEY_ENTER || e->ch == '=') calc_press(c, "=");
    else if (e->key == KEY_BACKSPACE) calc_press(c, "Back");
    else if (e->key == KEY_ESC) calc_press(c, "C");
    else if (e->key == KEY_DELETE) calc_press(c, "CE");
    w->dirty = true;
}
void calc_open(void) {
    window_t *w = wm_create("Calculator", -1, -1, 284, 222, WF_NO_MAX);
    calc_t *c = kzalloc(sizeof *c); strcpy(c->disp, "0"); c->fresh = true;
    w->data = c; w->paint = calc_paint; w->key = calc_key; w->icon = ICON_CALCULATOR;
    w->destroy = free_data;
}

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
    w->menus = kmalloc(sizeof paint_menus); memcpy(w->menus, paint_menus, sizeof paint_menus); w->nmenus = ARRAY_LEN(paint_menus);
    rect_t cr = wm_client_rect(w); wm_resize_client(w, cr.w, cr.h);
    if (path && *path) paint_load(w, path);
}

/* ======================= Image Viewer ======================= */
typedef struct { canvas_t *img; char name[96]; } iv_t;
static void iv_paint(window_t *w) {
    iv_t *v = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    if (!v->img) { draw_text(c, FONT_UI, 20, 20, "Unable to display this picture.", T.text); return; }
    int iw = v->img->w, ih = v->img->h, aw = c->w - 20, ah = c->h - 50;
    int sw = iw, sh = ih;
    if (sw > aw) { sh = sh * aw / sw; sw = aw; } if (sh > ah) { sw = sw * ah / sh; sh = ah; }
    int ox = (c->w - sw) / 2, oy = (c->h - 40 - sh) / 2;
    blend_rect(c, ox + 4, oy + 4, sw, sh, BLACK, 40);
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        uint32_t p = v->img->px[(y * ih / sh) * v->img->stride + x * iw / sw]; int a = p >> 24;
        uint32_t chk = ((x / 8 + y / 8) & 1) ? HEX(0xE0E0E0) : WHITE;      /* checkerboard behind transparency */
        put_pixel(c, ox + x, oy + y, a == 255 ? p : mix(chk, p | 0xFF000000, a));
    }
    if (ui_button(w, R(c->w / 2 - 110, c->h - 34, 100, 26), "Edit in Paint")) { char p[300]; snprintf(p, sizeof p, "%s", v->name); paint_open(p); }
    if (ui_button(w, R(c->w / 2 + 10, c->h - 34, 100, 26), "Close")) wm_destroy(w);
}
static void iv_destroy(window_t *w) { iv_t *v = w->data; canvas_free(v->img); kfree(v); }
void imageview_open(const char *path) {
    if (path && *path && vfs_resolve(path, NULL)) { char full_[256]; vfs_path(vfs_resolve(path, NULL), full_, sizeof full_); recent_add(full_); }   /* for The WRLD Search */
    vnode_t *n = vfs_resolve(path, NULL);
    iv_t *v = kzalloc(sizeof *v);
    if (n) { char err[96]; v->img = image_decode(n->data, n->size, err, sizeof err); vfs_path(n, v->name, sizeof v->name); }
    char t[160]; snprintf(t, sizeof t, "%s - Image Viewer", n ? n->name : "Image");
    window_t *w = wm_create(t, -1, -1, 560, 460, WF_RESIZABLE);
    w->data = v; w->paint = iv_paint; w->destroy = iv_destroy; w->icon = ICON_IMAGE_FILE;
}

/* ======================= Snake ======================= */
#define SN_W 28
#define SN_H 20
#define SN_C 16
typedef struct { int x[SN_W * SN_H], y[SN_W * SN_H]; int len, dx, dy, ndx, ndy, fx, fy, score, best; bool over, paused, started; } snake_t;
static void sn_food(snake_t *s) { for (;;) { s->fx = rand_next() % SN_W; s->fy = rand_next() % SN_H; bool ok = true; for (int i = 0; i < s->len; i++) if (s->x[i] == s->fx && s->y[i] == s->fy) ok = false; if (ok) return; } }
static void sn_reset(snake_t *s) { s->len = 4; for (int i = 0; i < 4; i++) { s->x[i] = 8 - i; s->y[i] = 10; } s->dx = s->ndx = 1; s->dy = s->ndy = 0; s->score = 0; s->over = false; s->paused = false; sn_food(s); }
static void sn_tick(window_t *w) {
    snake_t *s = w->data;
    if (s->over || s->paused || !s->started) return;
    s->dx = s->ndx; s->dy = s->ndy;
    int nx = s->x[0] + s->dx, ny = s->y[0] + s->dy;
    if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) { s->over = true; speaker_beep(220, 120); w->dirty = true; return; }
    for (int i = 0; i < s->len - 1; i++) if (s->x[i] == nx && s->y[i] == ny) { s->over = true; speaker_beep(220, 120); w->dirty = true; return; }
    bool eat = nx == s->fx && ny == s->fy;
    if (eat && s->len < SN_W * SN_H) s->len++;
    for (int i = s->len - 1; i > 0; i--) { s->x[i] = s->x[i - 1]; s->y[i] = s->y[i - 1]; }
    s->x[0] = nx; s->y[0] = ny;
    if (eat) { s->score += 10; if (s->score > s->best) s->best = s->score; sn_food(s); }
    w->timer_ms = MAX(50, 140 - s->score / 5);
    w->dirty = true;
}
static void sn_paint(window_t *w) {
    snake_t *s = w->data; canvas_t *c = w->surf;
    gradient_v(c, 0, 0, c->w, 30, HEX(0x2A5A2A), HEX(0x1A3A1A));
    char b[64]; snprintf(b, sizeof b, "Score: %d", s->score); draw_text(c, FONT_UI_BOLD, 10, 8, b, WHITE);
    snprintf(b, sizeof b, "Best: %d", s->best); draw_text(c, FONT_UI_BOLD, c->w - 90, 8, b, HEX(0xFFE080));
    for (int y = 0; y < SN_H; y++) for (int x = 0; x < SN_W; x++) fill_rect(c, x * SN_C, 30 + y * SN_C, SN_C, SN_C, (x + y) & 1 ? HEX(0xA8D86C) : HEX(0xB4E07A));
    fill_circle(c, s->fx * SN_C + 8, 30 + s->fy * SN_C + 8, 6, HEX(0xE03030)); fill_circle(c, s->fx * SN_C + 6, 30 + s->fy * SN_C + 6, 2, HEX(0xFF9090));
    for (int i = s->len - 1; i >= 0; i--) {
        uint32_t col = mix(HEX(0x2060D0), HEX(0x80B0FF), i * 255 / MAX(1, s->len));
        fill_round_rect(c, s->x[i] * SN_C + 1, 30 + s->y[i] * SN_C + 1, SN_C - 2, SN_C - 2, 4, col);
    }
    fill_circle(c, s->x[0] * SN_C + 5, 30 + s->y[0] * SN_C + 6, 2, WHITE); fill_circle(c, s->x[0] * SN_C + 11, 30 + s->y[0] * SN_C + 6, 2, WHITE);
    const char *msg = !s->started ? "Press an arrow key to start" : s->over ? "Game over - press Space to play again" : s->paused ? "Paused - press P" : NULL;
    if (msg) { int tw = text_width(FONT_LARGE_BOLD, msg); blend_rect(c, (c->w - tw) / 2 - 14, c->h / 2 - 20, tw + 28, 40, BLACK, 140); draw_text(c, FONT_LARGE_BOLD, (c->w - tw) / 2, c->h / 2 - 11, msg, WHITE); }
}
static void sn_key(window_t *w, key_event_t *e) {
    snake_t *s = w->data; if (!e->pressed) return;
    int k = e->key;
    if (k == KEY_UP && s->dy != 1) { s->ndx = 0; s->ndy = -1; s->started = true; }
    else if (k == KEY_DOWN && s->dy != -1) { s->ndx = 0; s->ndy = 1; s->started = true; }
    else if (k == KEY_LEFT && s->dx != 1 && s->started) { s->ndx = -1; s->ndy = 0; }
    else if (k == KEY_RIGHT && s->dx != -1) { s->ndx = 1; s->ndy = 0; s->started = true; }
    else if ((k == ' ' || k == KEY_ENTER) && s->over) { sn_reset(s); s->started = true; }
    else if (k == 'p' || k == 'P') s->paused = !s->paused;
    w->dirty = true;
}
void snake_open(void) {
    window_t *w = wm_create("Snake", -1, -1, SN_W * SN_C, SN_H * SN_C + 30, WF_NO_MAX);
    snake_t *s = kzalloc(sizeof *s); sn_reset(s);
    w->data = s; w->paint = sn_paint; w->key = sn_key; w->icon = ICON_GAME; w->destroy = free_data;
    wm_set_timer(w, 140, sn_tick);
}

/* ======================= Minesweeper ======================= */
#define MS_W 9
#define MS_H 9
#define MS_MINES 10
#define MS_CELL 26
typedef struct { uint8_t mine[MS_W * MS_H], open[MS_W * MS_H], flag[MS_W * MS_H]; int lost, won, flags, opened; } ms_t;
static void ms_reset(ms_t *m) {
    memset(m, 0, sizeof *m);
    for (int placed = 0; placed < MS_MINES;) {
        int i = rand_next() % (MS_W * MS_H);
        if (!m->mine[i]) { m->mine[i] = 1; placed++; }
    }
}
static int ms_around(ms_t *m, int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int nx = x + dx, ny = y + dy;
        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < MS_W && ny < MS_H) n += m->mine[ny * MS_W + nx];
    }
    return n;
}
static void ms_reveal(ms_t *m, int x, int y) {
    if (x < 0 || y < 0 || x >= MS_W || y >= MS_H) return;
    int i = y * MS_W + x;
    if (m->open[i] || m->flag[i]) return;
    m->open[i] = 1; m->opened++;
    if (m->mine[i]) { m->lost = 1; return; }
    if (!ms_around(m, x, y)) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) ms_reveal(m, x + dx, y + dy);
}
static void ms_paint(window_t *w) {
    ms_t *m = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    fill_rect(c, 0, 0, c->w, 30, T.pane_head_bot);
    char b[96]; snprintf(b, sizeof b, "Mines: %d    Flags: %d", MS_MINES, m->flags);
    draw_text(c, FONT_UI_BOLD, 8, 8, b, T.pane_head_text);
    draw_text(c, FONT_UI_BOLD, c->w - 80, 8, m->won ? "You win!" : m->lost ? "Boom!" : "R = reset", T.pane_head_text);
    for (int y = 0; y < MS_H; y++) for (int x = 0; x < MS_W; x++) {
        int i = y * MS_W + x, cx = 8 + x * MS_CELL, cy = 38 + y * MS_CELL;
        if (m->open[i]) {
            fill_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, m->mine[i] ? HEX(0xE05050) : T.window_bg);
            draw_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, T.face_dark);
            if (m->mine[i]) { fill_circle(c, cx + 12, cy + 12, 7, BLACK); fill_circle(c, cx + 9, cy + 9, 2, WHITE); }
            else {
                int n = ms_around(m, x, y);
                if (n) { char s[2] = { (char)('0' + n), 0 }; uint32_t col = n == 1 ? HEX(0x0000C0) : n == 2 ? HEX(0x008000) : n == 3 ? HEX(0xC00000) : n == 4 ? HEX(0x000080) : HEX(0x800080); if (dark_mode) col = lighten(col, 140); draw_text(c, FONT_UI_BOLD, cx + 8, cy + 6, s, col); }
            }
        } else {
            fill_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, T.face_light);
            draw_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, T.face_dark);
            if (m->flag[i]) draw_text(c, FONT_UI_BOLD, cx + 6, cy + 6, "F", dark_mode ? HEX(0xFF9080) : HEX(0xC02020));
        }
    }
    if (m->lost || m->won) {
        const char *msg = m->won ? "You cleared the field - Space to play again" : "Mine hit - Space to play again";
        int tw = text_width(FONT_UI_BOLD, msg);
        blend_rect(c, (c->w - tw) / 2 - 10, c->h / 2 - 16, tw + 20, 32, BLACK, 150);
        draw_text(c, FONT_UI_BOLD, (c->w - tw) / 2, c->h / 2 - 7, msg, WHITE);
    }
}
static void ms_mouse(window_t *w, int type, int x, int y, int btn) {
    ms_t *m = w->data;
    if (type != ME_DOWN || m->lost || m->won) return;
    int gx = (x - 8) / MS_CELL, gy = (y - 38) / MS_CELL;
    if (gx < 0 || gy < 0 || gx >= MS_W || gy >= MS_H) return;
    int i = gy * MS_W + gx;
    if (btn & MB_RIGHT) { if (!m->open[i]) { m->flag[i] = !m->flag[i]; m->flags += m->flag[i] ? 1 : -1; } }
    else if (btn & MB_LEFT) { if (m->flag[i]) return; ms_reveal(m, gx, gy); if (m->opened == MS_W * MS_H - MS_MINES) m->won = 1; }
    w->dirty = true;
}
static void ms_key(window_t *w, key_event_t *e) {
    ms_t *m = w->data;
    if (e->pressed && (e->key == 'r' || e->key == 'R' || e->key == ' ')) { ms_reset(m); w->dirty = true; }
}
void minesweeper_open(void) {
    window_t *w = wm_create("Minesweeper", -1, -1, 8 + MS_W * MS_CELL + 8, 38 + MS_H * MS_CELL + 10, WF_NO_MAX);
    ms_t *m = kzalloc(sizeof *m); ms_reset(m);
    w->data = m; w->paint = ms_paint; w->mouse = ms_mouse; w->key = ms_key; w->icon = ICON_GAME; w->destroy = free_data;
}

/* ======================= Pong ======================= */
#define PG_W 480
#define PG_H 300
typedef struct { int py, cy, bx, by, dx, dy, score, cpu; } pong_t;
static void pg_reset(pong_t *p, int dir) {
    p->bx = PG_W / 2; p->by = PG_H / 2; p->dx = 4 * dir; p->dy = (rand_next() & 1) ? 3 : -3;
}
static void pg_tick(window_t *w) {
    pong_t *p = w->data;
    if (p->score >= 10 || p->cpu >= 10) return;
    p->cy += (p->by > p->cy + 4) ? 3 : (p->by < p->cy - 4) ? -3 : 0;
    p->cy = CLAMP(p->cy, 16, PG_H - 16);
    p->bx += p->dx; p->by += p->dy;
    if (p->by <= 4 || p->by >= PG_H - 4) { p->dy = -p->dy; p->by = CLAMP(p->by, 4, PG_H - 4); }
    if (p->dx < 0 && p->bx <= 18 && p->by >= p->py && p->by <= p->py + 56) { p->dx = 4; p->bx = 19; }
    if (p->dx > 0 && p->bx >= PG_W - 18 && p->by >= p->cy && p->by <= p->cy + 56) { p->dx = -4; p->bx = PG_W - 19; }
    if (p->bx < 0) { p->cpu++; pg_reset(p, 1); }
    else if (p->bx > PG_W) { p->score++; pg_reset(p, -1); }
    w->dirty = true;
}
static void pg_paint(window_t *w) {
    pong_t *p = w->data; canvas_t *c = w->surf;
    gfx_clear(c, HEX(0x101820));
    for (int y = 0; y < c->h; y += 20) fill_rect(c, c->w / 2 - 1, y, 2, 10, HEX(0x405060));
    fill_rect(c, 8, p->py, 8, 56, T.check);
    fill_rect(c, c->w - 16, p->cy, 8, 56, HEX(0xE08040));
    fill_circle(c, p->bx, p->by, 5, WHITE);
    char b[32]; snprintf(b, sizeof b, "%d", p->score); draw_text(c, FONT_LARGE_BOLD, c->w / 2 - 40, 8, b, WHITE);
    snprintf(b, sizeof b, "%d", p->cpu); draw_text(c, FONT_LARGE_BOLD, c->w / 2 + 24, 8, b, WHITE);
    if (p->score >= 10 || p->cpu >= 10) {
        const char *m = p->score >= 10 ? "You win - Space for a new game" : "Computer wins - Space for a new game";
        int tw = text_width(FONT_UI_BOLD, m);
        blend_rect(c, (c->w - tw) / 2 - 12, c->h / 2 - 18, tw + 24, 36, BLACK, 160);
        draw_text(c, FONT_UI_BOLD, (c->w - tw) / 2, c->h / 2 - 8, m, WHITE);
    }
}
static void pg_key(window_t *w, key_event_t *e) {
    pong_t *p = w->data; if (!e->pressed) return;
    if (e->key == KEY_UP || e->key == 'w') p->py -= 18;
    if (e->key == KEY_DOWN || e->key == 's') p->py += 18;
    p->py = CLAMP(p->py, 4, PG_H - 60);
    if (e->key == ' ' && (p->score >= 10 || p->cpu >= 10)) { p->score = p->cpu = 0; p->py = PG_H / 2 - 28; pg_reset(p, 1); }
    w->dirty = true;
}
void pong_open(void) {
    window_t *w = wm_create("Pong", -1, -1, PG_W, PG_H, WF_NO_MAX);
    pong_t *p = kzalloc(sizeof *p); p->py = PG_H / 2 - 28; p->cy = PG_H / 2 - 28; pg_reset(p, 1);
    w->data = p; w->paint = pg_paint; w->key = pg_key; w->icon = ICON_GAME; w->destroy = free_data;
    wm_set_timer(w, 16, pg_tick);
}

/* ======================= About ======================= */
static void about_paint(window_t *w) {
    canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    gradient_h(c, 0, 0, c->w, 78, HEX(0x0A3CA8), HEX(0x5A8EF0));
    draw_globe(c, 10, 7, 64, (int)(timer_ticks() / 40));
    draw_wordmark(c, 84, 6, WORDMARK_MEDIUM, WHITE);
    draw_text(c, FONT_UI, 86, 58, OS_EDITION, HEX(0xE0E8FF));
    hline(c, 0, 78, c->w, HEX(0xF09A3C));
    int y = 92;
    const char *lines[] = { "Version " OS_VERSION " (Build " OS_BUILD ")", OS_KERNEL_DESC,
        "Drivers: PS/2 keyboard & mouse, RTC, PCI, VBE framebuffer, PC speaker", "Win32 subsystem for " OS_EXE_DESC " .exe programs", "POSIX-style shell for .sh scripts",
        "UDIF .dmg disk images with FAT volumes and .app bundles", "", "Fonts: Liberation family (SIL Open Font License).", "Icons and wallpapers are original procedural artwork." };
    for (unsigned i = 0; i < ARRAY_LEN(lines); i++) { draw_text(c, FONT_UI, 20, y, lines[i], T.text); y += 17; }
    char b[96]; snprintf(b, sizeof b, "Physical memory available to " OS_NAME ": %u KB", mm_phys_total() / 1024);
    draw_text(c, FONT_UI, 20, y + 6, b, T.gray_text);
    snprintf(b, sizeof b, "This product is licensed to: %s", user_name);
    draw_text(c, FONT_UI, 20, y + 24, b, T.gray_text);
    char st[120]; persist_status(st, sizeof st);
    snprintf(b, sizeof b, "%s: %s", system_volume ? "Installed on" : "Running as", st);
    draw_text_ellipsis(c, FONT_UI, 20, y + 42, c->w - 40, b, T.gray_text);
    if (ui_button_ex(w, R(c->w - 90, c->h - 34, 76, 24), "OK", true, true)) wm_destroy(w);
}
static void about_tick(window_t *w) { w->dirty = true; }
void about_open(void) {
    window_t *w = wm_create("About " OS_NAME, -1, -1, 470, 352, WF_NO_MAX | WF_NO_MIN);
    w->paint = about_paint; w->icon = ICON_ABOUT;
    wm_set_timer(w, 50, about_tick);
    wm_center(w);
}

/* ======================= Properties ======================= */
typedef struct { vnode_t *n; bool ro; char info[200]; } prop_t;
static void prop_paint(window_t *w) {
    prop_t *p = w->data; canvas_t *c = w->surf; vnode_t *n = p->n;
    gfx_clear(c, T.face);
    ui_tab(w, R(6, 6, 70, 22), "General", true);
    rect_t body = R(6, 28, c->w - 12, c->h - 72);
    fill_rect(c, body.x, body.y, body.w, body.h, T.classic ? T.face : T.window_bg);
    draw_rect(c, body.x, body.y, body.w, body.h, T.face_dark);
    draw_icon(c, file_icon(n), 32, 22, 42);
    draw_sunken_box(c, R(80, 46, body.w - 90, 22), T.window_bg);
    draw_text_ellipsis(c, FONT_UI, 86, 50, body.w - 100, !n->parent ? vfs_drive_label(n->drive) : n->name, T.text);
    hline(c, 16, 84, body.w - 20, T.face_dark);
    char path[256], b[160];
    int y = 96;
    #define ROW(label, val) do { draw_text(c, FONT_UI, 20, y, label, T.text); draw_text_wrapped(c, FONT_UI, 110, y, body.w - 110, val, T.text, 1); y += 20; } while (0)
    ROW("Type:", file_type_name(n));
    if (n->parent) { vfs_path(n->parent, path, sizeof path); ROW("Location:", path); }
    uint32_t sz = vfs_tree_size(n); format_size(b, sizeof b, sz);
    char b2[200]; snprintf(b2, sizeof b2, "%s (%u bytes)", b, sz); ROW("Size:", b2);
    if (n->dir) { snprintf(b, sizeof b, "%d items", vfs_count_children(n)); ROW("Contains:", b); }
    hline(c, 16, y + 2, body.w - 20, T.face_dark); y += 10;
    snprintf(b, sizeof b, "%02d/%02d/%04d %02d:%02d:%02d", n->mtime.month, n->mtime.day, n->mtime.year, n->mtime.hour, n->mtime.minute, n->mtime.second); ROW("Modified:", b);
    if (p->info[0]) { hline(c, 16, y + 2, body.w - 20, T.face_dark); y += 10; draw_text(c, FONT_UI, 20, y, "Details:", T.text); y += draw_text_wrapped(c, FONT_UI, 110, y, body.w - 110, p->info, T.text, 1) + 6; }
    hline(c, 16, y + 2, body.w - 20, T.face_dark); y += 12;
    draw_text(c, FONT_UI, 20, y, "Attributes:", T.text);
    bool was = p->ro;
    ui_checkbox(w, 110, y, "Read-only", &p->ro);
    if (was != p->ro && (!n->parent || !n->parent->readonly)) n->readonly = p->ro;
    #undef ROW
    if (ui_button_ex(w, R(c->w - 168, c->h - 36, 76, 24), "OK", true, true)) { wm_destroy(w); return; }
    if (ui_button(w, R(c->w - 86, c->h - 36, 76, 24), "Cancel")) wm_destroy(w);
}
void properties_open(vnode_t *n) {
    if (!n) return;
    prop_t *p = kzalloc(sizeof *p); p->n = n; p->ro = n->readonly;
    const char *e = vfs_ext(n->name);
    if (!n->dir && !strcasecmp(e, "exe")) pe_probe(n, p->info, sizeof p->info);
    else if (!n->dir && !strcasecmp(e, "dmg")) { uint32_t sz; uint8_t *d = dmg_extract(n->data, n->size, &sz, p->info, sizeof p->info); kfree(d); }
    else if (!n->dir) macho_probe(n->data, n->size, p->info, sizeof p->info);
    if (!n->parent) snprintf(p->info, sizeof p->info, "%s volume %c:", vfs_drive_kind(n->drive) == 2 ? "Read-only disk image" : "RAM-backed", n->drive);
    char t[160]; snprintf(t, sizeof t, "%s Properties", n->parent ? n->name : vfs_drive_label(n->drive));
    window_t *w = wm_create(t, -1, -1, 360, 420, WF_NO_MAX | WF_NO_MIN);
    w->data = p; w->paint = prop_paint; w->icon = file_icon(n);
    w->destroy = free_data;
    wm_center(w);
}
