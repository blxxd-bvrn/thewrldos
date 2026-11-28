/* The OpenGL Shading Language for gl.c: shader and program objects (OpenGL 2.0 and
 * GL_ARB_shader_objects), uniforms and attributes, and the programmable pipeline.
 *
 * The compiler is Rust (rust/src/glsl): it turns GLSL into the shader IR the Direct3D 10
 * engine runs (rust/src/dxbc.rs), and this file runs that: the vertex shader once per
 * vertex, then clipping, the triangle (or line, or point) rasteriser and the fragment
 * shader once per pixel, with depth test, alpha test, blending and colour masks as GL
 * sets them. Uniforms are constant buffer 0 of each stage; the built-in state uniforms
 * of the compatibility profile (gl_ModelViewProjectionMatrix, gl_LightSource and the
 * rest) are filled in from GL state at each draw. */
#include "dxbc.h"
#include "smp.h"
static void WINAPI glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx);

typedef struct { char name[96]; uint32_t gltype; int32_t count, reg, stride, location; } glsl_var_t;
typedef struct {
    dxbc_shader_t *shader;
    glsl_var_t *uniforms; int32_t nuniforms;
    glsl_var_t *attribs; int32_t nattribs;
    int32_t cb_rows, nsamplers, stage, ninstr;
} glsl_unit_t;
glsl_unit_t *glsl_compile(int stage, const char *src, size_t len, char *log, int loglen);
void glsl_free(glsl_unit_t *u);

/* vertex input slots: the built-in attributes, then the generic ones */
enum { SL_VERTEX, SL_NORMAL, SL_COLOR, SL_COLOR2, SL_FOG, SL_TEX0, SL_GEN0 = SL_TEX0 + 8, SL_N = SL_GEN0 + NATTR, SL_VERTEXID = 100, SL_INSTANCEID };
typedef struct { float a[SL_N][4]; } fatvert_t;

typedef struct { char name[96]; uint32_t gltype; int count; int reg[2], stride[2]; } sl_uni_t;   /* [0] fragment, [1] vertex; reg -1: not in it */
typedef struct sl_obj {
    int kind;                                   /* 1 shader, 2 program */
    bool deleted;
    /* shader */
    GLenum type; char *src; size_t srclen; bool compiled; int refs;
    /* program */
    GLuint sh[8]; int nsh; bool linked;
    glsl_unit_t *u[2];                          /* [0] fragment, [1] vertex */
    sl_uni_t *uni; int nuni;
    vec4_t *cb[2]; int rows[2];
    int unit[2][16];                            /* sampler slot -> texture unit */
    int in_slot[DX_MAX_REG];                    /* vertex input register -> SL_* */
    int link_src[DX_MAX_REG];                   /* fragment input register -> vertex output register, -1 none */
    int pos_reg, target_reg;
    struct { char name[64]; int loc; } bind[NATTR]; int nbind;
    int attr_loc[DX_MAX_REG];                   /* generic location of each user attribute (by vertex input register) */
    char *log;
} sl_obj_t;
struct glsl_state {
    sl_obj_t **o; int no;
    GLuint cur;
    fatvert_t *imm; int nimm, capimm;           /* glBegin/glEnd vertices while a program is in use */
    fatvert_t cur_v;                            /* the current attribute values */
    float tc[8][4];                             /* glMultiTexCoord per unit */
    arr_t tca[8]; uint32_t tc_on;               /* texture coordinate arrays per unit */
    /* fixed-function state the built-in uniforms read */
    struct { float amb[4], dif[4], spec[4], pos[4], sdir[3], sexp, scut, att[3]; } light[8];
    struct { float emi[4], amb[4], dif[4], spec[4], shin; } mat[2];
    float lm_amb[4];
    struct { float color[4], density, start, end; } fog;
    GLenum alpha_func; float alpha_ref;
    uint32_t frames, frame_start, draws, pixels;
    float depth_near, depth_far;
};

static struct glsl_state *SL(glctx_t *g) {
    struct gl2 *x = X(g); if (!x) return NULL;
    if (!x->sl) {
        struct glsl_state *s = kzalloc(sizeof *s);
        if (!s) return NULL;
        for (int i = 0; i < 8; i++) {
            float *a = s->light[i].amb; a[3] = 1;
            s->light[i].pos[2] = 1; s->light[i].sdir[2] = -1; s->light[i].scut = 180; s->light[i].att[0] = 1;
            s->light[i].dif[3] = s->light[i].spec[3] = 1;
            if (!i) for (int k = 0; k < 3; k++) s->light[i].dif[k] = s->light[i].spec[k] = 1;
            s->tc[i][3] = 1;
        }
        for (int f = 0; f < 2; f++) {
            for (int k = 0; k < 3; k++) { s->mat[f].amb[k] = 0.2f; s->mat[f].dif[k] = 0.8f; }
            s->mat[f].amb[3] = s->mat[f].dif[3] = s->mat[f].spec[3] = s->mat[f].emi[3] = 1;
        }
        for (int k = 0; k < 3; k++) s->lm_amb[k] = 0.2f;
        s->lm_amb[3] = 1;
        s->fog.density = 1; s->fog.end = 1;
        s->alpha_func = 0x0207;
        s->depth_far = 1;
        s->cur_v.a[SL_VERTEX][3] = 1; s->cur_v.a[SL_COLOR][0] = s->cur_v.a[SL_COLOR][1] = s->cur_v.a[SL_COLOR][2] = s->cur_v.a[SL_COLOR][3] = 1;
        s->cur_v.a[SL_NORMAL][2] = 1;
        x->sl = s;
    }
    return x->sl;
}
#define GS struct glsl_state *s = SL(g); if (!s) return
#define GSV(v) struct glsl_state *s = SL(g); if (!s) return v

