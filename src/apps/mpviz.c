/* Visualisations for THE WRLD Media Player.
 *
 * Most looks are feedback effects, the technique of the full-screen music
 * visualisers of the early 2000s: every frame the previous picture is warped
 * (zoomed, rotated, swirled, folded...), softened and faded, then new shapes
 * driven by the music are drawn on top, so everything leaves flowing trails.
 * The picture is kept as 8-bit intensities at 320x200 and coloured through a
 * 256-colour palette that drifts with time and flashes on the beat; it is then
 * stretched over the visualisation area with bilinear filtering.
 *
 * Collections: Ambience and Charge (feedback presets, Charge with a
 * Randomization preset that keeps inventing new combinations), Bars and Waves,
 * Plasma, Particles, Album Art and Spectrum 3D (drawn by the WRLD 3D engine). */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "r3d.h"
#include "mpviz.h"

#define VW 320
#define VH 200
#define PI 3.14159265f

/* ---------------- small maths ---------------- */
static int16_t sin_t[1024];                     /* Q14 */
static bool tables;
static void make_tables(void) {
    if (tables) return;
    for (int i = 0; i < 1024; i++) sin_t[i] = (int16_t)(r3d_sin(i * 2 * PI / 1024) * 16384);
    tables = true;
}
static inline float fsin(float a) { int i = (int)(a * (1024 / (2 * PI))) & 1023; return sin_t[i] / 16384.0f; }
static inline float fcos(float a) { return fsin(a + PI / 2); }
static float fatan2(float y, float x) {           /* within about 0.005 rad */
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax < 1e-9f && ay < 1e-9f) return 0;
    float a = ax > ay ? ay / ax : ax / ay, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = PI / 2 - r;
    if (x < 0) r = PI - r;
    return y < 0 ? -r : r;
}
static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float frand(void) { return (rnd() & 0xFFFFFF) / 16777216.0f; }
static inline uint8_t sat8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* ---------------- the 8-bit picture ---------------- */
static uint8_t *pic, *tmp;                       /* VW*VH each */
static uint32_t *warp;                            /* source index per pixel */
static uint32_t pal[256];
static float *pr, *pa;                            /* polar coordinates of each pixel */
static int warp_kind = -1, warp_age;
static bool buffers(void) {
    make_tables();
    if (pic) return true;
    pic = kzalloc(VW * VH); tmp = kzalloc(VW * VH);
    warp = kmalloc(sizeof(uint32_t) * VW * VH);
    pr = kmalloc(sizeof(float) * VW * VH); pa = kmalloc(sizeof(float) * VW * VH);
    if (!pic || !tmp || !warp || !pr || !pa) { kfree(pic); kfree(tmp); kfree(warp); kfree(pr); kfree(pa); pic = NULL; return false; }
    for (int y = 0; y < VH; y++) for (int x = 0; x < VW; x++) {
        float dx = x - VW / 2 + 0.5f, dy = (y - VH / 2 + 0.5f) * 1.2f;
        pr[y * VW + x] = r3d_sqrt(dx * dx + dy * dy);
        pa[y * VW + x] = fatan2(dy, dx);
    }
    return true;
}
void viz_reset(void) { if (pic) memset(pic, 0, VW * VH); warp_kind = -1; }

static inline void plot(int x, int y, int v) {
    if ((unsigned)x >= VW || (unsigned)y >= VH) return;
    uint8_t *p = &pic[y * VW + x]; *p = sat8(*p + v);
}
static void blob(int x, int y, int v) { plot(x, y, v); plot(x + 1, y, v * 3 / 4); plot(x, y + 1, v * 3 / 4); plot(x - 1, y, v / 2); plot(x, y - 1, v / 2); }
static void line8(int x0, int y0, int x1, int y1, int v, bool thick) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0, sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int n = 0; n < 2000; n++) {
        if (thick) blob(x0, y0, v); else plot(x0, y0, v);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void disc(int cx, int cy, int r, int v) {
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
        int d = x * x + y * y;
        if (d <= r * r) plot(cx + x, cy + y, v * (r * r - d) / (r * r + 1));
    }
}

