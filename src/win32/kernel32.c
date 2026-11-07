/* kernel32.dll / advapi32.dll / shell32.dll implementation */
#include "w32.h"

#define P w32_cur()
#define STD_IN_H  0x11
#define STD_OUT_H 0x12
#define STD_ERR_H 0x13
#define INVALID_HANDLE 0xFFFFFFFFu
#define HFILE_BASE 0x100
static inline void set_err(uint32_t e) { w32_proc_t *p = P; if (p) p->last_error = e; }

/* ---------------- console I/O ---------------- */
int w32_console_write(w32_proc_t *p, int fd, const char *s, int n) {
    if (!p) return n;
    if (!p->con) {
        /* GUI program writing to stdout: allocate a console on demand */
        window_t *w; p->con = terminal_new_console(path_basename(p->path), &w); p->own_console = true; p->in = p->con;
        w->pid = p->pid;
    }
    if (fd == 2) p->con->write(p->con, "\x1b[91m", 5);
    /* convert \r\n to \n */
    int start = 0;
    for (int i = 0; i < n; i++) if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') { if (i > start) p->con->write(p->con, s + start, i - start); start = i + 1; }
    if (n > start) p->con->write(p->con, s + start, n - start);
    if (fd == 2) p->con->write(p->con, "\x1b[0m", 4);
    return n;
}
static bool fill_stdin(w32_proc_t *p) {
    if (p->stdin_pos < p->stdin_len) return true;
    console_t *in = p->in ? p->in : p->con;
    if (!in) return false;
    char line[1000];
    int n = in->readline(in, line, sizeof line);
    if (n < 0) return false;
    p->stdin_len = 0; p->stdin_pos = 0;
    memcpy(p->stdin_buf, line, n); p->stdin_buf[n] = '\n'; p->stdin_len = n + 1;
    return true;
}
int w32_stdin_getc(w32_proc_t *p) { if (!fill_stdin(p)) return -1; return (uint8_t)p->stdin_buf[p->stdin_pos++]; }
int w32_stdin_read(w32_proc_t *p, char *buf, int n) {
    if (!fill_stdin(p)) return 0;
    int k = MIN(n, p->stdin_len - p->stdin_pos);
    memcpy(buf, p->stdin_buf + p->stdin_pos, k); p->stdin_pos += k;
    return k;
}

/* ---------------- handles ---------------- */
static w32_handle_t *hget(uint32_t h) {
    w32_proc_t *p = P; if (!p) return NULL;
    if (h < HFILE_BASE || ((h - HFILE_BASE) & 3)) return NULL;
    uint32_t i = (h - HFILE_BASE) / 4;
    if (i >= MAX_HANDLES || p->handles[i].kind == H_NONE) return NULL;
    return &p->handles[i];
}
static uint32_t halloc(int kind) {
    w32_proc_t *p = P;
    for (int i = 1; i < MAX_HANDLES; i++) if (p->handles[i].kind == H_NONE) { memset(&p->handles[i], 0, sizeof(w32_handle_t)); p->handles[i].kind = kind; return HFILE_BASE + i * 4; }
    return INVALID_HANDLE;
}
static const char *fixpath(const char *in, char *out, int cap) {
    w32_proc_t *p = P;
    if (isalpha(in[0]) && in[1] == ':') strlcpy(out, in, cap);
    else if (in[0] == '\\' || in[0] == '/') snprintf(out, cap, "C:%s", in);
    else path_join(out, cap, p ? p->cwd : "C:\\", in);
    path_normalize(out);
    return out;
}

