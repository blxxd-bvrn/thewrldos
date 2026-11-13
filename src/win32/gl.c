/* opengl32.dll, glu32.dll and the WGL/pixel-format part of gdi32.dll for Win32
 * programs, drawn by the WRLD 3D engine (src/gui/r3d.c).
 *
 * This is OpenGL 1.1, the fixed-function API: glBegin/glEnd, vertex arrays,
 * display lists of geometry, the matrix stacks, one directional light with
 * colour material, 2D textures (RGB/RGBA/BGRA/luminance bytes), depth test,
 * back-face culling and alpha blending. A context renders into its own back
 * buffer and depth buffer; SwapBuffers copies the frame into the window.
 * Shaders (OpenGL 2.0 and later) are not available: wglGetProcAddress returns
 * NULL for them, as on a 1.1 driver. */
#include "w32.h"
#include "r3d.h"

uintptr_t w32_dc_hwnd(uintptr_t hdc);
bool w32_hwnd_client(uintptr_t hwnd, canvas_t **surf, rect_t *client);

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef unsigned char GLboolean, GLubyte;

#define GLMAGIC 0x474C4358
enum { EN_DEPTH = 1, EN_LIGHTING = 2, EN_TEX2D = 4, EN_BLEND = 8, EN_CULL = 16, EN_LIGHT0 = 32, EN_COLORMAT = 64 };
enum { CS_VERTEX = 1, CS_NORMAL = 2, CS_COLOR = 4, CS_TEX = 8 };
typedef struct { GLint size; GLenum type; GLsizei stride; const void *ptr; } arr_t;
typedef struct { int prim; GLuint tex; r3d_vertex_t *v; int n; } batch_t;
typedef struct { batch_t *b; int n, cap; bool used; } dlist_t;
typedef struct { canvas_t *c; bool linear; bool used; } gltex_t;
#define NTEX 512
#define NLIST 256

typedef struct glctx {
    uint32_t magic;
    uintptr_t hwnd;
    canvas_t *back; float *depth; int bw, bh;
    mat4 mv[32], pj[4], tx[4]; int mvd, pjd, txd; GLenum mode;
    float col[4], nrm[3], tc[2];
    int prim; r3d_vertex_t *vb; int vn, vcap;
    uint32_t en; GLenum cull_face, front_face; bool depth_mask;
    float clear[4];
    int vx, vy, vw, vh;
    float light_dir[3], light_amb, light_dif, model_amb;
    float mat_dif[4];
    gltex_t tex[NTEX]; GLuint bound;
    uint32_t cs; arr_t va, na, ca, ta;
    dlist_t lists[NLIST]; int compiling; bool compile_exec;
    GLenum error;
    bool doublebuf;
} glctx_t;

/* the current context of each task (a program's threads each have their own) */
static struct { task_t *t; glctx_t *c; } current[32];
static glctx_t *CUR(void) { task_t *t = task_current(); for (int i = 0; i < 32; i++) if (current[i].t == t) return current[i].c; return NULL; }
static void set_current(glctx_t *c) {
    task_t *t = task_current(); int free_ = -1;
    for (int i = 0; i < 32; i++) { if (current[i].t == t) { current[i].c = c; if (!c) current[i].t = NULL; return; } if (!current[i].t && free_ < 0) free_ = i; }
    if (c && free_ >= 0) { current[free_].t = t; current[free_].c = c; }
}
#define G glctx_t *g = CUR(); if (!g) return
#define GV(v) glctx_t *g = CUR(); if (!g) return v
static void err(glctx_t *g, GLenum e) { if (!g->error) g->error = e; }

/* ---------------- buffers ---------------- */
static bool ensure_buffers(glctx_t *g) {
    canvas_t *surf; rect_t cr;
    if (!w32_hwnd_client(g->hwnd, &surf, &cr)) return false;
    int w = MAX(1, cr.w), h = MAX(1, cr.h);
    if (g->back && g->bw == w && g->bh == h) return true;
    if (g->back) canvas_free(g->back);
    kfree(g->depth);
    g->back = canvas_new(w, h);
    g->depth = kzalloc(sizeof(float) * w * h);
    g->bw = w; g->bh = h;
    if (!g->back || !g->depth) return false;
    gfx_clear(g->back, BLACK);
    if (!g->vw) { g->vx = 0; g->vy = 0; g->vw = w; g->vh = h; }
    return true;
}
static void present(glctx_t *g) {
    canvas_t *surf; rect_t cr;
    if (!g->back || !w32_hwnd_client(g->hwnd, &surf, &cr)) return;
    blit(surf, cr.x, cr.y, g->back, 0, 0, MIN(cr.w, g->bw), MIN(cr.h, g->bh));
    wm_need_redraw = true;
}

