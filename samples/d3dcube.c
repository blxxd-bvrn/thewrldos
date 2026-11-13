/* Direct3D 9 program: a lit, textured cube from vertex and index buffers, plus a
 * pre-transformed (XYZRHW) overlay, the way fixed-function D3D9 games draw.
 * It asks for Vulkan first and falls back to Direct3D 9 when there is no Vulkan
 * device, as many games do. On THE WRLD OS d3d9.dll draws with the WRLD 3D
 * engine; Control Panel > Visual Effects > 3D acceleration must be on.
 * Build: i686-w64-mingw32-gcc -O2 -mwindows d3dcube.c -ld3d9 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <math.h>

typedef struct { float x, y, z, nx, ny, nz, u, v; } vertex_t;
#define FVF_VERTEX (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
typedef struct { float x, y, z, rhw; DWORD color; } overlay_t;
#define FVF_OVERLAY (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static IDirect3D9 *d3d;
static IDirect3DDevice9 *dev;
static IDirect3DVertexBuffer9 *vb;
static IDirect3DIndexBuffer9 *ib;
static IDirect3DTexture9 *tex;
static int width = 480, height = 360;
static char api_note[64] = "";

/* Direct3D's left-handed matrices, row vectors */
static void identity(D3DMATRIX *m) { memset(m, 0, sizeof *m); m->_11 = m->_22 = m->_33 = m->_44 = 1; }
static void multiply(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b) {
    D3DMATRIX r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a->m[i][k] * b->m[k][j];
        r.m[i][j] = s;
    }
    *out = r;
}
static void rotation_y(D3DMATRIX *m, float a) { identity(m); m->_11 = cosf(a); m->_13 = -sinf(a); m->_31 = sinf(a); m->_33 = cosf(a); }
static void rotation_x(D3DMATRIX *m, float a) { identity(m); m->_22 = cosf(a); m->_23 = sinf(a); m->_32 = -sinf(a); m->_33 = cosf(a); }
static void perspective(D3DMATRIX *m, float fovy, float aspect, float zn, float zf) {
    float ys = 1 / tanf(fovy / 2);
    memset(m, 0, sizeof *m);
    m->_11 = ys / aspect; m->_22 = ys; m->_33 = zf / (zf - zn); m->_34 = 1; m->_43 = -zn * zf / (zf - zn);
}

/* Vulkan's loader is optional: a game checks for a device and falls back */
static int vulkan_available(void) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { strcpy(api_note, "no Vulkan loader"); return 0; }
    typedef int (WINAPI *create_t)(const void *, const void *, void **);
    create_t create = (create_t)GetProcAddress(vk, "vkCreateInstance");
    struct { int sType; const void *next; unsigned flags; const void *app; unsigned layers; const void *pl; unsigned exts; const void *pe; } info = { 1 };
    void *inst = NULL;
    int r = create ? create(&info, NULL, &inst) : -9;
    sprintf(api_note, "Vulkan: %s", r == 0 ? "available" : r == -9 ? "no device" : "failed");
    return r == 0;
}

static int init_d3d(HWND hwnd) {
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 0;
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = hwnd;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) return 0;

    /* 24 vertices (4 per face) and 36 indices */
    static const float n[6][3] = { {0,0,-1}, {0,0,1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    static const float c[6][4][3] = {   /* clockwise seen from outside: Direct3D's front faces */
        { {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}, { 1,-1,-1} },
        { { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}, {-1,-1, 1} },
        { { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}, { 1,-1, 1} },
        { {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1}, {-1,-1,-1} },
        { {-1, 1,-1}, {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1} },
        { {-1,-1, 1}, {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1} } };
    static const float uv[4][2] = { {0,1}, {0,0}, {1,0}, {1,1} };
    vertex_t *v; WORD *ix;
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, 24 * sizeof(vertex_t), 0, FVF_VERTEX, D3DPOOL_MANAGED, &vb, NULL))) return 0;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(dev, 36 * sizeof(WORD), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL))) return 0;
    IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0);
    IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&ix, 0);
    for (int f = 0; f < 6; f++) {
        for (int i = 0; i < 4; i++)
            v[f * 4 + i] = (vertex_t){ c[f][i][0], c[f][i][1], c[f][i][2], n[f][0], n[f][1], n[f][2], uv[i][0], uv[i][1] };
        WORD b = f * 4, q[6] = { b, b + 1, b + 2, b, b + 2, b + 3 };
        memcpy(ix + f * 6, q, sizeof q);
    }
    IDirect3DVertexBuffer9_Unlock(vb);
    IDirect3DIndexBuffer9_Unlock(ib);

    /* a 64x64 texture filled through LockRect */
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex, NULL))) return 0;
    D3DLOCKED_RECT lr;
    IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0);
    for (int y = 0; y < 64; y++) {
        DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < 64; x++) {
            int d = (x - 32) * (x - 32) + (y - 32) * (y - 32);
            row[x] = d < 22 * 22 && d > 14 * 14 ? 0xFFFFFFFF : ((x >> 4) + (y >> 4)) & 1 ? 0xFFD03030 : 0xFF3050C0;
        }
    }
    IDirect3DTexture9_UnlockRect(tex, 0);
    return 1;
}

