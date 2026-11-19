/* d3d9.dll and the D3DX matrix helpers for Win32 programs: Direct3D 9's
 * fixed-function pipeline on the WRLD 3D engine (src/gui/r3d.c).
 *
 * The COM interfaces (IDirect3D9, IDirect3DDevice9, IDirect3DTexture9 and the
 * vertex and index buffers) have their full method tables, generated from
 * mingw-w64's d3d9.h by tools/gen_d3d9.py, so every slot is a function with the
 * right number of arguments; what is not implemented returns D3DERR_INVALIDCALL.
 * Implemented: devices, Clear/Present/BeginScene/EndScene, transforms, viewport,
 * material, one directional light, the common render states, flexible vertex
 * formats (XYZ or XYZRHW, normal, diffuse, one texture coordinate set), the four
 * Draw*Primitive calls, 32-bit textures with LockRect, vertex and index buffers.
 * Vertex and pixel shaders are not: SetVertexShader/SetPixelShader accept NULL. */
#include "w32.h"
#include "r3d.h"

bool w32_hwnd_client(uintptr_t hwnd, canvas_t **surf, rect_t *client);

typedef int32_t HRESULT;
#define S_OK 0
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086C)
#define D3DERR_NOTAVAILABLE ((HRESULT)0x8876086A)

/* ---------------- method tables ---------------- */
enum { IF_D3D9, IF_DEVICE, IF_TEXTURE, IF_VB, IF_IB, IF_SURFACE, IF_D3D8, IF_DEVICE8, IF_TEXTURE8, IF_VB8, IF_IB8, IF_SURFACE8, IF_COUNT };
typedef struct { int iface, index, nargs; const char *name; } method_t;
#define IDirect3D9 IF_D3D9
#define IDirect3DDevice9 IF_DEVICE
#define IDirect3DTexture9 IF_TEXTURE
#define IDirect3DVertexBuffer9 IF_VB
#define IDirect3DIndexBuffer9 IF_IB
#define IDirect3DSurface9 IF_SURFACE
static const method_t methods[] = {
#define M(i, idx, name, n) { i, idx, n, #name },
#include "d3d9_methods.h"
#undef M
#define IDirect3D8 IF_D3D8
#define IDirect3DDevice8 IF_DEVICE8
#define IDirect3DTexture8 IF_TEXTURE8
#define IDirect3DVertexBuffer8 IF_VB8
#define IDirect3DIndexBuffer8 IF_IB8
#define IDirect3DSurface8 IF_SURFACE8
#define M(i, idx, name, n) { i, idx, n, #name },
#include "d3d8_methods.h"
#undef M
#undef IDirect3D8
#undef IDirect3DDevice8
#undef IDirect3DTexture8
#undef IDirect3DVertexBuffer8
#undef IDirect3DIndexBuffer8
#undef IDirect3DSurface8
};
#undef IDirect3D9
#undef IDirect3DDevice9
#undef IDirect3DTexture9
#undef IDirect3DVertexBuffer9
#undef IDirect3DIndexBuffer9
#undef IDirect3DSurface9

/* stubs for methods that are not implemented, one per argument count (stdcall pops them) */
typedef uintptr_t A;
#define STUB(n, ...) static HRESULT WINAPI stub##n(void *self, ##__VA_ARGS__) { (void)self; return D3DERR_INVALIDCALL; }
STUB(0) STUB(1, A a) STUB(2, A a, A b) STUB(3, A a, A b, A c) STUB(4, A a, A b, A c, A d) STUB(5, A a, A b, A c, A d, A e)
STUB(6, A a, A b, A c, A d, A e, A f) STUB(7, A a, A b, A c, A d, A e, A f, A g) STUB(8, A a, A b, A c, A d, A e, A f, A g, A h)
STUB(9, A a, A b, A c, A d, A e, A f, A g, A h, A i) STUB(10, A a, A b, A c, A d, A e, A f, A g, A h, A i, A j)
STUB(11, A a, A b, A c, A d, A e, A f, A g, A h, A i, A j, A k) STUB(12, A a, A b, A c, A d, A e, A f, A g, A h, A i, A j, A k, A l)
static void *const stubs[] = { stub0, stub1, stub2, stub3, stub4, stub5, stub6, stub7, stub8, stub9, stub10, stub11, stub12 };

typedef struct { int iface; const char *name; void *fn; } impl_t;
static void *vtables[IF_COUNT][128];
static bool vt_ready;
static void build_vtables(void);

/* ---------------- objects ---------------- */
typedef struct { void **vt; int refs; int kind; } com_t;
typedef struct { float r, g, b, a; } color4_t;
typedef struct { uint32_t w, h, refresh, format; } dmode_t;
typedef struct {
    uint32_t bb_w, bb_h, bb_format, bb_count, ms_type, ms_quality, swap_effect;
    uintptr_t hwnd; int32_t windowed, auto_depth; uint32_t depth_format, flags, refresh, interval;
} pparams_t;
typedef struct { uint32_t x, y, w, h; float minz, maxz; } vport_t;
typedef struct { color4_t diffuse, ambient, specular, emissive; float power; } material_t;
typedef struct { uint32_t type; color4_t diffuse, specular, ambient; float pos[3], dir[3]; float range, falloff, a0, a1, a2, theta, phi; } light_t;

typedef struct texture { com_t c; canvas_t *img; uint32_t format, levels; uint8_t *scratch; uint8_t *native; uint32_t npitch; } texture_t;
typedef struct { com_t c; uint8_t *data; uint32_t len, fvf, format; } buffer_t;
typedef struct device {
    com_t c;
    com_t *d3d;
    uintptr_t hwnd;
    canvas_t *back; float *depth; int bw, bh;
    mat4 world, view, proj;
    uint32_t rs[256];
    vport_t vp;
    material_t mat;
    light_t light; bool light_on;
    uint32_t fvf;
    texture_t *tex;
    bool linear;
    buffer_t *vb; uint32_t vb_off, vb_stride;
    buffer_t *ib;
    pparams_t pp;
    int32_t base8;                              /* Direct3D 8: the base vertex index given to SetIndices */
} device_t;

static void *com_new(int iface, int kind, size_t size) {
    build_vtables();
    com_t *o = w32_alloc(w32_cur(), (uint32_t)size);
    if (!o) return NULL;
    memset(o, 0, size);
    o->vt = vtables[iface]; o->refs = 1; o->kind = kind;
    return o;
}
static HRESULT WINAPI QueryInterface(com_t *self, const void *iid, void **out) { (void)iid; if (out) *out = NULL; (void)self; return E_NOINTERFACE; }
static uint32_t WINAPI AddRef(com_t *self) { return ++self->refs; }
static void dev_free(device_t *d) { if (d->back) canvas_free(d->back); kfree(d->depth); }
static uint32_t WINAPI Release(com_t *self) {
    if (--self->refs > 0) return self->refs;
    if (self->kind == IF_DEVICE) dev_free((device_t *)self);
    else if (self->kind == IF_TEXTURE) { texture_t *t = (texture_t *)self; if (t->img) canvas_free(t->img); kfree(t->scratch); kfree(t->native); }
    else if (self->kind == IF_VB || self->kind == IF_IB) kfree(((buffer_t *)self)->data);
    self->vt = NULL;
    w32_free(w32_cur(), self);
    return 0;
}

