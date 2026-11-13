/* WRLD 3D: a test of the software 3D engine (r3d.c). A textured cube wearing
 * the WRLD globe, a lit sphere and a tiled floor turn in a perspective scene;
 * the bar shows frames per second, triangles and pixels per frame, and toggles
 * for lighting, textures and filtering. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "r3d.h"

#define BAR_H 58
typedef struct {
    canvas_t *globe_tex, *floor_tex;
    r3d_vertex_t *sphere; int nsphere;
    uint32_t t0, frames, fps_t, fps;
    bool lighting, textures, filtering;
    uint32_t tris, pix;
} d3_t;

static canvas_t *make_globe_tex(void) {
    canvas_t *c = canvas_new(128, 128);
    if (!c) return NULL;
    gradient_v(c, 0, 0, 128, 128, HEX(0x2E7BE5), HEX(0x0E3C8C));
    draw_globe(c, 8, 8, 112, 0);
    draw_rect(c, 0, 0, 128, 128, HEX(0xFFFFFF));
    draw_rect(c, 1, 1, 126, 126, HEX(0x9EC6FF));
    return c;
}
static canvas_t *make_floor_tex(void) {
    canvas_t *c = canvas_new(64, 64);
    if (!c) return NULL;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
        c->px[y * c->stride + x] = ((x / 16 + y / 16) & 1) ? HEX(0xDCE4F0) : HEX(0x8FA3C2);
    return c;
}
static void make_sphere(d3_t *d) {
    const int LAT = 14, LON = 22;
    d->sphere = kmalloc(sizeof(r3d_vertex_t) * LAT * LON * 6);
    if (!d->sphere) return;
    int n = 0;
    for (int i = 0; i < LAT; i++) for (int j = 0; j < LON; j++) {
        float t0 = 3.14159265f * i / LAT, t1 = 3.14159265f * (i + 1) / LAT;
        float p0 = 6.2831853f * j / LON, p1 = 6.2831853f * (j + 1) / LON;
        float pts[4][2] = { { t0, p0 }, { t0, p1 }, { t1, p1 }, { t1, p0 } };
        r3d_vertex_t q[4];
        for (int k = 0; k < 4; k++) {
            float st = r3d_sin(pts[k][0]), ct = r3d_cos(pts[k][0]), sp = r3d_sin(pts[k][1]), cp = r3d_cos(pts[k][1]);
            float x = st * cp, y = ct, z = st * sp;
            int hue = (int)(pts[k][1] * 57.3f) % 360;
            uint32_t col = hsv_to_rgb(hue, 140, 255) | 0xFF000000u;
            q[k] = (r3d_vertex_t){ x, y, z, 0, 0, x, y, z, col };
        }
        /* two triangles, wound so the outside faces the viewer */
        r3d_vertex_t tri[6] = { q[0], q[2], q[1], q[0], q[3], q[2] };
        memcpy(d->sphere + n, tri, sizeof tri); n += 6;
    }
    d->nsphere = n;
}
static void cube(r3d_t *r) {
    static const float f[6][4][3] = {
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },      /* front  +z */
        { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },  /* back   -z */
        { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } },      /* right  +x */
        { { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },  /* left   -x */
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } },      /* top    +y */
        { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } },  /* bottom -y */
    };
    static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
    for (int i = 0; i < 6; i++) {
        r3d_vertex_t q[4];
        for (int k = 0; k < 4; k++) q[k] = (r3d_vertex_t){ f[i][k][0], f[i][k][1], f[i][k][2], uv[k][0], uv[k][1], n[i][0], n[i][1], n[i][2], 0xFFFFFFFFu };
        r3d_quad(r, q);
    }
}