static HANDLE WINAPI GetStdHandle(DWORD n) { return n == (DWORD)-10 ? STD_IN_H : n == (DWORD)-11 ? STD_OUT_H : n == (DWORD)-12 ? STD_ERR_H : INVALID_HANDLE; }
static BOOL WINAPI SetStdHandle(DWORD n, HANDLE h) { return 1; }
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
static HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    char path[512];
    if (!strcasecmp(name, "CONOUT$")) return STD_OUT_H;
    if (!strcasecmp(name, "CONIN$")) return STD_IN_H;
    fixpath(name, path, sizeof path);
    vnode_t *n = vfs_resolve(path, NULL);
    /* 1 CREATE_NEW 2 CREATE_ALWAYS 3 OPEN_EXISTING 4 OPEN_ALWAYS 5 TRUNCATE_EXISTING */
    if (disp == 1 && n) { set_err(80); return INVALID_HANDLE; }
    if ((disp == 3 || disp == 5) && !n) { set_err(2); return INVALID_HANDLE; }
    if (!n) { n = vfs_create(path, NULL, false); if (!n) { set_err(3); return INVALID_HANDLE; } }
    if (n->dir) { set_err(5); return INVALID_HANDLE; }
    if ((access & GENERIC_WRITE) && n->readonly) { set_err(5); return INVALID_HANDLE; }
    if (disp == 2 || disp == 5) { n->size = 0; vfs_generation++; }
    uint32_t h = halloc(H_FILE);
    if (h == INVALID_HANDLE) { set_err(4); return h; }
    w32_handle_t *e = hget(h); e->node = n; e->pos = 0; e->write = (access & GENERIC_WRITE) != 0;
    set_err(disp == 4 || disp == 2 ? 183 : 0);
    return h;
}
static HANDLE WINAPI CreateFileW(const WCHAR *name, DWORD a, DWORD s, void *sa, DWORD d, DWORD f, HANDLE t) {
    char n[260]; int i = 0; for (; name[i] && i < 259; i++) n[i] = (char)name[i]; n[i] = 0;
    return CreateFileA(n, a, s, sa, d, f, t);
}
static BOOL file_write(w32_handle_t *e, const void *buf, DWORD n) {
    vnode_t *v = e->node;
    if (!e->write || v->readonly) return 0;
    uint32_t end = e->pos + n;
    if (end > v->cap) { v->cap = end * 2 + 64; v->data = krealloc(v->data, v->cap); }
    if (e->pos > v->size) memset(v->data + v->size, 0, e->pos - v->size);
    memcpy(v->data + e->pos, buf, n);
    e->pos = end; if (end > v->size) v->size = end;
    rtc_now(&v->mtime); vfs_generation++;
    return 1;
}
static BOOL WINAPI WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov) {
    if (written) *written = n;
    if (h == STD_OUT_H || h == STD_ERR_H) { w32_console_write(P, h == STD_ERR_H ? 2 : 1, buf, n); return 1; }
    w32_handle_t *e = hget(h);
    if (!e || e->kind != H_FILE) { set_err(6); if (written) *written = 0; return 0; }
    if (!file_write(e, buf, n)) { set_err(5); if (written) *written = 0; return 0; }
    return 1;
}
static BOOL WINAPI ReadFile(HANDLE h, void *buf, DWORD n, DWORD *rd, void *ov) {
    if (h == STD_IN_H) { int k = w32_stdin_read(P, buf, n); if (rd) *rd = k; return 1; }
    w32_handle_t *e = hget(h);
    if (!e || e->kind != H_FILE) { set_err(6); if (rd) *rd = 0; return 0; }
    uint32_t avail = e->pos < e->node->size ? e->node->size - e->pos : 0;
    uint32_t k = MIN(n, avail);
    memcpy(buf, e->node->data + e->pos, k); e->pos += k;
    if (rd) *rd = k;
    return 1;
}
static BOOL WINAPI CloseHandle(HANDLE h) { w32_handle_t *e = hget(h); if (e) e->kind = H_NONE; return 1; }
static DWORD WINAPI GetFileSize(HANDLE h, DWORD *hi) { w32_handle_t *e = hget(h); if (hi) *hi = 0; return e && e->node ? e->node->size : 0xFFFFFFFF; }
static BOOL WINAPI GetFileSizeEx(HANDLE h, int64_t *sz) { w32_handle_t *e = hget(h); if (!e || !e->node) return 0; *sz = e->node->size; return 1; }
static DWORD WINAPI SetFilePointer(HANDLE h, LONG dist, LONG *hi, DWORD method) {
    w32_handle_t *e = hget(h); if (!e || !e->node) return 0xFFFFFFFF;
    int64_t np = method == 0 ? dist : method == 1 ? (int64_t)e->pos + dist : (int64_t)e->node->size + dist;
    if (np < 0) np = 0;
    e->pos = (uint32_t)np; if (hi) *hi = 0;
    return e->pos;
}
static BOOL WINAPI SetFilePointerEx(HANDLE h, int64_t dist, int64_t *np, DWORD method) { DWORD r = SetFilePointer(h, (LONG)dist, NULL, method); if (np) *np = r; return r != 0xFFFFFFFF; }
static BOOL WINAPI SetEndOfFile(HANDLE h) { w32_handle_t *e = hget(h); if (!e || !e->node) return 0; e->node->size = MIN(e->pos, e->node->size); return 1; }
static BOOL WINAPI FlushFileBuffers(HANDLE h) { return 1; }
static DWORD WINAPI GetFileType(HANDLE h) { return (h == STD_IN_H || h == STD_OUT_H || h == STD_ERR_H) ? 2 : hget(h) ? 1 : 0; }
static BOOL WINAPI DeleteFileA(const char *n) { char p[512]; vnode_t *v = vfs_resolve(fixpath(n, p, sizeof p), NULL); if (!v || v->dir) { set_err(2); return 0; } return vfs_delete(v); }
static BOOL WINAPI CreateDirectoryA(const char *n, void *sa) { char p[512]; fixpath(n, p, sizeof p); if (vfs_resolve(p, NULL)) { set_err(183); return 0; } return vfs_create(p, NULL, true) != NULL; }
static BOOL WINAPI RemoveDirectoryA(const char *n) { char p[512]; vnode_t *v = vfs_resolve(fixpath(n, p, sizeof p), NULL); if (!v || !v->dir || v->child) return 0; return vfs_delete(v); }
static BOOL WINAPI MoveFileA(const char *a, const char *b) {
    char p1[512], p2[512], dir[512]; vnode_t *v = vfs_resolve(fixpath(a, p1, sizeof p1), NULL); fixpath(b, p2, sizeof p2);
    if (!v || vfs_resolve(p2, NULL)) return 0;
    path_dirname(p2, dir, sizeof dir); vnode_t *d = vfs_resolve(dir, NULL);
    if (!d) return 0;
    if (d != v->parent && !vfs_move(v, d)) return 0;
    return vfs_rename(v, path_basename(p2));
}
static BOOL WINAPI CopyFileA(const char *a, const char *b, BOOL fail) {
    char p1[512], p2[512]; vnode_t *v = vfs_resolve(fixpath(a, p1, sizeof p1), NULL); fixpath(b, p2, sizeof p2);
    if (!v || v->dir) return 0;
    if (fail && vfs_resolve(p2, NULL)) return 0;
    return vfs_write_file(p2, v->data, v->size) != NULL;
}
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_READONLY 0x01
static DWORD WINAPI GetFileAttributesA(const char *n) { char p[512]; vnode_t *v = vfs_resolve(fixpath(n, p, sizeof p), NULL); if (!v) { set_err(2); return 0xFFFFFFFF; } return (v->dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL) | (v->readonly ? FILE_ATTRIBUTE_READONLY : 0); }
static DWORD WINAPI GetCurrentDirectoryA(DWORD n, char *buf) { w32_proc_t *p = P; DWORD l = strlen(p->cwd); if (buf && n > l) strcpy(buf, p->cwd); return n > l ? l : l + 1; }
static BOOL WINAPI SetCurrentDirectoryA(const char *d) { char p[512]; vnode_t *v = vfs_resolve(fixpath(d, p, sizeof p), NULL); if (!v || !v->dir) { set_err(3); return 0; } vfs_path(v, P->cwd, sizeof P->cwd); return 1; }
static DWORD WINAPI GetModuleFileNameA(HMODULE m, char *buf, DWORD n) { w32_proc_t *p = P; strlcpy(buf, p->path, n); return strlen(buf); }
static DWORD WINAPI GetModuleFileNameW(HMODULE m, WCHAR *buf, DWORD n) { w32_proc_t *p = P; DWORD i = 0; for (; p->path[i] && i + 1 < n; i++) buf[i] = p->path[i]; buf[i] = 0; return i; }
static DWORD WINAPI GetFullPathNameA(const char *n, DWORD cap, char *buf, char **fp) { char p[512]; fixpath(n, p, sizeof p); strlcpy(buf, p, cap); if (fp) { char *b = strrchr(buf, '\\'); *fp = b ? b + 1 : buf; } return strlen(p); }
static DWORD WINAPI GetTempPathA(DWORD n, char *buf) { strlcpy(buf, "C:\\tmp\\", n); return 7; }

