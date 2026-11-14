/* Direct3D 10 program: a textured, lit cube drawn entirely by shaders - a vertex
 * shader transforms and lights each corner, a pixel shader samples the texture
 * and sweeps a band of light across the faces. The shaders are Shader Model 4
 * bytecode in d3d10cube_shaders.h, assembled from d3d10cube.sm4. --sm5 / --sm6
 * select real FXC/DXC fixtures; these are WRLD extensions, not D3D11/12 APIs.
 * --bench renders 120 deterministic frames and reports timing with
 * OutputDebugString (captured by WRLD's serial log).
 * On THE WRLD OS supported shaders use the native legacy VMware SVGA3D GPU
 * backend, otherwise the native CPU interpreter. --cpu forces the latter for
 * comparisons. Control Panel > Visual Effects > 3D acceleration must be on.
 * Build: i686-w64-mingw32-gcc -O2 -mwindows d3d10cube.c -ld3d10 -ldxguid */
#define COBJMACROS
#define __USE_MINGW_ANSI_STDIO 0
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>
#include <math.h>
#include "d3d10cube_shaders.h"
#include "d3d10cube_modern_shaders.h"

#if defined(__MINGW32__) && defined(__i386__)
/* Do not pull in MinGW's diagnostic _matherr printer: some x86 CRT builds
 * use SSE2 just to copy its double arguments. WRLD's x86 path is x87-only. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
int _matherr(struct _exception *e) { (void)e; return 0; }
#pragma GCC diagnostic pop
#endif

typedef struct { float x, y, z, nx, ny, nz, u, v; } vertex_t;
typedef struct { float wvp[16], world[16], light[4], time[4]; } constants_t;   /* rows of cb0 */

static ID3D10Device *dev;
static IDXGISwapChain *swap;
static ID3D10RenderTargetView *rtv;
static ID3D10DepthStencilView *dsv;
static ID3D10Buffer *vb, *ib, *cb;
static int width = 480, height = 360;
static int shader_model = 4;
static int force_cpu;
typedef const char *(WINAPI *backend_fn_t)(ID3D10Device *);
typedef void (WINAPI *stats_fn_t)(ID3D10Device *, UINT *, UINT *);
static backend_fn_t backend_fn;
static stats_fn_t stats_fn;
static const char *backend_name(void) { return backend_fn ? backend_fn(dev) : "D3D10 driver"; }

/* row-vector matrices (Direct3D's convention): p' = p * M */
static void identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void multiply(float *out, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
    memcpy(out, r, 64);
}
static void transpose(float *out, const float *m) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[j * 4 + i] = m[i * 4 + j]; }
static void rotation(float *m, float yaw, float pitch) {
    float ry[16], rx[16];
    identity(ry); ry[0] = cosf(yaw); ry[2] = -sinf(yaw); ry[8] = sinf(yaw); ry[10] = cosf(yaw);
    identity(rx); rx[5] = cosf(pitch); rx[6] = sinf(pitch); rx[9] = -sinf(pitch); rx[10] = cosf(pitch);
    multiply(m, ry, rx);
}

static HRESULT make_targets(void) {
    ID3D10Texture2D *back, *depth;
    HRESULT hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D10Texture2D, (void **)&back);
    if (FAILED(hr)) return hr;
    hr = ID3D10Device_CreateRenderTargetView(dev, (ID3D10Resource *)back, NULL, &rtv);
    ID3D10Texture2D_Release(back);
    if (FAILED(hr)) return hr;
    D3D10_TEXTURE2D_DESC td = { width, height, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, { 1, 0 }, D3D10_USAGE_DEFAULT, D3D10_BIND_DEPTH_STENCIL, 0, 0 };
    hr = ID3D10Device_CreateTexture2D(dev, &td, NULL, &depth);
    if (FAILED(hr)) return hr;
    hr = ID3D10Device_CreateDepthStencilView(dev, (ID3D10Resource *)depth, NULL, &dsv);
    ID3D10Texture2D_Release(depth);
    return hr;
}

