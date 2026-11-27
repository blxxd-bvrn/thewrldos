/* opengl32.dll, glu32.dll and the WGL/pixel-format part of gdi32.dll for Win32
 * programs, drawn by the WRLD 3D engine (src/gui/r3d.c).
 *
 * This is OpenGL 1.1, the fixed-function API: glBegin/glEnd, vertex arrays,
 * display lists of geometry, the matrix stacks, one directional light with
 * colour material, 2D textures (RGB/RGBA/BGRA/luminance bytes), depth test,
 * back-face culling and alpha blending. A context renders into its own back
 * buffer and depth buffer; SwapBuffers copies the frame into the window.
 * Beyond 1.1 there are the extensions SDL's OpenGL renderer and similar 2D
 * layers use: framebuffer objects that render into a texture
 * (GL_EXT_framebuffer_object), packed pixel formats, scissoring, and swap
 * control (WGL_EXT_swap_control). Shaders (OpenGL 2.0 and later) are not
 * available: wglGetProcAddress returns NULL for them, as on a 1.1 driver. */
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
enum { EN_DEPTH = 1, EN_LIGHTING = 2, EN_TEX2D = 4, EN_BLEND = 8, EN_CULL = 16, EN_LIGHT0 = 32, EN_COLORMAT = 64, EN_SCISSOR = 128 };
enum { CS_VERTEX = 1, CS_NORMAL = 2, CS_COLOR = 4, CS_TEX = 8 };
typedef struct { GLint size; GLenum type; GLsizei stride; const void *ptr; } arr_t;
typedef struct { int prim; GLuint tex; r3d_vertex_t *v; int n; } batch_t;
typedef struct { batch_t *b; int n, cap; bool used; } dlist_t;
typedef struct { canvas_t *c; bool linear; bool used; } gltex_t;
#define NTEX 512
#define NLIST 256
#define NFBO 64
typedef struct { bool used; GLuint tex; float *depth; int dw, dh; } glfbo_t;

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
    glfbo_t fbo[NFBO]; GLuint fbo_bound;
    int sc[4];                                  /* scissor box, GL coordinates */
    int unpack_row, unpack_align, pack_row, pack_align;
    int raster[2];
    int swap_interval; uint32_t last_swap;
    int active_unit, client_unit;               /* GL_ARB_multitexture: one real unit, 0 */
    bool tex_bracket;                           /* GL_TEXTURE_2D enabled again while on; see glDisable */
    GLuint list_base;                           /* glListBase, for glCallLists */
    struct { uint32_t en; float col[4]; bool depth_mask; GLenum cull_face, front_face; float clear[4];
             int vx, vy, vw, vh, sc[4]; GLuint bound; } attr[16];
    int attrd;                                  /* glPushAttrib / glPopAttrib */
    /* selection mode (glRenderMode(GL_SELECT)): nothing is drawn; each name stack that
     * something drew into while in view becomes a hit record in the program's buffer */
    GLenum render_mode;
    GLuint *selbuf; int selsize, selpos, hits; bool sel_overflow;
    GLuint names[64]; int nnames;
    bool hit; float hit_min, hit_max;
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
/* where drawing goes: the back buffer, or the texture attached to the bound framebuffer
 * object. A texture keeps GL's bottom row in its first canvas row (see glTexImage2D), so
 * drawing into one is flipped vertically. */
typedef struct { canvas_t *c; float *depth; int w, h; bool flip; } tgt_t;
static bool target(glctx_t *g, tgt_t *t) {
    if (g->fbo_bound && g->fbo_bound < NFBO) {
        glfbo_t *f = &g->fbo[g->fbo_bound];
        canvas_t *c = f->tex && f->tex < NTEX ? g->tex[f->tex].c : NULL;
        if (!c) return false;
        if (!f->depth || f->dw != c->w || f->dh != c->h) { kfree(f->depth); f->depth = kzalloc(sizeof(float) * c->w * c->h); f->dw = c->w; f->dh = c->h; }
        if (!f->depth) return false;
        *t = (tgt_t){ c, f->depth, c->w, c->h, true };
        return true;
    }
    if (!ensure_buffers(g)) return false;
    *t = (tgt_t){ g->back, g->depth, g->bw, g->bh, false };
    return true;
}
/* a rectangle in GL window coordinates (y up) as canvas rows */
static rect_t gl_rect(const tgt_t *t, int x, int y, int w, int h) { return R(x, t->flip ? y : t->h - (y + h), w, h); }
static rect_t scissor_rect(glctx_t *g, const tgt_t *t) {
    if (!(g->en & EN_SCISSOR)) return R(0, 0, t->w, t->h);
    return rect_intersect(R(0, 0, t->w, t->h), gl_rect(t, g->sc[0], g->sc[1], g->sc[2], g->sc[3]));
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
    case 0x0B44: return EN_CULL; case 0x4000: return EN_LIGHT0; case 0x0B57: return EN_COLORMAT; case 0x0C11: return EN_SCISSOR;
    }
    return 0;                                                       /* fog, normalize, dithering...: accepted, no effect */
}
/* SDL 2.0.12's OpenGL renderer uploads a texture as "glEnable(GL_TEXTURE_2D); bind; upload;
 * glDisable(GL_TEXTURE_2D)" without noting that texturing is now off; on drivers with shaders
 * that goes unseen. So when texturing is enabled while already on and only texture-object
 * calls follow, the next glDisable restores the earlier (enabled) state instead. */