/* ---------------- IDirect3D9 ---------------- */
static uint32_t WINAPI GetAdapterCount(com_t *s) { (void)s; return 1; }
static HRESULT WINAPI GetAdapterIdentifier(com_t *s, uint32_t a, uint32_t f, char *id) {
    (void)s; (void)a; (void)f;
    if (!id) return D3DERR_INVALIDCALL;
    memset(id, 0, 1100);
    strcpy(id, "wrld3d.dll"); strcpy(id + 512, "WRLD 3D (software rasterizer)"); strcpy(id + 1024, "\\\\.\\DISPLAY1");
    return S_OK;
}
static uint32_t WINAPI GetAdapterModeCount(com_t *s, uint32_t a, uint32_t fmt) { (void)s; (void)a; (void)fmt; return 1; }
static HRESULT WINAPI GetAdapterDisplayMode(com_t *s, uint32_t a, dmode_t *m) { (void)s; (void)a; if (!m) return D3DERR_INVALIDCALL; m->w = SCREEN_W; m->h = SCREEN_H; m->refresh = 60; m->format = 22; return S_OK; }
static HRESULT WINAPI EnumAdapterModes(com_t *s, uint32_t a, uint32_t fmt, uint32_t mode, dmode_t *m) { (void)fmt; if (mode) return D3DERR_INVALIDCALL; return GetAdapterDisplayMode(s, a, m); }
static HRESULT WINAPI CheckDeviceType(com_t *s, uint32_t a, uint32_t t, uint32_t f1, uint32_t f2, int32_t w) { (void)s; (void)a; (void)t; (void)f1; (void)f2; (void)w; return S_OK; }
static HRESULT WINAPI CheckDeviceFormat(com_t *s, uint32_t a, uint32_t t, uint32_t af, uint32_t usage, uint32_t rt, uint32_t fmt) {
    (void)s; (void)a; (void)t; (void)af; (void)usage; (void)rt;
    extern bool d3d_tex_format(uint32_t f);
    return d3d_tex_format(fmt) || fmt == 75 || fmt == 77 || fmt == 80 || fmt == 71 || fmt == 73 ? S_OK : D3DERR_NOTAVAILABLE;   /* colour formats, D24S8, D24X8, D16, D32, D15S1 */
}
static HRESULT WINAPI CheckDeviceMultiSampleType(com_t *s, uint32_t a, uint32_t t, uint32_t f, int32_t w, uint32_t ms, uint32_t *q) {
    (void)s; (void)a; (void)t; (void)f; (void)w; if (q) *q = 0; return ms == 0 ? S_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT WINAPI CheckDepthStencilMatch(com_t *s, uint32_t a, uint32_t t, uint32_t f1, uint32_t f2, uint32_t f3) { (void)s; (void)a; (void)t; (void)f1; (void)f2; (void)f3; return S_OK; }
/* D3DCAPS9: a fixed-function, single-texture, single-light device */
static void fill_caps(uint32_t *c) {
    memset(c, 0, 304);
    c[0] = 1;                                   /* DeviceType: HAL */
    c[8] = 0x10 | 0x20 | 0x40;                  /* PrimitiveMiscCaps: CULLNONE, CULLCW, CULLCCW */
    c[16] = 0x100 | 0x200 | 0x1000000 | 0x2000000;   /* TextureFilterCaps: point and linear min/mag */
    c[22] = 2048; c[23] = 2048;                 /* MaxTextureWidth, MaxTextureHeight */
    c[39] = 1;                                  /* MaxTextureBlendStages */
    c[40] = 1;                                  /* MaxSimultaneousTextures */
    c[43] = 1;                                  /* MaxActiveLights */
    c[49] = 0xFFFFF; c[50] = 0xFFFFFF;          /* MaxPrimitiveCount, MaxVertexIndex */
    c[51] = 1; c[52] = 256;                     /* MaxStreams, MaxStreamStride */
    c[53] = 0xFFFE0000;                         /* VertexShaderVersion: none */
    c[55] = 0xFFFF0000;                         /* PixelShaderVersion: none */
}
static HRESULT WINAPI GetDeviceCaps9(com_t *s, uint32_t a, uint32_t t, uint32_t *caps) { (void)s; (void)a; (void)t; if (!caps) return D3DERR_INVALIDCALL; fill_caps(caps); return S_OK; }
static uintptr_t WINAPI GetAdapterMonitor(com_t *s, uint32_t a) { (void)s; (void)a; return 1; }

static bool dev_buffers(device_t *d) {
    canvas_t *surf; rect_t cr;
    int w = d->pp.bb_w, h = d->pp.bb_h;
    if ((!w || !h) && w32_hwnd_client(d->hwnd, &surf, &cr)) { w = cr.w; h = cr.h; }
    w = MAX(1, w); h = MAX(1, h);
    if (d->back && d->bw == w && d->bh == h) return true;
    if (d->back) canvas_free(d->back);
    kfree(d->depth);
    d->back = canvas_new(w, h); d->depth = kzalloc(sizeof(float) * w * h);
    d->bw = w; d->bh = h;
    d->vp = (vport_t){ 0, 0, (uint32_t)w, (uint32_t)h, 0, 1 };
    return d->back && d->depth;
}
static HRESULT WINAPI CreateDevice(com_t *s, uint32_t a, uint32_t type, uintptr_t focus, uint32_t behaviour, pparams_t *pp, device_t **out) {
    (void)a; (void)type; (void)behaviour;
    if (!out || !pp) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!accel3d) { klog("w32: Direct3D device refused: 3D acceleration is turned off"); return D3DERR_NOTAVAILABLE; }
    device_t *d = com_new(IF_DEVICE, IF_DEVICE, sizeof(device_t));
    if (!d) return D3DERR_INVALIDCALL;
    d->d3d = s; s->refs++;
    d->pp = *pp; d->hwnd = pp->hwnd ? pp->hwnd : focus;
    d->world = d->view = d->proj = m4_identity();
    d->rs[7] = pp->auto_depth ? 1 : 0;          /* ZENABLE */
    d->rs[14] = 1;                              /* ZWRITEENABLE */
    d->rs[22] = 3;                              /* CULLMODE: CCW */
    d->rs[137] = 1;                             /* LIGHTING */
    d->mat.diffuse = (color4_t){ 1, 1, 1, 1 };
    d->linear = false;
    if (!dev_buffers(d)) { Release(&d->c); return D3DERR_INVALIDCALL; }
    if (!pp->bb_w) pp->bb_w = d->bw;
    if (!pp->bb_h) pp->bb_h = d->bh;
    *out = d;
    return S_OK;
}