typedef struct PACKED { DWORD attr; uint64_t ctime, atime, mtime; DWORD size_hi, size_lo, r0, r1; char name[260]; char alt[14]; } WIN32_FIND_DATAA;
static void fill_find(WIN32_FIND_DATAA *f, vnode_t *n) {
    memset(f, 0, sizeof *f);
    f->attr = n->dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL; f->size_lo = n->size;
    strlcpy(f->name, n->name, 260);
}
static bool glob_ci(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;;) { if (glob_ci(p + 1, s)) return true; if (!*s) return false; s++; } }
    if (!*s) return false;
    if (*p == '?' || tolower(*p) == tolower(*s)) return glob_ci(p + 1, s + 1);
    return false;
}
static BOOL WINAPI FindNextFileA(HANDLE h, WIN32_FIND_DATAA *f) {
    w32_handle_t *e = hget(h); if (!e || e->kind != H_FIND) return 0;
    while (e->find_cur) {
        vnode_t *c = e->find_cur; e->find_cur = c->next;
        if (glob_ci(e->find_pat, c->name) || !strcmp(e->find_pat, "*.*")) { fill_find(f, c); return 1; }
    }
    set_err(18); return 0;
}
static HANDLE WINAPI FindFirstFileA(const char *pat, WIN32_FIND_DATAA *f) {
    char p[512], dir[512]; fixpath(pat, p, sizeof p); path_dirname(p, dir, sizeof dir);
    vnode_t *d = vfs_resolve(dir, NULL);
    if (!d || !d->dir) { set_err(3); return INVALID_HANDLE; }
    uint32_t h = halloc(H_FIND); w32_handle_t *e = hget(h);
    e->find_dir = d; e->find_cur = d->child; strlcpy(e->find_pat, path_basename(p), sizeof e->find_pat);
    if (!FindNextFileA(h, f)) { e->kind = H_NONE; set_err(2); return INVALID_HANDLE; }
    return h;
}
static BOOL WINAPI FindClose(HANDLE h) { return CloseHandle(h); }

static BOOL WINAPI WriteConsoleA(HANDLE h, const char *buf, DWORD n, DWORD *w, void *r) { w32_console_write(P, 1, buf, n); if (w) *w = n; return 1; }
static BOOL WINAPI WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD n, DWORD *w, void *r) { char tmp[512]; DWORD k = 0; for (DWORD i = 0; i < n; i++) { tmp[k++] = buf[i] < 128 ? (char)buf[i] : '?'; if (k == sizeof tmp) { w32_console_write(P, 1, tmp, k); k = 0; } } if (k) w32_console_write(P, 1, tmp, k); if (w) *w = n; return 1; }
static BOOL WINAPI ReadConsoleA(HANDLE h, char *buf, DWORD n, DWORD *rd, void *c) { int k = w32_stdin_read(P, buf, n); if (rd) *rd = k; return 1; }
static BOOL WINAPI SetConsoleTitleA(const char *t) { w32_proc_t *p = P; if (p->con) p->con->set_title(p->con, t); return 1; }
static DWORD WINAPI GetConsoleTitleA(char *b, DWORD n) { strlcpy(b, path_basename(P->path), n); return strlen(b); }
static BOOL WINAPI SetConsoleTextAttribute(HANDLE h, WORD a) { w32_proc_t *p = P; if (p->con) p->con->set_color(p->con, a & 15, (a >> 4) & 15); return 1; }
static BOOL WINAPI GetConsoleMode(HANDLE h, DWORD *m) { if (m) *m = 3; return 1; }
static BOOL WINAPI SetConsoleMode(HANDLE h, DWORD m) { return 1; }
static UINT WINAPI GetConsoleCP(void) { return 437; }
static UINT WINAPI GetConsoleOutputCP(void) { return 437; }
static BOOL WINAPI SetConsoleCP(UINT c) { return 1; }
static BOOL WINAPI SetConsoleOutputCP(UINT c) { return 1; }
static BOOL WINAPI AllocConsole(void) { w32_console_write(P, 1, "", 0); return 1; }
static BOOL WINAPI FreeConsole(void) { return 1; }
static BOOL WINAPI SetConsoleCtrlHandler(void *h, BOOL add) { return 1; }
typedef struct { int16_t X, Y; } COORD;
typedef struct { COORD size, cursor; WORD attr; int16_t l, t, r, b; COORD max; } CSBI;
static BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, CSBI *i) { memset(i, 0, sizeof *i); i->size.X = 80; i->size.Y = 25; i->attr = 7; i->r = 79; i->b = 24; i->max = i->size; return 1; }
static BOOL WINAPI SetConsoleCursorPosition(HANDLE h, COORD c) { return 1; }
static BOOL WINAPI FillConsoleOutputCharacterA(HANDLE h, char c, DWORD n, COORD xy, DWORD *w) { w32_proc_t *p = P; if (c == ' ' && n >= 80 * 24 && p->con) p->con->clear(p->con); if (w) *w = n; return 1; }
static BOOL WINAPI FillConsoleOutputAttribute(HANDLE h, WORD a, DWORD n, COORD xy, DWORD *w) { if (w) *w = n; return 1; }