static void WINAPI glEnable(GLenum cap) {
    G;
    if (cap == 0x0DE1) { if (g->active_unit) return; g->tex_bracket = (g->en & EN_TEX2D) != 0; }
    g->en |= en_bit(cap);
}
static void WINAPI glDisable(GLenum cap) {
    G;
    if (cap == 0x0DE1) { if (g->active_unit) return; if (g->tex_bracket) { g->tex_bracket = false; return; } }
    g->en &= ~en_bit(cap);
}
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
static void WINAPI glPixelStorei(GLenum a, GLint b) {
    G;
    if (a == 0x0CF2) g->unpack_row = MAX(b, 0); else if (a == 0x0CF5) g->unpack_align = b;
    else if (a == 0x0D02) g->pack_row = MAX(b, 0); else if (a == 0x0D05) g->pack_align = b;
}
static void WINAPI glPixelStoref(GLenum a, GLfloat b) { glPixelStorei(a, (GLint)b); }
static void WINAPI glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { G; g->sc[0] = x; g->sc[1] = y; g->sc[2] = w; g->sc[3] = h; }
static void WINAPI glClearDepth(GLclampd d) { (void)d; }
static void WINAPI glClearColor(GLclampf r, GLclampf gg, GLclampf b, GLclampf a) { G; g->clear[0] = r; g->clear[1] = gg; g->clear[2] = b; g->clear[3] = a; }
static uint32_t to_argb(const float *c) {
    int r = (int)(c[0] * 255 + 0.5f), gg = (int)(c[1] * 255 + 0.5f), b = (int)(c[2] * 255 + 0.5f), a = (int)(c[3] * 255 + 0.5f);
    r = CLAMP(r, 0, 255); gg = CLAMP(gg, 0, 255); b = CLAMP(b, 0, 255); a = CLAMP(a, 0, 255);
    return ((uint32_t)a << 24) | (r << 16) | (gg << 8) | b;
}
static void WINAPI glClear(GLbitfield m) {
    G; g->tex_bracket = false;
    tgt_t t; if (!target(g, &t)) return;
    rect_t r = scissor_rect(g, &t);
    if (r.w <= 0 || r.h <= 0) return;
    if (m & 0x4000) {
        uint32_t c = to_argb(g->clear);
        if (!t.flip) c |= 0xFF000000u;                              /* the window has no alpha; a texture keeps it */
        for (int y = r.y; y < r.y + r.h; y++) { uint32_t *row = t.c->px + (size_t)y * t.c->stride; for (int x = r.x; x < r.x + r.w; x++) row[x] = c; }
    }
    if (m & 0x0100) for (int y = r.y; y < r.y + r.h; y++) memset(t.depth + (size_t)y * t.w + r.x, 0, sizeof(float) * r.w);
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
    case 0x1F03: return (const GLubyte *)"GL_EXT_bgra GL_EXT_packed_pixels GL_ARB_texture_non_power_of_two GL_EXT_framebuffer_object GL_ARB_multitexture GL_EXT_blend_func_separate GL_EXT_blend_minmax";
    }
    return NULL;
}
static void WINAPI glGetIntegerv(GLenum n, GLint *v) {
    G;
    if (n == 0x0BA2) { v[0] = g->vx; v[1] = g->vy; v[2] = g->vw; v[3] = g->vh; }
    else if (n == 0x0D33) v[0] = 2048;
    else if (n == 0x0BA0) v[0] = g->mode;
    else if (n == 0x8CA6) v[0] = (GLint)g->fbo_bound;                /* GL_FRAMEBUFFER_BINDING */
    else if (n == 0x8069) v[0] = (GLint)g->bound;                    /* GL_TEXTURE_BINDING_2D */
    else if (n == 0x0C10) memcpy(v, g->sc, 16);                      /* GL_SCISSOR_BOX */
    else if (n == 0x0CF5) v[0] = g->unpack_align; else if (n == 0x0D05) v[0] = g->pack_align;
    else if (n == 0x0CF2) v[0] = g->unpack_row; else if (n == 0x0D02) v[0] = g->pack_row;
    else if (n == 0x84E2 || n == 0x8872) v[0] = 1;                   /* texture units */
    else if (n == 0x0D3A) { v[0] = 2048; v[1] = 2048; }              /* GL_MAX_VIEWPORT_DIMS */
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

/* ---------------- pixel transfer ---------------- */
/* bytes per pixel of a format/type pair; 0 if it is not supported */
static int px_bytes(GLenum fmt, GLenum type) {
    switch (type) {
    case 0x1401: return fmt == 0x1908 || fmt == 0x80E1 ? 4 : fmt == 0x1907 || fmt == 0x80E0 ? 3 : fmt == 0x190A ? 2 : 1;
    case 0x8367: case 0x8035: return 4;                             /* UNSIGNED_INT_8_8_8_8(_REV) */
    case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366: return 2;   /* 5_6_5, 4_4_4_4, 5_5_5_1 and reversed */
    }
    return 0;
}
static uint32_t px_read(GLenum fmt, GLenum type, const uint8_t *s) {
    uint32_t r, gg, b, a = 255;
    if (type == 0x8367 || type == 0x8035) {                          /* as 32-bit words; _REV is byte order on little endian */
        uint32_t w = *(const uint32_t *)s;
        if (type == 0x8035) w = __builtin_bswap32(w);
        uint32_t c0 = w & 0xFF, c1 = (w >> 8) & 0xFF, c2 = (w >> 16) & 0xFF, c3 = w >> 24;
        if (fmt == 0x80E1) { b = c0; gg = c1; r = c2; a = c3; } else { r = c0; gg = c1; b = c2; a = c3; }
        return (a << 24) | (r << 16) | (gg << 8) | b;
    }
    if (type != 0x1401) {
        uint16_t w = *(const uint16_t *)s;
        switch (type) {
        case 0x8363: r = (w >> 11) * 255 / 31; gg = ((w >> 5) & 63) * 255 / 63; b = (w & 31) * 255 / 31; break;
        case 0x8364: b = (w >> 11) * 255 / 31; gg = ((w >> 5) & 63) * 255 / 63; r = (w & 31) * 255 / 31; break;
        case 0x8033: r = (w >> 12) * 17; gg = ((w >> 8) & 15) * 17; b = ((w >> 4) & 15) * 17; a = (w & 15) * 17; break;
        case 0x8365: a = (w >> 12) * 17; b = ((w >> 8) & 15) * 17; gg = ((w >> 4) & 15) * 17; r = (w & 15) * 17; break;
        case 0x8034: r = (w >> 11) * 255 / 31; gg = ((w >> 6) & 31) * 255 / 31; b = ((w >> 1) & 31) * 255 / 31; a = (w & 1) * 255; break;
        default: a = (w >> 15) * 255; b = ((w >> 10) & 31) * 255 / 31; gg = ((w >> 5) & 31) * 255 / 31; r = (w & 31) * 255 / 31; break;
        }
        if (fmt == 0x80E1 || fmt == 0x80E0) { uint32_t t = r; r = b; b = t; }
        return (a << 24) | (r << 16) | (gg << 8) | b;
    }
    switch (fmt) {
    case 0x1908: r = s[0]; gg = s[1]; b = s[2]; a = s[3]; break;            /* RGBA */
    case 0x80E1: b = s[0]; gg = s[1]; r = s[2]; a = s[3]; break;            /* BGRA */
    case 0x1907: r = s[0]; gg = s[1]; b = s[2]; break;                      /* RGB */
    case 0x80E0: b = s[0]; gg = s[1]; r = s[2]; break;                      /* BGR */
    case 0x190A: r = gg = b = s[0]; a = s[1]; break;                        /* LUMINANCE_ALPHA */
    case 0x1906: r = gg = b = 255; a = s[0]; break;                         /* ALPHA */
    case 0x1903: r = s[0]; gg = b = 0; break;                               /* RED */
    default: r = gg = b = s[0]; break;                                      /* LUMINANCE */
    }
    return (a << 24) | (r << 16) | (gg << 8) | b;
}
/* the distance between rows of client memory, by GL_(UN)PACK_ROW_LENGTH and _ALIGNMENT */
static size_t row_bytes(int w, int bpp, int row_len, int align) {
    size_t n = (size_t)(row_len > 0 ? row_len : w) * bpp;
    if (align < 1) align = 4;
    return (n + align - 1) / align * align;
}
/* copy client pixels into a canvas region (GL row order: first row to canvas row y0) */
static void upload(glctx_t *g, canvas_t *c, int x0, int y0, int w, int h, GLenum fmt, GLenum type, const void *px) {
    int bpp = px_bytes(fmt, type);
    if (!bpp || !px) return;
    size_t rb = row_bytes(w, bpp, g->unpack_row, g->unpack_align);
    for (int y = 0; y < h; y++) {
        int ty = y0 + y; if (ty < 0 || ty >= c->h) continue;
        const uint8_t *s = (const uint8_t *)px + (size_t)y * rb;
        uint32_t *d = c->px + (size_t)ty * c->stride;
        if (bpp == 4 && fmt == 0x80E1 && (type == 0x1401 || type == 0x8367) && x0 >= 0 && x0 + w <= c->w) { memcpy(d + x0, s, (size_t)w * 4); continue; }
        for (int x = 0; x < w; x++, s += bpp) { int tx = x0 + x; if (tx >= 0 && tx < c->w) d[tx] = px_read(fmt, type, s); }
    }
}

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
static void WINAPI glBindTexture(GLenum target, GLuint t) { G; (void)target; if (g->active_unit) return; if (t < NTEX) { g->bound = t; if (t) { g->tex[t].used = true; } } }
static void WINAPI glTexParameteri(GLenum target, GLenum p, GLint v) { G; (void)target; if ((p == 0x2800 || p == 0x2801) && g->bound < NTEX) g->tex[g->bound].linear = v != 0x2600 && v != 0x2700; }
static void WINAPI glTexParameterf(GLenum target, GLenum p, GLfloat v) { glTexParameteri(target, p, (GLint)v); }
static void WINAPI glTexEnvi(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; }
static void WINAPI glTexEnvf(GLenum a, GLenum b, GLfloat c) { (void)a; (void)b; (void)c; }
static void WINAPI glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *px) {
    G; (void)target; (void)ifmt; (void)border;
    if (level != 0) return;                                         /* mipmaps: only the base level is used */
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || g->bound >= NTEX) { err(g, 0x0501); return; }
    if (!px_bytes(fmt, type)) { err(g, 0x0500); return; }
    gltex_t *t = &g->tex[g->bound];
    if (t->c && (t->c->w != w || t->c->h != h)) { canvas_free(t->c); t->c = NULL; }
    if (!t->c) t->c = canvas_new(w, h);
    t->used = true;
    if (!t->c) { err(g, 0x0505); return; }
    if (!px) { memset(t->c->px, 0, (size_t)t->c->stride * h * 4); return; }
    /* GL's first row is the bottom of the image, which is texture coordinate v = 0 */
    upload(g, t->c, 0, 0, w, h, fmt, type, px);
}
static void WINAPI glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *px) {
    G; (void)target;
    if (level || g->bound >= NTEX || !g->tex[g->bound].c) return;
    if (!px_bytes(fmt, type)) { err(g, 0x0500); return; }
    upload(g, g->tex[g->bound].c, xo, yo, w, h, fmt, type, px);
}
static void WINAPI glCopyTexSubImage2D(GLenum target_, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    G; (void)target_;
    tgt_t t; if (level || g->bound >= NTEX || !g->tex[g->bound].c || !target(g, &t)) return;
    canvas_t *c = g->tex[g->bound].c;
    for (int r = 0; r < h; r++) {
        int sy = y + r, row = t.flip ? sy : t.h - 1 - sy, dy = yo + r;
        if (row < 0 || row >= t.h || dy < 0 || dy >= c->h) continue;
        for (int k = 0; k < w; k++) { int sx = x + k, dx = xo + k; if (sx >= 0 && sx < t.w && dx >= 0 && dx < c->w) c->px[(size_t)dy * c->stride + dx] = t.c->px[(size_t)row * t.c->stride + sx]; }
    }
}