static void render(void) {
    float t = GetTickCount() / 1000.0f;
    D3DVIEWPORT9 vp = { 0, 0, width, height, 0, 1 };
    IDirect3DDevice9_SetViewport(dev, &vp);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 32, 48), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);

    D3DMATRIX world, rx, ry, view, proj;
    rotation_y(&ry, t * 0.9f); rotation_x(&rx, t * 0.6f);
    multiply(&world, &ry, &rx);
    identity(&view); view._42 = -0.2f; view._43 = 5.0f;     /* the camera 5 units back, looking along +z */
    perspective(&proj, 0.9f, (float)width / (height ? height : 1), 0.5f, 50);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &world);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &view);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &proj);

    D3DMATERIAL9 mat = { 0 };
    mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1;
    IDirect3DDevice9_SetMaterial(dev, &mat);
    D3DLIGHT9 light = { 0 };
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1;
    light.Direction.x = -0.4f; light.Direction.y = -0.7f; light.Direction.z = 0.6f;
    IDirect3DDevice9_SetLight(dev, 0, &light);
    IDirect3DDevice9_LightEnable(dev, 0, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_AMBIENT, D3DCOLOR_XRGB(60, 60, 60));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_CCW);

    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetFVF(dev, FVF_VERTEX);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(vertex_t));
    IDirect3DDevice9_SetIndices(dev, ib);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 24, 0, 12);

    /* a gradient bar along the bottom in screen pixels (pre-transformed vertices, no texture) */
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    float y0 = height - 28.0f, y1 = height - 8.0f, x1 = 8 + (width - 16) * (0.5f + 0.5f * sinf(t));
    overlay_t bar[4] = {
        { 8, y0, 0, 1, 0xFF40C040 }, { x1, y0, 0, 1, 0xFFE0E040 },
        { 8, y1, 0, 1, 0xFF208020 }, { x1, y1, 0, 1, 0xFFC0A020 } };
    IDirect3DDevice9_SetFVF(dev, FVF_OVERLAY);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, bar, sizeof(overlay_t));

    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: width = LOWORD(lp); height = HIWORD(lp); return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "D3DCube";
    RegisterClassA(&wc);
    RECT r = { 0, 0, width, height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("D3DCube", "Direct3D 9 cube", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    vulkan_available();
    if (!init_d3d(hwnd)) {
        MessageBoxA(hwnd, "Direct3D 9 is not available.\n\nTurn on 3D acceleration in Control Panel > Visual Effects and run this program again.",
                    "Direct3D 9 cube", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }
    D3DADAPTER_IDENTIFIER9 id;
    IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id);
    ShowWindow(hwnd, show);

    MSG m;
    int frames = 0;
    DWORD start = GetTickCount();
    for (;;) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) goto done;
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        render();
        frames++;
        DWORD now = GetTickCount();
        if (now - start >= 1000) {
            char title[160];
            sprintf(title, "Direct3D 9 cube - %d frames/s - %s - %s", frames * 1000 / (int)(now - start), id.Description, api_note);
            SetWindowTextA(hwnd, title);
            frames = 0; start = now;
        }
    }
done:
    IDirect3DTexture9_Release(tex);
    IDirect3DIndexBuffer9_Release(ib);
    IDirect3DVertexBuffer9_Release(vb);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