/* ---------------- process / thread ---------------- */
static void WINAPI ExitProcess(UINT code) { w32_exit_process(code); }
static BOOL WINAPI TerminateProcess(HANDLE h, UINT code) { if (h == 0xFFFFFFFF || h == 0) w32_exit_process(code); return 0; }
static HANDLE WINAPI GetCurrentProcess(void) { return 0xFFFFFFFF; }
static HANDLE WINAPI GetCurrentThread(void) { return 0xFFFFFFFE; }
static DWORD WINAPI GetCurrentProcessId(void) { return P->pid; }
static DWORD WINAPI GetCurrentThreadId(void) { return task_current()->pid; }
static char *WINAPI GetCommandLineA(void) { return P->cmdline; }
static WCHAR cmdw[512];
static WCHAR *WINAPI GetCommandLineW(void) { const char *s = P->cmdline; int i = 0; for (; s[i] && i < 511; i++) cmdw[i] = s[i]; cmdw[i] = 0; return cmdw; }
typedef struct { DWORD cb; char *res, *desk, *title; DWORD x, y, xs, ys, xc, yc, fill, flags; WORD show, r2; void *r3; HANDLE in, out, err; } STARTUPINFOA;
static void WINAPI GetStartupInfoA(STARTUPINFOA *s) { memset(s, 0, sizeof *s); s->cb = sizeof *s; s->show = 1; }
static void WINAPI GetStartupInfoW(STARTUPINFOA *s) { GetStartupInfoA(s); }
static void WINAPI Sleep(DWORD ms) { if (ms == 0) task_yield(); else task_sleep(ms); }
static DWORD WINAPI SleepEx(DWORD ms, BOOL a) { Sleep(ms); return 0; }
static BOOL WINAPI SwitchToThread(void) { task_yield(); return 1; }
typedef struct { void *fn; void *arg; w32_proc_t *p; } thr_start_t;
static void thread_main(void *a) {
    thr_start_t *s = a; void *fn = s->fn; void *arg = s->arg; kfree(s);
    uint32_t r = call_thread_proc(fn, arg);
    task_exit(r);
}
static HANDLE WINAPI CreateThread(void *sa, DWORD stack, void *fn, void *arg, DWORD flags, DWORD *tid) {
    w32_proc_t *p = P;
    thr_start_t *s = kmalloc(sizeof *s); s->fn = fn; s->arg = arg; s->p = p;
    task_t *t = task_create("thread", thread_main, s, MAX(stack, 128 * 1024u));
    snprintf(t->name, sizeof t->name, "%s:thread", path_basename(p->path));
    t->kind = 2; t->proc = p; t->cr3 = p->space; t->console = p->con;
    t->teb = (uintptr_t)w32_new_teb(p, t);
    p->threads++;
    if (tid) *tid = t->pid;
    uint32_t h = halloc(H_THREAD); hget(h)->tid = t->pid;
    return h;
}
static DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) {
    w32_handle_t *e = hget(h);
    uint32_t end = timer_ticks() + ms;
    if (!e) { if (ms != 0xFFFFFFFF) task_sleep(ms); return 0; }
    for (;;) {
        if (e->kind == H_THREAD && !task_find(e->tid)) return 0;
        if (e->kind == H_EVENT && e->signaled) return 0;
        if (e->kind != H_THREAD && e->kind != H_EVENT) return 0;
        if (ms != 0xFFFFFFFF && (int32_t)(timer_ticks() - end) >= 0) return 0x102;
        task_sleep(5);
    }
}
static DWORD WINAPI WaitForMultipleObjects(DWORD n, HANDLE *hs, BOOL all, DWORD ms) { for (DWORD i = 0; i < n; i++) WaitForSingleObject(hs[i], ms); return 0; }
static BOOL WINAPI GetExitCodeThread(HANDLE h, DWORD *c) { w32_handle_t *e = hget(h); if (!e) return 0; *c = task_find(e->tid) ? 259 : (DWORD)task_exit_code(e->tid); return 1; }
static HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL init, const char *n) { uint32_t h = halloc(H_EVENT); hget(h)->signaled = init; return h; }
static BOOL WINAPI SetEvent(HANDLE h) { w32_handle_t *e = hget(h); if (e) e->signaled = 1; return 1; }
static BOOL WINAPI ResetEvent(HANDLE h) { w32_handle_t *e = hget(h); if (e) e->signaled = 0; return 1; }
static HANDLE WINAPI CreateMutexA(void *sa, BOOL own, const char *n) { return halloc(H_EVENT); }
static BOOL WINAPI ReleaseMutex(HANDLE h) { return 1; }
static HANDLE WINAPI CreateSemaphoreA(void *sa, LONG i, LONG m, const char *n) { return halloc(H_EVENT); }
static BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG c, LONG *p) { return 1; }
static BOOL WINAPI DuplicateHandle(HANDLE a, HANDLE b, HANDLE c, HANDLE *d, DWORD e, BOOL f, DWORD g) { if (d) *d = b; return 1; }
static BOOL WINAPI GetExitCodeProcess(HANDLE h, DWORD *c) { *c = 259; return 1; }
typedef struct { HANDLE proc, thread; DWORD pid, tid; } PROCESS_INFORMATION;
static BOOL WINAPI CreateProcessA(const char *app, char *cmd, void *a, void *b, BOOL inh, DWORD fl, void *env, const char *cwd, STARTUPINFOA *si, PROCESS_INFORMATION *pi) {
    char path[512], args[512] = "";
    const char *src = app ? app : cmd;
    if (!src) return 0;
    const char *s = src; int n = 0;
    if (*s == '"') { s++; while (*s && *s != '"' && n < 511) path[n++] = *s++; if (*s) s++; }
    else while (*s && *s != ' ' && n < 511) path[n++] = *s++;
    path[n] = 0;
    if (cmd && !app) { while (*s == ' ') s++; strlcpy(args, s, sizeof args); }
    char full[512]; fixpath(path, full, sizeof full);
    if (!vfs_resolve(full, NULL)) { strlcat(full, ".exe", sizeof full); }
    char err[200];
    int pid = pe_spawn(full, args, P->con, err, sizeof err);
    if (pid < 0) { set_err(2); return 0; }
    if (pi) { pi->pid = pi->tid = pid; uint32_t h = halloc(H_THREAD); hget(h)->tid = pid; pi->proc = h; pi->thread = h; }
    return 1;
}

/* ---------------- memory ---------------- */
#define HEAP_H 0x00ABC000u
static HANDLE WINAPI GetProcessHeap(void) { return HEAP_H; }
static HANDLE WINAPI HeapCreate(DWORD o, DWORD i, DWORD m) { return HEAP_H; }
static BOOL WINAPI HeapDestroy(HANDLE h) { return 1; }
static void *WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T n) { void *p = w32_alloc(P, n); if (p && (flags & 8)) memset(p, 0, n); return p; }
static void *WINAPI HeapReAlloc(HANDLE h, DWORD flags, void *ptr, DWORD n) { uint32_t old = w32_msize(ptr); void *p = w32_realloc(P, ptr, n); if (p && (flags & 8) && n > old) memset((uint8_t *)p + old, 0, n - old); return p; }
static BOOL WINAPI HeapFree(HANDLE h, DWORD f, void *p) { w32_free(P, p); return 1; }
static SIZE_T WINAPI HeapSize(HANDLE h, DWORD f, void *p) { return w32_msize(p); }
static BOOL WINAPI HeapValidate(HANDLE h, DWORD f, void *p) { return 1; }
static void *WINAPI GlobalAlloc(UINT f, DWORD n) { void *p = w32_alloc(P, n); if (p && (f & 0x40)) memset(p, 0, n); return p; }
static void *WINAPI GlobalFree(void *p) { w32_free(P, p); return NULL; }
static void *WINAPI GlobalLock(void *p) { return p; }
static BOOL WINAPI GlobalUnlock(void *p) { return 1; }
static void *WINAPI LocalAlloc(UINT f, DWORD n) { return GlobalAlloc(f, n); }
static void *WINAPI LocalFree(void *p) { w32_free(P, p); return NULL; }
static void *WINAPI VirtualAlloc(void *addr, SIZE_T n, DWORD type, DWORD prot) {
    if (addr && !(type & 0x2000) && (type & 0x1000)) return addr;   /* commit of reserved region */
    if (n > 0x7FF00000u) return NULL;
    void *p = w32_alloc(P, n + 4096);
    if (!p) return NULL;
    uint8_t *a = (uint8_t *)(((uintptr_t)p + 4095) & ~(uintptr_t)4095);
    memset(a, 0, n);
    return a;
}
static BOOL WINAPI VirtualFree(void *a, DWORD n, DWORD t) { return 1; }
typedef struct { void *base, *alloc_base; DWORD alloc_prot; SIZE_T size; DWORD state, prot, type; } MBI;   /* same layout as Windows on x86 and x64 */
static SIZE_T WINAPI VirtualQuery(const void *addr, MBI *m, SIZE_T len) {
    memset(m, 0, sizeof *m);
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095;
    m->base = (void *)a; m->alloc_base = (void *)P->base; m->alloc_prot = 0x40; m->size = 4096; m->state = 0x1000; m->prot = 0x40; m->type = 0x1000000;
    return sizeof *m;
}
static BOOL WINAPI VirtualProtect(void *a, DWORD n, DWORD np, DWORD *op) { if (op) *op = 0x40; return 1; }
static BOOL WINAPI FlushInstructionCache(HANDLE h, const void *a, DWORD n) { return 1; }
typedef struct { DWORD len, load; SIZE_T total_phys, avail_phys, total_page, avail_page, total_virt, avail_virt; } MEMORYSTATUS;
static void WINAPI GlobalMemoryStatus(MEMORYSTATUS *m) { m->len = sizeof *m; m->total_phys = mm_phys_total(); m->avail_phys = mm_total() - mm_used(); m->load = mm_used() * 100 / mm_total(); m->total_page = m->total_phys; m->avail_page = m->avail_phys; m->total_virt = 0x7FFE0000; m->avail_virt = m->avail_phys; }
typedef struct { DWORD len, load; uint64_t total_phys, avail_phys, total_page, avail_page, total_virt, avail_virt, ext; } MEMORYSTATUSEX;
static BOOL WINAPI GlobalMemoryStatusEx(MEMORYSTATUSEX *m) { m->load = mm_used() * 100 / mm_total(); m->total_phys = mm_phys_total(); m->avail_phys = mm_total() - mm_used(); m->total_page = m->total_phys; m->avail_page = m->avail_phys; m->total_virt = 0x7FFE0000; m->avail_virt = m->avail_phys; m->ext = 0; return 1; }