/* ---------------- matrices ---------------- */
static mat4 *top(glctx_t *g) { return g->mode == 0x1701 ? &g->pj[g->pjd] : g->mode == 0x1702 ? &g->tx[g->txd] : &g->mv[g->mvd]; }
static void mult(glctx_t *g, mat4 m) { mat4 *t = top(g); *t = m4_mul(*t, m); }
static void WINAPI glMatrixMode(GLenum m) { G; g->mode = m; }
static void WINAPI glLoadIdentity(void) { G; *top(g) = m4_identity(); }
static void WINAPI glLoadMatrixf(const GLfloat *m) { G; memcpy(top(g)->m, m, 64); }
static void WINAPI glLoadMatrixd(const GLdouble *m) { G; for (int i = 0; i < 16; i++) top(g)->m[i] = (float)m[i]; }
static void WINAPI glMultMatrixf(const GLfloat *m) { G; mat4 x; memcpy(x.m, m, 64); mult(g, x); }
static void WINAPI glMultMatrixd(const GLdouble *m) { G; mat4 x; for (int i = 0; i < 16; i++) x.m[i] = (float)m[i]; mult(g, x); }
static void WINAPI glPushMatrix(void) {
    G;
    int *d = g->mode == 0x1701 ? &g->pjd : g->mode == 0x1702 ? &g->txd : &g->mvd, lim = g->mode == 0x1700 ? 31 : 3;
    mat4 *s = g->mode == 0x1701 ? g->pj : g->mode == 0x1702 ? g->tx : g->mv;
    if (*d >= lim) { err(g, 0x0503); return; }                   /* GL_STACK_OVERFLOW */
    s[*d + 1] = s[*d]; (*d)++;
}
static void WINAPI glPopMatrix(void) {
    G;
    int *d = g->mode == 0x1701 ? &g->pjd : g->mode == 0x1702 ? &g->txd : &g->mvd;
    if (*d <= 0) { err(g, 0x0504); return; }                     /* GL_STACK_UNDERFLOW */
    (*d)--;
}
static void WINAPI glTranslatef(GLfloat x, GLfloat y, GLfloat z) { G; mult(g, m4_translate(x, y, z)); }
static void WINAPI glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }
static void WINAPI glScalef(GLfloat x, GLfloat y, GLfloat z) { G; mult(g, m4_scale(x, y, z)); }
static void WINAPI glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }
static void WINAPI glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
    G;
    float l = r3d_sqrt(x * x + y * y + z * z); if (l <= 0) return;
    x /= l; y /= l; z /= l;
    float r = a * 3.14159265f / 180, c = r3d_cos(r), s = r3d_sin(r), t = 1 - c;
    mat4 m = m4_identity();
    m.m[0] = t * x * x + c;     m.m[4] = t * x * y - s * z; m.m[8] = t * x * z + s * y;
    m.m[1] = t * x * y + s * z; m.m[5] = t * y * y + c;     m.m[9] = t * y * z - s * x;
    m.m[2] = t * x * z - s * y; m.m[6] = t * y * z + s * x; m.m[10] = t * z * z + c;
    mult(g, m);
}
static void WINAPI glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)a, (float)x, (float)y, (float)z); }
static void WINAPI glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    G;
    mat4 m = { { 0 } };
    m.m[0] = (float)(2 * n / (r - l)); m.m[5] = (float)(2 * n / (t - b));
    m.m[8] = (float)((r + l) / (r - l)); m.m[9] = (float)((t + b) / (t - b)); m.m[10] = (float)(-(f + n) / (f - n)); m.m[11] = -1;
    m.m[14] = (float)(-2 * f * n / (f - n));
    mult(g, m);
}
static void WINAPI glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    G;
    mat4 m = m4_identity();
    m.m[0] = (float)(2 / (r - l)); m.m[5] = (float)(2 / (t - b)); m.m[10] = (float)(-2 / (f - n));
    m.m[12] = (float)(-(r + l) / (r - l)); m.m[13] = (float)(-(t + b) / (t - b)); m.m[14] = (float)(-(f + n) / (f - n));
    mult(g, m);
}
static void WINAPI glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { G; g->vx = x; g->vy = y; g->vw = w; g->vh = h; }

