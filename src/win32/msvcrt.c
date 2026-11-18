/* msvcrt.dll: C runtime for Win32 programs (stdio, string, memory, math, startup) */
#include "w32.h"

#define P w32_cur()

/* msvcrt FILE layout */
typedef struct { char *ptr; int cnt; char *base; int flag; int file; int charbuf; int bufsiz; char *tmpfname; } FILE32;
static FILE32 iob[20];
#define F_STDIN (&iob[0])
#define F_STDOUT (&iob[1])
#define F_STDERR (&iob[2])
void w32_crt_init(void) { for (int i = 0; i < 3; i++) iob[i].file = i; }

static int *CDECL _errno(void) { return &P->errno_val; }
static int *CDECL __p__fmode(void) { return (int *)&P->fmode; }
/* used by the x64 runtime */
static int CDECL ___mb_cur_max_func(void) { return 1; }
static UINT CDECL ___lc_codepage_func(void) { return 0; }        /* "C" locale */
static int CDECL __C_specific_handler(void *rec, void *frame, void *ctx, void *disp) { return 1; }   /* ExceptionContinueSearch */
static int *CDECL __p__commode(void) { return (int *)&P->commode; }
static int mb_cur_max = 1;
static char **initenv_dummy;
static char ***CDECL __p__environ(void) { initenv_dummy = P->envp; return &initenv_dummy; }
static char *acmdln_ptr;
static char **CDECL __p__acmdln(void) { acmdln_ptr = P->acmdln; return &acmdln_ptr; }
static int argc_v; static char **argv_v;
static int *CDECL __p___argc(void) { argc_v = P->argc; return &argc_v; }
static char ***CDECL __p___argv(void) { argv_v = P->argv; return &argv_v; }
static int CDECL __getmainargs(int *argc, char ***argv, char ***env, int wild, void *si) { w32_proc_t *p = P; *argc = p->argc; *argv = p->argv; *env = p->envp; initenv_dummy = p->envp; return 0; }
static void CDECL __set_app_type(int t) {}
static void CDECL __setusermatherr(void *f) {}
static void CDECL _initterm(void (**a)(void), void (**b)(void)) { for (; a < b; a++) if (*a) (*a)(); }
static int CDECL _initterm_e(int (**a)(void), int (**b)(void)) { for (; a < b; a++) if (*a) { int r = (*a)(); if (r) return r; } return 0; }
static void CDECL _cexit(void) { w32_proc_t *p = P; if (!p->exiting) { p->exiting = true; for (int i = p->natexit - 1; i >= 0; i--) p->atexit_fns[i](); } }
static void CDECL _c_exit(void) {}
static void CDECL exit_(int code) { w32_exit_process(code); }
static void CDECL _exit_(int code) { w32_proc_t *p = P; p->exiting = true; task_exit(code); }
static void CDECL abort_(void) { w32_proc_t *p = P; if (p->con) con_puts(p->con, "\nabnormal program termination\n"); else msgbox(path_basename(p->path), "abnormal program termination", MBI_ERROR, MBX_OK, NULL, NULL); p->exiting = true; task_exit(3); }
static void CDECL _amsg_exit(int n) { char b[64]; snprintf(b, sizeof b, "runtime error R60%02d", n); w32_proc_t *p = P; if (p->con) con_printf(p->con, "\n%s\n", b); p->exiting = true; task_exit(255); }
static void *CDECL _onexit(void *f) { w32_proc_t *p = P; if (p->natexit < 64) p->atexit_fns[p->natexit++] = f; return f; }
static int CDECL atexit_(void (*f)(void)) { return _onexit(f) ? 0 : -1; }
static void CDECL _lock(int n) {}
static void CDECL _unlock(int n) {}
static void *CDECL signal_(int s, void *h) { return NULL; }
static int CDECL raise_(int s) { if (s == 22) abort_(); return 0; }
static char *CDECL setlocale_(int c, const char *l) { static char C[] = "C"; return C; }
static struct { char *dp, *ts, *grp, *ics, *cs, *mdp, *mts, *mg, *ps, *ns; char f[8]; } lconv_v = { ".", "", "", "", "", "", "", "", "", "" };
static void *CDECL localeconv_(void) { return &lconv_v; }
static int CDECL _ismbblead(unsigned c) { return 0; }
static int CDECL __mb_cur_max_func(void) { return 1; }
static void CDECL _set_invalid_parameter_handler(void *h) {}
static int CDECL _setmode(int fd, int m) { return 0x4000; }
static int CDECL _fileno(FILE32 *f) { return f->file; }
static int CDECL _isatty(int fd) { return fd <= 2; }
static unsigned CDECL _controlfp(unsigned a, unsigned b) { return 0x9001F; }
static void CDECL _fpreset(void) { __asm__ volatile("fninit"); }
static int CDECL _configthreadlocale(int x) { return 0; }

/* ---------------- memory ---------------- */
static void *CDECL malloc_(size_t n) { return w32_alloc(P, n ? n : 1); }
static void *CDECL calloc_(size_t a, size_t b) { size_t t = a * b; void *p = w32_alloc(P, t ? t : 1); if (p) memset(p, 0, t); return p; }
static void *CDECL realloc_(void *p, size_t n) { return w32_realloc(P, p, n); }
static void CDECL free_(void *p) { w32_free(P, p); }
static size_t CDECL _msize(void *p) { return w32_msize(p); }
static char *CDECL _strdup(const char *s) { size_t l = strlen(s); char *r = malloc_(l + 1); memcpy(r, s, l + 1); return r; }