/* ---------------- fixed-function state for the built-ins (called from gl.c) ---------------- */
static void ff_light(glctx_t *g, GLenum light, GLenum p, const GLfloat *v) {
    GS; int i = (int)light - 0x4000; if (i < 0 || i >= 8) return;
    switch (p) {
    case 0x1200: memcpy(s->light[i].amb, v, 16); break;
    case 0x1201: memcpy(s->light[i].dif, v, 16); break;
    case 0x1202: memcpy(s->light[i].spec, v, 16); break;
    case 0x1203: {                                                  /* in eye space, by the modelview matrix now */
        const float *m = g->mv[g->mvd].m;
        for (int r = 0; r < 4; r++) s->light[i].pos[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
        break;
    }
    case 0x1204: { const float *m = g->mv[g->mvd].m; for (int r = 0; r < 3; r++) s->light[i].sdir[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2]; break; }
    case 0x1205: s->light[i].sexp = v[0]; break;
    case 0x1206: s->light[i].scut = v[0]; break;
    case 0x1207: case 0x1208: case 0x1209: s->light[i].att[p - 0x1207] = v[0]; break;
    }
}
static void ff_material(glctx_t *g, GLenum face, GLenum p, const GLfloat *v) {
    GS;
    if (face != 0x0404 && face != 0x0405 && face != 0x0408) { err(g, 0x0500); return; }   /* GL_INVALID_ENUM: the call does nothing */
    for (int f = 0; f < 2; f++) {
        if ((f == 0 && face == 0x0405) || (f == 1 && face == 0x0404)) continue;
        switch (p) {
        case 0x1200: memcpy(s->mat[f].amb, v, 16); break;
        case 0x1201: memcpy(s->mat[f].dif, v, 16); break;
        case 0x1202: memcpy(s->mat[f].spec, v, 16); break;
        case 0x1600: memcpy(s->mat[f].emi, v, 16); break;
        case 0x1601: s->mat[f].shin = v[0]; break;
        case 0x1602: memcpy(s->mat[f].amb, v, 16); memcpy(s->mat[f].dif, v, 16); break;
        }
    }
}
static void ff_light_model(glctx_t *g, GLenum p, const GLfloat *v) { GS; if (p == 0x0B53) memcpy(s->lm_amb, v, 16); }
static void ff_fog(glctx_t *g, GLenum p, const GLfloat *v) {
    GS;
    if (p == 0x0B66) memcpy(s->fog.color, v, 16); else if (p == 0x0B62) s->fog.density = v[0]; else if (p == 0x0B63) s->fog.start = v[0]; else if (p == 0x0B64) s->fog.end = v[0];
}
static void WINAPI glFogfv(GLenum p, const GLfloat *v) { G; ff_fog(g, p, v); }
static void WINAPI glFogf(GLenum p, GLfloat v) { G; ff_fog(g, p, &v); }
static void WINAPI glFogi(GLenum p, GLint v) { G; float f = (float)v; ff_fog(g, p, &f); }
static void WINAPI glFogiv(GLenum p, const GLint *v) { G; float f[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f }; if (p != 0x0B66) f[0] = (float)v[0]; ff_fog(g, p, f); }
static void WINAPI glDepthRange(GLclampd n, GLclampd f) { G; GS; s->depth_near = (float)n; s->depth_far = (float)f; }
static void WINAPI glDepthRangef(GLclampf n, GLclampf f) { glDepthRange(n, f); }
static void ff_alpha(glctx_t *g, GLenum f, GLclampf r) { GS; s->alpha_func = f; s->alpha_ref = r; }
static void ff_multitex(glctx_t *g, GLenum unit, const float *v, int n) {
    GS; int u = (int)unit - 0x84C0; if (u < 0 || u >= 8) return;
    for (int k = 0; k < 4; k++) s->tc[u][k] = k < n ? v[k] : k == 3 ? 1 : 0;
}

/* ---------------- objects ---------------- */
static sl_obj_t *obj(glctx_t *g, GLuint id, int kind) {
    struct glsl_state *s = SL(g);
    if (!s || !id || (int)id >= s->no || !s->o[id] || (kind && s->o[id]->kind != kind)) return NULL;
    return s->o[id];
}
static GLuint new_obj(glctx_t *g, int kind) {
    GSV(0);
    int id = 1;
    while (id < s->no && s->o[id]) id++;
    if (id >= s->no) {
        int nn = MAX(64, s->no * 2);
        if (nn > 65536) return 0;
        sl_obj_t **o = krealloc(s->o, sizeof(*o) * nn);
        if (!o) return 0;
        memset(o + s->no, 0, sizeof(*o) * (nn - s->no));
        s->o = o; s->no = nn;
    }
    sl_obj_t *ob = kzalloc(sizeof *ob);
    if (!ob) return 0;
    ob->kind = kind;
    s->o[id] = ob;
    return id;
}
static void set_log(sl_obj_t *o, const char *m) { kfree(o->log); o->log = m ? strdup(m) : NULL; }
static void unlink_prog(sl_obj_t *p) {
    for (int i = 0; i < 2; i++) { if (p->u[i]) glsl_free(p->u[i]); p->u[i] = NULL; kfree(p->cb[i]); p->cb[i] = NULL; p->rows[i] = 0; }
    kfree(p->uni); p->uni = NULL; p->nuni = 0; p->linked = false;
}
static void free_obj(glctx_t *g, GLuint id) {
    struct glsl_state *s = SL(g);
    sl_obj_t *o = obj(g, id, 0); if (!o) return;
    if (o->kind == 2) unlink_prog(o);
    kfree(o->src); kfree(o->log); kfree(o);
    s->o[id] = NULL;
}
static GLuint WINAPI glCreateShader(GLenum type) {
    GV(0); if (type != 0x8B30 && type != 0x8B31) { err(g, 0x0500); return 0; }
    GLuint id = new_obj(g, 1); if (!id) return 0;
    SL(g)->o[id]->type = type;
    return id;
}
static GLuint WINAPI glCreateProgram(void) { GV(0); return new_obj(g, 2); }
static void WINAPI glShaderSource(GLuint id, GLsizei n, const char *const *str, const GLint *len) {
    G; sl_obj_t *o = obj(g, id, 1); if (!o) { err(g, 0x0501); return; }
    size_t total = 0;
    for (int i = 0; i < n; i++) total += str[i] ? (len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i])) : 0;
    char *b = kmalloc(total + 1); if (!b) { err(g, 0x0505); return; }
    size_t at = 0;
    for (int i = 0; i < n; i++) { if (!str[i]) continue; size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(b + at, str[i], l); at += l; }
    b[at] = 0;
    kfree(o->src); o->src = b; o->srclen = at; o->compiled = false;
}
static void WINAPI glCompileShader(GLuint id) {
    G; sl_obj_t *o = obj(g, id, 1); if (!o) { err(g, 0x0501); return; }
    char log[256];
    glsl_unit_t *u = o->src ? glsl_compile(o->type == 0x8B31, o->src, o->srclen, log, sizeof log) : NULL;
    if (!o->src) strlcpy(log, "no source", sizeof log);
    o->compiled = u != NULL;
    if (u) { set_log(o, ""); glsl_free(u); }
    else {
        char m[300]; snprintf(m, sizeof m, "ERROR: 0:%s\n", log); set_log(o, m);
        klog("w32: GLSL %s shader %u does not compile: %s", o->type == 0x8B31 ? "vertex" : "fragment", id, log);
    }
}
static void WINAPI glAttachShader(GLuint p, GLuint sh) {
    G; sl_obj_t *po = obj(g, p, 2), *so = obj(g, sh, 1);
    if (!po || !so) { err(g, 0x0501); return; }
    for (int i = 0; i < po->nsh; i++) if (po->sh[i] == sh) return;
    if (po->nsh < 8) { po->sh[po->nsh++] = sh; so->refs++; }
}
static void WINAPI glDetachShader(GLuint p, GLuint sh) {
    G; sl_obj_t *po = obj(g, p, 2); if (!po) return;
    for (int i = 0; i < po->nsh; i++) if (po->sh[i] == sh) {
        po->sh[i] = po->sh[--po->nsh];
        sl_obj_t *so = obj(g, sh, 1);
        if (so && --so->refs <= 0 && so->deleted) free_obj(g, sh);
        return;
    }
}
static void WINAPI glDeleteShader(GLuint id) {
    G; sl_obj_t *o = obj(g, id, 0); if (!o) return;
    if (o->kind == 2) { struct glsl_state *s = SL(g); if (s->cur == id) { o->deleted = true; return; } for (int i = 0; i < o->nsh; i++) glDetachShader(id, o->sh[i]); free_obj(g, id); return; }
    if (o->refs > 0) o->deleted = true; else free_obj(g, id);
}
static void WINAPI glDeleteProgram(GLuint id) { glDeleteShader(id); }
static GLboolean WINAPI glIsShader(GLuint id) { GV(0); return obj(g, id, 1) != NULL; }
static GLboolean WINAPI glIsProgram(GLuint id) { GV(0); return obj(g, id, 2) != NULL; }
static void WINAPI glBindAttribLocation(GLuint p, GLuint loc, const char *name) {
    G; sl_obj_t *po = obj(g, p, 2); if (!po || loc >= NATTR || !name) { err(g, 0x0501); return; }
    for (int i = 0; i < po->nbind; i++) if (!strcmp(po->bind[i].name, name)) { po->bind[i].loc = loc; return; }
    if (po->nbind < NATTR) { strlcpy(po->bind[po->nbind].name, name, 64); po->bind[po->nbind++].loc = loc; }
}
/* the slot a built-in attribute comes from */
static int builtin_slot(const char *n) {
    if (!strcmp(n, "gl_Vertex")) return SL_VERTEX;
    if (!strcmp(n, "gl_Normal")) return SL_NORMAL;
    if (!strcmp(n, "gl_Color")) return SL_COLOR;
    if (!strcmp(n, "gl_SecondaryColor")) return SL_COLOR2;
    if (!strcmp(n, "gl_FogCoord")) return SL_FOG;
    if (!strncmp(n, "gl_MultiTexCoord", 16) && n[16] >= '0' && n[16] <= '7') return SL_TEX0 + n[16] - '0';
    if (!strcmp(n, "gl_VertexID") || !strcmp(n, "gl_VertexIndex")) return SL_VERTEXID;
    if (!strcmp(n, "gl_InstanceID") || !strcmp(n, "gl_InstanceIndex")) return SL_INSTANCEID;
    return -1;
}
static void WINAPI glLinkProgram(GLuint p) {
    G; sl_obj_t *po = obj(g, p, 2); if (!po) { err(g, 0x0501); return; }
    unlink_prog(po);
    /* each stage: the sources of its shaders, one after another (a #version line only in the first) */
    char *src[2] = { NULL, NULL }; size_t len[2] = { 0, 0 };
    for (int i = 0; i < po->nsh; i++) {
        sl_obj_t *so = obj(g, po->sh[i], 1); if (!so || !so->src) continue;
        int st = so->type == 0x8B31;
        char *n = krealloc(src[st], len[st] + so->srclen + 2);
        if (!n) continue;
        src[st] = n;
        memcpy(n + len[st], so->src, so->srclen);
        if (len[st]) { /* hide a later #version */ for (size_t k = len[st]; k + 8 < len[st] + so->srclen; k++) if (!memcmp(n + k, "#version", 8)) { memcpy(n + k, "//", 2); } }
        len[st] += so->srclen; n[len[st]++] = '\n'; n[len[st]] = 0;
    }
    char log[300] = "";
    for (int st = 1; st >= 0; st--) {
        if (!src[st]) {
            /* a program without one of the stages: the fixed-function stand-in */
            static const char *ff[2] = {
                "void main() { gl_FragColor = gl_Color; }",
                "void main() { gl_Position = ftransform(); gl_FrontColor = gl_Color; gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0; }" };
            src[st] = strdup(ff[st]); len[st] = strlen(ff[st]);
        }
        char e[256];
        po->u[st] = glsl_compile(st, src[st], len[st], e, sizeof e);
        if (!po->u[st]) { snprintf(log, sizeof log, "ERROR: %s shader: 0:%s\n", st ? "vertex" : "fragment", e); break; }
    }
    kfree(src[0]); kfree(src[1]);
    if (!po->u[0] || !po->u[1]) { set_log(po, log); klog("w32: GLSL program %u does not link: %s", p, log); unlink_prog(po); return; }
    /* uniforms of both stages by name; their storage */
    int nmax = po->u[0]->nuniforms + po->u[1]->nuniforms;
    po->uni = kzalloc(sizeof(sl_uni_t) * MAX(1, nmax));
    for (int st = 0; st < 2; st++) {
        po->rows[st] = po->u[st]->cb_rows;
        po->cb[st] = kzalloc(sizeof(vec4_t) * MAX(1, po->rows[st]));
        for (int i = 0; i < po->u[st]->nuniforms; i++) {
            glsl_var_t *v = &po->u[st]->uniforms[i];
            sl_uni_t *u = NULL;
            for (int k = 0; k < po->nuni; k++) if (!strcmp(po->uni[k].name, v->name)) { u = &po->uni[k]; break; }
            if (!u) { u = &po->uni[po->nuni++]; strlcpy(u->name, v->name, sizeof u->name); u->gltype = v->gltype; u->count = v->count; u->reg[0] = u->reg[1] = -1; }
            u->reg[st] = v->reg; u->stride[st] = v->stride;
        }
    }
    /* vertex inputs: built-in slots, or generic locations (bound, from the shader, or the next free one) */
    for (int i = 0; i < DX_MAX_REG; i++) { po->in_slot[i] = -1; po->link_src[i] = -1; po->attr_loc[i] = -1; }
    uint32_t used = 0;
    glsl_unit_t *vs = po->u[1], *fs = po->u[0];
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < vs->nattribs; i++) {
        glsl_var_t *a = &vs->attribs[i];
        int bs = builtin_slot(a->name);
        if (bs >= 0) { if (pass == 0) po->in_slot[a->reg] = bs; continue; }
        int loc = a->location;
        for (int k = 0; k < po->nbind; k++) if (!strcmp(po->bind[k].name, a->name)) loc = po->bind[k].loc;
        if (pass == 0 && loc < 0) continue;
        if (pass == 1) { if (po->in_slot[a->reg] >= 0) continue; loc = 0; while (loc < NATTR && (used & (1u << loc))) loc++; }
        for (int k = 0; k < a->count * a->stride && a->reg + k < DX_MAX_REG; k++) {
            int l = loc + k; if (l >= NATTR) break;
            po->in_slot[a->reg + k] = SL_GEN0 + l; used |= 1u << l;
            if (!k) po->attr_loc[a->reg] = l;
        }
    }
    /* gl_Vertex and generic attribute 0 are the same thing (as on most drivers) */
    for (int i = 0; i < DX_MAX_REG; i++) if (po->in_slot[i] == SL_GEN0 && !(used & ~1u) && 0) po->in_slot[i] = SL_VERTEX;
    /* varyings: fragment inputs from the vertex outputs of the same name and index */
    const dxsig_t *vo, *fi, *fo;
    int nvo = dxbc_outputs(vs->shader, &vo), nfi = dxbc_inputs(fs->shader, &fi), nfo = dxbc_outputs(fs->shader, &fo);
    po->pos_reg = -1; po->target_reg = -1;
    for (int i = 0; i < nvo; i++) if (vo[i].sysval == 1) po->pos_reg = vo[i].reg;
    for (int i = 0; i < nfo; i++) if (fo[i].sysval == 64) po->target_reg = fo[i].reg;
    for (int i = 0; i < nfi; i++) {
        if (fi[i].sysval) continue;
        for (int k = 0; k < nvo; k++) if (!strcmp(vo[k].name, fi[i].name) && vo[k].index == fi[i].index) { po->link_src[fi[i].reg] = vo[k].reg; break; }
    }
    if (po->pos_reg < 0) { set_log(po, "ERROR: the vertex shader has no gl_Position\n"); unlink_prog(po); return; }
    for (int st = 0; st < 2; st++) for (int k = 0; k < 16; k++) po->unit[st][k] = 0;
    po->linked = true;
    set_log(po, "");
    klog("w32: GLSL program %u linked (%d + %d instructions, %d uniforms)", p, vs->ninstr, fs->ninstr, po->nuni);
}
static void WINAPI glUseProgram(GLuint p) {
    G; GS;
    if (p && !obj(g, p, 2)) { err(g, 0x0501); return; }
    GLuint old = s->cur;
    s->cur = p;
    sl_obj_t *o = obj(g, old, 2);
    if (o && o->deleted && old != p) glDeleteShader(old);
}
static void WINAPI glValidateProgram(GLuint p) { (void)p; }
static sl_obj_t *cur_prog(glctx_t *g) {
    struct glsl_state *s = g->x ? g->x->sl : NULL;
    if (!s || !s->cur) return NULL;
    sl_obj_t *o = obj(g, s->cur, 2);
    return o && o->linked ? o : NULL;
}