/* ---------------- time ---------------- */
typedef struct { WORD y, mo, dow, d, h, mi, s, ms; } SYSTEMTIME;
static void fill_st(SYSTEMTIME *st, bool local) { rtc_time_t t; if (local) rtc_now(&t); else rtc_read(&t); st->y = t.year; st->mo = t.month; st->dow = t.weekday; st->d = t.day; st->h = t.hour; st->mi = t.minute; st->s = t.second; st->ms = timer_ticks() % 1000; }
static void WINAPI GetLocalTime(SYSTEMTIME *st) { fill_st(st, true); }
static void WINAPI GetSystemTime(SYSTEMTIME *st) { fill_st(st, false); }
static uint64_t filetime_now(void) {
    rtc_time_t t; rtc_read(&t);
    /* days since 1601-01-01 */
    int y = t.year, m = t.month;
    if (m <= 2) { y--; m += 12; }
    int64_t days = 365LL * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + t.day - 1 - 584694;
    days += 0;  /* calibrated so 1601-01-01 = 0 */
    uint64_t secs = (uint64_t)days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    return secs * 10000000ULL + (timer_ticks() % 1000) * 10000ULL;
}
static void WINAPI GetSystemTimeAsFileTime(uint64_t *ft) { *ft = filetime_now(); }
static BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, uint64_t *ft) { *ft = filetime_now(); return 1; }
static BOOL WINAPI FileTimeToSystemTime(const uint64_t *ft, SYSTEMTIME *st) { fill_st(st, false); return 1; }
static BOOL WINAPI FileTimeToLocalFileTime(const uint64_t *a, uint64_t *b) { *b = *a + (int64_t)tz_offset_minutes * 600000000LL; return 1; }
static DWORD WINAPI GetTickCount(void) { return timer_ticks(); }
static uint64_t WINAPI GetTickCount64(void) { return timer_ticks(); }
static BOOL WINAPI QueryPerformanceCounter(int64_t *c) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); (void)lo; (void)hi; *c = (int64_t)timer_ticks() * 1000; return 1; }
static BOOL WINAPI QueryPerformanceFrequency(int64_t *f) { *f = 1000000; return 1; }
typedef struct { LONG bias; WCHAR sname[32]; SYSTEMTIME sdate; LONG sbias; WCHAR dname[32]; SYSTEMTIME ddate; LONG dbias; } TZI;
static DWORD WINAPI GetTimeZoneInformation(TZI *t) { memset(t, 0, sizeof *t); t->bias = -tz_offset_minutes; return 0; }

/* ---------------- sync primitives (single CPU cooperative) ---------------- */
#define CS_SIZE (sizeof(void *) == 8 ? 40 : 24)       /* sizeof(CRITICAL_SECTION) */
static void WINAPI InitializeCriticalSection(void *cs) { memset(cs, 0, CS_SIZE); }
static BOOL WINAPI InitializeCriticalSectionAndSpinCount(void *cs, DWORD n) { memset(cs, 0, CS_SIZE); return 1; }
static void WINAPI DeleteCriticalSection(void *cs) {}
static void WINAPI EnterCriticalSection(void *cs) {}
static void WINAPI LeaveCriticalSection(void *cs) {}
static BOOL WINAPI TryEnterCriticalSection(void *cs) { return 1; }
static LONG WINAPI InterlockedIncrement(volatile LONG *v) { return ++*v; }
static LONG WINAPI InterlockedDecrement(volatile LONG *v) { return --*v; }
static LONG WINAPI InterlockedExchange(volatile LONG *v, LONG n) { LONG o = *v; *v = n; return o; }
static LONG WINAPI InterlockedExchangeAdd(volatile LONG *v, LONG n) { LONG o = *v; *v += n; return o; }
static LONG WINAPI InterlockedCompareExchange(volatile LONG *v, LONG n, LONG cmp) { LONG o = *v; if (o == cmp) *v = n; return o; }
static void WINAPI InitializeSListHead(void *h) { memset(h, 0, 8); }
static void WINAPI InitOnceInitialize(void *p) { *(uint32_t *)p = 0; }
static void WINAPI AcquireSRWLockExclusive(void *l) {}
static void WINAPI ReleaseSRWLockExclusive(void *l) {}
static void WINAPI InitializeSRWLock(void *l) { *(uint32_t *)l = 0; }

/* ---------------- TLS ---------------- */
static DWORD WINAPI TlsAlloc(void) { w32_proc_t *p = P; for (int i = 0; i < 64; i++) if (!(p->tls_used & (1ULL << i))) { p->tls_used |= 1ULL << i; p->tls_slots[i] = 0; return i; } return 0xFFFFFFFF; }
static BOOL WINAPI TlsFree(DWORD i) { if (i < 64) P->tls_used &= ~(1ULL << i); return 1; }
static void *WINAPI TlsGetValue(DWORD i) { set_err(0); return i < 64 ? (void *)P->tls_slots[i] : NULL; }
static BOOL WINAPI TlsSetValue(DWORD i, void *v) { if (i >= 64) return 0; P->tls_slots[i] = (uintptr_t)v; return 1; }
static DWORD WINAPI FlsAlloc(void *cb) { return TlsAlloc(); }
static void *WINAPI FlsGetValue(DWORD i) { return TlsGetValue(i); }
static BOOL WINAPI FlsSetValue(DWORD i, void *v) { return TlsSetValue(i, v); }
static BOOL WINAPI FlsFree(DWORD i) { return TlsFree(i); }