/* ---------------- warps ---------------- */
enum { W_ZOOM, W_ZOOMROT, W_SWIRL, W_RIPPLE, W_TUNNEL, W_KALEID, W_RISE, W_MIRROR, W_WAVE, W_OUT, W_COUNT };
/* recompute the source index of every pixel; `t` animates the warp, `k` is its strength */
static void make_warp(int kind, float t, float k) {
    float cx = VW / 2 - 0.5f, cy = VH / 2 - 0.5f;
    for (int y = 0; y < VH; y++) for (int x = 0; x < VW; x++) {
        int i = y * VW + x;
        float r = pr[i], a = pa[i], sx = x, sy = y;
        switch (kind) {
        case W_ZOOM: r *= 1 - 0.035f * k; goto polar;
        case W_ZOOMROT: r *= 1 - 0.025f * k; a += 0.02f * k * fsin(t * 0.3f); goto polar;
        case W_SWIRL: r *= 1 - 0.015f * k; a += 0.06f * k * (1 - r / 200 > 0 ? 1 - r / 200 : 0) + 0.004f; goto polar;
        case W_RIPPLE: r *= 1 - 0.012f * k + 0.01f * fsin(r * 0.12f - t * 3); goto polar;
        case W_TUNNEL: r *= 1 - 0.06f * k; a += 0.035f * k; goto polar;
        case W_KALEID: {                         /* fold into 6 wedges, mirror, drift inwards */
            float w = 2 * PI / 6, aa = a + t * 0.1f;
            while (aa < 0) aa += w;
            while (aa >= w) aa -= w;
            if (aa > w / 2) aa = w - aa;
            a = aa - t * 0.1f + 0.01f; r *= 1 - 0.02f * k;
            goto polar;
        }
        case W_RISE: sx = x + 1.2f * fsin(y * 0.08f + t * 2); sy = y + 1.6f * k; break;   /* flames: the picture climbs */
        case W_MIRROR: { float dx = x - cx; if (dx < 0) dx = -dx; sx = cx + dx * (1 - 0.03f * k); sy = cy + (y - cy) * (1 - 0.03f * k); break; }
        case W_WAVE: sx = cx + (x - cx) * (1 - 0.02f * k) + 1.5f * fsin(y * 0.06f + t); sy = cy + (y - cy) * (1 - 0.02f * k) + 1.5f * fcos(x * 0.05f + t * 1.3f); break;
        case W_OUT: r *= 1 + 0.03f * k; a -= 0.01f; goto polar;
        }
        goto store;
    polar:
        sx = cx + r * fcos(a);
        sy = cy + r * fsin(a) / 1.2f;
    store:
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx > VW - 2) sx = VW - 2;
        if (sy > VH - 2) sy = VH - 2;
        warp[i] = (uint32_t)sy * VW + (uint32_t)sx;
    }
}
/* warp + soften + fade */
static void feedback(int decay) {
    for (int i = 0; i < VW * VH; i++) {
        uint32_t s = warp[i];
        int v = (pic[s] + pic[s + 1] + pic[s + VW] + pic[s + VW + 1]) >> 2;
        tmp[i] = (uint8_t)(v * decay >> 8);
    }
    uint8_t *t = pic; pic = tmp; tmp = t;
}

/* ---------------- palettes ---------------- */
enum { P_FIRE, P_ICE, P_RAINBOW, P_NEON, P_GOLD, P_AURORA, P_HUE, P_TOXIC, P_COUNT };
static uint32_t lerpc(uint32_t a, uint32_t b, int t) { return mix(a, b, t); }
static void make_pal(int kind, float t, float flash) {
    int hue = (int)(t * 12) % 360;
    for (int i = 0; i < 256; i++) {
        uint32_t c;
        switch (kind) {
        case P_FIRE: c = i < 85 ? lerpc(0xFF000000, 0xFFB01000, i * 3) : i < 170 ? lerpc(0xFFB01000, 0xFFFFB020, (i - 85) * 3) : lerpc(0xFFFFB020, 0xFFFFFFFF, (i - 170) * 3); break;
        case P_ICE: c = i < 128 ? lerpc(0xFF000008, 0xFF1060D0, i * 2) : lerpc(0xFF1060D0, 0xFFE0FFFF, (i - 128) * 2); break;
        case P_RAINBOW: c = i < 24 ? lerpc(0xFF000000, hsv_to_rgb(hue + i * 3, 255, 120), i * 10) : hsv_to_rgb(hue + i * 2, 230 - i / 3, 120 + i / 2); break;
        case P_NEON: c = i < 100 ? lerpc(0xFF000000, 0xFF7010C0, i * 255 / 100) : i < 200 ? lerpc(0xFF7010C0, 0xFFFF40B0, (i - 100) * 255 / 100) : lerpc(0xFFFF40B0, 0xFFFFFFFF, (i - 200) * 255 / 56); break;
        case P_GOLD: c = i < 128 ? lerpc(0xFF080400, 0xFF906010, i * 2) : lerpc(0xFF906010, 0xFFFFF0B0, (i - 128) * 2); break;
        case P_AURORA: c = i < 85 ? lerpc(0xFF000010, 0xFF0C6060, i * 3) : i < 170 ? lerpc(0xFF0C6060, 0xFF40E080, (i - 85) * 3) : lerpc(0xFF40E080, 0xFFE0C0FF, (i - 170) * 3); break;
        case P_TOXIC: c = i < 128 ? lerpc(0xFF000000, 0xFF208010, i * 2) : lerpc(0xFF208010, 0xFFE0FF40, (i - 128) * 2); break;
        default: c = i < 128 ? lerpc(0xFF000000, hsv_to_rgb(hue, 220, 230), i * 2) : lerpc(hsv_to_rgb(hue, 220, 230), 0xFFFFFFFF, (i - 128) * 2); break;
        }
        if (flash > 0) c = mix(c, 0xFFFFFFFF, (int)(flash * i / 3));
        pal[i] = c | 0xFF000000u;
    }
}
/* stretch the picture over r through the palette, filtering between pixels */
static void present(canvas_t *dst, rect_t r) {
    rect_t c = rect_intersect(r, dst->clip);
    if (c.w <= 0 || c.h <= 0) return;
    uint32_t fx_step = (uint32_t)(((uint64_t)(VW - 1) << 16) / MAX(1, r.w)), fy_step = (uint32_t)(((uint64_t)(VH - 1) << 16) / MAX(1, r.h));
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t fy = (uint32_t)(y - r.y) * fy_step, sy = fy >> 16, wy = (fy >> 8) & 255;
        const uint8_t *r0 = pic + sy * VW, *r1 = r0 + (sy + 1 < VH ? VW : 0);
        uint32_t *d = dst->px + (size_t)y * dst->stride;
        uint32_t fx = (uint32_t)(c.x - r.x) * fx_step;
        for (int x = c.x; x < c.x + c.w; x++, fx += fx_step) {
            uint32_t sx = fx >> 16, wx = (fx >> 8) & 255;
            int a = r0[sx], b = r0[sx + 1], e = r1[sx], f = r1[sx + 1];
            int top = a + (((b - a) * (int)wx) >> 8), bot = e + (((f - e) * (int)wx) >> 8);
            d[x] = pal[top + (((bot - top) * (int)wy) >> 8)];
        }
    }
}

