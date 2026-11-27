/* Orbit Pinball: an original table (a planet with three moons for bumpers, two slingshots, a
 * bank of drop targets, three rollover lanes at the top, a plunger lane on the right).
 *
 * Keys: Z or Left Shift or Left arrow - left flipper; / or Right Shift or Right arrow - right
 * flipper; hold Space (or Down) and let go to launch; F2 - new game; P - pause.
 * Scoring: moon bumpers 100, slingshots 10, drop targets 500 (all three: 5,000 and they come back
 * up), lanes 250 (all three lit: the bonus multiplier goes up, up to 5x), launching through the
 * top 1,000. Three balls; a new one comes with every 50,000.
 *
 * The physics runs 8 steps a frame (2 ms each) so a fast ball cannot pass through a flipper. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

#define PW 420
#define PH 640
#define BR 8.0f
static float fabs_(float x) { return x < 0 ? -x : x; }
static float fsqrt_(float x) { if (x <= 0) return 0; float r = x > 1 ? x / 2 : 1; for (int i = 0; i < 12; i++) r = 0.5f * (r + x / r); return r; }
static float fsin_(float a) {                          /* within 0.001 */
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    float a2 = a * a;
    return a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
}
static float fcos_(float a) { return fsin_(a + 1.5707963f); }

typedef struct { float x0, y0, x1, y1, e; int kind; } seg_t;   /* e: restitution; kind 1: slingshot */
typedef struct { float x, y, r; int lit; } bumper_t;
typedef struct { float px, py, len, ang, rest, up, w; bool left, held; } flipper_t;
typedef struct {
    float bx, by, vx, vy; bool in_play, in_lane;
    float plunge; bool pulling;
    flipper_t fl[2];
    bumper_t bump[3];
    bool target[3]; int target_lit[3];
    bool lane[3]; int lane_lit[3];
    int score, balls, mult, best, next_extra;
    bool over, paused, gate_shut;
    char msg[64]; int msg_t;
    int sling_lit[2];
} pin_t;
static window_t *pin_win;

