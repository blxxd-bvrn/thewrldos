/* d3d10.dll and dxgi.dll for Win32 programs: Direct3D 10 on the processor.
 *
 * Direct3D 10 has no fixed-function pipeline: every draw runs a vertex shader
 * and a pixel shader given as Shader Model 4 bytecode (compiled HLSL). The
 * bytecode is decoded and interpreted by dxbc.c, once per vertex and once per
 * pixel, and triangles are rasterised here with perspective-correct
 * interpolation of every shader output, a depth buffer, culling and blending.
 *
 * Implemented: D3D10CreateDevice(AndSwapChain), CreateDXGIFactory, swap chains
 * that present into a window, buffers (vertex, index, constant; Map and
 * UpdateSubresource), 2D textures with shader resource, render target and depth
 * stencil views, input layouts, vertex and pixel shaders, sampler (point or
 * linear; wrap, mirror, clamp, border), rasterizer, depth-stencil and blend
 * states, Draw, DrawIndexed and DrawInstanced with triangle lists and strips.
 * Not implemented: geometry shaders, stencil, multisampling, line and point
 * primitives, stream output, mip levels below the first, HLSL compilation at
 * run time (D3D10CompileShader fails: programs must ship compiled shaders).
 *
 * The COM method tables are generated from mingw-w64's headers by
 * tools/gen_d3d10.py; methods that are not implemented return E_NOTIMPL. */
#include "w32.h"
#include "r3d.h"
#include "dxbc.h"

bool w32_hwnd_client(uintptr_t hwnd, canvas_t **surf, rect_t *client);
void blit_scaled_alpha(canvas_t *dst, rect_t r, canvas_t *src, int alpha);

double floor(double); double ceil(double);
static inline float floorf_(float x) { return (float)floor(x); }
static inline float ceilf_(float x) { return (float)ceil(x); }

typedef int32_t HRESULT;
#define S_OK 0
#define S_FALSE 1
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_FAIL ((HRESULT)0x80004005)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)

/* ---------------- COM plumbing ---------------- */
enum { IF_SWAP, IF_FACTORY, IF_ADAPTER, IF_DXGIDEV, IF_DEVICE, IF_BUFFER, IF_TEX2D, IF_RTV, IF_DSV, IF_SRV,
       IF_VS, IF_PS, IF_GS, IF_LAYOUT, IF_SAMPLER, IF_RAST, IF_DEPTH, IF_BLEND, IF_COUNT };
typedef struct { int iface, index, nargs; const char *name; } method_t;
#define IDXGISwapChain IF_SWAP
#define IDXGIFactory IF_FACTORY
#define IDXGIAdapter IF_ADAPTER
#define IDXGIDevice IF_DXGIDEV
#define ID3D10Device IF_DEVICE
#define ID3D10Buffer IF_BUFFER
#define ID3D10Texture2D IF_TEX2D
#define ID3D10RenderTargetView IF_RTV
#define ID3D10DepthStencilView IF_DSV
#define ID3D10ShaderResourceView IF_SRV
#define ID3D10VertexShader IF_VS
#define ID3D10PixelShader IF_PS
#define ID3D10GeometryShader IF_GS
#define ID3D10InputLayout IF_LAYOUT
#define ID3D10SamplerState IF_SAMPLER
#define ID3D10RasterizerState IF_RAST
#define ID3D10DepthStencilState IF_DEPTH
#define ID3D10BlendState IF_BLEND
static const method_t methods[] = {
#define M(i, idx, name, n) { i, idx, n, #name },
#include "d3d10_methods.h"
#undef M
};

typedef uintptr_t A;
#define STUB(n, ...) static HRESULT WINAPI stub##n(void *self, ##__VA_ARGS__) { (void)self; return E_NOTIMPL; }
STUB(0) STUB(1, A a) STUB(2, A a, A b) STUB(3, A a, A b, A c) STUB(4, A a, A b, A c, A d) STUB(5, A a, A b, A c, A d, A e)
STUB(6, A a, A b, A c, A d, A e, A f) STUB(7, A a, A b, A c, A d, A e, A f, A g) STUB(8, A a, A b, A c, A d, A e, A f, A g, A h)
STUB(9, A a, A b, A c, A d, A e, A f, A g, A h, A i) STUB(10, A a, A b, A c, A d, A e, A f, A g, A h, A i, A j)
static void *const stubs[] = { stub0, stub1, stub2, stub3, stub4, stub5, stub6, stub7, stub8, stub9, stub10 };

typedef struct { int iface; const char *name; void *fn; } impl_t;
static void *vtables[IF_COUNT][100];
static bool vt_ready;
static void build_vtables(void);

typedef struct com { void **vt; int refs; int kind; struct device *dev; } com_t;
static void *com_new(int kind, size_t size, struct device *dev) {
    build_vtables();
    com_t *o = w32_alloc(w32_cur(), (uint32_t)size);
    if (!o) return NULL;
    memset(o, 0, size);
    o->vt = vtables[kind]; o->refs = 1; o->kind = kind; o->dev = dev;
    return o;
}
static void ref(void *o) { if (o) ((com_t *)o)->refs++; }
static uint32_t WINAPI Release(com_t *self);
static void unref(void *o) { if (o) Release(o); }
static void set_ref(void **slot, void *o) { ref(o); unref(*slot); *slot = o; }

/* ---------------- objects ---------------- */
typedef struct { uint32_t w, h, num, den, format, scanline, scaling; } mode_desc_t;
typedef struct {
    mode_desc_t buf; uint32_t ms_count, ms_quality, usage, count; uintptr_t hwnd; int32_t windowed; uint32_t effect, flags;
} swap_desc_t;
typedef struct { uint32_t bytes, usage, bind, cpu, misc; } buffer_desc_t;
typedef struct { uint32_t w, h, mips, array, format, ms_count, ms_quality, usage, bind, cpu, misc; } tex2d_desc_t;
typedef struct { const void *mem; uint32_t pitch, slice; } subres_t;
typedef struct { const char *name; uint32_t index, format, slot, offset, cls, step; } elem_desc_t;
typedef struct { int32_t x, y; uint32_t w, h; float minz, maxz; } viewport_t;
typedef struct { uint32_t fill, cull; int32_t front_ccw, bias; float bias_clamp, slope_bias; int32_t depth_clip, scissor, ms, aa; } rast_desc_t;
typedef struct { uint32_t fail, dfail, pass, func; } stencil_op_t;
typedef struct { int32_t enable; uint32_t write_mask, func; int32_t stencil; uint8_t rmask, wmask, pad[2]; stencil_op_t front, back; } depth_desc_t;
typedef struct { int32_t a2c, enable[8]; uint32_t src, dst, op, src_a, dst_a, op_a; uint8_t mask[8]; } blend_desc_t;
typedef struct { uint32_t filter, au, av, aw; float bias; uint32_t aniso, cmp; float border[4], minlod, maxlod; } sampler_desc_t;

typedef struct { com_t c; buffer_desc_t d; uint8_t *data; } buffer_t;
typedef struct { com_t c; tex2d_desc_t d; uint32_t pitch, bpp; uint8_t *data, *scratch; } tex_t;
typedef struct { com_t c; tex_t *tex; } view_t;
typedef struct { com_t c; dxbc_shader_t *sh; } shader_t;
typedef struct { int reg, slot, format, sysval; uint32_t offset; bool instance; } layout_elem_t;
typedef struct { com_t c; layout_elem_t e[DX_MAX_SIG]; int n; } layout_t;
typedef struct { com_t c; sampler_desc_t d; } sampler_t;
typedef struct { com_t c; rast_desc_t d; } rast_t;
typedef struct { com_t c; depth_desc_t d; } depthst_t;
typedef struct { com_t c; blend_desc_t d; } blendst_t;

typedef struct device {
    com_t c;
    struct { buffer_t *b; uint32_t stride, offset; } vb[8];
    buffer_t *ib; uint32_t ib_format, ib_offset;
    uint32_t topology;
    layout_t *layout;
    shader_t *vs, *ps;
    buffer_t *vs_cb[14], *ps_cb[14];
    view_t *srv[16];
    sampler_t *smp[16];
    view_t *rtv, *dsv;
    viewport_t vp; bool vp_set;
    rast_t *rs; depthst_t *ds; blendst_t *bs;
} device_t;
typedef struct { com_t c; swap_desc_t d; tex_t *back; canvas_t *frame; } swap_t;

/* ---------------- formats ---------------- */
enum { F_RGBA32F = 2, F_RGB32F = 6, F_RG32F = 16, F_RGBA8 = 28, F_RGBA8_SRGB = 29, F_RGBA8_UINT = 30, F_D32F = 40, F_R32F = 41,
       F_R32_UINT = 42, F_D24S8 = 45, F_RG8 = 49, F_R16_UINT = 57, F_R8 = 61, F_BGRA8 = 87, F_BGRX8 = 88, F_BGRA8_SRGB = 91 };