/* ---------------- shapes ---------------- */
enum { S_LINE = 1, S_RING = 2, S_SPIKES = 4, S_DOTS = 8, S_BURST = 16, S_BOLT = 32, S_FLOOR = 64, S_STAR = 128, S_CROSS = 256 };
static void shapes(int s, const viz_audio_t *a, float t, float gain) {
    int cx = VW / 2, cy = VH / 2;
    int v = (int)(200 * gain);
    if (s & S_LINE) {                                /* the waveform across the middle */
        int py = cy;
        for (int x = 0; x < VW; x++) {
            int y = cy + a->wave[x * 512 / VW] * 60 / 32768;
            if (x) line8(x - 1, py, x, y, v, true);
            py = y;
        }
    }
    if (s & S_RING) {                                /* the waveform bent into a circle */
        float base = 34 + a->bass * 30;
        int px = 0, py = 0;
        for (int k = 0; k <= 128; k++) {
            float ang = k * 2 * PI / 128 + t * 0.4f, rr = base + a->wave[(k * 4) & 511] * 26 / 32768.0f;
            int x = cx + (int)(rr * fcos(ang)), y = cy + (int)(rr * fsin(ang) / 1.2f);
            if (k) line8(px, py, x, y, v, false);
            px = x; py = y;
        }
    }
    if (s & S_SPIKES) {                              /* spectrum as rays */
        for (int b = 0; b < VIZ_BANDS; b++) for (int side = 0; side < 2; side++) {
            float ang = (side ? -1 : 1) * (b + 0.5f) * PI / VIZ_BANDS + PI / 2 + t * 0.2f;
            float len = 10 + a->band[b] * 80;
            line8(cx, cy, cx + (int)(len * fcos(ang)), cy + (int)(len * fsin(ang) / 1.2f), (int)(v * (0.4f + a->band[b] * 0.6f)), false);
        }
    }
    if (s & S_DOTS) {                                /* dots orbiting with the bass */
        for (int k = 0; k < 10; k++) {
            float ang = t * (0.6f + k * 0.13f) + k * 0.63f, rr = 30 + k * 7 + a->bass * 40;
            blob(cx + (int)(rr * fcos(ang)), cy + (int)(rr * fsin(ang * 1.3f) / 1.2f), v);
        }
    }
    if ((s & S_BURST) && a->beat) disc(cx, cy, 12 + (int)(a->bass * 24), 255);
    if ((s & S_BOLT) && a->beat) {                   /* lightning from the centre */
        int x = cx, y = cy;
        float ang = frand() * 2 * PI;
        for (int k = 0; k < 14; k++) {
            int nx = x + (int)(10 * fcos(ang)) + (int)(rnd() % 9) - 4, ny = y + (int)(8 * fsin(ang)) + (int)(rnd() % 9) - 4;
            line8(x, y, nx, ny, 255, true);
            x = nx; y = ny;
        }
    }
    if (s & S_FLOOR) {                               /* spectrum bars along the bottom (with W_RISE they become flames) */
        int bw = VW / VIZ_BANDS;
        for (int b = 0; b < VIZ_BANDS; b++) {
            int h = (int)(a->band[b] * 60);
            for (int y = 0; y < h; y++) for (int x = 1; x < bw - 1; x++) plot(b * bw + x, VH - 1 - y, v / 2);
        }
    }
    if (s & S_STAR) {                                /* a turning star that grows with the bass */
        int n = 5; float rr = 20 + a->bass * 50, rot = t * 0.7f;
        for (int k = 0; k < n; k++) {
            float a0 = rot + k * 4 * PI / n, a1 = rot + (k + 1) * 4 * PI / n;
            line8(cx + (int)(rr * fcos(a0)), cy + (int)(rr * fsin(a0) / 1.2f), cx + (int)(rr * fcos(a1)), cy + (int)(rr * fsin(a1) / 1.2f), v, true);
        }
    }
    if (s & S_CROSS) {                               /* the waveform as a vertical and a horizontal line */
        for (int y = 0; y < VH; y++) plot(cx + a->wave[y * 2] * 50 / 32768, y, v);
        for (int x = 0; x < VW; x++) plot(x, cy + a->wave[(x + 200) & 511] * 40 / 32768, v);
    }
}