/* ---------------- reading and drawing pixels ---------------- */
static void WINAPI glReadBuffer(GLenum m) { (void)m; }
static void WINAPI glDrawBuffer(GLenum m) { (void)m; }
static void WINAPI glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out) {
    G;
    tgt_t t; if (!out || !target(g, &t)) return;
    int bpp = type == 0x1401 ? (fmt == 0x1908 || fmt == 0x80E1 ? 4 : fmt == 0x1907 || fmt == 0x80E0 ? 3 : 1) : type == 0x8367 ? 4 : 0;
    if (!bpp) { err(g, 0x0500); return; }
    size_t rb = row_bytes(w, bpp, g->pack_row, g->pack_align);
    for (int r = 0; r < h; r++) {
        int row = t.flip ? y + r : t.h - 1 - (y + r);
        uint8_t *d = (uint8_t *)out + (size_t)r * rb;
        for (int k = 0; k < w; k++, d += bpp) {
            int sx = x + k;
            uint32_t c = row >= 0 && row < t.h && sx >= 0 && sx < t.w ? t.c->px[(size_t)row * t.c->stride + sx] : 0;
            uint8_t R_ = (c >> 16) & 0xFF, G_ = (c >> 8) & 0xFF, B_ = c & 0xFF, A_ = t.flip ? c >> 24 : 255;
            if (fmt == 0x80E1 || fmt == 0x80E0) { d[0] = B_; d[1] = G_; d[2] = R_; } else { d[0] = R_; if (bpp > 1) { d[1] = G_; d[2] = B_; } }
            if (bpp == 4) d[3] = A_;
        }
    }
}
/* the raster position: the point through the modelview and projection matrices and the
 * viewport, in window pixels counted from the bottom left (as OpenGL does) */