static int fmt_bytes(uint32_t f) {
    switch (f) {
    case F_RGBA32F: return 16; case F_RGB32F: return 12; case F_RG32F: return 8;
    case F_RGBA8: case F_RGBA8_SRGB: case F_RGBA8_UINT: case F_BGRA8: case F_BGRX8: case F_BGRA8_SRGB:
    case F_D32F: case F_R32F: case F_R32_UINT: case F_D24S8: return 4;
    case F_RG8: case F_R16_UINT: return 2;
    case F_R8: return 1;
    }
    return 0;
}
static void read_px(uint32_t f, const uint8_t *p, float *o) {
    const float *fp = (const float *)p;
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    switch (f) {
    case F_RGBA32F: o[0] = fp[0]; o[1] = fp[1]; o[2] = fp[2]; o[3] = fp[3]; break;
    case F_RGB32F: o[0] = fp[0]; o[1] = fp[1]; o[2] = fp[2]; break;
    case F_RG32F: o[0] = fp[0]; o[1] = fp[1]; break;
    case F_R32F: case F_D32F: case F_D24S8: o[0] = fp[0]; break;
    case F_RGBA8: case F_RGBA8_SRGB: for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f; break;
    case F_BGRA8: case F_BGRA8_SRGB: o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f; break;
    case F_BGRX8: o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; break;
    case F_RG8: o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f; break;
    case F_R8: o[0] = p[0] / 255.0f; break;
    }
}
/* vertex attributes keep integer formats as raw integers */
static void read_attr(uint32_t f, const uint8_t *p, vec4_t *o) {
    if (f == F_RGBA8_UINT) { for (int i = 0; i < 4; i++) o->u[i] = p[i]; return; }
    if (f == F_R32_UINT) { o->u[0] = *(const uint32_t *)p; o->u[1] = o->u[2] = 0; o->u[3] = 1; return; }
    read_px(f, p, o->f);
}
static uint8_t to8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255 + 0.5f); }
static void write_px(uint32_t f, uint8_t *p, const float *c) {
    float *fp = (float *)p;
    switch (f) {
    case F_RGBA32F: fp[0] = c[0]; fp[1] = c[1]; fp[2] = c[2]; fp[3] = c[3]; break;
    case F_RGB32F: fp[0] = c[0]; fp[1] = c[1]; fp[2] = c[2]; break;
    case F_RG32F: fp[0] = c[0]; fp[1] = c[1]; break;
    case F_R32F: fp[0] = c[0]; break;
    case F_RGBA8: case F_RGBA8_SRGB: for (int i = 0; i < 4; i++) p[i] = to8(c[i]); break;
    case F_BGRA8: case F_BGRA8_SRGB: case F_BGRX8: p[0] = to8(c[2]); p[1] = to8(c[1]); p[2] = to8(c[0]); p[3] = f == F_BGRX8 ? 255 : to8(c[3]); break;
    case F_RG8: p[0] = to8(c[0]); p[1] = to8(c[1]); break;
    case F_R8: p[0] = to8(c[0]); break;
    }
}
static inline uint8_t *texel(tex_t *t, int x, int y) { return t->data + (size_t)y * t->pitch + (size_t)x * t->bpp; }

/* ---------------- IUnknown and device children ---------------- */
static HRESULT WINAPI QueryInterface(com_t *self, const void *iid, void **out) {
    (void)iid;
    if (!out) return E_INVALIDARG;
    /* one object per interface family: hand back the object itself for its own
     * interface and the ones it derives from (resource, device child, unknown) */
    *out = self; self->refs++;
    return S_OK;
}
static uint32_t WINAPI AddRef(com_t *self) { return ++self->refs; }
static void device_unbind_all(device_t *d);
static uint32_t WINAPI Release(com_t *self) {
    if (--self->refs > 0) return self->refs;
    switch (self->kind) {
    case IF_BUFFER: kfree(((buffer_t *)self)->data); break;
    case IF_TEX2D: kfree(((tex_t *)self)->data); kfree(((tex_t *)self)->scratch); break;
    case IF_RTV: case IF_DSV: case IF_SRV: unref(((view_t *)self)->tex); break;
    case IF_VS: case IF_PS: case IF_GS: dxbc_free(((shader_t *)self)->sh); break;
    case IF_SWAP: unref(((swap_t *)self)->back); if (((swap_t *)self)->frame) canvas_free(((swap_t *)self)->frame); break;
    case IF_DEVICE: device_unbind_all((device_t *)self); break;
    }
    self->vt = NULL;
    w32_free(w32_cur(), self);
    return 0;
}
static void WINAPI GetDevice(com_t *self, device_t **out) { if (out) { *out = self->dev; ref(self->dev); } }
static HRESULT WINAPI GetPrivateData(com_t *s, const void *g, uint32_t *size, void *data) { (void)s; (void)g; (void)data; if (size) *size = 0; return DXGI_ERROR_NOT_FOUND; }
static HRESULT WINAPI SetPrivateData(com_t *s, const void *g, uint32_t size, const void *data) { (void)s; (void)g; (void)size; (void)data; return S_OK; }
static HRESULT WINAPI SetPrivateDataInterface(com_t *s, const void *g, const void *u) { (void)s; (void)g; (void)u; return S_OK; }
static HRESULT WINAPI GetParent(com_t *s, const void *iid, void **out) { (void)s; (void)iid; if (out) *out = NULL; return E_NOINTERFACE; }

/* ---------------- resources ---------------- */
static void WINAPI GetType(com_t *s, uint32_t *t) { if (t) *t = s->kind == IF_BUFFER ? 1 : 3; }
static void WINAPI SetEvictionPriority(com_t *s, uint32_t p) { (void)s; (void)p; }
static uint32_t WINAPI GetEvictionPriority(com_t *s) { (void)s; return 0; }
static HRESULT WINAPI BufMap(buffer_t *b, uint32_t type, uint32_t flags, void **out) { (void)type; (void)flags; if (!out) return E_INVALIDARG; *out = b->data; return S_OK; }
static void WINAPI BufUnmap(buffer_t *b) { (void)b; }
static void WINAPI BufGetDesc(buffer_t *b, buffer_desc_t *d) { if (d) *d = b->d; }
typedef struct { void *data; uint32_t pitch; } mapped2d_t;
static HRESULT WINAPI TexMap(tex_t *t, uint32_t sub, uint32_t type, uint32_t flags, mapped2d_t *m) {
    (void)type; (void)flags;
    if (!m) return E_INVALIDARG;
    uint32_t mip = sub % MAX(t->d.mips, 1);
    if (mip) {                                         /* smaller mips are accepted and not used */
        uint32_t w = MAX(1u, t->d.w >> mip), h = MAX(1u, t->d.h >> mip);
        kfree(t->scratch); t->scratch = kzalloc(w * h * t->bpp);
        m->data = t->scratch; m->pitch = w * t->bpp;
        return t->scratch ? S_OK : E_OUTOFMEMORY;
    }
    m->data = t->data; m->pitch = t->pitch;
    return S_OK;
}
static void WINAPI TexUnmap(tex_t *t, uint32_t sub) { (void)t; (void)sub; }
static void WINAPI TexGetDesc(tex_t *t, tex2d_desc_t *d) { if (d) *d = t->d; }
static void WINAPI ViewGetResource(view_t *v, tex_t **out) { if (out) { *out = v->tex; ref(v->tex); } }

static tex_t *tex_new(device_t *dev, const tex2d_desc_t *d) {
    int bpp = fmt_bytes(d->format);
    if (!bpp || !d->w || !d->h || d->w > 8192 || d->h > 8192) return NULL;
    tex_t *t = com_new(IF_TEX2D, sizeof(tex_t), dev);
    if (!t) return NULL;
    t->d = *d; if (!t->d.mips) t->d.mips = 1;
    t->bpp = bpp; t->pitch = d->w * bpp;
    t->data = kzalloc((size_t)t->pitch * d->h);
    if (!t->data) { Release(&t->c); return NULL; }
    return t;
}