/* ---------------- presets ---------------- */
typedef struct { const char *name; int warp, shapes, pal, decay; float strength; } fb_t;
static const fb_t ambience[] = {
    { "Aurora", W_WAVE, S_RING, P_AURORA, 250, 0.6f },
    { "Deep Water", W_RIPPLE, S_LINE, P_ICE, 247, 1.0f },
    { "Drift", W_ZOOMROT, S_DOTS, P_HUE, 251, 0.7f },
    { "Galaxy", W_SWIRL, S_DOTS | S_STAR, P_NEON, 249, 1.0f },
    { "Mist", W_OUT, S_RING, P_ICE, 246, 0.6f },
};
static const fb_t charge[] = {
    { "Randomization", 0, 0, 0, 0, 0 },
    { "Event Horizon", W_TUNNEL, S_RING | S_BURST, P_FIRE, 240, 1.0f },
    { "Kaleidoscope", W_KALEID, S_SPIKES | S_LINE, P_RAINBOW, 244, 1.0f },
    { "Lightning", W_ZOOM, S_BOLT | S_LINE, P_ICE, 232, 1.0f },
    { "Spiral Burst", W_SWIRL, S_SPIKES | S_BURST, P_NEON, 243, 1.6f },
    { "Prism", W_MIRROR, S_RING | S_STAR, P_RAINBOW, 245, 1.0f },
    { "Flame Floor", W_RISE, S_FLOOR | S_LINE, P_FIRE, 236, 1.0f },
    { "Gold Dust", W_ZOOMROT, S_DOTS | S_BURST, P_GOLD, 247, 1.2f },
    { "Crosswire", W_ZOOM, S_CROSS, P_TOXIC, 238, 0.8f },
};
enum { C_AMBIENCE, C_CHARGE, C_BARS, C_PLASMA, C_PARTICLES, C_ART, C_3D, C_COUNT };
static const char *coll_names[C_COUNT] = { "Ambience", "Charge", "Bars and Waves", "Plasma", "Particles", "Album Art", "Spectrum 3D" };
static const char *bars_names[] = { "Bars", "Mirrored Bars", "Scope", "Fire Scope", "Spectrum Circle" };
static const char *plasma_names[] = { "Lava Lamp", "Liquid Neon", "Interference" };
static const char *particle_names[] = { "Starfield", "Fountain", "Fireworks" };
static const char *art_names[] = { "Album Art", "Pulsing Cover" };
static const char *d3_names[] = { "Terrain", "Towers" };

int viz_collections(void) { return C_COUNT; }
const char *viz_collection(int c) { return c >= 0 && c < C_COUNT ? coll_names[c] : ""; }
int viz_presets(int c) {
    switch (c) {
    case C_AMBIENCE: return ARRAY_LEN(ambience);
    case C_CHARGE: return ARRAY_LEN(charge);
    case C_BARS: return ARRAY_LEN(bars_names);
    case C_PLASMA: return ARRAY_LEN(plasma_names);
    case C_PARTICLES: return ARRAY_LEN(particle_names);
    case C_ART: return ARRAY_LEN(art_names);
    case C_3D: return ARRAY_LEN(d3_names);
    }
    return 0;
}
const char *viz_preset(int c, int p) {
    if (p < 0 || p >= viz_presets(c)) return "";
    switch (c) {
    case C_AMBIENCE: return ambience[p].name;
    case C_CHARGE: return charge[p].name;
    case C_BARS: return bars_names[p];
    case C_PLASMA: return plasma_names[p];
    case C_PARTICLES: return particle_names[p];
    case C_ART: return art_names[p];
    default: return d3_names[p];
    }
}

