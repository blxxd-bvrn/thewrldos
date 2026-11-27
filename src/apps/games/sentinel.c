/* WRLD Sentinel: a first-person maze shooter. Each level is a maze generated for the game (rooms
 * and loops cut into it), walled with textures drawn here; hovering security drones guard it and
 * fire energy bolts; a portal leads to the next level. All of it - levels, textures, drones,
 * the blaster - is made by this file; nothing comes from another game.
 *
 * Keys: W/S or Up/Down move, A/D step sideways, Left/Right turn, Space or Ctrl (or a click)
 * fires, M the map, P pauses, F2 starts again. Health cells (green) and energy cells (blue) lie
 * about; reaching the portal with every drone down gives a bonus.
 *
 * The view is cast at 320x200 (a ray per column, walls only, the floor and ceiling shaded by
 * distance) and drawn at twice the size. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

#define MW 21
#define MH 21
#define RW 320
#define RH 200
#define TS 64
#define MAXE 24
#define MAXP 24
#define MAXB 32

static float fabs_(float x) { return x < 0 ? -x : x; }
static float fsqrt_(float x) { if (x <= 0) return 0; float r = x > 1 ? x / 2 : 1; for (int i = 0; i < 10; i++) r = 0.5f * (r + x / r); return r; }
static float fsin_(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    float a2 = a * a;
    return a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
}
static float fcos_(float a) { return fsin_(a + 1.5707963f); }

typedef struct { float x, y; int hp, cool, hit, dead; float bob; } drone_t;
typedef struct { float x, y; int kind; bool taken; } pick_t;          /* kind 0 health, 1 energy, 2 portal */
typedef struct { float x, y, dx, dy; bool live; } bolt_t;
typedef struct {
    uint8_t map[MH][MW];
    float px, py, ang;
    int hp, energy, level, score, best, kills;
    drone_t e[MAXE]; int ne;
    pick_t pk[MAXP]; int npk;
    bolt_t b[MAXB];
    bool keys[512];
    int fire_cool, flash, hurt, msg_t, over, won_level, paused; bool show_map;
    char msg[80];
    float zbuf[RW];
    canvas_t *view;
} sen_t;
static window_t *sen_win;
static uint32_t tex[4][TS * TS];                         /* wall textures 1..3 */
static uint32_t spr[4][TS * TS];                         /* drone, health, energy, portal (alpha 0 = clear) */
static bool made;
static uint32_t rnd_state = 0x9E3779B9u;
static uint32_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }

/* ---------------- the art, drawn once ---------------- */
static void make_art(void) {
    if (made) return;
    made = true;
    for (int y = 0; y < TS; y++) for (int x = 0; x < TS; x++) {
        /* 1: metal panels with seams and rivets */
        int n = (int)(rnd() % 18);
        uint32_t c = HEX(0x5A6A82);
        if (y % 32 == 0 || x % 32 == 0) c = HEX(0x2A3240); else if (y % 32 == 1 || x % 32 == 1) c = HEX(0x8A9AB4);
        if ((x % 32 == 5 || x % 32 == 27) && (y % 32 == 5 || y % 32 == 27)) c = HEX(0xC8D2E0);
        tex[1][y * TS + x] = mix(c, n > 9 ? HEX(0x6A7A92) : HEX(0x4A5A72), 40);
        /* 2: a dark wall with glowing circuit lines */
        uint32_t d = HEX(0x161C28);
        if ((y == 20 && x > 8 && x < 40) || (x == 40 && y > 20 && y < 50) || (y == 50 && x > 40 && x < 60) || (x == 12 && y > 30)) d = HEX(0x30E0F0);
        if (ABSDIFF(x, 40) + ABSDIFF(y, 50) < 3) d = HEX(0xA0FFFF);
        tex[2][y * TS + x] = mix(d, HEX(0x0A0E16), n * 4);
        /* 3: rough stone */
        int k = (x * 7 + y * 13 + (int)(rnd() % 9)) % 23;
        uint32_t st = mix(HEX(0x6B5640), HEX(0x8A7258), k * 11);
        if ((y + (x / 16) * 8) % 16 == 0) st = HEX(0x3E3226);
        tex[3][y * TS + x] = st;
    }
    for (int k = 0; k < 4; k++) for (int i = 0; i < TS * TS; i++) spr[k][i] = 0;
    for (int y = 0; y < TS; y++) for (int x = 0; x < TS; x++) {
        int dx = x - 32, dy = y - 30, d2 = dx * dx + dy * dy;
        uint32_t *o = &spr[0][y * TS + x];
        if (d2 < 20 * 20) *o = mix(HEX(0xB0B8C8), HEX(0x505868), (dy + 20) * 6);            /* the drone's shell */
        if (d2 < 9 * 9) *o = HEX(0x200808);                                               /* its eye */
        if (d2 < 6 * 6) *o = HEX(0xFF3020);
        if (d2 < 2 * 2) *o = HEX(0xFFE0A0);
        if (ABSDIFF(y, 30) < 2 && ABSDIFF(x, 32) > 18 && ABSDIFF(x, 32) < 30) *o = HEX(0x707888);   /* fins */
        if (y > 50 && y < 54 && ABSDIFF(x, 32) < 8) *o = HEX(0x40C0FF);                   /* thruster glow */
        /* health: a green cell with a cross; energy: a blue cell with a bolt */
        bool cell = x > 20 && x < 44 && y > 36 && y < 62;
        if (cell) {
            spr[1][y * TS + x] = HEX(0x2E9E3E); spr[2][y * TS + x] = HEX(0x2D6BD0);
            if ((ABSDIFF(x, 32) < 3 && y > 41 && y < 57) || (ABSDIFF(y, 49) < 3 && x > 24 && x < 40)) spr[1][y * TS + x] = WHITE;
            if (ABSDIFF(x - 32, (49 - y) / 2) < 2 && y > 40 && y < 58) spr[2][y * TS + x] = HEX(0xFFE36A);
        }
        /* the portal: rings */
        int r = (int)fsqrt_((float)((x - 32) * (x - 32) + (y - 32) * (y - 32)));
        if (r < 30 && r > 4) spr[3][y * TS + x] = (r / 4) & 1 ? HEX(0x9C6CFF) : HEX(0x30E0F0);
    }
}

