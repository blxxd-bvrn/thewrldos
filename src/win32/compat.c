/* More of the Win32 API, for real programs (7-Zip, SQLite, PuTTY's tools and the like):
 * the Unicode ("W") versions of the file, process and locale functions, which forward to
 * the ANSI ones in kernel32.c; synchronisation and time functions; a few of advapi32,
 * user32, oleaut32 and the C runtime. pe.c looks names up here after the main tables,
 * by DLL group (w32_compat_find). Names are converted between UTF-16 and the OS's
 * 8-bit file names (characters past ASCII become '?' or UTF-8). */
#include "w32.h"

#define P w32_cur()
#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)
static inline void set_err(uint32_t e) { w32_proc_t *p = P; if (p) p->last_error = e; }
static void *K(const char *name) { void *fn = NULL; w32_lookup("kernel32.dll", name, &fn); return fn; }

/* UTF-16 -> UTF-8, and back */
static void w2a(const WCHAR *w, char *a, int cap) {
    int o = 0;
    if (!w) { a[0] = 0; return; }
    for (int i = 0; w[i] && o < cap - 4; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        if (c < 0x80) a[o++] = (char)c;
        else if (c < 0x800) { a[o++] = 0xC0 | (c >> 6); a[o++] = 0x80 | (c & 63); }
        else if (c < 0x10000) { a[o++] = 0xE0 | (c >> 12); a[o++] = 0x80 | ((c >> 6) & 63); a[o++] = 0x80 | (c & 63); }
        else { a[o++] = 0xF0 | (c >> 18); a[o++] = 0x80 | ((c >> 12) & 63); a[o++] = 0x80 | ((c >> 6) & 63); a[o++] = 0x80 | (c & 63); }
    }
    a[o] = 0;
}
static int a2w(const char *a, WCHAR *w, int cap) {
    int o = 0;
    for (const uint8_t *s = (const uint8_t *)a; *s && o < cap - 1; ) {
        uint32_t c = *s++;
        if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63); s += 3; }
        else if (c >= 0xE0 && s[0] && s[1]) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
        else if (c >= 0xC0 && s[0]) { c = (c & 31) << 6 | (s[0] & 63); s += 1; }
        if (c >= 0x10000 && o < cap - 2) { c -= 0x10000; w[o++] = 0xD800 + (c >> 10); w[o++] = 0xDC00 + (c & 1023); }
        else w[o++] = (WCHAR)c;
    }
    w[o] = 0;
    return o;
}
static int wlen(const WCHAR *w) { int n = 0; while (w && w[n]) n++; return n; }

/* ---------------------------------------------------------------- files (W) */
typedef BOOL (WINAPI *fn_s_t)(const char *);
typedef BOOL (WINAPI *fn_ss_t)(const char *, const char *);
#define PATHW(var, w) char var[520]; w2a(w, var, sizeof var)
static BOOL WINAPI CreateDirectoryW(const WCHAR *n, void *sa) { PATHW(a, n); return ((BOOL (WINAPI *)(const char *, void *))K("CreateDirectoryA"))(a, sa); }
static BOOL WINAPI DeleteFileW(const WCHAR *n) { PATHW(a, n); return ((fn_s_t)K("DeleteFileA"))(a); }
static BOOL WINAPI RemoveDirectoryW(const WCHAR *n) { PATHW(a, n); return ((fn_s_t)K("RemoveDirectoryA"))(a); }
static BOOL WINAPI MoveFileW(const WCHAR *x, const WCHAR *y) { PATHW(a, x); PATHW(b, y); return ((fn_ss_t)K("MoveFileA"))(a, b); }
static BOOL WINAPI MoveFileExA(const char *a, const char *b, DWORD fl) {
    if (!b) return ((fn_s_t)K("DeleteFileA"))(a);
    if (fl & 1) ((fn_s_t)K("DeleteFileA"))(b);                    /* MOVEFILE_REPLACE_EXISTING */
    return ((fn_ss_t)K("MoveFileA"))(a, b);
}
static BOOL WINAPI MoveFileExW(const WCHAR *x, const WCHAR *y, DWORD fl) { PATHW(a, x); if (!y) return MoveFileExA(a, NULL, fl); PATHW(b, y); return MoveFileExA(a, b, fl); }
static BOOL WINAPI MoveFileWithProgressW(const WCHAR *x, const WCHAR *y, void *cb, void *d, DWORD fl) { return MoveFileExW(x, y, fl); }
static BOOL WINAPI CopyFileW(const WCHAR *x, const WCHAR *y, BOOL f) { PATHW(a, x); PATHW(b, y); return ((BOOL (WINAPI *)(const char *, const char *, BOOL))K("CopyFileA"))(a, b, f); }
static BOOL WINAPI SetFileAttributesA(const char *n, DWORD a) { return ((DWORD (WINAPI *)(const char *))K("GetFileAttributesA"))(n) != 0xFFFFFFFF; }
static BOOL WINAPI SetFileAttributesW(const WCHAR *n, DWORD at) { PATHW(a, n); return SetFileAttributesA(a, at); }
static DWORD WINAPI GetFileAttributesW(const WCHAR *n) { PATHW(a, n); return ((DWORD (WINAPI *)(const char *))K("GetFileAttributesA"))(a); }
typedef struct { DWORD attr; uint32_t c_lo, c_hi, a_lo, a_hi, m_lo, m_hi; DWORD size_hi, size_lo; } WIN32_FILE_ATTRIBUTE_DATA;
static BOOL WINAPI GetFileAttributesExA(const char *n, int lvl, WIN32_FILE_ATTRIBUTE_DATA *d) {
    char p[520];
    if (isalpha(n[0]) && n[1] == ':') strlcpy(p, n, sizeof p); else path_join(p, sizeof p, P->cwd, n);
    path_normalize(p);
    vnode_t *v = vfs_resolve(p, NULL);
    if (!v) { set_err(2); return 0; }
    memset(d, 0, sizeof *d);
    d->attr = v->dir ? 0x10 : 0x80; d->size_lo = v->dir ? 0 : v->size;
    return 1;
}
static BOOL WINAPI GetFileAttributesExW(const WCHAR *n, int lvl, WIN32_FILE_ATTRIBUTE_DATA *d) { PATHW(a, n); return GetFileAttributesExA(a, lvl, d); }
static DWORD WINAPI GetCurrentDirectoryW(DWORD n, WCHAR *buf) {
    char a[300]; DWORD l = ((DWORD (WINAPI *)(DWORD, char *))K("GetCurrentDirectoryA"))(sizeof a, a);
    if (!buf || n <= l) return l + 1;
    return a2w(a, buf, n);
}
static BOOL WINAPI SetCurrentDirectoryW(const WCHAR *d) { PATHW(a, d); return ((fn_s_t)K("SetCurrentDirectoryA"))(a); }
static DWORD WINAPI GetTempPathW(DWORD n, WCHAR *buf) { vfs_mkdir_p("C:\\tmp"); if (n < 8) return 8; return a2w("C:\\tmp\\", buf, n); }
static DWORD WINAPI GetFullPathNameW(const WCHAR *n, DWORD cap, WCHAR *buf, WCHAR **fp) {
    PATHW(a, n); char full[520]; char *f = NULL;
    ((DWORD (WINAPI *)(const char *, DWORD, char *, char **))K("GetFullPathNameA"))(a, sizeof full, full, &f);
    int l = strlen(full);
    if (!buf || (int)cap <= l) return l + 1;
    int wl = a2w(full, buf, cap);
    if (fp) { int k = wl; while (k > 0 && buf[k - 1] != '\\') k--; *fp = buf + k; }
    return wl;
}
static DWORD WINAPI GetLongPathNameW(const WCHAR *s, WCHAR *d, DWORD n) { int l = wlen(s); if (!d || (int)n <= l) return l + 1; for (int i = 0; i <= l; i++) d[i] = s[i]; return l; }
static DWORD WINAPI GetShortPathNameW(const WCHAR *s, WCHAR *d, DWORD n) { return GetLongPathNameW(s, d, n); }
static DWORD WINAPI GetLongPathNameA(const char *s, char *d, DWORD n) { int l = strlen(s); if (!d || (int)n <= l) return l + 1; strcpy(d, s); return l; }

typedef struct __attribute__((packed)) { DWORD attr; uint64_t ctime, atime, mtime; DWORD size_hi, size_lo, r0, r1; char name[260]; char alt[14]; } FINDA;
typedef struct __attribute__((packed)) { DWORD attr; uint64_t ctime, atime, mtime; DWORD size_hi, size_lo, r0, r1; WCHAR name[260]; WCHAR alt[14]; } FINDW;
static void finda2w(const FINDA *a, FINDW *w) {
    memset(w, 0, sizeof *w);
    w->attr = a->attr; w->ctime = a->ctime; w->atime = a->atime; w->mtime = a->mtime; w->size_hi = a->size_hi; w->size_lo = a->size_lo;
    WCHAR name[260]; a2w(a->name, name, 260); memcpy(w->name, name, sizeof name);   /* (the field is unaligned) */
}
static HANDLE WINAPI FindFirstFileW(const WCHAR *pat, FINDW *f) {
    PATHW(a, pat); FINDA fa;
    HANDLE h = ((HANDLE (WINAPI *)(const char *, FINDA *))K("FindFirstFileA"))(a, &fa);
    if (h != INVALID_HANDLE) finda2w(&fa, f);
    return h;
}
static BOOL WINAPI FindNextFileW(HANDLE h, FINDW *f) {
    FINDA fa;
    BOOL r = ((BOOL (WINAPI *)(HANDLE, FINDA *))K("FindNextFileA"))(h, &fa);
    if (r) finda2w(&fa, f);
    return r;
}
static HANDLE WINAPI FindFirstFileExA(const char *pat, int lvl, FINDA *f, int op, void *filt, DWORD fl) { return ((HANDLE (WINAPI *)(const char *, FINDA *))K("FindFirstFileA"))(pat, f); }
static HANDLE WINAPI FindFirstFileExW(const WCHAR *pat, int lvl, FINDW *f, int op, void *filt, DWORD fl) { return FindFirstFileW(pat, f); }

