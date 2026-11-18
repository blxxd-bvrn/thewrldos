/* More of the C runtime (included by msvcrt.c): file descriptors, stat and find,
 * the Universal CRT's start-up and stdio entry points, wide-character printf,
 * time conversions, setjmp/longjmp, C++ operator new/delete and odds and ends.
 * Names are what msvcrt.dll, msvcr*.dll and ucrtbase.dll (api-ms-win-crt-*) export. */

static WCHAR *wdup_a(const char *s) { size_t n = strlen(s); WCHAR *w = w32_alloc(P, (n + 1) * 2); for (size_t i = 0; i <= n; i++) w[i] = (uint8_t)s[i]; return w; }
static void w2a_buf(const WCHAR *w, char *a, int cap) { int i = 0; if (w) for (; w[i] && i < cap - 1; i++) a[i] = w[i] < 256 ? (char)w[i] : '?'; a[i] = 0; }
static int wcmp(const WCHAR *a, const WCHAR *b) { while (*a && *a == *b) { a++; b++; } return (int)*a - (int)*b; }
#define K32(type, name) ((type)k32(name))

/* ---------------- errno values ---------------- */
enum { E_NOENT = 2, E_BADF = 9, E_ACCES = 13, E_EXIST = 17, E_INVAL = 22, E_MFILE = 24, E_RANGE = 34 };
static int set_errno(int e) { P->errno_val = e; return -1; }
static const char *errtext(int e) {
    static const char *t[] = { "No error", "Operation not permitted", "No such file or directory", "No such process", "Interrupted function call",
        "Input/output error", "No such device or address", "Arg list too long", "Exec format error", "Bad file descriptor", "No child processes",
        "Resource temporarily unavailable", "Not enough space", "Permission denied", "Bad address", "Unknown error", "Resource device",
        "File exists", "Improper link", "No such device", "Not a directory", "Is a directory", "Invalid argument", "Too many open files in system",
        "Too many open files", "Inappropriate I/O control operation", "Unknown error", "File too large", "No space left on device", "Invalid seek",
        "Read-only file system", "Too many links", "Broken pipe", "Domain error", "Result too large" };
    return e >= 0 && e < (int)ARRAY_LEN(t) ? t[e] : "Unknown error";
}
static char *CDECL strerror2_(int e) { static char b[64]; strlcpy(b, errtext(e), sizeof b); return b; }
static int CDECL strerror_s_(char *b, size_t n, int e) { if (!b || !n) return E_INVAL; strlcpy(b, errtext(e), n); return 0; }
static void CDECL perror_(const char *s) {
    char b[300]; snprintf(b, sizeof b, "%s%s%s\n", s && *s ? s : "", s && *s ? ": " : "", errtext(P->errno_val));
    w32_console_write(P, 2, b, strlen(b));
}
static const char *sys_errlist_v[35];
static int sys_nerr_v = 35;
static void init_errlist(void) { if (!sys_errlist_v[0]) for (int i = 0; i < 35; i++) sys_errlist_v[i] = errtext(i); }

/* ---------------- file descriptors ---------------- */
#define NFD 64
#define GR 0x80000000u
#define GW 0x40000000u
static uintptr_t fd_handle(w32_proc_t *p, int fd) {
    if (fd < 0 || fd >= NFD) return 0;
    if (p->fdh[fd]) return p->fdh[fd];
    return fd < 3 ? 0x11 + fd : 0;                       /* STD_INPUT/OUTPUT/ERROR pseudo handles */
}
static bool is_console_h(uintptr_t h) { return h >= 0x11 && h <= 0x13; }
static int fd_alloc(w32_proc_t *p, uintptr_t h) {
    for (int i = 3; i < NFD; i++) if (!p->fdh[i]) { p->fdh[i] = h; p->fdfl[i] = 0; return i; }
    return -1;
}
static bool handle_shared(w32_proc_t *p, int fd) { for (int i = 0; i < NFD; i++) if (i != fd && p->fdh[i] == p->fdh[fd]) return true; return false; }
static int open_common(const char *path, int of) {
    w32_proc_t *p = P;
    DWORD acc = (of & 3) == 0 ? GR : (of & 3) == 1 ? GW : GR | GW;
    DWORD disp = (of & 0x100) ? ((of & 0x400) ? 1 : (of & 0x200) ? 2 : 4) : ((of & 0x200) ? 5 : 3);
    if (!path) return set_errno(E_INVAL);
    HANDLE h = K32(HANDLE (WINAPI *)(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE), "CreateFileA")(path, acc, 3, NULL, disp, 0, 0);
    if ((uint32_t)h == 0xFFFFFFFFu) {
        DWORD e = K32(DWORD (WINAPI *)(void), "GetLastError")();
        return set_errno(e == 80 ? E_EXIST : e == 5 ? E_ACCES : E_NOENT);
    }
    int fd = fd_alloc(p, h);
    if (fd < 0) { K32(BOOL (WINAPI *)(HANDLE), "CloseHandle")(h); return set_errno(E_MFILE); }
    if (of & 8) p->fdfl[fd] |= 1;                        /* O_APPEND */
    return fd;
}
static int CDECL _open_(const char *path, int of, ...) { return open_common(path, of); }
static int CDECL _wopen_(const WCHAR *path, int of, ...) { char a[512]; w2a_buf(path, a, sizeof a); return open_common(a, of); }
static int CDECL _sopen_(const char *path, int of, int sh, ...) { return open_common(path, of); }
static int CDECL _sopen_s_(int *fd, const char *path, int of, int sh, int pm) { *fd = open_common(path, of); return *fd < 0 ? P->errno_val : 0; }
static int CDECL _wsopen_s_(int *fd, const WCHAR *path, int of, int sh, int pm) { char a[512]; w2a_buf(path, a, sizeof a); return _sopen_s_(fd, a, of, sh, pm); }
static int CDECL _close_(int fd) {
    w32_proc_t *p = P;
    if (fd < 0 || fd >= NFD || (!p->fdh[fd] && fd >= 3)) return set_errno(E_BADF);
    if (p->fdh[fd] && !is_console_h(p->fdh[fd]) && !handle_shared(p, fd)) K32(BOOL (WINAPI *)(HANDLE), "CloseHandle")(p->fdh[fd]);
    p->fdh[fd] = 0;
    return 0;
}
static int CDECL _read2_(int fd, void *b, unsigned n) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    if (!h) return set_errno(E_BADF);
    if (h == 0x11) return w32_stdin_read(p, b, n);
    if (is_console_h(h)) return set_errno(E_BADF);
    DWORD r = 0;
    if (!K32(BOOL (WINAPI *)(HANDLE, void *, DWORD, DWORD *, void *), "ReadFile")(h, b, n, &r, NULL)) return set_errno(E_BADF);
    return (int)r;
}
static int CDECL _write2_(int fd, const void *b, unsigned n) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    if (!h) return set_errno(E_BADF);
    if (h == 0x12 || h == 0x13) return w32_console_write(p, h == 0x13 ? 2 : 1, b, n);
    if (h == 0x11) return set_errno(E_BADF);
    if (fd < NFD && (p->fdfl[fd] & 1)) K32(DWORD (WINAPI *)(HANDLE, LONG, LONG *, DWORD), "SetFilePointer")(h, 0, NULL, 2);
    DWORD w = 0;
    if (!K32(BOOL (WINAPI *)(HANDLE, const void *, DWORD, DWORD *, void *), "WriteFile")(h, b, n, &w, NULL)) return set_errno(E_ACCES);
    return (int)w;
}
static int64_t seek_fd(int fd, int64_t off, int wh) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    if (!h || is_console_h(h)) return set_errno(E_BADF);
    if (wh < 0 || wh > 2) return set_errno(E_INVAL);
    DWORD r = K32(DWORD (WINAPI *)(HANDLE, LONG, LONG *, DWORD), "SetFilePointer")(h, (LONG)off, NULL, wh);
    return r == 0xFFFFFFFF ? set_errno(E_BADF) : (int64_t)r;
}
static wlong_t CDECL _lseek_(int fd, wlong_t off, int wh) { return (wlong_t)seek_fd(fd, off, wh); }
static int64_t CDECL _lseeki64_(int fd, int64_t off, int wh) { return seek_fd(fd, off, wh); }
static wlong_t CDECL _tell_(int fd) { return (wlong_t)seek_fd(fd, 0, 1); }
static int64_t CDECL _telli64_(int fd) { return seek_fd(fd, 0, 1); }
static int64_t fd_length(int fd) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    vnode_t *v = h ? w32_handle_node(p, h) : NULL;
    return v ? (int64_t)v->size : set_errno(E_BADF);
}
static wlong_t CDECL _filelength_(int fd) { return (wlong_t)fd_length(fd); }
static int64_t CDECL _filelengthi64_(int fd) { return fd_length(fd); }
static int CDECL _eof_(int fd) { int64_t l = fd_length(fd); if (l < 0) return -1; return seek_fd(fd, 0, 1) >= l; }
static int CDECL _dup_(int fd) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    if (!h) return set_errno(E_BADF);
    int n = fd_alloc(p, h);
    return n < 0 ? set_errno(E_MFILE) : n;
}
static int CDECL _dup2_(int a, int b) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, a);
    if (!h || b < 0 || b >= NFD) return set_errno(E_BADF);
    if (a == b) return 0;
    if (p->fdh[b]) _close_(b);
    p->fdh[b] = h; p->fdfl[b] = p->fdfl[a];
    return 0;
}
static int CDECL _commit_(int fd) { return fd_handle(P, fd) ? 0 : set_errno(E_BADF); }
static int CDECL _chsize_s_(int fd, int64_t size) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    if (!h || is_console_h(h)) return E_BADF;
    DWORD (WINAPI *sfp)(HANDLE, LONG, LONG *, DWORD) = k32("SetFilePointer");
    DWORD old = sfp(h, 0, NULL, 1);
    sfp(h, (LONG)size, NULL, 0);
    K32(BOOL (WINAPI *)(HANDLE), "SetEndOfFile")(h);
    sfp(h, old, NULL, 0);
    return 0;
}
static int CDECL _chsize_(int fd, wlong_t size) { int e = _chsize_s_(fd, size); return e ? set_errno(e) : 0; }
static int CDECL _open_osfhandle_(intptr_t h, int fl) { int fd = fd_alloc(P, (uintptr_t)h); if (fd >= 0 && (fl & 8)) P->fdfl[fd] |= 1; return fd < 0 ? set_errno(E_MFILE) : fd; }
static intptr_t CDECL _get_osfhandle2_(int fd) { uintptr_t h = fd_handle(P, fd); if (!h) { set_errno(E_BADF); return -1; } return (intptr_t)h; }
static int CDECL _locking_(int fd, int mode, wlong_t n) { return 0; }
static int CDECL _setmode2_(int fd, int m) { return fd_handle(P, fd) ? 0x4000 : set_errno(E_BADF); }
static int CDECL _isatty2_(int fd) { uintptr_t h = fd_handle(P, fd); return is_console_h(h) ? 1 : 0; }
static int CDECL _umask_(int m) { int o = P->umask; P->umask = m; return o; }
static int CDECL _getpid_(void) { return P->pid; }

