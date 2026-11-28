/* the twice-compiled pixel code itself (see glsl_px.inc): texture sampling in part 1,
 * the pixel tests, blending and the four-pixel rasteriser in part 2 */
#if SL_PART == 1
/* bilinear (or nearest) sampling in fixed point: 8-bit weights, whole texels read once */
static inline int PX(fl_int)(float f) { int i = (int)f; return i - ((float)i > f); }
static inline int PX(wrapi)(int i, int n, GLenum mode) {
    if ((unsigned)i < (unsigned)n) return i;
    return wrap_coord(i, n, mode);
}
SL_ENTRY static void PX(sl_sample)(void *ctx, int ti, int si, const float *uv, float lod, float *out) {
    (void)si; (void)lod;
    gltex_t *t; canvas_t *c = unit_texture(ctx, ti, &t);
    if (!c) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
    float fx = uv[0] * c->w - 0.5f, fy = uv[1] * c->h - 0.5f;
    if (!(fx > -1e6f && fx < 1e6f)) fx = 0;
    if (!(fy > -1e6f && fy < 1e6f)) fy = 0;
    GLenum ws = t->wrap_s ? t->wrap_s : 0x2901, wt = t->wrap_t ? t->wrap_t : 0x2901;
    int w = c->w, h = c->h;
    bool p2 = ws == 0x2901 && wt == 0x2901 && !(w & (w - 1)) && !(h & (h - 1));
    const uint32_t *px = c->px; int stride = c->stride;
    if (!t->linear) {
        int x = PX(fl_int)(fx + 0.5f), y = PX(fl_int)(fy + 0.5f);
        if (p2) { x &= w - 1; y &= h - 1; } else { x = PX(wrapi)(x, w, ws); y = PX(wrapi)(y, h, wt); }
        uint32_t v = px[(size_t)y * stride + x];
        const float k = 1.0f / 255;
        out[0] = ((v >> 16) & 255) * k; out[1] = ((v >> 8) & 255) * k; out[2] = (v & 255) * k; out[3] = (v >> 24) * k;
        return;
    }
    int x0 = PX(fl_int)(fx), y0 = PX(fl_int)(fy);
    int ax = (int)((fx - x0) * 256), ay = (int)((fy - y0) * 256);
    int xa, xb, ya, yb;
    if (p2) { xa = x0 & (w - 1); xb = (x0 + 1) & (w - 1); ya = y0 & (h - 1); yb = (y0 + 1) & (h - 1); }
    else { xa = PX(wrapi)(x0, w, ws); xb = PX(wrapi)(x0 + 1, w, ws); ya = PX(wrapi)(y0, h, wt); yb = PX(wrapi)(y0 + 1, h, wt); }
    uint32_t p00 = px[(size_t)ya * stride + xa], p10 = px[(size_t)ya * stride + xb], p01 = px[(size_t)yb * stride + xa], p11 = px[(size_t)yb * stride + xb];
    const float k = 1.0f / (255.0f * 65536.0f);
    for (int ch = 0; ch < 4; ch++) {
        int sh = ch == 3 ? 24 : 16 - 8 * ch;
        uint32_t a = (p00 >> sh) & 255, b = (p10 >> sh) & 255, cc = (p01 >> sh) & 255, d = (p11 >> sh) & 255;
        uint32_t top = a * (256 - ax) + b * ax, bot = cc * (256 - ax) + d * ax;
        out[ch] = (top * (256 - ay) + bot * ay) * k;
    }
}
#elif SL_PART == 2
static inline float PX(blend_f)(GLenum f, int ch, const float *s, const float *d, const float *bc) {
    switch (f) {
    case 0: return 0; case 1: return 1;
    case 0x0300: return s[ch]; case 0x0301: return 1 - s[ch];
    case 0x0302: return s[3]; case 0x0303: return 1 - s[3];
    case 0x0304: return d[3]; case 0x0305: return 1 - d[3];
    case 0x0306: return d[ch]; case 0x0307: return 1 - d[ch];
    case 0x0308: return ch == 3 ? 1 : MIN(s[3], 1 - d[3]);
    case 0x8001: return bc[ch]; case 0x8002: return 1 - bc[ch]; case 0x8003: return bc[3]; case 0x8004: return 1 - bc[3];
    }
    return 1;
}
static inline float PX(blend_eq)(GLenum e, float a, float b) {
    switch (e) { case 0x800A: return a - b; case 0x800B: return b - a; case 0x8007: return MIN(a, b); case 0x8008: return MAX(a, b); }
    return a + b;
}
static inline bool PX(test_depth)(GLenum f, float z, float cur) {
    switch (f) {
    case 0x0200: return false; case 0x0201: return z < cur; case 0x0202: return z == cur; case 0x0203: return z <= cur;
    case 0x0204: return z > cur; case 0x0205: return z != cur; case 0x0206: return z >= cur;
    }
    return true;
}
static inline bool PX(test_alpha)(GLenum f, float a, float ref) { return PX(test_depth)(f, a, ref); }
static inline void PX(frag_out)(sl_wk_t *W, int x, int y, float z, const float *c) {
    pipe_t *P = W->P;
    float *dz = P->t.depth + (size_t)y * P->t.w + x;
    float src[4]; for (int i = 0; i < 4; i++) src[i] = c[i] != c[i] ? 0 : CLAMP(c[i], 0, 1);
    if (P->alpha_test && !PX(test_alpha)(P->s->alpha_func, src[3], P->s->alpha_ref)) return;
    if (P->depth_write) *dz = 1 - z;
    W->pixels++;
    if (!P->any_mask) return;
    uint32_t *px = P->t.c->px + (size_t)y * P->t.c->stride + x;
    float out[4];
    struct gl2 *gx = P->g->x;
    if (P->blend) {
        float d[4]; uint32_t v = *px;
        d[0] = ((v >> 16) & 255) / 255.0f; d[1] = ((v >> 8) & 255) / 255.0f; d[2] = (v & 255) / 255.0f; d[3] = (v >> 24) / 255.0f;
        for (int i = 0; i < 3; i++) out[i] = PX(blend_eq)(gx->beq, src[i] * PX(blend_f)(gx->bsrc, i, src, d, gx->blend_color), d[i] * PX(blend_f)(gx->bdst, i, src, d, gx->blend_color));
        out[3] = PX(blend_eq)(gx->beq_a, src[3] * PX(blend_f)(gx->bsrc_a, 3, src, d, gx->blend_color), d[3] * PX(blend_f)(gx->bdst_a, 3, src, d, gx->blend_color));
    } else memcpy(out, src, 16);
    uint32_t v = *px, nv = 0;
    for (int i = 0; i < 4; i++) {
        int sh = i == 3 ? 24 : 16 - 8 * i;
        uint32_t ch = gx->cmask[i] ? (uint32_t)(CLAMP(out[i], 0, 1) * 255 + 0.5f) : (v >> sh) & 255;
        nv |= ch << sh;
    }
    if (!P->t.flip) nv |= 0xFF000000u;                  /* the window has no alpha */
    *px = nv;
}
/* a row's pixels xs..xe, four at a time: those inside the depth range that pass the
 * depth test are interpolated into the component vectors and shaded together */