static void raster_pos(glctx_t *g, float x, float y, float z) {
    const float *P = g->pj[g->pjd].m, *M = g->mv[g->mvd].m;
    float e[4], c[4], in[4] = { x, y, z, 1 };
    for (int r = 0; r < 4; r++) e[r] = M[r] * in[0] + M[4 + r] * in[1] + M[8 + r] * in[2] + M[12 + r] * in[3];
    for (int r = 0; r < 4; r++) c[r] = P[r] * e[0] + P[4 + r] * e[1] + P[8 + r] * e[2] + P[12 + r] * e[3];
    if (c[3] <= 1e-6f) return;
    g->raster[0] = (int)(g->vx + (c[0] / c[3] + 1) * 0.5f * g->vw + 0.5f);
    g->raster[1] = (int)(g->vy + (c[1] / c[3] + 1) * 0.5f * g->vh + 0.5f);
}
static void WINAPI glRasterPos2i(GLint x, GLint y) { G; raster_pos(g, (float)x, (float)y, 0); }
static void WINAPI glRasterPos2f(GLfloat x, GLfloat y) { G; raster_pos(g, x, y, 0); }
static void WINAPI glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { G; raster_pos(g, x, y, z); }
static void WINAPI glDrawPixels(GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *px) {
    G;
    tgt_t t; if (!px || !target(g, &t)) return;
    int bpp = px_bytes(fmt, type); if (!bpp) { err(g, 0x0500); return; }
    size_t rb = row_bytes(w, bpp, g->unpack_row, g->unpack_align);
    rect_t clip = scissor_rect(g, &t);
    for (int r = 0; r < h; r++) {
        int gy = g->raster[1] + r, row = t.flip ? gy : t.h - 1 - gy;
        if (row < clip.y || row >= clip.y + clip.h) continue;
        const uint8_t *s = (const uint8_t *)px + (size_t)r * rb;
        for (int k = 0; k < w; k++, s += bpp) { int dx = g->raster[0] + k; if (dx >= clip.x && dx < clip.x + clip.w) t.c->px[(size_t)row * t.c->stride + dx] = px_read(fmt, type, s) | (t.flip ? 0 : 0xFF000000u); }
    }
}
static void WINAPI glGetBooleanv(GLenum n, GLboolean *v) { GLint i[4] = { 0 }; glGetIntegerv(n, i); v[0] = i[0] != 0; }
static void WINAPI glGetPointerv(GLenum n, void **v) {
    G;
    *v = n == 0x808E ? (void *)g->va.ptr : n == 0x808F ? (void *)g->na.ptr : n == 0x8090 ? (void *)g->ca.ptr : n == 0x8092 ? (void *)g->ta.ptr : NULL;
}
static void WINAPI glGetTexImage(GLenum target_, GLint level, GLenum fmt, GLenum type, void *out) { (void)target_; (void)level; (void)fmt; (void)type; (void)out; }
static void WINAPI glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { (void)a; (void)b; (void)c; (void)d; }
static void WINAPI glBlendEquation(GLenum m) { (void)m; }
static void WINAPI glBlendEquationSeparate(GLenum a, GLenum b) { (void)a; (void)b; }
static void WINAPI glColorMask(GLboolean r, GLboolean gg, GLboolean b, GLboolean a) { (void)r; (void)gg; (void)b; (void)a; }
static void WINAPI glStencilFunc(GLenum f, GLint r, GLuint m) { (void)f; (void)r; (void)m; }
static void WINAPI glStencilOp(GLenum a, GLenum b, GLenum c) { (void)a; (void)b; (void)c; }
static void WINAPI glClearStencil(GLint s) { (void)s; }
static void WINAPI glTexParameteriv(GLenum t, GLenum p, const GLint *v) { glTexParameteri(t, p, v[0]); }
static void WINAPI glTexParameterfv(GLenum t, GLenum p, const GLfloat *v) { glTexParameteri(t, p, (GLint)v[0]); }
static void WINAPI glTexEnvfv(GLenum a, GLenum b, const GLfloat *c) { (void)a; (void)b; (void)c; }
static void WINAPI glTexEnviv(GLenum a, GLenum b, const GLint *c) { (void)a; (void)b; (void)c; }
static void WINAPI glTexImage1D(GLenum a, GLint b, GLint c, GLsizei d, GLint e, GLenum f, GLenum h, const void *i) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i; }
static GLboolean WINAPI glIsTexture(GLuint t) { GV(0); return t && t < NTEX && g->tex[t].used; }

static void WINAPI glTexCoord2f(GLfloat s, GLfloat t);
/* ---------------- GL_ARB_multitexture: one texture unit ----------------
 * Programs that only use unit 0 (SDL's renderer outside YUV) work; binds to other units
 * are ignored. GL_MAX_TEXTURE_UNITS says 1. */
static void WINAPI glActiveTexture(GLenum u) { G; g->active_unit = (int)(u - 0x84C0); }
static void WINAPI glClientActiveTexture(GLenum u) { G; g->client_unit = (int)(u - 0x84C0); }
static void WINAPI glMultiTexCoord2f(GLenum u, GLfloat s_, GLfloat t_) { if (u == 0x84C0) glTexCoord2f(s_, t_); }
static void WINAPI glMultiTexCoord2fv(GLenum u, const GLfloat *v) { if (u == 0x84C0) glTexCoord2f(v[0], v[1]); }