/* the table's walls */
static seg_t walls[64]; static int nwalls;
static void wall(float x0, float y0, float x1, float y1, float e, int kind) { if (nwalls < 64) walls[nwalls++] = (seg_t){ x0, y0, x1, y1, e, kind }; }
static void build_table(void) {
    nwalls = 0;
    /* the outer wall: a rounded top made of segments */
    wall(20, 600, 20, 160, 0.5f, 0);
    for (int i = 0; i < 10; i++) {
        float a0 = 3.14159f + i * 3.14159f / 10, a1 = 3.14159f + (i + 1) * 3.14159f / 10;
        wall(210 + 190 * fcos_(a0), 160 + 140 * fsin_(a0), 210 + 190 * fcos_(a1), 160 + 140 * fsin_(a1), 0.5f, 0);
    }
    wall(400, 160, 400, 630, 0.5f, 0);                   /* the right side, outside the plunger lane */
    wall(370, 630, 370, 210, 0.5f, 0);                   /* the plunger lane's inner wall */
    /* the bottom: guides down to the flippers */
    wall(20, 520, 124, 588, 0.5f, 0);
    wall(370, 520, 266, 588, 0.5f, 0);
    /* slingshots (kick) and their backs */
    wall(70, 430, 120, 520, 1.25f, 1); wall(70, 430, 70, 500, 0.5f, 0); wall(70, 500, 120, 520, 0.5f, 0);
    wall(320, 430, 270, 520, 1.25f, 1); wall(320, 430, 320, 500, 0.5f, 0); wall(320, 500, 270, 520, 0.5f, 0);
    /* the lane dividers at the top */
    wall(135, 70, 135, 110, 0.4f, 0); wall(195, 62, 195, 108, 0.4f, 0); wall(255, 62, 255, 108, 0.4f, 0); wall(315, 70, 315, 110, 0.4f, 0);
}
static void reset_ball(pin_t *p) {
    p->bx = 385; p->by = 600; p->vx = p->vy = 0; p->in_play = true; p->in_lane = true; p->gate_shut = false; p->plunge = 0;
}
static void pin_new(pin_t *p) {
    p->score = 0; p->balls = 3; p->mult = 1; p->over = false; p->next_extra = 50000;
    for (int i = 0; i < 3; i++) { p->target[i] = true; p->lane[i] = false; }
    p->fl[0] = (flipper_t){ 130, 590, 62, 0, 0.52f, -0.45f, 0, true, false };
    p->fl[1] = (flipper_t){ 260, 590, 62, 0, 3.14159f - 0.52f, 3.14159f + 0.45f, 0, false, false };
    p->fl[0].ang = p->fl[0].rest; p->fl[1].ang = p->fl[1].rest;
    p->bump[0] = (bumper_t){ 150, 230, 24, 0 }; p->bump[1] = (bumper_t){ 260, 220, 24, 0 }; p->bump[2] = (bumper_t){ 205, 300, 24, 0 };
    reset_ball(p);
    strcpy(p->msg, "Hold Space to pull the plunger"); p->msg_t = 200;
}
static void add_score(pin_t *p, int n) {
    p->score += n * p->mult;
    if (p->score >= p->next_extra) { p->balls++; p->next_extra += 50000; strcpy(p->msg, "Extra ball!"); p->msg_t = 90; speaker_beep(1200, 60); }
}
/* a ball against a segment: pushed out and bounced (true when it touched) */
static bool hit_seg(pin_t *p, float x0, float y0, float x1, float y1, float e, float svx, float svy, float rad) {
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p->bx - x0) * dx + (p->by - y0) * dy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float cx = x0 + t * dx, cy = y0 + t * dy, nx = p->bx - cx, ny = p->by - cy, d = fsqrt_(nx * nx + ny * ny);
    if (d >= BR + rad || d < 0.0001f) return false;
    nx /= d; ny /= d;
    p->bx = cx + nx * (BR + rad); p->by = cy + ny * (BR + rad);
    float rvx = p->vx - svx, rvy = p->vy - svy, vn = rvx * nx + rvy * ny;
    if (vn < 0) { rvx -= (1 + e) * vn * nx; rvy -= (1 + e) * vn * ny; p->vx = rvx + svx; p->vy = rvy + svy; }
    return true;
}
static void step(pin_t *p, float dt) {
    /* flippers swing */
    for (int k = 0; k < 2; k++) {
        flipper_t *f = &p->fl[k];
        float target = f->held ? f->up : f->rest, speed = 22.0f, old = f->ang;
        if (f->ang < target) { f->ang += speed * dt; if (f->ang > target) f->ang = target; }
        else if (f->ang > target) { f->ang -= speed * dt; if (f->ang < target) f->ang = target; }
        f->w = (f->ang - old) / dt;
    }
    if (!p->in_play) return;
    if (p->in_lane && p->pulling) { p->plunge += dt * 1.2f; if (p->plunge > 1) p->plunge = 1; p->by = 600 + p->plunge * 22; p->vx = p->vy = 0; return; }
    p->vy += 980 * dt;                                    /* the table's slope */
    if (p->in_lane && p->by > 600 && p->vy > 0) { p->by = 600; p->vy = 0; p->vx = 0; return; }   /* resting on the plunger */
    float sp = fsqrt_(p->vx * p->vx + p->vy * p->vy);
    if (sp > 1600) { p->vx *= 1600 / sp; p->vy *= 1600 / sp; }
    p->bx += p->vx * dt; p->by += p->vy * dt;
    if (p->in_lane && p->by < 200) { p->in_lane = false; add_score(p, 1000); }
    if (!p->in_lane && !p->gate_shut && p->bx < 360) p->gate_shut = true;
    for (int i = 0; i < nwalls; i++) {
        seg_t *s = &walls[i];
        if (hit_seg(p, s->x0, s->y0, s->x1, s->y1, s->e, 0, 0, 0) && s->kind == 1) {
            int side = s->x0 < 200 ? 0 : 1;
            float nx = s->y1 - s->y0, ny = -(s->x1 - s->x0), l = fsqrt_(nx * nx + ny * ny);
            nx /= l; ny /= l; if (side) { nx = -nx; ny = -ny; }
            p->vx += nx * 260; p->vy += ny * 260;
            p->sling_lit[side] = 8; add_score(p, 10);
        }
    }
    /* the gate over the plunger lane, once the ball is out */
    if (p->gate_shut) hit_seg(p, 370, 210, 400, 180, 0.4f, 0, 0, 0);
    /* the moons */
    for (int i = 0; i < 3; i++) {
        bumper_t *b = &p->bump[i];
        float dx = p->bx - b->x, dy = p->by - b->y, d = fsqrt_(dx * dx + dy * dy);
        if (d < b->r + BR && d > 0.001f) {
            dx /= d; dy /= d;
            p->bx = b->x + dx * (b->r + BR); p->by = b->y + dy * (b->r + BR);
            float vn = p->vx * dx + p->vy * dy;
            if (vn < 0) { p->vx -= 2 * vn * dx; p->vy -= 2 * vn * dy; }
            p->vx += dx * 320; p->vy += dy * 320;
            b->lit = 10; add_score(p, 100);
            speaker_beep(600 + i * 150, 15);
        }
    }
    /* drop targets: a bank on the left */
    for (int i = 0; i < 3; i++) {
        if (!p->target[i]) continue;
        float ty = 330 + i * 26;
        if (hit_seg(p, 40, ty, 40, ty + 20, 0.6f, 0, 0, 3)) {
            p->target[i] = false; p->target_lit[i] = 15; add_score(p, 500);
            if (!p->target[0] && !p->target[1] && !p->target[2]) {
                add_score(p, 5000); strcpy(p->msg, "Target bank! 5,000"); p->msg_t = 90;
                for (int k = 0; k < 3; k++) p->target[k] = true;
            }
        }
    }
    /* the lanes at the top */
    for (int i = 0; i < 3; i++) {
        float lx = 135 + i * 60;
        if (p->by > 70 && p->by < 100 && p->bx > lx && p->bx < lx + 60 && p->vy > 0 && !p->lane[i]) {
            p->lane[i] = true; p->lane_lit[i] = 20; add_score(p, 250);
            if (p->lane[0] && p->lane[1] && p->lane[2]) {
                if (p->mult < 5) p->mult++;
                snprintf(p->msg, sizeof p->msg, "Bonus x%d", p->mult); p->msg_t = 90;
                for (int k = 0; k < 3; k++) p->lane[k] = false;
            }
        }
    }
    /* flippers: a rounded bar, moving */
    for (int k = 0; k < 2; k++) {
        flipper_t *f = &p->fl[k];
        float tx = f->px + f->len * fcos_(f->ang), ty = f->py + f->len * fsin_(f->ang);
        float dx = tx - f->px, dy = ty - f->py, l2 = dx * dx + dy * dy;
        float t = ((p->bx - f->px) * dx + (p->by - f->py) * dy) / l2; t = t < 0 ? 0 : t > 1 ? 1 : t;
        float cx = f->px + t * dx, cy = f->py + t * dy;
        /* the surface moves at w x r */
        float rx = cx - f->px, ry = cy - f->py;
        float svx = -f->w * ry, svy = f->w * rx;
        hit_seg(p, f->px, f->py, tx, ty, 0.35f, svx, svy, 6 - t * 2);
    }
    if (p->by > PH + 20) {                               /* drained */
        p->in_play = false;
        if (--p->balls <= 0) {
            p->over = true;
            if (p->score > p->best) { p->best = p->score; char b[16]; snprintf(b, sizeof b, "%d", p->best); settings_set("pinball_best", b); settings_save(); }
            strcpy(p->msg, "Game over - F2 for a new game"); p->msg_t = 1 << 30;
        } else { strcpy(p->msg, "Ball lost"); p->msg_t = 60; reset_ball(p); }
        speaker_beep(180, 150);
    }
}
static void pin_tick(window_t *w) {
    pin_t *p = w->data;
    if (!p->paused && !p->over) for (int i = 0; i < 8; i++) step(p, 0.002f);
    for (int i = 0; i < 3; i++) { if (p->bump[i].lit) p->bump[i].lit--; if (p->target_lit[i]) p->target_lit[i]--; if (p->lane_lit[i]) p->lane_lit[i]--; }
    for (int i = 0; i < 2; i++) if (p->sling_lit[i]) p->sling_lit[i]--;
    if (p->msg_t > 0) p->msg_t--;
    w->dirty = true;
}

