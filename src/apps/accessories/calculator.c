/* Calculator */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

static void free_data(window_t *w) { kfree(w->data); }

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