typedef struct { DWORD attr; uint64_t c, a, m; DWORD serial, size_hi, size_lo, nlinks, idx_hi, idx_lo; } BHFI;
static BOOL WINAPI GetFileInformationByHandle(HANDLE h, void *out) {
    int64_t sz = 0;
    if (!((BOOL (WINAPI *)(HANDLE, int64_t *))K("GetFileSizeEx"))(h, &sz)) { set_err(6); return 0; }
    uint8_t *b = out; memset(b, 0, 52);
    DWORD v;
    v = 0x80; memcpy(b, &v, 4);
    v = 0x57524C44; memcpy(b + 28, &v, 4);                       /* volume serial */
    v = (DWORD)(sz >> 32); memcpy(b + 32, &v, 4); v = (DWORD)sz; memcpy(b + 36, &v, 4);
    v = 1; memcpy(b + 40, &v, 4);
    v = (DWORD)h; memcpy(b + 48, &v, 4);                          /* file index: unique enough while open */
    return 1;
}
static BOOL WINAPI SetFileTime(HANDLE h, const void *c, const void *a, const void *m) { return 1; }
static BOOL WINAPI GetFileTime(HANDLE h, uint64_t *c, uint64_t *a, uint64_t *m) {
    uint64_t now; ((void (WINAPI *)(uint64_t *))K("GetSystemTimeAsFileTime"))(&now);
    if (c) *c = now; if (a) *a = now; if (m) *m = now;
    return 1;
}
static BOOL WINAPI LockFileEx(HANDLE h, DWORD f, DWORD r, DWORD lo, DWORD hi, void *ov) { return 1; }
static BOOL WINAPI UnlockFileEx(HANDLE h, DWORD r, DWORD lo, DWORD hi, void *ov) { return 1; }
static BOOL WINAPI LockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { return 1; }
static BOOL WINAPI UnlockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { return 1; }
static BOOL WINAPI GetDiskFreeSpaceExW(const WCHAR *d, uint64_t *a, uint64_t *t, uint64_t *f) {
    char root[520] = "C:\\"; if (d) w2a(d, root, sizeof root);
    return ((BOOL (WINAPI *)(const char *, uint64_t *, uint64_t *, uint64_t *))K("GetDiskFreeSpaceExA"))(root, a, t, f);
}
static BOOL WINAPI GetDiskFreeSpaceA(const char *d, DWORD *spc, DWORD *bps, DWORD *fc, DWORD *tc) {
    uint64_t a = 0, t = 0, f = 0;
    ((BOOL (WINAPI *)(const char *, uint64_t *, uint64_t *, uint64_t *))K("GetDiskFreeSpaceExA"))(d ? d : "C:\\", &a, &t, &f);
    if (spc) *spc = 8; if (bps) *bps = 512; if (fc) *fc = (DWORD)(f / 4096); if (tc) *tc = (DWORD)(t / 4096);
    return 1;
}
static BOOL WINAPI GetDiskFreeSpaceW(const WCHAR *d, DWORD *spc, DWORD *bps, DWORD *fc, DWORD *tc) { char a[520] = "C:\\"; if (d) w2a(d, a, sizeof a); return GetDiskFreeSpaceA(a, spc, bps, fc, tc); }
static UINT WINAPI GetDriveTypeW(const WCHAR *r) { char a[520] = "C:\\"; if (r) w2a(r, a, sizeof a); return ((UINT (WINAPI *)(const char *))K("GetDriveTypeA"))(a); }
static DWORD WINAPI GetLogicalDriveStringsA(DWORD n, char *buf) {
    DWORD mask = ((DWORD (WINAPI *)(void))K("GetLogicalDrives"))(), o = 0;
    for (int i = 0; i < 26; i++) if (mask & (1u << i)) { if (o + 4 < n) { buf[o] = 'A' + i; buf[o + 1] = ':'; buf[o + 2] = '\\'; buf[o + 3] = 0; } o += 4; }
    if (o < n) buf[o] = 0;
    return o;
}
static DWORD WINAPI GetLogicalDriveStringsW(DWORD n, WCHAR *buf) { char a[110]; DWORD l = GetLogicalDriveStringsA(sizeof a, a); if (l + 1 > n) return l + 1; for (DWORD i = 0; i <= l; i++) buf[i] = (uint8_t)a[i]; return l; }
static BOOL WINAPI GetVolumeInformationW(const WCHAR *r, WCHAR *name, DWORD nn, DWORD *ser, DWORD *maxc, DWORD *fl, WCHAR *fs, DWORD fsn) {
    if (name && nn) a2w("WRLD", name, nn); if (ser) *ser = 0x57524C44; if (maxc) *maxc = 255; if (fl) *fl = 3;
    if (fs && fsn) a2w("NTFS", fs, fsn);
    return 1;
}
static BOOL WINAPI CreateHardLinkW(const WCHAR *a, const WCHAR *b, void *sa) { set_err(1); return 0; }
static BOOL WINAPI DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD ni, void *out, DWORD no, DWORD *ret, void *ov) { set_err(1); return 0; }
static HANDLE WINAPI CreateFileMappingA(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const char *name) { set_err(5); return 0; }
static HANDLE WINAPI CreateFileMappingW(HANDLE f, void *sa, DWORD prot, DWORD hi, DWORD lo, const WCHAR *name) { set_err(5); return 0; }
static HANDLE WINAPI OpenFileMappingW(DWORD acc, BOOL inh, const WCHAR *name) { set_err(2); return 0; }
static HANDLE WINAPI OpenFileMappingA(DWORD acc, BOOL inh, const char *name) { set_err(2); return 0; }
static void *WINAPI MapViewOfFile(HANDLE m, DWORD acc, DWORD hi, DWORD lo, SIZE_T n) { set_err(6); return NULL; }
static BOOL WINAPI UnmapViewOfFile(const void *p) { return 1; }
static BOOL WINAPI FlushViewOfFile(const void *p, SIZE_T n) { return 1; }
static BOOL WINAPI SetFileApisToOEM(void) { return 1; }
static BOOL WINAPI SetFileApisToANSI(void) { return 1; }
static BOOL WINAPI AreFileApisANSI(void) { return 1; }
static BOOL WINAPI GetOverlappedResult(HANDLE h, void *ov, DWORD *n, BOOL w) { if (n) *n = 0; set_err(38); return 0; }
static BOOL WINAPI CancelIo(HANDLE h) { return 1; }

