/* GUI Win32 program (RegisterClass / CreateWindow / message loop / GDI). */
#include <windows.h>
#include <stdio.h>

#define ID_BTN_HELLO 101
#define ID_BTN_COLOR 102
#define ID_BTN_QUIT  103
static int clicks = 0;
static COLORREF bg = RGB(255, 255, 230);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowA("BUTTON", "Say hello", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 150, 110, 28, hwnd, (HMENU)ID_BTN_HELLO, NULL, NULL);
        CreateWindowA("BUTTON", "Change colour", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 140, 150, 110, 28, hwnd, (HMENU)ID_BTN_COLOR, NULL, NULL);
        CreateWindowA("BUTTON", "Quit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 260, 150, 80, 28, hwnd, (HMENU)ID_BTN_QUIT, NULL, NULL);
        SetTimer(hwnd, 1, 1000, NULL);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_HELLO:
            clicks++;
            MessageBoxA(hwnd, "Hello from a Win32 GUI program running on Lumina OS!", "winhello.exe", MB_OK | MB_ICONINFORMATION);
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        case ID_BTN_COLOR:
            bg = RGB(150 + (GetTickCount() * 7) % 105, 180 + (GetTickCount() * 3) % 75, 200 + GetTickCount() % 55);
            InvalidateRect(hwnd, NULL, TRUE);
            break;
        case ID_BTN_QUIT:
            if (MessageBoxA(hwnd, "Really quit?", "winhello.exe", MB_YESNO | MB_ICONQUESTION) == IDYES) DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        char b[64];
        sprintf(b, "Clicked at %d, %d", (short)LOWORD(lp), (short)HIWORD(lp));
        SetWindowTextA(hwnd, b);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 128));
        TextOutA(dc, 20, 20, "Hello, Win32 world!", 19);
        char b[128];
        SYSTEMTIME st; GetLocalTime(&st);
        sprintf(b, "Button clicks: %d    Time: %02d:%02d:%02d", clicks, st.wHour, st.wMinute, st.wSecond);
        SetTextColor(dc, RGB(0, 0, 0));
        TextOutA(dc, 20, 50, b, lstrlenA(b));
        RECT r2 = { 20, 80, rc.right - 20, 140 };
        DrawTextA(dc, "This window, its buttons and this text are drawn through the Lumina Win32 subsystem (user32 / gdi32).", -1, &r2, DT_WORDBREAK);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(200, 60, 40));
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, 20, 195, NULL); LineTo(dc, rc.right - 20, 195);
        SelectObject(dc, old); DeleteObject(pen);
        Rectangle(dc, 20, 205, 80, 235);
        Ellipse(dc, 90, 205, 150, 235);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "LuminaHello";
    if (!RegisterClassA(&wc)) return 1;
    HWND hwnd = CreateWindowExA(0, "LuminaHello", "Hello Windows (winhello.exe)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 380, 290, NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}
