/* OpenGL 2.x/3.x state for gl.c: buffer objects, vertex array objects and generic
 * vertex attributes. Included by gl.c (it uses glctx_t). The shading-language
 * part (shader and program objects, uniforms, the programmable draw) is in glsl.inc. */

static struct gl2 *X(glctx_t *g) {
    if (!g->x) {
        g->x = kzalloc(sizeof(struct gl2));
        if (!g->x) return NULL;
        for (int i = 0; i < NATTR; i++) g->x->cur[i][3] = 1;
        g->x->vao[0].used = true;
        g->x->bsrc = g->x->bsrc_a = 0x0302; g->x->bdst = g->x->bdst_a = 0x0303;   /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA: what the fixed path does */
        g->x->beq = g->x->beq_a = 0x8006;
        g->x->depth_func = 0x0201;
        for (int i = 0; i < 4; i++) g->x->cmask[i] = true;
    }
    return g->x;
}
#define GX struct gl2 *x = X(g); if (!x) return
#define GXV(v) struct gl2 *x = X(g); if (!x) return v
static glvao_t *cur_vao(struct gl2 *x) { return &x->vao[x->vao_bound < NVAO ? x->vao_bound : 0]; }
static glbuf_t *buf_of(struct gl2 *x, GLuint id) { return id && (int)id < x->nbuf && x->buf[id].used ? &x->buf[id] : NULL; }
/* where an array or index pointer points: into a buffer object, or the program's memory */
static const uint8_t *resolve_ptr(struct gl2 *x, GLuint buf, uintptr_t ptr) {
    if (!buf) return (const uint8_t *)ptr;
    glbuf_t *b = buf_of(x, buf);
    return b && b->data && ptr < b->size ? b->data + ptr : NULL;
}