/* ---------------- IDirect3DDevice9 ---------------- */
static HRESULT WINAPI TestCooperativeLevel(device_t *d) { (void)d; return S_OK; }
static uint32_t WINAPI GetAvailableTextureMem(device_t *d) { (void)d; return 256u << 20; }
static HRESULT WINAPI GetDirect3D(device_t *d, com_t **out) { if (!out) return D3DERR_INVALIDCALL; *out = d->d3d; d->d3d->refs++; return S_OK; }
static HRESULT WINAPI DevGetDeviceCaps(device_t *d, uint32_t *caps) { (void)d; if (!caps) return D3DERR_INVALIDCALL; fill_caps(caps); return S_OK; }
static HRESULT WINAPI DevGetDisplayMode(device_t *d, uint32_t sw, dmode_t *m) { (void)d; (void)sw; return GetAdapterDisplayMode(NULL, 0, m); }
static HRESULT WINAPI Reset(device_t *d, pparams_t *pp) { if (pp) d->pp = *pp; d->bw = 0; return dev_buffers(d) ? S_OK : D3DERR_INVALIDCALL; }
static HRESULT WINAPI Present(device_t *d, const void *src, const void *dst, uintptr_t hwnd, const void *rgn) {
    (void)src; (void)dst; (void)rgn;
    canvas_t *surf; rect_t cr;
    if (!d->back || !w32_hwnd_client(hwnd ? hwnd : d->hwnd, &surf, &cr)) return D3DERR_INVALIDCALL;
    if (cr.w == d->bw && cr.h == d->bh) blit(surf, cr.x, cr.y, d->back, 0, 0, d->bw, d->bh);
    else {                                                    /* back buffer of another size: stretched, as Direct3D does */
        extern void blit_scaled_alpha(canvas_t *dst, rect_t r, canvas_t *src, int alpha);
        rect_t old = surf->clip; surf->clip = rect_intersect(cr, R(0, 0, surf->w, surf->h));
        blit_scaled_alpha(surf, cr, d->back, 255);
        surf->clip = old;
    }
    wm_need_redraw = true;
    task_yield();
    return S_OK;
}
static HRESULT WINAPI BeginScene(device_t *d) { return dev_buffers(d) ? S_OK : D3DERR_INVALIDCALL; }
static HRESULT WINAPI EndScene(device_t *d) { (void)d; return S_OK; }
static HRESULT WINAPI Clear(device_t *d, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    (void)count; (void)rects; (void)z; (void)stencil;
    if (!dev_buffers(d)) return D3DERR_INVALIDCALL;
    rect_t vr = rect_intersect(R(d->vp.x, d->vp.y, d->vp.w, d->vp.h), R(0, 0, d->bw, d->bh));
    if (flags & 1) fill_rect(d->back, vr.x, vr.y, vr.w, vr.h, color | 0xFF000000u);
    if (flags & 2) for (int y = vr.y; y < vr.y + vr.h; y++) memset(d->depth + (size_t)y * d->bw + vr.x, 0, sizeof(float) * vr.w);
    return S_OK;
}
static mat4 *transform_of(device_t *d, uint32_t which) { return which == 2 ? &d->view : which == 3 ? &d->proj : which == 256 ? &d->world : NULL; }
static HRESULT WINAPI SetTransform(device_t *d, uint32_t which, const float *m) { mat4 *t = transform_of(d, which); if (t && m) memcpy(t->m, m, 64); return S_OK; }
static HRESULT WINAPI GetTransform(device_t *d, uint32_t which, float *m) { mat4 *t = transform_of(d, which); if (!t || !m) return D3DERR_INVALIDCALL; memcpy(m, t->m, 64); return S_OK; }
static HRESULT WINAPI MultiplyTransform(device_t *d, uint32_t which, const float *m) {
    mat4 *t = transform_of(d, which); if (!t || !m) return D3DERR_INVALIDCALL;
    mat4 x; memcpy(x.m, m, 64);
    *t = m4_mul(*t, x);                        /* Direct3D: M' = m * M with row vectors, which is M * m here */
    return S_OK;
}
static HRESULT WINAPI SetViewport(device_t *d, const vport_t *v) { if (!v) return D3DERR_INVALIDCALL; d->vp = *v; return S_OK; }
static HRESULT WINAPI GetViewport(device_t *d, vport_t *v) { if (!v) return D3DERR_INVALIDCALL; *v = d->vp; return S_OK; }
static HRESULT WINAPI SetMaterial(device_t *d, const material_t *m) { if (!m) return D3DERR_INVALIDCALL; d->mat = *m; return S_OK; }
static HRESULT WINAPI GetMaterial(device_t *d, material_t *m) { if (!m) return D3DERR_INVALIDCALL; *m = d->mat; return S_OK; }
static HRESULT WINAPI SetLight(device_t *d, uint32_t i, const light_t *l) { if (!l) return D3DERR_INVALIDCALL; if (i == 0) d->light = *l; return S_OK; }
static HRESULT WINAPI GetLight(device_t *d, uint32_t i, light_t *l) { if (!l || i) return D3DERR_INVALIDCALL; *l = d->light; return S_OK; }
static HRESULT WINAPI LightEnable(device_t *d, uint32_t i, int32_t on) { if (i == 0) d->light_on = on != 0; return S_OK; }
static HRESULT WINAPI GetLightEnable(device_t *d, uint32_t i, int32_t *on) { if (!on) return D3DERR_INVALIDCALL; *on = i == 0 && d->light_on; return S_OK; }
static HRESULT WINAPI SetRenderState(device_t *d, uint32_t s, uint32_t v) { if (s < 256) d->rs[s] = v; return S_OK; }
static HRESULT WINAPI GetRenderState(device_t *d, uint32_t s, uint32_t *v) { if (!v || s >= 256) return D3DERR_INVALIDCALL; *v = d->rs[s]; return S_OK; }
static HRESULT WINAPI SetTexture(device_t *d, uint32_t stage, texture_t *t) {
    if (stage) return S_OK;                                   /* one texture stage */
    if (t && t->c.kind != IF_TEXTURE) return D3DERR_INVALIDCALL;
    if (t) t->c.refs++;
    if (d->tex) Release(&d->tex->c);
    d->tex = t;
    return S_OK;
}
static HRESULT WINAPI GetTexture(device_t *d, uint32_t stage, texture_t **t) { if (!t) return D3DERR_INVALIDCALL; *t = stage ? NULL : d->tex; if (*t) (*t)->c.refs++; return S_OK; }
static HRESULT WINAPI SetTextureStageState(device_t *d, uint32_t st, uint32_t ty, uint32_t v) { (void)d; (void)st; (void)ty; (void)v; return S_OK; }
static HRESULT WINAPI SetSamplerState(device_t *d, uint32_t smp, uint32_t ty, uint32_t v) { if (!smp && (ty == 5 || ty == 6)) d->linear = v >= 2; return S_OK; }
static HRESULT WINAPI SetFVF(device_t *d, uint32_t fvf) { d->fvf = fvf; return S_OK; }
static HRESULT WINAPI GetFVF(device_t *d, uint32_t *fvf) { if (!fvf) return D3DERR_INVALIDCALL; *fvf = d->fvf; return S_OK; }
static HRESULT WINAPI SetVertexShader(device_t *d, void *sh) { (void)d; return sh ? D3DERR_INVALIDCALL : S_OK; }
static HRESULT WINAPI SetPixelShader(device_t *d, void *sh) { (void)d; return sh ? D3DERR_INVALIDCALL : S_OK; }
static HRESULT WINAPI SetStreamSource(device_t *d, uint32_t n, buffer_t *vb, uint32_t off, uint32_t stride) {
    if (n) return S_OK;
    if (vb) vb->c.refs++;
    if (d->vb) Release(&d->vb->c);
    d->vb = vb; d->vb_off = off; d->vb_stride = stride;
    return S_OK;
}
static HRESULT WINAPI SetIndices(device_t *d, buffer_t *ib) { if (ib) ib->c.refs++; if (d->ib) Release(&d->ib->c); d->ib = ib; return S_OK; }