/* ---------------- errors / misc ---------------- */
static DWORD WINAPI GetLastError(void) { return P->last_error; }
static void WINAPI SetLastError(DWORD e) { P->last_error = e; }
static UINT WINAPI SetErrorMode(UINT m) { return 0; }
static void *WINAPI SetUnhandledExceptionFilter(void *f) { return NULL; }
static LONG WINAPI UnhandledExceptionFilter(void *e) { return 0; }
static void WINAPI RaiseException(DWORD code, DWORD flags, DWORD n, const DWORD *args) {
    char m[100]; snprintf(m, sizeof m, "Unhandled exception 0x%08x raised by the program.", code);
    w32_proc_t *p = P;
    if (p->con) con_printf(p->con, "\n%s\n", m); else msgbox(path_basename(p->path), m, MBI_ERROR, MBX_OK, NULL, NULL);
    w32_exit_process(code);
}
static void WINAPI RtlUnwind(void *a, void *b, void *c, void *d) {}
static BOOL WINAPI IsDebuggerPresent(void) { return 0; }
static void WINAPI OutputDebugStringA(const char *s) { klog("OutputDebugString: %s", s); }
static void WINAPI DebugBreak(void) {}
static BOOL WINAPI IsProcessorFeaturePresent(DWORD f) { return f == 0 || f == 1; }
static BOOL WINAPI Beep(DWORD f, DWORD d) { speaker_beep(f, d); return 1; }
static DWORD WINAPI GetVersion(void) { return 0x0A280105; }   /* 5.1 build 2600 */
typedef struct { DWORD size, major, minor, build, platform; char csd[128]; } OSVERSIONINFOA;
static BOOL WINAPI GetVersionExA(OSVERSIONINFOA *v) { v->major = 5; v->minor = 1; v->build = 2600; v->platform = 2; strcpy(v->csd, "THE WRLD OS"); return 1; }
static BOOL WINAPI GetComputerNameA(char *b, DWORD *n) { strlcpy(b, computer_name, *n); *n = strlen(b); return 1; }
static BOOL WINAPI GetUserNameA(char *b, DWORD *n) { strlcpy(b, user_name, *n); *n = strlen(b) + 1; return 1; }
typedef struct { WORD arch, r; DWORD page; void *minaddr, *maxaddr; uintptr_t mask; DWORD ncpu, type, gran; WORD level, rev; } SYSTEM_INFO;
static void WINAPI GetSystemInfo(SYSTEM_INFO *s) { memset(s, 0, sizeof *s); s->page = 4096; s->minaddr = (void *)0x10000; s->maxaddr = (void *)(uintptr_t)(ARCH_BITS == 64 ? 0x7FFFFFFEFFFFull : 0x7FFEFFFF); s->mask = 1; s->ncpu = 1;
    s->arch = ARCH_BITS == 64 ? 9 : 0; s->type = ARCH_BITS == 64 ? 8664 : 586; s->gran = 65536; s->level = 6; }
static void WINAPI GetNativeSystemInfo(SYSTEM_INFO *s) { GetSystemInfo(s); }
static UINT WINAPI GetACP(void) { return 1252; }
static UINT WINAPI GetOEMCP(void) { return 437; }
typedef struct { UINT max; BYTE def[2]; BYTE lead[12]; } CPINFO;
static BOOL WINAPI GetCPInfo(UINT cp, CPINFO *c) { memset(c, 0, sizeof *c); c->max = 1; c->def[0] = '?'; return 1; }
static BOOL WINAPI IsValidCodePage(UINT cp) { return 1; }
static BOOL WINAPI IsDBCSLeadByte(BYTE b) { return 0; }
static BOOL WINAPI IsDBCSLeadByteEx(UINT cp, BYTE b) { return 0; }
static int WINAPI MultiByteToWideChar(UINT cp, DWORD f, const char *s, int n, WCHAR *w, int wn) {
    if (n < 0) n = strlen(s) + 1;
    if (!wn) return n;
    int k = MIN(n, wn); for (int i = 0; i < k; i++) w[i] = (uint8_t)s[i];
    return k;
}
static int WINAPI WideCharToMultiByte(UINT cp, DWORD f, const WCHAR *w, int n, char *s, int sn, const char *def, BOOL *used) {
    if (n < 0) { n = 0; while (w[n]) n++; n++; }
    if (!sn) return n;
    int k = MIN(n, sn); for (int i = 0; i < k; i++) s[i] = w[i] < 256 ? (char)w[i] : '?';
    return k;
}
static DWORD WINAPI GetEnvironmentVariableA(const char *n, char *b, DWORD sz) {
    const char *v = NULL;
    if (!strcasecmp(n, "OS")) v = "THE_WRLD_OS"; else if (!strcasecmp(n, "USERPROFILE") || !strcasecmp(n, "HOME")) v = USER_HOME;
    else if (!strcasecmp(n, "TEMP") || !strcasecmp(n, "TMP")) v = "C:\\tmp"; else if (!strcasecmp(n, "USERNAME")) v = user_name;
    else if (!strcasecmp(n, "SystemRoot") || !strcasecmp(n, "windir")) v = "C:\\WRLD"; else if (!strcasecmp(n, "PATH")) v = "C:\\WRLD\\System32;C:\\WRLD";
    if (!v) { set_err(203); return 0; }
    DWORD l = strlen(v); if (b && sz > l) strcpy(b, v); return sz > l ? l : l + 1;
}
static BOOL WINAPI SetEnvironmentVariableA(const char *n, const char *v) { return 1; }
static char env_block[] = "OS=THE_WRLD_OS\0USERPROFILE=" USER_HOME "\0\0";
static char *WINAPI GetEnvironmentStringsA(void) { return env_block; }
static WCHAR envw[4] = { 0, 0, 0, 0 };
static WCHAR *WINAPI GetEnvironmentStringsW(void) { return envw; }
static BOOL WINAPI FreeEnvironmentStringsA(char *e) { return 1; }
static BOOL WINAPI FreeEnvironmentStringsW(WCHAR *e) { return 1; }
static int WINAPI lstrlenA(const char *s) { return s ? strlen(s) : 0; }
static char *WINAPI lstrcpyA(char *d, const char *s) { return strcpy(d, s); }
static char *WINAPI lstrcpynA(char *d, const char *s, int n) { strlcpy(d, s, n); return d; }
static char *WINAPI lstrcatA(char *d, const char *s) { return strcat(d, s); }
static int WINAPI lstrcmpA(const char *a, const char *b) { int r = strcmp(a, b); return r < 0 ? -1 : r > 0; }
static int WINAPI lstrcmpiA(const char *a, const char *b) { int r = strcasecmp(a, b); return r < 0 ? -1 : r > 0; }
static DWORD WINAPI GetLogicalDrives(void) { DWORD m = 0; for (char l = 'A'; l <= 'Z'; l++) if (vfs_drive_kind(l)) m |= 1 << (l - 'A'); return m; }
static UINT WINAPI GetDriveTypeA(const char *r) { return r && vfs_drive_kind(r[0]) == 2 ? 5 : 3; }
static UINT WINAPI GetWindowsDirectoryA(char *b, UINT n) { strlcpy(b, "C:\\WRLD", n); return 10; }
static UINT WINAPI GetSystemDirectoryA(char *b, UINT n) { strlcpy(b, "C:\\WRLD\\System32", n); return 19; }
static BOOL WINAPI GetDiskFreeSpaceExA(const char *d, uint64_t *a, uint64_t *t, uint64_t *f) { if (a) *a = mm_total() - mm_used(); if (t) *t = mm_total(); if (f) *f = mm_total() - mm_used(); return 1; }