/* FILE streams on descriptors */
static FILE32 *stream_on_handle(uintptr_t h, int fd, const char *mode) {
    xfile_t *x = w32_alloc(P, sizeof(xfile_t)); memset(x, 0, sizeof *x);
    x->h = h; x->unget = -1; x->f.file = fd;
    if (mode && mode[0] == 'a') K32(DWORD (WINAPI *)(HANDLE, LONG, LONG *, DWORD), "SetFilePointer")(h, 0, NULL, 2);
    return &x->f;
}
static FILE32 *CDECL _fdopen_(int fd, const char *mode) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    if (!h) { set_errno(E_BADF); return NULL; }
    if (fd <= 2 && is_console_h(h)) return &iob[fd];
    return stream_on_handle(h, fd, mode);
}
static FILE32 *CDECL _wfdopen_(int fd, const WCHAR *mode) { char m[16]; w2a_buf(mode, m, sizeof m); return _fdopen_(fd, m); }
static FILE32 *CDECL _wfopen_(const WCHAR *name, const WCHAR *mode) { char a[512], m[16]; w2a_buf(name, a, sizeof a); w2a_buf(mode, m, sizeof m); return fopen_(a, m); }
static FILE32 *CDECL _fsopen_(const char *name, const char *mode, int sh) { return fopen_(name, mode); }
static FILE32 *CDECL _wfsopen_(const WCHAR *name, const WCHAR *mode, int sh) { return _wfopen_(name, mode); }
static int CDECL fopen_s_(FILE32 **f, const char *name, const char *mode) { if (!f) return E_INVAL; *f = fopen_(name, mode); return *f ? 0 : P->errno_val ? P->errno_val : E_NOENT; }
static int CDECL _wfopen_s_(FILE32 **f, const WCHAR *name, const WCHAR *mode) { if (!f) return E_INVAL; *f = _wfopen_(name, mode); return *f ? 0 : E_NOENT; }
static FILE32 *CDECL freopen_(const char *name, const char *mode, FILE32 *f) {
    if (!f) return NULL;
    if (IS_STD(f)) {                                         /* stdout/stderr/stdin to a file: through the descriptor */
        int fd = f == F_STDIN ? 0 : f == F_STDOUT ? 1 : 2;
        if (!name) return f;
        int of = mode[0] == 'r' ? 0 : mode[0] == 'w' ? 0x301 : 0x109;
        if (strchr(mode, '+')) of = (of & ~3) | 2;
        int nfd = open_common(name, of);
        if (nfd < 0) return NULL;
        _dup2_(nfd, fd); _close_(nfd);
        return f;
    }
    xfile_t *x = (xfile_t *)f;
    FILE32 *n = fopen_(name, mode);
    if (!n) return NULL;
    K32(BOOL (WINAPI *)(HANDLE), "CloseHandle")(x->h);
    xfile_t *y = (xfile_t *)n;
    x->h = y->h; x->eof = x->err = false; x->unget = -1; x->f.file = y->f.file;
    w32_free(P, y);
    return f;
}
static int CDECL freopen_s_(FILE32 **r, const char *name, const char *mode, FILE32 *f) { FILE32 *n = freopen_(name, mode, f); if (r) *r = n; return n ? 0 : E_NOENT; }
static int CDECL _wfreopen_s_(FILE32 **r, const WCHAR *name, const WCHAR *mode, FILE32 *f) { char a[512], m[16]; w2a_buf(name, a, sizeof a); w2a_buf(mode, m, sizeof m); return freopen_s_(r, a, m, f); }
static int CDECL _fseeki64_(FILE32 *f, int64_t off, int wh) { return fseek_(f, (wlong_t)off, wh); }
static int64_t CDECL _ftelli64_(FILE32 *f) { return ftell_(f); }
static int CDECL fgetpos_(FILE32 *f, int64_t *pos) { wlong_t t = ftell_(f); if (t < 0) return -1; *pos = t; return 0; }
static int CDECL fsetpos_(FILE32 *f, const int64_t *pos) { return fseek_(f, (wlong_t)*pos, 0); }
static void CDECL _lock_file_(FILE32 *f) {}
static void CDECL _unlock_file_(FILE32 *f) {}
static int CDECL _fflush_nolock_(FILE32 *f) { return 0; }
static FILE32 *CDECL tmpfile_(void) {
    static int n; char name[64]; snprintf(name, sizeof name, "C:\\tmp\\crt%d_%d.tmp", P->pid, ++n);
    K32(BOOL (WINAPI *)(const char *, void *), "CreateDirectoryA")("C:\\tmp", NULL);
    return fopen_(name, "w+b");
}
static char *CDECL tmpnam_(char *b) { static char s[64]; static int n; if (!b) b = s; snprintf(b, 64, "C:\\tmp\\t%d_%d", P->pid, ++n); return b; }
static int CDECL _unlink_(const char *n) { return remove_(n) ? set_errno(E_NOENT) : 0; }
static int CDECL _wunlink_(const WCHAR *n) { char a[512]; w2a_buf(n, a, sizeof a); return _unlink_(a); }
static int CDECL _wremove_(const WCHAR *n) { return _wunlink_(n); }
static int CDECL _wrename_(const WCHAR *a, const WCHAR *b) { char x[512], y[512]; w2a_buf(a, x, sizeof x); w2a_buf(b, y, sizeof y); return rename_(x, y); }
static int CDECL _chmod_(const char *n, int m) { return _access(n, 0) ? set_errno(E_NOENT) : 0; }
static int CDECL _wchmod_(const WCHAR *n, int m) { char a[512]; w2a_buf(n, a, sizeof a); return _chmod_(a, m); }
static int CDECL _waccess_(const WCHAR *n, int m) { char a[512]; w2a_buf(n, a, sizeof a); return _access(a, m); }
static int CDECL _wmkdir_(const WCHAR *n) { char a[512]; w2a_buf(n, a, sizeof a); return _mkdir(a); }
static int CDECL _rmdir_(const char *n) { return K32(BOOL (WINAPI *)(const char *), "RemoveDirectoryA")(n) ? 0 : set_errno(E_NOENT); }
static int CDECL _wrmdir_(const WCHAR *n) { char a[512]; w2a_buf(n, a, sizeof a); return _rmdir_(a); }
static int CDECL _wchdir_(const WCHAR *n) { char a[512]; w2a_buf(n, a, sizeof a); return _chdir(a); }
static WCHAR *CDECL _wgetcwd_(WCHAR *b, int n) {
    w32_proc_t *p = P;
    if (!b) { n = MAX(n, (int)strlen(p->cwd) + 1); b = w32_alloc(p, n * 2); }
    int i = 0; for (; p->cwd[i] && i < n - 1; i++) b[i] = (uint8_t)p->cwd[i]; b[i] = 0;
    return b;
}
static char *CDECL _getdcwd_(int drive, char *b, int n) { return _getcwd(b, n); }
static int CDECL _getdrive_(void) { const char *c = P->cwd; return isalpha(c[0]) ? toupper(c[0]) - 'A' + 1 : 3; }
static char *CDECL _fullpath_(char *buf, const char *rel, size_t n) {
    char t[512];
    if (!rel || !*rel) rel = ".";
    if (isalpha(rel[0]) && rel[1] == ':') strlcpy(t, rel, sizeof t);
    else if (rel[0] == '\\' || rel[0] == '/') snprintf(t, sizeof t, "%c:%s", P->cwd[0] ? P->cwd[0] : 'C', rel);
    else path_join(t, sizeof t, P->cwd, rel);
    for (char *c = t; *c; c++) if (*c == '/') *c = '\\';
    path_normalize(t);
    if (!buf) { n = strlen(t) + 1; buf = malloc_(n); }
    if (strlen(t) >= n) { set_errno(E_RANGE); return NULL; }
    strcpy(buf, t);
    return buf;
}
static WCHAR *CDECL _wfullpath_(WCHAR *buf, const WCHAR *rel, size_t n) {
    char a[512], o[512]; w2a_buf(rel, a, sizeof a);
    if (!_fullpath_(o, a, sizeof o)) return NULL;
    size_t l = strlen(o);
    if (!buf) { n = l + 1; buf = w32_alloc(P, n * 2); }
    if (l >= n) return NULL;
    for (size_t i = 0; i <= l; i++) buf[i] = (uint8_t)o[i];
    return buf;
}
static void CDECL _splitpath_(const char *path, char *drv, char *dir, char *fname, char *ext) {
    const char *p = path;
    if (drv) { if (isalpha(p[0]) && p[1] == ':') { drv[0] = p[0]; drv[1] = ':'; drv[2] = 0; } else drv[0] = 0; }
    if (isalpha(p[0]) && p[1] == ':') p += 2;
    const char *last = NULL; for (const char *c = p; *c; c++) if (*c == '\\' || *c == '/') last = c;
    const char *base = last ? last + 1 : p;
    if (dir) { size_t l = base - p; memcpy(dir, p, l); dir[l] = 0; }
    const char *dot = strrchr(base, '.');
    if (!dot) dot = base + strlen(base);
    if (fname) { size_t l = dot - base; memcpy(fname, base, l); fname[l] = 0; }
    if (ext) strcpy(ext, dot);
}
static void CDECL _makepath_(char *out, const char *drv, const char *dir, const char *fname, const char *ext) {
    out[0] = 0;
    if (drv && *drv) { strcat(out, drv); if (drv[1] != ':') strcat(out, ":"); }
    if (dir && *dir) { strcat(out, dir); char c = dir[strlen(dir) - 1]; if (c != '\\' && c != '/') strcat(out, "\\"); }
    if (fname) strcat(out, fname);
    if (ext && *ext) { if (*ext != '.') strcat(out, "."); strcat(out, ext); }
}

/* ---------------- time ---------------- */
static int64_t days_from_civil(int64_t y, int m, int d) { y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; int yoe = (int)(y - era * 400); int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + doe - 719468; }
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t yy = yoe + era * 400; int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153; *d = doy - (153 * mp + 2) / 5 + 1; *m = mp + (mp < 10 ? 3 : -9); *y = (int)(yy + (*m <= 2));
}
static int64_t rtc_epoch(const rtc_time_t *r) { return days_from_civil(r->year, r->month, r->day) * 86400 + r->hour * 3600 + r->minute * 60 + r->second; }
static void epoch_tm(int64_t t, TM *tm) {
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400; int64_t rem = t - days * 86400;
    int y, m, d; civil_from_days(days, &y, &m, &d);
    tm->year = y - 1900; tm->mon = m - 1; tm->mday = d; tm->hour = (int)(rem / 3600); tm->min = (int)(rem / 60 % 60); tm->sec = (int)(rem % 60);
    tm->wday = (int)((days % 7 + 11) % 7); tm->yday = (int)(days - days_from_civil(y, 1, 1)); tm->isdst = 0;
}
static int64_t tm_epoch(TM *tm) {
    int64_t y = tm->year + 1900 + tm->mon / 12; int m = tm->mon % 12; if (m < 0) { m += 12; y--; }
    int64_t t = days_from_civil(y, m + 1, 1) * 86400 + (int64_t)(tm->mday - 1) * 86400 + tm->hour * 3600 + tm->min * 60 + tm->sec;
    epoch_tm(t, tm);                                              /* normalise the fields, as mktime does */
    return t;
}
/* the clock is kept in local time (rtc_now) and time() reports it as such; gmtime and localtime agree */
static int64_t now_epoch(void) { rtc_time_t r; rtc_now(&r); return rtc_epoch(&r); }
static TM tm2_v;
static TM *CDECL localtime32_(const int32_t *t) { epoch_tm(t ? *t : now_epoch(), &tm2_v); return &tm2_v; }
static TM *CDECL localtime64_(const int64_t *t) { epoch_tm(t ? *t : now_epoch(), &tm2_v); return &tm2_v; }
static int CDECL localtime64_s_(TM *tm, const int64_t *t) { if (!tm || !t) return E_INVAL; epoch_tm(*t, tm); return 0; }
static int CDECL localtime32_s_(TM *tm, const int32_t *t) { if (!tm || !t) return E_INVAL; epoch_tm(*t, tm); return 0; }
static int32_t CDECL mktime32_(TM *tm) { return (int32_t)tm_epoch(tm); }
static int64_t CDECL mktime64_(TM *tm) { return tm_epoch(tm); }
static double CDECL difftime64_(int64_t a, int64_t b) { return (double)(a - b); }
static double CDECL difftime32_(int32_t a, int32_t b) { return (double)(a - b); }
static int32_t CDECL time2_(int32_t *t) { int32_t v = (int32_t)now_epoch(); if (t) *t = v; return v; }
static int64_t CDECL time64_(int64_t *t) { int64_t v = now_epoch(); if (t) *t = v; return v; }
static void CDECL _tzset_(void) {}
static int daylight_v = 0; static wlong_t timezone_v = 0; static char tzn0[] = "UTC", tzn1[] = "UTC"; static char *tzname_v[2] = { tzn0, tzn1 };
static int *CDECL __daylight_(void) { return &daylight_v; }
static wlong_t *CDECL __timezone_(void) { return &timezone_v; }
static char **CDECL __tzname_(void) { return tzname_v; }
static int CDECL _get_timezone_(wlong_t *s) { *s = 0; return 0; }
static int CDECL _get_daylight_(int *d) { *d = 0; return 0; }
static char *asc_buf(const TM *t) {
    static char b[32]; static const char *dn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, *mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(b, sizeof b, "%s %s %2d %02d:%02d:%02d %d\n", dn[(t->wday % 7 + 7) % 7], mn[(t->mon % 12 + 12) % 12], t->mday, t->hour, t->min, t->sec, t->year + 1900);
    return b;
}
static char *CDECL asctime_(const TM *t) { return asc_buf(t); }
static char *CDECL ctime32_(const int32_t *t) { return asc_buf(localtime32_(t)); }
static char *CDECL ctime64_(const int64_t *t) { return asc_buf(localtime64_(t)); }
typedef struct { int32_t time; uint16_t ms; int16_t tz, dst; } TIMEB32;
typedef struct { int64_t time; uint16_t ms; int16_t tz, dst; } TIMEB64;
static void CDECL _ftime32_(TIMEB32 *t) { t->time = (int32_t)now_epoch(); t->ms = timer_ticks() % 1000; t->tz = 0; t->dst = 0; }
static void CDECL _ftime64_(TIMEB64 *t) { t->time = now_epoch(); t->ms = timer_ticks() % 1000; t->tz = 0; t->dst = 0; }
static size_t CDECL wcsftime_(WCHAR *b, size_t n, const WCHAR *f, const TM *t) {
    char ff[256], o[512]; w2a_buf(f, ff, sizeof ff);
    size_t r = strftime_(o, MIN(n, sizeof o), ff, t);
    for (size_t i = 0; i <= r && i < n; i++) b[i] = (uint8_t)o[i];
    return r;
}
static char *CDECL _strdate_(char *b) { TM *t = localtime64_(NULL); snprintf(b, 9, "%02d/%02d/%02d", t->mon + 1, t->mday, t->year % 100); return b; }
static char *CDECL _strtime_(char *b) { TM *t = localtime64_(NULL); snprintf(b, 9, "%02d:%02d:%02d", t->hour, t->min, t->sec); return b; }