/* ---------------- drawing ---------------- */
static void thick(canvas_t *c, float x0, float y0, float x1, float y1, int t, uint32_t col) { draw_thick_line(c, (int)x0, (int)y0, (int)x1, (int)y1, t, col); }
static void pin_paint(window_t *w) {
    pin_t *p = w->data; canvas_t *c = w->surf;
    gradient_v(c, 0, 0, c->w, c->h, HEX(0x0B1440), HEX(0x05081C));
    /* stars, fixed */
    for (int i = 0; i < 70; i++) { uint32_t h = i * 2654435761u; put_pixel(c, 24 + h % 340, 40 + (h >> 12) % 560, HEX(0x8FA8E0)); }
    /* the planet the moons orbit, and its orbit */
    fill_circle(c, 205, 245, 46, HEX(0x1E3F8A)); fill_circle(c, 195, 235, 38, HEX(0x2D6BD0)); fill_circle(c, 186, 226, 14, HEX(0x6FA8F0));
    draw_circle(c, 205, 245, 92, HEX(0x2E4A8C)); draw_circle(c, 205, 245, 93, HEX(0x22386E));
    /* the walls */
    for (int i = 0; i < nwalls; i++) {
        seg_t *s = &walls[i];
        uint32_t col = s->kind == 1 ? (p->sling_lit[s->x0 < 200 ? 0 : 1] ? HEX(0xFFF08A) : HEX(0xF0A030)) : HEX(0x9FB8F0);
        thick(c, s->x0, s->y0, s->x1, s->y1, s->kind == 1 ? 4 : 3, col);
    }
    if (p->gate_shut) thick(c, 370, 210, 400, 180, 3, HEX(0x9FB8F0));
    /* lanes, targets, moons */
    for (int i = 0; i < 3; i++) {
        int lx = 135 + i * 60;
        uint32_t lc = p->lane[i] || p->lane_lit[i] ? HEX(0x7CFF7C) : HEX(0x284A28);
        fill_circle(c, lx + 30, 92, 5, lc);
        int ty = 330 + i * 26;
        if (p->target[i]) fill_rect(c, 34, ty, 8, 20, p->target_lit[i] ? WHITE : HEX(0xFF5A7A));
        else fill_rect(c, 34, ty + 16, 8, 4, HEX(0x60283A));
        bumper_t *b = &p->bump[i];
        uint32_t body = b->lit ? HEX(0xFFF3B0) : i == 0 ? HEX(0xC8C8D8) : i == 1 ? HEX(0xE8A070) : HEX(0x8AD0E8);
        fill_circle(c, (int)b->x, (int)b->y, (int)b->r, darken(body, 60));
        fill_circle(c, (int)b->x - 2, (int)b->y - 2, (int)b->r - 3, body);
        fill_circle(c, (int)b->x - 8, (int)b->y - 6, 4, darken(body, 30));
        fill_circle(c, (int)b->x + 6, (int)b->y + 7, 3, darken(body, 30));
    }
    /* the flippers */
    for (int k = 0; k < 2; k++) {
        flipper_t *f = &p->fl[k];
        float tx = f->px + f->len * fcos_(f->ang), ty = f->py + f->len * fsin_(f->ang);
        thick(c, f->px, f->py, tx, ty, 12, HEX(0xD8452A)); thick(c, f->px, f->py, tx, ty, 8, HEX(0xFF8A4A));
        fill_circle(c, (int)f->px, (int)f->py, 7, HEX(0xF4F6F9));
    }
    /* the plunger */
    int py = 618 + (int)(p->plunge * 22);
    fill_rect(c, 378, py, 14, PH - py, HEX(0x8890A0)); fill_rect(c, 376, py, 18, 4, HEX(0xD8DCE4));
    /* the ball */
    if (p->in_play) { fill_circle(c, (int)p->bx, (int)p->by, (int)BR, HEX(0x8C95A3)); fill_circle(c, (int)p->bx - 1, (int)p->by - 1, (int)BR - 1, HEX(0xE8ECF2)); fill_circle(c, (int)p->bx - 3, (int)p->by - 3, 2, WHITE); }
    /* the score panel */
    fill_rect(c, 0, 0, c->w, 28, HEX(0x000000));
    char b[96]; snprintf(b, sizeof b, "SCORE %d", p->score); draw_text(c, FONT_LARGE_BOLD, 10, 4, b, HEX(0xFFB030));
    snprintf(b, sizeof b, "BALL %d  x%d  BEST %d", p->balls, p->mult, p->best); draw_text(c, FONT_UI_BOLD, c->w - 10 - text_width(FONT_UI_BOLD, b), 8, b, HEX(0x7CD0FF));
    if (p->msg_t > 0 || p->paused) {
        const char *m = p->paused ? "Paused - P to go on" : p->msg;
        int tw = text_width(FONT_UI_BOLD, m);
        fill_round_rect_blend(c, (c->w - tw) / 2 - 12, 400, tw + 24, 30, 6, BLACK, 170);
        draw_text(c, FONT_UI_BOLD, (c->w - tw) / 2, 408, m, WHITE);
    }
}
static bool is_left(key_event_t *e) { return e->key == 'z' || e->key == 'Z' || e->key == KEY_LSHIFT || e->key == KEY_LEFT; }
static bool is_right(key_event_t *e) { return e->key == '/' || e->key == '?' || e->key == KEY_RSHIFT || e->key == KEY_RIGHT; }
static void pin_key(window_t *w, key_event_t *e) {
    pin_t *p = w->data;
    if (is_left(e)) { p->fl[0].held = e->pressed; if (e->pressed) speaker_beep(90, 10); }
    else if (is_right(e)) { p->fl[1].held = e->pressed; if (e->pressed) speaker_beep(90, 10); }
    else if (e->key == ' ' || e->key == KEY_DOWN) {
        if (e->pressed) { if (p->in_lane && p->in_play) p->pulling = true; }
        else if (p->pulling) { p->pulling = false; p->vy = -(700 + p->plunge * 1300); p->plunge = 0; p->by = 600; }
    } else if (e->pressed && e->key == KEY_F2) pin_new(p);
    else if (e->pressed && (e->key == 'p' || e->key == 'P')) p->paused = !p->paused;
    w->dirty = true;
}
static void pin_destroy(window_t *w) { kfree(w->data); w->data = NULL; pin_win = NULL; }
void pinball_open(void) {
    if (pin_win && wm_alive(pin_win)) { wm_focus(pin_win); return; }
    build_table();
    pin_t *p = kzalloc(sizeof *p);
    if (!p) return;
    const char *v = settings_get("pinball_best"); p->best = v ? atoi(v) : 0;
    pin_new(p);
    window_t *w = wm_create("Orbit Pinball", -1, -1, PW, PH, WF_NO_MAX);
    w->data = p; w->paint = pin_paint; w->key = pin_key; w->destroy = pin_destroy; w->icon = ICON_PINBALL;
    wm_set_timer(w, 16, pin_tick);
    pin_win = w;
}