/* ---------------- buffer objects ---------------- */
static void WINAPI glGenBuffers(GLsizei n, GLuint *out) {
    G; GX;
    for (int i = 0; i < n; i++) {
        int id = 1;
        while (id < x->nbuf && x->buf[id].used) id++;
        if (id >= x->nbuf) {
            int nn = MAX(64, x->nbuf * 2);
            if (nn > 65536) { out[i] = 0; err(g, 0x0505); continue; }
            glbuf_t *nb = krealloc(x->buf, sizeof(glbuf_t) * nn);
            if (!nb) { out[i] = 0; err(g, 0x0505); continue; }
            memset(nb + x->nbuf, 0, sizeof(glbuf_t) * (nn - x->nbuf));
            x->buf = nb; x->nbuf = nn;
        }
        x->buf[id].used = true; x->buf[id].data = NULL; x->buf[id].size = 0;
        out[i] = id;
    }
}
static void WINAPI glDeleteBuffers(GLsizei n, const GLuint *ids) {
    G; GX;
    for (int i = 0; i < n; i++) {
        glbuf_t *b = buf_of(x, ids[i]); if (!b) continue;
        kfree(b->data); memset(b, 0, sizeof *b);
        if (x->abo == ids[i]) x->abo = 0;
        for (int v = 0; v < NVAO; v++) { if (x->vao[v].ebo == ids[i]) x->vao[v].ebo = 0; for (int a = 0; a < NATTR; a++) if (x->vao[v].a[a].buf == ids[i]) x->vao[v].a[a].buf = 0; }
    }
}
static GLboolean WINAPI glIsBuffer(GLuint id) { GV(0); GXV(0); return buf_of(x, id) != NULL; }
static void WINAPI glBindBuffer(GLenum target_, GLuint id) {
    G; GX;
    if (id && !buf_of(x, id)) {            /* a name the program made up: create it, as GL 2 allows */
        while ((int)id >= x->nbuf) { GLuint t; glGenBuffers(1, &t); if (!t) return; if (t != id) x->buf[t].used = false; }
        x->buf[id].used = true;
    }
    if (target_ == 0x8892) x->abo = id;
    else if (target_ == 0x8893) cur_vao(x)->ebo = id;
}
static glbuf_t *bound_buf(struct gl2 *x, GLenum target_) {
    return buf_of(x, target_ == 0x8892 ? x->abo : target_ == 0x8893 ? cur_vao(x)->ebo : 0);
}
static void WINAPI glBufferData(GLenum target_, intptr_t size, const void *data, GLenum usage) {
    G; GX;
    glbuf_t *b = bound_buf(x, target_);
    if (!b) { err(g, 0x0502); return; }
    if (size < 0 || size > 256 * 1024 * 1024) { err(g, 0x0501); return; }
    uint8_t *d = (size_t)size == b->size && b->data ? b->data : krealloc(b->data, MAX(1, (size_t)size));
    if (!d) { err(g, 0x0505); return; }
    b->data = d; b->size = (uint32_t)size; b->usage = usage;
    if (data) memcpy(d, data, size); else memset(d, 0, size);
}
static void WINAPI glBufferSubData(GLenum target_, intptr_t off, intptr_t size, const void *data) {
    G; GX;
    glbuf_t *b = bound_buf(x, target_);
    if (!b || off < 0 || size < 0 || (uint64_t)off + size > b->size) { err(g, 0x0501); return; }
    if (data) memcpy(b->data + off, data, size);
}
static void WINAPI glGetBufferSubData(GLenum target_, intptr_t off, intptr_t size, void *data) {
    G; GX;
    glbuf_t *b = bound_buf(x, target_);
    if (!b || off < 0 || size < 0 || (uint64_t)off + size > b->size) { err(g, 0x0501); return; }
    memcpy(data, b->data + off, size);
}
static void *WINAPI glMapBuffer(GLenum target_, GLenum access) {
    (void)access; GV(NULL); GXV(NULL);
    glbuf_t *b = bound_buf(x, target_);
    if (!b) return NULL;
    b->mapped = true; return b->data;
}
static void *WINAPI glMapBufferRange(GLenum target_, intptr_t off, intptr_t len, GLbitfield access) {
    (void)len; (void)access; GV(NULL); GXV(NULL);
    glbuf_t *b = bound_buf(x, target_);
    if (!b || off < 0 || (uint64_t)off > b->size) return NULL;
    b->mapped = true; return b->data + off;
}
static GLboolean WINAPI glUnmapBuffer(GLenum target_) { GV(0); GXV(0); glbuf_t *b = bound_buf(x, target_); if (b) b->mapped = false; return 1; }
static void WINAPI glFlushMappedBufferRange(GLenum t, intptr_t o, intptr_t l) { (void)t; (void)o; (void)l; }
static void WINAPI glGetBufferParameteriv(GLenum target_, GLenum p, GLint *v) {
    G; GX;
    glbuf_t *b = bound_buf(x, target_);
    *v = 0; if (!b) return;
    if (p == 0x8764) *v = b->size; else if (p == 0x8765) *v = b->usage ? b->usage : 0x88E4; else if (p == 0x88BC) *v = b->mapped; else if (p == 0x88BB) *v = 0x88BA;
}
static void WINAPI glGetBufferPointerv(GLenum target_, GLenum p, void **v) {
    G; GX; (void)p;
    glbuf_t *b = bound_buf(x, target_);
    *v = b && b->mapped ? b->data : NULL;
}