/* Randomization: a new combination every 12 seconds, or on a strong beat after 8 */
static fb_t rand_fb; static uint32_t rand_since; static char rand_name[48];
static const char *rand_words[2][8] = { { "Neon", "Silent", "Electric", "Burning", "Frozen", "Liquid", "Velvet", "Cosmic" },
                                        { "Spiral", "Bloom", "Pulse", "Storm", "Tide", "Prism", "Orbit", "Flare" } };
static void randomize(uint32_t now) {
    rand_fb.warp = rnd() % W_COUNT;
    static const int combos[] = { S_RING, S_LINE, S_SPIKES, S_DOTS | S_STAR, S_RING | S_BURST, S_SPIKES | S_BURST, S_BOLT | S_LINE, S_CROSS, S_FLOOR | S_LINE, S_DOTS | S_BURST, S_STAR | S_LINE };
    rand_fb.shapes = combos[rnd() % ARRAY_LEN(combos)];
    if (rand_fb.warp == W_RISE) rand_fb.shapes |= S_FLOOR;
    rand_fb.pal = rnd() % P_COUNT;
    rand_fb.decay = 234 + rnd() % 16;
    rand_fb.strength = 0.6f + frand();
    snprintf(rand_name, sizeof rand_name, "%s %s", rand_words[0][rnd() % 8], rand_words[1][rnd() % 8]);
    rand_since = now;
    warp_kind = -1;
}
const char *viz_current_name(void) { return rand_name; }

static float flash_level;
static void draw_feedback(canvas_t *dst, rect_t r, const fb_t *f, const viz_audio_t *a) {
    float t = a->ms / 1000.0f;
    /* the warp changes slowly: rebuild it a few times a second */
    if (warp_kind != f->warp || ++warp_age >= 4) { make_warp(f->warp, t, f->strength * (0.7f + a->bass * 0.6f)); warp_kind = f->warp; warp_age = 0; }
    feedback(f->decay);
    if (a->playing) shapes(f->shapes, a, t, 0.5f + a->level * 0.8f);
    else shapes(f->shapes & (S_RING | S_DOTS | S_STAR), a, t, 0.25f);
    flash_level = a->beat ? 0.7f : flash_level * 0.8f;
    make_pal(f->pal, t, flash_level * 0.5f);
    present(dst, r);
}

/* ---------------- Bars and Waves ---------------- */
static float peaks[VIZ_BANDS];
static void draw_bars(canvas_t *c, rect_t r, int p, const viz_audio_t *a) {
    fill_rect(c, r.x, r.y, r.w, r.h, 0xFF000000);
    if (p == 0 || p == 1) {
        int n = VIZ_BANDS, gap = 2, bw = MAX(2, (r.w - 20 - gap * (n - 1)) / n);
        int total = bw * n + gap * (n - 1), x0 = r.x + (r.w - total) / 2;
        int base = p ? r.y + r.h / 2 : r.y + r.h - 12, hmax = p ? r.h / 2 - 10 : r.h - 30;
        for (int b = 0; b < n; b++) {
            float v = a->band[b];
            peaks[b] = v > peaks[b] ? v : peaks[b] - 0.012f;
            if (peaks[b] < 0) peaks[b] = 0;
            int h = (int)(v * hmax), x = x0 + b * (bw + gap);
            for (int y = 0; y < h; y += 3) {
                uint32_t col = hsv_to_rgb(120 - y * 120 / MAX(1, hmax), 230, 255);   /* green to red */
                fill_rect(c, x, base - y - 2, bw, 2, col);
                if (p) fill_rect(c, x, base + y, bw, 2, mix(col, 0xFF000000, 120));
            }
            int py = base - (int)(peaks[b] * hmax) - 4;
            fill_rect(c, x, py, bw, 2, 0xFFFFFFFF);
        }
        return;
    }
    if (p == 2) {                                         /* scope */
        int cy = r.y + r.h / 2, px = 0, py = cy;
        hline(c, r.x, cy, r.w, 0xFF103010);
        for (int x = 0; x < r.w; x++) {
            int y = cy - a->wave[x * 512 / MAX(1, r.w)] * (r.h / 2 - 6) / 32768;
            if (x) { draw_line(c, r.x + px, py, r.x + x, y, 0xFF40FF60); draw_line(c, r.x + px, py + 1, r.x + x, y + 1, 0xFF108020); }
            px = x; py = y;
        }
        return;
    }
    if (p == 3) {                                         /* fire scope: a feedback scope */
        static const fb_t fs = { "", W_RISE, S_LINE, P_FIRE, 230, 1.2f };
        draw_feedback(c, r, &fs, a);
        return;
    }
    /* spectrum circle */
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2, base = MIN(r.w, r.h) / 6;
    float t = a->ms / 1000.0f;
    for (int b = 0; b < VIZ_BANDS * 2; b++) {
        float v = a->band[b < VIZ_BANDS ? b : VIZ_BANDS * 2 - 1 - b], ang = b * 2 * PI / (VIZ_BANDS * 2) + t * 0.3f;
        int len = base + (int)(v * MIN(r.w, r.h) / 3);
        uint32_t col = hsv_to_rgb((int)(b * 360 / (VIZ_BANDS * 2) + t * 40), 200, 255);
        draw_thick_line(c, cx + (int)(base * fcos(ang)), cy + (int)(base * fsin(ang)), cx + (int)(len * fcos(ang)), cy + (int)(len * fsin(ang)), 3, col);
    }
    fill_circle(c, cx, cy, base - 4 + (int)(a->bass * 8), 0xFF101828);
}