/* vertices in the device's flexible vertex format */
static int fvf_stride(uint32_t fvf) {
    int s = (fvf & 0xE) == 0x4 ? 16 : (fvf & 0xE) == 0x2 ? 12 : 12;
    if (fvf & 0x10) s += 12;
    if (fvf & 0x20) s += 4;
    if (fvf & 0x40) s += 4;
    if (fvf & 0x80) s += 4;
    s += ((fvf >> 8) & 0xF) * 8;
    return s;
}
static r3d_vertex_t decode(device_t *d, const uint8_t *p, bool *pretransformed) {
    uint32_t fvf = d->fvf;
    r3d_vertex_t v; memset(&v, 0, sizeof v);
    const float *f = (const float *)p;
    v.x = f[0]; v.y = f[1]; v.z = f[2];
    int o = 12;
    if ((fvf & 0xE) == 0x4) {                                 /* XYZRHW: screen pixels */
        float rhw = f[3] != 0 ? f[3] : 1, w = 1 / rhw;
        float cx = (v.x - d->vp.x) / MAX(1u, d->vp.w) * 2 - 1, cy = 1 - (v.y - d->vp.y) / MAX(1u, d->vp.h) * 2;
        v.x = cx * w; v.y = cy * w; v.z = v.z * w; v.w = w;
        *pretransformed = true;
        o = 16;
    }
    if (fvf & 0x10) { const float *n = (const float *)(p + o); v.nx = n[0]; v.ny = n[1]; v.nz = n[2]; o += 12; }
    if (fvf & 0x20) o += 4;
    bool lit = d->rs[137] && d->light_on && !*pretransformed;
    if (fvf & 0x40) { v.color = *(const uint32_t *)(p + o); o += 4; }
    else if (lit) {
        const color4_t *c = &d->mat.diffuse;
        v.color = ((uint32_t)CLAMP((int)(c->a * 255), 0, 255) << 24) | (CLAMP((int)(c->r * 255), 0, 255) << 16) | (CLAMP((int)(c->g * 255), 0, 255) << 8) | CLAMP((int)(c->b * 255), 0, 255);
    } else v.color = 0xFFFFFFFFu;
    if (fvf & 0x80) o += 4;
    if ((fvf >> 8) & 0xF) { const float *t = (const float *)(p + o); v.u = t[0]; v.v = t[1]; }
    return v;
}
static void draw(device_t *d, uint32_t prim, const uint8_t *vdata, uint32_t stride, const void *idx, int idx32, int base, uint32_t first, uint32_t nprim) {
    if (!dev_buffers(d) || !nprim) return;
    uint32_t nv = prim == 4 ? nprim * 3 : prim == 5 || prim == 6 ? nprim + 2 : prim == 2 ? nprim * 2 : prim == 3 ? nprim + 1 : nprim;
    if (nv > 1u << 20) return;
    r3d_vertex_t *v = kmalloc(sizeof(r3d_vertex_t) * nv);
    if (!v) return;
    bool pre = false;
    for (uint32_t i = 0; i < nv; i++) {
        long k = idx ? (idx32 ? (long)((const uint32_t *)idx)[first + i] : (long)((const uint16_t *)idx)[first + i]) + base : (long)(first + i);
        v[i] = decode(d, vdata + (size_t)k * stride, &pre);
    }
    rect_t vr = rect_intersect(R(d->vp.x, d->vp.y, d->vp.w, d->vp.h), R(0, 0, d->bw, d->bh));
    r3d_t r;
    if (vr.w > 0 && vr.h > 0 && prim >= 4 && r3d_begin_with(&r, d->back, vr, d->depth + 0, false)) {
        if (pre) { r.proj = r.view_m = r.model = m4_identity(); }
        else { r.proj = d->proj; r.view_m = d->view; r.model = d->world; }
        r.lighting = d->rs[137] && d->light_on && !pre && (d->fvf & 0x10);
        float lx = -d->light.dir[0], ly = -d->light.dir[1], lz = -d->light.dir[2], l = r3d_sqrt(lx * lx + ly * ly + lz * lz);
        if (l > 0) { r.light[0] = lx / l; r.light[1] = ly / l; r.light[2] = lz / l; }
        uint32_t amb = d->rs[139];
        r.ambient = (C_R(amb) + C_G(amb) + C_B(amb)) / 765.0f + (d->light.ambient.r + d->light.ambient.g + d->light.ambient.b) / 3;
        r.diffuse = (d->light.diffuse.r + d->light.diffuse.g + d->light.diffuse.b) / 3;
        r.texture = d->tex ? d->tex->img : NULL;
        r.bilinear = d->linear;
        r.cull_back = d->rs[22] != 1; r.front_cw = d->rs[22] != 2;    /* CULL_CCW (default) keeps clockwise faces */
        r.depth_test = d->rs[7] != 0; r.depth_write = r.depth_test && d->rs[14];
        if (!d->rs[27]) for (uint32_t i = 0; i < nv; i++) v[i].color |= 0xFF000000u;   /* ALPHABLENDENABLE off */
        r3d_vertex_t *t = kmalloc(sizeof(r3d_vertex_t) * (prim == 4 ? nv : nprim * 3));
        if (t) {
            int k = 0;
            if (prim == 4) { memcpy(t, v, sizeof(r3d_vertex_t) * nv); k = nv; }
            else for (uint32_t i = 0; i < nprim; i++) {
                if (prim == 5) { t[k++] = v[i]; t[k++] = v[i + 1 + (i & 1)]; t[k++] = v[i + 2 - (i & 1)]; }
                else { t[k++] = v[0]; t[k++] = v[i + 1]; t[k++] = v[i + 2]; }
            }
            r3d_triangles(&r, t, k);
            kfree(t);
        }
    }
    kfree(v);
}
static HRESULT WINAPI DrawPrimitiveUP(device_t *d, uint32_t prim, uint32_t n, const void *data, uint32_t stride) {
    if (!data) return D3DERR_INVALIDCALL;
    draw(d, prim, data, stride ? stride : (uint32_t)fvf_stride(d->fvf), NULL, 0, 0, 0, n);
    return S_OK;
}
static HRESULT WINAPI DrawIndexedPrimitiveUP(device_t *d, uint32_t prim, uint32_t minv, uint32_t nv, uint32_t n, const void *idx, uint32_t ifmt, const void *data, uint32_t stride) {
    (void)minv; (void)nv;
    if (!data || !idx) return D3DERR_INVALIDCALL;
    draw(d, prim, data, stride, idx, ifmt == 102, 0, 0, n);
    return S_OK;
}
static HRESULT WINAPI DrawPrimitive(device_t *d, uint32_t prim, uint32_t start, uint32_t n) {
    if (!d->vb) return D3DERR_INVALIDCALL;
    uint32_t stride = d->vb_stride ? d->vb_stride : (uint32_t)fvf_stride(d->fvf);
    draw(d, prim, d->vb->data + d->vb_off, stride, NULL, 0, 0, start, n);
    return S_OK;
}
static HRESULT WINAPI DrawIndexedPrimitive(device_t *d, uint32_t prim, int32_t base, uint32_t minv, uint32_t nv, uint32_t start, uint32_t n) {
    (void)minv; (void)nv;
    if (!d->vb || !d->ib) return D3DERR_INVALIDCALL;
    uint32_t stride = d->vb_stride ? d->vb_stride : (uint32_t)fvf_stride(d->fvf);
    draw(d, prim, d->vb->data + d->vb_off, stride, d->ib->data, d->ib->format == 102, base, start, n);
    return S_OK;
}

/* resources */
static HRESULT WINAPI CreateTexture(device_t *d, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, texture_t **out, void *share) {
    (void)d; (void)usage; (void)pool; (void)share;
    if (!out || !w || !h || w > 2048 || h > 2048) return D3DERR_INVALIDCALL;
    extern bool d3d_tex_format(uint32_t f);
    if (!d3d_tex_format(fmt)) return D3DERR_NOTAVAILABLE;
    texture_t *t = com_new(IF_TEXTURE, IF_TEXTURE, sizeof(texture_t));
    if (!t) return D3DERR_INVALIDCALL;
    t->img = canvas_new(w, h); t->format = fmt; t->levels = levels ? levels : 1;
    if (!t->img) { Release(&t->c); return D3DERR_INVALIDCALL; }
    gfx_clear(t->img, 0xFF000000u);
    *out = t;
    return S_OK;
}
typedef struct { int32_t pitch; void *bits; } locked_t;
/* texture formats: 32-bit ones are the canvas itself; the others (16-bit, 24-bit, alpha and
 * luminance, DXT1-5) are kept in their own layout and converted to the canvas on unlock */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