/* ---------------- swap chain ---------------- */
static bool swap_buffers(swap_t *s) {
    canvas_t *surf; rect_t cr;
    uint32_t w = s->d.buf.w, h = s->d.buf.h;
    if ((!w || !h) && w32_hwnd_client(s->d.hwnd, &surf, &cr)) { w = cr.w; h = cr.h; }
    w = MAX(1u, w); h = MAX(1u, h);
    s->d.buf.w = w; s->d.buf.h = h;
    if (!s->d.buf.format) s->d.buf.format = F_RGBA8;
    tex2d_desc_t td = { w, h, 1, 1, s->d.buf.format, 1, 0, 0, 0x20 /* RENDER_TARGET */, 0, 0 };
    tex_t *t = tex_new(s->c.dev, &td);
    if (!t) return false;
    unref(s->back); s->back = t;
    return true;
}
static HRESULT WINAPI Present(swap_t *s, uint32_t sync, uint32_t flags) {
    (void)sync;
    if (flags & 1) return S_OK;                         /* DXGI_PRESENT_TEST */
    canvas_t *surf; rect_t cr;
    tex_t *t = s->back;
    if (!t || !w32_hwnd_client(s->d.hwnd, &surf, &cr)) return S_OK;
    if (!s->frame || s->frame->w != (int)t->d.w || s->frame->h != (int)t->d.h) {
        if (s->frame) canvas_free(s->frame);
        s->frame = canvas_new(t->d.w, t->d.h);
        if (!s->frame) return E_OUTOFMEMORY;
    }
    for (uint32_t y = 0; y < t->d.h; y++) {
        uint32_t *dst = s->frame->px + (size_t)y * s->frame->stride;
        const uint8_t *src = texel(t, 0, y);
        if (t->d.format == F_BGRA8 || t->d.format == F_BGRX8 || t->d.format == F_BGRA8_SRGB)
            for (uint32_t x = 0; x < t->d.w; x++) dst[x] = ((const uint32_t *)src)[x] | 0xFF000000u;
        else if (t->d.format == F_RGBA8 || t->d.format == F_RGBA8_SRGB)
            for (uint32_t x = 0; x < t->d.w; x++, src += 4) dst[x] = 0xFF000000u | (src[0] << 16) | (src[1] << 8) | src[2];
        else for (uint32_t x = 0; x < t->d.w; x++, src += t->bpp) { float c[4]; read_px(t->d.format, src, c); dst[x] = 0xFF000000u | (to8(c[0]) << 16) | (to8(c[1]) << 8) | to8(c[2]); }
    }
    if (cr.w == (int)t->d.w && cr.h == (int)t->d.h) blit(surf, cr.x, cr.y, s->frame, 0, 0, cr.w, cr.h);
    else { rect_t old = surf->clip; surf->clip = rect_intersect(cr, R(0, 0, surf->w, surf->h)); blit_scaled_alpha(surf, cr, s->frame, 255); surf->clip = old; }
    wm_need_redraw = true;
    task_yield();
    return S_OK;
}
static HRESULT WINAPI GetBuffer(swap_t *s, uint32_t i, const void *iid, void **out) {
    (void)iid;
    if (!out || i) return E_INVALIDARG;
    *out = s->back; ref(s->back);
    return s->back ? S_OK : E_FAIL;
}
static HRESULT WINAPI SetFullscreenState(swap_t *s, int32_t full, void *target) { (void)s; (void)target; return full ? DXGI_ERROR_UNSUPPORTED : S_OK; }
static HRESULT WINAPI GetFullscreenState(swap_t *s, int32_t *full, void **target) { (void)s; if (full) *full = 0; if (target) *target = NULL; return S_OK; }
static HRESULT WINAPI SwapGetDesc(swap_t *s, swap_desc_t *d) { if (!d) return E_INVALIDARG; *d = s->d; return S_OK; }
static HRESULT WINAPI ResizeBuffers(swap_t *s, uint32_t count, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
    (void)count; (void)flags;
    s->d.buf.w = w; s->d.buf.h = h;
    if (fmt) s->d.buf.format = fmt;
    return swap_buffers(s) ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI ResizeTarget(swap_t *s, const mode_desc_t *m) { (void)s; (void)m; return S_OK; }
static HRESULT WINAPI GetContainingOutput(swap_t *s, void **out) { (void)s; if (out) *out = NULL; return DXGI_ERROR_UNSUPPORTED; }
static HRESULT WINAPI GetLastPresentCount(swap_t *s, uint32_t *n) { (void)s; if (n) *n = 0; return S_OK; }

static HRESULT make_swapchain(device_t *dev, swap_desc_t *d, swap_t **out) {
    if (!d || !out) return E_INVALIDARG;
    swap_t *s = com_new(IF_SWAP, sizeof(swap_t), dev);
    if (!s) return E_OUTOFMEMORY;
    s->d = *d;
    if (!swap_buffers(s)) { Release(&s->c); return E_OUTOFMEMORY; }
    d->buf.w = s->d.buf.w; d->buf.h = s->d.buf.h;
    *out = s;
    return S_OK;
}

/* ---------------- DXGI factory and adapter ---------------- */
static HRESULT WINAPI EnumAdapters(com_t *f, uint32_t i, com_t **out) {
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (i) return DXGI_ERROR_NOT_FOUND;
    *out = com_new(IF_ADAPTER, sizeof(com_t), f->dev);
    return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI MakeWindowAssociation(com_t *f, uintptr_t w, uint32_t fl) { (void)f; (void)w; (void)fl; return S_OK; }
static HRESULT WINAPI GetWindowAssociation(com_t *f, uintptr_t *w) { (void)f; if (w) *w = 0; return S_OK; }
static HRESULT WINAPI CreateSwapChain(com_t *f, device_t *dev, swap_desc_t *d, swap_t **out) { (void)f; return make_swapchain(dev, d, out); }
static HRESULT WINAPI EnumOutputs(com_t *a, uint32_t i, void **out) { (void)a; (void)i; if (out) *out = NULL; return DXGI_ERROR_NOT_FOUND; }
static HRESULT WINAPI AdapterGetDesc(com_t *a, uint16_t *d) {
    (void)a;
    if (!d) return E_INVALIDARG;
    memset(d, 0, 128 * 2 + 16 + 3 * sizeof(uintptr_t) + 8);
    const char *n = "WRLD 3D (software rasterizer)";
    for (int i = 0; n[i]; i++) d[i] = n[i];
    uintptr_t *mem = (uintptr_t *)((uint8_t *)d + 128 * 2 + 16);
    mem[2] = 256u << 20;                               /* shared system memory */
    return S_OK;
}
static HRESULT WINAPI CheckInterfaceSupport(com_t *a, const void *iid, int64_t *ver) { (void)a; (void)iid; if (ver) *ver = 0; return S_OK; }

/* ---------------- device: resource creation ---------------- */
static HRESULT WINAPI CreateBuffer(device_t *d, const buffer_desc_t *bd, const subres_t *init, buffer_t **out) {
    if (!bd || !bd->bytes || bd->bytes > (256u << 20)) return E_INVALIDARG;
    if (!out) return S_FALSE;
    buffer_t *b = com_new(IF_BUFFER, sizeof(buffer_t), d);
    if (!b) return E_OUTOFMEMORY;
    b->d = *bd;
    b->data = kzalloc(bd->bytes + 16);
    if (!b->data) { Release(&b->c); return E_OUTOFMEMORY; }
    if (init && init->mem) memcpy(b->data, init->mem, bd->bytes);
    *out = b;
    return S_OK;
}
static HRESULT WINAPI CreateTexture2D(device_t *d, const tex2d_desc_t *td, const subres_t *init, tex_t **out) {
    if (!td) return E_INVALIDARG;
    if (!fmt_bytes(td->format)) { klog("w32: d3d10: texture format %u is not supported", td->format); return E_INVALIDARG; }
    if (!out) return S_FALSE;
    tex_t *t = tex_new(d, td);
    if (!t) return E_OUTOFMEMORY;
    if (init && init->mem)
        for (uint32_t y = 0; y < td->h; y++) memcpy(texel(t, 0, y), (const uint8_t *)init->mem + (size_t)y * (init->pitch ? init->pitch : t->pitch), t->pitch);
    *out = t;
    return S_OK;
}
static HRESULT make_view(device_t *d, int kind, tex_t *t, view_t **out) {
    if (!t || t->c.kind != IF_TEX2D) return E_INVALIDARG;
    if (!out) return S_FALSE;
    view_t *v = com_new(kind, sizeof(view_t), d);
    if (!v) return E_OUTOFMEMORY;
    v->tex = t; ref(t);
    *out = v;
    return S_OK;
}
static HRESULT WINAPI CreateShaderResourceView(device_t *d, tex_t *t, const void *desc, view_t **out) { (void)desc; return make_view(d, IF_SRV, t, out); }
static HRESULT WINAPI CreateRenderTargetView(device_t *d, tex_t *t, const void *desc, view_t **out) { (void)desc; return make_view(d, IF_RTV, t, out); }
static HRESULT WINAPI CreateDepthStencilView(device_t *d, tex_t *t, const void *desc, view_t **out) {
    (void)desc;
    if (t && t->bpp != 4) { klog("w32: d3d10: only 32-bit depth formats are supported"); return E_INVALIDARG; }
    return make_view(d, IF_DSV, t, out);
}
static HRESULT make_shader(device_t *d, int kind, const void *code, uintptr_t len, shader_t **out) {
    char err[96];
    dxbc_shader_t *sh = dxbc_load(code, len, err, sizeof err);
    if (!sh) { klog("w32: d3d10: shader rejected: %s", err); return E_INVALIDARG; }
    if ((kind == IF_VS) != (dxbc_type(sh) == 1)) { dxbc_free(sh); klog("w32: d3d10: wrong shader type"); return E_INVALIDARG; }
    if (!out) { dxbc_free(sh); return S_FALSE; }
    shader_t *s = com_new(kind, sizeof(shader_t), d);
    if (!s) { dxbc_free(sh); return E_OUTOFMEMORY; }
    s->sh = sh;
    *out = s;
    return S_OK;
}
static HRESULT WINAPI CreateVertexShader(device_t *d, const void *code, uintptr_t len, shader_t **out) { return make_shader(d, IF_VS, code, len, out); }
static HRESULT WINAPI CreatePixelShader(device_t *d, const void *code, uintptr_t len, shader_t **out) { return make_shader(d, IF_PS, code, len, out); }
static HRESULT WINAPI CreateGeometryShader(device_t *d, const void *code, uintptr_t len, shader_t **out) {
    (void)d; (void)code; (void)len; if (out) *out = NULL;
    klog("w32: d3d10: geometry shaders are not supported");
    return E_NOTIMPL;
}
static HRESULT WINAPI CreateInputLayout(device_t *d, const elem_desc_t *el, uint32_t n, const void *code, uintptr_t len, layout_t **out) {
    if (!el || n > DX_MAX_SIG) return E_INVALIDARG;
    char err[96];
    dxbc_shader_t *sh = dxbc_load(code, len, err, sizeof err);
    if (!sh) { klog("w32: d3d10: input layout: %s", err); return E_INVALIDARG; }
    const dxsig_t *sig; int ns = dxbc_inputs(sh, &sig);
    layout_t *l = com_new(IF_LAYOUT, sizeof(layout_t), d);
    if (!l) { dxbc_free(sh); return E_OUTOFMEMORY; }
    uint32_t next_off[16] = { 0 };
    for (uint32_t i = 0; i < n; i++) {
        layout_elem_t *e = &l->e[l->n++];
        e->slot = el[i].slot & 15; e->format = el[i].format; e->instance = el[i].cls == 1;
        e->offset = el[i].offset == 0xFFFFFFFFu ? next_off[e->slot] : el[i].offset;
        next_off[e->slot] = e->offset + fmt_bytes(e->format);
        e->reg = -1;
        for (int k = 0; k < ns; k++) if (!strcasecmp(sig[k].name, el[i].name) && sig[k].index == (int)el[i].index) e->reg = sig[k].reg;
        if (!fmt_bytes(e->format)) { klog("w32: d3d10: vertex format %u is not supported", e->format); e->reg = -1; }
    }
    /* system values the shader reads (SV_VertexID, SV_InstanceID) */
    for (int k = 0; k < ns && l->n < DX_MAX_SIG; k++)
        if (sig[k].sysval == 6 || sig[k].sysval == 8) { layout_elem_t *e = &l->e[l->n++]; e->reg = sig[k].reg; e->sysval = sig[k].sysval; e->format = 0; }
    dxbc_free(sh);
    if (!out) { Release(&l->c); return S_FALSE; }
    *out = l;
    return S_OK;
}
#define STATE_CREATOR(fn, kind, type, desc_t) \
    static HRESULT WINAPI fn(device_t *d, const desc_t *desc, type **out) { \
        if (!desc) return E_INVALIDARG; if (!out) return S_FALSE; \
        type *s = com_new(kind, sizeof(type), d); if (!s) return E_OUTOFMEMORY; \
        s->d = *desc; *out = s; return S_OK; }
STATE_CREATOR(CreateBlendState, IF_BLEND, blendst_t, blend_desc_t)
STATE_CREATOR(CreateDepthStencilState, IF_DEPTH, depthst_t, depth_desc_t)
STATE_CREATOR(CreateRasterizerState, IF_RAST, rast_t, rast_desc_t)
STATE_CREATOR(CreateSamplerState, IF_SAMPLER, sampler_t, sampler_desc_t)
static void WINAPI StateGetDesc(com_t *s, void *d) {
    if (!d) return;
    switch (s->kind) {
    case IF_BLEND: memcpy(d, &((blendst_t *)s)->d, sizeof(blend_desc_t)); break;
    case IF_DEPTH: memcpy(d, &((depthst_t *)s)->d, sizeof(depth_desc_t)); break;
    case IF_RAST: memcpy(d, &((rast_t *)s)->d, sizeof(rast_desc_t)); break;
    case IF_SAMPLER: memcpy(d, &((sampler_t *)s)->d, sizeof(sampler_desc_t)); break;
    }
}
static HRESULT WINAPI CheckFormatSupport(device_t *d, uint32_t f, uint32_t *s) {
    (void)d; if (!s) return E_INVALIDARG;
    *s = fmt_bytes(f) ? 0x1 | 0x2 | 0x4 | 0x20 | 0x100 | 0x4000 | 0x10000 : 0;   /* buffer, IA, texture2d, sample, render target, depth... */
    return *s ? S_OK : E_FAIL;
}
static HRESULT WINAPI CheckMultisampleQualityLevels(device_t *d, uint32_t f, uint32_t n, uint32_t *q) { (void)d; (void)f; if (q) *q = n == 1; return S_OK; }
static HRESULT WINAPI GetDeviceRemovedReason(device_t *d) { (void)d; return S_OK; }
static HRESULT WINAPI SetExceptionMode(device_t *d, uint32_t m) { (void)d; (void)m; return S_OK; }
static uint32_t WINAPI GetExceptionMode(device_t *d) { (void)d; return 0; }

/* ---------------- device: pipeline state ---------------- */
static void set_list(void **slots, int max, uint32_t start, uint32_t n, void *const *objs) {
    for (uint32_t i = 0; i < n && start + i < (uint32_t)max; i++) set_ref(&slots[start + i], objs ? objs[i] : NULL);
}
static void WINAPI VSSetConstantBuffers(device_t *d, uint32_t s, uint32_t n, buffer_t *const *b) { set_list((void **)d->vs_cb, 14, s, n, (void *const *)b); }
static void WINAPI PSSetConstantBuffers(device_t *d, uint32_t s, uint32_t n, buffer_t *const *b) { set_list((void **)d->ps_cb, 14, s, n, (void *const *)b); }
static void WINAPI PSSetShaderResources(device_t *d, uint32_t s, uint32_t n, view_t *const *v) { set_list((void **)d->srv, 16, s, n, (void *const *)v); }
static void WINAPI PSSetSamplers(device_t *d, uint32_t s, uint32_t n, sampler_t *const *v) { set_list((void **)d->smp, 16, s, n, (void *const *)v); }
static void WINAPI VSSetShader(device_t *d, shader_t *s) { set_ref((void **)&d->vs, s); }
static void WINAPI PSSetShader(device_t *d, shader_t *s) { set_ref((void **)&d->ps, s); }
static void WINAPI GSSetShader(device_t *d, shader_t *s) { (void)d; if (s) klog("w32: d3d10: geometry shader ignored"); }
static void WINAPI IASetInputLayout(device_t *d, layout_t *l) { set_ref((void **)&d->layout, l); }
static void WINAPI IASetVertexBuffers(device_t *d, uint32_t s, uint32_t n, buffer_t *const *b, const uint32_t *stride, const uint32_t *off) {
    for (uint32_t i = 0; i < n && s + i < 8; i++) {
        set_ref((void **)&d->vb[s + i].b, b ? b[i] : NULL);
        d->vb[s + i].stride = stride ? stride[i] : 0; d->vb[s + i].offset = off ? off[i] : 0;
    }
}
static void WINAPI IASetIndexBuffer(device_t *d, buffer_t *b, uint32_t fmt, uint32_t off) { set_ref((void **)&d->ib, b); d->ib_format = fmt; d->ib_offset = off; }
static void WINAPI IASetPrimitiveTopology(device_t *d, uint32_t t) { d->topology = t; }
static void WINAPI IAGetPrimitiveTopology(device_t *d, uint32_t *t) { if (t) *t = d->topology; }
static void WINAPI OMSetRenderTargets(device_t *d, uint32_t n, view_t *const *rt, view_t *ds) {
    set_ref((void **)&d->rtv, n && rt ? rt[0] : NULL);
    set_ref((void **)&d->dsv, ds);
}
static void WINAPI OMGetRenderTargets(device_t *d, uint32_t n, view_t **rt, view_t **ds) {
    for (uint32_t i = 0; rt && i < n; i++) { rt[i] = i ? NULL : d->rtv; ref(rt[i]); }
    if (ds) { *ds = d->dsv; ref(d->dsv); }
}
static void WINAPI OMSetBlendState(device_t *d, blendst_t *b, const float *factor, uint32_t mask) { (void)factor; (void)mask; set_ref((void **)&d->bs, b); }
static void WINAPI OMSetDepthStencilState(device_t *d, depthst_t *s, uint32_t ref_) { (void)ref_; set_ref((void **)&d->ds, s); }
static void WINAPI RSSetState(device_t *d, rast_t *s) { set_ref((void **)&d->rs, s); }
static void WINAPI RSSetViewports(device_t *d, uint32_t n, const viewport_t *v) { if (n && v) { d->vp = v[0]; d->vp_set = true; } }
static void WINAPI RSGetViewports(device_t *d, uint32_t *n, viewport_t *v) { if (n) { if (v && *n) v[0] = d->vp; *n = d->vp_set; } }
static void WINAPI RSSetScissorRects(device_t *d, uint32_t n, const void *r) { (void)d; (void)n; (void)r; }
static void WINAPI ClearRenderTargetView(device_t *d, view_t *v, const float *c) {
    (void)d;
    if (!v || !c || !v->tex) return;
    tex_t *t = v->tex;
    uint8_t px[16]; write_px(t->d.format, px, c);
    if (t->bpp == 4) {
        uint32_t v; memcpy(&v, px, 4);
        for (uint32_t y = 0; y < t->d.h; y++) { uint32_t *row = (uint32_t *)texel(t, 0, y); for (uint32_t x = 0; x < t->d.w; x++) row[x] = v; }
    } else for (uint32_t y = 0; y < t->d.h; y++) for (uint32_t x = 0; x < t->d.w; x++) memcpy(texel(t, x, y), px, t->bpp);
}
static void WINAPI ClearDepthStencilView(device_t *d, view_t *v, uint32_t flags, float depth, uint8_t stencil) {
    (void)d; (void)stencil;
    if (!v || !v->tex || !(flags & 1)) return;
    tex_t *t = v->tex;
    for (uint32_t y = 0; y < t->d.h; y++) { float *row = (float *)texel(t, 0, y); for (uint32_t x = 0; x < t->d.w; x++) row[x] = depth; }
}
static void WINAPI GenerateMips(device_t *d, view_t *v) { (void)d; (void)v; }
static void WINAPI Flush(device_t *d) { (void)d; }
static void WINAPI UpdateSubresource(device_t *d, com_t *res, uint32_t sub, const uint32_t *box, const void *src, uint32_t pitch, uint32_t depth_pitch) {
    (void)d; (void)depth_pitch;
    if (!res || !src) return;
    if (res->kind == IF_BUFFER) {
        buffer_t *b = (buffer_t *)res;
        uint32_t x0 = box ? box[0] : 0, x1 = box ? box[3] : b->d.bytes;
        if (x0 < x1 && x1 <= b->d.bytes) memcpy(b->data + x0, src, x1 - x0);
    } else if (res->kind == IF_TEX2D && sub == 0) {
        tex_t *t = (tex_t *)res;
        uint32_t x0 = box ? box[0] : 0, y0 = box ? box[1] : 0, x1 = box ? box[3] : t->d.w, y1 = box ? box[4] : t->d.h;
        if (x1 > t->d.w || y1 > t->d.h || x0 >= x1 || y0 >= y1) return;
        for (uint32_t y = y0; y < y1; y++) memcpy(texel(t, x0, y), (const uint8_t *)src + (size_t)(y - y0) * pitch, (x1 - x0) * t->bpp);
    }
}
static void WINAPI CopyResource(device_t *d, com_t *dst, com_t *src) {
    (void)d;
    if (!dst || !src || dst->kind != src->kind) return;
    if (dst->kind == IF_BUFFER) { buffer_t *a = (buffer_t *)dst, *b = (buffer_t *)src; memcpy(a->data, b->data, MIN(a->d.bytes, b->d.bytes)); }
    else if (dst->kind == IF_TEX2D) {
        tex_t *a = (tex_t *)dst, *b = (tex_t *)src;
        if (a->bpp == b->bpp) for (uint32_t y = 0; y < MIN(a->d.h, b->d.h); y++) memcpy(texel(a, 0, y), texel(b, 0, y), MIN(a->pitch, b->pitch));
    }
}
static void device_unbind_all(device_t *d) {
    for (int i = 0; i < 8; i++) set_ref((void **)&d->vb[i].b, NULL);
    for (int i = 0; i < 14; i++) { set_ref((void **)&d->vs_cb[i], NULL); set_ref((void **)&d->ps_cb[i], NULL); }
    for (int i = 0; i < 16; i++) { set_ref((void **)&d->srv[i], NULL); set_ref((void **)&d->smp[i], NULL); }
    set_ref((void **)&d->ib, NULL); set_ref((void **)&d->layout, NULL);
    set_ref((void **)&d->vs, NULL); set_ref((void **)&d->ps, NULL);
    set_ref((void **)&d->rtv, NULL); set_ref((void **)&d->dsv, NULL);
    set_ref((void **)&d->rs, NULL); set_ref((void **)&d->ds, NULL); set_ref((void **)&d->bs, NULL);
}
static void WINAPI ClearState(device_t *d) { device_unbind_all(d); d->topology = 0; d->vp_set = false; }

/* ---------------- texture sampling (callbacks for the interpreter) ---------------- */
typedef struct { device_t *d; } sctx_t;
static int wrap(int i, int n, uint32_t mode, bool *border) {
    switch (mode) {
    case 1: i %= n; return i < 0 ? i + n : i;                                      /* wrap */
    case 2: { int p = 2 * n; i %= p; if (i < 0) i += p; return i < n ? i : p - 1 - i; }   /* mirror */
    case 4: if (i < 0 || i >= n) *border = true; return CLAMP(i, 0, n - 1);        /* border */
    default: return CLAMP(i, 0, n - 1);                                            /* clamp */
    }
}
static void fetch(tex_t *t, const sampler_desc_t *sd, int x, int y, float *o) {
    bool border = false;
    uint32_t au = sd ? sd->au : 3, av = sd ? sd->av : 3;
    x = wrap(x, t->d.w, au, &border); y = wrap(y, t->d.h, av, &border);
    if (border) { for (int i = 0; i < 4; i++) o[i] = sd->border[i]; return; }
    const uint8_t *p = texel(t, x, y);
    if (t->d.format == F_RGBA8 || t->d.format == F_RGBA8_SRGB) { const float k = 1 / 255.0f; o[0] = p[0] * k; o[1] = p[1] * k; o[2] = p[2] * k; o[3] = p[3] * k; return; }
    read_px(t->d.format, p, o);
}
static void cb_sample(void *ctx, int ti, int si, const float *uv, float lod, float *out) {
    (void)lod;
    device_t *d = ((sctx_t *)ctx)->d;
    view_t *v = ti >= 0 && ti < 16 ? d->srv[ti] : NULL;
    if (!v || !v->tex) { out[0] = out[1] = out[2] = 0; out[3] = 0; return; }
    tex_t *t = v->tex;
    sampler_t *sm = si >= 0 && si < 16 ? d->smp[si] : NULL;
    const sampler_desc_t *sd = sm ? &sm->d : NULL;
    bool linear = sd ? (sd->filter & 0x04) || sd->filter == 0x55 : true;
    float fx = uv[0] * t->d.w - 0.5f, fy = uv[1] * t->d.h - 0.5f;
    if (fx != fx) fx = 0;
    if (fy != fy) fy = 0;
    fx = CLAMP(fx, -1e6f, 1e6f); fy = CLAMP(fy, -1e6f, 1e6f);
    if (!linear) { fetch(t, sd, (int)(fx + 0.5f + 1e5f) - 100000, (int)(fy + 0.5f + 1e5f) - 100000, out); return; }
    int x0 = (int)(fx + 1e5f) - 100000, y0 = (int)(fy + 1e5f) - 100000;
    float ax = fx - x0, ay = fy - y0, p[4][4];
    fetch(t, sd, x0, y0, p[0]); fetch(t, sd, x0 + 1, y0, p[1]); fetch(t, sd, x0, y0 + 1, p[2]); fetch(t, sd, x0 + 1, y0 + 1, p[3]);
    for (int i = 0; i < 4; i++) {
        float top = p[0][i] + (p[1][i] - p[0][i]) * ax, bot = p[2][i] + (p[3][i] - p[2][i]) * ax;
        out[i] = top + (bot - top) * ay;
    }
}
static void cb_load(void *ctx, int ti, const int32_t *xy, float *out) {
    device_t *d = ((sctx_t *)ctx)->d;
    view_t *v = ti >= 0 && ti < 16 ? d->srv[ti] : NULL;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!v || !v->tex || xy[0] < 0 || xy[1] < 0 || xy[0] >= (int)v->tex->d.w || xy[1] >= (int)v->tex->d.h) return;
    read_px(v->tex->d.format, texel(v->tex, xy[0], xy[1]), out);
}
static void cb_size(void *ctx, int ti, uint32_t *out) {
    device_t *d = ((sctx_t *)ctx)->d;
    view_t *v = ti >= 0 && ti < 16 ? d->srv[ti] : NULL;
    if (v && v->tex) { out[0] = v->tex->d.w; out[1] = v->tex->d.h; out[2] = 1; out[3] = v->tex->d.mips; }
}

/* ---------------- drawing ---------------- */
#define MAXV 16                                     /* interpolated vec4s between the shaders */
typedef struct { float pos[4]; vec4_t var[MAXV]; } pv_t;  /* a vertex after the vertex shader */
typedef struct {
    device_t *d;
    dxbc_env_t vs_env, ps_env; sctx_t sctx;
    int pos_reg;                                    /* VS output holding SV_Position */
    int nvar; int vs_reg[MAXV], ps_reg[MAXV];       /* VS output -> PS input */
    int ps_pos_reg, ps_face_reg, target_reg;
    tex_t *rt, *ds;
    rect_t clip;
    float vx, vy, vw, vh, vminz, vmaxz;
    bool cull_back, cull_front, front_ccw, depth_on, depth_write, blend;
    uint32_t depth_func;
    const blend_desc_t *bd;
} pipe_t;

static void env_for(pipe_t *P, dxbc_env_t *e, buffer_t **cbs) {
    memset(e, 0, sizeof *e);
    for (int i = 0; i < 14; i++) if (cbs[i]) { e->cb[i] = (const vec4_t *)cbs[i]->data; e->cb_rows[i] = cbs[i]->d.bytes / 16; }
    e->ctx = &P->sctx; e->sample = cb_sample; e->load = cb_load; e->size = cb_size;
}
static bool run_vs(pipe_t *P, uint32_t index, uint32_t instance, pv_t *o) {
    device_t *d = P->d;
    vec4_t in[DX_MAX_REG], out[DX_MAX_REG];
    memset(in, 0, sizeof in); memset(out, 0, sizeof out);
    layout_t *l = d->layout;
    for (int k = 0; l && k < l->n; k++) {
        layout_elem_t *e = &l->e[k];
        if (e->reg < 0 || e->reg >= DX_MAX_REG) continue;
        if (e->sysval == 6) { in[e->reg].u[0] = index; continue; }
        if (e->sysval == 8) { in[e->reg].u[0] = instance; continue; }
        buffer_t *b = d->vb[e->slot].b;
        if (!b) continue;
        uint32_t at = d->vb[e->slot].offset + (e->instance ? instance : index) * d->vb[e->slot].stride + e->offset;
        if (at + fmt_bytes(e->format) > b->d.bytes) return false;
        read_attr(e->format, b->data + at, &in[e->reg]);
    }
    dxbc_run(d->vs->sh, &P->vs_env, in, out);
    memcpy(o->pos, out[P->pos_reg].f, 16);
    for (int i = 0; i < P->nvar; i++) o->var[i] = out[P->vs_reg[i]];
    return true;
}

static float blend_factor(uint32_t f, int ch, const float *s, const float *dst) {
    switch (f) {
    case 1: return 0; case 2: return 1;
    case 3: return s[ch]; case 4: return 1 - s[ch];
    case 5: return s[3]; case 6: return 1 - s[3];
    case 7: return dst[3]; case 8: return 1 - dst[3];
    case 9: return dst[ch]; case 10: return 1 - dst[ch];
    case 11: { float a = s[3], b = 1 - dst[3]; return ch == 3 ? 1 : (a < b ? a : b); }
    }
    return 1;
}
static float blend_op(uint32_t op, float a, float b) {
    switch (op) { case 2: return a - b; case 3: return b - a; case 4: return a < b ? a : b; case 5: return a > b ? a : b; }
    return a + b;
}
static bool depth_pass(uint32_t f, float z, float cur) {
    switch (f) {
    case 1: return false; case 2: return z < cur; case 3: return z == cur; case 4: return z <= cur;
    case 5: return z > cur; case 6: return z != cur; case 7: return z >= cur;
    }
    return true;
}

/* screen-space vertex: position, 1/w and varyings divided by w */
typedef struct { float x, y, z, iw; vec4_t v[MAXV]; } sv_t;
static void shade_triangle(pipe_t *P, const sv_t *a, const sv_t *b, const sv_t *c) {
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0 || area != area) return;
    bool cw = area > 0;                             /* clockwise on screen (y grows downwards) */
    bool front = P->front_ccw ? !cw : cw;
    if ((front && P->cull_front) || (!front && P->cull_back)) return;
    int x0 = (int)floorf_(MIN(a->x, MIN(b->x, c->x))), x1 = (int)ceilf_(MAX(a->x, MAX(b->x, c->x)));
    int y0 = (int)floorf_(MIN(a->y, MIN(b->y, c->y))), y1 = (int)ceilf_(MAX(a->y, MAX(b->y, c->y)));
    x0 = MAX(x0, P->clip.x); y0 = MAX(y0, P->clip.y);
    x1 = MIN(x1, P->clip.x + P->clip.w); y1 = MIN(y1, P->clip.y + P->clip.h);
    if (x0 >= x1 || y0 >= y1) return;
    float inv = 1.0f / area;
    vec4_t in[DX_MAX_REG], out[DX_MAX_REG];
    memset(in, 0, sizeof in);
    /* barycentric weights are linear in x: step them along each row */
    float dw0 = (b->y - c->y) * inv, dw1 = (c->y - a->y) * inv;
    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f, px0 = x0 + 0.5f;
        float w0r = ((b->x - px0) * (c->y - py) - (c->x - px0) * (b->y - py)) * inv;
        float w1r = ((c->x - px0) * (a->y - py) - (a->x - px0) * (c->y - py)) * inv;
        for (int x = x0; x < x1; x++, w0r += dw0, w1r += dw1) {
            float px = x + 0.5f;
            float w0 = w0r, w1 = w1r, w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * a->z + w1 * b->z + w2 * c->z;
            if (z < 0 || z > 1) continue;
            float zv = P->vminz + z * (P->vmaxz - P->vminz);
            float *dz = P->ds ? (float *)texel(P->ds, x, y) : NULL;
            if (dz && P->depth_on && !depth_pass(P->depth_func, zv, *dz)) continue;
            float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw, w = iw != 0 ? 1.0f / iw : 0;
            for (int k = 0; k < P->nvar; k++) {
                vec4_t *r = &in[P->ps_reg[k]];
                for (int i = 0; i < 4; i++) r->f[i] = (w0 * a->v[k].f[i] + w1 * b->v[k].f[i] + w2 * c->v[k].f[i]) * w;
            }
            if (P->ps_pos_reg >= 0) { vec4_t *r = &in[P->ps_pos_reg]; r->f[0] = px; r->f[1] = py; r->f[2] = zv; r->f[3] = iw; }
            if (P->ps_face_reg >= 0) in[P->ps_face_reg].u[0] = front ? ~0u : 0;
            memset(out, 0, sizeof(vec4_t) * (P->target_reg + 1));
            if (!dxbc_run(P->d->ps->sh, &P->ps_env, in, out)) continue;
            if (dz && P->depth_on && P->depth_write) *dz = zv;
            if (!P->rt) continue;
            uint8_t *dst = texel(P->rt, x, y);
            float *s = out[P->target_reg].f, col[4];
            if (P->blend) {
                float dc[4]; read_px(P->rt->d.format, dst, dc);
                const blend_desc_t *bd = P->bd;
                for (int i = 0; i < 3; i++) col[i] = blend_op(bd->op, s[i] * blend_factor(bd->src, i, s, dc), dc[i] * blend_factor(bd->dst, i, s, dc));
                col[3] = blend_op(bd->op_a, s[3] * blend_factor(bd->src_a, 3, s, dc), dc[3] * blend_factor(bd->dst_a, 3, s, dc));
            } else memcpy(col, s, 16);
            write_px(P->rt->d.format, dst, col);
        }
    }
}
static void to_screen(pipe_t *P, const pv_t *v, sv_t *s) {
    float iw = 1.0f / v->pos[3];
    s->x = P->vx + (v->pos[0] * iw + 1) * 0.5f * P->vw;
    s->y = P->vy + (1 - v->pos[1] * iw) * 0.5f * P->vh;
    s->z = v->pos[2] * iw;
    s->iw = iw;
    for (int k = 0; k < P->nvar; k++) for (int i = 0; i < 4; i++) s->v[k].f[i] = v->var[k].f[i] * iw;
}
static void lerp_pv(pipe_t *P, const pv_t *a, const pv_t *b, float t, pv_t *o) {
    for (int i = 0; i < 4; i++) o->pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * t;
    for (int k = 0; k < P->nvar; k++) for (int i = 0; i < 4; i++) o->var[k].f[i] = a->var[k].f[i] + (b->var[k].f[i] - a->var[k].f[i]) * t;
}
/* clip against z >= 0 and w > 0 (Direct3D's near plane), then fan the polygon */
static void draw_triangle(pipe_t *P, const pv_t *a, const pv_t *b, const pv_t *c) {
    static pv_t poly[2][8];
    const pv_t *in[3] = { a, b, c };
    int n = 0;
    bool all_in = true;
    for (int i = 0; i < 3; i++) if (!(in[i]->pos[2] >= 0 && in[i]->pos[3] > 1e-6f)) all_in = false;
    sv_t s[8];
    if (all_in) {
        for (int i = 0; i < 3; i++) to_screen(P, in[i], &s[i]);
        shade_triangle(P, &s[0], &s[1], &s[2]);
        return;
    }
    pv_t *src = poly[0], *dst = poly[1];
    for (int i = 0; i < 3; i++) src[i] = *in[i];
    n = 3;
    for (int plane = 0; plane < 2; plane++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            pv_t *p = &src[i], *q = &src[(i + 1) % n];
            float dp = plane ? p->pos[3] - 1e-5f : p->pos[2], dq = plane ? q->pos[3] - 1e-5f : q->pos[2];
            if (dp >= 0) dst[m++] = *p;
            if ((dp >= 0) != (dq >= 0) && m < 8) lerp_pv(P, p, q, dp / (dp - dq), &dst[m++]);
        }
        n = m; pv_t *t = src; src = dst; dst = t;
        if (n < 3) return;
    }
    for (int i = 0; i < n; i++) to_screen(P, &src[i], &s[i]);
    for (int i = 1; i + 1 < n; i++) shade_triangle(P, &s[0], &s[i], &s[i + 1]);
}
static bool setup(pipe_t *P, device_t *d) {
    memset(P, 0, sizeof *P);
    P->d = d;
    if (!d->vs || !d->ps) return false;
    if (d->topology != 4 && d->topology != 5) { klog("w32: d3d10: primitive topology %u is not supported", d->topology); return false; }
    P->sctx.d = d;
    env_for(P, &P->vs_env, d->vs_cb); env_for(P, &P->ps_env, d->ps_cb);
    P->rt = d->rtv ? d->rtv->tex : NULL;
    P->ds = d->dsv ? d->dsv->tex : NULL;
    tex_t *size_of = P->rt ? P->rt : P->ds;
    if (!size_of) return false;
    const dxsig_t *vo, *pi, *po;
    int nvo = dxbc_outputs(d->vs->sh, &vo), npi = dxbc_inputs(d->ps->sh, &pi), npo = dxbc_outputs(d->ps->sh, &po);
    P->pos_reg = -1;
    for (int i = 0; i < nvo; i++) if (vo[i].sysval == 1) P->pos_reg = vo[i].reg;
    if (P->pos_reg < 0) { klog("w32: d3d10: the vertex shader writes no SV_Position"); return false; }
    P->ps_pos_reg = P->ps_face_reg = -1;
    for (int i = 0; i < npi; i++) {
        if (pi[i].sysval == 1) { P->ps_pos_reg = pi[i].reg; continue; }
        if (pi[i].sysval == 9) { P->ps_face_reg = pi[i].reg; continue; }
        for (int k = 0; k < nvo; k++)
            if (!strcasecmp(vo[k].name, pi[i].name) && vo[k].index == pi[i].index && P->nvar < MAXV) {
                P->vs_reg[P->nvar] = vo[k].reg; P->ps_reg[P->nvar] = pi[i].reg; P->nvar++;
                break;
            }
    }
    P->target_reg = 0;
    for (int i = 0; i < npo; i++) if (!strncasecmp(po[i].name, "SV_Target", 9) && po[i].index == 0) P->target_reg = po[i].reg;
    P->target_reg = CLAMP(P->target_reg, 0, DX_MAX_REG - 2);
    viewport_t vp = d->vp;
    if (!d->vp_set) vp = (viewport_t){ 0, 0, size_of->d.w, size_of->d.h, 0, 1 };
    P->vx = vp.x; P->vy = vp.y; P->vw = vp.w; P->vh = vp.h; P->vminz = vp.minz; P->vmaxz = vp.maxz;
    P->clip = rect_intersect(R(vp.x, vp.y, vp.w, vp.h), R(0, 0, size_of->d.w, size_of->d.h));
    if (P->ds && P->rt && (P->ds->d.w < P->rt->d.w || P->ds->d.h < P->rt->d.h)) P->clip = rect_intersect(P->clip, R(0, 0, P->ds->d.w, P->ds->d.h));
    uint32_t cull = d->rs ? d->rs->d.cull : 3;
    P->cull_back = cull == 3; P->cull_front = cull == 2;
    P->front_ccw = d->rs && d->rs->d.front_ccw;
    P->depth_on = d->ds ? d->ds->d.enable != 0 : true;
    P->depth_write = d->ds ? d->ds->d.write_mask != 0 : true;
    P->depth_func = d->ds ? d->ds->d.func : 2;
    P->blend = d->bs && d->bs->d.enable[0];
    P->bd = d->bs ? &d->bs->d : NULL;
    return P->clip.w > 0 && P->clip.h > 0;
}
static void draw(device_t *d, uint32_t count, const void *idx, int idx32, int32_t base, uint32_t first, uint32_t instance) {
    pipe_t *P = kmalloc(sizeof(pipe_t));
    if (!P) return;
    if (!setup(P, d) || count < 3) { kfree(P); return; }
    /* vertices go through the vertex shader once each */
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t k = idx ? (idx32 ? ((const uint32_t *)idx)[i] : ((const uint16_t *)idx)[i]) + base : first + i;
        lo = MIN(lo, k); hi = MAX(hi, k);
    }
    uint32_t span = hi - lo + 1;
    if (span > (1u << 20)) { kfree(P); return; }
    pv_t *cache = kmalloc(sizeof(pv_t) * span);
    uint8_t *done = kzalloc(span);
    if (!cache || !done) { kfree(cache); kfree(done); kfree(P); return; }
    #define VTX(i, out) do { uint32_t k_ = idx ? (idx32 ? ((const uint32_t *)idx)[i] : ((const uint16_t *)idx)[i]) + base : first + (i); \
        uint32_t s_ = k_ - lo; if (!done[s_]) { done[s_] = run_vs(P, k_, instance, &cache[s_]) ? 1 : 2; } out = done[s_] == 1 ? &cache[s_] : NULL; } while (0)
    uint32_t ntri = d->topology == 4 ? count / 3 : count - 2;
    for (uint32_t t = 0; t < ntri; t++) {
        uint32_t i0, i1, i2;
        if (d->topology == 4) { i0 = t * 3; i1 = i0 + 1; i2 = i0 + 2; }
        else if (t & 1) { i0 = t + 1; i1 = t; i2 = t + 2; }
        else { i0 = t; i1 = t + 1; i2 = t + 2; }
        const pv_t *a, *b, *c;
        VTX(i0, a); VTX(i1, b); VTX(i2, c);
        if (a && b && c) draw_triangle(P, a, b, c);
    }
    #undef VTX
    kfree(cache); kfree(done); kfree(P);
}
static void WINAPI Draw(device_t *d, uint32_t count, uint32_t start) { draw(d, count, NULL, 0, 0, start, 0); }
static void WINAPI DrawIndexed(device_t *d, uint32_t count, uint32_t start, int32_t base) {
    buffer_t *ib = d->ib;
    if (!ib) return;
    int sz = d->ib_format == F_R32_UINT ? 4 : 2;
    uint32_t off = d->ib_offset + start * sz;
    if (off + (uint64_t)count * sz > ib->d.bytes) return;
    draw(d, count, ib->data + off, sz == 4, base, 0, 0);
}
static void WINAPI DrawInstanced(device_t *d, uint32_t count, uint32_t instances, uint32_t start, uint32_t first_inst) {
    for (uint32_t i = 0; i < instances; i++) draw(d, count, NULL, 0, 0, start, first_inst + i);
}