/* ---------------- state ---------------- */
static uint32_t en_bit(GLenum cap) {
    switch (cap) {
    case 0x0B71: return EN_DEPTH; case 0x0B50: return EN_LIGHTING; case 0x0DE1: return EN_TEX2D; case 0x0BE2: return EN_BLEND;
    case 0x0B44: return EN_CULL; case 0x4000: return EN_LIGHT0; case 0x0B57: return EN_COLORMAT;
    }
    return 0;                                                       /* fog, normalize, dithering...: accepted, no effect */
}
static void WINAPI glEnable(GLenum cap) { G; g->en |= en_bit(cap); }
static void WINAPI glDisable(GLenum cap) { G; g->en &= ~en_bit(cap); }
static GLboolean WINAPI glIsEnabled(GLenum cap) { GV(0); return (g->en & en_bit(cap)) != 0; }
static void WINAPI glCullFace(GLenum m) { G; g->cull_face = m; }
static void WINAPI glFrontFace(GLenum m) { G; g->front_face = m; }
static void WINAPI glDepthMask(GLboolean f) { G; g->depth_mask = f; }
static void WINAPI glDepthFunc(GLenum f) { (void)f; }
static void WINAPI glShadeModel(GLenum m) { (void)m; }
static void WINAPI glHint(GLenum a, GLenum b) { (void)a; (void)b; }
static void WINAPI glBlendFunc(GLenum a, GLenum b) { (void)a; (void)b; }       /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
static void WINAPI glAlphaFunc(GLenum f, GLclampf r) { (void)f; (void)r; }
static void WINAPI glPolygonMode(GLenum a, GLenum b) { (void)a; (void)b; }
static void WINAPI glLineWidth(GLfloat w) { (void)w; }
static void WINAPI glPointSize(GLfloat s) { (void)s; }
static void WINAPI glPixelStorei(GLenum a, GLint b) { (void)a; (void)b; }
static void WINAPI glClearDepth(GLclampd d) { (void)d; }
static void WINAPI glClearColor(GLclampf r, GLclampf gg, GLclampf b, GLclampf a) { G; g->clear[0] = r; g->clear[1] = gg; g->clear[2] = b; g->clear[3] = a; }
static uint32_t to_argb(const float *c) {
    int r = (int)(c[0] * 255 + 0.5f), gg = (int)(c[1] * 255 + 0.5f), b = (int)(c[2] * 255 + 0.5f), a = (int)(c[3] * 255 + 0.5f);
    r = CLAMP(r, 0, 255); gg = CLAMP(gg, 0, 255); b = CLAMP(b, 0, 255); a = CLAMP(a, 0, 255);
    return ((uint32_t)a << 24) | (r << 16) | (gg << 8) | b;
}
static void WINAPI glClear(GLbitfield m) {
    G;
    if (!ensure_buffers(g)) return;
    if (m & 0x4000) gfx_clear(g->back, to_argb(g->clear) | 0xFF000000u);
    if (m & 0x0100) memset(g->depth, 0, sizeof(float) * g->bw * g->bh);
}
static void WINAPI glLightfv(GLenum light, GLenum p, const GLfloat *v) {
    G;
    if (light != 0x4000) return;                                    /* one light: GL_LIGHT0 */
    if (p == 0x1203) {                                              /* GL_POSITION: to eye space now, as GL does */
        const float *m = g->mv[g->mvd].m;
        float x = m[0] * v[0] + m[4] * v[1] + m[8] * v[2], y = m[1] * v[0] + m[5] * v[1] + m[9] * v[2], z = m[2] * v[0] + m[6] * v[1] + m[10] * v[2];
        float l = r3d_sqrt(x * x + y * y + z * z);
        if (l > 0) { g->light_dir[0] = x / l; g->light_dir[1] = y / l; g->light_dir[2] = z / l; }
    } else if (p == 0x1200) g->light_amb = (v[0] + v[1] + v[2]) / 3;
    else if (p == 0x1201) g->light_dif = (v[0] + v[1] + v[2]) / 3;
}
static void WINAPI glLightf(GLenum l, GLenum p, GLfloat v) { (void)l; (void)p; (void)v; }
static void WINAPI glLightModelfv(GLenum p, const GLfloat *v) { G; if (p == 0x0B53) g->model_amb = (v[0] + v[1] + v[2]) / 3; }
static void WINAPI glLightModeli(GLenum p, GLint v) { (void)p; (void)v; }
static void WINAPI glMaterialfv(GLenum face, GLenum p, const GLfloat *v) { G; (void)face; if (p == 0x1201 || p == 0x1602) memcpy(g->mat_dif, v, 16); }
static void WINAPI glMaterialf(GLenum face, GLenum p, GLfloat v) { (void)face; (void)p; (void)v; }
static void WINAPI glColorMaterial(GLenum face, GLenum mode) { (void)face; (void)mode; }
static GLenum WINAPI glGetError(void) { GV(0); GLenum e = g->error; g->error = 0; return e; }
static const GLubyte *WINAPI glGetString(GLenum n) {
    switch (n) {
    case 0x1F00: return (const GLubyte *)"THE WRLD OS";
    case 0x1F01: return (const GLubyte *)"WRLD 3D (software)";
    case 0x1F02: return (const GLubyte *)"1.1 WRLD 3D";
    case 0x1F03: return (const GLubyte *)"GL_EXT_bgra";
    }
    return NULL;
}
static void WINAPI glGetIntegerv(GLenum n, GLint *v) {
    G;
    if (n == 0x0BA2) { v[0] = g->vx; v[1] = g->vy; v[2] = g->vw; v[3] = g->vh; }
    else if (n == 0x0D33) v[0] = 2048;
    else if (n == 0x0BA0) v[0] = g->mode;
    else v[0] = 0;
}
static void WINAPI glGetFloatv(GLenum n, GLfloat *v) {
    G;
    if (n == 0x0BA6) memcpy(v, g->mv[g->mvd].m, 64);
    else if (n == 0x0BA7) memcpy(v, g->pj[g->pjd].m, 64);
    else v[0] = 0;
}
static void WINAPI glGetDoublev(GLenum n, GLdouble *v) { float f[16] = { 0 }; glGetFloatv(n, f); for (int i = 0; i < 16; i++) v[i] = f[i]; }
static void WINAPI glFlush(void) { G; if (!g->doublebuf) present(g); task_yield(); }
static void WINAPI glFinish(void) { glFlush(); }

