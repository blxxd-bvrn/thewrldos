/* ccdemo.exe: the common controls, the registry and an INI file on THE WRLD OS */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

static HWND tb, sb, tab, lv, pb, trk, ud, edit, label;
static int runs;

static void status(const char *s) { SendMessageA(sb, SB_SETTEXTA, 0, (LPARAM)s); }

static void layout(HWND h) {
    RECT r; GetClientRect(h, &r);
    SendMessageA(tb, TB_AUTOSIZE, 0, 0);
    SendMessageA(sb, WM_SIZE, 0, 0);
    RECT tr; GetWindowRect(tb, &tr);
    int top = tr.bottom - tr.top + 6, bottom = r.bottom - 28;
    MoveWindow(tab, 8, top, r.right - 16, bottom - top - 70, TRUE);
    MoveWindow(lv, 16, top + 30, r.right - 32, bottom - top - 110, TRUE);
    MoveWindow(pb, 8, bottom - 60, r.right - 16, 18, TRUE);
    MoveWindow(trk, 8, bottom - 36, r.right - 140, 30, TRUE);
    MoveWindow(edit, r.right - 124, bottom - 32, 60, 22, TRUE);
    MoveWindow(ud, r.right - 64, bottom - 32, 16, 22, TRUE);
    MoveWindow(label, r.right - 44, bottom - 28, 40, 18, TRUE);
    int parts[3] = { r.right / 2, r.right * 3 / 4, -1 };
    SendMessageA(sb, SB_SETPARTS, 3, (LPARAM)parts);
}