static void d3_paint(window_t *w) {
    d3_t *d = w->data;
    canvas_t *c = w->surf;
    rect_t view = R(0, 0, c->w, c->h - BAR_H);
    gradient_v(c, 0, 0, c->w, view.h, HEX(0x0A1A3A), HEX(0x3A6EB8));
    r3d_t r;
    bool on = r3d_begin(&r, c, view);
    if (on) {
        float t = (timer_ticks() - d->t0) / 1000.0f;
        r.view_m = m4_mul(m4_translate(0, -0.4f, -7.5f), m4_rotate_x(0.32f));
        r.lighting = d->lighting; r.bilinear = d->filtering;
        /* floor */
        r.texture = d->textures ? d->floor_tex : NULL;
        r.model = m4_identity();
        /* tile the floor texture by drawing a 6x6 grid of quads */
        for (int gz = 0; gz < 6; gz++) for (int gx = 0; gx < 6; gx++) {
            float x0 = -6 + gx * 2, z0 = 4 - gz * 2;
            r3d_vertex_t q[4] = {
                { x0, -1.6f, z0, 0, 1, 0, 1, 0, 0xFFFFFFFFu }, { x0 + 2, -1.6f, z0, 1, 1, 0, 1, 0, 0xFFFFFFFFu },
                { x0 + 2, -1.6f, z0 - 2, 1, 0, 0, 1, 0, 0xFFFFFFFFu }, { x0, -1.6f, z0 - 2, 0, 0, 0, 1, 0, 0xFFFFFFFFu } };
            if (!d->textures) for (int k = 0; k < 4; k++) q[k].color = ((gx + gz) & 1) ? 0xFFDCE4F0u : 0xFF8FA3C2u;
            r3d_quad(&r, q);
        }
        /* the cube */
        r.texture = d->textures ? d->globe_tex : NULL;
        r.model = m4_mul(m4_translate(-1.5f, 0, 0), m4_mul(m4_rotate_y(t * 0.9f), m4_rotate_x(t * 0.5f)));
        cube(&r);
        /* the sphere */
        r.texture = NULL;
        r.model = m4_mul(m4_translate(1.8f, 0.1f + 0.3f * r3d_sin(t * 2), -0.5f), m4_rotate_y(-t * 0.7f));
        if (d->sphere) r3d_triangles(&r, d->sphere, d->nsphere);
        d->tris = r.triangles; d->pix = r.pixels;
    } else {
        int bw = MIN(440, view.w - 40), bx = (view.w - bw) / 2, by = view.h / 2 - 70;
        fill_round_rect(c, bx, by, bw, 140, 10, T.window_bg);
        draw_text(c, FONT_LARGE_BOLD, bx + 20, by + 18, "3D acceleration is turned off", T.text);
        draw_text_wrapped(c, FONT_UI, bx + 20, by + 48, bw - 40, "Turn it on to draw this scene with the WRLD 3D engine. The setting is also in Control Panel > Visual Effects.", T.gray_text, 2);
        if (ui_button(w, R(bx + 20, by + 98, 200, 28), "Turn on 3D acceleration")) { accel3d = true; settings_save(); wm_effects_changed(); }
    }
    /* the bar */
    int by = c->h - BAR_H;
    fill_rect(c, 0, by, c->w, BAR_H, T.face);
    hline(c, 0, by, c->w, T.face_dark);
    d->frames++;
    uint32_t now = timer_ticks();
    if (now - d->fps_t >= 1000) { d->fps = d->frames * 1000 / MAX(1, now - d->fps_t); d->frames = 0; d->fps_t = now; }
    char b[160];
    if (on) snprintf(b, sizeof b, "%u frames/s   %u triangles   %u pixels per frame", d->fps, d->tris, d->pix);
    else snprintf(b, sizeof b, "3D acceleration: off");
    draw_text(c, FONT_UI_BOLD, 10, by + 8, b, T.text);
    draw_text_ellipsis(c, FONT_SMALL, 10, by + 28, c->w - 20, r3d_renderer(), T.gray_text);
    int x = c->w - 330;
    if (x > 300) {
        ui_checkbox(w, x, by + 8, "Lighting", &d->lighting);
        ui_checkbox(w, x + 100, by + 8, "Textures", &d->textures);
        ui_checkbox(w, x + 200, by + 8, "Filtering", &d->filtering);
    }
}
static void d3_tick(window_t *w) { w->dirty = true; }
static void d3_destroy(window_t *w) {
    d3_t *d = w->data;
    if (d->globe_tex) canvas_free(d->globe_tex);
    if (d->floor_tex) canvas_free(d->floor_tex);
    kfree(d->sphere); kfree(d);
}
void demo3d_open(void) {
    window_t *w = wm_create("WRLD 3D", -1, -1, MIN(720, SCREEN_W - 40), MIN(520, SCREEN_H - 80), WF_RESIZABLE);
    d3_t *d = kzalloc(sizeof *d);
    d->globe_tex = make_globe_tex(); d->floor_tex = make_floor_tex();
    make_sphere(d);
    d->t0 = d->fps_t = timer_ticks();
    d->lighting = d->textures = d->filtering = true;
    w->data = d; w->paint = d3_paint; w->destroy = d3_destroy;
    w->icon = ICON_CP_DISPLAY; w->min_w = 420; w->min_h = 320;
    wm_set_timer(w, 16, d3_tick);
    wm_center(w);
}
