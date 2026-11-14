/* Framebuffer checks against the actual D3D10 draw path. No display/GPU/Windows
 * SDK is needed: only window presentation and process allocation are stubbed. */
#define main shader_core_main
#include "shadertest.c"
#undef main
#include "../src/win32/d3d10.c"

bool accel3d = true, wm_need_redraw;
static int diagnostics;
void klog(const char *fmt, ...) { (void)fmt; diagnostics++; }
w32_proc_t *w32_cur(void) { return NULL; }
void *w32_alloc(w32_proc_t *p, uint32_t n) { (void)p; return kzalloc(n); }
void w32_free(w32_proc_t *p, void *v) { (void)p; kfree(v); }
rect_t rect_intersect(rect_t a, rect_t b) {
    int x = MAX(a.x, b.x), y = MAX(a.y, b.y);
    return R(x, y, MAX(0, MIN(a.x + a.w, b.x + b.w) - x), MAX(0, MIN(a.y + a.h, b.y + b.h) - y));
}
canvas_t *canvas_new(int w, int h) { (void)w; (void)h; return NULL; }
void canvas_free(canvas_t *c) { (void)c; }
bool w32_hwnd_client(uintptr_t h, canvas_t **s, rect_t *r) { (void)h; (void)s; (void)r; return false; }
void blit(canvas_t *d, int x, int y, canvas_t *s, int sx, int sy, int w, int h) { (void)d; (void)x; (void)y; (void)s; (void)sx; (void)sy; (void)w; (void)h; }
void blit_scaled_alpha(canvas_t *d, rect_t r, canvas_t *s, int a) { (void)d; (void)r; (void)s; (void)a; }
void task_yield(void) { }