/* ---------------- queries ---------------- */
static void WINAPI glGetShaderiv(GLuint id, GLenum p, GLint *v) {
    G; sl_obj_t *o = obj(g, id, 0); if (!o) { err(g, 0x0501); return; }
    switch (p) {
    case 0x8B4E: *v = o->kind == 1 ? 0x8B48 : 0x8B40; break;                     /* GL_OBJECT_TYPE_ARB */
    case 0x8B4F: *v = o->kind == 1 ? (GLint)o->type : 0; break;                  /* GL_SHADER_TYPE */
    case 0x8B80: *v = o->deleted; break;
    case 0x8B81: *v = o->compiled; break;
    case 0x8B82: *v = o->linked; break;
    case 0x8B83: *v = 1; break;                                                  /* GL_VALIDATE_STATUS */
    case 0x8B84: *v = o->log && o->log[0] ? (GLint)strlen(o->log) + 1 : 0; break;
    case 0x8B85: *v = o->nsh; break;
    case 0x8B86: *v = o->nuni; break;                                            /* GL_ACTIVE_UNIFORMS */
    case 0x8B87: { int m = 0; for (int i = 0; i < o->nuni; i++) m = MAX(m, (int)strlen(o->uni[i].name) + 4); *v = m + 1; break; }
    case 0x8B88: *v = o->src ? (GLint)o->srclen + 1 : 0; break;
    case 0x8B89: *v = o->u[1] ? o->u[1]->nattribs : 0; break;                   /* GL_ACTIVE_ATTRIBUTES */
    case 0x8B8A: *v = 64; break;
    default: *v = 0;
    }
}
static void WINAPI glGetProgramiv(GLuint id, GLenum p, GLint *v) { glGetShaderiv(id, p, v); }
static void WINAPI glGetObjectParameteriv(GLuint id, GLenum p, GLint *v) { glGetShaderiv(id, p, v); }
static void WINAPI glGetObjectParameterfv(GLuint id, GLenum p, GLfloat *v) { GLint i = 0; glGetShaderiv(id, p, &i); *v = (float)i; }
static void WINAPI glGetShaderInfoLog(GLuint id, GLsizei max, GLsizei *len, char *out) {
    G; sl_obj_t *o = obj(g, id, 0);
    const char *l = o && o->log ? o->log : "";
    int n = MIN((int)strlen(l), max - 1);
    if (max > 0) { memcpy(out, l, MAX(n, 0)); out[MAX(n, 0)] = 0; }
    if (len) *len = MAX(n, 0);
}
static void WINAPI glGetShaderSource(GLuint id, GLsizei max, GLsizei *len, char *out) {
    G; sl_obj_t *o = obj(g, id, 1);
    const char *l = o && o->src ? o->src : "";
    int n = MIN((int)strlen(l), max - 1);
    if (max > 0) { memcpy(out, l, MAX(n, 0)); out[MAX(n, 0)] = 0; }
    if (len) *len = MAX(n, 0);
}
static void WINAPI glGetAttachedShaders(GLuint p, GLsizei max, GLsizei *count, GLuint *out) {
    G; sl_obj_t *o = obj(g, p, 2); int n = 0;
    if (o) for (int i = 0; i < o->nsh && n < max; i++) out[n++] = o->sh[i];
    if (count) *count = n;
}
static GLuint WINAPI glGetHandle(GLenum p) { GV(0); GSV(0); return p == 0x8B40 ? s->cur : 0; }
static GLint WINAPI glGetAttribLocation(GLuint p, const char *name) {
    GV(-1); sl_obj_t *o = obj(g, p, 2); if (!o || !o->linked || !name) return -1;
    glsl_unit_t *vs = o->u[1];
    for (int i = 0; i < vs->nattribs; i++) if (!strcmp(vs->attribs[i].name, name)) return builtin_slot(name) >= 0 ? -1 : o->attr_loc[vs->attribs[i].reg];
    return -1;
}
static void WINAPI glGetActiveAttrib(GLuint p, GLuint i, GLsizei max, GLsizei *len, GLint *size, GLenum *type, char *name) {
    G; sl_obj_t *o = obj(g, p, 2); if (!o || !o->linked || (int)i >= o->u[1]->nattribs) { err(g, 0x0501); return; }
    glsl_var_t *a = &o->u[1]->attribs[i];
    if (size) *size = a->count; if (type) *type = a->gltype;
    int n = MIN((int)strlen(a->name), max - 1); if (max > 0) { memcpy(name, a->name, MAX(n, 0)); name[MAX(n, 0)] = 0; }
    if (len) *len = MAX(n, 0);
}
static void WINAPI glGetActiveUniform(GLuint p, GLuint i, GLsizei max, GLsizei *len, GLint *size, GLenum *type, char *name) {
    G; sl_obj_t *o = obj(g, p, 2); if (!o || !o->linked || (int)i >= o->nuni) { err(g, 0x0501); return; }
    sl_uni_t *u = &o->uni[i];
    char b[104]; snprintf(b, sizeof b, u->count > 1 ? "%s[0]" : "%s", u->name);
    if (size) *size = u->count; if (type) *type = u->gltype;
    int n = MIN((int)strlen(b), max - 1); if (max > 0) { memcpy(name, b, MAX(n, 0)); name[MAX(n, 0)] = 0; }
    if (len) *len = MAX(n, 0);
}
/* a location: uniform index * 1024 + array element */
static GLint WINAPI glGetUniformLocation(GLuint p, const char *name) {
    GV(-1); sl_obj_t *o = obj(g, p, 2); if (!o || !o->linked || !name) return -1;
    char base[96]; strlcpy(base, name, sizeof base);
    int elem = 0;
    size_t l = strlen(base);
    if (l && base[l - 1] == ']') {
        char *b = strrchr(base, '[');
        if (b) { elem = atoi(b + 1); *b = 0; }
    }
    for (int i = 0; i < o->nuni; i++) {
        if (!strcmp(o->uni[i].name, base) && elem < o->uni[i].count) return i * 1024 + elem;
        /* "s[2].f" names an element of a structure array, which is its own uniform */
        if (!strcmp(o->uni[i].name, name)) return i * 1024;
    }
    return -1;
}
static GLint WINAPI glGetFragDataLocation(GLuint p, const char *n) { (void)p; (void)n; return 0; }
static void WINAPI glBindFragDataLocation(GLuint p, GLuint c, const char *n) { (void)p; (void)c; (void)n; }