/* not static: its float arguments then pass the standard way into the SSE copy */
SL_ENTRY void PX(raster_quads)(sl_wk_t *W, const sl_tri_t *T, const float *pl, int xs, int xe, int y, float py, float z, float iw, float *dz) {
    pipe_t *P = W->P;
    float *qi = W->qin;
    const float *qo = W->qout + 16 * P->o->target_reg;
    float fy = (P->t.flip ? y : P->t.h - 1 - y) + .5f;
    for (int x = xs; x <= xe; x += 4) {
        uint32_t live = 0;
        float zq[4], wq[4], wc[4];
        for (int l = 0; l < 4; l++, z += T->zdx, iw += T->idx) {
            zq[l] = z; wq[l] = iw; wc[l] = iw != 0 ? 1.0f / iw : 0;
            int xx = x + l;
            if (xx > xe || z < 0 || z > 1) continue;
            if (P->depth_test && !PX(test_depth)(P->dfunc, z, 1 - dz[xx])) continue;
            live |= 1u << l;
        }
        if (!live) continue;
        float px0 = x + .5f;
        for (int i = 0; i < P->np; i++) {
            float d = pl[3 * i + 1], v = pl[3 * i] + d * px0 + pl[3 * i + 2] * py;
            float *o = qi + 16 * P->lreg[i] + 4 * (P->lcomp[i] & 3);
            if (P->lpers[i]) { o[0] = v * wc[0]; o[1] = (v + d) * wc[1]; o[2] = (v + 2 * d) * wc[2]; o[3] = (v + 3 * d) * wc[3]; }
            else { o[0] = v; o[1] = v + d; o[2] = v + 2 * d; o[3] = v + 3 * d; }
        }
        if (P->fpos >= 0) {
            float *o = qi + 16 * P->fpos;
            for (int l = 0; l < 4; l++) { o[l] = px0 + l; o[4 + l] = fy; o[8 + l] = zq[l]; o[12 + l] = wq[l]; }
        }
        uint32_t keep = dxbc_quad_run(W->q, live);
        for (int l = 0; l < 4; l++) {
            if (!(keep & (1u << l))) continue;
            float c[4] = { qo[l], qo[4 + l], qo[8 + l], qo[12 + l] };
            PX(frag_out)(W, x + l, y, zq[l], c);
        }
    }
}
#else
/* part 3: the vertex shader stage and triangle setup (clipping, projection, plane equations) */
static void PX(draw_tri)(pipe_t *P, sl_bin_t *B, const pv_t *a, const pv_t *b, const pv_t *c);
/* plane equations of z, 1/w and every varying lane the fragment shader reads, set up once
 * per triangle (here, on the boot processor); the workers evaluate them at each pixel */