/* ---------------- method tables ---------------- */
#define CHILD(k) { k, "QueryInterface", QueryInterface }, { k, "AddRef", AddRef }, { k, "Release", Release }, { k, "GetDevice", GetDevice }, \
    { k, "GetPrivateData", GetPrivateData }, { k, "SetPrivateData", SetPrivateData }, { k, "SetPrivateDataInterface", SetPrivateDataInterface }
#define DXGIOBJ(k) { k, "QueryInterface", QueryInterface }, { k, "AddRef", AddRef }, { k, "Release", Release }, \
    { k, "GetPrivateData", GetPrivateData }, { k, "SetPrivateData", SetPrivateData }, { k, "SetPrivateDataInterface", SetPrivateDataInterface }, { k, "GetParent", GetParent }
static const impl_t impls[] = {
    DXGIOBJ(IF_SWAP), { IF_SWAP, "GetDevice", GetParent }, { IF_SWAP, "Present", Present }, { IF_SWAP, "GetBuffer", GetBuffer },
    { IF_SWAP, "SetFullscreenState", SetFullscreenState }, { IF_SWAP, "GetFullscreenState", GetFullscreenState }, { IF_SWAP, "GetDesc", SwapGetDesc },
    { IF_SWAP, "ResizeBuffers", ResizeBuffers }, { IF_SWAP, "ResizeTarget", ResizeTarget }, { IF_SWAP, "GetContainingOutput", GetContainingOutput },
    { IF_SWAP, "GetLastPresentCount", GetLastPresentCount },
    DXGIOBJ(IF_FACTORY), { IF_FACTORY, "EnumAdapters", EnumAdapters }, { IF_FACTORY, "MakeWindowAssociation", MakeWindowAssociation },
    { IF_FACTORY, "GetWindowAssociation", GetWindowAssociation }, { IF_FACTORY, "CreateSwapChain", CreateSwapChain },
    DXGIOBJ(IF_ADAPTER), { IF_ADAPTER, "EnumOutputs", EnumOutputs }, { IF_ADAPTER, "GetDesc", AdapterGetDesc }, { IF_ADAPTER, "CheckInterfaceSupport", CheckInterfaceSupport },
    { IF_DEVICE, "QueryInterface", QueryInterface }, { IF_DEVICE, "AddRef", AddRef }, { IF_DEVICE, "Release", Release },
    { IF_DEVICE, "VSSetConstantBuffers", VSSetConstantBuffers }, { IF_DEVICE, "PSSetShaderResources", PSSetShaderResources },
    { IF_DEVICE, "PSSetShader", PSSetShader }, { IF_DEVICE, "PSSetSamplers", PSSetSamplers }, { IF_DEVICE, "VSSetShader", VSSetShader },
    { IF_DEVICE, "DrawIndexed", DrawIndexed }, { IF_DEVICE, "Draw", Draw }, { IF_DEVICE, "PSSetConstantBuffers", PSSetConstantBuffers },
    { IF_DEVICE, "IASetInputLayout", IASetInputLayout }, { IF_DEVICE, "IASetVertexBuffers", IASetVertexBuffers }, { IF_DEVICE, "IASetIndexBuffer", IASetIndexBuffer },
    { IF_DEVICE, "DrawInstanced", DrawInstanced }, { IF_DEVICE, "GSSetShader", GSSetShader }, { IF_DEVICE, "IASetPrimitiveTopology", IASetPrimitiveTopology },
    { IF_DEVICE, "OMSetRenderTargets", OMSetRenderTargets }, { IF_DEVICE, "OMSetBlendState", OMSetBlendState },
    { IF_DEVICE, "OMSetDepthStencilState", OMSetDepthStencilState }, { IF_DEVICE, "RSSetState", RSSetState }, { IF_DEVICE, "RSSetViewports", RSSetViewports },
    { IF_DEVICE, "RSSetScissorRects", RSSetScissorRects }, { IF_DEVICE, "CopyResource", CopyResource }, { IF_DEVICE, "UpdateSubresource", UpdateSubresource },
    { IF_DEVICE, "ClearRenderTargetView", ClearRenderTargetView }, { IF_DEVICE, "ClearDepthStencilView", ClearDepthStencilView },
    { IF_DEVICE, "GenerateMips", GenerateMips }, { IF_DEVICE, "IAGetPrimitiveTopology", IAGetPrimitiveTopology },
    { IF_DEVICE, "OMGetRenderTargets", OMGetRenderTargets }, { IF_DEVICE, "RSGetViewports", RSGetViewports },
    { IF_DEVICE, "GetDeviceRemovedReason", GetDeviceRemovedReason }, { IF_DEVICE, "SetExceptionMode", SetExceptionMode },
    { IF_DEVICE, "GetExceptionMode", GetExceptionMode }, { IF_DEVICE, "ClearState", ClearState }, { IF_DEVICE, "Flush", Flush },
    { IF_DEVICE, "CreateBuffer", CreateBuffer }, { IF_DEVICE, "CreateTexture2D", CreateTexture2D },
    { IF_DEVICE, "CreateShaderResourceView", CreateShaderResourceView }, { IF_DEVICE, "CreateRenderTargetView", CreateRenderTargetView },
    { IF_DEVICE, "CreateDepthStencilView", CreateDepthStencilView }, { IF_DEVICE, "CreateInputLayout", CreateInputLayout },
    { IF_DEVICE, "CreateVertexShader", CreateVertexShader }, { IF_DEVICE, "CreateGeometryShader", CreateGeometryShader },
    { IF_DEVICE, "CreatePixelShader", CreatePixelShader }, { IF_DEVICE, "CreateBlendState", CreateBlendState },
    { IF_DEVICE, "CreateDepthStencilState", CreateDepthStencilState }, { IF_DEVICE, "CreateRasterizerState", CreateRasterizerState },
    { IF_DEVICE, "CreateSamplerState", CreateSamplerState }, { IF_DEVICE, "CheckFormatSupport", CheckFormatSupport },
    { IF_DEVICE, "CheckMultisampleQualityLevels", CheckMultisampleQualityLevels },
    CHILD(IF_BUFFER), { IF_BUFFER, "GetType", GetType }, { IF_BUFFER, "SetEvictionPriority", SetEvictionPriority }, { IF_BUFFER, "GetEvictionPriority", GetEvictionPriority },
    { IF_BUFFER, "Map", BufMap }, { IF_BUFFER, "Unmap", BufUnmap }, { IF_BUFFER, "GetDesc", BufGetDesc },
    CHILD(IF_TEX2D), { IF_TEX2D, "GetType", GetType }, { IF_TEX2D, "SetEvictionPriority", SetEvictionPriority }, { IF_TEX2D, "GetEvictionPriority", GetEvictionPriority },
    { IF_TEX2D, "Map", TexMap }, { IF_TEX2D, "Unmap", TexUnmap }, { IF_TEX2D, "GetDesc", TexGetDesc },
    CHILD(IF_RTV), { IF_RTV, "GetResource", ViewGetResource }, CHILD(IF_DSV), { IF_DSV, "GetResource", ViewGetResource },
    CHILD(IF_SRV), { IF_SRV, "GetResource", ViewGetResource },
    CHILD(IF_VS), CHILD(IF_PS), CHILD(IF_GS), CHILD(IF_LAYOUT),
    CHILD(IF_SAMPLER), { IF_SAMPLER, "GetDesc", StateGetDesc }, CHILD(IF_RAST), { IF_RAST, "GetDesc", StateGetDesc },
    CHILD(IF_DEPTH), { IF_DEPTH, "GetDesc", StateGetDesc }, CHILD(IF_BLEND), { IF_BLEND, "GetDesc", StateGetDesc },
};
static void build_vtables(void) {
    if (vt_ready) return;
    for (unsigned i = 0; i < ARRAY_LEN(methods); i++) {
        const method_t *m = &methods[i];
        void *fn = stubs[MIN(m->nargs, 10)];
        for (unsigned k = 0; k < ARRAY_LEN(impls); k++) if (impls[k].iface == m->iface && !strcmp(impls[k].name, m->name)) { fn = impls[k].fn; break; }
        if (m->index < 100) vtables[m->iface][m->index] = fn;
    }
    vt_ready = true;
}

