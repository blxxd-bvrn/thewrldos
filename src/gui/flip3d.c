/* Flip 3D: Alt+Tab with 3D acceleration on shows the open windows as a stack
 * of tilted cards in perspective, drawn by the WRLD 3D engine (r3d.c).
 *
 * Tab / Shift+Tab, the arrow keys or the mouse wheel move through the stack;
 * releasing Alt, Enter or a click picks the front window, Esc goes back to the
 * window that was active. Ctrl+Alt+Tab keeps the stack open after Alt is let go. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "r3d.h"

#define FLIP_MAX 16
#define TEX_MAX_W 560                       /* snapshots are scaled down to at most this width */

void wm_paint_window_to(canvas_t *c, window_t *w, int x, int y);   /* wm.c */
void blit_scaled_alpha(canvas_t *dst, rect_t r, canvas_t *src, int alpha);   /* composite.c */

static struct {
    bool active, sticky;
    window_t *w[FLIP_MAX]; int id[FLIP_MAX];
    canvas_t *tex[FLIP_MAX];
    int n, sel;
    float pos;                              /* the stack's position, eased towards sel */
    uint32_t start, last;
    window_t *was_active;
    canvas_t *backdrop;
} F;

bool flip3d_active(void) { return F.active; }
bool flip3d_animating(void) {
    if (!F.active) return false;
    float d = F.sel - F.pos;
    return d > 0.002f || d < -0.002f || timer_ticks() - F.start < 300;
}

static canvas_t *snapshot(window_t *w) {
    if (w->w <= 0 || w->h <= 0) return NULL;
    canvas_t *full = canvas_new(w->w, w->h);
    if (!full) return NULL;
    gfx_clear(full, T.face);
    wm_paint_window_to(full, w, 0, 0);
    if (w->w <= TEX_MAX_W) return full;
    int tw = TEX_MAX_W, th = MAX(1, w->h * TEX_MAX_W / w->w);
    canvas_t *small = canvas_new(tw, th);
    if (!small) return full;
    gfx_clear(small, 0);
    blit_scaled_alpha(small, R(0, 0, tw, th), full, 255);
    canvas_free(full);
    return small;
}
static void release_all(void) {
    for (int i = 0; i < F.n; i++) if (F.tex[i]) canvas_free(F.tex[i]);
    if (F.backdrop) canvas_free(F.backdrop);
    memset(&F, 0, sizeof F);
}

/* windows from the top of the stacking order down; `list` is wm.c's array, bottom first */
bool flip3d_start(window_t **list, int count, window_t *active, bool sticky) {
    if (!accel3d || F.active) return false;
    memset(&F, 0, sizeof F);
    for (int i = count - 1; i >= 0 && F.n < FLIP_MAX; i--) {
        window_t *w = list[i];
        if ((w->flags & (WF_HIDDEN | WF_POPUP | WF_DIALOG)) || w->w < 40 || w->h < 30) continue;
        F.w[F.n] = w; F.id[F.n] = w->id; F.tex[F.n] = snapshot(w);
        if (!F.tex[F.n]) continue;
        F.n++;
    }
    if (F.n < 1) { release_all(); return false; }
    /* the desktop behind, dimmed once */
    F.backdrop = canvas_new(SCREEN_W, SCREEN_H);
    if (F.backdrop) {
        desktop_paint(F.backdrop);
        blend_rect(F.backdrop, 0, 0, SCREEN_W, SCREEN_H, HEX(0x000814), 150);
    }
    F.active = true; F.sticky = sticky;
    F.was_active = active;
    F.sel = F.n > 1 ? 1 : 0;
    F.pos = 0;
    F.start = F.last = timer_ticks();
    wm_need_redraw = true;
    return true;
}
static void finish(bool pick) {
    window_t *w = pick && F.sel < F.n ? F.w[F.sel] : F.was_active;
    int id = pick && F.sel < F.n ? F.id[F.sel] : (F.was_active ? F.was_active->id : 0);
    release_all();
    if (w && wm_alive(w) && w->id == id) wm_focus(w);
    wm_need_redraw = true;
}
void flip3d_key(key_event_t *e) {
    if (!F.active) return;
    if (!e->pressed) {
        if ((e->key == KEY_LALT) && !F.sticky) finish(true);
        return;
    }
    switch (e->key) {
    case KEY_TAB: F.sel = (e->mods & MOD_SHIFT) ? (F.sel + F.n - 1) % F.n : (F.sel + 1) % F.n; break;
    case KEY_RIGHT: case KEY_DOWN: F.sel = (F.sel + 1) % F.n; break;
    case KEY_LEFT: case KEY_UP: F.sel = (F.sel + F.n - 1) % F.n; break;
    case KEY_ENTER: case ' ': finish(true); return;
    case KEY_ESC: finish(false); return;
    default: break;
    }
    wm_need_redraw = true;
}
void flip3d_mouse(int x, int y, int pressed, int wheel) {
    (void)x; (void)y;
    if (!F.active) return;
    if (wheel) { F.sel = ((F.sel + (wheel > 0 ? 1 : -1)) % F.n + F.n) % F.n; wm_need_redraw = true; }
    if (pressed & MB_LEFT) finish(true);
    else if (pressed & MB_RIGHT) finish(false);
}