/* ---------------- textures ---------------- */
static void WINAPI glGenTextures(GLsizei n, GLuint *out) {
    G;
    for (int i = 0; i < n; i++) {
        out[i] = 0;
        for (GLuint k = 1; k < NTEX; k++) if (!g->tex[k].used) { g->tex[k].used = true; g->tex[k].linear = true; out[i] = k; break; }
    }
}
static void WINAPI glDeleteTextures(GLsizei n, const GLuint *t) {
    G;
    for (int i = 0; i < n; i++) if (t[i] && t[i] < NTEX) { if (g->tex[t[i]].c) canvas_free(g->tex[t[i]].c); memset(&g->tex[t[i]], 0, sizeof(gltex_t)); }
}
static void WINAPI glBindTexture(GLenum target, GLuint t) { G; (void)target; if (t < NTEX) { g->bound = t; if (t) { g->tex[t].used = true; } } }
static void WINAPI glTexParameteri(GLenum target, GLenum p, GLint v) { G; (void)target; if ((p == 0x2800 || p == 0x2801) && g->bound < NTEX) g->tex[g->bound].linear = v != 0x2600 && v != 0x2700; }
static void WINAPI glTexParameterf(GLenum target, GLenum p, GLfloat v) { glTexParameteri(target, p, (GLint)v); }
static void WINAPI glTexEnvi(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; }
static void WINAPI glTexEnvf(GLenum a, GLenum b, GLfloat c) { (void)a; (void)b; (void)c; }
static void WINAPI glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *px) {
    G; (void)target; (void)ifmt; (void)border;
    if (level != 0) return;                                         /* mipmaps: only the base level is used */
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || g->bound >= NTEX) { err(g, 0x0501); return; }
    if (type != 0x1401) { err(g, 0x0500); return; }                 /* GL_UNSIGNED_BYTE only */
    gltex_t *t = &g->tex[g->bound];
    if (t->c) canvas_free(t->c);
    t->c = canvas_new(w, h); t->used = true;
    if (!t->c || !px) return;
    int bpp = fmt == 0x1908 || fmt == 0x80E1 ? 4 : fmt == 0x1907 || fmt == 0x80E0 ? 3 : fmt == 0x190A ? 2 : 1;
    int rowb = (w * bpp + 3) & ~3;                                  /* GL_UNPACK_ALIGNMENT 4 */
    const uint8_t *src = px;
    /* GL's first row is the bottom of the image, which is texture coordinate v = 0 */
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * rowb;
        uint32_t *d = t->c->px + (size_t)y * t->c->stride;
        for (int x = 0; x < w; x++, s += bpp) {
            uint32_t r, gg, b, a = 255;
            switch (fmt) {
            case 0x1908: r = s[0]; gg = s[1]; b = s[2]; a = s[3]; break;            /* RGBA */
            case 0x80E1: b = s[0]; gg = s[1]; r = s[2]; a = s[3]; break;            /* BGRA */
            case 0x1907: r = s[0]; gg = s[1]; b = s[2]; break;                      /* RGB */
            case 0x80E0: b = s[0]; gg = s[1]; r = s[2]; break;                      /* BGR */
            case 0x190A: r = gg = b = s[0]; a = s[1]; break;                        /* LUMINANCE_ALPHA */
            case 0x1906: r = gg = b = 255; a = s[0]; break;                         /* ALPHA */
            default: r = gg = b = s[0]; break;                                      /* LUMINANCE */
            }
            d[x] = (a << 24) | (r << 16) | (gg << 8) | b;
        }
    }
}
static void WINAPI glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *px) {
    G; (void)target;
    if (level || g->bound >= NTEX || !g->tex[g->bound].c || type != 0x1401) return;
    canvas_t *c = g->tex[g->bound].c;
    int bpp = fmt == 0x1908 || fmt == 0x80E1 ? 4 : fmt == 0x1907 || fmt == 0x80E0 ? 3 : 1;
    int rowb = (w * bpp + 3) & ~3;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int tx = xo + x, ty = yo + y;
        if (tx < 0 || ty < 0 || tx >= c->w || ty >= c->h) continue;
        const uint8_t *s = (const uint8_t *)px + (size_t)y * rowb + x * bpp;
        uint32_t r = s[0], gg = bpp > 1 ? s[1] : s[0], b = bpp > 2 ? s[2] : s[0], a = bpp == 4 ? s[3] : 255;
        if (fmt == 0x80E1 || fmt == 0x80E0) { uint32_t t = r; r = b; b = t; }
        c->px[ty * c->stride + tx] = (a << 24) | (r << 16) | (gg << 8) | b;
    }
}

