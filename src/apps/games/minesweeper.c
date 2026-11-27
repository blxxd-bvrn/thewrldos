/* Minesweeper */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

static void free_data(window_t *w) { kfree(w->data); }

/* ======================= Minesweeper =======================
 * Beginner 9x9 with 10 mines, Intermediate 16x16 with 40, Expert 30x16 with 99 (Game menu).
 * The first click is never a mine (the mines are laid after it, away from it); a click on a
 * number whose flags are all placed opens the cells around it. A clock runs from the first
 * click and each level keeps its best time. Keyboard: the arrows move a cursor, Space opens,
 * F flags, Enter opens around a number, F2 starts again. */
#define MS_MAXW 30
#define MS_MAXH 16
#define MS_CELL 24
static const struct { int w, h, mines; const char *name, *key; } ms_levels[3] = {
    { 9, 9, 10, "Beginner", "mines_best0" }, { 16, 16, 40, "Intermediate", "mines_best1" }, { 30, 16, 99, "Expert", "mines_best2" } };
typedef struct {
    uint8_t mine[MS_MAXW * MS_MAXH], open[MS_MAXW * MS_MAXH], flag[MS_MAXW * MS_MAXH];
    int lost, won, flags, opened, level, W, H, M; bool laid; uint32_t t0, t1; int cx, cy; bool kbd; int best;
} ms_t;
static menu_t ms_menus[] = { { "Game", { { "New    F2", 1, 0 }, { "-", 0, 0 }, { "Beginner", 10, MI_RADIO }, { "Intermediate", 11, MI_RADIO }, { "Expert", 12, MI_RADIO }, { "-", 0, 0 }, { "Exit", 2, 0 }, { NULL } } } };
static void ms_reset(ms_t *m) {
    int lv = m->level;
    memset(m, 0, sizeof *m);
    m->level = lv; m->W = ms_levels[lv].w; m->H = ms_levels[lv].h; m->M = ms_levels[lv].mines;
    const char *v = settings_get(ms_levels[lv].key); m->best = v ? atoi(v) : 0;
    m->cx = m->W / 2; m->cy = m->H / 2;
}
static void ms_lay(ms_t *m, int sx, int sy) {        /* after the first click: none at or around it */
    for (int placed = 0; placed < m->M;) {
        int i = rand_next() % (m->W * m->H), x = i % m->W, y = i / m->W;
        if (m->mine[i] || (ABSDIFF(x, sx) <= 1 && ABSDIFF(y, sy) <= 1)) continue;
        m->mine[i] = 1; placed++;
    }
    m->laid = true; m->t0 = timer_ticks();
}
static int ms_around(ms_t *m, int x, int y, uint8_t *what) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int nx = x + dx, ny = y + dy;
        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < m->W && ny < m->H) n += what[ny * m->W + nx];
    }
    return n;
}
static void ms_reveal(ms_t *m, int x, int y) {
    if (x < 0 || y < 0 || x >= m->W || y >= m->H) return;
    int i = y * m->W + x;
    if (m->open[i] || m->flag[i]) return;
    m->open[i] = 1; m->opened++;
    if (m->mine[i]) { m->lost = 1; m->t1 = timer_ticks(); return; }
    if (!ms_around(m, x, y, m->mine)) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) ms_reveal(m, x + dx, y + dy);
}
static void ms_check_won(ms_t *m) {
    if (m->lost || m->opened != m->W * m->H - m->M) return;
    m->won = 1; m->t1 = timer_ticks();
    int secs = (m->t1 - m->t0) / 1000;
    if (!m->best || secs < m->best) { m->best = secs; char b[16]; snprintf(b, sizeof b, "%d", secs); settings_set(ms_levels[m->level].key, b); settings_save(); }
}
static void ms_open_at(ms_t *m, int x, int y) {
    if (!m->laid) ms_lay(m, x, y);
    int i = y * m->W + x;
    if (m->open[i] && ms_around(m, x, y, m->mine) && ms_around(m, x, y, m->flag) == ms_around(m, x, y, m->mine)) {
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) ms_reveal(m, x + dx, y + dy);   /* chord */
    } else ms_reveal(m, x, y);
    ms_check_won(m);
    if (m->lost) speaker_beep(180, 150);
}
static void ms_flag_at(ms_t *m, int x, int y) { int i = y * m->W + x; if (!m->open[i]) { m->flag[i] = !m->flag[i]; m->flags += m->flag[i] ? 1 : -1; } }
static void ms_paint(window_t *w) {
    ms_t *m = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    fill_rect(c, 0, 0, c->w, 30, T.pane_head_bot);
    uint32_t secs = m->laid ? ((m->t1 ? m->t1 : timer_ticks()) - m->t0) / 1000 : 0;
    char b[96]; snprintf(b, sizeof b, "Mines left: %d", m->M - m->flags);
    draw_text(c, FONT_UI_BOLD, 8, 8, b, T.pane_head_text);
    snprintf(b, sizeof b, "Time: %u   Best: %d", secs, m->best);
    draw_text(c, FONT_UI_BOLD, c->w - 8 - text_width(FONT_UI_BOLD, b), 8, b, T.pane_head_text);
    for (int y = 0; y < m->H; y++) for (int x = 0; x < m->W; x++) {
        int i = y * m->W + x, cx = 8 + x * MS_CELL, cy = 38 + y * MS_CELL;
        if (m->open[i] || (m->lost && m->mine[i])) {
            fill_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, m->mine[i] ? (m->open[i] ? HEX(0xE05050) : HEX(0xD8C0C0)) : T.window_bg);
            draw_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, T.face_dark);
            if (m->mine[i]) { fill_circle(c, cx + 11, cy + 11, 6, BLACK); fill_circle(c, cx + 9, cy + 9, 2, WHITE); }
            else {
                int n = ms_around(m, x, y, m->mine);
                if (n) { char s2[2] = { (char)('0' + n), 0 }; uint32_t col = n == 1 ? HEX(0x0000C0) : n == 2 ? HEX(0x008000) : n == 3 ? HEX(0xC00000) : n == 4 ? HEX(0x000080) : HEX(0x800080); if (dark_mode) col = lighten(col, 140); draw_text(c, FONT_UI_BOLD, cx + 8, cy + 5, s2, col); }
            }
        } else {
            gradient_v(c, cx, cy, MS_CELL - 1, MS_CELL - 1, T.face_light, T.face);
            draw_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, T.face_dark);
            if (m->flag[i]) { vline(c, cx + 9, cy + 5, 13, BLACK); fill_rect(c, cx + 10, cy + 5, 7, 6, HEX(0xD02020)); hline(c, cx + 6, cy + 18, 8, BLACK); }
        }
        if (m->kbd && x == m->cx && y == m->cy) { draw_rect(c, cx - 1, cy - 1, MS_CELL + 1, MS_CELL + 1, HEX(0xF0A030)); draw_rect(c, cx, cy, MS_CELL - 1, MS_CELL - 1, HEX(0xF0A030)); }
    }
    if (m->lost || m->won) {
        const char *msg = m->won ? "You cleared the field - F2 to play again" : "Mine hit - F2 to play again";
        int tw = text_width(FONT_UI_BOLD, msg);
        blend_rect(c, (c->w - tw) / 2 - 10, c->h / 2 - 16, tw + 20, 32, BLACK, 150);
        draw_text(c, FONT_UI_BOLD, (c->w - tw) / 2, c->h / 2 - 7, msg, WHITE);
    }
}
static void ms_mouse(window_t *w, int type, int x, int y, int btn) {
    ms_t *m = w->data;
    if ((type != ME_DOWN && type != ME_DBLCLICK) || m->lost || m->won) return;
    int gx = (x - 8) / MS_CELL, gy = (y - 38) / MS_CELL;
    if (x < 8 || y < 38 || gx >= m->W || gy >= m->H) return;
    m->kbd = false;
    if (btn & (MB_RIGHT)) ms_flag_at(m, gx, gy);
    else if (btn & (MB_LEFT | MB_MIDDLE)) { if (m->flag[gy * m->W + gx]) return; ms_open_at(m, gx, gy); }
    w->dirty = true;
}
static void ms_size(window_t *w, ms_t *m) { wm_resize_client(w, 16 + m->W * MS_CELL, 38 + m->H * MS_CELL + 8); }
static void ms_menu(window_t *w, int id) {
    ms_t *m = w->data;
    if (id == 1) ms_reset(m);
    else if (id == 2) { wm_close(w); return; }
    else if (id >= 10 && id <= 12) {
        m->level = id - 10; ms_reset(m); ms_size(w, m);
        char b[4]; snprintf(b, sizeof b, "%d", m->level); settings_set("mines_level", b); settings_save();
        for (int k = 0; k < 3; k++) ms_menus[0].items[2 + k].flags = MI_RADIO | (k == m->level ? MI_CHECKED : 0);
    }
    w->dirty = true;
}
static void ms_key(window_t *w, key_event_t *e) {
    ms_t *m = w->data;
    if (!e->pressed) return;
    int k = e->key;
    if (k == KEY_F2 || k == 'r' || k == 'R') { ms_reset(m); w->dirty = true; return; }
    if (m->lost || m->won) { if (k == ' ') { ms_reset(m); w->dirty = true; } return; }
    m->kbd = true;
    if (k == KEY_LEFT) m->cx = (m->cx + m->W - 1) % m->W;
    else if (k == KEY_RIGHT) m->cx = (m->cx + 1) % m->W;
    else if (k == KEY_UP) m->cy = (m->cy + m->H - 1) % m->H;
    else if (k == KEY_DOWN) m->cy = (m->cy + 1) % m->H;
    else if (k == 'f' || k == 'F') ms_flag_at(m, m->cx, m->cy);
    else if ((k == ' ' || k == KEY_ENTER) && !m->flag[m->cy * m->W + m->cx]) ms_open_at(m, m->cx, m->cy);
    w->dirty = true;
}
static void ms_tick(window_t *w) { ms_t *m = w->data; if (m->laid && !m->won && !m->lost) w->dirty = true; }
void minesweeper_open(void) {
    ms_t *m = kzalloc(sizeof *m);
    const char *v = settings_get("mines_level"); m->level = v ? CLAMP(atoi(v), 0, 2) : 0;
    ms_reset(m);
    for (int k = 0; k < 3; k++) ms_menus[0].items[2 + k].flags = MI_RADIO | (k == m->level ? MI_CHECKED : 0);
    window_t *w = wm_create("Minesweeper", -1, -1, 16 + m->W * MS_CELL, 38 + m->H * MS_CELL + 8, WF_NO_MAX);
    w->data = m; w->paint = ms_paint; w->mouse = ms_mouse; w->key = ms_key; w->icon = ICON_GAME; w->destroy = free_data;
    w->menus = ms_menus; w->nmenus = ARRAY_LEN(ms_menus); w->menu_cmd = ms_menu;
    wm_set_timer(w, 1000, ms_tick);
}
