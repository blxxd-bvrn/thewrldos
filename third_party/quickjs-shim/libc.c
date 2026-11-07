/* C library pieces QuickJS needs that the kernel's lib.c does not provide. */
#include "qjs_libc.h"

/* kernel interfaces (declared here so the kernel headers stay out of the way) */
void *kmalloc(size_t n);
void kfree(void *p);
void *krealloc(void *p, size_t n);
size_t kmalloc_usable(void *p);
uint32_t timer_ticks(void);
void klog(const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((noreturn));
typedef struct { int year, month, day, hour, minute, second, weekday; } k_rtc_time_t;
void rtc_read(k_rtc_time_t *t);
extern int tz_offset_minutes;

int errno;

void *malloc(size_t n) { return n > 0x7FFF0000u ? NULL : kmalloc(n); }
void *calloc(size_t n, size_t m) {
    if (m && n > 0x7FFF0000u / m) return NULL;
    void *p = malloc(n * m); if (p) memset(p, 0, n * m); return p;
}
void *realloc(void *p, size_t n) {
    if (!n) { kfree(p); return NULL; }
    return n > 0x7FFF0000u ? NULL : krealloc(p, n);
}
void free(void *p) { kfree(p); }
size_t malloc_usable_size(void *p) { return kmalloc_usable(p); }

void abort(void) { panic("JavaScript engine: abort()"); }
void exit(int c) { panic("JavaScript engine: exit(%d)", c); }
void qjs_assert_fail(const char *e, const char *f, int l) { panic("JavaScript engine: assertion %s failed (%s:%d)", e, f, l); }
int abs(int x) { return x < 0 ? -x : x; }

/* stdio: everything goes to the kernel log */
struct qjs_file { int fd; };
static struct qjs_file f_in = { 0 }, f_out = { 1 }, f_err = { 2 };
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char b[512]; int n = vsnprintf(b, sizeof b, fmt, ap);
    size_t l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0;
    klog("js: %s", b);
    (void)f; return n;
}
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(f, fmt, ap); va_end(ap); return n; }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = vfprintf(stdout, fmt, ap); va_end(ap); return n; }

/* time: wall clock from the RTC at boot plus the PIT tick count */
static int64_t boot_epoch_ms = -1;
static uint32_t boot_tick;
static int64_t days_from_civil(int y, unsigned m, unsigned d) {   /* H. Hinnant */
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static int64_t now_ms(void) {
    if (boot_epoch_ms < 0) {
        k_rtc_time_t t; rtc_read(&t);            /* the RTC keeps UTC */
        boot_epoch_ms = (days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second) * 1000;
        boot_tick = timer_ticks();
    }
    return boot_epoch_ms + (uint32_t)(timer_ticks() - boot_tick);
}
int gettimeofday(struct timeval *tv, void *tz) {
    int64_t ms = now_ms();
    tv->tv_sec = ms / 1000; tv->tv_usec = (ms % 1000) * 1000;
    (void)tz; return 0;
}
time_t time(time_t *t) { time_t s = now_ms() / 1000; if (t) *t = s; return s; }
int clock_gettime(int clk, struct timespec *ts) {
    if (clk == CLOCK_MONOTONIC) { uint32_t ms = timer_ticks(); ts->tv_sec = ms / 1000; ts->tv_nsec = (ms % 1000) * 1000000L; }
    else { int64_t ms = now_ms(); ts->tv_sec = ms / 1000; ts->tv_nsec = (ms % 1000) * 1000000L; }
    return 0;
}
struct tm *gmtime_r(const time_t *tp, struct tm *o) {
    int64_t t = *tp, days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    o->tm_hour = rem / 3600; o->tm_min = rem % 3600 / 60; o->tm_sec = rem % 60;
    o->tm_wday = (int)((days % 7 + 11) % 7);       /* 1970-01-01 was a Thursday */
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yoe + era * 400); unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    o->tm_year = y - 1900; o->tm_mon = m - 1; o->tm_mday = d;
    o->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    o->tm_isdst = 0; o->tm_gmtoff = 0; o->tm_zone = "UTC";
    return o;
}
struct tm *localtime_r(const time_t *tp, struct tm *o) {
    time_t t = *tp + (time_t)tz_offset_minutes * 60;
    gmtime_r(&t, o);
    o->tm_gmtoff = tz_offset_minutes * 60L; o->tm_zone = "";
    return o;
}

/* strtod: QuickJS parses numbers itself (dtoa.c); this is only a fallback */
double strtod(const char *s, char **end) {
    const char *p = s; while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int neg = 0; if (*p == '+' || *p == '-') neg = *p++ == '-';
    double v = 0; int any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = 1; }
    if (*p == '.') { p++; double f = 0.1; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f /= 10; any = 1; } }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1; int en = 0, e = 0; if (*q == '+' || *q == '-') en = *q++ == '-';
        if (*q >= '0' && *q <= '9') { while (*q >= '0' && *q <= '9') e = e * 10 + (*q++ - '0'); p = q; while (e--) v = en ? v / 10 : v * 10; }
    }
    if (end) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}