/* ---------------- drawing ---------------- */
static void draw_lines(glctx_t *g, const r3d_vertex_t *v, int n, int prim) {
    mat4 mvp = m4_mul(g->pj[g->pjd], g->mv[g->mvd]);
    int sx[2048], sy[2048]; bool ok[2048]; n = MIN(n, 2048);
    for (int i = 0; i < n; i++) {
        const float *m = mvp.m;
        float x = m[0] * v[i].x + m[4] * v[i].y + m[8] * v[i].z + m[12], y = m[1] * v[i].x + m[5] * v[i].y + m[9] * v[i].z + m[13];
        float w = m[3] * v[i].x + m[7] * v[i].y + m[11] * v[i].z + m[15];
        ok[i] = w > 0.01f;
        if (!ok[i]) continue;
        sx[i] = g->vx + (int)((x / w * 0.5f + 0.5f) * g->vw);
        sy[i] = g->bh - (g->vy + (int)((y / w * 0.5f + 0.5f) * g->vh));
    }
    if (prim == 0) { for (int i = 0; i < n; i++) if (ok[i]) fill_rect(g->back, sx[i], sy[i], 2, 2, v[i].color | 0xFF000000u); return; }
    int step = prim == 1 ? 2 : 1, last = prim == 2 ? n : n - 1;
    for (int i = 0; i + (prim == 2 ? 0 : 1) < n && i < last; i += step) {
        int j = (i + 1) % n;
        if (ok[i] && ok[j]) draw_line(g->back, sx[i], sy[i], sx[j], sy[j], v[i].color | 0xFF000000u);
    }
}
static void draw_batch(glctx_t *g, int prim, GLuint texid, const r3d_vertex_t *v, int n) {
    if (n <= 0 || !ensure_buffers(g)) return;
    if (prim <= 3) { draw_lines(g, v, n, prim); return; }
    /* viewport: GL counts from the bottom */
    int vx = CLAMP(g->vx, 0, g->bw), vw = CLAMP(g->vw, 1, g->bw - vx);
    int vy = CLAMP(g->bh - (g->vy + g->vh), 0, g->bh), vh = CLAMP(g->vh, 1, g->bh - vy);
    if (vw <= 0 || vh <= 0) return;
    r3d_t r;
    if (!r3d_begin_with(&r, g->back, R(vx, vy, vw, vh), g->depth, false)) return;
    r.proj = g->pj[g->pjd]; r.view_m = m4_identity(); r.model = g->mv[g->mvd];
    r.lighting = (g->en & EN_LIGHTING) && (g->en & EN_LIGHT0);
    memcpy(r.light, g->light_dir, sizeof r.light);
    r.ambient = g->model_amb + g->light_amb; r.diffuse = g->light_dif;
    gltex_t *t = (g->en & EN_TEX2D) && texid && texid < NTEX ? &g->tex[texid] : NULL;
    r.texture = t && t->c ? t->c : NULL;
    r.bilinear = t ? t->linear : true;
    r.cull_back = (g->en & EN_CULL) && g->cull_face == 0x0405;
    r.front_cw = g->front_face == 0x0900;
    r.depth_test = (g->en & EN_DEPTH) != 0; r.depth_write = r.depth_test && g->depth_mask;
    /* triangulate */
    r3d_vertex_t *tri = kmalloc(sizeof(r3d_vertex_t) * (n * 3 + 6));
    if (!tri) return;
    int k = 0;
    #define T3(a, b, c) do { tri[k++] = v[a]; tri[k++] = v[b]; tri[k++] = v[c]; } while (0)
    switch (prim) {
    case 4: for (int i = 0; i + 2 < n; i += 3) T3(i, i + 1, i + 2); break;
    case 5: for (int i = 0; i + 2 < n; i++) { if (i & 1) T3(i + 1, i, i + 2); else T3(i, i + 1, i + 2); } break;
    case 6: case 9: for (int i = 1; i + 1 < n; i++) T3(0, i, i + 1); break;
    case 7: for (int i = 0; i + 3 < n; i += 4) { T3(i, i + 1, i + 2); T3(i, i + 2, i + 3); } break;
    case 8: for (int i = 0; i + 3 < n; i += 2) { T3(i, i + 1, i + 3); T3(i, i + 3, i + 2); } break;
    }
    #undef T3
    if (!(g->en & EN_BLEND)) for (int i = 0; i < k; i++) tri[i].color |= 0xFF000000u;   /* without blending, alpha is ignored */
    r3d_triangles(&r, tri, k);
    kfree(tri);
}
static void emit(glctx_t *g, int prim, const r3d_vertex_t *v, int n) {
    if (g->compiling) {
        dlist_t *l = &g->lists[g->compiling];
        if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->b = krealloc(l->b, sizeof(batch_t) * l->cap); }
        batch_t *b = &l->b[l->n++];
        b->prim = prim; b->tex = (g->en & EN_TEX2D) ? g->bound : 0; b->n = n;
        b->v = kmalloc(sizeof(r3d_vertex_t) * MAX(n, 1)); memcpy(b->v, v, sizeof(r3d_vertex_t) * n);
        if (!g->compile_exec) return;
    }
    draw_batch(g, prim, g->bound, v, n);
}
static r3d_vertex_t make_vertex(glctx_t *g, float x, float y, float z, float w) {
    r3d_vertex_t v;
    v.x = x; v.y = y; v.z = z; v.w = w;
    v.u = g->tc[0]; v.v = 1 - g->tc[1];                             /* GL's v = 0 is the bottom row */
    v.nx = g->nrm[0]; v.ny = g->nrm[1]; v.nz = g->nrm[2];
    bool lit = (g->en & EN_LIGHTING) && (g->en & EN_LIGHT0);
    v.color = to_argb(lit && !(g->en & EN_COLORMAT) ? g->mat_dif : g->col);
    return v;
}
static void WINAPI glBegin(GLenum mode) { G; g->prim = (int)mode; g->vn = 0; }
static void WINAPI glEnd(void) { G; if (g->prim >= 0) emit(g, g->prim, g->vb, g->vn); g->prim = -1; g->vn = 0; }
static void add_vertex(glctx_t *g, float x, float y, float z, float w) {
    if (g->prim < 0) return;
    if (g->vn == g->vcap) { g->vcap = g->vcap ? g->vcap * 2 : 256; g->vb = krealloc(g->vb, sizeof(r3d_vertex_t) * g->vcap); }
    g->vb[g->vn++] = make_vertex(g, x, y, z, w);
}
static void WINAPI glVertex2f(GLfloat x, GLfloat y) { G; add_vertex(g, x, y, 0, 1); }
static void WINAPI glVertex2i(GLint x, GLint y) { G; add_vertex(g, (float)x, (float)y, 0, 1); }
static void WINAPI glVertex2d(GLdouble x, GLdouble y) { G; add_vertex(g, (float)x, (float)y, 0, 1); }
static void WINAPI glVertex3f(GLfloat x, GLfloat y, GLfloat z) { G; add_vertex(g, x, y, z, 1); }
static void WINAPI glVertex3d(GLdouble x, GLdouble y, GLdouble z) { G; add_vertex(g, (float)x, (float)y, (float)z, 1); }
static void WINAPI glVertex3i(GLint x, GLint y, GLint z) { G; add_vertex(g, (float)x, (float)y, (float)z, 1); }
static void WINAPI glVertex3fv(const GLfloat *v) { G; add_vertex(g, v[0], v[1], v[2], 1); }
static void WINAPI glVertex3dv(const GLdouble *v) { G; add_vertex(g, (float)v[0], (float)v[1], (float)v[2], 1); }
static void WINAPI glVertex2fv(const GLfloat *v) { G; add_vertex(g, v[0], v[1], 0, 1); }
static void WINAPI glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { G; add_vertex(g, x, y, z, w); }
static void WINAPI glColor3f(GLfloat r, GLfloat gg, GLfloat b) { G; g->col[0] = r; g->col[1] = gg; g->col[2] = b; g->col[3] = 1; }
static void WINAPI glColor4f(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) { G; g->col[0] = r; g->col[1] = gg; g->col[2] = b; g->col[3] = a; }
static void WINAPI glColor3d(GLdouble r, GLdouble gg, GLdouble b) { glColor3f((float)r, (float)gg, (float)b); }
static void WINAPI glColor4d(GLdouble r, GLdouble gg, GLdouble b, GLdouble a) { glColor4f((float)r, (float)gg, (float)b, (float)a); }
static void WINAPI glColor3ub(GLubyte r, GLubyte gg, GLubyte b) { glColor3f(r / 255.0f, gg / 255.0f, b / 255.0f); }
static void WINAPI glColor4ub(GLubyte r, GLubyte gg, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, gg / 255.0f, b / 255.0f, a / 255.0f); }
static void WINAPI glColor3fv(const GLfloat *c) { glColor3f(c[0], c[1], c[2]); }
static void WINAPI glColor4fv(const GLfloat *c) { glColor4f(c[0], c[1], c[2], c[3]); }
static void WINAPI glColor3ubv(const GLubyte *c) { glColor3ub(c[0], c[1], c[2]); }
static void WINAPI glNormal3f(GLfloat x, GLfloat y, GLfloat z) { G; g->nrm[0] = x; g->nrm[1] = y; g->nrm[2] = z; }
static void WINAPI glNormal3d(GLdouble x, GLdouble y, GLdouble z) { glNormal3f((float)x, (float)y, (float)z); }
static void WINAPI glNormal3fv(const GLfloat *n) { glNormal3f(n[0], n[1], n[2]); }
static void WINAPI glTexCoord2f(GLfloat s, GLfloat t) { G; g->tc[0] = s; g->tc[1] = t; }
static void WINAPI glTexCoord2d(GLdouble s, GLdouble t) { glTexCoord2f((float)s, (float)t); }
static void WINAPI glTexCoord2fv(const GLfloat *v) { glTexCoord2f(v[0], v[1]); }
static void WINAPI glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { glBegin(7); glVertex2f(x1, y1); glVertex2f(x2, y1); glVertex2f(x2, y2); glVertex2f(x1, y2); glEnd(); }