/* ---------------- framebuffer objects (GL_EXT_framebuffer_object) ---------------- */
static void WINAPI glGenFramebuffers(GLsizei n, GLuint *out) {
    G;
    for (int i = 0; i < n; i++) { out[i] = 0; for (GLuint k = 1; k < NFBO; k++) if (!g->fbo[k].used) { g->fbo[k].used = true; out[i] = k; break; } }
}
static void WINAPI glDeleteFramebuffers(GLsizei n, const GLuint *ids) {
    G;
    for (int i = 0; i < n; i++) if (ids[i] && ids[i] < NFBO) { if (g->fbo_bound == ids[i]) g->fbo_bound = 0; kfree(g->fbo[ids[i]].depth); memset(&g->fbo[ids[i]], 0, sizeof(glfbo_t)); }
}
static void WINAPI glBindFramebuffer(GLenum target_, GLuint id) { G; (void)target_; if (id < NFBO) { g->fbo_bound = id; if (id) g->fbo[id].used = true; } }
static void WINAPI glFramebufferTexture2D(GLenum target_, GLenum attach, GLenum textarget, GLuint tex, GLint level) {
    G; (void)target_; (void)textarget; (void)level;
    if (attach == 0x8CE0 && g->fbo_bound && g->fbo_bound < NFBO) g->fbo[g->fbo_bound].tex = tex < NTEX ? tex : 0;   /* COLOR_ATTACHMENT0 */
}
static GLenum WINAPI glCheckFramebufferStatus(GLenum target_) {
    GV(0); (void)target_;
    if (!g->fbo_bound) return 0x8CD5;                                   /* GL_FRAMEBUFFER_COMPLETE */
    glfbo_t *f = &g->fbo[g->fbo_bound];
    return f->tex && g->tex[f->tex].c ? 0x8CD5 : 0x8CD7;                /* ...or INCOMPLETE_MISSING_ATTACHMENT */
}
static GLboolean WINAPI glIsFramebuffer(GLuint id) { GV(0); return id && id < NFBO && g->fbo[id].used; }
/* renderbuffers: accepted (programs ask for depth ones), drawing uses the FBO's own depth buffer */
static GLuint rb_next = 1;
static void WINAPI glGenRenderbuffers(GLsizei n, GLuint *out) { for (int i = 0; i < n; i++) out[i] = rb_next++; }
static void WINAPI glDeleteRenderbuffers(GLsizei n, const GLuint *ids) { (void)n; (void)ids; }
static void WINAPI glBindRenderbuffer(GLenum t, GLuint id) { (void)t; (void)id; }
static void WINAPI glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) { (void)t; (void)f; (void)w; (void)h; }
static void WINAPI glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint rb) { (void)t; (void)a; (void)rt; (void)rb; }
static void WINAPI glGenerateMipmap(GLenum t) { (void)t; }

/* ---------------- drawing ---------------- */
static void draw_lines(glctx_t *g, const tgt_t *t, const r3d_vertex_t *v, int n, int prim) {
    mat4 mvp = m4_mul(g->pj[g->pjd], g->mv[g->mvd]);
    rect_t old = canvas_push_clip(t->c, scissor_rect(g, t));
    int sx[2048], sy[2048]; bool ok[2048]; n = MIN(n, 2048);
    for (int i = 0; i < n; i++) {
        const float *m = mvp.m;
        float x = m[0] * v[i].x + m[4] * v[i].y + m[8] * v[i].z + m[12], y = m[1] * v[i].x + m[5] * v[i].y + m[9] * v[i].z + m[13];
        float w = m[3] * v[i].x + m[7] * v[i].y + m[11] * v[i].z + m[15];
        ok[i] = w > 0.01f;
        if (!ok[i]) continue;
        sx[i] = g->vx + (int)((x / w * 0.5f + 0.5f) * g->vw);
        int gy = g->vy + (int)((y / w * 0.5f + 0.5f) * g->vh);
        sy[i] = t->flip ? gy : t->h - gy;
    }
    if (prim == 0) { for (int i = 0; i < n; i++) if (ok[i]) fill_rect(t->c, sx[i], sy[i], 2, 2, v[i].color | 0xFF000000u); t->c->clip = old; return; }
    int step = prim == 1 ? 2 : 1, last = prim == 2 ? n : n - 1;
    for (int i = 0; i + (prim == 2 ? 0 : 1) < n && i < last; i += step) {
        int j = (i + 1) % n;
        if (ok[i] && ok[j]) draw_line(t->c, sx[i], sy[i], sx[j], sy[j], v[i].color | 0xFF000000u);
    }
    t->c->clip = old;
}
/* ---- selection mode ---- */
#define GL_RENDER 0x1C00
#define GL_SELECT 0x1C02
static void sel_flush(glctx_t *g) {
    if (!g->hit) return;
    g->hit = false;
    int need = 3 + g->nnames;
    if (!g->selbuf || g->selpos + need > g->selsize) { g->sel_overflow = true; return; }
    GLuint *b = g->selbuf + g->selpos;
    b[0] = g->nnames;
    b[1] = (GLuint)(CLAMP(g->hit_min, 0.0f, 1.0f) * 4294967295.0f);
    b[2] = (GLuint)(CLAMP(g->hit_max, 0.0f, 1.0f) * 4294967295.0f);
    for (int i = 0; i < g->nnames; i++) b[3 + i] = g->names[i];
    g->selpos += need; g->hits++;
}
/* a primitive in selection mode: a hit when any part of it is inside the view volume
 * (its corners' bounding box against the clip square), with its depth range */
