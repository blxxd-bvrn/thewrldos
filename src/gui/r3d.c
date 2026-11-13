/* WRLD 3D: a software 3D pipeline (see r3d.h).
 *
 * Vertices are transformed by model, view and projection, lit per vertex, then
 * each triangle is clipped against the near plane (Sutherland-Hodgman), divided
 * by w and mapped to the viewport. Rasterisation walks the triangle's bounding
 * box row by row with edge functions; attributes are interpolated as attr/w and
 * 1/w so textures stay straight in perspective, and the depth buffer keeps 1/w
 * (larger is nearer). */
#include "kernel.h"
#include "gfx.h"
#include "r3d.h"

bool accel3d = true;

/* ---------------- maths (the kernel has no libm) ---------------- */
#define PI_F 3.14159265f
float r3d_sin(float x) {
    while (x > PI_F) x -= 2 * PI_F;
    while (x < -PI_F) x += 2 * PI_F;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
float r3d_cos(float x) { return r3d_sin(x + PI_F / 2); }
float r3d_sqrt(float x) {
    if (x <= 0) return 0;
    float r = x > 1 ? x / 2 : 1;
    for (int i = 0; i < 16; i++) r = 0.5f * (r + x / r);
    return r;
}
mat4 m4_identity(void) { mat4 m = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } }; return m; }
mat4 m4_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
mat4 m4_translate(float x, float y, float z) { mat4 m = m4_identity(); m.m[12] = x; m.m[13] = y; m.m[14] = z; return m; }
mat4 m4_scale(float x, float y, float z) { mat4 m = m4_identity(); m.m[0] = x; m.m[5] = y; m.m[10] = z; return m; }
mat4 m4_rotate_x(float a) { mat4 m = m4_identity(); float c = r3d_cos(a), s = r3d_sin(a); m.m[5] = c; m.m[6] = s; m.m[9] = -s; m.m[10] = c; return m; }
mat4 m4_rotate_y(float a) { mat4 m = m4_identity(); float c = r3d_cos(a), s = r3d_sin(a); m.m[0] = c; m.m[2] = -s; m.m[8] = s; m.m[10] = c; return m; }
mat4 m4_rotate_z(float a) { mat4 m = m4_identity(); float c = r3d_cos(a), s = r3d_sin(a); m.m[0] = c; m.m[1] = s; m.m[4] = -s; m.m[5] = c; return m; }
mat4 m4_perspective(float fovy, float aspect, float n, float f) {
    float t = r3d_sin(fovy / 2) / r3d_cos(fovy / 2), fl = 1 / t;
    mat4 m = { { 0 } };
    m.m[0] = fl / aspect; m.m[5] = fl;
    m.m[10] = (f + n) / (n - f); m.m[11] = -1;
    m.m[14] = 2 * f * n / (n - f);
    return m;
}

/* ---------------- state ---------------- */
static float *zbuf;
static int zcap;
const char *r3d_renderer(void) {
    return ARCH_BITS == 64 ? "WRLD 3D software renderer (x64, SSE floating point)" : "WRLD 3D software renderer (x86, x87 floating point)";
}
static void defaults(r3d_t *r) {
    r->proj = m4_perspective(PI_F / 3, (float)r->view.w / r->view.h, 0.1f, 100);
    r->view_m = m4_identity(); r->model = m4_identity();
    r->light[0] = -0.4f; r->light[1] = 0.6f; r->light[2] = 0.7f;
    float l = r3d_sqrt(r->light[0] * r->light[0] + r->light[1] * r->light[1] + r->light[2] * r->light[2]);
    for (int i = 0; i < 3; i++) r->light[i] /= l;
    r->ambient = 0.35f; r->diffuse = 0.75f;
    r->bilinear = true; r->depth_test = r->depth_write = true; r->cull_back = true;
    r->alpha = 255;
}
bool r3d_begin_with(r3d_t *r, canvas_t *target, rect_t view, float *depth, bool clear) {
    if (!accel3d || !depth) return false;
    memset(r, 0, sizeof *r);
    r->target = target;
    r->view = rect_intersect(view, R(0, 0, target->w, target->h));
    if (r->view.w <= 0 || r->view.h <= 0 || r->view.w != view.w || r->view.h != view.h) return false;
    r->depth = depth;
    defaults(r);
    if (clear) r3d_clear_depth(r);
    return true;
}
bool r3d_begin(r3d_t *r, canvas_t *target, rect_t view) {
    if (!accel3d) return false;
    memset(r, 0, sizeof *r);
    r->target = target;
    r->view = rect_intersect(view, R(0, 0, target->w, target->h));
    if (r->view.w <= 0 || r->view.h <= 0) return false;
    int need = r->view.w * r->view.h;
    if (need > zcap) {
        float *nz = krealloc(zbuf, sizeof(float) * need);
        if (!nz) return false;
        zbuf = nz; zcap = need;
    }
    r->depth = zbuf;
    defaults(r);
    r3d_clear_depth(r);
    return true;
}
void r3d_clear_depth(r3d_t *r) { for (int i = 0; i < r->view.w * r->view.h; i++) r->depth[i] = 0; }