/* ---------------- vertex array objects ---------------- */
static void WINAPI glGenVertexArrays(GLsizei n, GLuint *out) {
    G; GX;
    for (int i = 0; i < n; i++) {
        out[i] = 0;
        for (int v = 1; v < NVAO; v++) if (!x->vao[v].used) { memset(&x->vao[v], 0, sizeof x->vao[v]); x->vao[v].used = true; out[i] = v; break; }
        if (!out[i]) err(g, 0x0505);
    }
}
static void WINAPI glDeleteVertexArrays(GLsizei n, const GLuint *ids) {
    G; GX;
    for (int i = 0; i < n; i++) if (ids[i] && ids[i] < NVAO) { x->vao[ids[i]].used = false; if (x->vao_bound == ids[i]) x->vao_bound = 0; }
}
static void WINAPI glBindVertexArray(GLuint id) {
    G; GX;
    if (id >= NVAO) { err(g, 0x0502); return; }
    if (id && !x->vao[id].used) { memset(&x->vao[id], 0, sizeof x->vao[id]); x->vao[id].used = true; }
    x->vao_bound = id;
}
static GLboolean WINAPI glIsVertexArray(GLuint id) { GV(0); GXV(0); return id && id < NVAO && x->vao[id].used; }

/* ---------------- generic vertex attributes ---------------- */
static void WINAPI glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    G; GX;
    if (i >= NATTR) { err(g, 0x0501); return; }
    vattr_t *a = &cur_vao(x)->a[i];
    a->size = size == 0x80E1 ? 4 : size; a->type = type; a->norm = norm; a->stride = stride; a->ptr = (uintptr_t)p; a->buf = x->abo; a->integer = false;
}
static void WINAPI glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const void *p) {
    glVertexAttribPointer(i, size, type, 0, stride, p);
    G; GX; if (i < NATTR) cur_vao(x)->a[i].integer = true;
}
static void WINAPI glEnableVertexAttribArray(GLuint i) { G; GX; if (i < NATTR) cur_vao(x)->a[i].on = true; }
static void WINAPI glDisableVertexAttribArray(GLuint i) { G; GX; if (i < NATTR) cur_vao(x)->a[i].on = false; }
static void WINAPI glVertexAttribDivisor(GLuint i, GLuint d) { G; GX; if (i < NATTR) cur_vao(x)->a[i].divisor = d; }
static void set_attr(GLuint i, float a, float b, float c, float d) { G; GX; if (i < NATTR) { x->cur[i][0] = a; x->cur[i][1] = b; x->cur[i][2] = c; x->cur[i][3] = d; } }
static void WINAPI glVertexAttrib1f(GLuint i, GLfloat a) { set_attr(i, a, 0, 0, 1); }
static void WINAPI glVertexAttrib2f(GLuint i, GLfloat a, GLfloat b) { set_attr(i, a, b, 0, 1); }
static void WINAPI glVertexAttrib3f(GLuint i, GLfloat a, GLfloat b, GLfloat c) { set_attr(i, a, b, c, 1); }
static void WINAPI glVertexAttrib4f(GLuint i, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { set_attr(i, a, b, c, d); }
static void WINAPI glVertexAttrib1fv(GLuint i, const GLfloat *v) { set_attr(i, v[0], 0, 0, 1); }
static void WINAPI glVertexAttrib2fv(GLuint i, const GLfloat *v) { set_attr(i, v[0], v[1], 0, 1); }
static void WINAPI glVertexAttrib3fv(GLuint i, const GLfloat *v) { set_attr(i, v[0], v[1], v[2], 1); }
static void WINAPI glVertexAttrib4fv(GLuint i, const GLfloat *v) { set_attr(i, v[0], v[1], v[2], v[3]); }
static void WINAPI glVertexAttrib4ubv(GLuint i, const GLubyte *v) { set_attr(i, v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, v[3] / 255.f); }
static void WINAPI glVertexAttrib4Nubv(GLuint i, const GLubyte *v) { glVertexAttrib4ubv(i, v); }
static void WINAPI glGetVertexAttribiv(GLuint i, GLenum p, GLint *v) {
    G; GX; if (i >= NATTR) { err(g, 0x0501); return; }
    vattr_t *a = &cur_vao(x)->a[i];
    switch (p) {
    case 0x8622: *v = a->on; break; case 0x8623: *v = a->size ? a->size : 4; break; case 0x8624: *v = a->stride; break;
    case 0x8625: *v = a->type ? a->type : 0x1406; break; case 0x886A: *v = a->norm; break; case 0x889F: *v = a->buf; break;
    case 0x8626: for (int k = 0; k < 4; k++) v[k] = (GLint)x->cur[i][k]; break;
    default: *v = 0;
    }
}
static void WINAPI glGetVertexAttribfv(GLuint i, GLenum p, GLfloat *v) {
    G; GX;
    if (p == 0x8626 && i < NATTR) { memcpy(v, x->cur[i], 16); return; }
    GLint t; glGetVertexAttribiv(i, p, &t); *v = (float)t;
}
static void WINAPI glGetVertexAttribPointerv(GLuint i, GLenum p, void **v) { G; GX; (void)p; *v = i < NATTR ? (void *)cur_vao(x)->a[i].ptr : NULL; }
/* one component of an array element, converted as glVertexAttribPointer says */
static float attr_fetch(const uint8_t *base, const vattr_t *a, uint32_t i, int c) {
    int ts = type_size(a->type), stride = a->stride ? a->stride : ts * a->size;
    const uint8_t *p = base + (size_t)i * stride + c * ts;
    switch (a->type) {
    case 0x1406: return *(const float *)p;
    case 0x140A: return (float)*(const double *)p;
    case 0x1401: return a->norm ? *p / 255.0f : *p;
    case 0x1400: return a->norm ? MAX(-1.0f, *(const int8_t *)p / 127.0f) : *(const int8_t *)p;
    case 0x1402: return a->norm ? MAX(-1.0f, *(const int16_t *)p / 32767.0f) : *(const int16_t *)p;
    case 0x1403: return a->norm ? *(const uint16_t *)p / 65535.0f : *(const uint16_t *)p;
    case 0x1404: return a->norm ? MAX(-1.0f, *(const int32_t *)p / 2147483647.0f) : (float)*(const int32_t *)p;
    case 0x140B: { uint16_t h = *(const uint16_t *)p; uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 1023, u;
                   if (!e) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } u = s | (e << 23) | ((m & 1023) << 13); } }
                   else u = s | (e == 31 ? 0x7F800000u | (m << 13) : ((e + 112) << 23) | (m << 13));
                   float f; memcpy(&f, &u, 4); return f; }
    default: return a->norm ? *(const uint32_t *)p / 4294967295.0f : (float)*(const uint32_t *)p;
    }
}

