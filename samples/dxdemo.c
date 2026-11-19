/* dxdemo.exe: Direct3D 8, DirectInput 8 and DirectSound together on THE WRLD OS.
 * A textured cube turns; the arrow keys (or dragging with the mouse) turn it faster, the
 * wheel zooms, Space plays a tone. The title bar shows what DirectInput reports. */
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d8.h>
#include <dinput.h>
#include <dsound.h>
#include <math.h>
#include <stdio.h>

typedef IDirect3D8 *(WINAPI *create8_t)(UINT);
static IDirect3DDevice8 *dev;
static IDirect3DVertexBuffer8 *vb;
static IDirect3DIndexBuffer8 *ib;
static IDirect3DTexture8 *tex;
static IDirectInputDevice8A *kbd, *mouse;
static IDirectSoundBuffer *tone;
static float ax = 0.4f, ay = 0.6f, dist = 4.0f;

typedef struct { float x, y, z; DWORD color; float u, v; } vtx_t;
#define FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void matrices(void) {
    float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    D3DMATRIX w = { { { cy, sx * sy, -cx * sy, 0, 0, cx, sx, 0, sy, -sx * cy, cx * cy, 0, 0, 0, 0, 1 } } };
    D3DMATRIX v = { { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, dist, 1 } } };
    float zn = 0.1f, zf = 100, ys = 1 / tanf(0.5f), xs = ys * 3 / 4;
    D3DMATRIX p = { { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0 } } };
    IDirect3DDevice8_SetTransform(dev, D3DTS_WORLD, &w);
    IDirect3DDevice8_SetTransform(dev, D3DTS_VIEW, &v);
    IDirect3DDevice8_SetTransform(dev, D3DTS_PROJECTION, &p);
}