/* ---------------- modules ---------------- */
#define KMOD(i) (0x7C000000u + (i) * 0x10000)
static const char *modnames[] = { "kernel32.dll", "msvcrt.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "ntdll.dll", "kernelbase.dll", "ucrtbase.dll" };
static HMODULE mod_handle(const char *n) {
    if (!n) return P->base;
    char b[64]; strlcpy(b, path_basename(n), sizeof b);
    if (!strchr(b, '.')) strlcat(b, ".dll", sizeof b);
    for (unsigned i = 0; i < ARRAY_LEN(modnames); i++) if (!strcasecmp(b, modnames[i])) return KMOD(i);
    return 0;
}
static HMODULE WINAPI GetModuleHandleA(const char *n) { HMODULE h = mod_handle(n); if (!h) set_err(126); return h; }
static HMODULE WINAPI GetModuleHandleW(const WCHAR *n) { if (!n) return P->base; char b[64]; int i = 0; for (; n[i] && i < 63; i++) b[i] = (char)n[i]; b[i] = 0; return GetModuleHandleA(b); }
static BOOL WINAPI GetModuleHandleExW(DWORD f, const WCHAR *n, HMODULE *h) { *h = GetModuleHandleW(n); return *h != 0; }
static HMODULE WINAPI LoadLibraryA(const char *n) { HMODULE h = mod_handle(n); if (!h) { set_err(126); klog("w32: LoadLibrary(%s) failed", n); } return h; }
static HMODULE WINAPI LoadLibraryW(const WCHAR *n) { return GetModuleHandleW(n); }
static HMODULE WINAPI LoadLibraryExA(const char *n, HANDLE f, DWORD fl) { return LoadLibraryA(n); }
static BOOL WINAPI FreeLibrary(HMODULE h) { return 1; }
static void *WINAPI GetProcAddress(HMODULE h, const char *name) {
    if ((uintptr_t)name < 0x10000) return NULL;
    void *fn = NULL;
    if (h >= KMOD(0) && h < KMOD(ARRAY_LEN(modnames))) {
        const char *dll = modnames[(h - KMOD(0)) / 0x10000];
        if (!strcasecmp(dll, "ntdll.dll") || !strcasecmp(dll, "kernelbase.dll")) dll = "kernel32.dll";
        if (!strcasecmp(dll, "ucrtbase.dll")) dll = "msvcrt.dll";
        w32_lookup(dll, name, &fn);
    }
    if (!fn) set_err(127);
    return fn;
}
static BOOL WINAPI DisableThreadLibraryCalls(HMODULE h) { return 1; }
static HANDLE WINAPI FindResourceA(HMODULE m, const char *n, const char *t) { return 0; }
static int WINAPI MulDiv(int a, int b, int c) { return c ? (int)((int64_t)a * b / c) : -1; }
static DWORD WINAPI FormatMessageA(DWORD f, const void *src, DWORD id, DWORD lang, char *buf, DWORD n, void *args) { if (f & 0x100) { char **pb = (char **)buf; *pb = w32_alloc(P, 64); buf = *pb; n = 64; } snprintf(buf, n, "Error %u", id); return strlen(buf); }
static int WINAPI CompareStringA(DWORD lc, DWORD f, const char *a, int na, const char *b, int nb) { int r = (f & 1) ? strcasecmp(a, b) : strcmp(a, b); return r < 0 ? 1 : r > 0 ? 3 : 2; }
static DWORD WINAPI GetUserDefaultLCID(void) { return 0x409; }
static int WINAPI GetLocaleInfoA(DWORD lc, DWORD t, char *b, int n) { if (n) strlcpy(b, ".", n); return 2; }
static BOOL WINAPI GetStringTypeW(DWORD t, const WCHAR *s, int n, WORD *o) { if (n < 0) { n = 0; while (s[n]) n++; } for (int i = 0; i < n; i++) o[i] = s[i] == ' ' ? 0x48 : (s[i] >= '0' && s[i] <= '9') ? 0x84 : 0x101; return 1; }
static int WINAPI LCMapStringW(DWORD lc, DWORD f, const WCHAR *s, int n, WCHAR *d, int dn) { if (n < 0) { n = 0; while (s[n]) n++; n++; } if (!dn) return n; for (int i = 0; i < n && i < dn; i++) d[i] = s[i]; return MIN(n, dn); }
static void *WINAPI EncodePointer(void *p) { return p; }
static void *WINAPI DecodePointer(void *p) { return p; }

/* advapi32 */
static LONG WINAPI RegOpenKeyExA(HANDLE k, const char *s, DWORD o, DWORD sam, HANDLE *r) { return 2; }
static LONG WINAPI RegQueryValueExA(HANDLE k, const char *n, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) { return 2; }
static LONG WINAPI RegCloseKey(HANDLE k) { return 0; }
static LONG WINAPI RegCreateKeyExA(HANDLE k, const char *s, DWORD r, char *c, DWORD o, DWORD sam, void *sa, HANDLE *res, DWORD *d) { if (res) *res = 0x1234; return 0; }
static LONG WINAPI RegSetValueExA(HANDLE k, const char *n, DWORD r, DWORD t, const BYTE *d, DWORD sz) { return 0; }
static BOOL WINAPI CryptAcquireContextA(HANDLE *h, const char *a, const char *b, DWORD t, DWORD f) { *h = 1; return 1; }
static BOOL WINAPI CryptGenRandom(HANDLE h, DWORD n, BYTE *b) { for (DWORD i = 0; i < n; i++) b[i] = rand_next(); return 1; }
static BOOL WINAPI CryptReleaseContext(HANDLE h, DWORD f) { return 1; }
static BOOL WINAPI SystemFunction036(void *b, DWORD n) { for (DWORD i = 0; i < n; i++) ((BYTE *)b)[i] = rand_next(); return 1; }

/* shell32 */
typedef struct { void *fn; } shell_req_t;
static void do_open(void *a) { char *p = a; shell_open(p, NULL); kfree(p); }
static HANDLE WINAPI ShellExecuteA(HWND h, const char *op, const char *file, const char *params, const char *dir, int show) {
    char p[512]; fixpath(file, p, sizeof p);
    if (!vfs_resolve(p, NULL)) return 2;
    wm_defer(do_open, strdup(p));
    return 42;
}
static int WINAPI SHGetFolderPathA(HWND h, int csidl, HANDLE t, DWORD f, char *path) { strcpy(path, csidl == 0 || csidl == 0x10 ? USER_DESKTOP : csidl == 5 ? USER_DOCS : USER_HOME); return 0; }

#define E(n) { #n, (void *)n }
const w32_export_t kernel32_exports[] = {
    E(GetStdHandle), E(SetStdHandle), E(CreateFileA), E(CreateFileW), E(WriteFile), E(ReadFile), E(CloseHandle), E(GetFileSize), E(GetFileSizeEx),
    E(SetFilePointer), E(SetFilePointerEx), E(SetEndOfFile), E(FlushFileBuffers), E(GetFileType), E(DeleteFileA), E(CreateDirectoryA), E(RemoveDirectoryA),
    E(MoveFileA), E(CopyFileA), E(GetFileAttributesA), E(GetCurrentDirectoryA), E(SetCurrentDirectoryA), E(GetModuleFileNameA), E(GetModuleFileNameW),
    E(GetFullPathNameA), E(GetTempPathA), E(FindFirstFileA), E(FindNextFileA), E(FindClose),
    E(WriteConsoleA), E(WriteConsoleW), E(ReadConsoleA), E(SetConsoleTitleA), E(GetConsoleTitleA), E(SetConsoleTextAttribute), E(GetConsoleMode), E(SetConsoleMode),
    E(GetConsoleCP), E(GetConsoleOutputCP), E(SetConsoleCP), E(SetConsoleOutputCP), E(AllocConsole), E(FreeConsole), E(SetConsoleCtrlHandler),
    E(GetConsoleScreenBufferInfo), E(SetConsoleCursorPosition), E(FillConsoleOutputCharacterA), E(FillConsoleOutputAttribute),
    E(ExitProcess), E(TerminateProcess), E(GetCurrentProcess), E(GetCurrentThread), E(GetCurrentProcessId), E(GetCurrentThreadId), E(GetCommandLineA), E(GetCommandLineW),
    E(GetStartupInfoA), E(GetStartupInfoW), E(Sleep), E(SleepEx), E(SwitchToThread), E(CreateThread), E(WaitForSingleObject), E(WaitForMultipleObjects), E(GetExitCodeThread),
    E(CreateEventA), E(SetEvent), E(ResetEvent), E(CreateMutexA), E(ReleaseMutex), E(CreateSemaphoreA), E(ReleaseSemaphore), E(DuplicateHandle), E(GetExitCodeProcess), E(CreateProcessA),
    E(GetProcessHeap), E(HeapCreate), E(HeapDestroy), E(HeapAlloc), E(HeapReAlloc), E(HeapFree), E(HeapSize), E(HeapValidate), E(GlobalAlloc), E(GlobalFree), E(GlobalLock), E(GlobalUnlock),
    E(LocalAlloc), E(LocalFree), E(VirtualAlloc), E(VirtualFree), E(VirtualQuery), E(VirtualProtect), E(FlushInstructionCache), E(GlobalMemoryStatus), E(GlobalMemoryStatusEx),
    E(GetLocalTime), E(GetSystemTime), E(GetSystemTimeAsFileTime), E(SystemTimeToFileTime), E(FileTimeToSystemTime), E(FileTimeToLocalFileTime), E(GetTickCount), E(GetTickCount64),
    E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetTimeZoneInformation),
    E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection), E(TryEnterCriticalSection),
    E(InterlockedIncrement), E(InterlockedDecrement), E(InterlockedExchange), E(InterlockedExchangeAdd), E(InterlockedCompareExchange), E(InitializeSListHead), E(InitOnceInitialize),
    E(AcquireSRWLockExclusive), E(ReleaseSRWLockExclusive), E(InitializeSRWLock),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(FlsAlloc), E(FlsGetValue), E(FlsSetValue), E(FlsFree),
    E(GetLastError), E(SetLastError), E(SetErrorMode), E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter), E(RaiseException), E(RtlUnwind), E(IsDebuggerPresent),
    E(OutputDebugStringA), E(DebugBreak), E(IsProcessorFeaturePresent), E(Beep), E(GetVersion), E(GetVersionExA), E(GetComputerNameA), E(GetSystemInfo), E(GetNativeSystemInfo),
    E(GetACP), E(GetOEMCP), E(GetCPInfo), E(IsValidCodePage), E(IsDBCSLeadByte), E(IsDBCSLeadByteEx), E(MultiByteToWideChar), E(WideCharToMultiByte),
    E(GetEnvironmentVariableA), E(SetEnvironmentVariableA), E(GetEnvironmentStringsA), E(GetEnvironmentStringsW), E(FreeEnvironmentStringsA), E(FreeEnvironmentStringsW),
    E(lstrlenA), E(lstrcpyA), E(lstrcpynA), E(lstrcatA), E(lstrcmpA), E(lstrcmpiA), E(GetLogicalDrives), E(GetDriveTypeA), E(GetWindowsDirectoryA), E(GetSystemDirectoryA), E(GetDiskFreeSpaceExA),
    E(GetModuleHandleA), E(GetModuleHandleW), E(GetModuleHandleExW), E(LoadLibraryA), E(LoadLibraryW), E(LoadLibraryExA), E(FreeLibrary), E(GetProcAddress), E(DisableThreadLibraryCalls),
    E(FindResourceA), E(MulDiv), E(FormatMessageA), E(CompareStringA), E(GetUserDefaultLCID), E(GetLocaleInfoA), E(GetStringTypeW), E(LCMapStringW), E(EncodePointer), E(DecodePointer),
    { "RtlCaptureContext", (void *)RtlUnwind },
};
const int kernel32_count = ARRAY_LEN(kernel32_exports);
const w32_export_t advapi32_exports[] = {
    E(GetUserNameA), E(RegOpenKeyExA), E(RegQueryValueExA), E(RegCloseKey), E(RegCreateKeyExA), E(RegSetValueExA),
    E(CryptAcquireContextA), E(CryptGenRandom), E(CryptReleaseContext), E(SystemFunction036),
};
const int advapi32_count = ARRAY_LEN(advapi32_exports);
const w32_export_t shell32_exports[] = { E(ShellExecuteA), E(SHGetFolderPathA) };
const int shell32_count = ARRAY_LEN(shell32_exports);