/* ---------------- pipeline ---------------- */
typedef struct { float x, y, z, w; float u, v; float r, g, b, a; } cv_t;   /* clip-space vertex with attributes */

static cv_t transform(r3d_t *r, const mat4 *mvp, const r3d_vertex_t *v) {
    const float *m = mvp->m;
    cv_t o;
    float W = v->w != 0 ? v->w : 1;
    o.x = m[0] * v->x + m[4] * v->y + m[8] * v->z + m[12] * W;
    o.y = m[1] * v->x + m[5] * v->y + m[9] * v->z + m[13] * W;
    o.z = m[2] * v->x + m[6] * v->y + m[10] * v->z + m[14] * W;
    o.w = m[3] * v->x + m[7] * v->y + m[11] * v->z + m[15] * W;
    o.u = v->u; o.v = v->v;
    float k = 1;
    if (r->lighting) {                                        /* normal through the model matrix (no scaling assumed) */
        const float *mm = r->model.m;
        float nx = mm[0] * v->nx + mm[4] * v->ny + mm[8] * v->nz;
        float ny = mm[1] * v->nx + mm[5] * v->ny + mm[9] * v->nz;
        float nz = mm[2] * v->nx + mm[6] * v->ny + mm[10] * v->nz;
        float l = r3d_sqrt(nx * nx + ny * ny + nz * nz);
        if (l > 0) { nx /= l; ny /= l; nz /= l; }
        float d = nx * r->light[0] + ny * r->light[1] + nz * r->light[2];
        k = r->ambient + (d > 0 ? d : 0) * r->diffuse;
        if (k > 1.25f) k = 1.25f;
    }
    o.r = C_R(v->color) / 255.0f * k; o.g = C_G(v->color) / 255.0f * k; o.b = C_B(v->color) / 255.0f * k;
    o.a = C_A(v->color) / 255.0f;
    return o;
}
static cv_t lerp(const cv_t *a, const cv_t *b, float t) {
    cv_t o;
    o.x = a->x + (b->x - a->x) * t; o.y = a->y + (b->y - a->y) * t; o.z = a->z + (b->z - a->z) * t; o.w = a->w + (b->w - a->w) * t;
    o.u = a->u + (b->u - a->u) * t; o.v = a->v + (b->v - a->v) * t;
    o.r = a->r + (b->r - a->r) * t; o.g = a->g + (b->g - a->g) * t; o.b = a->b + (b->b - a->b) * t; o.a = a->a + (b->a - a->a) * t;
    return o;
}