static bool is_dxt(uint32_t f) { return f == FOURCC('D','X','T','1') || f == FOURCC('D','X','T','2') || f == FOURCC('D','X','T','3') || f == FOURCC('D','X','T','4') || f == FOURCC('D','X','T','5'); }
static int fmt_bpp(uint32_t f) {
    switch (f) { case 21: case 22: return 32; case 20: return 24; case 23: case 24: case 25: case 26: case 30: case 51: return 16; case 28: case 29: case 50: case 52: return 8; }
    return 0;
}
bool d3d_tex_format(uint32_t f) { return fmt_bpp(f) || is_dxt(f); }
static uint32_t pitch_of(uint32_t f, uint32_t w) { return is_dxt(f) ? ((w + 3) / 4) * (f == FOURCC('D','X','T','1') ? 8 : 16) : w * (fmt_bpp(f) / 8); }
static uint32_t rows_of(uint32_t f, uint32_t h) { return is_dxt(f) ? (h + 3) / 4 : h; }
static uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t c565(uint16_t c) { return x5(c >> 11) << 16 | x6((c >> 5) & 63) << 8 | x5(c & 31); }
static uint32_t px_of(uint32_t f, const uint8_t *p) {
    uint16_t v = p[0] | p[1] << 8;
    switch (f) {
    case 20: return 0xFF000000u | p[2] << 16 | p[1] << 8 | p[0];
    case 23: return 0xFF000000u | c565(v);
    case 24: return 0xFF000000u | x5((v >> 10) & 31) << 16 | x5((v >> 5) & 31) << 8 | x5(v & 31);
    case 25: return ((v & 0x8000) ? 0xFF000000u : 0) | x5((v >> 10) & 31) << 16 | x5((v >> 5) & 31) << 8 | x5(v & 31);
    case 26: return ((v >> 12) & 15) * 0x11u << 24 | ((v >> 8) & 15) * 0x11u << 16 | ((v >> 4) & 15) * 0x11u << 8 | (v & 15) * 0x11u;
    case 28: return (uint32_t)p[0] << 24 | 0xFFFFFF;
    case 29: return 0xFF000000u | ((p[0] >> 5) * 255 / 7) << 16 | (((p[0] >> 2) & 7) * 255 / 7) << 8 | (p[0] & 3) * 85;
    case 30: return (uint32_t)p[1] << 24 | ((p[0] >> 5) * 255 / 7) << 16 | (((p[0] >> 2) & 7) * 255 / 7) << 8 | (p[0] & 3) * 85;
    case 50: return 0xFF000000u | p[0] * 0x010101u;
    case 51: return (uint32_t)p[1] << 24 | p[0] * 0x010101u;
    case 52: return ((p[0] >> 4) * 0x11u) << 24 | (p[0] & 15) * 0x111111u;
    }
    return 0xFF000000u;
}
static void dxt_block(uint32_t f, const uint8_t *b, uint32_t out[16]) {
    bool dxt1 = f == FOURCC('D','X','T','1');
    const uint8_t *cb = dxt1 ? b : b + 8;
    uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
    uint32_t p0 = c565(c0), p1 = c565(c1), pal[4] = { p0, p1, 0, 0 };
    #define MIXC(a, b, wa, wb, d) ((((((a) >> 16) & 255) * (wa) + (((b) >> 16) & 255) * (wb)) / (d)) << 16 | (((((a) >> 8) & 255) * (wa) + (((b) >> 8) & 255) * (wb)) / (d)) << 8 | ((((a) & 255) * (wa) + ((b) & 255) * (wb)) / (d)))
    bool four = !dxt1 || c0 > c1;
    if (four) { pal[2] = MIXC(p0, p1, 2, 1, 3); pal[3] = MIXC(p0, p1, 1, 2, 3); } else { pal[2] = MIXC(p0, p1, 1, 1, 2); pal[3] = 0; }
    #undef MIXC
    uint32_t bits = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
    for (int i = 0; i < 16; i++) { int k = (bits >> (2 * i)) & 3; out[i] = pal[k] | ((dxt1 && !four && k == 3) ? 0 : 0xFF000000u); }
    if (f == FOURCC('D','X','T','2') || f == FOURCC('D','X','T','3')) {           /* explicit 4-bit alpha */
        for (int i = 0; i < 16; i++) { uint32_t a = (b[i / 2] >> ((i & 1) * 4)) & 15; out[i] = (out[i] & 0xFFFFFF) | (a * 0x11u) << 24; }
    } else if (!dxt1) {                                                            /* DXT4/5: interpolated alpha */
        uint32_t a[8]; a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
        uint64_t ab = 0; for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) out[i] = (out[i] & 0xFFFFFF) | a[(ab >> (3 * i)) & 7] << 24;
    }
}
static void tex_convert(texture_t *t) {
    canvas_t *c = t->img; uint32_t f = t->format;
    if (is_dxt(f)) {
        for (int by = 0; by < (c->h + 3) / 4; by++) for (int bx = 0; bx < (c->w + 3) / 4; bx++) {
            uint32_t px[16]; dxt_block(f, t->native + by * t->npitch + bx * (f == FOURCC('D','X','T','1') ? 8 : 16), px);
            for (int i = 0; i < 16; i++) { int x = bx * 4 + (i & 3), y = by * 4 + i / 4; if (x < c->w && y < c->h) c->px[y * c->stride + x] = px[i]; }
        }
        return;
    }
    int bpp = fmt_bpp(f) / 8;
    for (int y = 0; y < c->h; y++) { const uint8_t *r = t->native + y * t->npitch; for (int x = 0; x < c->w; x++) c->px[y * c->stride + x] = px_of(f, r + x * bpp); }
}
static HRESULT WINAPI TexLockRect(texture_t *t, uint32_t level, locked_t *lr, const int32_t *rect, uint32_t flags) {
    (void)flags;
    if (!lr) return D3DERR_INVALIDCALL;
    if (level) {                                              /* smaller mip levels are accepted and not used */
        uint32_t w = MAX(1, t->img->w >> level), h = MAX(1, t->img->h >> level);
        kfree(t->scratch); t->scratch = kzalloc(pitch_of(t->format, w) * rows_of(t->format, h) + 64);
        lr->pitch = pitch_of(t->format, w); lr->bits = t->scratch;
        return t->scratch ? S_OK : D3DERR_INVALIDCALL;
    }
    if (t->format == 21 || t->format == 22) {
        lr->pitch = t->img->stride * 4;
        uint32_t *p = t->img->px;
        if (rect) p += rect[1] * t->img->stride + rect[0];
        lr->bits = p;
        return S_OK;
    }
    if (!t->native) { t->npitch = pitch_of(t->format, t->img->w); t->native = kzalloc(t->npitch * rows_of(t->format, t->img->h) + 64); if (!t->native) return D3DERR_INVALIDCALL; }
    lr->pitch = t->npitch;
    uint8_t *p = t->native;
    if (rect) p += is_dxt(t->format) ? (rect[1] / 4) * t->npitch + (rect[0] / 4) * (t->format == FOURCC('D','X','T','1') ? 8 : 16) : rect[1] * t->npitch + rect[0] * (fmt_bpp(t->format) / 8);
    lr->bits = p;
    return S_OK;
}
static HRESULT WINAPI TexUnlockRect(texture_t *t, uint32_t level) {
    if (level) return S_OK;
    if (t->format == 22) for (int i = 0; i < t->img->h * t->img->stride; i++) t->img->px[i] |= 0xFF000000u;   /* X8: opaque */
    else if (t->format != 21 && t->native) tex_convert(t);
    return S_OK;
}
static uint32_t WINAPI TexGetLevelCount(texture_t *t) { return t->levels; }
static HRESULT WINAPI TexGetLevelDesc(texture_t *t, uint32_t level, uint32_t *desc) {
    if (!desc) return D3DERR_INVALIDCALL;
    memset(desc, 0, 32);
    desc[0] = t->format; desc[1] = 1; desc[6] = MAX(1, t->img->w >> level); desc[7] = MAX(1, t->img->h >> level);
    return S_OK;
}
static HRESULT make_buffer(int iface, uint32_t len, uint32_t fvf_or_fmt, buffer_t **out) {
    if (!out || !len || len > (64u << 20)) return D3DERR_INVALIDCALL;
    buffer_t *b = com_new(iface, iface, sizeof(buffer_t));
    if (!b) return D3DERR_INVALIDCALL;
    b->data = kzalloc(len); b->len = len;
    if (iface == IF_VB) b->fvf = fvf_or_fmt; else b->format = fvf_or_fmt;
    if (!b->data) { Release(&b->c); return D3DERR_INVALIDCALL; }
    *out = b;
    return S_OK;
}
static HRESULT WINAPI CreateVertexBuffer(device_t *d, uint32_t len, uint32_t usage, uint32_t fvf, uint32_t pool, buffer_t **out, void *share) { (void)d; (void)usage; (void)pool; (void)share; return make_buffer(IF_VB, len, fvf, out); }
static HRESULT WINAPI CreateIndexBuffer(device_t *d, uint32_t len, uint32_t usage, uint32_t fmt, uint32_t pool, buffer_t **out, void *share) { (void)d; (void)usage; (void)pool; (void)share; return make_buffer(IF_IB, len, fmt, out); }
static HRESULT WINAPI BufLock(buffer_t *b, uint32_t off, uint32_t size, void **data, uint32_t flags) {
    (void)size; (void)flags;
    if (!data || off > b->len) return D3DERR_INVALIDCALL;
    *data = b->data + off;
    return S_OK;
}
static HRESULT WINAPI BufUnlock(buffer_t *b) { (void)b; return S_OK; }


/* ---------------- Direct3D 8: the same device behind Direct3D 8's interfaces ----------------
 * IDirect3D8 and IDirect3DDevice8 differ from Direct3D 9 in a few signatures: no
 * multisample quality in the presentation parameters, the vertex buffer offset and base
 * vertex index (set with SetIndices), the FVF passed to SetVertexShader, sampler states
 * among the texture stage states, and no share handles on resources. */