/* ---------------- Plasma ---------------- */
static void draw_plasma(canvas_t *c, rect_t r, int p, const viz_audio_t *a) {
    if (!buffers()) return;
    float t = a->ms / 1000.0f * (0.6f + a->level);
    int t1 = (int)(t * 60), t2 = (int)(t * 41), t3 = (int)(t * 23);
    int sc = p == 2 ? 9 : p == 1 ? 6 : 3;
    for (int y = 0; y < VH; y++) for (int x = 0; x < VW; x++) {
        int i = y * VW + x, v;
        if (p == 2) v = sin_t[((int)(pr[i] * 9) - t1 * 3) & 1023] + sin_t[((int)(pr[i] * 7) + x * 2 + t2 * 2) & 1023];
        else v = sin_t[(x * sc + t1) & 1023] + sin_t[(y * sc * 3 / 2 - t2) & 1023] + sin_t[((x + y) * sc / 2 + t3) & 1023] + sin_t[((int)(pr[i] * sc) - t1) & 1023];
        pic[i] = (uint8_t)((v >> 8) + 128 + (int)(a->bass * 60));
    }
    flash_level = a->beat ? 0.6f : flash_level * 0.8f;
    make_pal(p == 0 ? P_FIRE : p == 1 ? P_NEON : P_HUE, t, flash_level * 0.4f);
    if (p == 0) for (int i = 0; i < 256; i++) pal[i] = hsv_to_rgb((int)(i * 1.4f + t * 20) % 360, 240, 90 + (i > 128 ? 255 - i : i)) | 0xFF000000u;
    present(c, r);
}

/* ---------------- Particles ---------------- */
#define NP 700
typedef struct { float x, y, z, vx, vy, vz; int life; uint8_t hue; } part_t;
static part_t *parts; static int part_mode = -1;
static void draw_particles(canvas_t *c, rect_t r, int p, const viz_audio_t *a) {
    if (!buffers()) return;
    if (!parts) { parts = kzalloc(sizeof(part_t) * NP); if (!parts) return; }
    if (part_mode != p) { memset(parts, 0, sizeof(part_t) * NP); memset(pic, 0, VW * VH); part_mode = p; }
    for (int i = 0; i < VW * VH; i++) pic[i] = (uint8_t)(pic[i] * (p == 0 ? 180 : 215) >> 8);
    float speed = 0.5f + a->bass * 2.5f + a->level;
    for (int i = 0; i < NP; i++) {
        part_t *q = &parts[i];
        if (p == 0) {                                       /* starfield: stars fly out of the screen */
            if (q->z <= 0.05f) { q->x = frand() * 2 - 1; q->y = frand() * 2 - 1; q->z = 1 + frand() * 2; }
            q->z -= 0.02f * speed;
            if (q->z <= 0.05f) continue;
            int sx = VW / 2 + (int)(q->x / q->z * VW / 2), sy = VH / 2 + (int)(q->y / q->z * VH / 2);
            int v = (int)(255 * (1.2f - q->z / 3));
            if (q->z < 0.6f) blob(sx, sy, v); else plot(sx, sy, v);
        } else if (p == 1) {                                /* fountain */
            if (q->life <= 0) {
                if (frand() > 0.05f + a->level * 0.5f) continue;
                q->x = VW / 2; q->y = VH - 4; q->vx = (frand() - 0.5f) * (1.5f + a->mid * 3); q->vy = -(2.5f + frand() * 2 + a->bass * 4); q->life = 120;
            }
            q->vy += 0.06f; q->x += q->vx; q->y += q->vy; q->life--;
            if (q->y >= VH) { q->life = 0; continue; }
            blob((int)q->x, (int)q->y, 60 + q->life);
        } else {                                            /* fireworks: a burst on every beat */
            if (q->life <= 0) continue;
            q->vy += 0.03f; q->vx *= 0.985f; q->vy *= 0.985f;
            q->x += q->vx; q->y += q->vy; q->life--;
            blob((int)q->x, (int)q->y, MIN(255, q->life * 4));
        }
    }
    if (p == 2 && (a->beat || (!a->playing && rnd() % 40 == 0))) {
        float bx = 40 + frand() * (VW - 80), by = 30 + frand() * (VH / 2);
        int made = 0;
        for (int i = 0; i < NP && made < 90; i++) if (parts[i].life <= 0) {
            float ang = frand() * 2 * PI, sp = 0.5f + frand() * 2.2f;
            parts[i].x = bx; parts[i].y = by; parts[i].vx = sp * fcos(ang); parts[i].vy = sp * fsin(ang); parts[i].life = 60 + rnd() % 30;
            made++;
        }
    }
    make_pal(p == 0 ? P_ICE : p == 1 ? P_AURORA : P_RAINBOW, a->ms / 1000.0f, 0);
    present(c, r);
}