static void sel_test(glctx_t *g, const r3d_vertex_t *v, int n) {
    const float *P = g->pj[g->pjd].m, *M = g->mv[g->mvd].m;
    float x0 = 2, y0 = 2, x1 = -2, y1 = -2, z0 = 2, z1 = -2; bool any = false;
    for (int i = 0; i < n; i++) {
        float e[4], c[4], in[4] = { v[i].x, v[i].y, v[i].z, v[i].w ? v[i].w : 1 };
        for (int r = 0; r < 4; r++) e[r] = M[r] * in[0] + M[4 + r] * in[1] + M[8 + r] * in[2] + M[12 + r] * in[3];
        for (int r = 0; r < 4; r++) c[r] = P[r] * e[0] + P[4 + r] * e[1] + P[8 + r] * e[2] + P[12 + r] * e[3];
        if (c[3] <= 1e-6f) continue;
        float nx = c[0] / c[3], ny = c[1] / c[3], nz = c[2] / c[3];
        x0 = MIN(x0, nx); x1 = MAX(x1, nx); y0 = MIN(y0, ny); y1 = MAX(y1, ny); z0 = MIN(z0, nz); z1 = MAX(z1, nz);
        any = true;
    }
    if (!any || x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1 || z1 < -1 || z0 > 1) return;
    float d0 = (CLAMP(z0, -1, 1) + 1) / 2, d1 = (CLAMP(z1, -1, 1) + 1) / 2;
    if (!g->hit) { g->hit = true; g->hit_min = d0; g->hit_max = d1; }
    else { g->hit_min = MIN(g->hit_min, d0); g->hit_max = MAX(g->hit_max, d1); }
}
static void draw_batch(glctx_t *g, int prim, GLuint texid, const r3d_vertex_t *v, int n) {
    if (g->render_mode == GL_SELECT) { if (n > 0) sel_test(g, v, n); return; }
    tgt_t tg;
    g->tex_bracket = false;
    if (n <= 0 || !target(g, &tg)) return;
    if (prim <= 3) { draw_lines(g, &tg, v, n, prim); return; }
    /* viewport: GL counts from the bottom (a texture target is stored bottom row first) */
    int vx = CLAMP(g->vx, 0, tg.w), vw = CLAMP(g->vw, 1, tg.w - vx);
    int vy = CLAMP(tg.flip ? g->vy : tg.h - (g->vy + g->vh), 0, tg.h), vh = CLAMP(g->vh, 1, tg.h - vy);
    if (vw <= 0 || vh <= 0) return;
    r3d_t r;
    /* the depth buffer is per pixel of the target; r3d indexes it by the viewport */
    float *depth = tg.depth;
    if (vx || vy || vw != tg.w || vh != tg.h) depth = NULL;
    float *tmp = NULL;
    if (!depth) { tmp = kzalloc(sizeof(float) * vw * vh); depth = tmp; }
    if (!depth || !r3d_begin_with(&r, tg.c, R(vx, vy, vw, vh), depth, false)) { kfree(tmp); return; }
    r.proj = g->pj[g->pjd]; r.view_m = m4_identity(); r.model = g->mv[g->mvd];
    if (tg.flip) r.proj = m4_mul(m4_scale(1, -1, 1), r.proj);
    r.lighting = (g->en & EN_LIGHTING) && (g->en & EN_LIGHT0);
    memcpy(r.light, g->light_dir, sizeof r.light);
    r.ambient = g->model_amb + g->light_amb; r.diffuse = g->light_dif;
    gltex_t *t = (g->en & EN_TEX2D) && texid && texid < NTEX ? &g->tex[texid] : NULL;
    r.texture = t && t->c ? t->c : NULL;
    r.bilinear = t ? t->linear : true;
    r.cull_back = (g->en & EN_CULL) && g->cull_face == 0x0405;
    r.front_cw = (g->front_face == 0x0900) != tg.flip;
    r.depth_test = (g->en & EN_DEPTH) != 0; r.depth_write = r.depth_test && g->depth_mask;
    r.blend = (g->en & EN_BLEND) != 0;
    /* triangulate */
    r3d_vertex_t *tri = kmalloc(sizeof(r3d_vertex_t) * (n * 3 + 6));
    if (!tri) { kfree(tmp); return; }
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
    rect_t old = canvas_push_clip(tg.c, scissor_rect(g, &tg));
    r3d_triangles(&r, tri, k);
    tg.c->clip = old;
    kfree(tri); kfree(tmp);
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
static void WINAPI glBegin(GLenum mode) { G; g->tex_bracket = false; g->prim = (int)mode; g->vn = 0; }
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

static void WINAPI glColor4ubv(const GLubyte *c) { glColor4f(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f); }
static void WINAPI glTexCoord2i(GLint s_, GLint t_) { glTexCoord2f((float)s_, (float)t_); }
static void WINAPI glRecti(GLint a, GLint b, GLint c, GLint d) { glRectf((float)a, (float)b, (float)c, (float)d); }

/* vertex arrays */
static uint32_t cs_bit(GLenum a) { return a == 0x8074 ? CS_VERTEX : a == 0x8075 ? CS_NORMAL : a == 0x8076 ? CS_COLOR : a == 0x8078 ? CS_TEX : 0; }
static void WINAPI glEnableClientState(GLenum a) { G; g->cs |= cs_bit(a); }
static void WINAPI glDisableClientState(GLenum a) { G; g->cs &= ~cs_bit(a); }
static void WINAPI glVertexPointer(GLint s, GLenum t, GLsizei st, const void *p) { G; g->va = (arr_t){ s, t, st, p }; }
static void WINAPI glNormalPointer(GLenum t, GLsizei st, const void *p) { G; g->na = (arr_t){ 3, t, st, p }; }
static void WINAPI glColorPointer(GLint s, GLenum t, GLsizei st, const void *p) { G; g->ca = (arr_t){ s, t, st, p }; }
static void WINAPI glTexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p) { G; if (g->client_unit) return; g->ta = (arr_t){ s, t, st, p }; }
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
/* wglUseFontBitmaps: a list holding one character, drawn at the raster position in the
 * current colour with the OS font the DC had (n holds the font), then the raster moves on */
#define PRIM_GLYPH 100
static void draw_glyph(glctx_t *g, uint32_t ch, int font) {
    if (g->render_mode == GL_SELECT) return;
    tgt_t t; if (!target(g, &t)) return;
    char s[5]; int k = 0;
    if (ch < 0x80) s[k++] = (char)ch;
    else if (ch < 0x800) { s[k++] = (char)(0xC0 | (ch >> 6)); s[k++] = (char)(0x80 | (ch & 0x3F)); }
    else { s[k++] = (char)(0xE0 | (ch >> 12)); s[k++] = (char)(0x80 | ((ch >> 6) & 0x3F)); s[k++] = (char)(0x80 | (ch & 0x3F)); }
    s[k] = 0;
    /* the raster position is the glyph's baseline origin, counted from the bottom */
    int base = t.flip ? g->raster[1] : t.h - 1 - g->raster[1];
    uint32_t col = 0xFF000000u | ((uint32_t)(CLAMP(g->col[0], 0, 1) * 255) << 16) | ((uint32_t)(CLAMP(g->col[1], 0, 1) * 255) << 8) | (uint32_t)(CLAMP(g->col[2], 0, 1) * 255);
    rect_t old = canvas_push_clip(t.c, scissor_rect(g, &t));
    draw_text(t.c, font, g->raster[0], base - font_ascent(font) + 1, s, col);
    t.c->clip = old;
    g->raster[0] += text_width(font, s);
}
static void WINAPI glCallList(GLuint list) {
    G; if (!list || list >= NLIST) return;
    dlist_t *l = &g->lists[list];
    for (int i = 0; i < l->n; i++) {
        if (l->b[i].prim == PRIM_GLYPH) { draw_glyph(g, l->b[i].tex, l->b[i].n); continue; }
        draw_batch(g, l->b[i].prim, l->b[i].tex ? l->b[i].tex : g->bound, l->b[i].v, l->b[i].n);
    }
}
static void WINAPI glListBase(GLuint base) { G; g->list_base = base; }
static void WINAPI glCallLists(GLsizei n, GLenum type, const void *lists) {
    G; if (!lists || n <= 0) return;
    for (GLsizei i = 0; i < n; i++) {
        long v;
        switch (type) {
        case 0x1400: v = ((const int8_t *)lists)[i]; break;      /* GL_BYTE */
        case 0x1401: v = ((const uint8_t *)lists)[i]; break;     /* GL_UNSIGNED_BYTE */
        case 0x1402: v = ((const int16_t *)lists)[i]; break;
        case 0x1403: v = ((const uint16_t *)lists)[i]; break;
        case 0x1404: v = ((const int32_t *)lists)[i]; break;
        case 0x1405: v = (long)((const uint32_t *)lists)[i]; break;
        case 0x1406: v = (long)((const float *)lists)[i]; break;
        default: err(g, 0x0500); return;
        }
        glCallList((GLuint)(g->list_base + v));
    }
}
static void WINAPI glDeleteLists(GLuint list, GLsizei n) { G; for (GLuint i = list; i < list + (GLuint)n && i < NLIST; i++) { free_list(&g->lists[i]); g->lists[i].used = false; } }
static GLboolean WINAPI glIsList(GLuint list) { GV(0); return list && list < NLIST && g->lists[list].used; }