typedef struct { uint32_t bb_w, bb_h, bb_format, bb_count, ms_type, swap_effect; uintptr_t hwnd; int32_t windowed, auto_depth; uint32_t depth_format, flags, refresh, interval; } pparams8_t;
static void pp_from8(pparams_t *p, const pparams8_t *q) {
    memset(p, 0, sizeof *p);
    p->bb_w = q->bb_w; p->bb_h = q->bb_h; p->bb_format = q->bb_format; p->bb_count = q->bb_count; p->ms_type = q->ms_type; p->swap_effect = q->swap_effect;
    p->hwnd = q->hwnd; p->windowed = q->windowed; p->auto_depth = q->auto_depth; p->depth_format = q->depth_format; p->flags = q->flags; p->refresh = q->refresh; p->interval = q->interval;
}
static HRESULT WINAPI GetAdapterIdentifier8(com_t *s, uint32_t a, uint32_t f, char *id) {
    (void)s; (void)a; (void)f;
    if (!id) return D3DERR_INVALIDCALL;
    memset(id, 0, 1068);                                         /* D3DADAPTER_IDENTIFIER8: no DeviceName */
    strcpy(id, "wrld3d.dll"); strcpy(id + 512, "WRLD 3D (software rasterizer)");
    return S_OK;
}
static uint32_t WINAPI GetAdapterModeCount8(com_t *s, uint32_t a) { (void)s; (void)a; return 1; }
static HRESULT WINAPI EnumAdapterModes8(com_t *s, uint32_t a, uint32_t mode, dmode_t *m) { if (mode) return D3DERR_INVALIDCALL; return GetAdapterDisplayMode(s, a, m); }
static HRESULT WINAPI CheckDeviceMultiSampleType8(com_t *s, uint32_t a, uint32_t t, uint32_t f, int32_t w, uint32_t ms) { (void)s; (void)a; (void)t; (void)f; (void)w; return ms == 0 ? S_OK : D3DERR_NOTAVAILABLE; }
static void fill_caps8(uint32_t *c) { uint32_t c9[80]; fill_caps(c9); c9[53] = 0xFFFE0000; memcpy(c, c9, 212); }   /* D3DCAPS8: the first 53 fields of D3DCAPS9 */
static HRESULT WINAPI GetDeviceCaps8(com_t *s, uint32_t a, uint32_t t, uint32_t *caps) { (void)s; (void)a; (void)t; if (!caps) return D3DERR_INVALIDCALL; fill_caps8(caps); return S_OK; }
static HRESULT WINAPI CreateDevice8(com_t *s, uint32_t a, uint32_t type, uintptr_t focus, uint32_t behaviour, pparams8_t *pp8, device_t **out) {
    if (!pp8 || !out) return D3DERR_INVALIDCALL;
    pparams_t pp; pp_from8(&pp, pp8);
    HRESULT r = CreateDevice(s, a, type, focus, behaviour, &pp, out);
    if (r == S_OK) { (*out)->c.vt = vtables[IF_DEVICE8]; pp8->bb_w = pp.bb_w; pp8->bb_h = pp.bb_h; }
    return r;
}
static HRESULT WINAPI DevGetDeviceCaps8(device_t *d, uint32_t *caps) { (void)d; if (!caps) return D3DERR_INVALIDCALL; fill_caps8(caps); return S_OK; }
static HRESULT WINAPI DevGetDisplayMode8(device_t *d, dmode_t *m) { (void)d; return GetAdapterDisplayMode(NULL, 0, m); }
static HRESULT WINAPI Reset8(device_t *d, pparams8_t *pp8) { pparams_t pp; if (pp8) { pp_from8(&pp, pp8); return Reset(d, &pp); } return Reset(d, NULL); }
static HRESULT WINAPI SetStreamSource8(device_t *d, uint32_t n, buffer_t *vb, uint32_t stride) { return SetStreamSource(d, n, vb, 0, stride); }
static HRESULT WINAPI GetStreamSource8(device_t *d, uint32_t n, buffer_t **vb, uint32_t *stride) { if (!vb || !stride) return D3DERR_INVALIDCALL; *vb = n ? NULL : d->vb; *stride = d->vb_stride; if (*vb) (*vb)->c.refs++; return S_OK; }
static HRESULT WINAPI SetIndices8(device_t *d, buffer_t *ib, uint32_t base) { d->base8 = (int32_t)base; return SetIndices(d, ib); }
static HRESULT WINAPI GetIndices8(device_t *d, buffer_t **ib, uint32_t *base) { if (!ib) return D3DERR_INVALIDCALL; *ib = d->ib; if (*ib) (*ib)->c.refs++; if (base) *base = d->base8; return S_OK; }
static HRESULT WINAPI DrawIndexedPrimitive8(device_t *d, uint32_t prim, uint32_t minv, uint32_t nv, uint32_t start, uint32_t n) { return DrawIndexedPrimitive(d, prim, d->base8, minv, nv, start, n); }
static HRESULT WINAPI SetVertexShader8(device_t *d, uint32_t handle) { d->fvf = handle; return S_OK; }   /* fixed function: the handle is the FVF */
static HRESULT WINAPI GetVertexShader8(device_t *d, uint32_t *h) { if (!h) return D3DERR_INVALIDCALL; *h = d->fvf; return S_OK; }
static HRESULT WINAPI SetPixelShader8(device_t *d, uint32_t h) { (void)d; return h ? D3DERR_INVALIDCALL : S_OK; }
static HRESULT WINAPI SetTextureStageState8(device_t *d, uint32_t st, uint32_t ty, uint32_t v) { if (!st && (ty == 16 || ty == 17)) d->linear = v >= 2; return S_OK; }   /* MAGFILTER, MINFILTER */
static HRESULT WINAPI GetTextureStageState8(device_t *d, uint32_t st, uint32_t ty, uint32_t *v) { if (!v) return D3DERR_INVALIDCALL; *v = (ty == 16 || ty == 17) ? (d->linear ? 2 : 1) : 0; return S_OK; }
static HRESULT WINAPI CreateTexture8(device_t *d, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, texture_t **out) {
    HRESULT r = CreateTexture(d, w, h, levels, usage, fmt, pool, out, NULL);
    if (r == S_OK) (*out)->c.vt = vtables[IF_TEXTURE8];
    return r;
}
static HRESULT WINAPI CreateVertexBuffer8(device_t *d, uint32_t len, uint32_t usage, uint32_t fvf, uint32_t pool, buffer_t **out) { HRESULT r = make_buffer(IF_VB, len, fvf, out); if (r == S_OK) (*out)->c.vt = vtables[IF_VB8]; return r; }
static HRESULT WINAPI CreateIndexBuffer8(device_t *d, uint32_t len, uint32_t usage, uint32_t fmt, uint32_t pool, buffer_t **out) { HRESULT r = make_buffer(IF_IB, len, fmt, out); if (r == S_OK) (*out)->c.vt = vtables[IF_IB8]; return r; }
static HRESULT WINAPI ResourceManagerDiscardBytes(device_t *d, uint32_t n) { (void)d; (void)n; return S_OK; }
static HRESULT WINAPI SetRenderState8(device_t *d, uint32_t s, uint32_t v) { return SetRenderState(d, s, v); }
static HRESULT WINAPI ValidateDevice8(device_t *d, uint32_t *passes) { (void)d; if (passes) *passes = 1; return S_OK; }
static HRESULT WINAPI SetClipPlane8(device_t *d, uint32_t i, const float *p) { (void)d; (void)i; (void)p; return S_OK; }
static HRESULT WINAPI SetPaletteEntries8(device_t *d, uint32_t n, const void *e) { (void)d; (void)n; (void)e; return S_OK; }
static HRESULT WINAPI SetCurrentTexturePalette8(device_t *d, uint32_t n) { (void)d; (void)n; return S_OK; }
static HRESULT WINAPI SetGammaRamp8(device_t *d, uint32_t f, const void *r) { (void)d; (void)f; (void)r; return S_OK; }
static HRESULT WINAPI ShowCursor8(device_t *d, int32_t s) { (void)d; return !s; }
static void WINAPI SetCursorPosition8(device_t *d, int32_t x, int32_t y, uint32_t f) { (void)d; (void)x; (void)y; (void)f; }
static HRESULT WINAPI ApplyStateBlock8(device_t *d, uint32_t t) { (void)d; (void)t; return S_OK; }
static HRESULT WINAPI CaptureStateBlock8(device_t *d, uint32_t t) { (void)d; (void)t; return S_OK; }
static HRESULT WINAPI DeleteStateBlock8(device_t *d, uint32_t t) { (void)d; (void)t; return S_OK; }
static HRESULT WINAPI BeginStateBlock8(device_t *d) { (void)d; return S_OK; }
static HRESULT WINAPI EndStateBlock8(device_t *d, uint32_t *t) { (void)d; if (t) *t = 1; return S_OK; }
static HRESULT WINAPI CreateStateBlock8(device_t *d, uint32_t ty, uint32_t *t) { (void)d; (void)ty; if (t) *t = 1; return S_OK; }
static HRESULT WINAPI DeletePixelShader8(device_t *d, uint32_t h) { (void)d; (void)h; return S_OK; }
static HRESULT WINAPI DeleteVertexShader8(device_t *d, uint32_t h) { (void)d; (void)h; return S_OK; }
static HRESULT WINAPI SetVertexShaderConstant8(device_t *d, uint32_t r, const void *c, uint32_t n) { (void)d; (void)r; (void)c; (void)n; return S_OK; }
static HRESULT WINAPI GetInfo8(device_t *d, uint32_t id, void *p, uint32_t n) { (void)d; (void)id; (void)p; (void)n; return 1; }