/* ---------------------------------------------------------------- processes, threads, sync */
static HANDLE WINAPI CreateEventW(void *sa, BOOL man, BOOL init, const WCHAR *n) { return ((HANDLE (WINAPI *)(void *, BOOL, BOOL, const char *))K("CreateEventA"))(sa, man, init, NULL); }
static HANDLE WINAPI CreateEventExW(void *sa, const WCHAR *n, DWORD fl, DWORD acc) { return CreateEventW(sa, (fl & 1) != 0, (fl & 2) != 0, n); }
static HANDLE WINAPI OpenEventW(DWORD acc, BOOL inh, const WCHAR *n) { set_err(2); return 0; }
static HANDLE WINAPI OpenEventA(DWORD acc, BOOL inh, const char *n) { set_err(2); return 0; }
static HANDLE WINAPI CreateSemaphoreW(void *sa, LONG i, LONG m, const WCHAR *n) { return ((HANDLE (WINAPI *)(void *, LONG, LONG, const char *))K("CreateSemaphoreA"))(sa, i, m, NULL); }
static HANDLE WINAPI CreateMutexW(void *sa, BOOL own, const WCHAR *n) { return ((HANDLE (WINAPI *)(void *, BOOL, const char *))K("CreateMutexA"))(sa, own, NULL); }
static DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL al) { return ((DWORD (WINAPI *)(HANDLE, DWORD))K("WaitForSingleObject"))(h, ms); }
static DWORD WINAPI WaitForMultipleObjectsEx(DWORD n, HANDLE *hs, BOOL all, DWORD ms, BOOL al) { return ((DWORD (WINAPI *)(DWORD, HANDLE *, BOOL, DWORD))K("WaitForMultipleObjects"))(n, hs, all, ms); }
static DWORD WINAPI ResumeThread(HANDLE h) { return 1; }
static DWORD WINAPI SuspendThread(HANDLE h) { return 0; }
static ULONG_PTR WINAPI SetThreadAffinityMask(HANDLE h, ULONG_PTR m) { return 1; }
static BOOL WINAPI GetProcessAffinityMask(HANDLE h, ULONG_PTR *pm, ULONG_PTR *sm) { if (pm) *pm = 1; if (sm) *sm = 1; return 1; }
static BOOL WINAPI SetProcessAffinityMask(HANDLE h, ULONG_PTR m) { return 1; }
static BOOL WINAPI SetThreadPriority(HANDLE h, int p) { return 1; }
static int WINAPI GetThreadPriority(HANDLE h) { return 0; }
static BOOL WINAPI SetPriorityClass(HANDLE h, DWORD c) { return 1; }
static DWORD WINAPI GetPriorityClass(HANDLE h) { return 0x20; }
static BOOL WINAPI GetProcessTimes(HANDLE h, uint64_t *c, uint64_t *e, uint64_t *k, uint64_t *u) {
    if (c) *c = 0; if (e) *e = 0; if (k) *k = 0; if (u) *u = (uint64_t)timer_ticks() * 10000;
    return 1;
}
static BOOL WINAPI GetThreadTimes(HANDLE h, uint64_t *c, uint64_t *e, uint64_t *k, uint64_t *u) { return GetProcessTimes(h, c, e, k, u); }
static void WINAPI ExitThread(DWORD code) { task_exit(code); for (;;) ; }
static void WINAPI FreeLibraryAndExitThread(HMODULE m, DWORD code) { ExitThread(code); }
static BOOL WINAPI IsThreadAFiber(void) { return 0; }
static HANDLE WINAPI OpenProcess(DWORD acc, BOOL inh, DWORD pid) { if (pid == (DWORD)P->pid) return (HANDLE)(intptr_t)-1; set_err(87); return 0; }
static BOOL WINAPI SetHandleInformation(HANDLE h, DWORD m, DWORD f) { return 1; }
static BOOL WINAPI GetHandleInformation(HANDLE h, DWORD *f) { if (f) *f = 0; return 1; }
static BOOL WINAPI CreatePipe(HANDLE *r, HANDLE *w, void *sa, DWORD n) { set_err(1); return 0; }
static BOOL WINAPI PeekNamedPipe(HANDLE h, void *b, DWORD n, DWORD *rd, DWORD *avail, DWORD *left) { if (rd) *rd = 0; if (avail) *avail = 0; if (left) *left = 0; return 1; }
static HANDLE WINAPI CreateNamedPipeA(const char *n, DWORD om, DWORD pm, DWORD mi, DWORD ob, DWORD ib, DWORD to, void *sa) { set_err(1); return INVALID_HANDLE; }
static BOOL WINAPI ConnectNamedPipe(HANDLE h, void *ov) { set_err(1); return 0; }
static BOOL WINAPI WaitNamedPipeA(const char *n, DWORD to) { set_err(2); return 0; }
typedef struct { DWORD cb; void *res; WCHAR *desk, *title; DWORD x, y, xs, ys, xc, yc, fill, flags; WORD show, r2; void *r3; HANDLE in, out, err; } STARTUPINFOW;
static BOOL WINAPI CreateProcessW(const WCHAR *app, WCHAR *cmd, void *a, void *b, BOOL inh, DWORD fl, void *env, const WCHAR *cwd, STARTUPINFOW *si, void *pi) {
    char aa[520], ca[600], da[520];
    if (app) w2a(app, aa, sizeof aa); if (cmd) w2a(cmd, ca, sizeof ca); if (cwd) w2a(cwd, da, sizeof da);
    return ((BOOL (WINAPI *)(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, void *, void *))K("CreateProcessA"))(
        app ? aa : NULL, cmd ? ca : NULL, a, b, inh, fl, env, cwd ? da : NULL, si, pi);
}
static HMODULE WINAPI LoadLibraryExW(const WCHAR *n, HANDLE f, DWORD fl) { char a[260]; w2a(n, a, sizeof a); return ((HMODULE (WINAPI *)(const char *))K("LoadLibraryA"))(a); }
static void WINAPI OutputDebugStringW(const WCHAR *s) { char a[300]; w2a(s, a, sizeof a); klog("w32: debug: %s", a); }
static BOOL WINAPI SetEnvironmentVariableW(const WCHAR *n, const WCHAR *v) {
    char a[200], b[600]; w2a(n, a, sizeof a); if (v) w2a(v, b, sizeof b);
    return ((BOOL (WINAPI *)(const char *, const char *))K("SetEnvironmentVariableA"))(a, v ? b : NULL);
}
static DWORD WINAPI GetEnvironmentVariableW(const WCHAR *n, WCHAR *buf, DWORD cap) {
    char a[200], v[1000]; w2a(n, a, sizeof a);
    DWORD l = ((DWORD (WINAPI *)(const char *, char *, DWORD))K("GetEnvironmentVariableA"))(a, v, sizeof v);
    if (!l || l >= sizeof v) return l;
    if (!buf || cap <= l) return l + 1;
    return a2w(v, buf, cap);
}
static DWORD WINAPI ExpandEnvironmentStringsW(const WCHAR *s, WCHAR *d, DWORD n) { int l = wlen(s); if (!d || (int)n <= l) return l + 1; for (int i = 0; i <= l; i++) d[i] = s[i]; return l + 1; }
static BOOL WINAPI ReadConsoleW(HANDLE h, WCHAR *buf, DWORD n, DWORD *rd, void *c) {
    char tmp[1024]; DWORD k = 0;
    ((BOOL (WINAPI *)(HANDLE, char *, DWORD, DWORD *, void *))K("ReadConsoleA"))(h, tmp, MIN(n, (DWORD)sizeof tmp), &k, c);
    for (DWORD i = 0; i < k; i++) buf[i] = (uint8_t)tmp[i];
    if (rd) *rd = k;
    return 1;
}
static BOOL WINAPI SetConsoleTitleW(const WCHAR *t) { char a[200]; w2a(t, a, sizeof a); return ((BOOL (WINAPI *)(const char *))K("SetConsoleTitleA"))(a); }
static BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, DWORD *n) { if (h != 0x11) { set_err(6); return 0; } if (n) *n = 0; return 1; }   /* the console's input handle only */
static BOOL WINAPI FlushConsoleInputBuffer(HANDLE h) { return 1; }
static DWORD WINAPI GetConsoleProcessList(DWORD *l, DWORD n) { if (n) l[0] = P->pid; return 1; }
static HWND WINAPI GetConsoleWindow(void) { return 0; }
static BOOL WINAPI SetConsoleScreenBufferSize(HANDLE h, uint32_t c) { return 1; }

/* SRW locks (kernel32.c) and condition variables. A condition variable's word counts wakeups:
 * a sleeper waits until it changes. Waking one wakes every sleeper, which Windows allows
 * (spurious wakeups); callers re-check their condition. */
void WINAPI w32_srw_acquire(volatile uintptr_t *l, bool excl);
BOOL WINAPI w32_srw_try(volatile uintptr_t *l, bool excl);
void WINAPI w32_srw_release(volatile uintptr_t *l, bool excl);
static BOOL WINAPI TryAcquireSRWLockExclusive(void *l) { return w32_srw_try(l, true); }
static void WINAPI AcquireSRWLockShared(void *l) { w32_srw_acquire(l, false); }
static void WINAPI ReleaseSRWLockShared(void *l) { w32_srw_release(l, false); }
static BOOL WINAPI TryAcquireSRWLockShared(void *l) { return w32_srw_try(l, false); }
static void WINAPI InitializeConditionVariable(void *c) { *(uintptr_t *)c = 0; }
static void WINAPI WakeConditionVariable(void *c) { __atomic_add_fetch((uintptr_t *)c, 1, __ATOMIC_SEQ_CST); }
static void WINAPI WakeAllConditionVariable(void *c) { __atomic_add_fetch((uintptr_t *)c, 1, __ATOMIC_SEQ_CST); }
/* wait for a wakeup after `seq`; false on timeout */
static bool cv_wait(volatile uintptr_t *c, uintptr_t seq, DWORD ms) {
    uint32_t end = timer_ticks() + ms;
    for (int spins = 0; *c == seq; spins++) {
        if (ms != 0xFFFFFFFF && (int32_t)(timer_ticks() - end) >= 0) return false;
        if (spins < 4) task_yield(); else task_sleep(1);
    }
    return true;
}
static BOOL WINAPI SleepConditionVariableSRW(void *c, void *l, DWORD ms, DWORD f) {
    bool shared = f & 1;                                            /* CONDITION_VARIABLE_LOCKMODE_SHARED */
    uintptr_t seq = *(volatile uintptr_t *)c;
    w32_srw_release(l, !shared);
    bool ok = cv_wait(c, seq, ms);
    w32_srw_acquire(l, !shared);
    if (!ok) { set_err(1460); return 0; }                           /* ERROR_TIMEOUT */
    return 1;
}
static BOOL WINAPI SleepConditionVariableCS(void *c, void *cs, DWORD ms) {
    /* the critical section may be held recursively: release it fully and take it back as deep */
    typedef struct { void *debug; LONG lock_count, recursion; uintptr_t owner, sem, spin; } cs_t;
    cs_t *s = cs; LONG depth = s->recursion;
    uintptr_t seq = *(volatile uintptr_t *)c;
    for (LONG i = 0; i < depth; i++) ((void (WINAPI *)(void *))K("LeaveCriticalSection"))(cs);
    bool ok = cv_wait(c, seq, ms);
    for (LONG i = 0; i < depth; i++) ((void (WINAPI *)(void *))K("EnterCriticalSection"))(cs);
    if (!ok) { set_err(1460); return 0; }
    return 1;
}
static BOOL WINAPI InitializeCriticalSectionEx(void *cs, DWORD spin, DWORD fl) { memset(cs, 0, sizeof(void *) == 8 ? 40 : 24); return 1; }
static BOOL WINAPI InitOnceExecuteOnce(uintptr_t *once, BOOL (WINAPI *fn)(void *, void *, void **), void *param, void **ctx) {
    if (*once == 2) return 1;
    *once = 2;
    return fn(once, param, ctx);
}
static BOOL WINAPI InitOnceBeginInitialize(uintptr_t *once, DWORD fl, BOOL *pending, void **ctx) { *pending = *once != 2; return 1; }
static BOOL WINAPI InitOnceComplete(uintptr_t *once, DWORD fl, void *ctx) { *once = 2; return 1; }
/* SLIST_HEADER: the first pointer is the top of the list */
static void *WINAPI InterlockedPushEntrySList(void **head, void **entry) { void *old = *head; *entry = old; *head = entry; return old; }
static void *WINAPI InterlockedPopEntrySList(void **head) { void **top = *head; if (top) *head = *top; return top; }
static void *WINAPI InterlockedFlushSList(void **head) { void *old = *head; *head = NULL; return old; }
static WORD WINAPI QueryDepthSList(void **head) { WORD n = 0; for (void **e = *head; e; e = *e) n++; return n; }