/* ---- attribute stack, clip planes, colour index arrays ---- */
static void WINAPI glPushAttrib(GLbitfield mask) {
    G; (void)mask;
    if (g->attrd >= 16) { err(g, 0x0503); return; }                      /* GL_STACK_OVERFLOW */
    typeof(g->attr[0]) *a = &g->attr[g->attrd++];
    a->en = g->en; memcpy(a->col, g->col, sizeof a->col); a->depth_mask = g->depth_mask; a->cull_face = g->cull_face; a->front_face = g->front_face;
    memcpy(a->clear, g->clear, sizeof a->clear); a->vx = g->vx; a->vy = g->vy; a->vw = g->vw; a->vh = g->vh; memcpy(a->sc, g->sc, sizeof a->sc); a->bound = g->bound;
}
static void WINAPI glPopAttrib(void) {
    G;
    if (g->attrd <= 0) { err(g, 0x0504); return; }                       /* GL_STACK_UNDERFLOW */
    typeof(g->attr[0]) *a = &g->attr[--g->attrd];
    g->en = a->en; memcpy(g->col, a->col, sizeof a->col); g->depth_mask = a->depth_mask; g->cull_face = a->cull_face; g->front_face = a->front_face;
    memcpy(g->clear, a->clear, sizeof a->clear); g->vx = a->vx; g->vy = a->vy; g->vw = a->vw; g->vh = a->vh; memcpy(g->sc, a->sc, sizeof a->sc); g->bound = a->bound;
}
static void WINAPI glClipPlane(GLenum plane, const GLdouble *eq) { (void)plane; (void)eq; }   /* user clip planes are not applied */
static void WINAPI glIndexPointer(GLenum type, GLsizei stride, const void *p) { (void)type; (void)stride; (void)p; }   /* colour index mode is not used in RGBA */
static void WINAPI glColor3i(GLint r, GLint gg, GLint b) { glColor3f(r / 2147483647.0f, gg / 2147483647.0f, b / 2147483647.0f); }
/* ---- selection ---- */
static void WINAPI glSelectBuffer(GLsizei size, GLuint *buf) { G; g->selbuf = buf; g->selsize = size > 0 ? size : 0; }
static GLint WINAPI glRenderMode(GLenum mode) {
    GV(0);
    GLint r = 0;
    if (g->render_mode == GL_SELECT) { sel_flush(g); r = g->sel_overflow ? -1 : g->hits; }
    g->render_mode = mode == GL_SELECT ? GL_SELECT : GL_RENDER;
    if (mode == GL_SELECT) { g->selpos = g->hits = 0; g->sel_overflow = false; g->nnames = 0; g->hit = false; }
    return r;
}
static void WINAPI glInitNames(void) { G; sel_flush(g); g->nnames = 0; }
static void WINAPI glPushName(GLuint name) { G; sel_flush(g); if (g->nnames < 64) g->names[g->nnames++] = name; else err(g, 0x0503); }
static void WINAPI glPopName(void) { G; sel_flush(g); if (g->nnames > 0) g->nnames--; else err(g, 0x0504); }
static void WINAPI glLoadName(GLuint name) { G; sel_flush(g); if (g->nnames > 0) g->names[g->nnames - 1] = name; else err(g, 0x0502); }

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
    if (g && (g->hwnd == hwnd || !hwnd)) {
        present(g);
        if (g->swap_interval > 0) {                                 /* "vsync": at most one frame per 60 Hz refresh */
            uint32_t frame = 1000 * g->swap_interval / 60, now = timer_ticks();
            if (now - g->last_swap < frame) task_sleep(frame - (now - g->last_swap));
            g->last_swap = timer_ticks();
        } else task_yield();
        return 1;
    }
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
    g->unpack_align = g->pack_align = 4;
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
    for (int i = 0; i < NFBO; i++) kfree(g->fbo[i].depth);
    if (g->back) canvas_free(g->back);
    kfree(g->depth); kfree(g->vb);
    g->magic = 0; kfree(g);
    return 1;
}
static HANDLE WINAPI wglGetCurrentContext(void) { return (HANDLE)CUR(); }
static HDC WINAPI wglGetCurrentDC(void) { return 0; }
/* extensions (GL_EXT_framebuffer_object and friends, WGL_EXT_swap_control) */
static const char *WINAPI wglGetExtensionsStringARB(HDC dc) { (void)dc; return "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_EXT_swap_control"; }
static const char *WINAPI wglGetExtensionsStringEXT(void) { return wglGetExtensionsStringARB(0); }
static BOOL WINAPI wglSwapIntervalEXT(int n) { GV(0); g->swap_interval = MAX(n, 0); return 1; }
static int WINAPI wglGetSwapIntervalEXT(void) { GV(0); return g->swap_interval; }
static const struct { const char *name; void *fn; } gl_ext[] = {
    { "glGenFramebuffers", (void *)glGenFramebuffers }, { "glDeleteFramebuffers", (void *)glDeleteFramebuffers }, { "glBindFramebuffer", (void *)glBindFramebuffer },
    { "glFramebufferTexture2D", (void *)glFramebufferTexture2D }, { "glCheckFramebufferStatus", (void *)glCheckFramebufferStatus }, { "glIsFramebuffer", (void *)glIsFramebuffer },
    { "glGenRenderbuffers", (void *)glGenRenderbuffers }, { "glDeleteRenderbuffers", (void *)glDeleteRenderbuffers }, { "glBindRenderbuffer", (void *)glBindRenderbuffer },
    { "glRenderbufferStorage", (void *)glRenderbufferStorage }, { "glFramebufferRenderbuffer", (void *)glFramebufferRenderbuffer }, { "glGenerateMipmap", (void *)glGenerateMipmap },
    { "glActiveTexture", (void *)glActiveTexture }, { "glClientActiveTexture", (void *)glClientActiveTexture },
    { "glMultiTexCoord2f", (void *)glMultiTexCoord2f }, { "glMultiTexCoord2fv", (void *)glMultiTexCoord2fv },
    { "glBlendFuncSeparate", (void *)glBlendFuncSeparate }, { "glBlendEquation", (void *)glBlendEquation }, { "glBlendEquationSeparate", (void *)glBlendEquationSeparate },
    { "wglGetExtensionsStringARB", (void *)wglGetExtensionsStringARB }, { "wglGetExtensionsStringEXT", (void *)wglGetExtensionsStringEXT },
    { "wglSwapIntervalEXT", (void *)wglSwapIntervalEXT }, { "wglGetSwapIntervalEXT", (void *)wglGetSwapIntervalEXT },
};
static void *WINAPI wglGetProcAddress(const char *name) {
    if (!name) return NULL;
    char base[64]; strlcpy(base, name, sizeof base);
    size_t l = strlen(base);                                            /* glFooEXT and glFooARB are the same function here */
    if (l > 3 && (!strcmp(base + l - 3, "EXT") || !strcmp(base + l - 3, "ARB")) && strncmp(base, "wgl", 3)) base[l - 3] = 0;
    for (unsigned i = 0; i < ARRAY_LEN(gl_ext); i++) if (!strcmp(gl_ext[i].name, base) || !strcmp(gl_ext[i].name, name)) return gl_ext[i].fn;
    return NULL;                                                        /* shaders and the rest of OpenGL 2+ */
}
static BOOL WINAPI wglShareLists(HANDLE a, HANDLE b) { (void)a; (void)b; return 0; }
extern int w32_dc_font(uintptr_t hdc);
static BOOL WINAPI wglUseFontBitmapsW(HDC dc, DWORD first, DWORD count, DWORD base) {
    GV(0);
    int font = w32_dc_font((uintptr_t)dc);
    for (DWORD i = 0; i < count; i++) {
        DWORD list = base + i;
        if (!list || list >= NLIST) continue;
        dlist_t *l = &g->lists[list];
        free_list(l);
        l->used = true;
        l->b = kzalloc(sizeof(batch_t));
        if (!l->b) return 0;
        l->b[0].prim = PRIM_GLYPH; l->b[0].tex = first + i; l->b[0].n = font; l->b[0].v = NULL;
        l->n = l->cap = 1;
    }
    return 1;
}
static BOOL WINAPI wglUseFontBitmapsA(HDC dc, DWORD first, DWORD count, DWORD base) { return wglUseFontBitmapsW(dc, first, count, base); }
static BOOL WINAPI wglSwapLayerBuffers(HDC dc, UINT planes) { (void)planes; return w32_SwapBuffers(dc); }