/* vertex arrays */
static uint32_t cs_bit(GLenum a) { return a == 0x8074 ? CS_VERTEX : a == 0x8075 ? CS_NORMAL : a == 0x8076 ? CS_COLOR : a == 0x8078 ? CS_TEX : 0; }
static void WINAPI glEnableClientState(GLenum a) { G; g->cs |= cs_bit(a); }
static void WINAPI glDisableClientState(GLenum a) { G; g->cs &= ~cs_bit(a); }
static void WINAPI glVertexPointer(GLint s, GLenum t, GLsizei st, const void *p) { G; g->va = (arr_t){ s, t, st, p }; }
static void WINAPI glNormalPointer(GLenum t, GLsizei st, const void *p) { G; g->na = (arr_t){ 3, t, st, p }; }
static void WINAPI glColorPointer(GLint s, GLenum t, GLsizei st, const void *p) { G; g->ca = (arr_t){ s, t, st, p }; }
static void WINAPI glTexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p) { G; g->ta = (arr_t){ s, t, st, p }; }
static int type_size(GLenum t) { return t == 0x1400 || t == 0x1401 ? 1 : t == 0x1402 || t == 0x1403 ? 2 : t == 0x140A ? 8 : 4; }
static float fetch(const arr_t *a, int i, int c) {
    if (c >= a->size) return c == 3 ? 1.0f : 0.0f;
    int ts = type_size(a->type), stride = a->stride ? a->stride : ts * a->size;
    const uint8_t *p = (const uint8_t *)a->ptr + (size_t)i * stride + c * ts;
    switch (a->type) {
    case 0x1406: return *(const float *)p;
    case 0x140A: return (float)*(const double *)p;
    case 0x1401: return *p / 255.0f;
    case 0x1400: return *(const int8_t *)p / 127.0f;
    case 0x1402: return *(const int16_t *)p;
    case 0x1403: return *(const uint16_t *)p;
    case 0x1404: return (float)*(const int32_t *)p;
    default: return (float)*(const uint32_t *)p;
    }
}
static void array_element(glctx_t *g, int i) {
    if (g->cs & CS_COLOR) { for (int c = 0; c < 4; c++) g->col[c] = fetch(&g->ca, i, c); }
    if (g->cs & CS_NORMAL) { for (int c = 0; c < 3; c++) g->nrm[c] = fetch(&g->na, i, c); }
    if (g->cs & CS_TEX) { g->tc[0] = fetch(&g->ta, i, 0); g->tc[1] = fetch(&g->ta, i, 1); }
    if (g->cs & CS_VERTEX) add_vertex(g, fetch(&g->va, i, 0), fetch(&g->va, i, 1), g->va.size > 2 ? fetch(&g->va, i, 2) : 0, 1);
}
static void WINAPI glArrayElement(GLint i) { G; array_element(g, i); }
static void WINAPI glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    G; if (!(g->cs & CS_VERTEX)) return;
    glBegin(mode); for (int i = 0; i < count; i++) array_element(g, first + i); glEnd();
}
static void WINAPI glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    G; if (!(g->cs & CS_VERTEX)) return;
    glBegin(mode);
    for (int i = 0; i < count; i++) {
        int k = type == 0x1401 ? ((const uint8_t *)idx)[i] : type == 0x1403 ? ((const uint16_t *)idx)[i] : (int)((const uint32_t *)idx)[i];
        array_element(g, k);
    }
    glEnd();
}