/* ---------------- setting uniforms ---------------- */
static bool is_int_type(uint32_t t) { return t == 0x1404 || (t >= 0x8B53 && t <= 0x8B55) || t == 0x1405 || (t >= 0x8DC6 && t <= 0x8DC8); }
static bool is_bool_type(uint32_t t) { return t >= 0x8B56 && t <= 0x8B59; }
static bool is_sampler_type(uint32_t t) { return (t >= 0x8B5D && t <= 0x8B64) || t == 0x8DC1 || t == 0x8DCA || t == 0x8DD2; }
static int type_cols(uint32_t t) {
    switch (t) { case 0x8B5A: case 0x8B65: case 0x8B66: return 2; case 0x8B5B: case 0x8B67: case 0x8B68: return 3; case 0x8B5C: case 0x8B69: case 0x8B6A: return 4; }
    return 1;
}
static int type_rows(uint32_t t) {
    switch (t) {
    case 0x8B5A: case 0x8B67: case 0x8B69: return 2; case 0x8B5B: case 0x8B65: case 0x8B6A: return 3; case 0x8B5C: case 0x8B66: case 0x8B68: return 4;
    case 0x8B50: case 0x8B53: case 0x8B57: case 0x8DC6: return 2; case 0x8B51: case 0x8B54: case 0x8B58: case 0x8DC7: return 3; case 0x8B52: case 0x8B55: case 0x8B59: case 0x8DC8: return 4;
    }
    return 1;
}
/* n elements of `comps` values each (floats or ints), at a location */
static void set_uniform(GLint loc, int n, int comps, const void *data, bool ints, bool transpose) {
    G; if (loc < 0) return;
    sl_obj_t *o = obj(g, SL(g) ? SL(g)->cur : 0, 2);
    if (!o || !o->linked) { err(g, 0x0502); return; }
    int ui = loc / 1024, el = loc % 1024;
    if (ui >= o->nuni) { err(g, 0x0502); return; }
    sl_uni_t *u = &o->uni[ui];
    int cols = type_cols(u->gltype), rows = type_rows(u->gltype);
    if (cols * rows != comps && !(cols == 1 && comps == rows)) { if (comps != 1 || cols * rows != 1) { err(g, 0x0502); return; } }
    for (int e = 0; e < n && el + e < u->count; e++) {
        const uint8_t *src = (const uint8_t *)data + (size_t)e * comps * 4;
        if (is_sampler_type(u->gltype)) {
            int unit = ints ? *(const int32_t *)src : (int)*(const float *)src;
            for (int st = 0; st < 2; st++) if (u->reg[st] >= 0 && u->reg[st] + el + e < 16) o->unit[st][u->reg[st] + el + e] = unit;
            continue;
        }
        for (int st = 0; st < 2; st++) {
            if (u->reg[st] < 0) continue;
            for (int c = 0; c < cols; c++) {
                int row = u->reg[st] + (el + e) * u->stride[st] + c;
                if (row < 0 || row >= o->rows[st]) continue;
                vec4_t *d = &o->cb[st][row];
                for (int r = 0; r < rows; r++) {
                    int k = transpose ? r * cols + c : c * rows + r;
                    uint32_t bits; memcpy(&bits, src + k * 4, 4);
                    if (is_bool_type(u->gltype)) { float f; memcpy(&f, &bits, 4); d->u[r] = (ints ? bits != 0 : f != 0) ? ~0u : 0; }
                    else if (is_int_type(u->gltype)) d->i[r] = ints ? (int32_t)bits : (int32_t)*(const float *)(src + k * 4);
                    else d->f[r] = ints ? (float)(int32_t)bits : *(const float *)(src + k * 4);
                }
            }
        }
    }
}
#define UF(n, k) static void WINAPI glUniform##n##fv(GLint l, GLsizei c, const GLfloat *v) { set_uniform(l, c, k, v, false, false); } \
                 static void WINAPI glUniform##n##iv(GLint l, GLsizei c, const GLint *v) { set_uniform(l, c, k, v, true, false); }
UF(1, 1) UF(2, 2) UF(3, 3) UF(4, 4)
static void WINAPI glUniform1f(GLint l, GLfloat a) { glUniform1fv(l, 1, &a); }
static void WINAPI glUniform2f(GLint l, GLfloat a, GLfloat b) { GLfloat v[2] = { a, b }; glUniform2fv(l, 1, v); }
static void WINAPI glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { GLfloat v[3] = { a, b, c }; glUniform3fv(l, 1, v); }
static void WINAPI glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { GLfloat v[4] = { a, b, c, d }; glUniform4fv(l, 1, v); }
static void WINAPI glUniform1i(GLint l, GLint a) { glUniform1iv(l, 1, &a); }
static void WINAPI glUniform2i(GLint l, GLint a, GLint b) { GLint v[2] = { a, b }; glUniform2iv(l, 1, v); }
static void WINAPI glUniform3i(GLint l, GLint a, GLint b, GLint c) { GLint v[3] = { a, b, c }; glUniform3iv(l, 1, v); }
static void WINAPI glUniform4i(GLint l, GLint a, GLint b, GLint c, GLint d) { GLint v[4] = { a, b, c, d }; glUniform4iv(l, 1, v); }
static void WINAPI glUniform1ui(GLint l, GLuint a) { glUniform1iv(l, 1, (const GLint *)&a); }
#define UM(n, k) static void WINAPI glUniformMatrix##n##fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { set_uniform(l, c, k, v, false, t); }
UM(2, 4) UM(3, 9) UM(4, 16) UM(2x3, 6) UM(3x2, 6) UM(2x4, 8) UM(4x2, 8) UM(3x4, 12) UM(4x3, 12)
static void WINAPI glGetUniformfv(GLuint p, GLint loc, GLfloat *out) {
    G; sl_obj_t *o = obj(g, p, 2); if (!o || !o->linked || loc < 0 || loc / 1024 >= o->nuni) { err(g, 0x0502); return; }
    sl_uni_t *u = &o->uni[loc / 1024]; int el = loc % 1024, st = u->reg[1] >= 0 ? 1 : 0;
    if (u->reg[st] < 0 || is_sampler_type(u->gltype)) { out[0] = is_sampler_type(u->gltype) ? (float)o->unit[st][MAX(0, u->reg[st]) + el] : 0; return; }
    int cols = type_cols(u->gltype), rows = type_rows(u->gltype);
    for (int c = 0; c < cols; c++) { const vec4_t *d = &o->cb[st][u->reg[st] + el * u->stride[st] + c]; for (int r = 0; r < rows; r++) out[c * rows + r] = is_int_type(u->gltype) ? (float)d->i[r] : is_bool_type(u->gltype) ? (d->u[r] ? 1.0f : 0.0f) : d->f[r]; }
}
static void WINAPI glGetUniformiv(GLuint p, GLint loc, GLint *out) { GLfloat f[16]; glGetUniformfv(p, loc, f); for (int i = 0; i < 16; i++) out[i] = (GLint)f[i]; }