static void fill_list(int page) {
    SendMessageA(lv, LVM_DELETEALLITEMS, 0, 0);
    static const char *names[][3] = {
        { "Notepad.exe", "Application", "68 KB" }, { "Read Me.txt", "Text Document", "2 KB" }, { "Welcome to WordPad.rtf", "Rich Text", "3 KB" },
        { "mathlib.dll", "Application extension", "13 KB" }, { "ccdemo.exe", "Application", "40 KB" }, { "registry.dat", "DAT File", "1 KB" },
    };
    for (int i = 0; i < 6; i++) {
        char first[64]; snprintf(first, sizeof first, page ? "%s (copy)" : "%s", names[i][0]);
        LVITEMA it = { 0 }; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = i; it.pszText = first; it.lParam = i;
        int idx = (int)SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
        for (int c = 1; c < 3; c++) { LVITEMA s = { 0 }; s.iSubItem = c; s.pszText = (char *)names[i][c]; SendMessageA(lv, LVM_SETITEMTEXTA, idx, (LPARAM)&s); }
    }
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        tb = CreateWindowExA(0, TOOLBARCLASSNAMEA, NULL, WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST, 0, 0, 0, 0, h, (HMENU)1, NULL, NULL);
        SendMessageA(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        int s0 = (int)SendMessageA(tb, TB_ADDSTRINGA, 0, (LPARAM)"Fill\0Step\0Reset\0");
        TBBUTTON b[4] = { { 0, 101, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, s0 }, { 1, 102, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, s0 + 1 },
                          { 0, 0, 0, BTNS_SEP, { 0 }, 0, 0 }, { 2, 103, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, s0 + 2 } };
        SendMessageA(tb, TB_ADDBUTTONSA, 4, (LPARAM)b);
        sb = CreateWindowExA(0, STATUSCLASSNAMEA, "Ready", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h, (HMENU)2, NULL, NULL);
        tab = CreateWindowExA(0, WC_TABCONTROLA, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, (HMENU)3, NULL, NULL);
        TCITEMA ti = { 0 }; ti.mask = TCIF_TEXT;
        ti.pszText = "Files"; SendMessageA(tab, TCM_INSERTITEMA, 0, (LPARAM)&ti);
        ti.pszText = "Copies"; SendMessageA(tab, TCM_INSERTITEMA, 1, (LPARAM)&ti);
        lv = CreateWindowExA(0, WC_LISTVIEWA, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 10, 10, h, (HMENU)4, NULL, NULL);
        SendMessageA(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        static const char *cols[] = { "Name", "Type", "Size" }; int widths[] = { 220, 170, 80 };
        for (int c = 0; c < 3; c++) { LVCOLUMNA col = { 0 }; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; col.fmt = c == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT; col.cx = widths[c]; col.pszText = (char *)cols[c]; SendMessageA(lv, LVM_INSERTCOLUMNA, c, (LPARAM)&col); }
        fill_list(0);
        pb = CreateWindowExA(0, PROGRESS_CLASSA, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, (HMENU)5, NULL, NULL);
        SendMessageA(pb, PBM_SETRANGE32, 0, 100); SendMessageA(pb, PBM_SETSTEP, 10, 0);
        trk = CreateWindowExA(0, TRACKBAR_CLASSA, NULL, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 0, 0, 10, 10, h, (HMENU)6, NULL, NULL);
        SendMessageA(trk, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "5", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 10, 10, h, (HMENU)7, NULL, NULL);
        ud = CreateWindowExA(0, UPDOWN_CLASSA, NULL, WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT, 0, 0, 10, 10, h, (HMENU)8, NULL, NULL);
        SendMessageA(ud, UDM_SETBUDDY, (WPARAM)edit, 0); SendMessageA(ud, UDM_SETRANGE32, 0, 20); SendMessageA(ud, UDM_SETPOS32, 0, 5);
        label = CreateWindowExA(0, "STATIC", "runs", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, (HMENU)9, NULL, NULL);
        /* settings: the run count in the registry, the last tab in an INI file */
        HKEY k; DWORD n = 0, sz = sizeof n;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\WRLD Samples\\ccdemo", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS) {
            RegQueryValueExA(k, "Runs", NULL, NULL, (BYTE *)&n, &sz); runs = (int)n + 1; n = runs;
            RegSetValueExA(k, "Runs", 0, REG_DWORD, (BYTE *)&n, sizeof n); RegCloseKey(k);
        }
        int last = GetPrivateProfileIntA("ccdemo", "Tab", 0, "ccdemo.ini");
        SendMessageA(tab, TCM_SETCURSEL, last, 0); fill_list(last);
        char msg[96]; snprintf(msg, sizeof msg, "Run %d (registry), tab %d (INI)", runs, last); status(msg);
        SendMessageA(sb, SB_SETTEXTA, 1, (LPARAM)"Common controls");
        return 0;
    }
    case WM_SIZE: layout(h); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 101) { SendMessageA(pb, PBM_SETPOS, 100, 0); status("Filled the progress bar"); }
        if (LOWORD(wp) == 102) { SendMessageA(pb, PBM_STEPIT, 0, 0); status("Stepped"); }
        if (LOWORD(wp) == 103) { SendMessageA(pb, PBM_SETPOS, 0, 0); SendMessageA(trk, TBM_SETPOS, TRUE, 0); status("Reset"); }
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == trk) { char b[64]; int v = (int)SendMessageA(trk, TBM_GETPOS, 0, 0); snprintf(b, sizeof b, "Trackbar: %d", v); SendMessageA(sb, SB_SETTEXTA, 2, (LPARAM)b); SendMessageA(pb, PBM_SETPOS, v, 0); }
        return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->hwndFrom == tab && n->code == TCN_SELCHANGE) {
            int sel = (int)SendMessageA(tab, TCM_GETCURSEL, 0, 0); fill_list(sel);
            char v[8]; snprintf(v, sizeof v, "%d", sel); WritePrivateProfileStringA("ccdemo", "Tab", v, "ccdemo.ini");
            status(sel ? "Tab: Copies" : "Tab: Files");
        }
        if (n->hwndFrom == lv && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *l = (NMLISTVIEW *)lp;
            if (l->uNewState & LVIS_SELECTED) { char t[128]; LVITEMA it = { 0 }; it.pszText = t; it.cchTextMax = sizeof t; SendMessageA(lv, LVM_GETITEMTEXTA, l->iItem, (LPARAM)&it); char b[160]; snprintf(b, sizeof b, "Selected: %s", t); status(b); }
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = "ccdemo";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, "ccdemo", "Common Controls Demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 560, 460, NULL, NULL, inst, NULL);
    ShowWindow(h, show);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}