/* ---------------- a level ---------------- */
static bool solid(sen_t *s, float x, float y) { int ix = (int)x, iy = (int)y; return ix < 0 || iy < 0 || ix >= MW || iy >= MH || s->map[iy][ix]; }
static void level_new(sen_t *s) {
    rnd_state ^= timer_ticks() * 2654435761u;
    for (int y = 0; y < MH; y++) for (int x = 0; x < MW; x++) s->map[y][x] = 1 + (rnd() % 7 == 0 ? 1 : 0) + (rnd() % 11 == 0 ? 1 : 0);
    /* a maze: depth first from (1,1) on the odd cells */
    int stack[MW * MH], sp = 0; bool seen[MH][MW] = { { false } };
    stack[sp++] = 1 * MW + 1; seen[1][1] = true; s->map[1][1] = 0;
    while (sp) {
        int cur = stack[sp - 1], cx = cur % MW, cy = cur / MW, opts[4], no = 0;
        static const int dx[4] = { 2, -2, 0, 0 }, dy[4] = { 0, 0, 2, -2 };
        for (int k = 0; k < 4; k++) { int nx = cx + dx[k], ny = cy + dy[k]; if (nx > 0 && ny > 0 && nx < MW - 1 && ny < MH - 1 && !seen[ny][nx]) opts[no++] = k; }
        if (!no) { sp--; continue; }
        int k = opts[rnd() % no], nx = cx + dx[k], ny = cy + dy[k];
        s->map[cy + dy[k] / 2][cx + dx[k] / 2] = 0; s->map[ny][nx] = 0; seen[ny][nx] = true;
        stack[sp++] = ny * MW + nx;
    }
    /* loops and a few rooms, so it is not one corridor */
    for (int i = 0; i < 30; i++) { int x = 1 + rnd() % (MW - 2), y = 1 + rnd() % (MH - 2); if ((x + y) % 2) s->map[y][x] = 0; }
    for (int r = 0; r < 3; r++) { int x = 2 + rnd() % (MW - 6), y = 2 + rnd() % (MH - 6); for (int yy = y; yy < y + 3; yy++) for (int xx = x; xx < x + 3; xx++) s->map[yy][xx] = 0; }
    /* the start, and the farthest place from it for the portal */
    s->px = 1.5f; s->py = 1.5f;
    s->ang = !s->map[1][2] ? 0 : 1.5707963f;           /* facing down the open passage */
    int dist[MH][MW], q[MW * MH], qh = 0, qt = 0, far = MW + 1;
    for (int y = 0; y < MH; y++) for (int x = 0; x < MW; x++) dist[y][x] = -1;
    dist[1][1] = 0; q[qt++] = MW + 1;
    while (qh < qt) {
        int c = q[qh++], x = c % MW, y = c / MW;
        static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) { int nx = x + dx[k], ny = y + dy[k]; if (!s->map[ny][nx] && dist[ny][nx] < 0) { dist[ny][nx] = dist[y][x] + 1; q[qt++] = ny * MW + nx; if (dist[ny][nx] > dist[far / MW][far % MW]) far = ny * MW + nx; } }
    }
    s->npk = 0;
    s->pk[s->npk++] = (pick_t){ far % MW + 0.5f, far / MW + 0.5f, 2, false };
    /* drones away from the start; cells */
    s->ne = MIN(MAXE, 4 + s->level * 3);
    for (int i = 0; i < s->ne; i++) {
        int x, y, tries = 0;
        do { x = 1 + rnd() % (MW - 2); y = 1 + rnd() % (MH - 2); } while ((s->map[y][x] || dist[y][x] < 6) && ++tries < 500);
        s->e[i] = (drone_t){ x + 0.5f, y + 0.5f, 3, 30 + rnd() % 60, 0, 0, (float)(rnd() % 100) };
    }
    for (int i = 0; i < 8 && s->npk < MAXP; i++) {
        int x, y, tries = 0;
        do { x = 1 + rnd() % (MW - 2); y = 1 + rnd() % (MH - 2); } while (s->map[y][x] && ++tries < 500);
        s->pk[s->npk++] = (pick_t){ x + 0.5f, y + 0.5f, i % 2, false };
    }
    for (int i = 0; i < MAXB; i++) s->b[i].live = false;
    s->won_level = 0;
    snprintf(s->msg, sizeof s->msg, "Level %d: find the portal", s->level); s->msg_t = 120;
}
static void game_new(sen_t *s) { s->hp = 100; s->energy = 40; s->level = 1; s->score = 0; s->kills = 0; s->over = 0; level_new(s); }
static bool sight(sen_t *s, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0, d = fsqrt_(dx * dx + dy * dy);
    int n = (int)(d * 8) + 1;
    for (int i = 1; i < n; i++) if (solid(s, x0 + dx * i / n, y0 + dy * i / n)) return false;
    return true;
}