/* ---------------- built-in uniforms ---------------- */
static mat4 m4_transpose(mat4 a) { mat4 r; for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) r.m[c * 4 + k] = a.m[k * 4 + c]; return r; }
static mat4 m4_inverse(mat4 a) {
    const float *m = a.m; float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    mat4 r = m4_identity();
    if (det == 0) return r;
    for (int i = 0; i < 16; i++) r.m[i] = inv[i] / det;
    return r;
}
static void put_mat(vec4_t *rows, int at, int max, mat4 m, int n) {
    for (int c = 0; c < n; c++) if (at + c >= 0 && at + c < max) for (int r = 0; r < 4; r++) rows[at + c].f[r] = r < n ? m.m[c * 4 + r] : 0;
}
static void put4(vec4_t *rows, int at, int max, const float *v) { if (at >= 0 && at < max) memcpy(rows[at].f, v, 16); }
static void put1(vec4_t *rows, int at, int max, float v) { if (at >= 0 && at < max) rows[at].f[0] = v; }
/* every gl_* uniform of a stage, from the GL state */
static void builtin_uniforms(glctx_t *g, struct glsl_state *s, sl_obj_t *o, int st, bool flip) {
    vec4_t *cb = o->cb[st]; int max = o->rows[st];
    glsl_unit_t *u = o->u[st];
    mat4 mv = g->mv[g->mvd], pj = g->pj[g->pjd];
    if (flip) pj = m4_mul(m4_scale(1, -1, 1), pj);           /* drawing into a texture: GL's bottom row first */
    (void)flip;
    bool cm = (g->en & EN_COLORMAT) != 0;
    for (int i = 0; i < u->nuniforms; i++) {
        glsl_var_t *v = &u->uniforms[i];
        const char *n = v->name;
        if (strncmp(n, "gl_", 3)) continue;
        int at = v->reg;
        n += 3;
        if (!strncmp(n, "ModelViewProjectionMatrix", 25)) {
            mat4 m = m4_mul(g->pj[g->pjd], mv); const char *t = n + 25;
            if (!strcmp(t, "Inverse")) m = m4_inverse(m); else if (!strcmp(t, "Transpose")) m = m4_transpose(m); else if (!strcmp(t, "InverseTranspose")) m = m4_transpose(m4_inverse(m));
            put_mat(cb, at, max, m, 4);
        } else if (!strncmp(n, "ModelViewMatrix", 15)) {
            mat4 m = mv; const char *t = n + 15;
            if (!strcmp(t, "Inverse")) m = m4_inverse(m); else if (!strcmp(t, "Transpose")) m = m4_transpose(m); else if (!strcmp(t, "InverseTranspose")) m = m4_transpose(m4_inverse(m));
            put_mat(cb, at, max, m, 4);
        } else if (!strncmp(n, "ProjectionMatrix", 16)) {
            mat4 m = g->pj[g->pjd]; const char *t = n + 16;
            if (!strcmp(t, "Inverse")) m = m4_inverse(m); else if (!strcmp(t, "Transpose")) m = m4_transpose(m); else if (!strcmp(t, "InverseTranspose")) m = m4_transpose(m4_inverse(m));
            put_mat(cb, at, max, m, 4);
        } else if (!strncmp(n, "TextureMatrix", 13)) {
            const char *t = n + 13;
            for (int k = 0; k < v->count; k++) {
                mat4 m = k == 0 ? g->tx[g->txd] : m4_identity();
                if (!strcmp(t, "Inverse")) m = m4_inverse(m); else if (!strcmp(t, "Transpose")) m = m4_transpose(m); else if (!strcmp(t, "InverseTranspose")) m = m4_transpose(m4_inverse(m));
                put_mat(cb, at + k * 4, max, m, 4);
            }
        } else if (!strcmp(n, "NormalMatrix")) {
            put_mat(cb, at, max, m4_transpose(m4_inverse(mv)), 3);
        } else if (!strcmp(n, "NormalScale")) {
            put1(cb, at, max, 1);
        } else if (!strncmp(n, "LightSource[", 12)) {
            int k = atoi(n + 12); if (k < 0 || k >= 8) continue;
            const char *f = strchr(n, '.'); if (!f) continue; f++;
            typeof(s->light[0]) *l = &s->light[k];
            if (!strcmp(f, "ambient")) put4(cb, at, max, l->amb); else if (!strcmp(f, "diffuse")) put4(cb, at, max, l->dif); else if (!strcmp(f, "specular")) put4(cb, at, max, l->spec);
            else if (!strcmp(f, "position")) put4(cb, at, max, l->pos);
            else if (!strcmp(f, "halfVector")) {
                float h[4] = { l->pos[0], l->pos[1], l->pos[2] + 1, 0 }, len = r3d_sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                if (len > 0) for (int c = 0; c < 3; c++) h[c] /= len;
                put4(cb, at, max, h);
            }
            else if (!strcmp(f, "spotDirection")) { float d[4] = { l->sdir[0], l->sdir[1], l->sdir[2], 0 }; put4(cb, at, max, d); }
            else if (!strcmp(f, "spotExponent")) put1(cb, at, max, l->sexp); else if (!strcmp(f, "spotCutoff")) put1(cb, at, max, l->scut);
            else if (!strcmp(f, "spotCosCutoff")) put1(cb, at, max, r3d_cos(l->scut * 3.14159265f / 180));
            else if (!strcmp(f, "constantAttenuation")) put1(cb, at, max, l->att[0]); else if (!strcmp(f, "linearAttenuation")) put1(cb, at, max, l->att[1]);
            else if (!strcmp(f, "quadraticAttenuation")) put1(cb, at, max, l->att[2]);
        } else if (!strncmp(n, "FrontMaterial.", 14) || !strncmp(n, "BackMaterial.", 13)) {
            int face = n[0] == 'B'; const char *f = strchr(n, '.') + 1;
            float amb[4], dif[4]; memcpy(amb, s->mat[face].amb, 16); memcpy(dif, s->mat[face].dif, 16);
            if (cm) { memcpy(amb, g->col, 16); memcpy(dif, g->col, 16); }
            if (!strcmp(f, "emission")) put4(cb, at, max, s->mat[face].emi); else if (!strcmp(f, "ambient")) put4(cb, at, max, amb); else if (!strcmp(f, "diffuse")) put4(cb, at, max, dif);
            else if (!strcmp(f, "specular")) put4(cb, at, max, s->mat[face].spec); else if (!strcmp(f, "shininess")) put1(cb, at, max, s->mat[face].shin);
        } else if (!strcmp(n, "LightModel.ambient")) {
            put4(cb, at, max, s->lm_amb);
        } else if (!strncmp(n, "FrontLightModelProduct", 22) || !strncmp(n, "BackLightModelProduct", 21)) {
            int face = n[0] == 'B';
            const float *amb = cm ? g->col : s->mat[face].amb;
            float c[4]; for (int k = 0; k < 4; k++) c[k] = s->mat[face].emi[k] + amb[k] * s->lm_amb[k];
            c[3] = (cm ? g->col : s->mat[face].dif)[3];
            put4(cb, at, max, c);
        } else if (!strncmp(n, "FrontLightProduct[", 18) || !strncmp(n, "BackLightProduct[", 17)) {
            int face = n[0] == 'B';
            int k = atoi(strchr(n, '[') + 1); if (k < 0 || k >= 8) continue;
            const char *f = strchr(n, '.'); if (!f) continue; f++;
            const float *a, *b;
            if (!strcmp(f, "ambient")) { a = s->light[k].amb; b = cm ? g->col : s->mat[face].amb; }
            else if (!strcmp(f, "diffuse")) { a = s->light[k].dif; b = cm ? g->col : s->mat[face].dif; }
            else { a = s->light[k].spec; b = s->mat[face].spec; }
            float c[4]; for (int q = 0; q < 4; q++) c[q] = a[q] * b[q];
            put4(cb, at, max, c);
        } else if (!strncmp(n, "Fog.", 4)) {
            const char *f = n + 4;
            if (!strcmp(f, "color")) put4(cb, at, max, s->fog.color); else if (!strcmp(f, "density")) put1(cb, at, max, s->fog.density);
            else if (!strcmp(f, "start")) put1(cb, at, max, s->fog.start); else if (!strcmp(f, "end")) put1(cb, at, max, s->fog.end);
            else if (!strcmp(f, "scale")) put1(cb, at, max, s->fog.end != s->fog.start ? 1 / (s->fog.end - s->fog.start) : 0);
        } else if (!strncmp(n, "DepthRange.", 11)) {
            const char *f = n + 11;
            put1(cb, at, max, !strcmp(f, "near") ? s->depth_near : !strcmp(f, "far") ? s->depth_far : s->depth_far - s->depth_near);
        }
    }
}

/* ---------------- texture sampling ---------------- */
typedef struct { glctx_t *g; sl_obj_t *o; int st; } sctx_t;
static int wrap_coord(int i, int n, GLenum mode) {
    if (mode == 0x812F || mode == 0x2900 || mode == 0x812D) return CLAMP(i, 0, n - 1);          /* clamp to edge (and clamp, border) */
    if (mode == 0x8370) { int p = n * 2; i %= p; if (i < 0) i += p; return i < n ? i : p - 1 - i; } /* mirrored repeat */
    i %= n; if (i < 0) i += n; return i;                                                         /* repeat */
}
static canvas_t *unit_texture(sctx_t *c, int slot, gltex_t **t_out) {
    glctx_t *g = c->g;
    int unit = slot >= 0 && slot < 16 ? c->o->unit[c->st][slot] : 0;
    GLuint id = g->x && unit >= 0 && unit < 16 ? g->x->unit_tex[unit] : 0;
    if (!id || id >= NTEX || !g->tex[id].c) return NULL;
    *t_out = &g->tex[id];
    return g->tex[id].c;
}
static void texel_rgba(canvas_t *c, int x, int y, float *o) {
    uint32_t v = c->px[(size_t)y * c->stride + x];
    o[0] = ((v >> 16) & 255) / 255.0f; o[1] = ((v >> 8) & 255) / 255.0f; o[2] = (v & 255) / 255.0f; o[3] = (v >> 24) / 255.0f;
}
/* bilinear (or nearest) sampling in fixed point: 8-bit weights, whole texels read once */
#define SL_PART 1
#include "glsl_px.inc"
#undef SL_PART
static void sl_load(void *ctx, int ti, const int32_t *xy, float *out) {
    gltex_t *t; canvas_t *c = unit_texture(ctx, ti, &t);
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    if (!c || xy[0] < 0 || xy[1] < 0 || xy[0] >= c->w || xy[1] >= c->h) return;
    texel_rgba(c, xy[0], xy[1], out);
}
static void sl_size(void *ctx, int ti, uint32_t *out) {
    gltex_t *t; canvas_t *c = unit_texture(ctx, ti, &t);
    out[0] = c ? c->w : 0; out[1] = c ? c->h : 0; out[2] = 1; out[3] = 1;
}

/* ---------------- the pipeline ---------------- */
#define SLV 16                                  /* varyings (vec4) between the stages */
typedef struct { float pos[4]; vec4_t v[SLV]; } pv_t;
typedef struct { float x, y, z, iw; vec4_t v[SLV]; } sv_t;
typedef struct {
    glctx_t *g; struct glsl_state *s; sl_obj_t *o;
    dxbc_env_t env[2]; sctx_t sc[2];
    dxbc_exec_t vx, fx;
    vec4_t vin[DX_MAX_REG], vout[DX_MAX_REG], fin[DX_MAX_REG], fout[DX_MAX_REG];
    int nvar; int vreg[SLV], freg[SLV]; uint8_t flat[SLV], noper[SLV], lanes[SLV];
    vec4_t flatv[SLV];
    int fpos, fface;
    tgt_t t; rect_t clip;
    float vx0, vy0, vw, vh, zn, zf;
    bool cull_front, cull_back, front_ccw_screen, depth_test, depth_write, blend, alpha_test, any_mask;
    GLenum dfunc;
    pv_t poly[2][12];
    /* the draw's triangles: each worker sets up a share into its bin, then every worker
     * rasterises all bins, in order */
    struct sl_bin *bins;
    int np, lreg[SLV * 4], lcomp[SLV * 4]; uint8_t lpers[SLV * 4];   /* interpolated lanes */
    int nflat, freg_flat[SLV], kflat[SLV];                          /* flat varyings */
    struct sl_wk *wk; int nwk;
} pipe_t;
/* one triangle: its rows, edges, z and 1/w planes; its lane planes and flat values in the pool */
typedef struct sl_tri { int x0, x1, y0, y1; float vx[3], vy[3], slope[3]; float zo, zdx, zdy, io, idx, idy; uint32_t front; size_t off; } sl_tri_t;
typedef struct sl_bin { sl_tri_t *tris; int n, cap; float *pool; size_t npool, cappool; bool grow, overflow; pv_t poly[2][12]; vec4_t flatv[SLV]; } sl_bin_t;
typedef struct { const pv_t *a, *b, *c; } sl_tri3_t;
/* one worker's registers */