/* ---------------------------------------------------------------- time */
static void WINAPI GetSystemTimePreciseAsFileTime(uint64_t *ft) { ((void (WINAPI *)(uint64_t *))K("GetSystemTimeAsFileTime"))(ft); }
static LONG WINAPI CompareFileTime(const uint64_t *a, const uint64_t *b) { return *a < *b ? -1 : *a > *b ? 1 : 0; }
static BOOL WINAPI LocalFileTimeToFileTime(const uint64_t *a, uint64_t *b) { *b = *a - (int64_t)tz_offset_minutes * 600000000LL; return 1; }
typedef struct { WORD y, mo, dow, d, h, mi, s, ms; } SYSTEMTIME;
/* days since 1601-01-01 <-> civil dates */
static int64_t days_from_civil(int y, int m, int d) { y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; int yoe = (int)(y - era * 400); int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + doe - 719468; }
static void civil_from_days(int64_t z, int *y, int *m, int *d) { z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; int doe = (int)(z - era * 146097); int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int doy = doe - (365 * yoe + yoe / 4 - yoe / 100); int mp = (5 * doy + 2) / 153; *d = doy - (153 * mp + 2) / 5 + 1; *m = mp + (mp < 10 ? 3 : -9); *y = (int)(yoe + era * 400) + (*m <= 2); }
#define EPOCH_1601_DAYS 134774LL   /* days from 1601-01-01 to 1970-01-01 */
static BOOL WINAPI SystemTimeToFileTime2(const SYSTEMTIME *st, uint64_t *ft) {
    int64_t days = days_from_civil(st->y, st->mo, st->d) + EPOCH_1601_DAYS;
    *ft = (uint64_t)(((days * 86400 + st->h * 3600 + st->mi * 60 + st->s) * 1000 + st->ms) * 10000);
    return 1;
}
static BOOL WINAPI FileTimeToSystemTime2(const uint64_t *ft, SYSTEMTIME *st) {
    uint64_t ms = *ft / 10000, secs = ms / 1000; int64_t days = (int64_t)(secs / 86400) - EPOCH_1601_DAYS;
    int y, m, d; civil_from_days(days, &y, &m, &d);
    uint32_t r = (uint32_t)(secs % 86400);
    st->y = y; st->mo = m; st->d = d; st->h = r / 3600; st->mi = r / 60 % 60; st->s = r % 60; st->ms = ms % 1000;
    st->dow = (WORD)((days + 4) % 7 + 7) % 7;
    return 1;
}
static BOOL WINAPI FileTimeToDosDateTime(const uint64_t *ft, WORD *date, WORD *time) {
    SYSTEMTIME st; FileTimeToSystemTime2(ft, &st);
    if (date) *date = (WORD)(((st.y - 1980) << 9) | (st.mo << 5) | st.d);
    if (time) *time = (WORD)((st.h << 11) | (st.mi << 5) | (st.s / 2));
    return 1;
}
static BOOL WINAPI DosDateTimeToFileTime(WORD date, WORD time, uint64_t *ft) {
    SYSTEMTIME st = { (WORD)((date >> 9) + 1980), (WORD)((date >> 5) & 15), 0, (WORD)(date & 31), (WORD)(time >> 11), (WORD)((time >> 5) & 63), (WORD)((time & 31) * 2), 0 };
    return SystemTimeToFileTime2(&st, ft);
}
static BOOL WINAPI SystemTimeToTzSpecificLocalTime(void *tz, const SYSTEMTIME *u, SYSTEMTIME *l) {
    uint64_t ft; SystemTimeToFileTime2(u, &ft); ft += (int64_t)tz_offset_minutes * 600000000LL; return FileTimeToSystemTime2(&ft, l);
}
static BOOL WINAPI TzSpecificLocalTimeToSystemTime(void *tz, const SYSTEMTIME *l, SYSTEMTIME *u) {
    uint64_t ft; SystemTimeToFileTime2(l, &ft); ft -= (int64_t)tz_offset_minutes * 600000000LL; return FileTimeToSystemTime2(&ft, u);
}
static int fmt_date(const SYSTEMTIME *st, bool time, char *out, int cap) {
    SYSTEMTIME now; if (!st) { ((void (WINAPI *)(SYSTEMTIME *))K("GetLocalTime"))(&now); st = &now; }
    return time ? snprintf(out, cap, "%02u:%02u:%02u", st->h, st->mi, st->s) : snprintf(out, cap, "%u/%u/%04u", st->mo, st->d, st->y);
}
static int WINAPI GetDateFormatW(DWORD lc, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *out, int n) { char a[64]; int l = fmt_date(st, false, a, sizeof a); if (!n) return l + 1; a2w(a, out, n); return l + 1; }
static int WINAPI GetTimeFormatW(DWORD lc, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *out, int n) { char a[64]; int l = fmt_date(st, true, a, sizeof a); if (!n) return l + 1; a2w(a, out, n); return l + 1; }
static int WINAPI GetDateFormatA(DWORD lc, DWORD f, const SYSTEMTIME *st, const char *fmt, char *out, int n) { char a[64]; int l = fmt_date(st, false, a, sizeof a); if (n) strlcpy(out, a, n); return l + 1; }
static int WINAPI GetTimeFormatA(DWORD lc, DWORD f, const SYSTEMTIME *st, const char *fmt, char *out, int n) { char a[64]; int l = fmt_date(st, true, a, sizeof a); if (n) strlcpy(out, a, n); return l + 1; }