/* display lists (geometry only) */
static GLuint WINAPI glGenLists(GLsizei n) {
    GV(0);
    for (int s = 1; s + n <= NLIST; s++) {
        bool ok = true; for (int k = 0; k < n; k++) if (g->lists[s + k].used) { ok = false; break; }
        if (ok) { for (int k = 0; k < n; k++) g->lists[s + k].used = true; return s; }
    }
    return 0;
}
static void free_list(dlist_t *l) { for (int i = 0; i < l->n; i++) kfree(l->b[i].v); kfree(l->b); l->b = NULL; l->n = l->cap = 0; }
static void WINAPI glNewList(GLuint list, GLenum mode) { G; if (list && list < NLIST) { free_list(&g->lists[list]); g->lists[list].used = true; g->compiling = list; g->compile_exec = mode == 0x1301; } }
static void WINAPI glEndList(void) { G; g->compiling = 0; }
static void WINAPI glCallList(GLuint list) {
    G; if (!list || list >= NLIST) return;
    dlist_t *l = &g->lists[list];
    for (int i = 0; i < l->n; i++) draw_batch(g, l->b[i].prim, l->b[i].tex ? l->b[i].tex : g->bound, l->b[i].v, l->b[i].n);
}
static void WINAPI glDeleteLists(GLuint list, GLsizei n) { G; for (GLuint i = list; i < list + (GLuint)n && i < NLIST; i++) { free_list(&g->lists[i]); g->lists[i].used = false; } }
static GLboolean WINAPI glIsList(GLuint list) { GV(0); return list && list < NLIST && g->lists[list].used; }

/* ---------------- WGL and pixel formats (gdi32) ---------------- */
typedef struct { WORD size, version; DWORD flags; BYTE type, color_bits, rbits, rshift, gbits, gshift, bbits, bshift, abits, ashift, accum, ar, ag, ab, aa, depth, stencil, aux, layer, res; DWORD lmask, vmask, dmask; } PFD;
static void fill_pfd(PFD *p) {
    memset(p, 0, sizeof *p);
    p->size = sizeof *p; p->version = 1;
    p->flags = 0x4 | 0x20 | 0x1 | 0x40;                             /* DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLEBUFFER, GENERIC_FORMAT */
    p->type = 0; p->color_bits = 32; p->rbits = p->gbits = p->bbits = p->abits = 8; p->rshift = 16; p->gshift = 8; p->ashift = 24;
    p->depth = 24;
}
int WINAPI w32_ChoosePixelFormat(HDC dc, const PFD *p) { (void)dc; (void)p; return 1; }
BOOL WINAPI w32_SetPixelFormat(HDC dc, int fmt, const PFD *p) { (void)dc; (void)fmt; (void)p; return 1; }
int WINAPI w32_GetPixelFormat(HDC dc) { (void)dc; return 1; }
int WINAPI w32_DescribePixelFormat(HDC dc, int fmt, UINT bytes, PFD *p) { (void)dc; (void)fmt; if (p && bytes >= sizeof(PFD)) fill_pfd(p); return 1; }
BOOL WINAPI w32_SwapBuffers(HDC dc) {
    uintptr_t hwnd = w32_dc_hwnd(dc);
    glctx_t *g = CUR();
    if (g && (g->hwnd == hwnd || !hwnd)) { present(g); task_yield(); return 1; }
    return 0;
}
static HANDLE WINAPI wglCreateContext(HDC dc) {
    uintptr_t hwnd = w32_dc_hwnd(dc);
    if (!hwnd || !accel3d) { if (!accel3d) klog("w32: wglCreateContext refused: 3D acceleration is turned off"); return 0; }
    glctx_t *g = kzalloc(sizeof *g);
    if (!g) return 0;
    g->magic = GLMAGIC; g->hwnd = hwnd; g->prim = -1;
    g->mv[0] = g->pj[0] = g->tx[0] = m4_identity(); g->mode = 0x1700;
    g->col[0] = g->col[1] = g->col[2] = g->col[3] = 1; g->nrm[2] = 1;
    g->cull_face = 0x0405; g->front_face = 0x0901; g->depth_mask = 1;
    g->light_dir[2] = 1; g->light_dif = 1; g->model_amb = 0.2f;
    g->mat_dif[0] = g->mat_dif[1] = g->mat_dif[2] = 0.8f; g->mat_dif[3] = 1;
    g->doublebuf = true;
    return (HANDLE)g;
}
static glctx_t *CTX(HANDLE h) { glctx_t *g = (glctx_t *)h; return g && g->magic == GLMAGIC ? g : NULL; }
static BOOL WINAPI wglMakeCurrent(HDC dc, HANDLE h) {
    (void)dc;
    glctx_t *g = CTX(h);
    if (h && !g) return 0;
    set_current(g);
    if (g) ensure_buffers(g);
    return 1;
}
static BOOL WINAPI wglDeleteContext(HANDLE h) {
    glctx_t *g = CTX(h);
    if (!g) return 0;
    if (CUR() == g) set_current(NULL);
    for (int i = 0; i < NTEX; i++) if (g->tex[i].c) canvas_free(g->tex[i].c);
    for (int i = 0; i < NLIST; i++) free_list(&g->lists[i]);
    if (g->back) canvas_free(g->back);
    kfree(g->depth); kfree(g->vb);
    g->magic = 0; kfree(g);
    return 1;
}
static HANDLE WINAPI wglGetCurrentContext(void) { return (HANDLE)CUR(); }
static HDC WINAPI wglGetCurrentDC(void) { return 0; }
static void *WINAPI wglGetProcAddress(const char *name) { (void)name; return NULL; }     /* no extensions beyond 1.1 */
static BOOL WINAPI wglShareLists(HANDLE a, HANDLE b) { (void)a; (void)b; return 0; }
static BOOL WINAPI wglSwapLayerBuffers(HDC dc, UINT planes) { (void)planes; return w32_SwapBuffers(dc); }