/* ---------------- the world moving ---------------- */
static bool key(sen_t *s, int k) { return k >= 0 && k < 512 && s->keys[k]; }
static void fire(sen_t *s) {
    if (s->fire_cool || s->energy <= 0) return;
    s->energy--; s->fire_cool = 8; s->flash = 4;
    speaker_beep(880, 20);
    /* the nearest drone near the middle of the view, nearer than the wall there */
    float dirx = fcos_(s->ang), diry = fsin_(s->ang), best = 1e9f; int hit = -1;
    for (int i = 0; i < s->ne; i++) {
        drone_t *e = &s->e[i];
        if (e->dead) continue;
        float rx = e->x - s->px, ry = e->y - s->py;
        float depth = rx * dirx + ry * diry, side = -rx * diry + ry * dirx;
        if (depth < 0.2f || fabs_(side) > 0.35f || depth > s->zbuf[RW / 2] + 0.2f) continue;
        if (depth < best) { best = depth; hit = i; }
    }
    if (hit >= 0) {
        drone_t *e = &s->e[hit];
        e->hit = 6;
        if (--e->hp <= 0) { e->dead = 1; s->score += 100; s->kills++; speaker_beep(220, 60); }
    }
}
static void move(sen_t *s, float dx, float dy) {
    float r = 0.22f;
    if (!solid(s, s->px + dx + (dx > 0 ? r : -r), s->py)) s->px += dx;
    if (!solid(s, s->px, s->py + dy + (dy > 0 ? r : -r))) s->py += dy;
}
static void sen_tick(window_t *w) {
    sen_t *s = w->data;
    w->dirty = true;
    if (s->msg_t > 0) s->msg_t--;
    if (s->paused || s->over) return;
    float dt = 0.033f, sp = 3.0f * dt, rot = 2.4f * dt;
    float dirx = fcos_(s->ang), diry = fsin_(s->ang);
    if (key(s, KEY_LEFT)) s->ang -= rot;
    if (key(s, KEY_RIGHT)) s->ang += rot;
    if (key(s, 'w') || key(s, KEY_UP)) move(s, dirx * sp, diry * sp);
    if (key(s, 's') || key(s, KEY_DOWN)) move(s, -dirx * sp, -diry * sp);
    if (key(s, 'a')) move(s, diry * sp, -dirx * sp);
    if (key(s, 'd')) move(s, -diry * sp, dirx * sp);
    if (key(s, ' ') || key(s, KEY_LCTRL)) fire(s);
    if (s->fire_cool) s->fire_cool--;
    if (s->flash) s->flash--;
    if (s->hurt) s->hurt--;
    int alive = 0;
    for (int i = 0; i < s->ne; i++) {
        drone_t *e = &s->e[i];
        if (e->dead) continue;
        alive++;
        e->bob += 0.12f; if (e->hit) e->hit--;
        float rx = s->px - e->x, ry = s->py - e->y, d = fsqrt_(rx * rx + ry * ry);
        if (d < 9 && sight(s, e->x, e->y, s->px, s->py)) {
            if (d > 1.6f) {                              /* closer, round the walls */
                float vx = rx / d * 1.1f * dt, vy = ry / d * 1.1f * dt;
                if (!solid(s, e->x + vx * 3, e->y)) e->x += vx;
                if (!solid(s, e->x, e->y + vy * 3)) e->y += vy;
            }
            if (--e->cool <= 0) {                        /* a bolt */
                e->cool = 45 + rnd() % 40 - s->level * 4;
                for (int k = 0; k < MAXB; k++) if (!s->b[k].live) { s->b[k] = (bolt_t){ e->x, e->y, rx / d * 5 * dt, ry / d * 5 * dt, true }; break; }
            }
        }
    }
    for (int k = 0; k < MAXB; k++) {
        bolt_t *b = &s->b[k];
        if (!b->live) continue;
        b->x += b->dx; b->y += b->dy;
        if (solid(s, b->x, b->y)) { b->live = false; continue; }
        float dx = b->x - s->px, dy = b->y - s->py;
        if (dx * dx + dy * dy < 0.09f) {
            b->live = false; s->hp -= 8 + s->level * 2; s->hurt = 8; speaker_beep(140, 40);
            if (s->hp <= 0) {
                s->hp = 0; s->over = 1;
                if (s->score > s->best) { s->best = s->score; char bb[16]; snprintf(bb, sizeof bb, "%d", s->best); settings_set("sentinel_best", bb); settings_save(); }
                strcpy(s->msg, "You were stopped. F2 to start again."); s->msg_t = 1 << 30;
            }
        }
    }
    for (int i = 0; i < s->npk; i++) {
        pick_t *p = &s->pk[i];
        if (p->taken) continue;
        float dx = p->x - s->px, dy = p->y - s->py;
        if (dx * dx + dy * dy > 0.25f) continue;
        if (p->kind == 0 && s->hp < 100) { p->taken = true; s->hp = MIN(100, s->hp + 30); speaker_beep(1000, 30); }
        else if (p->kind == 1) { p->taken = true; s->energy += 20; speaker_beep(1200, 30); }
        else if (p->kind == 2) {                         /* the portal */
            int bonus = alive ? 500 : 2000 + s->level * 500;
            s->score += bonus; s->level++;
            level_new(s);
            snprintf(s->msg, sizeof s->msg, "Level %d (+%d%s)", s->level, bonus, alive ? "" : ", all drones down");
            s->msg_t = 120;
            return;
        }
    }
}