typedef struct sl_wk {
    pipe_t *P; dxbc_exec_t fx, vx; vec4_t fin[DX_MAX_REG], fout[DX_MAX_REG], vin[DX_MAX_REG], vout[DX_MAX_REG]; uint32_t pixels;
    /* four pixels at a time, where the fragment shader compiles that way: registers as
     * component vectors (register r, component c, pixel l at 16 * r + 4 * c + l) */
    dxbc_quad_t *q; float *qin, *qout;
} sl_wk_t;
static bool vertex_slot(pipe_t *P, int slot, uint32_t index, const fatvert_t *imm, float *out) {
    glctx_t *g = P->g; struct gl2 *x = g->x; struct glsl_state *s = P->s;
    if (imm) { memcpy(out, imm->a[slot], 16); return true; }
    glvao_t *va = cur_vao(x);
    const arr_t *a = NULL; arr_t tmp;
    if (slot >= SL_GEN0) {
        vattr_t *at = &va->a[slot - SL_GEN0];
        if (at->on) {
            const uint8_t *base = resolve_ptr(x, at->buf, at->ptr);
            if (!base) return false;
            for (int c = 0; c < 4; c++) out[c] = c < at->size ? attr_fetch(base, at, index, c) : c == 3 ? 1 : 0;
            return true;
        }
        memcpy(out, x->cur[slot - SL_GEN0], 16);
        return true;
    }
    switch (slot) {
    case SL_VERTEX:
        if (g->cs & CS_VERTEX) a = &g->va;
        else if (va->a[0].on) return vertex_slot(P, SL_GEN0, index, NULL, out);
        break;
    case SL_NORMAL: if (g->cs & CS_NORMAL) a = &g->na; else { out[0] = g->nrm[0]; out[1] = g->nrm[1]; out[2] = g->nrm[2]; out[3] = 1; return true; } break;
    case SL_COLOR: if (g->cs & CS_COLOR) a = &g->ca; else { memcpy(out, g->col, 16); return true; } break;
    default:
        if (slot >= SL_TEX0 && slot < SL_TEX0 + 8) {
            int u = slot - SL_TEX0;
            if (u == 0 && (g->cs & CS_TEX)) a = &g->ta;
            else if (u && (s->tc_on & (1u << u))) a = &s->tca[u];
            else { memcpy(out, s->tc[u], 16); if (!u) { out[0] = g->tc[0]; out[1] = g->tc[1]; } return true; }
        } else { out[0] = out[1] = out[2] = 0; out[3] = slot == SL_COLOR2 ? 1 : 0; return true; }
    }
    if (!a) { out[0] = out[1] = out[2] = 0; out[3] = 1; return true; }
    tmp = *a;
    if (tmp.buf) { tmp.ptr = resolve_ptr(x, tmp.buf, (uintptr_t)tmp.ptr); if (!tmp.ptr) return false; }
    for (int c = 0; c < 4; c++) out[c] = fetch(&tmp, (int)index, c);
    if (slot == SL_COLOR && (tmp.type == 0x1401 || tmp.type == 0x1400) && tmp.size == 4 && 0) {}
    return true;
}
/* one fragment at canvas (x, y) whose inputs are set: run the shader, test, blend, write */
/* rows go to the workers in bands of 8: worker w takes the bands whose number is w mod n */
#define SL_BAND 3
static void fragment(sl_wk_t *W, int x, int y, float z);
static void sl_yield(void);
static bool sl_sse2(void);
#define SL_PART 2
#include "glsl_px.inc"
#undef SL_PART
static void fragment(sl_wk_t *W, int x, int y, float z) {
    pipe_t *P = W->P;
    float *dz = P->t.depth + (size_t)y * P->t.w + x;       /* holds 1 - z: 0 is the far plane */
    if (P->depth_test && !test_depth_x87(P->dfunc, z, 1 - *dz)) return;
    if (P->fpos >= 0) { vec4_t *r = &W->fin[P->fpos]; r->f[0] = x + .5f; r->f[1] = (P->t.flip ? y : P->t.h - 1 - y) + .5f; r->f[2] = z; }
    if (!dxbc_exec_run(&W->fx)) return;
    frag_out_x87(W, x, y, z, W->fout[P->o->target_reg].f);
}
/* a shaded pixel (colour c) through the alpha test, depth write, blending and colour mask */
/* kernel tasks share the processor by yielding: a long draw lets the desktop run every few ms */
static uint32_t sl_last_yield;
/* the first frame, if it takes longer than a second, is shown as it is drawn, once a
 * second, rather than leaving the window empty */
static uint32_t sl_partial_at; static glctx_t *sl_partial_g;
static void sl_yield(void) {
    uint32_t t = timer_ticks();
    if (t - sl_last_yield >= 8) { task_yield(); sl_last_yield = t = timer_ticks(); }
    if (sl_partial_g && sl_partial_g->x && sl_partial_g->x->sl && !sl_partial_g->x->sl->frames && !sl_partial_g->fbo_bound && t - sl_partial_g->x->sl->frame_start > 1000 && t - sl_partial_at >= 1000) { sl_partial_at = t; present(sl_partial_g); }
}
/* the vertex shader on the vertices a draw uses, split between the workers */
typedef struct { pipe_t *P; pv_t *cache; uint8_t *done; uint32_t lo, span; const fatvert_t *imm; } vjob_t;
typedef struct { pipe_t *P; const sl_tri3_t *tri; int ntri; } sjob_t;
static void raster_job(void *arg, int w, int n) {
    pipe_t *P = arg;
    sl_wk_t *W = &P->wk[w < P->nwk ? w : 0];
    if (w >= P->nwk) return;
    void (*rt)(sl_wk_t *, const sl_bin_t *, const sl_tri_t *, int, int) = sl_sse2() ? raster_tri_sse : raster_tri_x87;
    for (int b = 0; b < P->nwk; b++) { const sl_bin_t *B = &P->bins[b]; for (int i = 0; i < B->n; i++) rt(W, B, &B->tris[i], w, n); }
}
/* SSE2 present (the 64-bit edition always has it): the SSE copies of the pixel code run */
static bool sl_sse2(void) {
#ifdef __i386__
    static int have = -1;
    if (have < 0) { uint32_t a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0)); have = (d >> 26) & 1; }
    return have;
#else
    return true;