static const impl_t impls[] = {
    { IF_D3D9, "QueryInterface", QueryInterface }, { IF_D3D9, "AddRef", AddRef }, { IF_D3D9, "Release", Release },
    { IF_D3D9, "GetAdapterCount", GetAdapterCount }, { IF_D3D9, "GetAdapterIdentifier", GetAdapterIdentifier },
    { IF_D3D9, "GetAdapterModeCount", GetAdapterModeCount }, { IF_D3D9, "EnumAdapterModes", EnumAdapterModes },
    { IF_D3D9, "GetAdapterDisplayMode", GetAdapterDisplayMode }, { IF_D3D9, "CheckDeviceType", CheckDeviceType },
    { IF_D3D9, "CheckDeviceFormat", CheckDeviceFormat }, { IF_D3D9, "CheckDeviceMultiSampleType", CheckDeviceMultiSampleType },
    { IF_D3D9, "CheckDepthStencilMatch", CheckDepthStencilMatch }, { IF_D3D9, "GetDeviceCaps", GetDeviceCaps9 },
    { IF_D3D9, "GetAdapterMonitor", GetAdapterMonitor }, { IF_D3D9, "CreateDevice", CreateDevice },
    { IF_DEVICE, "QueryInterface", QueryInterface }, { IF_DEVICE, "AddRef", AddRef }, { IF_DEVICE, "Release", Release },
    { IF_DEVICE, "TestCooperativeLevel", TestCooperativeLevel }, { IF_DEVICE, "GetAvailableTextureMem", GetAvailableTextureMem },
    { IF_DEVICE, "GetDirect3D", GetDirect3D }, { IF_DEVICE, "GetDeviceCaps", DevGetDeviceCaps }, { IF_DEVICE, "GetDisplayMode", DevGetDisplayMode },
    { IF_DEVICE, "Reset", Reset }, { IF_DEVICE, "Present", Present }, { IF_DEVICE, "BeginScene", BeginScene }, { IF_DEVICE, "EndScene", EndScene },
    { IF_DEVICE, "Clear", Clear }, { IF_DEVICE, "SetTransform", SetTransform }, { IF_DEVICE, "GetTransform", GetTransform },
    { IF_DEVICE, "MultiplyTransform", MultiplyTransform }, { IF_DEVICE, "SetViewport", SetViewport }, { IF_DEVICE, "GetViewport", GetViewport },
    { IF_DEVICE, "SetMaterial", SetMaterial }, { IF_DEVICE, "GetMaterial", GetMaterial }, { IF_DEVICE, "SetLight", SetLight },
    { IF_DEVICE, "GetLight", GetLight }, { IF_DEVICE, "LightEnable", LightEnable }, { IF_DEVICE, "GetLightEnable", GetLightEnable },
    { IF_DEVICE, "SetRenderState", SetRenderState }, { IF_DEVICE, "GetRenderState", GetRenderState },
    { IF_DEVICE, "SetTexture", SetTexture }, { IF_DEVICE, "GetTexture", GetTexture }, { IF_DEVICE, "SetTextureStageState", SetTextureStageState },
    { IF_DEVICE, "SetSamplerState", SetSamplerState }, { IF_DEVICE, "SetFVF", SetFVF }, { IF_DEVICE, "GetFVF", GetFVF },
    { IF_DEVICE, "SetVertexShader", SetVertexShader }, { IF_DEVICE, "SetPixelShader", SetPixelShader },
    { IF_DEVICE, "SetStreamSource", SetStreamSource }, { IF_DEVICE, "SetIndices", SetIndices },
    { IF_DEVICE, "DrawPrimitive", DrawPrimitive }, { IF_DEVICE, "DrawIndexedPrimitive", DrawIndexedPrimitive },
    { IF_DEVICE, "DrawPrimitiveUP", DrawPrimitiveUP }, { IF_DEVICE, "DrawIndexedPrimitiveUP", DrawIndexedPrimitiveUP },
    { IF_DEVICE, "CreateTexture", CreateTexture }, { IF_DEVICE, "CreateVertexBuffer", CreateVertexBuffer }, { IF_DEVICE, "CreateIndexBuffer", CreateIndexBuffer },
    { IF_TEXTURE, "QueryInterface", QueryInterface }, { IF_TEXTURE, "AddRef", AddRef }, { IF_TEXTURE, "Release", Release },
    { IF_TEXTURE, "LockRect", TexLockRect }, { IF_TEXTURE, "UnlockRect", TexUnlockRect }, { IF_TEXTURE, "GetLevelCount", TexGetLevelCount },
    { IF_TEXTURE, "GetLevelDesc", TexGetLevelDesc },
    { IF_VB, "QueryInterface", QueryInterface }, { IF_VB, "AddRef", AddRef }, { IF_VB, "Release", Release },
    { IF_VB, "Lock", BufLock }, { IF_VB, "Unlock", BufUnlock },
    { IF_IB, "QueryInterface", QueryInterface }, { IF_IB, "AddRef", AddRef }, { IF_IB, "Release", Release },
    { IF_IB, "Lock", BufLock }, { IF_IB, "Unlock", BufUnlock },
    { IF_SURFACE, "QueryInterface", QueryInterface }, { IF_SURFACE, "AddRef", AddRef }, { IF_SURFACE, "Release", Release },
    /* Direct3D 8 */
    { IF_D3D8, "QueryInterface", QueryInterface }, { IF_D3D8, "AddRef", AddRef }, { IF_D3D8, "Release", Release },
    { IF_D3D8, "GetAdapterCount", GetAdapterCount }, { IF_D3D8, "GetAdapterIdentifier", GetAdapterIdentifier8 },
    { IF_D3D8, "GetAdapterModeCount", GetAdapterModeCount8 }, { IF_D3D8, "EnumAdapterModes", EnumAdapterModes8 },
    { IF_D3D8, "GetAdapterDisplayMode", GetAdapterDisplayMode }, { IF_D3D8, "CheckDeviceType", CheckDeviceType },
    { IF_D3D8, "CheckDeviceFormat", CheckDeviceFormat }, { IF_D3D8, "CheckDeviceMultiSampleType", CheckDeviceMultiSampleType8 },
    { IF_D3D8, "CheckDepthStencilMatch", CheckDepthStencilMatch }, { IF_D3D8, "GetDeviceCaps", GetDeviceCaps8 },
    { IF_D3D8, "GetAdapterMonitor", GetAdapterMonitor }, { IF_D3D8, "CreateDevice", CreateDevice8 },
    { IF_DEVICE8, "QueryInterface", QueryInterface }, { IF_DEVICE8, "AddRef", AddRef }, { IF_DEVICE8, "Release", Release },
    { IF_DEVICE8, "TestCooperativeLevel", TestCooperativeLevel }, { IF_DEVICE8, "GetAvailableTextureMem", GetAvailableTextureMem },
    { IF_DEVICE8, "ResourceManagerDiscardBytes", ResourceManagerDiscardBytes }, { IF_DEVICE8, "GetDirect3D", GetDirect3D },
    { IF_DEVICE8, "GetDeviceCaps", DevGetDeviceCaps8 }, { IF_DEVICE8, "GetDisplayMode", DevGetDisplayMode8 }, { IF_DEVICE8, "Reset", Reset8 },
    { IF_DEVICE8, "Present", Present }, { IF_DEVICE8, "BeginScene", BeginScene }, { IF_DEVICE8, "EndScene", EndScene }, { IF_DEVICE8, "Clear", Clear },
    { IF_DEVICE8, "SetTransform", SetTransform }, { IF_DEVICE8, "GetTransform", GetTransform }, { IF_DEVICE8, "MultiplyTransform", MultiplyTransform },
    { IF_DEVICE8, "SetViewport", SetViewport }, { IF_DEVICE8, "GetViewport", GetViewport }, { IF_DEVICE8, "SetMaterial", SetMaterial },
    { IF_DEVICE8, "GetMaterial", GetMaterial }, { IF_DEVICE8, "SetLight", SetLight }, { IF_DEVICE8, "GetLight", GetLight },
    { IF_DEVICE8, "LightEnable", LightEnable }, { IF_DEVICE8, "GetLightEnable", GetLightEnable }, { IF_DEVICE8, "SetClipPlane", SetClipPlane8 },
    { IF_DEVICE8, "SetRenderState", SetRenderState8 }, { IF_DEVICE8, "GetRenderState", GetRenderState },
    { IF_DEVICE8, "BeginStateBlock", BeginStateBlock8 }, { IF_DEVICE8, "EndStateBlock", EndStateBlock8 }, { IF_DEVICE8, "ApplyStateBlock", ApplyStateBlock8 },
    { IF_DEVICE8, "CaptureStateBlock", CaptureStateBlock8 }, { IF_DEVICE8, "DeleteStateBlock", DeleteStateBlock8 }, { IF_DEVICE8, "CreateStateBlock", CreateStateBlock8 },
    { IF_DEVICE8, "SetTexture", SetTexture }, { IF_DEVICE8, "GetTexture", GetTexture }, { IF_DEVICE8, "SetTextureStageState", SetTextureStageState8 },
    { IF_DEVICE8, "GetTextureStageState", GetTextureStageState8 }, { IF_DEVICE8, "ValidateDevice", ValidateDevice8 }, { IF_DEVICE8, "GetInfo", GetInfo8 },
    { IF_DEVICE8, "SetPaletteEntries", SetPaletteEntries8 }, { IF_DEVICE8, "SetCurrentTexturePalette", SetCurrentTexturePalette8 },
    { IF_DEVICE8, "SetGammaRamp", SetGammaRamp8 }, { IF_DEVICE8, "ShowCursor", ShowCursor8 }, { IF_DEVICE8, "SetCursorPosition", SetCursorPosition8 },
    { IF_DEVICE8, "DrawPrimitive", DrawPrimitive }, { IF_DEVICE8, "DrawIndexedPrimitive", DrawIndexedPrimitive8 },
    { IF_DEVICE8, "DrawPrimitiveUP", DrawPrimitiveUP }, { IF_DEVICE8, "DrawIndexedPrimitiveUP", DrawIndexedPrimitiveUP },
    { IF_DEVICE8, "SetVertexShader", SetVertexShader8 }, { IF_DEVICE8, "GetVertexShader", GetVertexShader8 }, { IF_DEVICE8, "DeleteVertexShader", DeleteVertexShader8 },
    { IF_DEVICE8, "SetVertexShaderConstant", SetVertexShaderConstant8 }, { IF_DEVICE8, "SetPixelShader", SetPixelShader8 }, { IF_DEVICE8, "DeletePixelShader", DeletePixelShader8 },
    { IF_DEVICE8, "SetPixelShaderConstant", SetVertexShaderConstant8 },
    { IF_DEVICE8, "SetStreamSource", SetStreamSource8 }, { IF_DEVICE8, "GetStreamSource", GetStreamSource8 },
    { IF_DEVICE8, "SetIndices", SetIndices8 }, { IF_DEVICE8, "GetIndices", GetIndices8 },
    { IF_DEVICE8, "CreateTexture", CreateTexture8 }, { IF_DEVICE8, "CreateVertexBuffer", CreateVertexBuffer8 }, { IF_DEVICE8, "CreateIndexBuffer", CreateIndexBuffer8 },
    { IF_TEXTURE8, "QueryInterface", QueryInterface }, { IF_TEXTURE8, "AddRef", AddRef }, { IF_TEXTURE8, "Release", Release },
    { IF_TEXTURE8, "LockRect", TexLockRect }, { IF_TEXTURE8, "UnlockRect", TexUnlockRect }, { IF_TEXTURE8, "GetLevelCount", TexGetLevelCount },
    { IF_TEXTURE8, "GetLevelDesc", TexGetLevelDesc },
    { IF_VB8, "QueryInterface", QueryInterface }, { IF_VB8, "AddRef", AddRef }, { IF_VB8, "Release", Release }, { IF_VB8, "Lock", BufLock }, { IF_VB8, "Unlock", BufUnlock },
    { IF_IB8, "QueryInterface", QueryInterface }, { IF_IB8, "AddRef", AddRef }, { IF_IB8, "Release", Release }, { IF_IB8, "Lock", BufLock }, { IF_IB8, "Unlock", BufUnlock },
    { IF_SURFACE8, "QueryInterface", QueryInterface }, { IF_SURFACE8, "AddRef", AddRef }, { IF_SURFACE8, "Release", Release },
};
static const int nimpls = ARRAY_LEN(impls);
static void build_vtables(void) {
    if (vt_ready) return;
    for (unsigned i = 0; i < ARRAY_LEN(methods); i++) {
        const method_t *m = &methods[i];
        void *fn = stubs[MIN(m->nargs, 12)];
        for (int k = 0; k < nimpls; k++) if (impls[k].iface == m->iface && !strcmp(impls[k].name, m->name)) { fn = impls[k].fn; break; }
        vtables[m->iface][m->index] = fn;
    }
    vt_ready = true;
}