/* ---------------- stat and find ---------------- */
/* kind: 0 _stat32, 1 _stat32i64 (_stati64), 2 _stat64i32, 3 _stat64 */
static uint16_t stat_mode(vnode_t *v, const char *name) {
    uint16_t m = v->dir ? 0x4000 | 0x100 | 0x40 : 0x8000 | 0x100;
    if (!v->readonly) m |= 0x80;
    const char *dot = strrchr(name, '.');
    if (!v->dir && dot && (!strcasecmp(dot, ".exe") || !strcasecmp(dot, ".com") || !strcasecmp(dot, ".bat") || !strcasecmp(dot, ".cmd"))) m |= 0x40;
    return m | ((m & 0x1C0) >> 3) | ((m & 0x1C0) >> 6);
}
static void fill_stat(uint8_t *b, int kind, vnode_t *v, const char *name, int drive) {
    memset(b, 0, kind == 0 ? 36 : kind == 3 ? 56 : 48);
    *(uint32_t *)b = drive; *(uint16_t *)(b + 6) = stat_mode(v, name); *(int16_t *)(b + 8) = 1; *(uint32_t *)(b + 16) = drive;
    int64_t t = rtc_epoch(&v->mtime); uint64_t sz = v->dir ? 0 : v->size;
    switch (kind) {
    case 0: *(int32_t *)(b + 20) = (int32_t)sz; *(int32_t *)(b + 24) = *(int32_t *)(b + 28) = *(int32_t *)(b + 32) = (int32_t)t; break;
    case 1: *(int64_t *)(b + 24) = sz; *(int32_t *)(b + 32) = *(int32_t *)(b + 36) = *(int32_t *)(b + 40) = (int32_t)t; break;
    case 2: *(int32_t *)(b + 20) = (int32_t)sz; *(int64_t *)(b + 24) = *(int64_t *)(b + 32) = *(int64_t *)(b + 40) = t; break;
    default: *(int64_t *)(b + 24) = sz; *(int64_t *)(b + 32) = *(int64_t *)(b + 40) = *(int64_t *)(b + 48) = t; break;
    }
}
static int stat_path(const char *name, void *buf, int kind) {
    char full[512];
    if (!name || !_fullpath_(full, name, sizeof full)) return set_errno(E_NOENT);
    size_t l = strlen(full);
    if (l > 3 && full[l - 1] == '\\') full[l - 1] = 0;
    vnode_t *v = vfs_resolve(full, NULL);
    if (!v) return set_errno(E_NOENT);
    fill_stat(buf, kind, v, full, toupper(full[0]) - 'A');
    return 0;
}
static int stat_wpath(const WCHAR *name, void *buf, int kind) { char a[512]; w2a_buf(name, a, sizeof a); return stat_path(a, buf, kind); }
static int stat_fd(int fd, void *buf, int kind) {
    w32_proc_t *p = P; uintptr_t h = fd_handle(p, fd);
    if (!h) return set_errno(E_BADF);
    if (is_console_h(h)) { memset(buf, 0, kind == 0 ? 36 : kind == 3 ? 56 : 48); *(uint16_t *)((uint8_t *)buf + 6) = 0x2000 | 0x1B6; *(int16_t *)((uint8_t *)buf + 8) = 1; return 0; }
    vnode_t *v = w32_handle_node(p, h);
    if (!v) return set_errno(E_BADF);
    fill_stat(buf, kind, v, v->name, 2);
    return 0;
}
static int CDECL _stat32_(const char *n, void *b) { return stat_path(n, b, 0); }
static int CDECL _stat32i64_(const char *n, void *b) { return stat_path(n, b, 1); }
static int CDECL _stat64i32_(const char *n, void *b) { return stat_path(n, b, 2); }
static int CDECL _stat64_(const char *n, void *b) { return stat_path(n, b, 3); }
static int CDECL _wstat32_(const WCHAR *n, void *b) { return stat_wpath(n, b, 0); }
static int CDECL _wstat32i64_(const WCHAR *n, void *b) { return stat_wpath(n, b, 1); }
static int CDECL _wstat64i32_(const WCHAR *n, void *b) { return stat_wpath(n, b, 2); }
static int CDECL _wstat64_(const WCHAR *n, void *b) { return stat_wpath(n, b, 3); }
static int CDECL _fstat32_(int fd, void *b) { return stat_fd(fd, b, 0); }
static int CDECL _fstat32i64_(int fd, void *b) { return stat_fd(fd, b, 1); }
static int CDECL _fstat64i32_(int fd, void *b) { return stat_fd(fd, b, 2); }
static int CDECL _fstat64_(int fd, void *b) { return stat_fd(fd, b, 3); }

/* _findfirst: kind as for stat (0 _finddata32_t, 1 _finddata32i64_t, 2 _finddata64i32_t, 3 _finddata64_t); wide names are WCHAR[260] */
typedef struct { DWORD attr; uint32_t ct[2], at[2], wt[2]; DWORD szhi, szlo, r0, r1; char name[260]; char alt[14]; } FINDA;
static int64_t ft_epoch(const uint32_t *ft) { uint64_t v = ((uint64_t)ft[1] << 32) | ft[0]; return v ? (int64_t)((v - 116444736000000000ull) / 10000000ull) : 0; }
static void fill_find(uint8_t *b, int kind, bool wide, const FINDA *f) {
    int64_t t = ft_epoch(f->wt); uint64_t sz = ((uint64_t)f->szhi << 32) | f->szlo;
    int name_off;
    *(uint32_t *)b = f->attr;
    switch (kind) {
    case 0: *(int32_t *)(b + 4) = *(int32_t *)(b + 8) = *(int32_t *)(b + 12) = (int32_t)t; *(uint32_t *)(b + 16) = (uint32_t)sz; name_off = 20; break;
    case 1: *(int32_t *)(b + 4) = *(int32_t *)(b + 8) = *(int32_t *)(b + 12) = (int32_t)t; *(uint64_t *)(b + 16) = sz; name_off = 24; break;
    case 2: *(int64_t *)(b + 8) = *(int64_t *)(b + 16) = *(int64_t *)(b + 24) = t; *(uint32_t *)(b + 32) = (uint32_t)sz; name_off = 36; break;
    default: *(int64_t *)(b + 8) = *(int64_t *)(b + 16) = *(int64_t *)(b + 24) = t; *(uint64_t *)(b + 32) = sz; name_off = 40; break;
    }
    if (wide) { WCHAR *w = (WCHAR *)(b + name_off); int i = 0; for (; f->name[i] && i < 259; i++) w[i] = (uint8_t)f->name[i]; w[i] = 0; }
    else strlcpy((char *)(b + name_off), f->name, 260);
}
static intptr_t find_first(const char *spec, void *out, int kind, bool wide) {
    FINDA f;
    HANDLE h = K32(HANDLE (WINAPI *)(const char *, FINDA *), "FindFirstFileA")(spec, &f);
    if ((uint32_t)h == 0xFFFFFFFFu) { set_errno(E_NOENT); return -1; }
    fill_find(out, kind, wide, &f);
    return (intptr_t)h;
}
static int find_next(intptr_t h, void *out, int kind, bool wide) {
    FINDA f;
    if (!K32(BOOL (WINAPI *)(HANDLE, FINDA *), "FindNextFileA")((HANDLE)h, &f)) return set_errno(E_NOENT);
    fill_find(out, kind, wide, &f);
    return 0;
}
static intptr_t CDECL _findfirst32_(const char *s, void *o) { return find_first(s, o, 0, false); }
static intptr_t CDECL _findfirst32i64_(const char *s, void *o) { return find_first(s, o, 1, false); }
static intptr_t CDECL _findfirst64i32_(const char *s, void *o) { return find_first(s, o, 2, false); }
static intptr_t CDECL _findfirst64_(const char *s, void *o) { return find_first(s, o, 3, false); }
static int CDECL _findnext32_(intptr_t h, void *o) { return find_next(h, o, 0, false); }
static int CDECL _findnext32i64_(intptr_t h, void *o) { return find_next(h, o, 1, false); }
static int CDECL _findnext64i32_(intptr_t h, void *o) { return find_next(h, o, 2, false); }
static int CDECL _findnext64_(intptr_t h, void *o) { return find_next(h, o, 3, false); }
static intptr_t CDECL _wfindfirst32_(const WCHAR *s, void *o) { char a[512]; w2a_buf(s, a, sizeof a); return find_first(a, o, 0, true); }
static intptr_t CDECL _wfindfirst64i32_(const WCHAR *s, void *o) { char a[512]; w2a_buf(s, a, sizeof a); return find_first(a, o, 2, true); }
static intptr_t CDECL _wfindfirst64_(const WCHAR *s, void *o) { char a[512]; w2a_buf(s, a, sizeof a); return find_first(a, o, 3, true); }
static int CDECL _wfindnext32_(intptr_t h, void *o) { return find_next(h, o, 0, true); }
static int CDECL _wfindnext64i32_(intptr_t h, void *o) { return find_next(h, o, 2, true); }
static int CDECL _wfindnext64_(intptr_t h, void *o) { return find_next(h, o, 3, true); }
static int CDECL _findclose_(intptr_t h) { return K32(BOOL (WINAPI *)(HANDLE), "FindClose")((HANDLE)h) ? 0 : -1; }