/* ---------------------------------------------------------------- locale, strings, messages */
static int WINAPI CompareStringW(DWORD lc, DWORD f, const WCHAR *a, int na, const WCHAR *b, int nb) {
    if (na < 0) na = wlen(a); if (nb < 0) nb = wlen(b);
    for (int i = 0; ; i++) {
        if (i == na || i == nb) return na == nb ? 2 : (i == na ? 1 : 3);
        WCHAR x = a[i], y = b[i];
        if (f & 1) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
        if (x != y) return x < y ? 1 : 3;
    }
}
static int WINAPI CompareStringEx(const WCHAR *ln, DWORD f, const WCHAR *a, int na, const WCHAR *b, int nb, void *v, void *r, LPARAM s) { return CompareStringW(0, f, a, na, b, nb); }
static int WINAPI CompareStringOrdinal(const WCHAR *a, int na, const WCHAR *b, int nb, BOOL ic) { return CompareStringW(0, ic ? 1 : 0, a, na, b, nb); }
static int WINAPI GetLocaleInfoW(DWORD lc, DWORD t, WCHAR *b, int n) {
    char a[16] = ".";
    switch (t & 0xFFFF) { case 0x0E: strcpy(a, "."); break; case 0x0F: strcpy(a, ","); break; case 0x59: strcpy(a, "en"); break; case 0x5A: strcpy(a, "US"); break; case 0x1004: strcpy(a, "1252"); break; case 0x0B: strcpy(a, "437"); break; }
    int l = strlen(a);
    if (!n) return l + 1;
    a2w(a, b, n); return l + 1;
}
static int WINAPI GetLocaleInfoEx(const WCHAR *ln, DWORD t, WCHAR *b, int n) { return GetLocaleInfoW(0x409, t, b, n); }
static BOOL WINAPI IsValidLocale(DWORD lc, DWORD f) { return 1; }
static BOOL WINAPI IsValidLocaleName(const WCHAR *n) { return 1; }
static BOOL WINAPI EnumSystemLocalesW(BOOL (WINAPI *cb)(WCHAR *), DWORD f) { WCHAR s[16]; a2w("00000409", s, 16); cb(s); return 1; }
static DWORD WINAPI GetUserDefaultLangID(void) { return 0x409; }
static DWORD WINAPI GetSystemDefaultLCID(void) { return 0x409; }
static DWORD WINAPI GetSystemDefaultLangID(void) { return 0x409; }
static DWORD WINAPI GetUserDefaultUILanguage(void) { return 0x409; }
static DWORD WINAPI GetThreadLocale(void) { return 0x409; }
static int WINAPI GetUserDefaultLocaleName(WCHAR *b, int n) { return a2w("en-US", b, n) + 1; }
static int WINAPI LCIDToLocaleName(DWORD lc, WCHAR *b, int n, DWORD f) { if (!n) return 6; return a2w("en-US", b, n) + 1; }
static DWORD WINAPI LocaleNameToLCID(const WCHAR *n, DWORD f) { return 0x409; }
static int WINAPI LCMapStringEx(const WCHAR *ln, DWORD f, const WCHAR *s, int n, WCHAR *d, int dn, void *v, void *r, LPARAM h) {
    if (n < 0) n = wlen(s) + 1;
    if (!dn) return n;
    for (int i = 0; i < n && i < dn; i++) { WCHAR c = s[i]; if ((f & 0x200) && c >= 'a' && c <= 'z') c -= 32; if ((f & 0x100) && c >= 'A' && c <= 'Z') c += 32; d[i] = c; }
    return MIN(n, dn);
}
static int WINAPI LCMapStringA(DWORD lc, DWORD f, const char *s, int n, char *d, int dn) {
    if (n < 0) n = strlen(s) + 1;
    if (!dn) return n;
    for (int i = 0; i < n && i < dn; i++) { char c = s[i]; if (f & 0x200) c = toupper(c); if (f & 0x100) c = tolower(c); d[i] = c; }
    return MIN(n, dn);
}
static BOOL WINAPI GetStringTypeA(DWORD lc, DWORD t, const char *s, int n, WORD *o) { if (n < 0) n = strlen(s); for (int i = 0; i < n; i++) o[i] = s[i] == ' ' ? 0x48 : isdigit(s[i]) ? 0x84 : isalpha(s[i]) ? 0x101 : 0x10; return 1; }
static BOOL WINAPI GetStringTypeExW(DWORD lc, DWORD t, const WCHAR *s, int n, WORD *o) { return ((BOOL (WINAPI *)(DWORD, const WCHAR *, int, WORD *))K("GetStringTypeW"))(t, s, n, o); }
static const char *err_text(DWORD id) {
    switch (id) {
    case 0: return "The operation completed successfully.";
    case 1: return "Incorrect function.";
    case 2: return "The system cannot find the file specified.";
    case 3: return "The system cannot find the path specified.";
    case 5: return "Access is denied.";
    case 6: return "The handle is invalid.";
    case 8: return "Not enough memory resources are available to process this command.";
    case 32: return "The process cannot access the file because it is being used by another process.";
    case 80: return "The file exists.";
    case 87: return "The parameter is incorrect.";
    case 112: return "There is not enough space on the disk.";
    case 183: return "Cannot create a file when that file already exists.";
    default: return NULL;
    }
}
static DWORD WINAPI FormatMessageW(DWORD f, const void *src, DWORD id, DWORD lang, WCHAR *buf, DWORD n, void *args) {
    char a[200]; const char *t = (f & 0x1000) ? err_text(id) : NULL;          /* FORMAT_MESSAGE_FROM_SYSTEM */
    if (t) snprintf(a, sizeof a, "%s\r\n", t); else snprintf(a, sizeof a, "Error %u\r\n", id);
    if (f & 0x200) a[strlen(a) - 2] = 0;                                        /* (ignore inserts: nothing to insert) */
    int l = strlen(a);
    if (f & 0x100) { WCHAR **pb = (WCHAR **)buf; *pb = w32_alloc(P, (l + 1) * 2); buf = *pb; n = l + 1; }   /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
    if (!buf || n == 0) return 0;
    return a2w(a, buf, n);
}
typedef struct { DWORD size, major, minor, build, platform; WCHAR csd[128]; WORD spmaj, spmin, suite; BYTE type, res; } OSVERSIONINFOEXW;
static BOOL WINAPI GetVersionExW(OSVERSIONINFOEXW *v) {
    DWORD sz = v->size; memset((uint8_t *)v + 4, 0, sz - 4);
    v->major = 6; v->minor = 1; v->build = 7601; v->platform = 2;                /* report Windows 7 */
    return 1;
}
static BOOL WINAPI GetComputerNameW(WCHAR *b, DWORD *n) { int l = a2w("WRLD", b, *n); *n = l; return 1; }
static BOOL WINAPI GetComputerNameExW(int t, WCHAR *b, DWORD *n) { return GetComputerNameW(b, n); }
static UINT WINAPI GetWindowsDirectoryW(WCHAR *b, UINT n) { return a2w("C:\\WRLD", b, n); }
static UINT WINAPI GetSystemDirectoryW(WCHAR *b, UINT n) { return a2w("C:\\WRLD\\System32", b, n); }
static UINT WINAPI GetSystemWindowsDirectoryW(WCHAR *b, UINT n) { return a2w("C:\\WRLD", b, n); }
static UINT WINAPI GetTempFileNameW(const WCHAR *dir, const WCHAR *pre, UINT u, WCHAR *out) {
    char d[260], p[16], r[300]; w2a(dir, d, sizeof d); w2a(pre, p, 4);
    static UINT seq = 1;
    if (!u) u = (timer_ticks() + seq++) & 0xFFFF;
    snprintf(r, sizeof r, "%s%s%s%04X.tmp", d, d[0] && d[strlen(d) - 1] != '\\' ? "\\" : "", p, u);
    a2w(r, out, 260);
    return u;
}
static UINT WINAPI GetTempFileNameA(const char *d, const char *p, UINT u, char *out) {
    static UINT seq = 1;
    if (!u) u = (timer_ticks() + seq++) & 0xFFFF;
    snprintf(out, 260, "%s%s%.3s%04X.tmp", d, d[0] && d[strlen(d) - 1] != '\\' ? "\\" : "", p, u);
    return u;
}
/* resources of the program's own image (other modules have none: they are built in) */
static HANDLE WINAPI FindResourceW(HMODULE m, const WCHAR *n, const WCHAR *t) { HANDLE r = w32_find_resource_in(P, m, (uintptr_t)t, (uintptr_t)n, true, 0); if (!r) set_err(1814); return r; }
static HANDLE WINAPI FindResourceExW(HMODULE m, const WCHAR *t, const WCHAR *n, WORD l) { HANDLE r = w32_find_resource_in(P, m, (uintptr_t)t, (uintptr_t)n, true, l); if (!r) set_err(1814); return r; }
static HANDLE WINAPI FindResourceA2(HMODULE m, const char *n, const char *t) { HANDLE r = w32_find_resource_in(P, m, (uintptr_t)t, (uintptr_t)n, false, 0); if (!r) set_err(1814); return r; }
static HANDLE WINAPI FindResourceExA(HMODULE m, const char *t, const char *n, WORD l) { HANDLE r = w32_find_resource_in(P, m, (uintptr_t)t, (uintptr_t)n, false, l); if (!r) set_err(1814); return r; }
static HANDLE WINAPI LoadResource(HMODULE m, HANDLE r) { return (HANDLE)w32_resource_data(P, r, NULL); }
static void *WINAPI LockResource(HANDLE r) { return (void *)r; }
static BOOL WINAPI FreeResource(HANDLE r) { return 0; }
static DWORD WINAPI SizeofResource(HMODULE m, HANDLE r) { uint32_t n = 0; w32_resource_data(P, r, &n); return n; }
static SIZE_T WINAPI HeapCompact(HANDLE h, DWORD f) { return 1 << 20; }
static BOOL WINAPI HeapSetInformation(HANDLE h, int c, void *i, SIZE_T n) { return 1; }
static BOOL WINAPI GetCommState(HANDLE h, void *d) { set_err(1); return 0; }
static BOOL WINAPI SetCommState(HANDLE h, void *d) { set_err(1); return 0; }
static BOOL WINAPI SetCommTimeouts(HANDLE h, void *t) { set_err(1); return 0; }
static BOOL WINAPI SetCommBreak(HANDLE h) { set_err(1); return 0; }
static BOOL WINAPI ClearCommBreak(HANDLE h) { set_err(1); return 0; }
static BOOL WINAPI QueryFullProcessImageNameW(HANDLE h, DWORD f, WCHAR *b, DWORD *n) { int l = a2w(P->path, b, *n); *n = l; return 1; }
static DWORD WINAPI GetModuleFileNameExW(HANDLE h, HMODULE m, WCHAR *b, DWORD n) { return a2w(P->path, b, n); }
static BOOL WINAPI IsWow64Process(HANDLE h, BOOL *w) { *w = 0; return 1; }
static UINT WINAPI SetErrorModeW(UINT m) { return 0; }
static BOOL WINAPI SetDllDirectoryW(const WCHAR *d) { return 1; }
static BOOL WINAPI SetDefaultDllDirectories(DWORD f) { return 1; }
static BOOL WINAPI SetProcessDEPPolicy(DWORD f) { return 1; }

