/* OpenGL 1.1 program: a lit, textured cube spinning in a window, set up with
 * WGL the way Windows programs do (ChoosePixelFormat, wglCreateContext,
 * SwapBuffers). On THE WRLD OS opengl32.dll draws with the WRLD 3D engine;
 * Control Panel > Visual Effects > 3D acceleration must be on.
 * Build: i686-w64-mingw32-gcc -O2 -mwindows glcube.c -lopengl32 -lglu32 -lgdi32 */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdio.h>

static HDC dc;
static HGLRC rc;
static GLuint tex, cube_list;
static int width = 480, height = 360;
static int frames;
static DWORD fps_start;

static void make_texture(void) {
    static unsigned char px[64 * 64 * 3];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        unsigned char *p = px + (y * 64 + x) * 3;
        int check = ((x >> 3) ^ (y >> 3)) & 1;
        p[0] = check ? 250 : 40; p[1] = check ? 190 : 110; p[2] = check ? 40 : 210;
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
}

static void make_cube(void) {
    static const float n[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    static const float v[6][4][3] = {
        { {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} },
        { { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} },
        { { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1} },
        { {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} },
        { {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} },
        { {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} } };
    static const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    cube_list = glGenLists(1);
    glNewList(cube_list, GL_COMPILE);
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glNormal3fv(n[f]);
        for (int i = 0; i < 4; i++) { glTexCoord2fv(uv[i]); glVertex3fv(v[f][i]); }
    }
    glEnd();
    glEndList();
}

static void render(void) {
    float t = GetTickCount() / 1000.0f;
    glViewport(0, 0, width, height);
    glClearColor(0.08f, 0.12f, 0.22f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(50, (double)width / (height ? height : 1), 0.5, 50);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 1.5, 6, 0, 0, 0, 0, 1, 0);

    /* a floor of coloured, unlit triangles */
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_TRIANGLE_FAN);
    glColor3f(0.25f, 0.3f, 0.4f); glVertex3f(0, -1.6f, 0);
    for (int i = 0; i <= 24; i++) {
        float a = i * 6.2831853f / 24;
        glColor3f(0.1f, 0.14f, 0.2f);
        glVertex3f(4 * __builtin_cosf(a), -1.6f, -4 * __builtin_sinf(a));
    }
    glEnd();

    /* the cube */
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_TEXTURE_2D);
    glPushMatrix();
    glRotatef(t * 50, 0, 1, 0);
    glRotatef(t * 30, 1, 0, 0);
    glColor3f(1, 1, 1);
    glCallList(cube_list);
    glPopMatrix();

    SwapBuffers(dc);
    frames++;
    DWORD now = GetTickCount();
    if (now - fps_start >= 1000) {
        char title[96];
        sprintf(title, "OpenGL cube - %d frames/s - %s", frames * 1000 / (int)(now - fps_start), (const char *)glGetString(GL_RENDERER));
        SetWindowTextA(WindowFromDC(dc), title);
        frames = 0; fps_start = now;
    }
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
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "GLCube";
    RegisterClassA(&wc);
    RECT r = { 0, 0, width, height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("GLCube", "OpenGL cube", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    rc = wglCreateContext(dc);
    if (!rc) {
        MessageBoxA(hwnd, "OpenGL is not available.\n\nTurn on 3D acceleration in Control Panel > Visual Effects and run this program again.",
                    "OpenGL cube", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }
    wglMakeCurrent(dc, rc);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    float light[4] = { 0.4f, 0.8f, 1.0f, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, light);
    make_texture();
    make_cube();
    ShowWindow(hwnd, show);
    fps_start = GetTickCount();

    MSG m;
    for (;;) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) goto done;
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        render();
    }
done:
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    return 0;
}