/* ---------------- exports ---------------- */
static com_t *WINAPI Direct3DCreate9(uint32_t sdk) {
    (void)sdk;
    if (!accel3d) { klog("w32: Direct3DCreate9 refused: 3D acceleration is turned off"); return NULL; }
    return com_new(IF_D3D9, IF_D3D9, sizeof(com_t));
}

static com_t *WINAPI Direct3DCreate8(uint32_t sdk) {
    (void)sdk;
    if (!accel3d) { klog("w32: Direct3DCreate8 refused: 3D acceleration is turned off"); return NULL; }
    return com_new(IF_D3D8, IF_D3D9, sizeof(com_t));
}

/* D3DX matrix helpers (Direct3D's left-handed, row-vector conventions) */
static float *ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; return m; }
static float *WINAPI D3DXMatrixIdentity(float *m) { return ident(m); }
static float *WINAPI D3DXMatrixMultiply(float *out, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
    memcpy(out, r, 64); return out;
}
static float *WINAPI D3DXMatrixTranslation(float *m, float x, float y, float z) { ident(m); m[12] = x; m[13] = y; m[14] = z; return m; }
static float *WINAPI D3DXMatrixScaling(float *m, float x, float y, float z) { ident(m); m[0] = x; m[5] = y; m[10] = z; return m; }
static float *WINAPI D3DXMatrixRotationX(float *m, float a) { ident(m); float c = r3d_cos(a), s = r3d_sin(a); m[5] = c; m[6] = s; m[9] = -s; m[10] = c; return m; }
static float *WINAPI D3DXMatrixRotationY(float *m, float a) { ident(m); float c = r3d_cos(a), s = r3d_sin(a); m[0] = c; m[2] = -s; m[8] = s; m[10] = c; return m; }
static float *WINAPI D3DXMatrixRotationZ(float *m, float a) { ident(m); float c = r3d_cos(a), s = r3d_sin(a); m[0] = c; m[1] = s; m[4] = -s; m[5] = c; return m; }
static float *WINAPI D3DXMatrixRotationYawPitchRoll(float *m, float yaw, float pitch, float roll) {
    float a[16], b[16], c[16];
    D3DXMatrixRotationZ(a, roll); D3DXMatrixRotationX(b, pitch); D3DXMatrixRotationY(c, yaw);
    D3DXMatrixMultiply(m, a, b); return D3DXMatrixMultiply(m, m, c);
}
static float *WINAPI D3DXMatrixPerspectiveFovLH(float *m, float fovy, float aspect, float zn, float zf) {
    float ys = r3d_cos(fovy / 2) / r3d_sin(fovy / 2), xs = ys / aspect;
    memset(m, 0, 64);
    m[0] = xs; m[5] = ys; m[10] = zf / (zf - zn); m[11] = 1; m[14] = -zn * zf / (zf - zn);
    return m;
}
static float *WINAPI D3DXMatrixLookAtLH(float *m, const float *eye, const float *at, const float *up) {
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] }, l = r3d_sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (int i = 0; i < 3; i++) z[i] /= l;
    float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
    l = r3d_sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); for (int i = 0; i < 3; i++) x[i] /= l;
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    ident(m);
    for (int i = 0; i < 3; i++) { m[i * 4 + 0] = x[i]; m[i * 4 + 1] = y[i]; m[i * 4 + 2] = z[i]; }
    m[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    m[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    m[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    return m;
}

#define E(n) { #n, (void *)n }
const w32_export_t d3d9_exports[] = { E(Direct3DCreate9) };
const int d3d9_count = ARRAY_LEN(d3d9_exports);
const w32_export_t d3d8_exports[] = { E(Direct3DCreate8) };
const int d3d8_count = ARRAY_LEN(d3d8_exports);
const w32_export_t d3dx9_exports[] = {
    E(D3DXMatrixIdentity), E(D3DXMatrixMultiply), E(D3DXMatrixTranslation), E(D3DXMatrixScaling), E(D3DXMatrixRotationX),
    E(D3DXMatrixRotationY), E(D3DXMatrixRotationZ), E(D3DXMatrixRotationYawPitchRoll), E(D3DXMatrixPerspectiveFovLH), E(D3DXMatrixLookAtLH),
};
const int d3dx9_count = ARRAY_LEN(d3dx9_exports);