typedef struct { float p[4], c[4], uv[2]; } test_vertex_t;
enum { TEST_W = 64, TEST_H = 48 };
typedef struct { device_t *d; tex_t *color, *depth; view_t *rt, *ds; } test_device_t;
static test_device_t test_device(uint32_t format) {
    test_device_t t = { 0 };
    check("create test device", D3D10CreateDevice(NULL, 0, 0, 0, 29, &t.d) == S_OK);
    tex2d_desc_t td = { TEST_W, TEST_H, 1, 1, format, 1, 0, 0, 0x20, 0, 0 };
    check("create test target", CreateTexture2D(t.d, &td, NULL, &t.color) == S_OK);
    check("create RTV", CreateRenderTargetView(t.d, t.color, NULL, &t.rt) == S_OK);
    td.format = F_D32F; td.bind = 0x40;
    check("create depth target", CreateTexture2D(t.d, &td, NULL, &t.depth) == S_OK);
    check("create DSV", CreateDepthStencilView(t.d, t.depth, NULL, &t.ds) == S_OK);
    OMSetRenderTargets(t.d, 1, &t.rt, t.ds);
    IASetPrimitiveTopology(t.d, 4);
    rast_desc_t rd = { 3, 1, 0, 0, 0, 0, 1 };
    rast_t *rs = NULL; check("create rasterizer", CreateRasterizerState(t.d, &rd, &rs) == S_OK);
    RSSetState(t.d, rs); Release(&rs->c);
    return t;
}
static void destroy_device(test_device_t *t) {
    ClearState(t->d); Release(&t->rt->c); Release(&t->ds->c);
    Release(&t->color->c); Release(&t->depth->c); Release(&t->d->c);
}
static unsigned char *fixture_data(const char *name, const char *stage, int model, size_t *size) {
    char path[160]; snprintf(path, sizeof path, "tools/shaders/%s-%s_%d_0.%s", name, stage, model, model == 6 ? "dxil" : "dxbc");
    unsigned char *p = read_file(path, size); check("render fixture exists", p != NULL); return p;
}
static void bind_program(test_device_t *t, const char *vertex, int vm, const char *pixel, int pm) {
    size_t size; unsigned char *p = fixture_data(vertex, "vs", vm, &size);
    shader_t *vs = NULL, *ps = NULL;
    check("CreateVertexShader real fixture", CreateVertexShader(t->d, p, size, &vs) == S_OK);
    elem_desc_t e[] = { { "POSITION", 0, F_RGBA32F, 0, 0, 0, 0 }, { "COLOR", 0, F_RGBA32F, 0, 16, 0, 0 }, { "TEXCOORD", 0, F_RG32F, 0, 32, 0, 0 } };
    if (!strcmp(vertex, "uint_vertex")) e[1].format = F_RGBA8_UINT;
    layout_t *layout = NULL;
    check("CreateInputLayout real fixture", CreateInputLayout(t->d, e, 3, p, size, &layout) == S_OK);
    free(p); p = fixture_data(pixel, "ps", pm, &size);
    check("CreatePixelShader real fixture", CreatePixelShader(t->d, p, size, &ps) == S_OK); free(p);
    if (vs && ps && layout) { VSSetShader(t->d, vs); PSSetShader(t->d, ps); IASetInputLayout(t->d, layout); }
    if (vs) Release(&vs->c); if (ps) Release(&ps->c); if (layout) Release(&layout->c);
    vec4_t cb[5] = { 0 }; for (int i = 0; i < 4; i++) { cb[0].f[i] = 1; cb[i + 1].f[i] = 1; }
    buffer_desc_t bd = { sizeof cb, 0, 4, 0, 0 }; subres_t init = { cb, 0, 0 }; buffer_t *b;
    check("create scene constants", CreateBuffer(t->d, &bd, &init, &b) == S_OK);
    VSSetConstantBuffers(t->d, 0, 1, &b); PSSetConstantBuffers(t->d, 0, 1, &b); Release(&b->c);
}
static void bind_vertices(test_device_t *t, const test_vertex_t *v, int count) {
    buffer_desc_t bd = { count * sizeof *v, 0, 1, 0, 0 }; subres_t init = { v, 0, 0 }; buffer_t *b;
    check("create vertex buffer", CreateBuffer(t->d, &bd, &init, &b) == S_OK);
    uint32_t stride = sizeof *v, offset = 0; IASetVertexBuffers(t->d, 0, 1, &b, &stride, &offset); Release(&b->c);
}
static void clear_test(test_device_t *t, float r, float g, float b, float a, float depth) {
    float c[] = { r, g, b, a }; ClearRenderTargetView(t->d, t->rt, c); ClearDepthStencilView(t->d, t->ds, 1, depth, 0);
}
static void rectangle(test_device_t *t, const float *color) {
    test_vertex_t v[6] = {
        { { -1, -1, .5f, 1 } }, { { -1, 1, .5f, 1 } }, { { 1, 1, .5f, 1 } },
        { { -1, -1, .5f, 1 } }, { { 1, 1, .5f, 1 } }, { { 1, -1, .5f, 1 } }
    };
    for (int i = 0; i < 6; i++) { memcpy(v[i].c, color, 16); v[i].uv[0] = v[i].uv[1] = .25f; }
    bind_vertices(t, v, 6); Draw(t->d, 6, 0);
}
static void coverage_cases(int model) {
    test_device_t t = test_device(F_RGBA32F);
    bind_program(&t, "vertex", model, "flat_color", model);
    float c[] = { .25f, .5f, .75f, 1 };
    clear_test(&t, 0, 0, 0, 0, 1);
    blend_desc_t bd = { 0 }; bd.enable[0] = 1; bd.src = bd.dst = bd.src_a = bd.dst_a = 2; bd.op = bd.op_a = 1; bd.mask[0] = 15;
    blendst_t *blend = NULL; check("create additive blend", CreateBlendState(t.d, &bd, &blend) == S_OK); OMSetBlendState(t.d, blend, NULL, ~0u);
    depth_desc_t dd = { 0 }; depthst_t *disabled = NULL; check("disable depth for shared-edge test", CreateDepthStencilState(t.d, &dd, &disabled) == S_OK); OMSetDepthStencilState(t.d, disabled, 0);
    rectangle(&t, c);
    for (int y = 0; y < TEST_H; y++) for (int x = 0; x < TEST_W; x++) {
        float *p = (float *)texel(t.color, x, y);
        for (int k = 0; k < 4; k++) check("span coverage/shared edge/blending", close_float(p[k], c[k]));
    }
    OMSetBlendState(t.d, NULL, NULL, ~0u); Release(&blend->c);
    OMSetDepthStencilState(t.d, NULL, 0); Release(&disabled->c);
    bind_program(&t, "vertex", model, "depth", model);
    clear_test(&t, 0, 0, 0, 0, .4f); rectangle(&t, c);
    check("late shader depth writes color", close_float(((float *)texel(t.color, 20, 20))[0], .25f));
    check("late shader depth writes depth", *(float *)texel(t.depth, 20, 20) == .25f);
    clear_test(&t, 0, 0, 0, 0, .2f); rectangle(&t, c);
    check("late shader depth rejection", *(float *)texel(t.color, 20, 20) == 0 && *(float *)texel(t.depth, 20, 20) == .2f);
    bind_program(&t, "vertex", model, "clip_color", model);
    clear_test(&t, 0, 0, 0, 0, 1); rectangle(&t, c);
    check("discard leaves color/depth untouched", *(float *)texel(t.color, 20, 20) == 0 && *(float *)texel(t.depth, 20, 20) == 1);
    bind_program(&t, "vertex", model, "flat_color", model);
    bd.enable[0] = 0; bd.mask[0] = 1;
    check("create color write mask", CreateBlendState(t.d, &bd, &blend) == S_OK); OMSetBlendState(t.d, blend, NULL, ~0u);
    clear_test(&t, 0, .125f, .25f, .5f, 1); rectangle(&t, c);
    float *masked = (float *)texel(t.color, 20, 20);
    check("color write mask preserves components", masked[0] == .25f && masked[1] == .125f && masked[2] == .25f && masked[3] == .5f);
    Release(&blend->c);
    destroy_device(&t);
}
static void integer_target_cases(int model, uint32_t format) {
    test_device_t t = test_device(format);
    bind_program(&t, "uint_vertex", model, "bits", model);
    test_vertex_t v[] = { { { -1, -1, .5f, 1 } }, { { -1, 1, .5f, 1 } }, { { 1, 1, .5f, 1 } } };
    for (int i = 0; i < 3; i++) { uint8_t color[] = { 0xF0, 1, 8, 16 }; memcpy(v[i].c, color, 4); }
    clear_test(&t, 0, 0, 0, 0, 1); bind_vertices(&t, v, 3); Draw(t.d, 3, 0);
    uint8_t *p = texel(t.color, 10, 10);
    if (format == F_RGBA8_UINT) check("uint target raw writes/clamping", p[0] == 4 && p[1] == 255 && p[2] == 3 && p[3] == 4);
    else check("R32_UINT raw write", *(uint32_t *)p == 4);
    bind_program(&t, "vertex", 4, "flat_color", 4);
    clear_test(&t, 0, 0, 0, 0, 1);
    int before = diagnostics; float color[] = { 1, 1, 1, 1 }; rectangle(&t, color);
    check("integer/float binding mismatch diagnosed", diagnostics == before + 1 && *(uint32_t *)p == 0);
    destroy_device(&t);
}
static void packed_link_cases(int vm, int pm) {
    test_device_t t = test_device(F_RGBA32F);
    bind_program(&t, "packed_vertex", vm, "packed_pixel", pm);
    clear_test(&t, 0, 0, 0, 0, 1);
    float color[] = { .25f, .5f, .75f, 1 }; rectangle(&t, color);
    float *p = (float *)texel(t.color, 20, 20);
    check("cross-model semantic component remap", close_float(p[0], .5f) && close_float(p[1], .75f) && close_float(p[2], .75f) && p[3] == 1);
    destroy_device(&t);
}
static void interpolation_cases(int mode, int clipped) {
    test_device_t t = test_device(F_RGBA32F);
    bind_program(&t, "vertex", 5, mode == 1 ? "flat_color" : mode == 4 ? "linear_color" : "smooth_color", 6);
    test_vertex_t v[] = {
        { { -.9f, -.8f, clipped ? -.25f : .2f, 1 }, { .1f, .2f, .3f, 1 } },
        { { -.2f, .8f, .6f, 1 }, { .8f, .4f, .6f, 1 } },
        { { 1.6f, -.4f, .8f, 2 }, { .3f, .9f, .5f, 1 } }
    };
    clear_test(&t, -1, -1, -1, -1, 1); bind_vertices(&t, v, 3); Draw(t.d, 3, 0);
    float sx[3], sy[3];
    for (int i = 0; i < 3; i++) { sx[i] = (v[i].p[0] / v[i].p[3] + 1) * TEST_W / 2; sy[i] = (1 - v[i].p[1] / v[i].p[3]) * TEST_H / 2; }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    int compared = 0;
    for (int y = 0; y < TEST_H; y++) for (int x = 0; x < TEST_W; x++) {
        float px = x + .5f, py = y + .5f;
        float w[] = { ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area,
                      ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area, 0 };
        w[2] = 1 - w[0] - w[1];
        float z = 0, iw = 0; for (int i = 0; i < 3; i++) { z += w[i] * v[i].p[2] / v[i].p[3]; iw += w[i] / v[i].p[3]; }
        if (w[0] < .02f || w[1] < .02f || w[2] < .02f || z < .02f) continue;
        float *p = (float *)texel(t.color, x, y);
        for (int k = 0; k < 4; k++) {
            float expected = 0;
            for (int i = 0; i < 3; i++) expected += w[i] * v[i].c[k] * (mode == 2 ? 1 / v[i].p[3] : 1);
            if (mode == 2) expected /= iw;
            if (mode == 1) expected = v[0].c[k];
            check("stepped interpolation vs barycentric reference", fabsf(p[k] - expected) < .00005f);
        }
        compared++;
    }
    check("interior pixels compared", compared > 500);
    destroy_device(&t);
}
static void sampling_cases(void) {
    uint8_t pixels[] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255 };
    tex_t tex = { .d = { 2, 2, 1, 1, F_RGBA8, 1 }, .pitch = 8, .bpp = 4, .data = pixels };
    view_t view = { .tex = &tex }; sampler_t smp = { .d = { .au = 1, .av = 1 } };
    device_t d = { 0 }; d.srv[7] = &view; d.smp[5] = &smp; sctx_t ctx = { &d };
    float uv[] = { .25f, .25f }, out[4];
    cb_sample(&ctx, 7, 5, uv, 0, out); check("native point sampler", out[0] == 1 && out[1] == 0 && out[2] == 0);
    uv[0] = -.25f; cb_sample(&ctx, 7, 5, uv, 0, out); check("native point sampler negative wrap", out[0] == 0 && out[1] == 1 && out[2] == 0);
    smp.d.filter = 4; uv[0] = uv[1] = .5f;
    cb_sample(&ctx, 7, 5, uv, 0, out); check("native bilinear sampler", out[0] == .5f && out[1] == .5f && out[2] == .5f && out[3] == 1);
    uv[0] = -50000.375f; uv[1] = .25f;
    cb_sample(&ctx, 7, 5, uv, 0, out); check("native sampler large negative floor", out[0] == .25f && out[1] == .75f && out[2] == 0);
    cb_sample(&ctx, 6, 5, uv, 0, out); check("unbound texture reads zero", out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
}
int main(void) {
    for (int model = 4; model <= 6; model++) coverage_cases(model);
    for (int vm = 4; vm <= 6; vm++) for (int pm = 4; pm <= 6; pm++) packed_link_cases(vm, pm);
    for (int clipped = 0; clipped < 2; clipped++) { interpolation_cases(1, clipped); interpolation_cases(2, clipped); interpolation_cases(4, clipped); }
    for (int model = 5; model <= 6; model++) { integer_target_cases(model, F_RGBA8_UINT); integer_target_cases(model, F_R32_UINT); }
    sampling_cases();
    check("renderer allocations released", live_allocations == 0);
    printf("D3D10 framebuffer tests: %s (%d checks, %d diagnostics)\n", failures ? "FAILED" : "passed", checks, diagnostics);
    return failures ? 1 : 0;
}