static int setup_d3d(HWND h) {
    HMODULE m = LoadLibraryA("d3d8.dll");
    create8_t create = m ? (create8_t)GetProcAddress(m, "Direct3DCreate8") : NULL;
    IDirect3D8 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) return 0;
    D3DDISPLAYMODE mode; IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = mode.Format;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D16;
    if (FAILED(IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, h, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) return 0;
    /* a cube: 4 vertices a face, colours per face; the index buffer is shared through the base vertex */
    static const float face[6][4][3] = {
        { {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}, { 1,-1,-1} }, { { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}, {-1,-1, 1} },
        { {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1}, {-1,-1,-1} }, { { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1}, { 1,-1, 1} },
        { {-1, 1,-1}, {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1} }, { {-1,-1, 1}, {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1} } };
    static const DWORD col[6] = { 0xFFFF8080, 0xFF80FF80, 0xFF8080FF, 0xFFFFFF80, 0xFF80FFFF, 0xFFFF80FF };
    IDirect3DDevice8_CreateVertexBuffer(dev, 24 * sizeof(vtx_t), 0, FVF, D3DPOOL_MANAGED, &vb);
    vtx_t *v; IDirect3DVertexBuffer8_Lock(vb, 0, 0, (BYTE **)&v, 0);
    static const float uv[4][2] = { {0,1}, {0,0}, {1,0}, {1,1} };
    for (int f = 0; f < 6; f++) for (int k = 0; k < 4; k++) { vtx_t *p = &v[f * 4 + k]; p->x = face[f][k][0]; p->y = face[f][k][1]; p->z = face[f][k][2]; p->color = col[f]; p->u = uv[k][0]; p->v = uv[k][1]; }
    IDirect3DVertexBuffer8_Unlock(vb);
    IDirect3DDevice8_CreateIndexBuffer(dev, 6 * sizeof(WORD), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
    WORD *ix; IDirect3DIndexBuffer8_Lock(ib, 0, 0, (BYTE **)&ix, 0);
    static const WORD quad[6] = { 0, 1, 2, 0, 2, 3 }; memcpy(ix, quad, sizeof quad);
    IDirect3DIndexBuffer8_Unlock(ib);
    /* a 16-bit (R5G6B5) checker texture */
    if (SUCCEEDED(IDirect3DDevice8_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED, &tex))) {
        D3DLOCKED_RECT lr; IDirect3DTexture8_LockRect(tex, 0, &lr, NULL, 0);
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) ((WORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = ((x / 8 + y / 8) & 1) ? 0xFFFF : 0x4208;
        IDirect3DTexture8_UnlockRect(tex, 0);
    }
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    return 1;
}

static void setup_input(HWND h) {
    IDirectInput8A *di;
    if (FAILED(DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&di, NULL))) return;
    if (SUCCEEDED(IDirectInput8_CreateDevice(di, &GUID_SysKeyboard, &kbd, NULL))) {
        IDirectInputDevice8_SetDataFormat(kbd, &c_dfDIKeyboard);
        IDirectInputDevice8_SetCooperativeLevel(kbd, h, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
        IDirectInputDevice8_Acquire(kbd);
    }
    if (SUCCEEDED(IDirectInput8_CreateDevice(di, &GUID_SysMouse, &mouse, NULL))) {
        IDirectInputDevice8_SetDataFormat(mouse, &c_dfDIMouse);
        IDirectInputDevice8_SetCooperativeLevel(mouse, h, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
        IDirectInputDevice8_Acquire(mouse);
    }
}

static void setup_sound(HWND h) {
    IDirectSound *ds;
    if (FAILED(DirectSoundCreate(NULL, &ds, NULL))) return;
    IDirectSound_SetCooperativeLevel(ds, h, DSSCL_PRIORITY);
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
    DSBUFFERDESC d = { sizeof d, DSBCAPS_CTRLVOLUME, 22050 * 2 * 3 / 10, 0, &wf };
    if (FAILED(IDirectSound_CreateSoundBuffer(ds, &d, &tone, NULL))) return;
    void *p1, *p2; DWORD n1, n2;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(tone, 0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER))) {
        short *s = p1; int n = n1 / 2;
        for (int i = 0; i < n; i++) { float env = i < 400 ? i / 400.0f : i > n - 2000 ? (n - i) / 2000.0f : 1; s[i] = (short)(sinf(i * 2 * 3.14159265f * 440 / 22050) * 12000 * env); }
        IDirectSoundBuffer_Unlock(tone, p1, n1, p2, n2);
    }
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "dxdemo";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "dxdemo", "DirectX 8 demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 520, 440, NULL, NULL, inst, NULL);
    if (!setup_d3d(h)) { MessageBoxA(h, "Direct3D 8 is not available.", "DirectX 8 demo", MB_ICONERROR); return 1; }
    setup_input(h);
    setup_sound(h);
    MSG msg; DWORD last = GetTickCount(); int space_was = 0; char title[160] = "";
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return 0; TranslateMessage(&msg); DispatchMessageA(&msg); }
        DWORD now = GetTickCount(); float dt = (now - last) / 1000.0f; last = now;
        BYTE keys[256] = { 0 }; DIMOUSESTATE ms = { 0 };
        if (kbd && FAILED(IDirectInputDevice8_GetDeviceState(kbd, sizeof keys, keys))) IDirectInputDevice8_Acquire(kbd);
        if (mouse && FAILED(IDirectInputDevice8_GetDeviceState(mouse, sizeof ms, &ms))) IDirectInputDevice8_Acquire(mouse);
        ay += dt * 0.8f; ax += dt * 0.3f;
        if (keys[DIK_LEFT] & 0x80) ay -= dt * 3; if (keys[DIK_RIGHT] & 0x80) ay += dt * 3;
        if (keys[DIK_UP] & 0x80) ax -= dt * 3; if (keys[DIK_DOWN] & 0x80) ax += dt * 3;
        if (ms.rgbButtons[0] & 0x80) { ay += ms.lX * 0.01f; ax += ms.lY * 0.01f; }
        dist -= ms.lZ * 0.002f; if (dist < 2.5f) dist = 2.5f; if (dist > 12) dist = 12;
        int space = (keys[DIK_SPACE] & 0x80) != 0;
        if (space && !space_was && tone) { IDirectSoundBuffer_SetCurrentPosition(tone, 0); IDirectSoundBuffer_Play(tone, 0, 0, 0); }
        space_was = space;
        char t[160]; int down = 0; for (int k = 0; k < 256; k++) if (keys[k] & 0x80) down++;
        snprintf(t, sizeof t, "DirectX 8 demo - keys down %d, mouse %ld,%ld wheel %ld buttons %d%d%d%s", down, ms.lX, ms.lY, ms.lZ,
                 !!(ms.rgbButtons[0] & 0x80), !!(ms.rgbButtons[1] & 0x80), !!(ms.rgbButtons[2] & 0x80), space ? " - tone" : "");
        if (strcmp(t, title)) { strcpy(title, t); SetWindowTextA(h, title); }
        IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(16, 32, 64), 1.0f, 0);
        IDirect3DDevice8_BeginScene(dev);
        matrices();
        IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
        IDirect3DDevice8_SetVertexShader(dev, FVF);
        IDirect3DDevice8_SetStreamSource(dev, 0, vb, sizeof(vtx_t));
        for (int f = 0; f < 6; f++) { IDirect3DDevice8_SetIndices(dev, ib, f * 4); IDirect3DDevice8_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 4, 0, 2); }
        IDirect3DDevice8_EndScene(dev);
        IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
        Sleep(10);
    }
}