/* ---------------- processes ---------------- */
static int spawn_wait(int mode, const char *cmdline) {
    typedef struct { HANDLE proc, thread; DWORD pid, tid; } PI;
    struct { DWORD cb; uint8_t rest[100]; } si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PI pi; char cmd[1024]; strlcpy(cmd, cmdline, sizeof cmd);
    if (!K32(BOOL (WINAPI *)(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, void *, PI *), "CreateProcessA")(NULL, cmd, NULL, NULL, 0, 0, NULL, NULL, &si, &pi))
        return set_errno(E_NOENT);
    if (mode == 0) {                                          /* _P_WAIT: the exit code */
        K32(DWORD (WINAPI *)(HANDLE, DWORD), "WaitForSingleObject")(pi.proc, 0xFFFFFFFF);
        DWORD c = 0; K32(BOOL (WINAPI *)(HANDLE, DWORD *), "GetExitCodeProcess")(pi.proc, &c);
        return (int)c;
    }
    return mode == 2 ? (w32_exit_process(0), 0) : (int)(intptr_t)pi.proc;   /* _P_OVERLAY replaces this program */
}
static void join_argv(char *out, int cap, const char *name, const char *const *argv) {
    out[0] = 0;
    for (int i = 0; argv && argv[i]; i++) {
        const char *a = i == 0 ? name : argv[i];
        bool q = !*a || strchr(a, ' ');
        if (i) strlcat(out, " ", cap);
        if (q) strlcat(out, "\"", cap);
        strlcat(out, a, cap);
        if (q) strlcat(out, "\"", cap);
    }
    if (!argv || !argv[0]) strlcpy(out, name, cap);
}
static intptr_t CDECL _spawnv_(int mode, const char *name, const char *const *argv) { char c[1024]; join_argv(c, sizeof c, name, argv); return spawn_wait(mode, c); }
static intptr_t CDECL _spawnve_(int mode, const char *name, const char *const *argv, const char *const *env) { return _spawnv_(mode, name, argv); }
static intptr_t CDECL _spawnl_(int mode, const char *name, const char *a0, ...) {
    const char *argv[32]; int n = 0; argv[n++] = a0;
    w32_va_list ap; w32_va_start(ap, a0); const char *a; while (n < 31 && (a = va_arg(ap, const char *))) argv[n++] = a; w32_va_end(ap);
    argv[n] = NULL; return _spawnv_(mode, name, argv);
}
static intptr_t CDECL _execv_(const char *name, const char *const *argv) { return _spawnv_(2, name, argv); }
static intptr_t CDECL _wspawnv_(int mode, const WCHAR *name, const WCHAR *const *argv) {
    char n[512], c[1024] = ""; w2a_buf(name, n, sizeof n);
    for (int i = 0; argv && argv[i]; i++) { char a[512]; w2a_buf(i ? argv[i] : name, a, sizeof a); bool q = strchr(a, ' ') != NULL; if (i) strlcat(c, " ", sizeof c); if (q) strlcat(c, "\"", sizeof c); strlcat(c, a, sizeof c); if (q) strlcat(c, "\"", sizeof c); }
    if (!c[0]) strlcpy(c, n, sizeof c);
    return spawn_wait(mode, c);
}
static intptr_t CDECL _wspawnve_(int mode, const WCHAR *name, const WCHAR *const *argv, const WCHAR *const *env) { return _wspawnv_(mode, name, argv); }
static intptr_t CDECL _wexecv_(const WCHAR *name, const WCHAR *const *argv) { return _wspawnv_(2, name, argv); }
static intptr_t CDECL _wexecve_(const WCHAR *name, const WCHAR *const *argv, const WCHAR *const *env) { return _wspawnv_(2, name, argv); }
static intptr_t CDECL _cwait_(int *st, intptr_t h, int act) {
    K32(DWORD (WINAPI *)(HANDLE, DWORD), "WaitForSingleObject")((HANDLE)h, 0xFFFFFFFF);
    DWORD c = 0; K32(BOOL (WINAPI *)(HANDLE, DWORD *), "GetExitCodeProcess")((HANDLE)h, &c);
    if (st) *st = (int)c; return h;
}
static int CDECL _wsystem_(const WCHAR *c) { if (!c) return 1; char a[1024]; w2a_buf(c, a, sizeof a); return system_(a); }
static FILE32 *CDECL _popen_(const char *c, const char *m) { set_errno(E_INVAL); return NULL; }
static int CDECL _pclose_(FILE32 *f) { return -1; }

/* ---------------- environment ---------------- */
static int env_set(const char *name, const char *val) {
    w32_proc_t *p = P;
    int n = 0; while (p->envp && p->envp[n]) n++;
    size_t nl = strlen(name);
    for (int i = 0; i < n; i++) if (!strncasecmp(p->envp[i], name, nl) && p->envp[i][nl] == '=') {
        if (!val || !*val) { for (int k = i; k < n; k++) p->envp[k] = p->envp[k + 1]; return 0; }
        char *e = kmalloc(nl + strlen(val) + 2); sprintf(e, "%s=%s", name, val); p->envp[i] = e; return 0;
    }
    if (!val || !*val) return 0;
    char **ne = kzalloc((n + 2) * sizeof(char *)); if (n) memcpy(ne, p->envp, n * sizeof(char *));
    char *e = kmalloc(nl + strlen(val) + 2); sprintf(e, "%s=%s", name, val); ne[n] = e;
    kfree(p->envp); p->envp = ne;
    p->wenvp = NULL;
    return 0;
}
static int CDECL _putenv_(const char *s) { const char *eq = strchr(s, '='); if (!eq) return -1; char n[128]; size_t l = MIN((size_t)(eq - s), sizeof n - 1); memcpy(n, s, l); n[l] = 0; return env_set(n, eq + 1); }
static int CDECL _putenv_s_(const char *n, const char *v) { return env_set(n, v); }
static int CDECL _wputenv_(const WCHAR *s) { char a[1024]; w2a_buf(s, a, sizeof a); return _putenv_(a); }
static int CDECL _wputenv_s_(const WCHAR *n, const WCHAR *v) { char a[128], b[1024]; w2a_buf(n, a, sizeof a); w2a_buf(v, b, sizeof b); return env_set(a, b); }
static WCHAR *CDECL _wgetenv_(const WCHAR *n) {
    static WCHAR b[4][512]; static int k;
    char a[128]; w2a_buf(n, a, sizeof a);
    char *v = getenv_(a); if (!v) return NULL;
    WCHAR *o = b[k++ & 3]; int i = 0; for (; v[i] && i < 511; i++) o[i] = (uint8_t)v[i]; o[i] = 0; return o;
}
static int CDECL getenv_s_(size_t *rq, char *b, size_t n, const char *name) { char *v = getenv_(name); if (!v) { if (rq) *rq = 0; if (b && n) b[0] = 0; return 0; } size_t l = strlen(v) + 1; if (rq) *rq = l; if (b && n >= l) memcpy(b, v, l); return b && n < l ? E_RANGE : 0; }
static int CDECL _dupenv_s_(char **b, size_t *n, const char *name) { char *v = getenv_(name); *b = v ? _strdup(v) : NULL; if (n) *n = v ? strlen(v) + 1 : 0; return 0; }
static void make_wide_args(w32_proc_t *p) {
    if (!p->wargv) { p->wargv = w32_alloc(p, (p->argc + 1) * sizeof(WCHAR *)); for (int i = 0; i < p->argc; i++) p->wargv[i] = wdup_a(p->argv[i]); p->wargv[p->argc] = NULL; }
    if (!p->wenvp) { int n = 0; while (p->envp && p->envp[n]) n++; p->wenvp = w32_alloc(p, (n + 1) * sizeof(WCHAR *)); for (int i = 0; i < n; i++) p->wenvp[i] = wdup_a(p->envp[i]); p->wenvp[n] = NULL; }
    if (!p->wcmdln) p->wcmdln = wdup_a(p->cmdline);
}
static WCHAR **wargv_v, **wenv_v;
static WCHAR ***CDECL __p___wargv_(void) { w32_proc_t *p = P; make_wide_args(p); wargv_v = p->wargv; return &wargv_v; }
static WCHAR ***CDECL __p__wenviron_(void) { w32_proc_t *p = P; make_wide_args(p); wenv_v = p->wenvp; return &wenv_v; }
static int CDECL __wgetmainargs_(int *argc, WCHAR ***argv, WCHAR ***env, int wild, void *si) { w32_proc_t *p = P; make_wide_args(p); *argc = p->argc; *argv = p->wargv; *env = p->wenvp; return 0; }
static WCHAR **CDECL _get_initial_wide_environment_(void) { w32_proc_t *p = P; make_wide_args(p); return p->wenvp; }
static char **CDECL _get_initial_narrow_environment_(void) { return P->envp; }
static WCHAR *CDECL _get_wide_winmain_command_line_(void) {
    w32_proc_t *p = P; make_wide_args(p);
    WCHAR *c = p->wcmdln;
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; } else while (*c > ' ') c++;
    while (*c == ' ' || *c == '\t') c++;
    return c;
}
static char *CDECL _get_narrow_winmain_command_line_(void) {
    char *c = P->acmdln;
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; } else while (*c > ' ') c++;
    while (*c == ' ' || *c == '\t') c++;
    return c;
}
static int CDECL _configure_argv_(int mode) { return 0; }
static int CDECL _initialize_environment_(void) { return 0; }
static int *CDECL __p___argc2_(void) { return __p___argc(); }

/* ---------------- Universal CRT start-up ---------------- */
typedef struct { void (**first)(void); void (**last)(void); void (**end)(void); } onexit_table_t;
static int CDECL _initialize_onexit_table_(onexit_table_t *t) { if (!t) return -1; if (!t->first) t->first = t->last = t->end = NULL; return 0; }
static int CDECL _register_onexit_function_(onexit_table_t *t, void (*f)(void)) {
    if (!t) return -1;
    if (t->last == t->end) {
        size_t n = t->first ? t->end - t->first : 0, nn = n ? n * 2 : 32;
        void (**b)(void) = realloc_(t->first, nn * sizeof(void *));
        if (!b) return -1;
        t->last = b + (t->last - t->first); t->first = b; t->end = b + nn;
    }
    *t->last++ = f;
    return 0;
}
static int CDECL _execute_onexit_table_(onexit_table_t *t) {
    if (!t || !t->first) return 0;
    void (**f)(void) = t->first, (**l)(void) = t->last;
    t->first = t->last = t->end = NULL;
    while (l > f) { --l; if (*l) (*l)(); }
    free_(f);
    return 0;
}
static int CDECL _crt_atexit_(void (*f)(void)) { return atexit_(f); }
static int CDECL _crt_at_quick_exit_(void (*f)(void)) { return 0; }
static void CDECL _register_thread_local_exe_atexit_callback_(void *f) {}
static void CDECL _set_app_type_(int t) {}
static int CDECL _set_fmode_(int m) { P->fmode = m; return 0; }
static int CDECL _get_fmode_(int *m) { *m = P->fmode; return 0; }
static int CDECL _set_new_mode_(int m) { return 0; }
static int CDECL _query_new_mode_(void) { return 0; }
static unsigned CDECL _set_abort_behavior_(unsigned a, unsigned b) { return 0; }
static int CDECL _seh_filter_(unsigned code, void *ex) { return 0; }                /* EXCEPTION_CONTINUE_SEARCH */
static void *CDECL _set_thread_local_invalid_parameter_handler_(void *h) { return NULL; }
static void *CDECL _get_invalid_parameter_handler_(void) { return NULL; }
static void CDECL _invalid_parameter_noinfo_(void) { klog("w32: pid %d: invalid parameter passed to a C runtime function", P->pid); }
static void CDECL _invalid_parameter_noinfo_noreturn_(void) { _invalid_parameter_noinfo_(); abort_(); }
static void CDECL _invoke_watson_(const void *a, const void *b, const void *c, unsigned d, uintptr_t e) { abort_(); }
static int CDECL __lconv_init_(void) { return 0; }
static int CDECL __control87_2_(unsigned n, unsigned m, unsigned *x87, unsigned *sse) { if (x87) *x87 = 0x9001F; if (sse) *sse = 0x9001F; return 1; }
static unsigned CDECL _control87_(unsigned n, unsigned m) { return 0x9001F; }
static int CDECL _controlfp_s_(unsigned *cur, unsigned n, unsigned m) { if (cur) *cur = 0x9001F; return 0; }
static unsigned CDECL _clearfp_(void) { return 0; }
static unsigned CDECL _statusfp_(void) { return 0; }
static int CDECL __fpe_flt_rounds_(void) { return 1; }
static int CDECL _except1_(void) { return 0; }
static int CDECL _resetstkoflw_(void) { return 1; }
static int CDECL _heapmin_(void) { return 0; }
static int CDECL _heapchk_(void) { return -2; }                                     /* _HEAPOK */
static int CDECL _callnewh_(size_t n) { return 0; }
static int CDECL _get_errno_(int *e) { *e = P->errno_val; return 0; }
static int CDECL _set_errno_(int e) { P->errno_val = e; return 0; }
static unsigned long *CDECL __doserrno_(void) { static unsigned long d; return &d; }
static int CDECL _get_pgmptr_(char **p) { *p = P->path; return 0; }
static int CDECL _get_wpgmptr_(WCHAR **p) { static WCHAR *w; w = wdup_a(P->path); *p = w; return 0; }
static int winmajor_v = 6, winminor_v = 1, winver_v = 0x601, osver_v = 7601;
static int CDECL __uncaught_exception_(void) { return 0; }
static void *CDECL __current_exception_(void) { static void *e; return &e; }
static void *CDECL __current_exception_context_(void) { static void *e; return &e; }
static void *CDECL __processing_throw_(void) { static int e; return &e; }
static int CDECL _isctype_(int c, int m) {
    int r = 0;
    if (isupper_(c)) r |= 1; if (islower_(c)) r |= 2; if (isdigit(c)) r |= 4; if (isspace(c)) r |= 8;
    if (ispunct_(c)) r |= 0x10; if (iscntrl_(c)) r |= 0x20; if (c == ' ') r |= 0x40; if (isxdigit(c)) r |= 0x80; if (isalpha(c)) r |= 0x100;
    return r & m;
}
static uint16_t ctype_tab[257];
static const uint16_t *pctype_v;
static void init_ctype(void) { if (!ctype_tab[1 + 'A']) { for (int c = 0; c < 256; c++) ctype_tab[1 + c] = c < 128 ? _isctype_(c, 0x1FF) : 0; } pctype_v = ctype_tab + 1; }
static const uint16_t **CDECL __pctype_func_(void) { init_ctype(); return &pctype_v; }
static const uint16_t *CDECL __pwctype_func_(void) { init_ctype(); return ctype_tab + 1; }