/* ---------------- glu32 ---------------- */
static void WINAPI gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble n, GLdouble f) {
    double t = n * r3d_sin((float)(fovy * 3.14159265 / 360)) / r3d_cos((float)(fovy * 3.14159265 / 360));
    glFrustum(-t * aspect, t * aspect, -t, t, n, f);
}
static void WINAPI gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t) { glOrtho(l, r, b, t, -1, 1); }
static void WINAPI gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx, GLdouble cy, GLdouble cz, GLdouble ux, GLdouble uy, GLdouble uz) {
    float f[3] = { (float)(cx - ex), (float)(cy - ey), (float)(cz - ez) };
    float l = r3d_sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); if (l <= 0) return;
    for (int i = 0; i < 3; i++) f[i] /= l;
    float u[3] = { (float)ux, (float)uy, (float)uz };
    float s[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
    l = r3d_sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]); if (l <= 0) return;
    for (int i = 0; i < 3; i++) s[i] /= l;
    float v[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    float m[16] = { s[0], v[0], -f[0], 0, s[1], v[1], -f[1], 0, s[2], v[2], -f[2], 0, 0, 0, 0, 1 };
    glMultMatrixf(m);
    glTranslated(-ex, -ey, -ez);
}
static GLint WINAPI gluBuild2DMipmaps(GLenum target, GLint ifmt, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *data) {
    glTexImage2D(target, 0, ifmt, w, h, 0, fmt, type, data); return 0;
}
static const GLubyte *WINAPI gluErrorString(GLenum e) { (void)e; return (const GLubyte *)"OpenGL error"; }

#define E(n) { #n, (void *)n }
const w32_export_t opengl32_exports[] = {
    E(glMatrixMode), E(glLoadIdentity), E(glLoadMatrixf), E(glLoadMatrixd), E(glMultMatrixf), E(glMultMatrixd), E(glPushMatrix), E(glPopMatrix),
    E(glTranslatef), E(glTranslated), E(glScalef), E(glScaled), E(glRotatef), E(glRotated), E(glFrustum), E(glOrtho), E(glViewport),
    E(glEnable), E(glDisable), E(glIsEnabled), E(glCullFace), E(glFrontFace), E(glDepthMask), E(glDepthFunc), E(glShadeModel), E(glHint),
    E(glBlendFunc), E(glAlphaFunc), E(glPolygonMode), E(glLineWidth), E(glPointSize), E(glPixelStorei), E(glClearDepth), E(glClearColor), E(glClear),
    E(glLightfv), E(glLightf), E(glLightModelfv), E(glLightModeli), E(glMaterialfv), E(glMaterialf), E(glColorMaterial),
    E(glGetError), E(glGetString), E(glGetIntegerv), E(glGetFloatv), E(glGetDoublev), E(glFlush), E(glFinish),
    E(glGenTextures), E(glDeleteTextures), E(glBindTexture), E(glTexParameteri), E(glTexParameterf), E(glTexEnvi), E(glTexEnvf), E(glTexImage2D), E(glTexSubImage2D),
    E(glBegin), E(glEnd), E(glVertex2f), E(glVertex2i), E(glVertex2d), E(glVertex3f), E(glVertex3d), E(glVertex3i), E(glVertex3fv), E(glVertex3dv), E(glVertex2fv), E(glVertex4f),
    E(glColor3f), E(glColor4f), E(glColor3d), E(glColor4d), E(glColor3ub), E(glColor4ub), E(glColor3fv), E(glColor4fv), E(glColor3ubv),
    E(glNormal3f), E(glNormal3d), E(glNormal3fv), E(glTexCoord2f), E(glTexCoord2d), E(glTexCoord2fv), E(glRectf),
    E(glEnableClientState), E(glDisableClientState), E(glVertexPointer), E(glNormalPointer), E(glColorPointer), E(glTexCoordPointer),
    E(glArrayElement), E(glDrawArrays), E(glDrawElements),
    E(glGenLists), E(glNewList), E(glEndList), E(glCallList), E(glDeleteLists), E(glIsList),
    E(wglCreateContext), E(wglMakeCurrent), E(wglDeleteContext), E(wglGetCurrentContext), E(wglGetCurrentDC), E(wglGetProcAddress), E(wglShareLists), E(wglSwapLayerBuffers),
};
const int opengl32_count = ARRAY_LEN(opengl32_exports);
const w32_export_t glu32_exports[] = { E(gluPerspective), E(gluOrtho2D), E(gluLookAt), E(gluBuild2DMipmaps), E(gluErrorString) };
const int glu32_count = ARRAY_LEN(glu32_exports);