/* x64 exception tables: the subsystem has none to walk, so code that looks for a handler finds a
 * leaf function, and an exception that must unwind ends the program with a message */
static void *WINAPI RtlLookupFunctionEntry(uintptr_t pc, uintptr_t *base, void *hist) { if (base) *base = P->base; return NULL; }
static void *WINAPI RtlPcToFileHeader(uintptr_t pc, uintptr_t *base) { w32_mod_t *m = w32_mod_at(P, pc); *base = m ? m->base : 0; return (void *)*base; }
static void *WINAPI RtlVirtualUnwind(DWORD t, uintptr_t base, uintptr_t pc, void *fe, void *ctx, void **hd, uintptr_t *est, void *cp) { if (hd) *hd = NULL; return NULL; }
static void fatal_exception(const char *what) {
    w32_proc_t *p = P;
    klog("w32: pid %d: %s", p ? p->pid : -1, what);
    if (p) w32_console_write(p, 2, what, strlen(what));
    w32_exit_process(3);
}
static void WINAPI RtlUnwindEx(void *frame, void *target, void *rec, void *ret, void *ctx, void *hist) { fatal_exception("\nThe program raised an exception the WRLD Win32 subsystem cannot unwind yet.\n"); }

/* ---------------------------------------------------------------- advapi32 */
static BOOL WINAPI OpenProcessToken(HANDLE p, DWORD acc, HANDLE *t) { *t = 0x7E57; return 1; }
static BOOL WINAPI OpenThreadToken(HANDLE th, DWORD acc, BOOL self, HANDLE *t) { set_err(1008); return 0; }
static BOOL WINAPI GetTokenInformation(HANDLE t, int cls, void *b, DWORD n, DWORD *ret) { if (ret) *ret = 0; set_err(87); return 0; }
static BOOL WINAPI AdjustTokenPrivileges(HANDLE t, BOOL d, void *ns, DWORD n, void *ps, DWORD *r) { set_err(0); return 1; }
static BOOL WINAPI LookupPrivilegeValueW(const WCHAR *s, const WCHAR *n, void *luid) { memset(luid, 0, 8); return 1; }
static BOOL WINAPI LookupPrivilegeValueA(const char *s, const char *n, void *luid) { memset(luid, 0, 8); return 1; }
static BOOL WINAPI GetFileSecurityW(const WCHAR *f, DWORD what, void *sd, DWORD n, DWORD *need) { if (need) *need = 0; set_err(1); return 0; }
static BOOL WINAPI SetFileSecurityW(const WCHAR *f, DWORD what, void *sd) { return 1; }
static LONG WINAPI RegOpenKeyExW(HANDLE k, const WCHAR *s, DWORD o, DWORD sam, HANDLE *r) { return 2; }
static LONG WINAPI RegQueryValueExW(HANDLE k, const WCHAR *n, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) { return 2; }
static LONG WINAPI RegCreateKeyExW(HANDLE k, const WCHAR *s, DWORD r, WCHAR *c, DWORD o, DWORD sam, void *sa, HANDLE *res, DWORD *d) { if (res) *res = 0x1234; if (d) *d = 1; return 0; }
static LONG WINAPI RegSetValueExW(HANDLE k, const WCHAR *n, DWORD r, DWORD t, const BYTE *d, DWORD sz) { return 0; }
static LONG WINAPI RegDeleteValueA(HANDLE k, const char *n) { return 2; }
static LONG WINAPI RegDeleteValueW(HANDLE k, const WCHAR *n) { return 2; }
static LONG WINAPI RegDeleteKeyA(HANDLE k, const char *n) { return 2; }
static LONG WINAPI RegDeleteKeyW(HANDLE k, const WCHAR *n) { return 2; }
static LONG WINAPI RegEnumKeyA(HANDLE k, DWORD i, char *n, DWORD c) { return 259; }
static LONG WINAPI RegEnumKeyExA(HANDLE k, DWORD i, char *n, DWORD *c, DWORD *r, char *cl, DWORD *cc, void *ft) { return 259; }
static LONG WINAPI RegEnumValueA(HANDLE k, DWORD i, char *n, DWORD *c, DWORD *r, DWORD *t, BYTE *d, DWORD *dc) { return 259; }
static LONG WINAPI RegOpenKeyA(HANDLE k, const char *s, HANDLE *r) { return 2; }
static LONG WINAPI RegCreateKeyA(HANDLE k, const char *s, HANDLE *r) { if (r) *r = 0x1234; return 0; }
static LONG WINAPI RegFlushKey(HANDLE k) { return 0; }
static BOOL WINAPI GetUserNameW(WCHAR *b, DWORD *n) { int l = a2w(user_name[0] ? user_name : "User", b, *n); *n = l + 1; return 1; }
static BOOL WINAPI AllocateAndInitializeSid(void *auth, BYTE n, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, void **sid) {
    uint8_t *s = w32_alloc(P, 8 + 4 * 8); memset(s, 0, 40);
    s[0] = 1; s[1] = n; if (auth) memcpy(s + 2, auth, 6);
    DWORD sub[8] = { a0, a1, a2, a3, a4, a5, a6, a7 }; memcpy(s + 8, sub, 4 * n);
    *sid = s; return 1;
}
static void *WINAPI FreeSid(void *s) { w32_free(P, s); return NULL; }
static DWORD WINAPI GetLengthSid(const uint8_t *s) { return 8 + 4 * (s ? s[1] : 0); }
static BOOL WINAPI CopySid(DWORD n, void *d, const uint8_t *s) { DWORD l = GetLengthSid(s); if (n < l) { set_err(122); return 0; } memcpy(d, s, l); return 1; }
static BOOL WINAPI EqualSid(const uint8_t *a, const uint8_t *b) { DWORD l = GetLengthSid(a); return l == GetLengthSid(b) && !memcmp(a, b, l); }
static BOOL WINAPI IsValidSid(const uint8_t *s) { return s && s[0] == 1; }
static BOOL WINAPI InitializeSecurityDescriptor(void *sd, DWORD rev) { memset(sd, 0, sizeof(void *) == 8 ? 40 : 20); *(uint8_t *)sd = 1; return 1; }
static BOOL WINAPI SetSecurityDescriptorDacl(void *sd, BOOL p, void *acl, BOOL d) { return 1; }
static BOOL WINAPI SetSecurityDescriptorOwner(void *sd, void *sid, BOOL d) { return 1; }
static BOOL WINAPI SetSecurityDescriptorGroup(void *sd, void *sid, BOOL d) { return 1; }
static BOOL WINAPI InitializeAcl(void *acl, DWORD n, DWORD rev) { memset(acl, 0, MIN(n, 8u)); return 1; }
static BOOL WINAPI AddAccessAllowedAce(void *acl, DWORD rev, DWORD mask, void *sid) { return 1; }
static DWORD WINAPI GetSecurityInfo(HANDLE h, int t, DWORD si, void **o, void **g, void **d, void **s, void **sd) { if (sd) *sd = NULL; return 1; }
static DWORD WINAPI SetEntriesInAclA(DWORD n, void *e, void *old, void **acl) { *acl = NULL; return 0; }