/* ---------------- drawing ---------------- */
static void cast(sen_t *s) {
    canvas_t *v = s->view;
    float dirx = fcos_(s->ang), diry = fsin_(s->ang), plx = -diry * 0.66f, ply = dirx * 0.66f;
    uint32_t sky = s->level % 3 == 1 ? HEX(0x101826) : s->level % 3 == 2 ? HEX(0x1A1020) : HEX(0x0C1A16);
    for (int y = 0; y < RH / 2; y++) { uint32_t c = mix(sky, HEX(0x000000), y * 255 / (RH / 2)); for (int x = 0; x < RW; x++) v->px[y * v->stride + x] = c; }
    for (int y = RH / 2; y < RH; y++) { uint32_t c = mix(HEX(0x000000), HEX(0x3A3A40), (y - RH / 2) * 255 / (RH / 2)); for (int x = 0; x < RW; x++) v->px[y * v->stride + x] = c; }
    for (int x = 0; x < RW; x++) {
        float cam = 2.0f * x / RW - 1, rx = dirx + plx * cam, ry = diry + ply * cam;
        int mx = (int)s->px, my = (int)s->py;
        float ddx = rx == 0 ? 1e30f : fabs_(1 / rx), ddy = ry == 0 ? 1e30f : fabs_(1 / ry), sdx, sdy;
        int stx, sty, side = 0, t = 1;
        if (rx < 0) { stx = -1; sdx = (s->px - mx) * ddx; } else { stx = 1; sdx = (mx + 1 - s->px) * ddx; }
        if (ry < 0) { sty = -1; sdy = (s->py - my) * ddy; } else { sty = 1; sdy = (my + 1 - s->py) * ddy; }
        for (int n = 0; n < 64; n++) {
            if (sdx < sdy) { sdx += ddx; mx += stx; side = 0; } else { sdy += ddy; my += sty; side = 1; }
            if (mx < 0 || my < 0 || mx >= MW || my >= MH) break;
            if (s->map[my][mx]) { t = s->map[my][mx]; break; }
        }
        float dist = side ? sdy - ddy : sdx - ddx;
        if (dist < 0.05f) dist = 0.05f;
        s->zbuf[x] = dist;
        int h = (int)(RH / dist), y0 = RH / 2 - h / 2;
        float wx = side ? s->px + dist * rx : s->py + dist * ry; wx -= (int)wx;
        int tx = (int)(wx * TS) & (TS - 1);
        int shade = MIN(230, (int)(dist * 22)) + (side ? 30 : 0);
        for (int y = MAX(0, y0); y < MIN(RH, y0 + h); y++) {
            int ty = ((y - y0) * TS / MAX(1, h)) & (TS - 1);
            v->px[y * v->stride + x] = mix(tex[t > 3 ? 1 : t][ty * TS + tx], HEX(0x000000), MIN(255, shade));
        }
    }
    /* sprites, far to near */
    struct { float x, y; int k; int hit; float bob; } sp[MAXE + MAXP + MAXB]; int ns = 0;
    for (int i = 0; i < s->ne; i++) if (!s->e[i].dead) { sp[ns].x = s->e[i].x; sp[ns].y = s->e[i].y; sp[ns].k = 0; sp[ns].hit = s->e[i].hit; sp[ns].bob = s->e[i].bob; ns++; }
    for (int i = 0; i < s->npk; i++) if (!s->pk[i].taken) { sp[ns].x = s->pk[i].x; sp[ns].y = s->pk[i].y; sp[ns].k = s->pk[i].kind + 1; sp[ns].hit = 0; sp[ns].bob = 0; ns++; }
    for (int i = 0; i < MAXB; i++) if (s->b[i].live) { sp[ns].x = s->b[i].x; sp[ns].y = s->b[i].y; sp[ns].k = 9; sp[ns].hit = 0; sp[ns].bob = 0; ns++; }
    float inv = 1.0f / (plx * diry - dirx * ply);
    for (int a = 1; a < ns; a++) for (int b = a; b > 0; b--) {
        float d0 = (sp[b].x - s->px) * (sp[b].x - s->px) + (sp[b].y - s->py) * (sp[b].y - s->py);
        float d1 = (sp[b - 1].x - s->px) * (sp[b - 1].x - s->px) + (sp[b - 1].y - s->py) * (sp[b - 1].y - s->py);
        if (d0 > d1) { __typeof__(sp[0]) t = sp[b]; sp[b] = sp[b - 1]; sp[b - 1] = t; } else break;
    }
    for (int i = 0; i < ns; i++) {
        float rx = sp[i].x - s->px, ry = sp[i].y - s->py;
        float tx = inv * (diry * rx - dirx * ry), ty = inv * (-ply * rx + plx * ry);
        if (ty < 0.1f) continue;
        int sx = (int)((RW / 2) * (1 + tx / ty));
        if (sp[i].k == 9) {                              /* a bolt: a bright dot */
            int r = MAX(1, (int)(6 / ty));
            if (sx >= 0 && sx < RW && ty < s->zbuf[sx]) for (int yy = -r; yy <= r; yy++) for (int xx = -r; xx <= r; xx++) {
                int px = sx + xx, py = RH / 2 + yy; if (px >= 0 && px < RW && py >= 0 && py < RH && xx * xx + yy * yy <= r * r) v->px[py * v->stride + px] = HEX(0xFFB040);
            }
            continue;
        }
        float scale = sp[i].k == 0 ? 0.7f : sp[i].k == 3 ? 1.0f : 0.6f;
        int h = (int)(RH / ty * scale), wdt = h;
        int lift = sp[i].k == 0 ? (int)(RH / ty * (0.1f + 0.05f * fsin_(sp[i].bob))) : sp[i].k == 3 ? 0 : -(int)(RH / ty * 0.2f);
        int y0 = RH / 2 - h / 2 - lift, x0 = sx - wdt / 2;
        const uint32_t *img = spr[sp[i].k == 0 ? 0 : sp[i].k == 1 ? 1 : sp[i].k == 2 ? 2 : 3];
        for (int x = MAX(0, x0); x < MIN(RW, x0 + wdt); x++) {
            if (ty >= s->zbuf[x]) continue;
            int u = (x - x0) * TS / MAX(1, wdt);
            for (int y = MAX(0, y0); y < MIN(RH, y0 + h); y++) {
                uint32_t c = img[((y - y0) * TS / MAX(1, h)) * TS + u];
                if (!c) continue;
                if (sp[i].hit) c = HEX(0xFFFFFF);
                v->px[y * v->stride + x] = mix(c, HEX(0x000000), MIN(200, (int)(ty * 18)));
            }
        }
    }
}
static void sen_paint(window_t *w) {
    sen_t *s = w->data; canvas_t *c = w->surf;
    if (!s->view) s->view = canvas_new(RW, RH);
    if (!s->view) { gfx_clear(c, BLACK); return; }
    cast(s);
    int vh = c->h - 40;
    blit_scaled_alpha(c, R(0, 0, c->w, vh), s->view, 255);
    if (s->hurt) blend_rect(c, 0, 0, c->w, vh, HEX(0xC00000), s->hurt * 12);
    /* the blaster */
    int gx = c->w / 2, gy = vh;
    fill_round_rect(c, gx - 30, gy - 70, 60, 80, 10, HEX(0x2A3240));
    fill_round_rect(c, gx - 22, gy - 90, 44, 40, 8, HEX(0x5A6A82));
    fill_rect(c, gx - 8, gy - 110, 16, 30, HEX(0x8A9AB4));
    fill_rect(c, gx - 12, gy - 60, 24, 6, HEX(0x30E0F0));
    if (s->flash) { fill_circle(c, gx, gy - 116, 14 + s->flash * 2, HEX(0xFFE36A)); fill_circle(c, gx, gy - 116, 8, WHITE); }
    /* the crosshair */
    hline(c, c->w / 2 - 10, vh / 2, 7, HEX(0x9CFF9C)); hline(c, c->w / 2 + 4, vh / 2, 7, HEX(0x9CFF9C));
    vline(c, c->w / 2, vh / 2 - 10, 7, HEX(0x9CFF9C)); vline(c, c->w / 2, vh / 2 + 4, 7, HEX(0x9CFF9C));
    /* the status bar */
    gradient_v(c, 0, vh, c->w, 40, HEX(0x1A2230), HEX(0x0A0E16));
    char b[120];
    int alive = 0; for (int i = 0; i < s->ne; i++) alive += !s->e[i].dead;
    draw_text(c, FONT_UI_BOLD, 12, vh + 6, "HEALTH", HEX(0x9CFF9C));
    fill_rect(c, 12, vh + 24, 120, 8, HEX(0x203020)); fill_rect(c, 12, vh + 24, s->hp * 120 / 100, 8, s->hp > 30 ? HEX(0x40D040) : HEX(0xE04040));
    snprintf(b, sizeof b, "ENERGY %d", s->energy); draw_text(c, FONT_UI_BOLD, 150, vh + 12, b, HEX(0x7CD0FF));
    snprintf(b, sizeof b, "LEVEL %d   DRONES %d   SCORE %d   BEST %d", s->level, alive, s->score, s->best);
    draw_text(c, FONT_UI_BOLD, 260, vh + 12, b, WHITE);
    /* the map */
    if (s->show_map) {
        int cs = 6, ox = c->w - MW * cs - 10, oy = 10;
        blend_rect(c, ox - 4, oy - 4, MW * cs + 8, MH * cs + 8, BLACK, 150);
        for (int y = 0; y < MH; y++) for (int x = 0; x < MW; x++) if (s->map[y][x]) fill_rect(c, ox + x * cs, oy + y * cs, cs, cs, HEX(0x5A6A82));
        for (int i = 0; i < s->npk; i++) if (s->pk[i].kind == 2) fill_circle(c, ox + (int)(s->pk[i].x * cs), oy + (int)(s->pk[i].y * cs), 3, HEX(0x9C6CFF));
        for (int i = 0; i < s->ne; i++) if (!s->e[i].dead) fill_rect(c, ox + (int)(s->e[i].x * cs) - 1, oy + (int)(s->e[i].y * cs) - 1, 3, 3, HEX(0xFF4030));
        int ppx = ox + (int)(s->px * cs), ppy = oy + (int)(s->py * cs);
        fill_circle(c, ppx, ppy, 3, HEX(0x9CFF9C));
        draw_line(c, ppx, ppy, ppx + (int)(fcos_(s->ang) * 8), ppy + (int)(fsin_(s->ang) * 8), HEX(0x9CFF9C));
    }
    if (s->msg_t > 0 || s->paused) {
        const char *m = s->paused ? "Paused - P to go on" : s->msg;
        int tw = text_width(FONT_LARGE_BOLD, m);
        fill_round_rect_blend(c, (c->w - tw) / 2 - 14, 30, tw + 28, 38, 8, BLACK, 160);
        draw_text(c, FONT_LARGE_BOLD, (c->w - tw) / 2, 38, m, HEX(0xFFE36A));
    }
}
static void sen_key(window_t *w, key_event_t *e) {
    sen_t *s = w->data;
    int k = e->key;
    if (k >= 'A' && k <= 'Z') k += 32;
    if (k >= 0 && k < 512) s->keys[k] = e->pressed;
    if (!e->pressed) return;
    if (k == KEY_F2) game_new(s);
    else if (k == 'm') s->show_map = !s->show_map;
    else if (k == 'p' || k == KEY_ESC) s->paused = !s->paused;
    w->dirty = true;
}
static void sen_mouse(window_t *w, int type, int x, int y, int btn) { (void)x; (void)y; sen_t *s = w->data; if (type == ME_DOWN && (btn & MB_LEFT)) fire(s); }
static void sen_activate(window_t *w, bool on) { sen_t *s = w->data; if (!on) memset(s->keys, 0, sizeof s->keys); }   /* no key stays held */
static void sen_destroy(window_t *w) { sen_t *s = w->data; if (s->view) canvas_free(s->view); kfree(s); w->data = NULL; sen_win = NULL; }
void sentinel_open(void) {
    if (sen_win && wm_alive(sen_win)) { wm_focus(sen_win); return; }
    make_art();
    sen_t *s = kzalloc(sizeof *s);
    if (!s) return;
    const char *v = settings_get("sentinel_best"); s->best = v ? atoi(v) : 0;
    game_new(s);
    s->show_map = true;
    window_t *w = wm_create("WRLD Sentinel", -1, -1, 640, 440, WF_NO_MAX);
    w->data = s; w->paint = sen_paint; w->key = sen_key; w->mouse = sen_mouse; w->destroy = sen_destroy; w->activate = sen_activate; w->icon = ICON_FPS;
    wm_set_timer(w, 33, sen_tick);
    sen_win = w;
}