/* ---------------- printf and scanf entry points of the Universal CRT ---------------- */
static int fmt_dyn(char **out, const char *fmt, w32_va_list ap) {        /* formats into a buffer big enough */
    static char small[2048];
    w32_va_list a2; w32_va_copy(a2, ap);
    int n = w32_vformat(small, sizeof small, fmt, ap);
    if (n < (int)sizeof small) { *out = small; return n; }
    char *b = kmalloc(n + 1);
    if (!b) { *out = small; return sizeof small - 1; }
    w32_vformat(b, n + 1, fmt, a2);
    *out = b; return n;
}
static int vfprintf2_(FILE32 *f, const char *fmt, w32_va_list ap) {
    char *b; int n = fmt_dyn(&b, fmt, ap);
    int w = stream_write(f, b, n);
    if (n >= 2048) kfree(b);
    return w < 0 ? -1 : n;
}
static int CDECL __stdio_common_vfprintf_(uint64_t opt, FILE32 *f, const char *fmt, void *loc, w32_va_list ap) { return vfprintf2_(f, fmt, ap); }
static int CDECL __stdio_common_vsprintf_(uint64_t opt, char *b, size_t n, const char *fmt, void *loc, w32_va_list ap) {
    int r = w32_vformat(b, b ? n : 0, fmt, ap);
    if (!b) return r;
    if (r >= (int)n && n) { if (opt & 2) return r; b[n - 1] = 0; return (opt & 1) ? -1 : -1; }   /* legacy: -1 when it does not fit */
    return r;
}
static int CDECL __stdio_common_vsprintf_s_(uint64_t opt, char *b, size_t n, const char *fmt, void *loc, w32_va_list ap) {
    int r = w32_vformat(b, n, fmt, ap);
    if (r >= (int)n) { if (n) b[0] = 0; return -1; }
    return r;
}
static int CDECL __stdio_common_vsnprintf_s_(uint64_t opt, char *b, size_t n, size_t cnt, const char *fmt, void *loc, w32_va_list ap) {
    size_t lim = cnt == (size_t)-1 ? n : MIN(n, cnt + 1);
    int r = w32_vformat(b, lim, fmt, ap);
    if (r >= (int)lim) return cnt == (size_t)-1 ? -1 : -1;
    return r;
}
static int CDECL vsscanf_(const char *s, const char *fmt, w32_va_list ap) { scan_src_t src = { s, NULL, -1 }; return vscan(&src, fmt, ap); }
static int CDECL __stdio_common_vsscanf_(uint64_t opt, const char *s, size_t n, const char *fmt, void *loc, w32_va_list ap) { return vsscanf_(s, fmt, ap); }
static int CDECL __stdio_common_vfscanf_(uint64_t opt, FILE32 *f, const char *fmt, void *loc, w32_va_list ap) { scan_src_t s = { NULL, f, -1 }; int r = vscan(&s, fmt, ap); if (s.peek >= 0) ungetc_(s.peek, f); return r; }
static int CDECL vfscanf_(FILE32 *f, const char *fmt, w32_va_list ap) { return __stdio_common_vfscanf_(0, f, fmt, NULL, ap); }
static int CDECL vscanf_(const char *fmt, w32_va_list ap) { return __stdio_common_vfscanf_(0, F_STDIN, fmt, NULL, ap); }

/* wide printf: the format is rewritten for the narrow engine (in a wide format %s and %c are wide,
 * %S and %C narrow), the result widened */
static void wfmt_to_narrow(const WCHAR *w, char *f, int cap) {
    int k = 0;
    for (int i = 0; w[i] && k < cap - 8; i++) {
        WCHAR c = w[i];
        if (c != '%') { f[k++] = c < 256 ? (char)c : '?'; continue; }
        f[k++] = '%'; i++;
        if (w[i] == '%') { f[k++] = '%'; continue; }
        bool h = false, l = false;
        while (w[i] && strchr("-+ #0123456789.*", (char)w[i])) f[k++] = (char)w[i++];
        for (;;) {
            if (w[i] == 'h') { h = true; i++; }
            else if (w[i] == 'l' || w[i] == 'w') { l = true; f[k++] = 'l'; i++; }
            else if (w[i] == 'L' || w[i] == 'z' || w[i] == 'j' || w[i] == 't') f[k++] = (char)w[i++];
            else if (w[i] == 'I') { f[k++] = 'I'; i++; while (w[i] >= '0' && w[i] <= '9') f[k++] = (char)w[i++]; }
            else break;
        }
        c = w[i];
        if (!c) break;
        if (c == 's' || c == 'c') { if (!h && !l) f[k++] = 'l'; f[k++] = (char)c; }
        else if (c == 'S' || c == 'C') f[k++] = c == 'S' ? 's' : 'c';
        else { if (h) f[k++] = 'h'; f[k++] = (char)c; }
    }
    f[k] = 0;
}
static int vswfmt(WCHAR *out, size_t cap, const WCHAR *wf, w32_va_list ap) {
    char f[1024]; wfmt_to_narrow(wf, f, sizeof f);
    char *b; int n = fmt_dyn(&b, f, ap);
    if (out && cap) { size_t m = MIN((size_t)n, cap - 1); for (size_t i = 0; i < m; i++) out[i] = (uint8_t)b[i]; out[m] = 0; }
    if (n >= 2048) kfree(b);
    return n;
}
static int CDECL __stdio_common_vswprintf_(uint64_t opt, WCHAR *b, size_t n, const WCHAR *fmt, void *loc, w32_va_list ap) {
    int r = vswfmt(b, b ? n : 0, fmt, ap);
    if (b && r >= (int)n && !(opt & 2)) return -1;
    return r;
}
static int CDECL __stdio_common_vswprintf_s_(uint64_t opt, WCHAR *b, size_t n, const WCHAR *fmt, void *loc, w32_va_list ap) { int r = vswfmt(b, n, fmt, ap); if (r >= (int)n) { if (n) b[0] = 0; return -1; } return r; }
static int CDECL __stdio_common_vfwprintf_(uint64_t opt, FILE32 *f, const WCHAR *fmt, void *loc, w32_va_list ap) {
    char nf[1024]; wfmt_to_narrow(fmt, nf, sizeof nf);
    return vfprintf2_(f, nf, ap);
}
static int CDECL vfwprintf_(FILE32 *f, const WCHAR *fmt, w32_va_list ap) { return __stdio_common_vfwprintf_(0, f, fmt, NULL, ap); }
static int CDECL fwprintf_(FILE32 *f, const WCHAR *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = vfwprintf_(f, fmt, ap); w32_va_end(ap); return r; }
static int CDECL vwprintf_(const WCHAR *fmt, w32_va_list ap) { return vfwprintf_(F_STDOUT, fmt, ap); }
static int CDECL wprintf_(const WCHAR *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = vfwprintf_(F_STDOUT, fmt, ap); w32_va_end(ap); return r; }
static int CDECL _vsnwprintf_(WCHAR *b, size_t n, const WCHAR *fmt, w32_va_list ap) { int r = vswfmt(b, n, fmt, ap); return r >= (int)n ? -1 : r; }
static int CDECL _snwprintf_(WCHAR *b, size_t n, const WCHAR *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = _vsnwprintf_(b, n, fmt, ap); w32_va_end(ap); return r; }
static int CDECL vswprintf_(WCHAR *b, size_t n, const WCHAR *fmt, w32_va_list ap) { return vswfmt(b, n, fmt, ap); }
static int CDECL swprintf_(WCHAR *b, size_t n, const WCHAR *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = vswfmt(b, n, fmt, ap); w32_va_end(ap); return r; }
static int CDECL _swprintf_(WCHAR *b, const WCHAR *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = vswfmt(b, 0x7FFFFFFF, fmt, ap); w32_va_end(ap); return r; }
static int CDECL _vscwprintf_(const WCHAR *fmt, w32_va_list ap) { return vswfmt(NULL, 0, fmt, ap); }
static int CDECL _vscprintf_(const char *fmt, w32_va_list ap) { return w32_vformat(NULL, 0, fmt, ap); }
static int CDECL _scprintf_(const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = w32_vformat(NULL, 0, fmt, ap); w32_va_end(ap); return r; }
static int CDECL sprintf_s_(char *b, size_t n, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = w32_vformat(b, n, fmt, ap); w32_va_end(ap); if (r >= (int)n) { if (n) b[0] = 0; return -1; } return r; }
static int CDECL vsprintf_s_(char *b, size_t n, const char *fmt, w32_va_list ap) { int r = w32_vformat(b, n, fmt, ap); if (r >= (int)n) { if (n) b[0] = 0; return -1; } return r; }
static int CDECL _snprintf_s_(char *b, size_t n, size_t c, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = __stdio_common_vsnprintf_s_(0, b, n, c, fmt, NULL, ap); w32_va_end(ap); return r; }
static int CDECL _vsnprintf_s_(char *b, size_t n, size_t c, const char *fmt, w32_va_list ap) { return __stdio_common_vsnprintf_s_(0, b, n, c, fmt, NULL, ap); }
static int CDECL swscanf_(const WCHAR *s, const WCHAR *fmt, ...) {
    char a[1024], f[256]; w2a_buf(s, a, sizeof a); w2a_buf(fmt, f, sizeof f);
    w32_va_list ap; w32_va_start(ap, fmt); int r = vsscanf_(a, f, ap); w32_va_end(ap); return r;
}
static int CDECL fputws_(const WCHAR *s, FILE32 *f) { char a[1024]; while (*s) { int k = 0; while (*s && k < 1023) { a[k++] = *s < 256 ? (char)*s : '?'; s++; } if (stream_write(f, a, k) < 0) return -1; } return 0; }
static int CDECL _putws_(const WCHAR *s) { fputws_(s, F_STDOUT); return fputc_('\n', F_STDOUT); }
static WCHAR CDECL fputwc_(WCHAR c, FILE32 *f) { char ch = c < 256 ? (char)c : '?'; return stream_write(f, &ch, 1) < 0 ? 0xFFFF : c; }
static WCHAR CDECL putwchar_(WCHAR c) { return fputwc_(c, F_STDOUT); }
static WCHAR CDECL fgetwc_(FILE32 *f) { int c = stream_getc(f); return c < 0 ? 0xFFFF : (WCHAR)c; }
static WCHAR CDECL getwchar_(void) { return fgetwc_(F_STDIN); }
static WCHAR CDECL ungetwc_(WCHAR c, FILE32 *f) { ungetc_(c, f); return c; }
static WCHAR *CDECL fgetws_(WCHAR *b, int n, FILE32 *f) { int k = 0; while (k < n - 1) { int c = stream_getc(f); if (c < 0) break; b[k++] = c; if (c == '\n') break; } b[k] = 0; return k ? b : NULL; }