#endif
}
#define SL_PART 3
#include "glsl_px.inc"
#undef SL_PART
/* lines and points: one pixel wide, varyings interpolated along the line */
static void draw_point_sv(pipe_t *P, const sv_t *s) {
    int x = (int)s->x, y = (int)s->y;
    if (x < P->clip.x || y < P->clip.y || x >= P->clip.x + P->clip.w || y >= P->clip.y + P->clip.h || s->z < 0 || s->z > 1) return;
    float w = s->iw != 0 ? 1 / s->iw : 0;
    sl_wk_t *W = &P->wk[0];
    for (int k = 0; k < P->nvar; k++) for (int i = 0; i < 4; i++) W->fin[P->freg[k]].f[i] = P->flat[k] ? s->v[k].f[i] : s->v[k].f[i] * (P->noper[k] ? 1 : w);
    if (P->fpos >= 0) W->fin[P->fpos].f[3] = s->iw;
    if (P->fface >= 0) W->fin[P->fface].u[0] = ~0u;
    fragment(W, x, y, s->z);
}
static void draw_line_pv(pipe_t *P, const pv_t *a, const pv_t *b) {
    pv_t c0 = *a, c1 = *b;
    for (int p = 0; p < 7; p++) {
        float d0 = clip_dist_x87(&c0, p), d1 = clip_dist_x87(&c1, p);
        if (d0 < 0 && d1 < 0) return;
        if (d0 < 0) { pv_t t; lerp_pv_x87(P, &c0, &c1, d0 / (d0 - d1), &t); c0 = t; }
        else if (d1 < 0) { pv_t t; lerp_pv_x87(P, &c1, &c0, d1 / (d1 - d0), &t); c1 = t; }
    }
    sv_t s0, s1; to_screen_x87(P, &c0, &s0); to_screen_x87(P, &c1, &s1);
    float dxl = s1.x - s0.x, dyl = s1.y - s0.y;
    int n = (int)MAX(dxl < 0 ? -dxl : dxl, dyl < 0 ? -dyl : dyl) + 1;
    if (n > 8192) n = 8192;
    for (int i = 0; i < n; i++) {
        float t = n > 1 ? (float)i / (n - 1) : 0;
        sv_t s; s.x = s0.x + (s1.x - s0.x) * t; s.y = s0.y + (s1.y - s0.y) * t; s.z = s0.z + (s1.z - s0.z) * t; s.iw = s0.iw + (s1.iw - s0.iw) * t;
        for (int k = 0; k < P->nvar; k++) for (int q = 0; q < 4; q++) s.v[k].f[q] = s0.v[k].f[q] + (s1.v[k].f[q] - s0.v[k].f[q]) * t;
        draw_point_sv(P, &s);
    }
}
static void draw_point_pv(pipe_t *P, const pv_t *a) {
    for (int p = 0; p < 7; p++) if (clip_dist_x87(a, p) < 0) return;
    sv_t s; to_screen_x87(P, a, &s); draw_point_sv(P, &s);
}
static bool pipe_setup(pipe_t *P, glctx_t *g, sl_obj_t *o) {
    memset(P, 0, sizeof *P);
    P->g = g; P->s = g->x->sl; P->o = o;
    if (!target(g, &P->t)) return false;
    struct glsl_state *s = P->s;
    builtin_uniforms(g, s, o, 0, false); builtin_uniforms(g, s, o, 1, false);
    for (int st = 0; st < 2; st++) {
        dxbc_env_t *e = &P->env[st];
        e->cb[0] = o->cb[st]; e->cb_rows[0] = o->rows[st];
        P->sc[st] = (sctx_t){ g, o, st };
        e->ctx = &P->sc[st]; e->sample = sl_sample_x87; e->load = sl_load; e->size = sl_size;
    }
    /* varyings */
    P->fpos = P->fface = -1;
    const dxsig_t *fi; int nfi = dxbc_inputs(o->u[0]->shader, &fi);
    for (int i = 0; i < nfi; i++) {
        if (fi[i].sysval == 1) { P->fpos = fi[i].reg; continue; }
        if (fi[i].sysval == 9) { P->fface = fi[i].reg; continue; }
        int src = o->link_src[fi[i].reg];
        if (src < 0 || P->nvar >= SLV) continue;          /* not written by the vertex shader: zero */
        P->vreg[P->nvar] = src; P->freg[P->nvar] = fi[i].reg;
        int ip = dxbc_interpolation(o->u[0]->shader, fi[i].reg);
        P->flat[P->nvar] = ip == 1; P->noper[P->nvar] = ip == 4;
        P->lanes[P->nvar] = dxbc_read_mask(o->u[0]->shader, fi[i].reg) & 15;
        P->nvar++;
    }
    /* viewport, depth range, scissor */
    P->vx0 = (float)g->vx; P->vy0 = (float)g->vy; P->vw = (float)g->vw; P->vh = (float)g->vh;
    if (P->vw <= 0 || P->vh <= 0) return false;
    P->zn = s->depth_near; P->zf = s->depth_far;
    P->clip = scissor_rect(g, &P->t);
    P->clip = rect_intersect(P->clip, R(0, 0, P->t.w, P->t.h));
    if (P->clip.w <= 0 || P->clip.h <= 0) return false;
    bool cull = (g->en & EN_CULL) != 0;
    P->cull_back = cull && (g->cull_face == 0x0405 || g->cull_face == 0x0408);
    P->cull_front = cull && (g->cull_face == 0x0404 || g->cull_face == 0x0408);
    /* GL front faces wind counter-clockwise with y up; the window's rows run downwards */
    bool ccw_front = g->front_face != 0x0900;
    P->front_ccw_screen = P->t.flip ? ccw_front : !ccw_front;
    P->depth_test = (g->en & EN_DEPTH) != 0;
    P->depth_write = P->depth_test && g->depth_mask;
    P->dfunc = g->x->depth_func;
    P->blend = (g->en & EN_BLEND) != 0;
    P->alpha_test = (g->en & EN_ALPHA) != 0;
    P->any_mask = g->x->cmask[0] || g->x->cmask[1] || g->x->cmask[2] || g->x->cmask[3];
    /* the lanes that are interpolated, and the flat varyings */
    for (int k = 0; k < P->nvar; k++) {
        if (P->flat[k]) { P->freg_flat[P->nflat] = P->freg[k]; P->kflat[P->nflat++] = k; continue; }
        for (int i = 0; i < 4; i++) if (P->lanes[k] & (1u << i)) { P->lreg[P->np] = P->freg[k]; P->lcomp[P->np] = k << 2 | i; P->lpers[P->np] = !P->noper[k]; P->np++; }
    }
    if (!dxbc_exec_init(&P->vx, o->u[1]->shader, &P->env[1], P->vin, P->vout)) return false;
    /* one set of fragment registers per worker (the other processors take rows too) */
    P->nwk = MAX(1, smp_workers_n);
    P->wk = kzalloc(sizeof(sl_wk_t) * P->nwk);
    if (!P->wk) { dxbc_exec_fini(&P->vx); return false; }
    for (int i = 0; i < P->nwk; i++) {
        P->wk[i].P = P;
        if (!dxbc_exec_init(&P->wk[i].fx, o->u[0]->shader, &P->env[0], P->wk[i].fin, P->wk[i].fout)) {
            for (int k = 0; k < i; k++) { dxbc_exec_fini(&P->wk[k].fx); dxbc_exec_fini(&P->wk[k].vx); }
            kfree(P->wk); dxbc_exec_fini(&P->vx); return false;
        }
        if (!dxbc_exec_init(&P->wk[i].vx, o->u[1]->shader, &P->env[1], P->wk[i].vin, P->wk[i].vout)) {
            dxbc_exec_fini(&P->wk[i].fx);
            for (int k = 0; k < i; k++) { dxbc_exec_fini(&P->wk[k].fx); dxbc_exec_fini(&P->wk[k].vx); }
            kfree(P->wk); dxbc_exec_fini(&P->vx); return false;
        }
    }
    /* four pixels at a time if the fragment shader compiles that way for every worker */
    bool quad = true;
    for (int i = 0; i < P->nwk && quad; i++) {
        sl_wk_t *W = &P->wk[i];
        W->qin = kzalloc(sizeof(float) * 16 * DX_MAX_REG); W->qout = kzalloc(sizeof(float) * 16 * DX_MAX_REG);
        if (W->qin && W->qout) W->q = dxbc_quad_new(o->u[0]->shader, &P->env[0], W->qin, W->qout);
        quad = W->q != NULL;
    }
    if (!quad) for (int i = 0; i < P->nwk; i++) { sl_wk_t *W = &P->wk[i]; if (W->q) dxbc_quad_free(W->q); W->q = NULL; }
    else {
        P->env[0].sample = sl_sample_sse;                /* the four-pixel code needs SSE2 anyway */
        for (int i = 0; i < P->nwk; i++) dxbc_quad_sample4(P->wk[i].q, sl_sample4_sse);
    }
    return true;
}
static void pipe_free(pipe_t *P) {
    dxbc_exec_fini(&P->vx);
    for (int i = 0; i < P->nwk; i++) {
        sl_wk_t *W = &P->wk[i];
        dxbc_exec_fini(&W->fx); dxbc_exec_fini(&W->vx);
        if (W->q) dxbc_quad_free(W->q);
        kfree(W->qin); kfree(W->qout);
    }
    if (P->bins) for (int i = 0; i < P->nwk; i++) { kfree(P->bins[i].tris); kfree(P->bins[i].pool); }
    kfree(P->bins); kfree(P->wk); kfree(P);
}
/* assemble primitives of `mode` from n vertices (already through the vertex shader):
 * points and lines are drawn now; triangles go to tri[] (the third vertex provokes flat
 * values) and *ntri counts them */
static void assemble(pipe_t *P, GLenum mode, pv_t **v, int n, sl_tri3_t *tri, int *ntri) {
    #define FLAT(p) do { for (int k_ = 0; k_ < P->nvar; k_++) if (P->flat[k_]) P->flatv[k_] = (p)->v[k_]; } while (0)
    #define TRI(a_, b_, c_) do { if (v[a_] && v[b_] && v[c_]) { tri[*ntri].a = v[a_]; tri[*ntri].b = v[b_]; tri[*ntri].c = v[c_]; (*ntri)++; } } while (0)
    switch (mode) {
    case 0: for (int i = 0; i < n; i++) if (v[i]) { FLAT(v[i]); draw_point_pv(P, v[i]); } break;
    case 1: for (int i = 0; i + 1 < n; i += 2) if (v[i] && v[i + 1]) { FLAT(v[i + 1]); draw_line_pv(P, v[i], v[i + 1]); } break;
    case 2: case 3:
        for (int i = 0; i + 1 < n; i++) if (v[i] && v[i + 1]) { FLAT(v[i + 1]); draw_line_pv(P, v[i], v[i + 1]); }
        if (mode == 2 && n > 2 && v[n - 1] && v[0]) draw_line_pv(P, v[n - 1], v[0]);
        break;
    case 4: for (int i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2); break;
    case 5: for (int i = 0; i + 2 < n; i++) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); } break;
    case 6: case 9: for (int i = 1; i + 1 < n; i++) TRI(0, i, i + 1); break;
    case 7: for (int i = 0; i + 3 < n; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); } break;
    case 8: for (int i = 0; i + 3 < n; i += 2) { TRI(i, i + 1, i + 3); TRI(i, i + 3, i + 2); } break;
    }
    #undef TRI
    #undef FLAT
}
/* a draw with the program in use: vertices from arrays (imm NULL; idx NULL for glDrawArrays)
 * or from glBegin/glEnd (imm) */