/* ---------------- glu32 ---------------- */
static void WINAPI gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble n, GLdouble f) {
    double t = n * r3d_sin((float)(fovy * 3.14159265 / 360)) / r3d_cos((float)(fovy * 3.14159265 / 360));
    glFrustum(-t * aspect, t * aspect, -t, t, n, f);
}
static void WINAPI gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t) { glOrtho(l, r, b, t, -1, 1); }
/* the picking region: the small box around (x, y) fills the view */
static void WINAPI gluPickMatrix(GLdouble x, GLdouble y, GLdouble dx, GLdouble dy, GLint *vp) {
    if (dx <= 0 || dy <= 0 || !vp) return;
    glTranslated((vp[2] - 2 * (x - vp[0])) / dx, (vp[3] - 2 * (y - vp[1])) / dy, 0);
    glScaled(vp[2] / dx, vp[3] / dy, 1.0);
}
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
    E(glPixelStoref), E(glScissor), E(glCopyTexSubImage2D), E(glReadBuffer), E(glDrawBuffer), E(glReadPixels), E(glRasterPos2i), E(glRasterPos2f), E(glRasterPos3f), E(glDrawPixels), E(glListBase), E(glCallLists), E(glPushAttrib), E(glPopAttrib),
    E(glClipPlane), E(glIndexPointer), E(glColor3i), E(glSelectBuffer), E(glRenderMode), E(glInitNames), E(glPushName), E(glPopName), E(glLoadName),
    E(glGetBooleanv), E(glGetPointerv), E(glGetTexImage), E(glColorMask), E(glStencilFunc), E(glStencilOp), E(glClearStencil), E(glTexParameteriv), E(glTexParameterfv),
    E(glTexEnvfv), E(glTexEnviv), E(glTexImage1D), E(glIsTexture), E(glColor4ubv), E(glTexCoord2i), E(glRecti),
    E(glGenLists), E(glNewList), E(glEndList), E(glCallList), E(glDeleteLists), E(glIsList),
    E(wglCreateContext), E(wglMakeCurrent), E(wglDeleteContext), E(wglGetCurrentContext), E(wglGetCurrentDC), E(wglGetProcAddress), E(wglShareLists), E(wglSwapLayerBuffers), E(wglUseFontBitmapsW), E(wglUseFontBitmapsA),
};
const int opengl32_count = ARRAY_LEN(opengl32_exports);
const w32_export_t glu32_exports[] = { E(gluPerspective), E(gluOrtho2D), E(gluPickMatrix), E(gluLookAt), E(gluBuild2DMipmaps), E(gluErrorString) };
const int glu32_count = ARRAY_LEN(glu32_exports);