/* ---------------- Album Art ---------------- */
static void draw_art(canvas_t *c, rect_t r, int p, const viz_audio_t *a, canvas_t *art) {
    gradient_v(c, r.x, r.y, r.w, r.h, 0xFF101C40, 0xFF000000);
    if (!art) { draw_text_centered(c, FONT_UI, r, "No album art for this item", 0xFFC0C8E0); return; }
    int s = MIN(r.w, r.h) * 2 / 3;
    if (p == 1) s = s * (90 + (int)(a->bass * 14)) / 100;
    rect_t d = R(r.x + (r.w - s) / 2, r.y + (r.h - s) / 2 - 6, s, s);
    blit_scaled_alpha(c, d, art, 255);
    draw_rect(c, d.x - 1, d.y - 1, d.w + 2, d.h + 2, 0xFF6080C0);
    /* reflection */
    for (int y = 0; y < 24 && d.y + d.h + 2 + y < r.y + r.h; y++)
        blend_rect(c, d.x, d.y + d.h + 2 + y, d.w, 1, 0xFF6080C0, 60 - y * 2);
}

/* ---------------- Spectrum 3D ---------------- */
#define GW 24
#define GD 20
static float hist[GD][GW];
static void draw_3d(canvas_t *c, rect_t r, int p, const viz_audio_t *a) {
    gradient_v(c, r.x, r.y, r.w, r.h, 0xFF000010, 0xFF101830);
    if (!accel3d) {
        draw_text_centered(c, FONT_UI, R(r.x, r.y + r.h / 2 - 20, r.w, 20), "Spectrum 3D needs 3D acceleration.", 0xFFE0E8FF);
        draw_text_centered(c, FONT_SMALL, R(r.x, r.y + r.h / 2 + 2, r.w, 16), "Turn it on in Control Panel > Visual Effects.", 0xFFA0B0D0);
        return;
    }
    /* history: the newest spectrum at the front row */
    static uint32_t last;
    if (a->ms - last > 60 || a->ms < last) {
        last = a->ms;
        memmove(hist[1], hist[0], sizeof(float) * GW * (GD - 1));
        for (int x = 0; x < GW; x++) hist[0][x] = a->band[x * VIZ_BANDS / GW];
    }
    r3d_t R3;
    if (!r3d_begin(&R3, c, r)) return;
    R3.lighting = false; R3.cull_back = false;
    R3.proj = m4_perspective(0.9f, (float)r.w / MAX(1, r.h), 0.1f, 50);
    float t = a->ms / 1000.0f;
    R3.view_m = m4_mul(m4_translate(0, -0.9f, -4.2f), m4_mul(m4_rotate_x(0.55f), m4_rotate_y(0.25f * r3d_sin(t * 0.2f))));
    R3.model = m4_identity();
    r3d_vertex_t v[GW * 6];
    if (p == 0) {                                          /* a landscape of the recent spectrum */
        for (int z = 0; z < GD - 1; z++) {
            int k = 0;
            for (int x = 0; x < GW - 1; x++) {
                float x0 = (x - GW / 2.0f) * 0.2f, x1 = x0 + 0.2f, z0 = -z * 0.25f + 1.5f, z1 = z0 - 0.25f;
                float h00 = hist[z][x] * 1.2f, h10 = hist[z][x + 1] * 1.2f, h01 = hist[z + 1][x] * 1.2f, h11 = hist[z + 1][x + 1] * 1.2f;
                uint32_t c00 = hsv_to_rgb(240 - (int)(h00 * 200), 230, 120 + (int)(h00 * 110)), c11 = hsv_to_rgb(240 - (int)(h11 * 200), 230, 120 + (int)(h11 * 110));
                r3d_vertex_t q[4] = { { x0, h00, z0, 0, 0, 0, 1, 0, c00, 0 }, { x1, h10, z0, 0, 0, 0, 1, 0, c00, 0 },
                                      { x1, h11, z1, 0, 0, 0, 1, 0, c11, 0 }, { x0, h01, z1, 0, 0, 0, 1, 0, c11, 0 } };
                v[k++] = q[0]; v[k++] = q[1]; v[k++] = q[2]; v[k++] = q[0]; v[k++] = q[2]; v[k++] = q[3];
            }
            r3d_triangles(&R3, v, k);
        }
    } else {                                               /* towers: one block per band */
        R3.view_m = m4_mul(m4_translate(0, -0.6f, -4.5f), m4_mul(m4_rotate_x(0.45f), m4_rotate_y(t * 0.3f)));
        for (int b = 0; b < GW; b++) {
            float ang = b * 2 * PI / GW, rad = 1.3f, h = 0.05f + hist[0][b] * 1.6f, s = 0.12f;
            float cx = rad * fcos(ang), cz = rad * fsin(ang);
            uint32_t top = hsv_to_rgb(b * 360 / GW, 200, 255), side = hsv_to_rgb(b * 360 / GW, 220, 160);
            float X0 = cx - s, X1 = cx + s, Z0 = cz - s, Z1 = cz + s;
            r3d_vertex_t box[30] = {
                { X0, h, Z0, 0, 0, 0, 0, 0, top, 0 }, { X1, h, Z0, 0, 0, 0, 0, 0, top, 0 }, { X1, h, Z1, 0, 0, 0, 0, 0, top, 0 },
                { X0, h, Z0, 0, 0, 0, 0, 0, top, 0 }, { X1, h, Z1, 0, 0, 0, 0, 0, top, 0 }, { X0, h, Z1, 0, 0, 0, 0, 0, top, 0 },
                { X0, 0, Z0, 0, 0, 0, 0, 0, side, 0 }, { X1, 0, Z0, 0, 0, 0, 0, 0, side, 0 }, { X1, h, Z0, 0, 0, 0, 0, 0, side, 0 },
                { X0, 0, Z0, 0, 0, 0, 0, 0, side, 0 }, { X1, h, Z0, 0, 0, 0, 0, 0, side, 0 }, { X0, h, Z0, 0, 0, 0, 0, 0, side, 0 },
                { X0, 0, Z1, 0, 0, 0, 0, 0, side, 0 }, { X1, 0, Z1, 0, 0, 0, 0, 0, side, 0 }, { X1, h, Z1, 0, 0, 0, 0, 0, side, 0 },
                { X0, 0, Z1, 0, 0, 0, 0, 0, side, 0 }, { X1, h, Z1, 0, 0, 0, 0, 0, side, 0 }, { X0, h, Z1, 0, 0, 0, 0, 0, side, 0 },
                { X0, 0, Z0, 0, 0, 0, 0, 0, side, 0 }, { X0, 0, Z1, 0, 0, 0, 0, 0, side, 0 }, { X0, h, Z1, 0, 0, 0, 0, 0, side, 0 },
                { X0, 0, Z0, 0, 0, 0, 0, 0, side, 0 }, { X0, h, Z1, 0, 0, 0, 0, 0, side, 0 }, { X0, h, Z0, 0, 0, 0, 0, 0, side, 0 },
                { X1, 0, Z0, 0, 0, 0, 0, 0, side, 0 }, { X1, 0, Z1, 0, 0, 0, 0, 0, side, 0 }, { X1, h, Z1, 0, 0, 0, 0, 0, side, 0 },
                { X1, 0, Z0, 0, 0, 0, 0, 0, side, 0 }, { X1, h, Z1, 0, 0, 0, 0, 0, side, 0 }, { X1, h, Z0, 0, 0, 0, 0, 0, side, 0 },
            };
            r3d_triangles(&R3, box, 30);
        }
    }
}

