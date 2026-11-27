/* Pong */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

static void free_data(window_t *w) { kfree(w->data); }

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