/* ---------------------------------------------------------------- user32 */
static WCHAR *WINAPI CharUpperW(WCHAR *s) {
    if ((uintptr_t)s < 0x10000) { WCHAR c = (WCHAR)(uintptr_t)s; return (WCHAR *)(uintptr_t)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    for (WCHAR *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    return s;
}
static WCHAR *WINAPI CharLowerW(WCHAR *s) {
    if ((uintptr_t)s < 0x10000) { WCHAR c = (WCHAR)(uintptr_t)s; return (WCHAR *)(uintptr_t)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
    for (WCHAR *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    return s;
}
static HWND WINAPI GetClipboardOwner(void) { return 0; }
static DWORD WINAPI GetQueueStatus(UINT f) { return 0; }

/* ---------------------------------------------------------------- oleaut32: BSTRs and VARIANTs */
static WCHAR *WINAPI SysAllocStringLen(const WCHAR *s, UINT n) {
    uint8_t *b = w32_alloc(P, 4 + n * 2 + 2);
    if (!b) return NULL;
    *(uint32_t *)b = n * 2;
    WCHAR *w = (WCHAR *)(b + 4);
    if (s) memcpy(w, s, n * 2); else memset(w, 0, n * 2);
    w[n] = 0;
    return w;
}
static WCHAR *WINAPI SysAllocString(const WCHAR *s) { return s ? SysAllocStringLen(s, wlen(s)) : NULL; }
static void WINAPI SysFreeString(WCHAR *s) { if (s) w32_free(P, (uint8_t *)s - 4); }
static UINT WINAPI SysStringLen(const WCHAR *s) { return s ? *(const uint32_t *)((const uint8_t *)s - 4) / 2 : 0; }
static UINT WINAPI SysStringByteLen(const WCHAR *s) { return s ? *(const uint32_t *)((const uint8_t *)s - 4) : 0; }
static int WINAPI SysReAllocStringLen(WCHAR **p, const WCHAR *s, UINT n) { WCHAR *nw = SysAllocStringLen(s, n); if (!nw) return 0; SysFreeString(*p); *p = nw; return 1; }
/* VARIANT: vt at 0, the value at 8 (both architectures) */
static int32_t WINAPI VariantClear(uint8_t *v) { if (*(WORD *)v == 8) SysFreeString(*(WCHAR **)(v + 8)); memset(v, 0, 8 + sizeof(void *) * 2); return 0; }
static void WINAPI VariantInit(uint8_t *v) { memset(v, 0, 8 + sizeof(void *) * 2); }
static int32_t WINAPI VariantCopy(uint8_t *d, const uint8_t *s) {
    if (d != s) { VariantClear(d); memcpy(d, s, 8 + sizeof(void *) * 2); }
    if (*(const WORD *)s == 8) *(WCHAR **)(d + 8) = SysAllocString(*(WCHAR *const *)(s + 8));
    return 0;
}

/* ---------------------------------------------------------------- C runtime */
static int CDECL wcscmp_(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
static int CDECL wcsncmp_(const WCHAR *a, const WCHAR *b, SIZE_T n) { for (SIZE_T i = 0; i < n; i++) { if (a[i] != b[i] || !a[i]) return (int)a[i] - (int)b[i]; } return 0; }
static WCHAR *CDECL wcsstr_(const WCHAR *h, const WCHAR *n) {
    int nl = wlen(n);
    for (; *h; h++) { int i = 0; while (i < nl && h[i] == n[i]) i++; if (i == nl) return (WCHAR *)h; }
    return nl ? NULL : (WCHAR *)h;
}
static WCHAR *CDECL wcschr_(const WCHAR *s, WCHAR c) { for (; ; s++) { if (*s == c) return (WCHAR *)s; if (!*s) return NULL; } }
static WCHAR *CDECL wcsrchr_(const WCHAR *s, WCHAR c) { const WCHAR *r = NULL; for (; ; s++) { if (*s == c) r = s; if (!*s) return (WCHAR *)r; } }
static WCHAR *CDECL wcscpy_(WCHAR *d, const WCHAR *s) { WCHAR *r = d; while ((*d++ = *s++)) ; return r; }
static WCHAR *CDECL wcscat_(WCHAR *d, const WCHAR *s) { wcscpy_(d + wlen(d), s); return d; }
static WCHAR *CDECL wcsncpy_(WCHAR *d, const WCHAR *s, SIZE_T n) { SIZE_T i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static int CDECL _wcsicmp_(const WCHAR *a, const WCHAR *b) {
    for (;; a++, b++) { WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b; if (x != y || !x) return (int)x - (int)y; }
}
static int CDECL towupper_(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int CDECL towlower_(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int CDECL iswspace_(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int CDECL iswdigit_(int c) { return c >= '0' && c <= '9'; }
static int CDECL iswalpha_(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0; }
static intptr_t CDECL _get_osfhandle_(int fd) { return fd == 0 ? 0x11 : fd == 1 ? 0x12 : fd == 2 ? 0x13 : -1; }
static int CDECL _purecall_(void) { fatal_exception("\nA pure virtual function was called.\n"); return 0; }
static void CDECL terminate_(void) { fatal_exception("\nThe program called terminate().\n"); }
static void CDECL CxxThrowException_(void *obj, void *info) { fatal_exception("\nThe program threw a C++ exception, which the WRLD Win32 subsystem cannot catch yet.\n"); }
static int CDECL CxxFrameHandler_(void *rec, void *frame, void *ctx, void *dc) { return 1; }   /* ExceptionContinueSearch */
static void CDECL type_info_dtor(void) {}
static int adjust_fdiv = 0;
static char **initenv_ptr;
static char ***CDECL p_initenv(void) { initenv_ptr = P->envp; return &initenv_ptr; }

/* ---------------------------------------------------------------- the table */
#define C(group, n) { group, #n, (void *)n }
#define CA(group, name, fn) { group, name, (void *)(fn) }
typedef struct { const char *group, *name; void *fn; } compat_t;
static const compat_t compat[] = {
    /* kernel32 */
    C("kernel32", CreateDirectoryW), C("kernel32", DeleteFileW), C("kernel32", RemoveDirectoryW), C("kernel32", MoveFileW), C("kernel32", MoveFileExA),
    C("kernel32", MoveFileExW), C("kernel32", MoveFileWithProgressW), C("kernel32", CopyFileW), C("kernel32", SetFileAttributesA), C("kernel32", SetFileAttributesW),
    C("kernel32", GetFileAttributesW), C("kernel32", GetFileAttributesExA), C("kernel32", GetFileAttributesExW), C("kernel32", GetCurrentDirectoryW),
    C("kernel32", SetCurrentDirectoryW), C("kernel32", GetTempPathW), C("kernel32", GetFullPathNameW), C("kernel32", GetLongPathNameW), C("kernel32", GetLongPathNameA),
    C("kernel32", GetShortPathNameW), C("kernel32", FindFirstFileW), C("kernel32", FindNextFileW), C("kernel32", FindFirstFileExA), C("kernel32", FindFirstFileExW),
    C("kernel32", GetFileInformationByHandle), C("kernel32", SetFileTime), C("kernel32", GetFileTime), C("kernel32", LockFileEx), C("kernel32", UnlockFileEx),
    C("kernel32", LockFile), C("kernel32", UnlockFile), C("kernel32", GetDiskFreeSpaceExW), C("kernel32", GetDiskFreeSpaceA), C("kernel32", GetDiskFreeSpaceW),
    C("kernel32", GetDriveTypeW), C("kernel32", GetLogicalDriveStringsA), C("kernel32", GetLogicalDriveStringsW), C("kernel32", GetVolumeInformationW),
    C("kernel32", CreateHardLinkW), C("kernel32", DeviceIoControl), C("kernel32", CreateFileMappingA), C("kernel32", CreateFileMappingW), C("kernel32", OpenFileMappingW),
    C("kernel32", OpenFileMappingA), C("kernel32", MapViewOfFile), C("kernel32", UnmapViewOfFile), C("kernel32", FlushViewOfFile), C("kernel32", SetFileApisToOEM),
    C("kernel32", SetFileApisToANSI), C("kernel32", AreFileApisANSI), C("kernel32", GetOverlappedResult), C("kernel32", CancelIo),
    C("kernel32", CreateEventW), C("kernel32", CreateEventExW), C("kernel32", OpenEventW), C("kernel32", OpenEventA), C("kernel32", CreateSemaphoreW), C("kernel32", CreateMutexW),
    C("kernel32", WaitForSingleObjectEx), C("kernel32", WaitForMultipleObjectsEx), C("kernel32", ResumeThread), C("kernel32", SuspendThread), C("kernel32", SetThreadAffinityMask),
    C("kernel32", GetProcessAffinityMask), C("kernel32", SetProcessAffinityMask), C("kernel32", SetThreadPriority), C("kernel32", GetThreadPriority),
    C("kernel32", SetPriorityClass), C("kernel32", GetPriorityClass), C("kernel32", GetProcessTimes), C("kernel32", GetThreadTimes), C("kernel32", ExitThread),
    C("kernel32", FreeLibraryAndExitThread), C("kernel32", IsThreadAFiber), C("kernel32", OpenProcess), C("kernel32", SetHandleInformation), C("kernel32", GetHandleInformation),
    C("kernel32", CreatePipe), C("kernel32", PeekNamedPipe), C("kernel32", CreateNamedPipeA), C("kernel32", ConnectNamedPipe), C("kernel32", WaitNamedPipeA),
    C("kernel32", CreateProcessW), C("kernel32", LoadLibraryExW), C("kernel32", OutputDebugStringW), C("kernel32", SetEnvironmentVariableW),
    C("kernel32", GetEnvironmentVariableW), C("kernel32", ExpandEnvironmentStringsW), C("kernel32", ReadConsoleW), C("kernel32", SetConsoleTitleW),
    C("kernel32", GetNumberOfConsoleInputEvents), C("kernel32", FlushConsoleInputBuffer), C("kernel32", GetConsoleProcessList), C("kernel32", GetConsoleWindow),
    C("kernel32", SetConsoleScreenBufferSize),
    C("kernel32", TryAcquireSRWLockExclusive), C("kernel32", AcquireSRWLockShared), C("kernel32", ReleaseSRWLockShared), C("kernel32", TryAcquireSRWLockShared),
    C("kernel32", InitializeConditionVariable), C("kernel32", WakeConditionVariable), C("kernel32", WakeAllConditionVariable), C("kernel32", SleepConditionVariableSRW),
    C("kernel32", SleepConditionVariableCS), C("kernel32", InitializeCriticalSectionEx), C("kernel32", InitOnceExecuteOnce), C("kernel32", InitOnceBeginInitialize),
    C("kernel32", InitOnceComplete), C("kernel32", InterlockedPushEntrySList), C("kernel32", InterlockedPopEntrySList), C("kernel32", InterlockedFlushSList),
    C("kernel32", QueryDepthSList), CA("kernel32", "RtlInterlockedFlushSList", InterlockedFlushSList), CA("kernel32", "RtlInterlockedPushEntrySList", InterlockedPushEntrySList),
    C("kernel32", GetSystemTimePreciseAsFileTime), C("kernel32", CompareFileTime), C("kernel32", LocalFileTimeToFileTime), C("kernel32", FileTimeToDosDateTime),
    C("kernel32", DosDateTimeToFileTime), C("kernel32", SystemTimeToTzSpecificLocalTime), C("kernel32", TzSpecificLocalTimeToSystemTime),
    C("kernel32", GetDateFormatW), C("kernel32", GetTimeFormatW), C("kernel32", GetDateFormatA), C("kernel32", GetTimeFormatA),
    C("kernel32", CompareStringW), C("kernel32", CompareStringEx), C("kernel32", CompareStringOrdinal), C("kernel32", GetLocaleInfoW), C("kernel32", GetLocaleInfoEx),
    C("kernel32", IsValidLocale), C("kernel32", IsValidLocaleName), C("kernel32", EnumSystemLocalesW), C("kernel32", GetUserDefaultLangID), C("kernel32", GetSystemDefaultLCID),
    C("kernel32", GetSystemDefaultLangID), C("kernel32", GetUserDefaultUILanguage), C("kernel32", GetThreadLocale), C("kernel32", GetUserDefaultLocaleName),
    C("kernel32", LCIDToLocaleName), C("kernel32", LocaleNameToLCID), C("kernel32", LCMapStringEx), C("kernel32", LCMapStringA), C("kernel32", GetStringTypeA),
    C("kernel32", GetStringTypeExW), C("kernel32", FormatMessageW), C("kernel32", GetVersionExW), C("kernel32", GetComputerNameW), C("kernel32", GetComputerNameExW),
    C("kernel32", GetWindowsDirectoryW), C("kernel32", GetSystemDirectoryW), C("kernel32", GetSystemWindowsDirectoryW), C("kernel32", GetTempFileNameW),
    C("kernel32", GetTempFileNameA), C("kernel32", FindResourceW), C("kernel32", FindResourceExW), CA("kernel32", "FindResourceA", FindResourceA2), C("kernel32", FindResourceExA), C("kernel32", FreeResource), C("kernel32", LoadResource), C("kernel32", LockResource),
    C("kernel32", SizeofResource), C("kernel32", HeapCompact), C("kernel32", HeapSetInformation), C("kernel32", GetCommState), C("kernel32", SetCommState),
    C("kernel32", SetCommTimeouts), C("kernel32", SetCommBreak), C("kernel32", ClearCommBreak), C("kernel32", QueryFullProcessImageNameW),
    CA("kernel32", "K32GetModuleFileNameExW", GetModuleFileNameExW), C("kernel32", IsWow64Process), C("kernel32", SetDllDirectoryW), C("kernel32", SetDefaultDllDirectories),
    C("kernel32", SetProcessDEPPolicy), C("kernel32", RtlLookupFunctionEntry), C("kernel32", RtlPcToFileHeader), C("kernel32", RtlVirtualUnwind), C("kernel32", RtlUnwindEx),
    /* advapi32 */
    C("advapi32", OpenProcessToken), C("advapi32", OpenThreadToken), C("advapi32", GetTokenInformation), C("advapi32", AdjustTokenPrivileges),
    C("advapi32", LookupPrivilegeValueW), C("advapi32", LookupPrivilegeValueA), C("advapi32", GetFileSecurityW), C("advapi32", SetFileSecurityW),
    C("advapi32", RegOpenKeyExW), C("advapi32", RegQueryValueExW), C("advapi32", RegCreateKeyExW), C("advapi32", RegSetValueExW), C("advapi32", RegDeleteValueA),
    C("advapi32", RegDeleteValueW), C("advapi32", RegDeleteKeyA), C("advapi32", RegDeleteKeyW), C("advapi32", RegEnumKeyA), C("advapi32", RegEnumKeyExA),
    C("advapi32", RegEnumValueA), C("advapi32", RegOpenKeyA), C("advapi32", RegCreateKeyA), C("advapi32", RegFlushKey), C("advapi32", GetUserNameW),
    C("advapi32", AllocateAndInitializeSid), C("advapi32", FreeSid), C("advapi32", GetLengthSid), C("advapi32", CopySid), C("advapi32", EqualSid), C("advapi32", IsValidSid),
    C("advapi32", InitializeSecurityDescriptor), C("advapi32", SetSecurityDescriptorDacl), C("advapi32", SetSecurityDescriptorOwner), C("advapi32", SetSecurityDescriptorGroup),
    C("advapi32", InitializeAcl), C("advapi32", AddAccessAllowedAce), C("advapi32", GetSecurityInfo), C("advapi32", SetEntriesInAclA),
    /* user32 */
    C("user32", CharUpperW), C("user32", CharLowerW), C("user32", GetClipboardOwner), C("user32", GetQueueStatus),
    /* oleaut32 (imported by ordinal) */
    CA("oleaut32", "Ordinal2", SysAllocString), CA("oleaut32", "Ordinal4", SysAllocStringLen), CA("oleaut32", "Ordinal5", SysReAllocStringLen),
    CA("oleaut32", "Ordinal6", SysFreeString), CA("oleaut32", "Ordinal7", SysStringLen), CA("oleaut32", "Ordinal8", VariantInit),
    CA("oleaut32", "Ordinal9", VariantClear), CA("oleaut32", "Ordinal10", VariantCopy), CA("oleaut32", "Ordinal149", SysStringByteLen),
    C("oleaut32", SysAllocString), C("oleaut32", SysAllocStringLen), C("oleaut32", SysFreeString), C("oleaut32", SysStringLen), C("oleaut32", SysStringByteLen),
    C("oleaut32", VariantInit), C("oleaut32", VariantClear), C("oleaut32", VariantCopy),
    /* C runtime */
    CA("msvcrt", "wcscmp", wcscmp_), CA("msvcrt", "wcsncmp", wcsncmp_), CA("msvcrt", "wcsstr", wcsstr_), CA("msvcrt", "wcschr", wcschr_), CA("msvcrt", "wcsrchr", wcsrchr_),
    CA("msvcrt", "wcscpy", wcscpy_), CA("msvcrt", "wcscat", wcscat_), CA("msvcrt", "wcsncpy", wcsncpy_), CA("msvcrt", "_wcsicmp", _wcsicmp_),
    CA("msvcrt", "towupper", towupper_), CA("msvcrt", "towlower", towlower_), CA("msvcrt", "iswspace", iswspace_), CA("msvcrt", "iswdigit", iswdigit_),
    CA("msvcrt", "iswalpha", iswalpha_), CA("msvcrt", "_get_osfhandle", _get_osfhandle_), CA("msvcrt", "_purecall", _purecall_),
    CA("msvcrt", "?terminate@@YAXXZ", terminate_), CA("msvcrt", "_CxxThrowException", CxxThrowException_), CA("msvcrt", "__CxxFrameHandler", CxxFrameHandler_),
    CA("msvcrt", "__CxxFrameHandler3", CxxFrameHandler_), CA("msvcrt", "??1type_info@@UAE@XZ", type_info_dtor), CA("msvcrt", "??1type_info@@UEAA@XZ", type_info_dtor),
    CA("msvcrt", "_adjust_fdiv", &adjust_fdiv), CA("msvcrt", "__p___initenv", p_initenv),
};

/* the group of a DLL name: its own name without ".dll", with the C runtime's and kernel's many names folded */
void w32_dll_group(const char *dll, char *g, int cap) {
    char b[64]; strlcpy(b, dll, sizeof b);
    for (char *c = b; *c; c++) *c = tolower(*c);
    char *dot = strstr(b, ".dll"); if (dot) *dot = 0;
    if (!strncmp(b, "api-ms-win-crt-", 15) || !strcmp(b, "ucrtbase") || !strncmp(b, "vcruntime", 9) || (!strncmp(b, "msvcp", 5) && strcmp(b, "msvcp60")) || !strncmp(b, "msvcr", 5) || !strcmp(b, "crtdll")) strlcpy(g, "msvcrt", cap);
    else if (!strncmp(b, "api-ms-win-core-", 16) || !strcmp(b, "kernelbase") || !strcmp(b, "ntdll")) strlcpy(g, "kernel32", cap);
    else if (!strncmp(b, "api-ms-win-security-", 20) || !strncmp(b, "api-ms-win-eventing-", 20)) strlcpy(g, "advapi32", cap);
    else if (!strcmp(b, "wsock32")) strlcpy(g, "ws2_32", cap);
    else if (!strncmp(b, "api-ms-win-shell-", 17) || !strncmp(b, "api-ms-win-shcore-", 18)) strlcpy(g, "shlwapi", cap);
    else strlcpy(g, b, cap);
}
void *w32_compat_find(const char *dll, const char *name) {
    char g[64]; w32_dll_group(dll ? dll : "", g, sizeof g);
    for (unsigned i = 0; i < ARRAY_LEN(compat); i++)
        if ((!dll || !strcmp(compat[i].group, g)) && !strcmp(compat[i].name, name)) return compat[i].fn;
    return NULL;
}
int w32_compat_count(void) { return ARRAY_LEN(compat); }