/* ---------------- texture units, blending, depth, colour mask ---------------- */
static void WINAPI glBlendColor(GLclampf r, GLclampf gg, GLclampf b, GLclampf a) { G; GX; x->blend_color[0] = r; x->blend_color[1] = gg; x->blend_color[2] = b; x->blend_color[3] = a; }
static void WINAPI glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { G; GX; x->bsrc = a; x->bdst = b; x->bsrc_a = c; x->bdst_a = d; }
static void WINAPI glBlendFunc(GLenum a, GLenum b) { glBlendFuncSeparate(a, b, a, b); }
static void WINAPI glBlendEquationSeparate(GLenum a, GLenum b) { G; GX; x->beq = a; x->beq_a = b; }
static void WINAPI glBlendEquation(GLenum m) { glBlendEquationSeparate(m, m); }
static void WINAPI glDepthFunc(GLenum f) { G; GX; x->depth_func = f; }
static void WINAPI glColorMask(GLboolean r, GLboolean gg, GLboolean b, GLboolean a) { G; GX; x->cmask[0] = r; x->cmask[1] = gg; x->cmask[2] = b; x->cmask[3] = a; }
static void sl_free(glctx_t *g);   /* glsl.inc */
static void gl2_free(glctx_t *g) {
    struct gl2 *x = g->x; if (!x) return;
    sl_free(g);
    for (int i = 0; i < x->nbuf; i++) kfree(x->buf[i].data);
    kfree(x->buf); kfree(x); g->x = NULL;
}