/* texel fetch; u, v in 16.16 texels (already scaled by the texture size) */
static inline uint32_t fetch_nearest(const canvas_t *t, int32_t u, int32_t v) {
    int x = u >> 16, y = v >> 16;
    x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x; y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y;
    return t->px[y * t->stride + x];
}
static inline uint32_t fetch_bilinear(const canvas_t *t, int32_t u, int32_t v) {
    u -= 0x8000; v -= 0x8000;                                  /* texel centres */
    int x0 = u >> 16, y0 = v >> 16, ax = (u >> 8) & 255, ay = (v >> 8) & 255;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : x0 >= t->w ? t->w - 1 : x0; x1 = x1 < 0 ? 0 : x1 >= t->w ? t->w - 1 : x1;
    y0 = y0 < 0 ? 0 : y0 >= t->h ? t->h - 1 : y0; y1 = y1 < 0 ? 0 : y1 >= t->h ? t->h - 1 : y1;
    const uint32_t *r0 = t->px + y0 * t->stride, *r1 = t->px + y1 * t->stride;
    uint32_t c00 = r0[x0], c10 = r0[x1], c01 = r1[x0], c11 = r1[x1];
    /* two channels at a time: (A,G) and (R,B) */
    uint32_t ag00 = (c00 >> 8) & 0x00FF00FF, ag10 = (c10 >> 8) & 0x00FF00FF, ag01 = (c01 >> 8) & 0x00FF00FF, ag11 = (c11 >> 8) & 0x00FF00FF;
    uint32_t rb00 = c00 & 0x00FF00FF, rb10 = c10 & 0x00FF00FF, rb01 = c01 & 0x00FF00FF, rb11 = c11 & 0x00FF00FF;
    uint32_t agt = (ag00 + (((ag10 - ag00) * ax) >> 8)) & 0x00FF00FF, agb = (ag01 + (((ag11 - ag01) * ax) >> 8)) & 0x00FF00FF;
    uint32_t rbt = (rb00 + (((rb10 - rb00) * ax) >> 8)) & 0x00FF00FF, rbb = (rb01 + (((rb11 - rb01) * ax) >> 8)) & 0x00FF00FF;
    uint32_t ag = (agt + (((agb - agt) * ay) >> 8)) & 0x00FF00FF, rb = (rbt + (((rbb - rbt) * ay) >> 8)) & 0x00FF00FF;
    return (ag << 8) | rb;
}

typedef struct { float sx, sy, iz, uw, vw, rw, gw, bw, aw; } sv_t;   /* screen vertex: attributes divided by w */
#define NATTR 7                                                   /* iz, uw, vw, rw, gw, bw, aw */
#define SPAN 16                                                   /* pixels between perspective divisions */

/* Exact spans per row from the three edges; every attribute is a plane over the
 * screen (attr/w is linear in screen space), stepped by adds. The perspective
 * division happens every SPAN pixels, with linear steps between. */