static void sl_run(glctx_t *g, sl_obj_t *o, GLenum mode, int first, int count, GLenum type, const uint8_t *idx, const fatvert_t *imm) {
    if (count <= 0 || mode > 9) return;
    if (g->render_mode == GL_SELECT) return;
    pipe_t *P = kmalloc(sizeof(pipe_t));
    if (!P) return;
    if (!pipe_setup(P, g, o)) { kfree(P); return; }
    P->s->draws++; sl_partial_g = g;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (int i = 0; i < count; i++) {
        uint32_t k = imm ? (uint32_t)i : idx ? (type == 0x1401 ? idx[i] : type == 0x1403 ? ((const uint16_t *)idx)[i] : ((const uint32_t *)idx)[i]) : (uint32_t)(first + i);
        lo = MIN(lo, k); hi = MAX(hi, k);
    }
    uint32_t span = hi - lo + 1;
    if (span > (1u << 22)) { pipe_free(P); return; }
    pv_t *cache = kmalloc(sizeof(pv_t) * span);
    uint8_t *done = kzalloc(span);
    pv_t **list = kmalloc(sizeof(pv_t *) * count);
    if (!cache || !done || !list) { kfree(cache); kfree(done); kfree(list); pipe_free(P); return; }
    /* mark the vertices used, shade them on all workers, then list them in draw order */
    for (int i = 0; i < count; i++) {
        uint32_t k = imm ? (uint32_t)i : idx ? (type == 0x1401 ? idx[i] : type == 0x1403 ? ((const uint16_t *)idx)[i] : ((const uint32_t *)idx)[i]) : (uint32_t)(first + i);
        done[k - lo] = 1;
    }
    vjob_t vj = { P, cache, done, lo, span, imm };
    void (*vjf)(void *, int, int) = sl_sse2() ? vertex_job_sse : vertex_job_x87;
    if (span >= 64) smp_run(vjf, &vj); else vjf(&vj, 0, 1);
    for (int i = 0; i < count; i++) {
        uint32_t k = imm ? (uint32_t)i : idx ? (type == 0x1401 ? idx[i] : type == 0x1403 ? ((const uint16_t *)idx)[i] : ((const uint32_t *)idx)[i]) : (uint32_t)(first + i);
        uint32_t s = k - lo;
        list[i] = done[s] == 2 ? &cache[s] : NULL;
    }
    sl_tri3_t *tri = mode >= 4 ? kmalloc(sizeof(sl_tri3_t) * (size_t)(count * 2 + 4)) : NULL;
    int ntri = 0;
    if (mode < 4 || tri) assemble(P, mode, list, count, tri, &ntri);
    bool any = false;
    if (ntri) {
        /* each worker's bin, sized for its share of triangles (clipping can add a few) */
        size_t need = (size_t)P->np * 3 + (size_t)P->nflat * 4;
        P->bins = kzalloc(sizeof(sl_bin_t) * P->nwk);
        bool ok = P->bins != NULL;
        for (int w = 0; ok && w < P->nwk; w++) {
            sl_bin_t *B = &P->bins[w];
            B->cap = ntri / P->nwk * 2 + 64; B->cappool = (size_t)B->cap * MAX(need, 1);
            B->tris = kmalloc(sizeof(sl_tri_t) * B->cap); B->pool = kmalloc(sizeof(float) * B->cappool);
            if (!B->tris || !B->pool) ok = false;
        }
        if (ok) {
            sjob_t sj = { P, tri, ntri };
            void (*sjf)(void *, int, int) = sl_sse2() ? setup_job_sse : setup_job_x87;
            if (ntri >= 32) smp_run(sjf, &sj); else sjf(&sj, 0, 1);
            bool over = false;
            for (int w = 0; w < P->nwk; w++) over |= P->bins[w].overflow;
            if (over) {                                   /* again, all on this processor, growing as needed */
                for (int w = 0; w < P->nwk; w++) { P->bins[w].n = 0; P->bins[w].npool = 0; P->bins[w].overflow = false; }
                P->bins[0].grow = true;
                sjf(&sj, 0, 1);
            }
            any = true;
        }
    }
    if (any) smp_run(raster_job, P);
    kfree(tri);
    for (int i = 0; i < P->nwk; i++) P->s->pixels += P->wk[i].pixels;
    kfree(cache); kfree(done); kfree(list);
    pipe_free(P);
}
static bool sl_draw(glctx_t *g, GLenum mode, GLint first, GLsizei count, GLenum type, const void *idx) {
    sl_obj_t *o = cur_prog(g);
    if (!o) return false;
    struct gl2 *x = g->x;
    const uint8_t *ip = idx;
    if (type) {
        if (cur_vao(x)->ebo) { ip = resolve_ptr(x, cur_vao(x)->ebo, (uintptr_t)idx); if (!ip) return true; }
        if (!ip) return true;
    }
    sl_run(g, o, mode, first, count, type, ip, NULL);
    return true;
}
/* glBegin/glEnd while a program is in use: the vertices as they were given */
static bool sl_active(glctx_t *g) { return cur_prog(g) != NULL; }
static void sl_imm_vertex(glctx_t *g, float x0, float y0, float z0, float w0) {
    struct glsl_state *s = g->x ? g->x->sl : NULL; if (!s) return;
    if (s->nimm == s->capimm) { int c = s->capimm ? s->capimm * 2 : 64; fatvert_t *n = krealloc(s->imm, sizeof(fatvert_t) * c); if (!n) return; s->imm = n; s->capimm = c; }
    fatvert_t *v = &s->imm[s->nimm++];
    v->a[SL_VERTEX][0] = x0; v->a[SL_VERTEX][1] = y0; v->a[SL_VERTEX][2] = z0; v->a[SL_VERTEX][3] = w0;
    v->a[SL_NORMAL][0] = g->nrm[0]; v->a[SL_NORMAL][1] = g->nrm[1]; v->a[SL_NORMAL][2] = g->nrm[2]; v->a[SL_NORMAL][3] = 1;
    memcpy(v->a[SL_COLOR], g->col, 16);
    v->a[SL_COLOR2][3] = 1; v->a[SL_COLOR2][0] = v->a[SL_COLOR2][1] = v->a[SL_COLOR2][2] = 0;
    v->a[SL_FOG][0] = 0;
    for (int u = 0; u < 8; u++) memcpy(v->a[SL_TEX0 + u], s->tc[u], 16);
    v->a[SL_TEX0][0] = g->tc[0]; v->a[SL_TEX0][1] = g->tc[1];
    for (int k = 0; k < NATTR; k++) memcpy(v->a[SL_GEN0 + k], g->x->cur[k], 16);
    v->a[SL_GEN0][0] = x0; v->a[SL_GEN0][1] = y0; v->a[SL_GEN0][2] = z0; v->a[SL_GEN0][3] = w0;
}
static bool sl_imm_end(glctx_t *g, int prim) {
    sl_obj_t *o = cur_prog(g);
    struct glsl_state *s = g->x ? g->x->sl : NULL;
    if (!o || !s) return false;
    sl_run(g, o, prim, 0, s->nimm, 0, NULL, s->imm);
    s->nimm = 0;
    return true;
}
/* a display list's geometry, drawn with the program */
static bool sl_draw_r3d(glctx_t *g, int prim, const r3d_vertex_t *rv, int n) {
    sl_obj_t *o = cur_prog(g);
    struct glsl_state *s = g->x ? g->x->sl : NULL;
    if (!o || !s || n <= 0) return false;
    fatvert_t *f = kmalloc(sizeof(fatvert_t) * n);
    if (!f) return true;
    for (int i = 0; i < n; i++) {
        float w = rv[i].w ? rv[i].w : 1;
        fatvert_t *v = &f[i];
        float pos[4] = { rv[i].x, rv[i].y, rv[i].z, w };
        memcpy(v->a[SL_VERTEX], pos, 16);
        v->a[SL_NORMAL][0] = rv[i].nx; v->a[SL_NORMAL][1] = rv[i].ny; v->a[SL_NORMAL][2] = rv[i].nz; v->a[SL_NORMAL][3] = 1;
        uint32_t c = rv[i].color;
        v->a[SL_COLOR][0] = ((c >> 16) & 255) / 255.0f; v->a[SL_COLOR][1] = ((c >> 8) & 255) / 255.0f; v->a[SL_COLOR][2] = (c & 255) / 255.0f; v->a[SL_COLOR][3] = (c >> 24) / 255.0f;
        v->a[SL_COLOR2][0] = v->a[SL_COLOR2][1] = v->a[SL_COLOR2][2] = 0; v->a[SL_COLOR2][3] = 1; v->a[SL_FOG][0] = 0;
        for (int u = 0; u < 8; u++) memcpy(v->a[SL_TEX0 + u], s->tc[u], 16);
        v->a[SL_TEX0][0] = rv[i].u; v->a[SL_TEX0][1] = rv[i].v; v->a[SL_TEX0][2] = 0; v->a[SL_TEX0][3] = 1;
        for (int k = 0; k < NATTR; k++) memcpy(v->a[SL_GEN0 + k], g->x->cur[k], 16);
        memcpy(v->a[SL_GEN0], pos, 16);
    }
    sl_run(g, o, prim, 0, n, 0, NULL, f);
    kfree(f);
    return true;
}
static void sl_free(glctx_t *g) {
    struct glsl_state *s = g->x ? g->x->sl : NULL; if (!s) return;
    for (int i = 0; i < s->no; i++) if (s->o[i]) free_obj(g, i);
    kfree(s->o); kfree(s->imm); kfree(s);
    g->x->sl = NULL;
}
/* texture coordinate arrays of units other than 0 (glClientActiveTexture) */
static void sl_texcoord_pointer(glctx_t *g, int unit, GLint size, GLenum type, GLsizei stride, const void *p) {
    GS; if (unit < 1 || unit >= 8) return;
    s->tca[unit] = (arr_t){ size, type, stride, p, g->x->abo };
}
static void sl_texcoord_array(glctx_t *g, int unit, bool on) {
    GS; if (unit < 1 || unit >= 8) return;
    if (on) s->tc_on |= 1u << unit; else s->tc_on &= ~(1u << unit);
}

static void WINAPI glDrawRangeElements(GLenum mode, GLuint lo, GLuint hi, GLsizei count, GLenum type, const void *idx) { (void)lo; (void)hi; glDrawElements(mode, count, type, idx); }
/* ARB assembly programs (GL_ARB_vertex_program): not implemented, accepted so programs that
 * also probe for them keep going */
static void WINAPI glBindProgram(GLenum t, GLuint p) { (void)t; (void)p; }
static void WINAPI glProgramLocalParameter4f(GLenum t, GLuint i, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { (void)t; (void)i; (void)a; (void)b; (void)c; (void)d; }
static void WINAPI glGenPrograms(GLsizei n, GLuint *out) { for (int i = 0; i < n; i++) out[i] = i + 1; }
static void WINAPI glDeletePrograms(GLsizei n, const GLuint *p) { (void)n; (void)p; }
static void WINAPI glProgramString(GLenum t, GLenum f, GLsizei l, const void *s) { (void)t; (void)f; (void)l; (void)s; }
static void sl_begin(glctx_t *g) { if (g->x && g->x->sl) g->x->sl->nimm = 0; }
static void sl_frame_done(glctx_t *g) {
    struct glsl_state *s = g->x ? g->x->sl : NULL;
    if (!s) return;
    uint32_t now = timer_ticks();
    if (s->draws && s->frames < 5) klog("w32: GLSL frame %u: %u draws, %u pixels shaded, %u ms", s->frames, s->draws, s->pixels, now - s->frame_start);
    if (s->draws) s->frames++;
    s->draws = s->pixels = 0; s->frame_start = now;
}