/* ---------------- console ---------------- */
static int CDECL _putch_(int c) { char ch = c; w32_console_write(P, 1, &ch, 1); return c; }
static WCHAR CDECL _putwch_(WCHAR c) { _putch_(c < 256 ? c : '?'); return c; }
static int pending_ch = -1;
static int CDECL _getch2_(void) { if (pending_ch >= 0) { int c = pending_ch; pending_ch = -1; return c; } return _getch(); }
static int CDECL _getche_(void) { int c = _getch2_(); if (c > 0) _putch_(c); return c; }
static WCHAR CDECL _getwch_(void) { return (WCHAR)_getch2_(); }
static WCHAR CDECL _getwche_(void) { return (WCHAR)_getche_(); }
static int CDECL _ungetch_(int c) { pending_ch = c; return c; }
static WCHAR CDECL _ungetwch_(WCHAR c) { pending_ch = c; return c; }
static int CDECL _cputs_(const char *s) { w32_console_write(P, 1, s, strlen(s)); return 0; }
static int CDECL _cprintf_(const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = vfprintf2_(F_STDOUT, fmt, ap); w32_va_end(ap); return r; }

/* ---------------- strings ---------------- */
static int64_t CDECL strtoll_(const char *s, char **e, int b) {
    const char *p = s; while (isspace(*p)) p++;
    bool neg = false; if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((b == 0 || b == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit(p[2])) { p += 2; b = 16; }
    else if (b == 0) b = *p == '0' ? 8 : 10;
    uint64_t v = 0; bool any = false;
    for (;; p++) { int d = isdigit(*p) ? *p - '0' : isalpha(*p) ? toupper(*p) - 'A' + 10 : 99; if (d >= b) break; v = v * b + d; any = true; }
    if (e) *e = (char *)(any ? p : s);
    return neg ? -(int64_t)v : (int64_t)v;
}
static uint64_t CDECL strtoull_(const char *s, char **e, int b) { return (uint64_t)strtoll_(s, e, b); }
static int64_t CDECL wcstoll_(const WCHAR *s, WCHAR **e, int b) { char a[128]; w2a_buf(s, a, sizeof a); char *ae; int64_t v = strtoll_(a, &ae, b); if (e) *e = (WCHAR *)(s + (ae - a)); return v; }
static uint64_t CDECL wcstoull_(const WCHAR *s, WCHAR **e, int b) { return (uint64_t)wcstoll_(s, e, b); }
static wlong_t CDECL wcstol_(const WCHAR *s, WCHAR **e, int b) { return (wlong_t)wcstoll_(s, e, b); }
static wulong_t CDECL wcstoul_(const WCHAR *s, WCHAR **e, int b) { return (wulong_t)wcstoll_(s, e, b); }
static double CDECL wcstod_(const WCHAR *s, WCHAR **e) { char a[128]; w2a_buf(s, a, sizeof a); char *ae; double v = strtod_(a, &ae); if (e) *e = (WCHAR *)(s + (ae - a)); return v; }
static float CDECL strtof_(const char *s, char **e) { return (float)strtod_(s, e); }
static int CDECL _wtoi_(const WCHAR *s) { return (int)wcstoll_(s, NULL, 10); }
static int64_t CDECL _wtoi64_(const WCHAR *s) { return wcstoll_(s, NULL, 10); }
static double CDECL _wtof_(const WCHAR *s) { return wcstod_(s, NULL); }
static int64_t CDECL _atoll_(const char *s) { return strtoll_(s, NULL, 10); }
static char *CDECL _i64toa_(int64_t v, char *b, int r) { char t[72]; int k = 0; bool neg = r == 10 && v < 0; uint64_t u = neg ? -(uint64_t)v : (uint64_t)v; do { int d = u % r; t[k++] = d < 10 ? '0' + d : 'a' + d - 10; u /= r; } while (u); int i = 0; if (neg) b[i++] = '-'; while (k) b[i++] = t[--k]; b[i] = 0; return b; }
static char *CDECL _ui64toa_(uint64_t v, char *b, int r) { char t[72]; int k = 0; do { int d = v % r; t[k++] = d < 10 ? '0' + d : 'a' + d - 10; v /= r; } while (v); int i = 0; while (k) b[i++] = t[--k]; b[i] = 0; return b; }
static char *CDECL _itoa_(int v, char *b, int r) { return _i64toa_(r == 10 ? (int64_t)v : (int64_t)(uint32_t)v, b, r); }
static char *CDECL _ltoa_(wlong_t v, char *b, int r) { return _itoa_(v, b, r); }
static char *CDECL _ultoa_(wulong_t v, char *b, int r) { return _ui64toa_(v, b, r); }
static WCHAR *CDECL _itow_(int v, WCHAR *b, int r) { char a[40]; _itoa_(v, a, r); int i = 0; for (; a[i]; i++) b[i] = a[i]; b[i] = 0; return b; }
static WCHAR *CDECL _i64tow_(int64_t v, WCHAR *b, int r) { char a[72]; _i64toa_(v, a, r); int i = 0; for (; a[i]; i++) b[i] = a[i]; b[i] = 0; return b; }
static WCHAR *CDECL _ui64tow_(uint64_t v, WCHAR *b, int r) { char a[72]; _ui64toa_(v, a, r); int i = 0; for (; a[i]; i++) b[i] = a[i]; b[i] = 0; return b; }
static int CDECL _itoa_s_(int v, char *b, size_t n, int r) { char t[40]; _itoa_(v, t, r); if (strlen(t) >= n) return E_RANGE; strcpy(b, t); return 0; }
static int CDECL _i64toa_s_(int64_t v, char *b, size_t n, int r) { char t[72]; _i64toa_(v, t, r); if (strlen(t) >= n) return E_RANGE; strcpy(b, t); return 0; }
static int CDECL strcpy_s_(char *d, size_t n, const char *s) { if (!d || !n) return E_INVAL; if (!s) { d[0] = 0; return E_INVAL; } size_t l = strlen(s); if (l >= n) { d[0] = 0; return E_RANGE; } memcpy(d, s, l + 1); return 0; }
static int CDECL strncpy_s_(char *d, size_t n, const char *s, size_t c) {
    if (!d || !n) return E_INVAL;
    size_t l = 0; while (l < c && s[l]) l++;
    if (c == (size_t)-1 && l >= n) { l = n - 1; memcpy(d, s, l); d[l] = 0; return 80; }   /* _TRUNCATE */
    if (l >= n) { d[0] = 0; return E_RANGE; }
    memcpy(d, s, l); d[l] = 0; return 0;
}
static int CDECL strcat_s_(char *d, size_t n, const char *s) { if (!d || !n) return E_INVAL; size_t a = strnlen_(d, n), b = strlen(s); if (a + b >= n) { d[0] = 0; return E_RANGE; } memcpy(d + a, s, b + 1); return 0; }
static int CDECL strncat_s_(char *d, size_t n, const char *s, size_t c) { size_t a = strnlen_(d, n), b = 0; while (b < c && s[b]) b++; if (a + b >= n) { if (c == (size_t)-1) b = n - a - 1; else { d[0] = 0; return E_RANGE; } } memcpy(d + a, s, b); d[a + b] = 0; return 0; }
static int CDECL memcpy_s_(void *d, size_t n, const void *s, size_t c) { if (c > n) { if (d) memset(d, 0, n); return E_RANGE; } memcpy(d, s, c); return 0; }
static int CDECL memmove_s_(void *d, size_t n, const void *s, size_t c) { if (c > n) return E_RANGE; memmove(d, s, c); return 0; }
static size_t wlen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static int CDECL wcscpy_s_(WCHAR *d, size_t n, const WCHAR *s) { size_t l = wlen(s); if (!d || l >= n) { if (d && n) d[0] = 0; return E_RANGE; } memcpy(d, s, (l + 1) * 2); return 0; }
static int CDECL wcsncpy_s_(WCHAR *d, size_t n, const WCHAR *s, size_t c) {
    size_t l = 0; while (l < c && s[l]) l++;
    if (c == (size_t)-1 && l >= n) { l = n - 1; memcpy(d, s, l * 2); d[l] = 0; return 80; }
    if (l >= n) { d[0] = 0; return E_RANGE; }
    memcpy(d, s, l * 2); d[l] = 0; return 0;
}
static int CDECL wcscat_s_(WCHAR *d, size_t n, const WCHAR *s) { size_t a = 0; while (a < n && d[a]) a++; size_t b = wlen(s); if (a + b >= n) { if (n) d[0] = 0; return E_RANGE; } memcpy(d + a, s, (b + 1) * 2); return 0; }
static int CDECL wcsncat_s_(WCHAR *d, size_t n, const WCHAR *s, size_t c) { size_t a = 0; while (a < n && d[a]) a++; size_t b = 0; while (b < c && s[b]) b++; if (a + b >= n) { if (c == (size_t)-1) b = n - a - 1; else { d[0] = 0; return E_RANGE; } } memcpy(d + a, s, b * 2); d[a + b] = 0; return 0; }
static WCHAR *CDECL wcsncat_(WCHAR *d, const WCHAR *s, size_t c) { size_t a = wlen(d), b = 0; while (b < c && s[b]) { d[a + b] = s[b]; b++; } d[a + b] = 0; return d; }
static size_t CDECL wcsnlen_(const WCHAR *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
static int CDECL wcscoll_(const WCHAR *a, const WCHAR *b) { return wcmp(a, b); }
static int CDECL strcoll_(const char *a, const char *b) { return strcmp(a, b); }
static int CDECL _stricoll_(const char *a, const char *b) { return strcasecmp(a, b); }
static size_t CDECL wcsxfrm_(WCHAR *d, const WCHAR *s, size_t n) { size_t l = wlen(s); if (l < n) memcpy(d, s, (l + 1) * 2); return l; }
static size_t CDECL strxfrm_(char *d, const char *s, size_t n) { size_t l = strlen(s); if (l < n) memcpy(d, s, l + 1); return l; }
static WCHAR *CDECL wcstok_s_(WCHAR *s, const WCHAR *d, WCHAR **ctx) {
    WCHAR *p = s ? s : *ctx;
    if (!p) return NULL;
    #define WIN(c) ({ bool _f = false; for (const WCHAR *q = d; *q; q++) if (*q == (c)) _f = true; _f; })
    while (*p && WIN(*p)) p++;
    if (!*p) { *ctx = p; return NULL; }
    WCHAR *t = p; while (*p && !WIN(*p)) p++;
    if (*p) *p++ = 0;
    *ctx = p;
    #undef WIN
    return t;
}
static WCHAR *CDECL wcstok_(WCHAR *s, const WCHAR *d, WCHAR **ctx) { static WCHAR *save; return wcstok_s_(s, d, ctx ? ctx : &save); }
static char *CDECL strtok_s_(char *s, const char *d, char **ctx) {
    char *p = s ? s : *ctx;
    if (!p) return NULL;
    p += strspn_(p, d);
    if (!*p) { *ctx = p; return NULL; }
    char *t = p; p += strcspn_(p, d);
    if (*p) *p++ = 0;
    *ctx = p; return t;
}
static WCHAR *CDECL _wcsdup_(const WCHAR *s) { size_t l = wlen(s); WCHAR *d = malloc_((l + 1) * 2); memcpy(d, s, (l + 1) * 2); return d; }
static int CDECL _wcsnicmp_(const WCHAR *a, const WCHAR *b, size_t n) { for (size_t i = 0; i < n; i++) { int x = a[i] < 128 ? tolower(a[i]) : a[i], y = b[i] < 128 ? tolower(b[i]) : b[i]; if (x != y) return x - y; if (!x) return 0; } return 0; }
static WCHAR *CDECL _wcslwr_(WCHAR *s) { for (WCHAR *p = s; *p; p++) if (*p < 128) *p = tolower(*p); return s; }
static WCHAR *CDECL _wcsupr_(WCHAR *s) { for (WCHAR *p = s; *p; p++) if (*p < 128) *p = toupper(*p); return s; }
static size_t CDECL wcsspn_(const WCHAR *s, const WCHAR *a) { size_t n = 0; for (; s[n]; n++) { const WCHAR *q = a; while (*q && *q != s[n]) q++; if (!*q) break; } return n; }
static size_t CDECL wcscspn_(const WCHAR *s, const WCHAR *r) { size_t n = 0; for (; s[n]; n++) { const WCHAR *q = r; while (*q && *q != s[n]) q++; if (*q) break; } return n; }
static WCHAR *CDECL wcspbrk_(const WCHAR *s, const WCHAR *a) { for (; *s; s++) for (const WCHAR *q = a; *q; q++) if (*q == *s) return (WCHAR *)s; return NULL; }
static WCHAR *CDECL wmemcpy_(WCHAR *d, const WCHAR *s, size_t n) { memcpy(d, s, n * 2); return d; }
static WCHAR *CDECL wmemmove_(WCHAR *d, const WCHAR *s, size_t n) { memmove(d, s, n * 2); return d; }
static WCHAR *CDECL wmemset_(WCHAR *d, WCHAR c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }
static int CDECL wmemcmp_(const WCHAR *a, const WCHAR *b, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static WCHAR *CDECL wmemchr_(const WCHAR *s, WCHAR c, size_t n) { for (size_t i = 0; i < n; i++) if (s[i] == c) return (WCHAR *)s + i; return NULL; }
static int CDECL iswupper_(WCHAR c) { return c >= 'A' && c <= 'Z'; }
static int CDECL iswlower_(WCHAR c) { return c >= 'a' && c <= 'z'; }
static int CDECL iswalnum_(WCHAR c) { return c < 128 ? isalnum(c) : c > 0xBF; }
static int CDECL iswprint_(WCHAR c) { return c >= 32 && c != 127; }
static int CDECL iswpunct_(WCHAR c) { return c < 128 ? ispunct_(c) : 0; }
static int CDECL iswxdigit_(WCHAR c) { return c < 128 && isxdigit(c); }
static int CDECL iswcntrl_(WCHAR c) { return c < 32 || c == 127; }
static int CDECL iswgraph_(WCHAR c) { return c > 32 && c != 127; }
static int CDECL iswctype_(WCHAR c, uint16_t m) { return c < 128 ? _isctype_(c, m) : (m & 0x103) ? 0x100 & m : 0; }
static int CDECL isleadbyte_(int c) { return 0; }
static int CDECL __isascii_(int c) { return (unsigned)c < 128; }
static int CDECL __iscsym_(int c) { return isalnum(c) || c == '_'; }
static int CDECL __iscsymf_(int c) { return isalpha(c) || c == '_'; }
static int CDECL __toascii_(int c) { return c & 0x7F; }
static void CDECL _swab_(char *a, char *b, int n) { for (int i = 0; i + 1 < n; i += 2) { char t = a[i]; b[i] = a[i + 1]; b[i + 1] = t; } }
static uint32_t CDECL _byteswap_ulong_(uint32_t v) { return __builtin_bswap32(v); }
static uint16_t CDECL _byteswap_ushort_(uint16_t v) { return __builtin_bswap16(v); }
static uint64_t CDECL _byteswap_uint64_(uint64_t v) { return __builtin_bswap64(v); }
static uint32_t CDECL _rotl_(uint32_t v, int s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }
static uint32_t CDECL _rotr_(uint32_t v, int s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static uint64_t CDECL _rotl64_(uint64_t v, int s) { s &= 63; return s ? (v << s) | (v >> (64 - s)) : v; }
static uint64_t CDECL _rotr64_(uint64_t v, int s) { s &= 63; return s ? (v >> s) | (v << (64 - s)) : v; }
static char *CDECL _strset_(char *s, int c) { for (char *p = s; *p; p++) *p = c; return s; }
static char *CDECL _strnset_(char *s, int c, size_t n) { for (size_t i = 0; i < n && s[i]; i++) s[i] = c; return s; }
static void *CDECL _memccpy_(void *d, const void *s, int c, size_t n) { uint8_t *o = d; const uint8_t *i = s; for (size_t k = 0; k < n; k++) { o[k] = i[k]; if (i[k] == (uint8_t)c) return o + k + 1; } return NULL; }
static int CDECL _memicmp_(const void *a, const void *b, size_t n) { const uint8_t *x = a, *y = b; for (size_t i = 0; i < n; i++) { int c = tolower(x[i]) - tolower(y[i]); if (c) return c; } return 0; }

/* multibyte: the "C" locale, one byte per character (Latin-1) */
static size_t CDECL mbstowcs_(WCHAR *d, const char *s, size_t n) { size_t l = strlen(s); if (!d) return l; size_t i = 0; for (; i < n && s[i]; i++) d[i] = (uint8_t)s[i]; if (i < n) d[i] = 0; return i; }
static size_t CDECL wcstombs_(char *d, const WCHAR *s, size_t n) { size_t l = wlen(s); if (!d) return l; size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i] < 256 ? (char)s[i] : '?'; if (i < n) d[i] = 0; return i; }
static int CDECL mbstowcs_s_(size_t *r, WCHAR *d, size_t n, const char *s, size_t c) { size_t l = MIN(strlen(s), c); if (d && l >= n) { if (c != (size_t)-1) { d[0] = 0; return E_RANGE; } l = n - 1; } if (d) { for (size_t i = 0; i < l; i++) d[i] = (uint8_t)s[i]; d[l] = 0; } if (r) *r = l + 1; return 0; }
static int CDECL wcstombs_s_(size_t *r, char *d, size_t n, const WCHAR *s, size_t c) { size_t l = MIN(wlen(s), c); if (d && l >= n) { if (c != (size_t)-1) { d[0] = 0; return E_RANGE; } l = n - 1; } if (d) { for (size_t i = 0; i < l; i++) d[i] = s[i] < 256 ? (char)s[i] : '?'; d[l] = 0; } if (r) *r = l + 1; return 0; }
static size_t CDECL mbrtowc_(WCHAR *w, const char *s, size_t n, void *st) { if (!s) return 0; if (!n) return (size_t)-2; if (w) *w = (uint8_t)*s; return *s ? 1 : 0; }
static size_t CDECL wcrtomb_(char *s, WCHAR w, void *st) { if (!s) return 1; *s = w < 256 ? (char)w : '?'; return 1; }
static size_t CDECL mbrlen_(const char *s, size_t n, void *st) { if (!s) return 0; if (!n) return (size_t)-2; return *s ? 1 : 0; }
static int CDECL mblen_(const char *s, size_t n) { return s && n && *s ? 1 : 0; }
static int CDECL mbtowc_(WCHAR *w, const char *s, size_t n) { if (!s || !n) return 0; if (w) *w = (uint8_t)*s; return *s ? 1 : 0; }
static int CDECL wctomb_(char *s, WCHAR w) { if (!s) return 0; *s = w < 256 ? (char)w : '?'; return 1; }
static int CDECL wctob_(WCHAR w) { return w < 256 ? w : -1; }
static WCHAR CDECL btowc_(int c) { return c < 0 ? 0xFFFF : (WCHAR)(uint8_t)c; }
static int CDECL mbsinit_(const void *st) { return 1; }
static size_t CDECL mbsrtowcs_(WCHAR *d, const char **s, size_t n, void *st) { size_t r = mbstowcs_(d, *s, n); if (d) *s = (*s)[r] ? *s + r : NULL; return r; }
static size_t CDECL wcsrtombs_(char *d, const WCHAR **s, size_t n, void *st) { size_t r = wcstombs_(d, *s, n); if (d) *s = (*s)[r] ? *s + r : NULL; return r; }
static int CDECL ___mb_cur_max_l_func_(void *l) { return 1; }
static int CDECL _getmbcp_(void) { return 0; }
static int CDECL _setmbcp_(int c) { return 0; }
static unsigned char *CDECL _mbsinc_(const unsigned char *s) { return (unsigned char *)s + 1; }
static size_t CDECL _mbslen_(const unsigned char *s) { return strlen((const char *)s); }
static unsigned char *CDECL _mbschr_(const unsigned char *s, unsigned c) { return (unsigned char *)strchr((const char *)s, c); }
static unsigned char *CDECL _mbsrchr_(const unsigned char *s, unsigned c) { return (unsigned char *)strrchr((const char *)s, c); }
static int CDECL _mbsicmp_(const unsigned char *a, const unsigned char *b) { return strcasecmp((const char *)a, (const char *)b); }
static WCHAR *CDECL _wsetlocale_(int c, const WCHAR *l) { static WCHAR C[] = { 'C', 0 }; return C; }
static void *CDECL _create_locale_(int c, const char *l) { static int dummy; return &dummy; }
static void CDECL _free_locale_(void *l) {}
static void *CDECL _get_current_locale_(void) { static int dummy; return &dummy; }
static int CDECL _configthreadlocale2_(int x) { return 2; }
static char *CDECL _strlwr_l_(char *s, void *l) { return _strlwr(s); }
static int CDECL _stricmp_l_(const char *a, const char *b, void *l) { return strcasecmp(a, b); }
static int CDECL _strnicmp_l_(const char *a, const char *b, size_t n, void *l) { return strncasecmp(a, b, n); }
static double CDECL _strtod_l_(const char *s, char **e, void *l) { return strtod_(s, e); }
static int CDECL _tolower_l_(int c, void *l) { return tolower(c); }
static int CDECL _toupper_l_(int c, void *l) { return toupper(c); }
static int CDECL _isctype_l_(int c, int m, void *l) { return _isctype_(c, m); }

/* ---------------- math ---------------- */
static double CDECL copysign_(double a, double b) { a = fabs_(a); return (b < 0 || (b == 0 && 1 / b < 0)) ? -a : a; }
static double CDECL frexp_(double x, int *e) {
    *e = 0; if (x == 0 || x != x) return x;
    double a = fabs_(x);
    while (a >= 1) { a /= 2; (*e)++; }
    while (a < 0.5) { a *= 2; (*e)--; }
    return x < 0 ? -a : a;
}
static double CDECL erf_(double x) {                         /* Abramowitz and Stegun 7.1.26 */
    double t = 1 / (1 + 0.3275911 * fabs_(x));
    double y = 1 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp_(-x * x);
    return x < 0 ? -y : y;
}
static double CDECL erfc_(double x) { return 1 - erf_(x); }
static int CDECL _finite_(double x) { return x == x && x - x == 0; }
static int CDECL _isnan_(double x) { return x != x; }
static int CDECL _fpclass_(double x) { if (x != x) return 2; if (x - x != 0) return x < 0 ? 4 : 0x200; if (x == 0) return 1 / x < 0 ? 0x20 : 0x40; return x < 0 ? 8 : 0x100; }
static double CDECL hypot_(double a, double b) { return sqrt_(a * a + b * b); }
static double CDECL trunc_(double x) { return x < 0 ? ceil_(x) : floor_(x); }
static wlong_t CDECL lround_(double x) { return (wlong_t)round_(x); }
static int64_t CDECL llround_(double x) { return (int64_t)round_(x); }
static wlong_t CDECL lrint_(double x) { return (wlong_t)round_(x); }
static double CDECL rint_(double x) { return round_(x); }
static double CDECL fmax_(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
static double CDECL fmin_(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static double CDECL cbrt_(double x) { if (x == 0) return 0; double r = exp2_(log2_(fabs_(x)) / 3); r = (2 * r + fabs_(x) / (r * r)) / 3; return x < 0 ? -r : r; }
static double CDECL log1p_(double x) { return log_(1 + x); }
static double CDECL expm1_(double x) { return exp_(x) - 1; }
static double CDECL atanh_(double x) { return 0.5 * log_((1 + x) / (1 - x)); }
static double CDECL asinh_(double x) { return log_(x + sqrt_(x * x + 1)); }
static double CDECL acosh_(double x) { return log_(x + sqrt_(x * x - 1)); }
static double CDECL exp2b_(double x) { return exp2_(x); }
static double CDECL log2b_(double x) { return log2_(x); }
static double CDECL scalbn_(double x, int e) { return ldexp_(x, e); }
static int CDECL ilogb_(double x) { int e; frexp_(x, &e); return e - 1; }
static double CDECL logb_(double x) { return ilogb_(x); }
static double CDECL nan_(const char *s) { return 0.0 / 0.0; }
static double CDECL fdim_(double a, double b) { return a > b ? a - b : 0; }
static double CDECL fma_(double a, double b, double c) { return a * b + c; }
static double CDECL remainder_(double a, double b) { return a - round_(a / b) * b; }
static double CDECL _chgsign_(double x) { return -x; }
static double CDECL _logb_(double x) { return logb_(x); }
static double CDECL _scalb_(double x, wlong_t e) { return ldexp_(x, e); }
static int64_t CDECL llabs_(int64_t x) { return x < 0 ? -x : x; }
typedef struct { int quot, rem; } div_t32;
typedef struct { int32_t quot, rem; } ldiv_t32;
static div_t32 CDECL div_(int a, int b) { div_t32 r = { a / b, a % b }; return r; }
static ldiv_t32 CDECL ldiv_(int32_t a, int32_t b) { ldiv_t32 r = { a / b, a % b }; return r; }
/* single-precision versions (x64 programs import these) */
static float CDECL sqrtf_(float x) { return (float)sqrt_(x); }
static float CDECL sinf_(float x) { return (float)sin_(x); }
static float CDECL cosf_(float x) { return (float)cos_(x); }
static float CDECL tanf_(float x) { return (float)tan_(x); }
static float CDECL atan2f_(float y, float x) { return (float)atan2_(y, x); }
static float CDECL atanf_(float x) { return (float)atan_(x); }
static float CDECL asinf_(float x) { return (float)asin_(x); }
static float CDECL acosf_(float x) { return (float)acos_(x); }
static float CDECL floorf_(float x) { return (float)floor_(x); }
static float CDECL ceilf_(float x) { return (float)ceil_(x); }
static float CDECL fmodf_(float a, float b) { return (float)fmod_(a, b); }
static float CDECL powf_(float a, float b) { return (float)pow_(a, b); }
static float CDECL expf_(float x) { return (float)exp_(x); }
static float CDECL logf_(float x) { return (float)log_(x); }
static float CDECL log10f_(float x) { return (float)log10_(x); }
static float CDECL roundf_(float x) { return (float)round_(x); }
static float CDECL truncf_(float x) { return (float)trunc_(x); }
static float CDECL fabsf_(float x) { return x < 0 ? -x : x; }
static float CDECL ldexpf_(float x, int e) { return (float)ldexp_(x, e); }

#ifndef __x86_64__
/* x86 compiler intrinsics: the argument(s) on the x87 stack, the result in st(0) */
double ci_sin(double x) { return sin_(x); } double ci_cos(double x) { return cos_(x); } double ci_tan(double x) { return tan_(x); }
double ci_sqrt(double x) { return sqrt_(x); } double ci_exp(double x) { return exp_(x); } double ci_log(double x) { return log_(x); }
double ci_log10(double x) { return log10_(x); } double ci_asin(double x) { return asin_(x); } double ci_acos(double x) { return acos_(x); }
double ci_atan(double x) { return atan_(x); } double ci_sinh(double x) { return sinh_(x); } double ci_cosh(double x) { return cosh_(x); }
double ci_tanh(double x) { return tanh_(x); }
double ci_pow(double a, double b) { return pow_(a, b); } double ci_atan2(double y, double x) { return atan2_(y, x); } double ci_fmod(double a, double b) { return fmod_(a, b); }
#define CI1(n) ".globl _CI" #n "_; _CI" #n "_: sub $12,%esp; fstpl (%esp); call ci_" #n "; add $12,%esp; ret\n"
#define CI2(n) ".globl _CI" #n "_; _CI" #n "_: sub $20,%esp; fstpl 8(%esp); fstpl (%esp); call ci_" #n "; add $20,%esp; ret\n"
__asm__(".text\n" CI1(sin) CI1(cos) CI1(tan) CI1(sqrt) CI1(exp) CI1(log) CI1(log10) CI1(asin) CI1(acos) CI1(atan) CI1(sinh) CI1(cosh) CI1(tanh)
        CI2(pow) CI2(atan2) CI2(fmod));
void _CIsin_(void), _CIcos_(void), _CItan_(void), _CIsqrt_(void), _CIexp_(void), _CIlog_(void), _CIlog10_(void), _CIasin_(void), _CIacos_(void),
     _CIatan_(void), _CIsinh_(void), _CIcosh_(void), _CItanh_(void), _CIpow_(void), _CIatan2_(void), _CIfmod_(void);
/* _allmul/_alldiv/... are statically linked by MSVC; _ftol2 and _ftol2_sse are imported by some */
static int64_t CDECL _ftol2_(void) { double v; __asm__ volatile("fstpl %0" : "=m"(v)); return (int64_t)v; }

/* setjmp/longjmp: our own layout in the program's jmp_buf (ebp ebx edi esi esp eip) */
__asm__(".text\n.globl w32_setjmp\nw32_setjmp:\n"
        " mov 4(%esp),%eax\n mov %ebp,0(%eax)\n mov %ebx,4(%eax)\n mov %edi,8(%eax)\n mov %esi,12(%eax)\n"
        " lea 4(%esp),%ecx\n mov %ecx,16(%eax)\n mov (%esp),%ecx\n mov %ecx,20(%eax)\n xor %eax,%eax\n ret\n"
        ".globl w32_longjmp\nw32_longjmp:\n"
        " mov 4(%esp),%edx\n mov 8(%esp),%eax\n test %eax,%eax\n jnz 1f\n inc %eax\n1:\n"
        " mov 0(%edx),%ebp\n mov 4(%edx),%ebx\n mov 8(%edx),%edi\n mov 12(%edx),%esi\n mov 16(%edx),%esp\n jmp *20(%edx)\n");
#else
/* x64: rbx rbp rdi rsi r12-r15 rsp rip, then xmm6-xmm15 (the Microsoft ABI's callee-saved registers) */
__asm__(".text\n.globl w32_setjmp\nw32_setjmp:\n"
        " mov %rbx,0(%rcx)\n mov %rbp,8(%rcx)\n mov %rdi,16(%rcx)\n mov %rsi,24(%rcx)\n mov %r12,32(%rcx)\n mov %r13,40(%rcx)\n mov %r14,48(%rcx)\n mov %r15,56(%rcx)\n"
        " lea 8(%rsp),%rdx\n mov %rdx,64(%rcx)\n mov (%rsp),%rdx\n mov %rdx,72(%rcx)\n"
        " movdqu %xmm6,80(%rcx)\n movdqu %xmm7,96(%rcx)\n movdqu %xmm8,112(%rcx)\n movdqu %xmm9,128(%rcx)\n movdqu %xmm10,144(%rcx)\n"
        " movdqu %xmm11,160(%rcx)\n movdqu %xmm12,176(%rcx)\n movdqu %xmm13,192(%rcx)\n movdqu %xmm14,208(%rcx)\n movdqu %xmm15,224(%rcx)\n"
        " xor %eax,%eax\n ret\n"
        ".globl w32_longjmp\nw32_longjmp:\n"
        " mov %edx,%eax\n test %eax,%eax\n jnz 1f\n inc %eax\n1:\n"
        " mov 0(%rcx),%rbx\n mov 8(%rcx),%rbp\n mov 16(%rcx),%rdi\n mov 24(%rcx),%rsi\n mov 32(%rcx),%r12\n mov 40(%rcx),%r13\n mov 48(%rcx),%r14\n mov 56(%rcx),%r15\n"
        " movdqu 80(%rcx),%xmm6\n movdqu 96(%rcx),%xmm7\n movdqu 112(%rcx),%xmm8\n movdqu 128(%rcx),%xmm9\n movdqu 144(%rcx),%xmm10\n"
        " movdqu 160(%rcx),%xmm11\n movdqu 176(%rcx),%xmm12\n movdqu 192(%rcx),%xmm13\n movdqu 208(%rcx),%xmm14\n movdqu 224(%rcx),%xmm15\n"
        " mov 64(%rcx),%rsp\n jmp *72(%rcx)\n");
#endif
void w32_setjmp(void), w32_longjmp(void);

/* ---------------- C++ runtime basics ---------------- */
static void *CDECL op_new_(size_t n) { return malloc_(n ? n : 1); }
static void CDECL op_delete_(void *p) { free_(p); }
static void CDECL op_delete_sized_(void *p, size_t n) { free_(p); }
static void *CDECL op_new_nothrow_(size_t n, const void *nt) { return malloc_(n ? n : 1); }
static void CDECL terminate2_(void) { abort_(); }
static void *CDECL set_terminate_(void *f) { return NULL; }
static void *CDECL set_unexpected_(void *f) { return NULL; }
static void *CDECL _set_se_translator_(void *f) { return NULL; }
static void *CDECL _set_purecall_handler_(void *f) { return NULL; }
static void *CDECL __std_type_info_name_(void *data, void *list) { return NULL; }
static void CDECL __std_type_info_destroy_list_(void *l) {}
static int CDECL __std_type_info_compare_(const void *a, const void *b) { return a == b ? 0 : 1; }
static void CDECL __std_exception_copy_(const void *src, void *dst) { ((void **)dst)[0] = ((void *const *)src)[0]; ((char *)dst)[sizeof(void *)] = 0; }
static void CDECL __std_exception_destroy_(void *e) {}
static void *CDECL __std_terminate_(void) { abort_(); return NULL; }
static void CDECL _local_unwind_(void *f, void *t) {}
static void CDECL _global_unwind2_(void *f) {}
static void CDECL _local_unwind2_(void *f, int t) {}
static void CDECL _local_unwind4_(void *c, void *f, int t) {}
static int CDECL _except_handler2_(void) { return 1; }
static void *CDECL __AdjustPointer_(void *p, const int *pmd) { return (char *)p + pmd[0]; }
static void *CDECL __CxxFrameHandler4_(void *a, void *b, void *c, void *d) { return (void *)1; }
static int CDECL __uncaught_exceptions_(void) { return 0; }
static void CDECL _CRT_RTC_INITW_(void) {}
static int CDECL _CrtDbgReport_(int t, const char *f, int l, const char *m, const char *fmt, ...) { return 0; }
static int CDECL _CrtSetReportMode_(int t, int m) { return 0; }
static int CDECL _CrtSetDbgFlag_(int f) { return 0; }