/* draws the whole screen; false if 3D could not be used (the caller composes normally) */
bool flip3d_paint(canvas_t *screen) {
    if (!F.active) return false;
    for (int i = 0; i < F.n; i++) if (!wm_alive(F.w[i]) || F.w[i]->id != F.id[i]) { finish(false); return false; }
    uint32_t now = timer_ticks();
    float dt = (now - F.last) / 1000.0f; F.last = now;
    if (dt > 0.1f) dt = 0.1f;
    /* ease towards the selected card; wrap around the shorter way at the ends */
    float target = F.sel;
    if (target - F.pos > F.n / 2.0f) F.pos += F.n;
    if (F.pos - target > F.n / 2.0f) F.pos -= F.n;
    float k = dt * 10; if (k > 1) k = 1;
    F.pos += (target - F.pos) * k;
    float intro = (now - F.start) / 300.0f; if (intro > 1) intro = 1;
    float ease = intro * intro * (3 - 2 * intro);

    if (F.backdrop) blit(screen, 0, 0, F.backdrop, 0, 0, SCREEN_W, SCREEN_H);
    else gfx_clear(screen, HEX(0x0A1428));
    r3d_t r;
    int taskbar = 30;
    if (!r3d_begin(&r, screen, R(0, 0, SCREEN_W, SCREEN_H - taskbar))) return false;
    r.lighting = false; r.cull_back = false;
    r.proj = m4_perspective(0.9f, (float)SCREEN_W / (SCREEN_H - taskbar), 0.1f, 100);
    /* largest window sets the card scale */
    int maxw = 1, maxh = 1;
    for (int i = 0; i < F.n; i++) { maxw = MAX(maxw, F.w[i]->w); maxh = MAX(maxh, F.w[i]->h); }
    float unit = 3.3f / MAX(maxw, maxh * 4 / 3);
    /* cards from the back to the front */
    for (int pass = F.n - 1; pass >= 0; pass--) {
        /* the card `pass` places behind the front, counted from the selection */
        int i = ((int)(F.pos + 0.5f) + pass) % F.n;
        float d = i - F.pos;
        while (d < -0.999f) d += F.n;
        while (d > F.n - 1.001f) d -= F.n;
        float alpha = 1;
        if (d < 0) alpha = 1 + d;                              /* the card leaving the front fades */
        if (alpha <= 0.01f) continue;
        float cw = F.w[i]->w * unit, ch = F.w[i]->h * unit;
        float tx = -0.55f + d * 0.95f, ty = 0.05f + d * 0.42f, tz = -4.5f - d * 2.6f;
        tz -= (1 - ease) * 3.0f;                               /* the stack slides in */
        r.model = m4_mul(m4_translate(tx, ty, tz), m4_mul(m4_rotate_y(-0.78f), m4_scale(cw, ch, 1)));
        r.texture = F.tex[i];
        r.bilinear = d < 1.5f;                                 /* only the cards in front are filtered */
        r.alpha = (int)(255 * alpha * (0.35f + 0.65f * ease));
        uint32_t tint = d < 0.5f ? 0xFFFFFFFFu : 0xFFE6ECF6u;
        r3d_vertex_t q[4] = {
            { -0.5f, -0.5f, 0, 0, 1, 0, 0, 1, tint }, { 0.5f, -0.5f, 0, 1, 1, 0, 0, 1, tint },
            { 0.5f, 0.5f, 0, 1, 0, 0, 0, 1, tint }, { -0.5f, 0.5f, 0, 0, 0, 0, 0, 1, tint } };
        r3d_quad(&r, q);
    }
    /* the selected window's name */
    window_t *s = F.w[F.sel];
    int tw = text_width(FONT_LARGE_BOLD, s->title) + 48, bx = (SCREEN_W - tw) / 2, by = SCREEN_H - taskbar - 64;
    fill_round_rect_blend(screen, bx, by, tw, 40, 10, 0xFF000000, 150);
    draw_icon(screen, s->icon, 32, bx + 8, by + 4);
    draw_text(screen, FONT_LARGE_BOLD, bx + 44, by + 10, s->title, WHITE);
    return true;
}