static void PX(shade_triangle)(pipe_t *P, sl_bin_t *B, const sv_t *a, const sv_t *b, const sv_t *c) {
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0 || area != area) return;
    bool cw = area > 0;
    bool front = P->front_ccw_screen ? !cw : cw;
    if ((front && P->cull_front) || (!front && P->cull_back)) return;
    if (!cw) { const sv_t *t = b; b = c; c = t; area = -area; }
    int x0 = (int)MAX((float)P->clip.x, MIN(a->x, MIN(b->x, c->x)));
    int x1 = (int)MIN((float)(P->clip.x + P->clip.w), MAX(a->x, MAX(b->x, c->x)) + 1);
    int y0 = (int)MAX((float)P->clip.y, MIN(a->y, MIN(b->y, c->y)));
    int y1 = (int)MIN((float)(P->clip.y + P->clip.h), MAX(a->y, MAX(b->y, c->y)) + 1);
    if (x0 >= x1 || y0 >= y1) return;
    size_t need = (size_t)P->np * 3 + (size_t)P->nflat * 4;
    /* the other processors cannot allocate: a full bin is redone on this one */
    if (B->n == B->cap || B->npool + need > B->cappool) {
        if (!B->grow) { B->overflow = true; return; }
        if (B->n == B->cap) { int nc = B->cap ? B->cap * 2 : 256; sl_tri_t *n = krealloc(B->tris, sizeof(sl_tri_t) * nc); if (!n) return; B->tris = n; B->cap = nc; }
        if (B->npool + need > B->cappool) { size_t nc = MAX(B->cappool * 2, B->npool + need + 4096); float *n = krealloc(B->pool, sizeof(float) * nc); if (!n) return; B->pool = n; B->cappool = nc; }
    }
    sl_tri_t *T = &B->tris[B->n++];
    T->x0 = x0; T->x1 = x1; T->y0 = y0; T->y1 = y1; T->front = front ? ~0u : 0; T->off = B->npool;
    T->vx[0] = a->x; T->vx[1] = b->x; T->vx[2] = c->x; T->vy[0] = a->y; T->vy[1] = b->y; T->vy[2] = c->y;
    float inv = 1.0f / area;
    /* barycentric weights of a and b as planes over the screen: w = o + dx * x + dy * y */
    float w0dx = (b->y - c->y) * inv, w0dy = (c->x - b->x) * inv, w0o = (b->x * c->y - c->x * b->y) * inv;
    float w1dx = (c->y - a->y) * inv, w1dy = (a->x - c->x) * inv, w1o = (c->x * a->y - a->x * c->y) * inv;
    #define PLANE(va, vb, vc, o_, dx_, dy_) do { float da_ = (va) - (vc), db_ = (vb) - (vc); \
        dx_ = w0dx * da_ + w1dx * db_; dy_ = w0dy * da_ + w1dy * db_; o_ = (vc) + w0o * da_ + w1o * db_; } while (0)
    PLANE(a->z, b->z, c->z, T->zo, T->zdx, T->zdy);
    PLANE(a->iw, b->iw, c->iw, T->io, T->idx, T->idy);
    float *pl = B->pool + B->npool;
    for (int i = 0; i < P->np; i++) {
        int k = P->lcomp[i] >> 2, cc = P->lcomp[i] & 3;
        PLANE(a->v[k].f[cc], b->v[k].f[cc], c->v[k].f[cc], pl[3 * i], pl[3 * i + 1], pl[3 * i + 2]);
    }
    #undef PLANE
    float *fl = pl + 3 * P->np;
    for (int i = 0; i < P->nflat; i++) memcpy(fl + 4 * i, B->flatv[P->kflat[i]].f, 16);
    B->npool += need;
}
static bool PX(run_vertex)(pipe_t *P, struct sl_wk *W, uint32_t index, const fatvert_t *imm, pv_t *o) {
    sl_obj_t *ob = P->o;
    for (int r = 0; r < DX_MAX_REG; r++) {
        int sl = ob->in_slot[r];
        if (sl < 0) continue;
        if (sl == SL_VERTEXID) { W->vin[r].u[0] = index; continue; }
        if (sl == SL_INSTANCEID) { W->vin[r].u[0] = 0; continue; }
        if (!vertex_slot(P, sl, index, imm, W->vin[r].f)) return false;
    }
    if (!dxbc_exec_run(&W->vx)) return false;
    memcpy(o->pos, W->vout[ob->pos_reg].f, 16);
    for (int k = 0; k < P->nvar; k++) o->v[k] = W->vout[P->vreg[k]];
    return true;
}
SL_ENTRY void PX(vertex_job)(void *arg, int w, int n) {
    vjob_t *J = arg; pipe_t *P = J->P;
    if (w >= P->nwk) return;
    sl_wk_t *W = &P->wk[w];
    uint32_t a = (uint32_t)((uint64_t)J->span * w / n), b = (uint32_t)((uint64_t)J->span * (w + 1) / n);
    for (uint32_t s = a; s < b; s++) {
        if (!J->done[s]) continue;                       /* 1: used by the draw */
        if (w == 0 && !(s & 63)) sl_yield();
        uint32_t k = J->lo + s;
        J->done[s] = PX(run_vertex)(P, W, k, J->imm ? &J->imm[k] : NULL, &J->cache[s]) ? 2 : 3;
    }
}
SL_ENTRY void PX(setup_job)(void *arg, int w, int n) {
    sjob_t *J = arg; pipe_t *P = J->P;
    if (w >= P->nwk) return;
    sl_bin_t *B = &P->bins[w];
    int a = (int)((int64_t)J->ntri * w / n), b = (int)((int64_t)J->ntri * (w + 1) / n);
    for (int i = a; i < b && !B->overflow; i++) {
        if (w == 0 && !(i & 255)) sl_yield();
        for (int k = 0; k < P->nvar; k++) if (P->flat[k]) B->flatv[k] = J->tri[i].c->v[k];
        PX(draw_tri)(P, B, J->tri[i].a, J->tri[i].b, J->tri[i].c);
    }
}
static void PX(to_screen)(pipe_t *P, const pv_t *v, sv_t *s) {
    float iw = 1.0f / v->pos[3];
    s->x = P->vx0 + (v->pos[0] * iw + 1) * 0.5f * P->vw;
    float gy = P->vy0 + (v->pos[1] * iw + 1) * 0.5f * P->vh;          /* GL window y, from the bottom */
    s->y = P->t.flip ? gy : P->t.h - gy;
    s->z = P->zn + (v->pos[2] * iw + 1) * 0.5f * (P->zf - P->zn);
    s->iw = iw;
    for (int k = 0; k < P->nvar; k++) {
        if (P->flat[k]) { s->v[k] = v->v[k]; continue; }
        float m = P->noper[k] ? 1 : iw;
        for (int i = 0; i < 4; i++) if (P->lanes[k] & (1u << i)) s->v[k].f[i] = v->v[k].f[i] * m;
    }
}
static void PX(lerp_pv)(pipe_t *P, const pv_t *a, const pv_t *b, float t, pv_t *o) {
    for (int i = 0; i < 4; i++) o->pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * t;
    for (int k = 0; k < P->nvar; k++) for (int i = 0; i < 4; i++) if (P->lanes[k] & (1u << i)) o->v[k].f[i] = a->v[k].f[i] + (b->v[k].f[i] - a->v[k].f[i]) * t;
}
static float PX(clip_dist)(const pv_t *v, int plane) {
    switch (plane) {
    case 0: return v->pos[3] + v->pos[2]; case 1: return v->pos[3] - v->pos[2];
    case 2: return v->pos[3] + v->pos[0]; case 3: return v->pos[3] - v->pos[0];
    case 4: return v->pos[3] + v->pos[1]; case 5: return v->pos[3] - v->pos[1];
    default: return v->pos[3] - 1e-6f;
    }
}
static void PX(draw_tri)(pipe_t *P, sl_bin_t *B, const pv_t *a, const pv_t *b, const pv_t *c) {
    const pv_t *in[3] = { a, b, c };
    bool all_in = true;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 4; k++) if (!(in[i]->pos[k] > -1e20f && in[i]->pos[k] < 1e20f)) return;
        for (int p = 0; p < 7; p++) if (!(PX(clip_dist)(in[i], p) >= 0)) all_in = false;
    }
    sv_t s[12];
    if (all_in) { for (int i = 0; i < 3; i++) PX(to_screen)(P, in[i], &s[i]); PX(shade_triangle)(P, B, &s[0], &s[1], &s[2]); return; }
    pv_t *src = B->poly[0], *dst = B->poly[1];
    for (int i = 0; i < 3; i++) src[i] = *in[i];
    int n = 3;
    for (int p = 0; p < 7; p++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            pv_t *q0 = &src[i], *q1 = &src[(i + 1) % n];
            float d0 = PX(clip_dist)(q0, p), d1 = PX(clip_dist)(q1, p);
            if (d0 >= 0) { if (m == 12) return; dst[m++] = *q0; }
            if ((d0 >= 0) != (d1 >= 0)) { if (m == 12) return; PX(lerp_pv)(P, q0, q1, d0 / (d0 - d1), &dst[m++]); }
        }
        n = m; pv_t *t = src; src = dst; dst = t;
        if (n < 3) return;
    }
    for (int i = 0; i < n; i++) PX(to_screen)(P, &src[i], &s[i]);
    for (int i = 1; i + 1 < n; i++) PX(shade_triangle)(P, B, &s[0], &s[i], &s[i + 1]);
}
#endif