void viz_draw(canvas_t *dst, rect_t r, int c, int p, const viz_audio_t *a, canvas_t *art) {
    if (r.w <= 0 || r.h <= 0) return;
    rect_t old = canvas_push_clip(dst, r);
    switch (c) {
    case C_AMBIENCE:
    case C_CHARGE: {
        if (!buffers()) break;
        const fb_t *f = c == C_AMBIENCE ? &ambience[CLAMP(p, 0, (int)ARRAY_LEN(ambience) - 1)] : &charge[CLAMP(p, 0, (int)ARRAY_LEN(charge) - 1)];
        if (c == C_CHARGE && p == 0) {
            if (!rand_name[0] || a->ms - rand_since > 12000 || a->ms < rand_since || (a->beat && a->bass > 0.85f && a->ms - rand_since > 8000)) randomize(a->ms);
            f = &rand_fb;
        }
        draw_feedback(dst, r, f, a);
        break;
    }
    case C_BARS: draw_bars(dst, r, p, a); break;
    case C_PLASMA: draw_plasma(dst, r, p, a); break;
    case C_PARTICLES: draw_particles(dst, r, p, a); break;
    case C_ART: draw_art(dst, r, p, a, art); break;
    default: draw_3d(dst, r, p, a); break;
    }
    dst->clip = old;
}
