/* Snake */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

static void free_data(window_t *w) { kfree(w->data); }

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
    if (eat) { s->score += 10; if (s->score > s->best) { s->best = s->score; char b[16]; snprintf(b, sizeof b, "%d", s->best); settings_set("snake_best", b); } sn_food(s); }
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
    { const char *v = settings_get("snake_best"); s->best = v ? atoi(v) : 0; }
    w->data = s; w->paint = sn_paint; w->key = sn_key; w->icon = ICON_GAME; w->destroy = free_data;
    wm_set_timer(w, 140, sn_tick);
}