static void raster(r3d_t *r, const sv_t *a, const sv_t *b, const sv_t *c) {
    float area = (b->sx - a->sx) * (c->sy - a->sy) - (b->sy - a->sy) * (c->sx - a->sx);
    if (area > -1e-6f && area < 1e-6f) return;
    if (r->cull_back && (r->front_cw ? area < 0 : area > 0)) return;   /* screen y points down: counter-clockwise faces are negative */
    canvas_t *t = r->target;
    rect_t cl = rect_intersect(r->view, t->clip);
    float miny = MIN(a->sy, MIN(b->sy, c->sy)), maxy = MAX(a->sy, MAX(b->sy, c->sy));
    float minx = MIN(a->sx, MIN(b->sx, c->sx)), maxx = MAX(a->sx, MAX(b->sx, c->sx));
    int y0 = MAX(cl.y, (int)miny), y1 = MIN(cl.y + cl.h - 1, (int)maxy + 1);
    int cx0 = MAX(cl.x, (int)minx), cx1 = MIN(cl.x + cl.w - 1, (int)maxx + 1);
    if (y0 > y1 || cx0 > cx1) return;
    r->triangles++;
    float inv = 1.0f / area;
    /* attribute planes: A(x, y) = A_a + dAdx (x - a.sx) + dAdy (y - a.sy) */
    const float *va = &a->iz, *vb = &b->iz, *vc = &c->iz;
    float dx[NATTR], dy[NATTR];
    for (int k = 0; k < NATTR; k++) {
        float e1 = vb[k] - va[k], e2 = vc[k] - va[k];
        dx[k] = (e1 * (c->sy - a->sy) - e2 * (b->sy - a->sy)) * inv;
        dy[k] = (e2 * (b->sx - a->sx) - e1 * (c->sx - a->sx)) * inv;
    }
    /* edges as x-ranges: for edge (p, q) the inside is where E(x) = (q.sx - p.sx)(y - p.sy) - (q.sy - p.sy)(x - p.sx) has the sign of -area */
    const sv_t *E[3][2] = { { a, b }, { b, c }, { c, a } };
    const canvas_t *tex = r->texture;
    float tw = tex ? (float)tex->w * 65536.0f : 0, th = tex ? (float)tex->h * 65536.0f : 0;
    int galpha = r->alpha;
    float sgn = area > 0 ? 1.0f : -1.0f;                         /* E(opposite vertex) = area: inside has its sign */
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f, lo = cx0, hi = cx1 + 1;
        for (int e = 0; e < 3; e++) {
            const sv_t *p = E[e][0], *q = E[e][1];
            /* sgn * E(x) >= 0  <=>  sgn * (A + B x) >= 0 with B = -(q.sy - p.sy) */
            float A = (q->sx - p->sx) * (py - p->sy) + (q->sy - p->sy) * p->sx, B = -(q->sy - p->sy);
            A *= sgn; B *= sgn;
            if (B > 1e-9f) { float xl = -A / B; if (xl > lo) lo = xl; }
            else if (B < -1e-9f) { float xr = -A / B; if (xr < hi) hi = xr; }
            else if (A < 0) { lo = hi; break; }
        }
        float fl = lo - 0.5f, fh = hi - 0.5f;                     /* pixel centres x + 0.5 inside [lo, hi] */
        int xs = (int)fl; if (xs < fl) xs++;                      /* ceil */
        int xe = (int)fh; if (xe > fh) xe--;                      /* floor */
        if (xs < cx0) xs = cx0;
        if (xe > cx1) xe = cx1;
        if (xs > xe) continue;
        uint32_t *row = t->px + (size_t)y * t->stride;
        float *zrow = r->depth + (size_t)(y - r->view.y) * r->view.w - r->view.x;
        float px0 = xs + 0.5f;
        float at[NATTR];
        for (int k = 0; k < NATTR; k++) at[k] = va[k] + dx[k] * (px0 - a->sx) + dy[k] * (py - a->sy);
        int x = xs;
        while (x <= xe) {
            int n = MIN(SPAN, xe - x + 1);
            /* perspective-correct values at both ends of the span */
            float z0 = 1.0f / at[0], iz1 = at[0] + dx[0] * n, z1 = 1.0f / iz1;
            float u0 = at[1] * z0 * tw, v0 = at[2] * z0 * th;
            float u1 = (at[1] + dx[1] * n) * z1 * tw, v1 = (at[2] + dx[2] * n) * z1 * th;
            float r0 = at[3] * z0, g0 = at[4] * z0, b0 = at[5] * z0, a0 = at[6] * z0;
            float r1 = (at[3] + dx[3] * n) * z1, g1 = (at[4] + dx[4] * n) * z1, b1 = (at[5] + dx[5] * n) * z1, a1 = (at[6] + dx[6] * n) * z1;
            int32_t U = (int32_t)u0, V = (int32_t)v0, dU = (int32_t)((u1 - u0) / n), dV = (int32_t)((v1 - v0) / n);
            /* colour factors in 8.8 fixed point (1.0 = 256) */
            int32_t CR = (int32_t)(r0 * 65536), CG = (int32_t)(g0 * 65536), CB = (int32_t)(b0 * 65536), CA = (int32_t)(a0 * 65536);
            int32_t dCR = (int32_t)((r1 - r0) * 65536 / n), dCG = (int32_t)((g1 - g0) * 65536 / n), dCB = (int32_t)((b1 - b0) * 65536 / n), dCA = (int32_t)((a1 - a0) * 65536 / n);
            float iz = at[0], diz = dx[0];
            for (int i = 0; i < n; i++, x++, iz += diz, U += dU, V += dV, CR += dCR, CG += dCG, CB += dCB, CA += dCA) {
                if (r->depth_test && iz <= zrow[x]) continue;
                uint32_t col = tex ? (r->bilinear ? fetch_bilinear(tex, U, V) : fetch_nearest(tex, U, V)) : 0xFFFFFFFFu;
                int R_ = (int)((C_R(col) * (int64_t)CR) >> 16), G_ = (int)((C_G(col) * (int64_t)CG) >> 16), B_ = (int)((C_B(col) * (int64_t)CB) >> 16);
                int A_ = (int)(((C_A(col) * (int64_t)CA) >> 16) * galpha / 255);
                R_ = R_ > 255 ? 255 : R_ < 0 ? 0 : R_; G_ = G_ > 255 ? 255 : G_ < 0 ? 0 : G_; B_ = B_ > 255 ? 255 : B_ < 0 ? 0 : B_;
                if (A_ <= 0) continue;
                if (A_ >= 255) row[x] = 0xFF000000u | (R_ << 16) | (G_ << 8) | B_;
                else {
                    uint32_t d = row[x];
                    int ia = 255 - A_;
                    row[x] = 0xFF000000u | (((R_ * A_ + C_R(d) * ia) / 255) << 16) | (((G_ * A_ + C_G(d) * ia) / 255) << 8) | ((B_ * A_ + C_B(d) * ia) / 255);
                }
                if (r->depth_write) zrow[x] = iz;
                r->pixels++;
            }
            for (int k = 0; k < NATTR; k++) at[k] += dx[k] * n;
        }
    }
}
static sv_t to_screen(r3d_t *r, const cv_t *v) {
    sv_t s;
    float iw = 1.0f / v->w;
    s.sx = r->view.x + (v->x * iw * 0.5f + 0.5f) * r->view.w;
    s.sy = r->view.y + (0.5f - v->y * iw * 0.5f) * r->view.h;
    s.iz = iw;
    s.uw = v->u * iw; s.vw = v->v * iw;
    s.rw = v->r * iw; s.gw = v->g * iw; s.bw = v->b * iw; s.aw = v->a * iw;
    return s;
}
void r3d_triangles(r3d_t *r, const r3d_vertex_t *v, int count) {
    if (!r || !r->target) return;
    mat4 mvp = m4_mul(r->proj, m4_mul(r->view_m, r->model));
    for (int i = 0; i + 2 < count; i += 3) {
        cv_t in[3] = { transform(r, &mvp, &v[i]), transform(r, &mvp, &v[i + 1]), transform(r, &mvp, &v[i + 2]) };
        /* clip against the near plane (w > 0.1); triangles wholly outside the sides are dropped */
        bool out_l = true, out_r = true, out_t = true, out_b = true;
        for (int k = 0; k < 3; k++) {
            if (in[k].x >= -in[k].w) out_l = false;
            if (in[k].x <= in[k].w) out_r = false;
            if (in[k].y >= -in[k].w) out_b = false;
            if (in[k].y <= in[k].w) out_t = false;
        }
        if (out_l || out_r || out_t || out_b) continue;
        cv_t poly[4]; int n = 0;
        const float NEAR = 0.1f;
        for (int k = 0; k < 3; k++) {
            const cv_t *p = &in[k], *q = &in[(k + 1) % 3];
            bool pin = p->w >= NEAR, qin = q->w >= NEAR;
            if (pin) poly[n++] = *p;
            if (pin != qin && n < 4) poly[n++] = lerp(p, q, (NEAR - p->w) / (q->w - p->w));
        }
        if (n < 3) continue;
        sv_t s[4];
        for (int k = 0; k < n; k++) s[k] = to_screen(r, &poly[k]);
        raster(r, &s[0], &s[1], &s[2]);
        if (n == 4) raster(r, &s[0], &s[2], &s[3]);
    }
}
void r3d_quad(r3d_t *r, const r3d_vertex_t q[4]) {
    r3d_vertex_t t[6] = { q[0], q[1], q[2], q[0], q[2], q[3] };
    r3d_triangles(r, t, 6);
}