/* ---------------- string / ctype wrappers ---------------- */
static void *CDECL memcpy_(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *CDECL memmove_(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *CDECL memset_(void *d, int c, size_t n) { return memset(d, c, n); }
static int CDECL memcmp_(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void *CDECL memchr_(const void *s, int c, size_t n) { const uint8_t *p = s; for (size_t i = 0; i < n; i++) if (p[i] == (uint8_t)c) return (void *)(p + i); return NULL; }
static size_t CDECL strlen_(const char *s) { return strlen(s); }
static size_t CDECL strnlen_(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
static int CDECL strcmp_(const char *a, const char *b) { return strcmp(a, b); }
static int CDECL strncmp_(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static int CDECL _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static int CDECL _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static char *CDECL strcpy_(char *d, const char *s) { return strcpy(d, s); }
static char *CDECL strncpy_(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static char *CDECL strcat_(char *d, const char *s) { return strcat(d, s); }
static char *CDECL strncat_(char *d, const char *s, size_t n) { size_t l = strlen(d), i = 0; for (; i < n && s[i]; i++) d[l + i] = s[i]; d[l + i] = 0; return d; }
static char *CDECL strchr_(const char *s, int c) { return strchr(s, c); }
static char *CDECL strrchr_(const char *s, int c) { return strrchr(s, c); }
static char *CDECL strstr_(const char *h, const char *n) { return strstr(h, n); }
static size_t CDECL strspn_(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
static size_t CDECL strcspn_(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
static char *CDECL strpbrk_(const char *s, const char *a) { for (; *s; s++) if (strchr(a, *s)) return (char *)s; return NULL; }
static char *tok_save;
static char *CDECL strtok_(char *s, const char *d) {
    if (s) tok_save = s;
    if (!tok_save) return NULL;
    char *p = tok_save + strspn_(tok_save, d);
    if (!*p) { tok_save = NULL; return NULL; }
    char *e = p + strcspn_(p, d);
    if (*e) { *e = 0; tok_save = e + 1; } else tok_save = NULL;
    return p;
}
static char *CDECL _strupr(char *s) { for (char *p = s; *p; p++) *p = toupper(*p); return s; }
static char *CDECL _strlwr(char *s) { for (char *p = s; *p; p++) *p = tolower(*p); return s; }
static char *CDECL _strrev(char *s) { int l = strlen(s); for (int i = 0; i < l / 2; i++) { char t = s[i]; s[i] = s[l - 1 - i]; s[l - 1 - i] = t; } return s; }
static char *CDECL strerror_(int e) { static char b[32]; snprintf(b, sizeof b, "Error %d", e); return b; }
static size_t CDECL wcslen_(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static int CDECL toupper_(int c) { return toupper(c); }
static int CDECL tolower_(int c) { return tolower(c); }
static int CDECL isalpha_(int c) { return isalpha(c); }
static int CDECL isdigit_(int c) { return isdigit(c); }
static int CDECL isalnum_(int c) { return isalnum(c); }
static int CDECL isspace_(int c) { return isspace(c); }
static int CDECL isupper_(int c) { return c >= 'A' && c <= 'Z'; }
static int CDECL islower_(int c) { return c >= 'a' && c <= 'z'; }
static int CDECL isxdigit_(int c) { return isxdigit(c); }
static int CDECL ispunct_(int c) { return c > 32 && c < 127 && !isalnum(c); }
static int CDECL isprint_(int c) { return c >= 32 && c < 127; }
static int CDECL iscntrl_(int c) { return c < 32 || c == 127; }
static int CDECL isgraph_(int c) { return c > 32 && c < 127; }
static int CDECL atoi_(const char *s) { return atoi(s); }
/* Windows' long is 32 bits on x64 too (LLP64) */
typedef int32_t wlong_t;
typedef uint32_t wulong_t;
static wlong_t CDECL atol_(const char *s) { return strtol(s, NULL, 10); }
static wlong_t CDECL strtol_(const char *s, char **e, int b) { return strtol(s, e, b); }
static wulong_t CDECL strtoul_(const char *s, char **e, int b) { return strtoul(s, e, b); }
static int64_t CDECL _atoi64(const char *s) { int neg = 0; while (isspace(*s)) s++; if (*s == '-') { neg = 1; s++; } int64_t v = 0; while (isdigit(*s)) v = v * 10 + (*s++ - '0'); return neg ? -v : v; }
static double CDECL strtod_(const char *s, char **end) {
    const char *p = s; while (isspace(*p)) p++;
    int neg = 0; if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    double v = 0; bool any = false;
    while (isdigit(*p)) { v = v * 10 + (*p++ - '0'); any = true; }
    if (*p == '.') { p++; double f = 0.1; while (isdigit(*p)) { v += (*p++ - '0') * f; f /= 10; any = true; } }
    if (any && (*p == 'e' || *p == 'E')) { p++; int en = 0, es = 1; if (*p == '-') { es = -1; p++; } else if (*p == '+') p++; while (isdigit(*p)) en = en * 10 + (*p++ - '0'); while (en--) v = es > 0 ? v * 10 : v / 10; }
    if (end) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}
static double CDECL atof_(const char *s) { return strtod_(s, NULL); }
static int CDECL abs_(int x) { return x < 0 ? -x : x; }
static wlong_t CDECL labs_(wlong_t x) { return x < 0 ? -x : x; }
static uint32_t rnd_state = 1;
static int CDECL rand_(void) { rnd_state = rnd_state * 214013 + 2531011; return (rnd_state >> 16) & 0x7FFF; }
static void CDECL srand_(unsigned s) { rnd_state = s; }
static void CDECL qsort_(void *base, size_t n, size_t sz, int (CDECL *cmp)(const void *, const void *)) {
    uint8_t *b = base; uint8_t tmp[256]; if (sz > 256) return;
    for (size_t i = 1; i < n; i++) for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--) { memcpy(tmp, b + j * sz, sz); memcpy(b + j * sz, b + (j - 1) * sz, sz); memcpy(b + (j - 1) * sz, tmp, sz); }
}
static void *CDECL bsearch_(const void *key, const void *base, size_t n, size_t sz, int (CDECL *cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; int c = cmp(key, (const uint8_t *)base + m * sz); if (!c) return (void *)((const uint8_t *)base + m * sz); if (c < 0) hi = m; else lo = m + 1; }
    return NULL;
}
static char *CDECL getenv_(const char *n) {
    static char b[256];
    w32_proc_t *p = P;
    for (int i = 0; p->envp && p->envp[i]; i++) { int l = strlen(n); if (!strncasecmp(p->envp[i], n, l) && p->envp[i][l] == '=') return p->envp[i] + l + 1; }
    if (!strcasecmp(n, "TEMP") || !strcasecmp(n, "TMP")) { strcpy(b, "C:\\tmp"); return b; }
    return NULL;
}
static int CDECL system_(const char *cmd) { if (!cmd) return 1; return sh_run_string(P->con, cmd, P->cwd); }
static int32_t CDECL time_(int32_t *t) {
    rtc_time_t r; rtc_read(&r);
    int y = r.year, m = r.month; if (m <= 2) { y--; m += 12; }
    int64_t days = 365LL * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + r.day - 1 - 719468;
    int32_t v = (int32_t)(days * 86400 + r.hour * 3600 + r.minute * 60 + r.second);
    if (t) *t = v;
    return v;
}
static int32_t CDECL _time32(int32_t *t) { return time_(t); }
static int64_t CDECL _time64(int64_t *t) { int64_t v = time_(NULL); if (t) *t = v; return v; }
static int32_t CDECL clock_(void) { return timer_ticks(); }
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; } TM;
static TM tm_v;
static TM *CDECL localtime_(const int32_t *t) { rtc_time_t r; rtc_now(&r); tm_v.sec = r.second; tm_v.min = r.minute; tm_v.hour = r.hour; tm_v.mday = r.day; tm_v.mon = r.month - 1; tm_v.year = r.year - 1900; tm_v.wday = r.weekday; tm_v.yday = 0; tm_v.isdst = 0; return &tm_v; }
static TM *CDECL _localtime64(const int64_t *t) { return localtime_(NULL); }
static size_t CDECL strftime_(char *b, size_t n, const char *f, const TM *t) {
    size_t k = 0;
    for (; *f && k + 12 < n; f++) {
        if (*f != '%') { b[k++] = *f; continue; }
        f++;
        switch (*f) {
        case 'Y': k += snprintf(b + k, n - k, "%04d", t->year + 1900); break; case 'm': k += snprintf(b + k, n - k, "%02d", t->mon + 1); break;
        case 'd': k += snprintf(b + k, n - k, "%02d", t->mday); break; case 'H': k += snprintf(b + k, n - k, "%02d", t->hour); break;
        case 'M': k += snprintf(b + k, n - k, "%02d", t->min); break; case 'S': k += snprintf(b + k, n - k, "%02d", t->sec); break;
        default: b[k++] = *f;
        }
    }
    b[k] = 0; return k;
}

/* ---------------- printf family ---------------- */
static void fmt_double(char *out, int cap, double v, int prec, char conv, bool alt, bool plus, bool space) {
    char *o = out; int left = cap - 1;
#define PUT(c) do { if (left > 0) { *o++ = (c); left--; } } while (0)
    if (v != v) { const char *s = "nan"; while (*s) PUT(*s++); *o = 0; return; }
    if (v < 0 || (v == 0 && 1 / v < 0)) { PUT('-'); v = -v; } else if (plus) PUT('+'); else if (space) PUT(' ');
    if (v > 1e308) { const char *s = "inf"; while (*s) PUT(*s++); *o = 0; return; }
    if (prec < 0) prec = 6;
    int exp10 = 0;
    bool use_exp = conv == 'e' || conv == 'E';
    if (conv == 'g' || conv == 'G') {
        if (prec == 0) prec = 1;
        double t = v; int e = 0;
        if (t != 0) { while (t >= 10) { t /= 10; e++; } while (t < 1) { t *= 10; e--; } }
        if (e < -4 || e >= prec) { use_exp = true; prec -= 1; } else prec = prec - 1 - e;
        if (prec < 0) prec = 0;
    }
    if (use_exp && v != 0) { while (v >= 10) { v /= 10; exp10++; } while (v < 1) { v *= 10; exp10--; } }
    double r = 0.5; for (int i = 0; i < prec; i++) r /= 10;
    v += r;
    if (use_exp && v >= 10) { v /= 10; exp10++; }
    uint64_t ip = (uint64_t)v; double fp = v - (double)ip;
    char digs[24]; int nd = 0;
    if (!ip) digs[nd++] = '0';
    while (ip) { digs[nd++] = '0' + ip % 10; ip /= 10; }
    while (nd) PUT(digs[--nd]);
    char frac[64]; int nf = 0;
    for (int i = 0; i < prec && i < 60; i++) { fp *= 10; int d = (int)fp; frac[nf++] = '0' + d; fp -= d; }
    if ((conv == 'g' || conv == 'G') && !alt) while (nf && frac[nf - 1] == '0') nf--;
    if (nf || alt) PUT('.');
    for (int i = 0; i < nf; i++) PUT(frac[i]);
    if (use_exp) { PUT(conv == 'E' || conv == 'G' ? 'E' : 'e'); PUT(exp10 < 0 ? '-' : '+'); int e = exp10 < 0 ? -exp10 : exp10; PUT('0' + e / 100); PUT('0' + (e / 10) % 10); PUT('0' + e % 10); }
    *o = 0;
#undef PUT
}
int w32_vformat(char *out, int cap, const char *fmt, w32_va_list ap) {
    int len = 0;
#define OUT(c) do { if (len < cap - 1) out[len] = (c); len++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { OUT(*fmt); continue; }
        fmt++;
        bool left = false, plus = false, space = false, zero = false, alt = false;
        for (;; fmt++) { if (*fmt == '-') left = true; else if (*fmt == '+') plus = true; else if (*fmt == ' ') space = true; else if (*fmt == '0') zero = true; else if (*fmt == '#') alt = true; else break; }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } fmt++; } else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; if (*fmt == '*') { prec = va_arg(ap, int); fmt++; } else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        int lng = 0; bool sh = false;
        for (;;) {
            if (*fmt == 'l') { lng++; fmt++; } else if (*fmt == 'h') { sh = true; fmt++; } else if (*fmt == 'L' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') { if (*fmt == 'j' || (*fmt != 'L' && sizeof(void *) == 8)) lng = 2; fmt++; }
            else if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') { lng = 2; fmt += 3; } else if (fmt[0] == 'I' && fmt[1] == '3' && fmt[2] == '2') { fmt += 3; } else if (*fmt == 'I') { if (sizeof(void *) == 8) lng = 2; fmt++; } else break;
        }
        char tmp[400]; const char *s = tmp; int sl = 0; char sign = 0; const char *pfx = "";
        char c = *fmt;
        switch (c) {
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (sh) v = (int16_t)v;
            uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
            if (v < 0) sign = '-'; else if (plus) sign = '+'; else if (space) sign = ' ';
            int n = 0; char d[24]; if (!u && prec != 0) d[n++] = '0'; while (u) { d[n++] = '0' + u % 10; u /= 10; }
            while (n < prec) d[n++] = '0';
            for (int i = 0; i < n; i++) tmp[i] = d[n - 1 - i]; sl = n;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t u = lng >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            if (sh) u = (uint16_t)u;
            int base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            const char *dg = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (alt && u && base == 16) pfx = c == 'X' ? "0X" : "0x";
            if (alt && base == 8) pfx = "0";
            int n = 0; char d[24]; if (!u && prec != 0) d[n++] = '0'; while (u) { d[n++] = dg[u % base]; u /= base; }
            while (n < prec) d[n++] = '0';
            for (int i = 0; i < n; i++) tmp[i] = d[n - 1 - i]; sl = n;
            break;
        }
        case 'p': { uintptr_t v = (uintptr_t)va_arg(ap, void *); sl = snprintf(tmp, sizeof tmp, "%0*llX", (int)sizeof(void *) * 2, (unsigned long long)v); break; }
        case 'c': tmp[0] = (char)va_arg(ap, int); sl = 1; break;
        case 'C': tmp[0] = (char)va_arg(ap, int); sl = 1; break;
        case 's': case 'S':
            if (lng || c == 'S') { const WCHAR *w = va_arg(ap, const WCHAR *); if (!w) { s = "(null)"; sl = 6; } else { int n = 0; while (w[n] && n < 399 && (prec < 0 || n < prec)) { tmp[n] = (char)w[n]; n++; } sl = n; } }
            else { s = va_arg(ap, const char *); if (!s) s = "(null)"; sl = strlen(s); if (prec >= 0 && sl > prec) sl = prec; }
            prec = -1;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': {
            double v = va_arg(ap, double);
            fmt_double(tmp, sizeof tmp, v, prec, c == 'F' ? 'f' : c == 'a' ? 'e' : c, alt, plus, space);
            if (tmp[0] == '-' || tmp[0] == '+' || tmp[0] == ' ') { sign = tmp[0]; memmove(tmp, tmp + 1, strlen(tmp)); }
            sl = strlen(tmp);
            break;
        }
        case 'n': { int *n = va_arg(ap, int *); if (n) *n = len; continue; }
        case '%': OUT('%'); continue;
        default: OUT('%'); if (c) OUT(c); else fmt--; continue;
        }
        int total = sl + (sign ? 1 : 0) + strlen(pfx);
        if (!left && !(zero && c != 's' && c != 'c')) for (int i = total; i < width; i++) OUT(' ');
        if (sign) OUT(sign);
        for (const char *q = pfx; *q; q++) OUT(*q);
        if (!left && zero && c != 's' && c != 'c') for (int i = total; i < width; i++) OUT('0');
        for (int i = 0; i < sl; i++) OUT(s[i]);
        if (left) for (int i = total; i < width; i++) OUT(' ');
    }
    if (cap > 0) out[len < cap ? len : cap - 1] = 0;
    return len;
#undef OUT
}

/* ---------------- FILE streams ---------------- */
typedef struct { FILE32 f; HANDLE h; bool eof, err; int unget; } xfile_t;
#define IS_STD(f) ((f) >= &iob[0] && (f) <= &iob[2])
extern const w32_export_t kernel32_exports[];
static void *k32(const char *name) { void *fn = NULL; w32_lookup("kernel32.dll", name, &fn); return fn; }
static int stream_write(FILE32 *f, const char *s, int n) {
    if (f == F_STDOUT || f == F_STDERR) return w32_console_write(P, f == F_STDERR ? 2 : 1, s, n);
    if (IS_STD(f)) return -1;
    xfile_t *x = (xfile_t *)f;
    BOOL (WINAPI *wf)(HANDLE, const void *, DWORD, DWORD *, void *) = k32("WriteFile");
    DWORD w = 0;
    if (!wf(x->h, s, n, &w, NULL)) { x->err = true; return -1; }
    return w;
}
static int stream_getc(FILE32 *f) {
    if (f == F_STDIN) return w32_stdin_getc(P);
    if (IS_STD(f)) return -1;
    xfile_t *x = (xfile_t *)f;
    if (x->unget >= 0) { int c = x->unget; x->unget = -1; return c; }
    BOOL (WINAPI *rf)(HANDLE, void *, DWORD, DWORD *, void *) = k32("ReadFile");
    uint8_t c; DWORD r = 0;
    if (!rf(x->h, &c, 1, &r, NULL) || !r) { x->eof = true; return -1; }
    return c;
}
static int vfprintf2_(FILE32 *f, const char *fmt, w32_va_list ap);
static int CDECL vfprintf_(FILE32 *f, const char *fmt, w32_va_list ap) { return vfprintf2_(f, fmt, ap); }
static int CDECL fprintf_(FILE32 *f, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = vfprintf_(f, fmt, ap); w32_va_end(ap); return r; }
static int CDECL vprintf_(const char *fmt, w32_va_list ap) { return vfprintf_(F_STDOUT, fmt, ap); }
static int CDECL printf_(const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = vfprintf_(F_STDOUT, fmt, ap); w32_va_end(ap); return r; }
static int CDECL vsprintf_(char *b, const char *fmt, w32_va_list ap) { return w32_vformat(b, 0x7FFFFFFF, fmt, ap); }
static int CDECL sprintf_(char *b, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = w32_vformat(b, 0x7FFFFFFF, fmt, ap); w32_va_end(ap); return r; }
static int CDECL _vsnprintf(char *b, size_t n, const char *fmt, w32_va_list ap) { int r = w32_vformat(b, n, fmt, ap); return r >= (int)n ? -1 : r; }
static int CDECL _snprintf(char *b, size_t n, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = _vsnprintf(b, n, fmt, ap); w32_va_end(ap); return r; }
static int CDECL vsnprintf_(char *b, size_t n, const char *fmt, w32_va_list ap) { return w32_vformat(b, n, fmt, ap); }
static int CDECL snprintf_(char *b, size_t n, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = w32_vformat(b, n, fmt, ap); w32_va_end(ap); return r; }
static int CDECL puts_(const char *s) { stream_write(F_STDOUT, s, strlen(s)); stream_write(F_STDOUT, "\n", 1); return 0; }
static int CDECL fputs_(const char *s, FILE32 *f) { return stream_write(f, s, strlen(s)) < 0 ? -1 : 0; }
static int CDECL fputc_(int c, FILE32 *f) { char ch = c; return stream_write(f, &ch, 1) < 0 ? -1 : (uint8_t)c; }
static int CDECL putchar_(int c) { return fputc_(c, F_STDOUT); }
static int CDECL _flsbuf(int c, FILE32 *f) { return fputc_(c, f); }
static size_t CDECL fwrite_(const void *p, size_t sz, size_t n, FILE32 *f) { if (!sz || !n) return 0; int w = stream_write(f, p, sz * n); return w < 0 ? 0 : w / sz; }
static int CDECL fflush_(FILE32 *f) { return 0; }
static int CDECL fgetc_(FILE32 *f) { return stream_getc(f); }
static int CDECL getc_(FILE32 *f) { return stream_getc(f); }
static int CDECL getchar_(void) { return stream_getc(F_STDIN); }
static int CDECL _getch(void) { w32_proc_t *p = P; return p->con ? p->con->getkey(p->con) : -1; }
static int CDECL _kbhit(void) { return 0; }
static int CDECL ungetc_(int c, FILE32 *f) { if (f == F_STDIN) { w32_proc_t *p = P; if (p->stdin_pos > 0) p->stdin_buf[--p->stdin_pos] = c; return c; } if (!IS_STD(f)) ((xfile_t *)f)->unget = c; return c; }
static char *CDECL fgets_(char *b, int n, FILE32 *f) {
    int k = 0;
    while (k < n - 1) { int c = stream_getc(f); if (c < 0) break; b[k++] = c; if (c == '\n') break; }
    b[k] = 0;
    return k ? b : NULL;
}
static char *CDECL gets_(char *b) { char *r = fgets_(b, 1024, F_STDIN); if (r) { int l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0; } return r; }
static size_t CDECL fread_(void *p, size_t sz, size_t n, FILE32 *f) {
    if (!sz || !n) return 0;
    uint8_t *d = p; size_t total = sz * n, k = 0;
    if (!IS_STD(f)) {
        xfile_t *x = (xfile_t *)f;
        BOOL (WINAPI *rf)(HANDLE, void *, DWORD, DWORD *, void *) = k32("ReadFile");
        DWORD r = 0; rf(x->h, d, total, &r, NULL); if (r < total) x->eof = true; return r / sz;
    }
    while (k < total) { int c = stream_getc(f); if (c < 0) break; d[k++] = c; }
    return k / sz;
}
static int fd_alloc(w32_proc_t *p, uintptr_t h);
static FILE32 *CDECL fopen_(const char *name, const char *mode) {
    HANDLE (WINAPI *cf)(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) = k32("CreateFileA");
    DWORD acc = 0x80000000u, disp = 3;
    if (mode[0] == 'w') { acc = 0x40000000u; disp = 2; }
    if (mode[0] == 'a') { acc = 0x40000000u; disp = 4; }
    if (strchr(mode, '+')) acc |= 0xC0000000u;
    HANDLE h = cf(name, acc, 0, NULL, disp, 0, 0);
    if ((uint32_t)h == 0xFFFFFFFF) { P->errno_val = 2; return NULL; }
    xfile_t *x = w32_alloc(P, sizeof(xfile_t)); memset(x, 0, sizeof *x);
    x->h = h; x->unget = -1; x->f.file = fd_alloc(P, h);
    if (mode[0] == 'a') { DWORD (WINAPI *sfp)(HANDLE, LONG, LONG *, DWORD) = k32("SetFilePointer"); sfp(h, 0, NULL, 2); }
    return &x->f;
}
static int CDECL fclose_(FILE32 *f) {
    if (IS_STD(f)) return 0;
    xfile_t *x = (xfile_t *)f; w32_proc_t *p = P;
    int fd = x->f.file; bool shared = false;
    if (fd >= 3 && fd < 64 && p->fdh[fd] == x->h) { p->fdh[fd] = 0; for (int i = 0; i < 64; i++) if (p->fdh[i] == x->h) shared = true; }
    if (!shared) { BOOL (WINAPI *ch)(HANDLE) = k32("CloseHandle"); ch(x->h); }
    w32_free(P, x); return 0;
}
static int CDECL fseek_(FILE32 *f, wlong_t off, int wh) { if (IS_STD(f)) return -1; xfile_t *x = (xfile_t *)f; DWORD (WINAPI *sfp)(HANDLE, LONG, LONG *, DWORD) = k32("SetFilePointer"); x->eof = false; x->unget = -1; return sfp(x->h, off, NULL, wh) == 0xFFFFFFFF ? -1 : 0; }
static wlong_t CDECL ftell_(FILE32 *f) { if (IS_STD(f)) return -1; xfile_t *x = (xfile_t *)f; DWORD (WINAPI *sfp)(HANDLE, LONG, LONG *, DWORD) = k32("SetFilePointer"); return sfp(x->h, 0, NULL, 1); }
static void CDECL rewind_(FILE32 *f) { fseek_(f, 0, 0); }
static int CDECL feof_(FILE32 *f) { return IS_STD(f) ? 0 : ((xfile_t *)f)->eof; }
static int CDECL ferror_(FILE32 *f) { return IS_STD(f) ? 0 : ((xfile_t *)f)->err; }
static void CDECL clearerr_(FILE32 *f) { if (!IS_STD(f)) { ((xfile_t *)f)->eof = ((xfile_t *)f)->err = false; } }
static int CDECL setvbuf_(FILE32 *f, char *b, int m, size_t s) { return 0; }
static void CDECL setbuf_(FILE32 *f, char *b) {}
static FILE32 *CDECL __iob_func(void) { return iob; }
static FILE32 *CDECL __acrt_iob_func(unsigned i) { return &iob[i < 3 ? i : 0]; }
static int CDECL remove_(const char *n) { BOOL (WINAPI *df)(const char *) = k32("DeleteFileA"); return df(n) ? 0 : -1; }
static int CDECL rename_(const char *a, const char *b) { BOOL (WINAPI *mf)(const char *, const char *) = k32("MoveFileA"); return mf(a, b) ? 0 : -1; }
static int CDECL _mkdir(const char *n) { BOOL (WINAPI *cd)(const char *, void *) = k32("CreateDirectoryA"); return cd(n, NULL) ? 0 : -1; }
static int CDECL _chdir(const char *n) { BOOL (WINAPI *sd)(const char *) = k32("SetCurrentDirectoryA"); return sd(n) ? 0 : -1; }
static char *CDECL _getcwd(char *b, int n) { if (!b) b = malloc_(n = 260); strlcpy(b, P->cwd, n); return b; }
static int CDECL _access(const char *n, int m) { DWORD (WINAPI *ga)(const char *) = k32("GetFileAttributesA"); return ga(n) == 0xFFFFFFFF ? -1 : 0; }
static int CDECL _write(int fd, const void *b, unsigned n) { if (fd == 1 || fd == 2) return w32_console_write(P, fd, b, n); return -1; }
static int CDECL _read(int fd, void *b, unsigned n) { if (fd == 0) return w32_stdin_read(P, b, n); return -1; }

/* ---------------- scanf family ---------------- */
typedef struct { const char *s; FILE32 *f; int peek; } scan_src_t;
static int sc_get(scan_src_t *src) { if (src->peek >= 0) { int c = src->peek; src->peek = -1; return c; } if (src->s) return *src->s ? (uint8_t)*src->s++ : -1; return stream_getc(src->f); }
static void sc_unget(scan_src_t *src, int c) { if (c >= 0) src->peek = c; }
static int vscan(scan_src_t *src, const char *fmt, w32_va_list ap) {
    int assigned = 0, c = 0;
    for (; *fmt; fmt++) {
        if (isspace(*fmt)) { while ((c = sc_get(src)) >= 0 && isspace(c)) {} sc_unget(src, c); continue; }
        if (*fmt != '%') { c = sc_get(src); if (c != *fmt) { sc_unget(src, c); break; } continue; }
        fmt++;
        bool skip = false; if (*fmt == '*') { skip = true; fmt++; }
        int width = 0; while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        int lng = 0; bool hh = false; while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L') { if (*fmt == 'l') lng++; if (*fmt == 'h') hh = true; fmt++; }
        char conv = *fmt;
        if (conv == '%') { c = sc_get(src); if (c != '%') break; continue; }
        if (conv != 'c' && conv != '[') { while ((c = sc_get(src)) >= 0 && isspace(c)) {} sc_unget(src, c); }
        char buf[512]; int n = 0; if (!width) width = conv == 'c' ? 1 : 511;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            while (n < width && (c = sc_get(src)) >= 0 && (isxdigit(c) || ((c == '-' || c == '+') && n == 0) || ((c == 'x' || c == 'X') && n == 1 && buf[0] == '0'))) {
                if (conv != 'x' && conv != 'X' && conv != 'i' && isalpha(c)) { sc_unget(src, c); c = -2; break; }
                buf[n++] = c;
            }
            if (c != -2 && n < width) sc_unget(src, c);
            buf[n] = 0;
            if (!n || (n == 1 && (buf[0] == '-' || buf[0] == '+'))) break;
            long v = strtol(buf, NULL, conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10);
            if (!skip) { if (lng >= 2) *va_arg(ap, int64_t *) = v; else if (hh) *va_arg(ap, short *) = v; else *va_arg(ap, int *) = v; assigned++; }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
            while (n < width && (c = sc_get(src)) >= 0 && (isdigit(c) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) buf[n++] = c;
            if (n < width) sc_unget(src, c);
            buf[n] = 0; if (!n) break;
            double v = strtod_(buf, NULL);
            if (!skip) { if (lng) *va_arg(ap, double *) = v; else *va_arg(ap, float *) = (float)v; assigned++; }
        } else if (conv == 's') {
            char *d = skip ? NULL : va_arg(ap, char *);
            while (n < width && (c = sc_get(src)) >= 0 && !isspace(c)) { if (d) d[n] = c; n++; }
            sc_unget(src, c);
            if (!n) break;
            if (d) { d[n] = 0; assigned++; }
        } else if (conv == 'c') {
            char *d = skip ? NULL : va_arg(ap, char *);
            for (; n < width; n++) { c = sc_get(src); if (c < 0) break; if (d) d[n] = c; }
            if (!n) break;
            if (d) assigned++;
        } else if (conv == '[') {
            fmt++; bool neg = false; if (*fmt == '^') { neg = true; fmt++; }
            char set[128]; int ns = 0; if (*fmt == ']') set[ns++] = *fmt++;
            while (*fmt && *fmt != ']' && ns < 127) set[ns++] = *fmt++;
            set[ns] = 0;
            char *d = skip ? NULL : va_arg(ap, char *);
            while (n < width && (c = sc_get(src)) >= 0 && ((strchr(set, c) != NULL) != neg)) { if (d) d[n] = c; n++; }
            sc_unget(src, c);
            if (!n) break;
            if (d) { d[n] = 0; assigned++; }
        } else if (conv == 'n') { if (!skip) *va_arg(ap, int *) = 0; }
        else break;
    }
    return assigned;
}
int w32_vsscanf(const char *str, const char *fmt, w32_va_list ap) { scan_src_t s = { str, NULL, -1 }; return vscan(&s, fmt, ap); }
static int CDECL sscanf_(const char *str, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = w32_vsscanf(str, fmt, ap); w32_va_end(ap); return r; }
static int CDECL scanf_(const char *fmt, ...) {
    w32_va_list ap; w32_va_start(ap, fmt);
    scan_src_t s = { NULL, F_STDIN, -1 };
    int r = vscan(&s, fmt, ap);
    w32_va_end(ap);
    if (s.peek >= 0) ungetc_(s.peek, F_STDIN);
    return r;
}
static int CDECL fscanf_(FILE32 *f, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); scan_src_t s = { NULL, f, -1 }; int r = vscan(&s, fmt, ap); w32_va_end(ap); if (s.peek >= 0) ungetc_(s.peek, f); return r; }

/* ---------------- math (x87) ---------------- */
static double CDECL sqrt_(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static double CDECL sin_(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static double CDECL cos_(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static double CDECL tan_(double x) { double r, one; __asm__("fptan" : "=t"(one), "=u"(r) : "0"(x)); return r; }
static double CDECL atan2_(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static double CDECL atan_(double x) { return atan2_(x, 1.0); }
static double CDECL fabs_(double x) { return x < 0 ? -x : x; }
static double CDECL floor_(double x) { double r = (double)(int64_t)x; if (r > x) r -= 1; return r; }
static double CDECL ceil_(double x) { double r = (double)(int64_t)x; if (r < x) r += 1; return r; }
static double CDECL fmod_(double a, double b) { if (b == 0) return 0; double q = a / b; q = q < 0 ? ceil_(q) : floor_(q); return a - q * b; }
static double CDECL log2_(double x) { double r; __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x)); return r; }
static double CDECL log_(double x) { return log2_(x) * 0.69314718055994530942; }
static double CDECL log10_(double x) { return log2_(x) * 0.30102999566398119521; }
static double CDECL exp2_(double x) {
    double ip = floor_(x), fp = x - ip, r;
    __asm__("f2xm1" : "=t"(r) : "0"(fp)); r += 1.0;
    int e = (int)ip; while (e > 0) { r *= 2; e--; } while (e < 0) { r /= 2; e++; }
    return r;
}
static double CDECL exp_(double x) { return exp2_(x * 1.44269504088896340736); }
static double CDECL pow_(double a, double b) {
    if (b == 0) return 1; if (a == 0) return 0;
    if (b == (int)b && b > -64 && b < 64) { double r = 1; int n = b < 0 ? -(int)b : (int)b; while (n--) r *= a; return b < 0 ? 1 / r : r; }
    if (a < 0) return 0.0 / 0.0;
    return exp2_(b * log2_(a));
}
static double CDECL asin_(double x) { return atan2_(x, sqrt_(1 - x * x)); }
static double CDECL acos_(double x) { return atan2_(sqrt_(1 - x * x), x); }
static double CDECL sinh_(double x) { return (exp_(x) - exp_(-x)) / 2; }
static double CDECL cosh_(double x) { return (exp_(x) + exp_(-x)) / 2; }
static double CDECL tanh_(double x) { double a = exp_(x), b = exp_(-x); return (a - b) / (a + b); }
static double CDECL ldexp_(double x, int e) { while (e > 0) { x *= 2; e--; } while (e < 0) { x /= 2; e++; } return x; }
static double CDECL modf_(double x, double *ip) { double i = x < 0 ? ceil_(x) : floor_(x); *ip = i; return x - i; }
static int64_t CDECL _ftol(void) { double v; __asm__ volatile("fstpl %0" : "=m"(v)); return (int64_t)v; }
static double CDECL _hypot(double a, double b) { return sqrt_(a * a + b * b); }
static double CDECL round_(double x) { return x < 0 ? -floor_(-x + 0.5) : floor_(x + 0.5); }

static void CDECL _assert(const char *m, const char *f, unsigned l) {
    char b[300]; snprintf(b, sizeof b, "Assertion failed: %s, file %s, line %u", m, f, l);
    w32_proc_t *p = P; if (p->con) con_printf(p->con, "%s\n", b); else msgbox(path_basename(p->path), b, MBI_ERROR, MBX_OK, NULL, NULL);
    abort_();
}
static int CDECL _setjmp3(void *buf, int n) { return 0; }
static void CDECL __dllonexit(void *f, void *a, void *b) { _onexit(f); }
static int CDECL _XcptFilter(unsigned c, void *p) { return 0; }
static int CDECL _except_handler3(void) { return 1; }
static int CDECL _except_handler4_common(void) { return 1; }
static void *CDECL _beginthreadex(void *sec, unsigned st, void *fn, void *arg, unsigned fl, unsigned *tid) { HANDLE (WINAPI *ct)(void *, DWORD, void *, void *, DWORD, DWORD *) = k32("CreateThread"); return (void *)ct(NULL, st, fn, arg, fl, (DWORD *)tid); }
static void CDECL _endthreadex(unsigned c) { task_exit(c); }

#include "crt2.inc"

#define E(n) { #n, (void *)n }
#define EA(n, f) { n, (void *)f }
const w32_export_t msvcrt_exports[] = {
#include "crt2_tab.inc"
    E(_errno), E(__p__fmode), E(__p__commode), E(__p__environ), E(__p__acmdln), E(__p___argc), E(__p___argv), E(__getmainargs), E(__set_app_type), E(__setusermatherr),
    E(_initterm), E(_initterm_e), E(_cexit), E(_c_exit), EA("exit", exit_), EA("_exit", _exit_), EA("abort", abort_), E(_amsg_exit), E(_onexit), EA("atexit", atexit_),
    E(_lock), E(_unlock), EA("signal", signal_), EA("raise", raise_), EA("setlocale", setlocale_), EA("localeconv", localeconv_), E(_ismbblead), E(__mb_cur_max_func),
    EA("__mb_cur_max", &mb_cur_max), EA("__initenv", &initenv_dummy), EA("_iob", iob), E(__iob_func), E(__acrt_iob_func),
    E(_set_invalid_parameter_handler), EA("_setmode", _setmode2_), E(_fileno), EA("_isatty", _isatty2_), E(_controlfp), E(_fpreset), E(_configthreadlocale),
    EA("malloc", malloc_), EA("calloc", calloc_), EA("realloc", realloc_), EA("free", free_), E(_msize), E(_strdup), EA("strdup", _strdup),
    EA("memcpy", memcpy_), EA("memmove", memmove_), EA("memset", memset_), EA("memcmp", memcmp_), EA("memchr", memchr_),
    EA("strlen", strlen_), EA("strnlen", strnlen_), EA("strcmp", strcmp_), EA("strncmp", strncmp_), E(_stricmp), E(_strnicmp), EA("_strcmpi", _stricmp), EA("stricmp", _stricmp),
    EA("strcpy", strcpy_), EA("strncpy", strncpy_), EA("strcat", strcat_), EA("strncat", strncat_), EA("strchr", strchr_), EA("strrchr", strrchr_), EA("strstr", strstr_),
    EA("strspn", strspn_), EA("strcspn", strcspn_), EA("strpbrk", strpbrk_), EA("strtok", strtok_), E(_strupr), E(_strlwr), E(_strrev), EA("wcslen", wcslen_),
    EA("toupper", toupper_), EA("tolower", tolower_), EA("isalpha", isalpha_), EA("isdigit", isdigit_), EA("isalnum", isalnum_), EA("isspace", isspace_), EA("isupper", isupper_),
    EA("islower", islower_), EA("isxdigit", isxdigit_), EA("ispunct", ispunct_), EA("isprint", isprint_), EA("iscntrl", iscntrl_), EA("isgraph", isgraph_),
    EA("atoi", atoi_), EA("atol", atol_), EA("strtol", strtol_), EA("strtoul", strtoul_), E(_atoi64), EA("strtod", strtod_), EA("atof", atof_), EA("abs", abs_), EA("labs", labs_),
    EA("rand", rand_), EA("srand", srand_), EA("qsort", qsort_), EA("bsearch", bsearch_), EA("getenv", getenv_), EA("system", system_),
    E(___mb_cur_max_func), E(___lc_codepage_func), E(__C_specific_handler),
    EA("clock", clock_), EA("strftime", strftime_),
    EA("vfprintf", vfprintf_), EA("fprintf", fprintf_), EA("vprintf", vprintf_), EA("printf", printf_), EA("vsprintf", vsprintf_), EA("sprintf", sprintf_),
    E(_vsnprintf), E(_snprintf), EA("vsnprintf", vsnprintf_), EA("snprintf", snprintf_), EA("puts", puts_), EA("fputs", fputs_), EA("fputc", fputc_), EA("putc", fputc_),
    EA("putchar", putchar_), E(_flsbuf), EA("fwrite", fwrite_), EA("fflush", fflush_), EA("fgetc", fgetc_), EA("getc", getc_), EA("getchar", getchar_), E(_getch), E(_kbhit),
    EA("ungetc", ungetc_), EA("fgets", fgets_), EA("gets", gets_), EA("fread", fread_), EA("fopen", fopen_), EA("fclose", fclose_), EA("fseek", fseek_), EA("ftell", ftell_),
    EA("rewind", rewind_), EA("feof", feof_), EA("ferror", ferror_), EA("clearerr", clearerr_), EA("setvbuf", setvbuf_), EA("setbuf", setbuf_),
    EA("remove", remove_), EA("rename", rename_), E(_mkdir), E(_chdir), E(_getcwd), E(_access), EA("_write", _write2_), EA("_read", _read2_),
    EA("sscanf", sscanf_), EA("scanf", scanf_), EA("fscanf", fscanf_),
    EA("sqrt", sqrt_), EA("sin", sin_), EA("cos", cos_), EA("tan", tan_), EA("atan2", atan2_), EA("atan", atan_), EA("fabs", fabs_), EA("floor", floor_), EA("ceil", ceil_),
    EA("fmod", fmod_), EA("log", log_), EA("log10", log10_), EA("log2", log2_), EA("exp", exp_), EA("pow", pow_), EA("asin", asin_), EA("acos", acos_), EA("sinh", sinh_),
    EA("cosh", cosh_), EA("tanh", tanh_), EA("ldexp", ldexp_), EA("modf", modf_), E(_ftol), E(_hypot), EA("round", round_),
    E(_assert), E(__dllonexit), E(_XcptFilter), E(_except_handler3), E(_except_handler4_common), E(_beginthreadex), E(_endthreadex),
};
const int msvcrt_count = ARRAY_LEN(msvcrt_exports);