/* ---------------- exports ---------------- */
static HRESULT new_device(device_t **out) {
    if (!accel3d) { klog("w32: Direct3D 10 device refused: 3D acceleration is turned off"); return DXGI_ERROR_UNSUPPORTED; }
    device_t *d = com_new(IF_DEVICE, sizeof(device_t), NULL);
    if (!d) return E_OUTOFMEMORY;
    d->c.dev = d;
    *out = d;
    return S_OK;
}
static HRESULT WINAPI D3D10CreateDevice(void *adapter, uint32_t type, uintptr_t sw, uint32_t flags, uint32_t sdk, device_t **out) {
    (void)adapter; (void)type; (void)sw; (void)flags; (void)sdk;
    device_t *d;
    HRESULT hr = new_device(&d);
    if (hr) { if (out) *out = NULL; return hr; }
    if (out) *out = d; else Release(&d->c);
    return S_OK;
}
static HRESULT WINAPI D3D10CreateDeviceAndSwapChain(void *adapter, uint32_t type, uintptr_t sw, uint32_t flags, uint32_t sdk,
                                                    swap_desc_t *sd, swap_t **swap, device_t **out) {
    (void)adapter; (void)type; (void)sw; (void)flags; (void)sdk;
    if (swap) *swap = NULL;
    if (out) *out = NULL;
    device_t *d;
    HRESULT hr = new_device(&d);
    if (hr) return hr;
    if (swap) { hr = make_swapchain(d, sd, swap); if (hr) { Release(&d->c); return hr; } }
    if (out) *out = d; else Release(&d->c);
    return S_OK;
}
static HRESULT WINAPI D3D10CompileShader(const char *src, uintptr_t len, const char *file, const void *defs, void *inc,
                                         const char *entry, const char *profile, uint32_t flags, void **code, void **errors) {
    (void)src; (void)len; (void)file; (void)defs; (void)inc; (void)entry; (void)profile; (void)flags;
    if (code) *code = NULL;
    if (errors) *errors = NULL;
    klog("w32: d3d10: compiling HLSL at run time is not supported; programs must ship compiled shaders");
    return E_NOTIMPL;
}
static const char *WINAPI D3D10GetVertexShaderProfile(void *dev) { (void)dev; return "vs_4_0"; }
static const char *WINAPI D3D10GetPixelShaderProfile(void *dev) { (void)dev; return "ps_4_0"; }
static const char *WINAPI D3D10GetGeometryShaderProfile(void *dev) { (void)dev; return "gs_4_0"; }
static HRESULT WINAPI CreateDXGIFactory(const void *iid, com_t **out) {
    (void)iid;
    if (!out) return E_INVALIDARG;
    *out = com_new(IF_FACTORY, sizeof(com_t), NULL);
    return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI D3DCompile(const void *src, uintptr_t len, const char *name, const void *defs, void *inc, const char *entry,
                                 const char *target, uint32_t f1, uint32_t f2, void **code, void **errors) {
    (void)src; (void)len; (void)name; (void)defs; (void)inc; (void)entry; (void)target; (void)f1; (void)f2;
    if (code) *code = NULL;
    if (errors) *errors = NULL;
    klog("w32: d3dcompiler: compiling HLSL at run time is not supported; programs must ship compiled shaders");
    return E_NOTIMPL;
}

#define E(n) { #n, (void *)n }
const w32_export_t d3d10_exports[] = {
    E(D3D10CreateDevice), E(D3D10CreateDeviceAndSwapChain), E(D3D10CompileShader),
    E(D3D10GetVertexShaderProfile), E(D3D10GetPixelShaderProfile), E(D3D10GetGeometryShaderProfile),
};
const int d3d10_count = ARRAY_LEN(d3d10_exports);
const w32_export_t dxgi_exports[] = { E(CreateDXGIFactory), { "CreateDXGIFactory1", (void *)CreateDXGIFactory } };
const int dxgi_count = ARRAY_LEN(dxgi_exports);
const w32_export_t d3dcompiler_exports[] = { E(D3DCompile) };
const int d3dcompiler_count = ARRAY_LEN(d3dcompiler_exports);