static HRESULT init(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferDesc.Width = width; sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    HRESULT hr = D3D10CreateDeviceAndSwapChain(NULL, force_cpu ? D3D10_DRIVER_TYPE_REFERENCE : D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &sd, &swap, &dev);
    if (FAILED(hr)) return hr;
    HMODULE module = GetModuleHandleA("d3d10.dll");
    if (module) { backend_fn = (backend_fn_t)GetProcAddress(module, "WRLDGetD3D10Backend"); stats_fn = (stats_fn_t)GetProcAddress(module, "WRLDGetD3D10DrawStats"); }
    if (FAILED(hr = make_targets())) return hr;

    ID3D10VertexShader *vs; ID3D10PixelShader *ps; ID3D10InputLayout *layout;
    const void *vs_code = shader_model == 6 ? g_vs6 : shader_model == 5 ? g_vs5 : g_vs;
    const void *ps_code = shader_model == 6 ? g_ps6 : shader_model == 5 ? g_ps5 : g_ps;
    SIZE_T vs_size = shader_model == 6 ? sizeof g_vs6 : shader_model == 5 ? sizeof g_vs5 : sizeof g_vs;
    SIZE_T ps_size = shader_model == 6 ? sizeof g_ps6 : shader_model == 5 ? sizeof g_ps5 : sizeof g_ps;
    if (FAILED(hr = ID3D10Device_CreateVertexShader(dev, vs_code, vs_size, &vs))) return hr;
    if (FAILED(hr = ID3D10Device_CreatePixelShader(dev, ps_code, ps_size, &ps))) return hr;
    D3D10_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D10_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(hr = ID3D10Device_CreateInputLayout(dev, el, 3, vs_code, vs_size, &layout))) return hr;

    /* the cube: 4 vertices and 2 triangles per face, clockwise seen from outside */
    static const float n[6][3] = { {0,0,-1}, {0,0,1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    static const float c[6][4][3] = {
        { {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}, { 1,-1,-1} }, { { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}, {-1,-1, 1} },
        { { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}, { 1,-1, 1} }, { {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1}, {-1,-1,-1} },
        { {-1, 1,-1}, {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1} }, { {-1,-1, 1}, {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1} } };
    static const float uv[4][2] = { {0,1}, {0,0}, {1,0}, {1,1} };
    vertex_t verts[24]; WORD idx[36];
    for (int f = 0; f < 6; f++) {
        for (int i = 0; i < 4; i++) verts[f * 4 + i] = (vertex_t){ c[f][i][0], c[f][i][1], c[f][i][2], n[f][0], n[f][1], n[f][2], uv[i][0], uv[i][1] };
        WORD b = f * 4, q[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
        memcpy(idx + f * 6, q, sizeof q);
    }
    D3D10_BUFFER_DESC bd = { sizeof verts, D3D10_USAGE_IMMUTABLE, D3D10_BIND_VERTEX_BUFFER, 0, 0 };
    D3D10_SUBRESOURCE_DATA init = { verts, 0, 0 };
    if (FAILED(hr = ID3D10Device_CreateBuffer(dev, &bd, &init, &vb))) return hr;
    bd.ByteWidth = sizeof idx; bd.BindFlags = D3D10_BIND_INDEX_BUFFER; init.pSysMem = idx;
    if (FAILED(hr = ID3D10Device_CreateBuffer(dev, &bd, &init, &ib))) return hr;
    bd.ByteWidth = sizeof(constants_t); bd.Usage = D3D10_USAGE_DEFAULT; bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    if (FAILED(hr = ID3D10Device_CreateBuffer(dev, &bd, NULL, &cb))) return hr;

    /* a 64x64 texture: tiles with a ring */
    static DWORD px[64 * 64];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        int d = (x - 32) * (x - 32) + (y - 32) * (y - 32);
        DWORD rgba = d < 22 * 22 && d > 15 * 15 ? 0xFFF0F0F0 : ((x >> 4) + (y >> 4)) & 1 ? 0xFF20A0F0 : 0xFF6030C0;   /* ABGR in memory: R first */
        px[y * 64 + x] = rgba;
    }
    D3D10_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D10_USAGE_IMMUTABLE, D3D10_BIND_SHADER_RESOURCE, 0, 0 };
    D3D10_SUBRESOURCE_DATA tinit = { px, 64 * 4, 0 };
    ID3D10Texture2D *tex; ID3D10ShaderResourceView *srv; ID3D10SamplerState *smp;
    if (FAILED(hr = ID3D10Device_CreateTexture2D(dev, &td, &tinit, &tex))) return hr;
    if (FAILED(hr = ID3D10Device_CreateShaderResourceView(dev, (ID3D10Resource *)tex, NULL, &srv))) return hr;
    D3D10_SAMPLER_DESC sdesc = { D3D10_FILTER_MIN_MAG_MIP_LINEAR, D3D10_TEXTURE_ADDRESS_WRAP, D3D10_TEXTURE_ADDRESS_WRAP, D3D10_TEXTURE_ADDRESS_WRAP,
                                 0, 1, D3D10_COMPARISON_NEVER, { 0, 0, 0, 0 }, 0, D3D10_FLOAT32_MAX };
    if (FAILED(hr = ID3D10Device_CreateSamplerState(dev, &sdesc, &smp))) return hr;

    UINT stride = sizeof(vertex_t), offset = 0;
    ID3D10Device_IASetInputLayout(dev, layout);
    ID3D10Device_IASetVertexBuffers(dev, 0, 1, &vb, &stride, &offset);
    ID3D10Device_IASetIndexBuffer(dev, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D10Device_IASetPrimitiveTopology(dev, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Device_VSSetShader(dev, vs);
    ID3D10Device_PSSetShader(dev, ps);
    ID3D10Device_VSSetConstantBuffers(dev, 0, 1, &cb);
    ID3D10Device_PSSetConstantBuffers(dev, 0, 1, &cb);
    ID3D10Device_PSSetShaderResources(dev, 0, 1, &srv);
    ID3D10Device_PSSetSamplers(dev, 0, 1, &smp);
    ID3D10VertexShader_Release(vs); ID3D10PixelShader_Release(ps); ID3D10InputLayout_Release(layout);
    ID3D10Texture2D_Release(tex); ID3D10ShaderResourceView_Release(srv); ID3D10SamplerState_Release(smp);
    return S_OK;
}

static void render(float t) {
    ID3D10Device_OMSetRenderTargets(dev, 1, &rtv, dsv);
    D3D10_VIEWPORT vp = { 0, 0, width, height, 0, 1 };
    ID3D10Device_RSSetViewports(dev, 1, &vp);
    float clear[4] = { 0.06f, 0.08f, 0.14f, 1 };
    ID3D10Device_ClearRenderTargetView(dev, rtv, clear);
    ID3D10Device_ClearDepthStencilView(dev, dsv, D3D10_CLEAR_DEPTH, 1.0f, 0);

    constants_t k;
    float world[16], view[16], proj[16], wv[16], wvp[16];
    rotation(world, t * 0.8f, t * 0.5f);
    identity(view); view[13] = -0.1f; view[14] = 5.0f;                       /* the camera 5 units back */
    float ys = 1 / tanf(0.45f), zn = 0.5f, zf = 50;
    memset(proj, 0, 64);
    proj[0] = ys * height / width; proj[5] = ys; proj[10] = zf / (zf - zn); proj[11] = 1; proj[14] = -zn * zf / (zf - zn);
    multiply(wv, world, view); multiply(wvp, wv, proj);
    transpose(k.wvp, wvp); transpose(k.world, world);
    float l[3] = { 0.4f, 0.7f, -0.6f }, len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    k.light[0] = l[0] / len; k.light[1] = l[1] / len; k.light[2] = l[2] / len; k.light[3] = 0;
    k.time[0] = t * 0.6f; k.time[1] = k.time[2] = k.time[3] = 0;
    ID3D10Device_UpdateSubresource(dev, (ID3D10Resource *)cb, 0, NULL, &k, 0, 0);

    ID3D10Device_DrawIndexed(dev, 36, 0, 0);
    IDXGISwapChain_Present(swap, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev;
    int bench = strstr(cmd, "--bench") != NULL, bench_frames = 0;
    force_cpu = strstr(cmd, "--cpu") != NULL;
    if (strstr(cmd, "--sm6")) shader_model = 6;
    else if (strstr(cmd, "--sm5")) shader_model = 5;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "D3D10Cube";
    RegisterClassA(&wc);
    RECT r = { 0, 0, width, height };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowA("D3D10Cube", "Direct3D 10 cube", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    HRESULT hr = init(hwnd);
    if (FAILED(hr)) {
        char msg[200];
        sprintf(msg, "D3D10CUBE model=%d init rejected hr=%08lX", shader_model, (unsigned long)hr);
        OutputDebugStringA(msg);
        sprintf(msg, "Direct3D 10 is not available (error 0x%08lX).\n\nTurn on 3D acceleration in Control Panel > Visual Effects and run this program again.", (unsigned long)hr);
        MessageBoxA(hwnd, msg, "Direct3D 10 cube", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }
    ShowWindow(hwnd, show);
    if (bench) for (int i = 0; i < 4; i++) render(0);
    MSG m;
    int frames = 0;
    DWORD start = GetTickCount();
    DWORD bench_start = start;
    for (;;) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) goto done;
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        render(bench ? bench_frames * (1.0f / 60) : GetTickCount() / 1000.0f);
        frames++;
        if (bench && ++bench_frames == 120) {
            char result[240]; UINT gpu = 0, cpu = 0;
            if (stats_fn) stats_fn(dev, &gpu, &cpu);
            sprintf(result, "D3D10BENCH model=%d frames=120 ms=%lu size=%dx%d backend=%s gpu_draws=%u cpu_draws=%u", shader_model,
                    (unsigned long)(GetTickCount() - bench_start), width, height, backend_name(), gpu, cpu);
            OutputDebugStringA(result);
            goto done;
        }
        DWORD now = GetTickCount();
        if (now - start >= 1000) {
            char title[96];
            _snprintf(title, sizeof title, "Direct3D 10 cube - SM%d - %d frames/s - %s", shader_model, frames * 1000 / (int)(now - start), backend_name());
            title[sizeof title-1]=0;
            SetWindowTextA(hwnd, title);
            frames = 0; start = now;
        }
    }
done:
    ID3D10Device_ClearState(dev);
    ID3D10RenderTargetView_Release(rtv); ID3D10DepthStencilView_Release(dsv);
    ID3D10Buffer_Release(vb); ID3D10Buffer_Release(ib); ID3D10Buffer_Release(cb);
    IDXGISwapChain_Release(swap);
    ID3D10Device_Release(dev);
    return 0;
}
