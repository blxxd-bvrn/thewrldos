/* Minimal C library interface for building QuickJS inside THE WRLD OS kernel.
 * String/memory/printf functions come from the kernel's lib.c; the rest is in
 * quickjs-shim/libc.c (allocation, time, stdio stubs) and libm.c (x87 math). */
#ifndef QJS_LIBC_H
#define QJS_LIBC_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <float.h>

typedef int32_t ssize_t;
typedef int64_t off_t;
typedef int64_t time_t;
typedef int64_t suseconds_t;
typedef long clock_t;

/* string.h */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strerror(int e);
int strcasecmp(const char *a, const char *b);

/* ctype.h */
int toupper(int c); int tolower(int c); int isdigit(int c); int isalpha(int c); int isalnum(int c);
int isspace(int c); int isxdigit(int c); int isupper(int c); int islower(int c); int isprint(int c);

/* stdlib.h */
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
size_t malloc_usable_size(void *p);
void abort(void) __attribute__((noreturn));
void exit(int c) __attribute__((noreturn));
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
int atoi(const char *s);
int abs(int x);
long labs(long x);
long long llabs(long long x);
char *getenv(const char *n);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
int rand(void);
void srand(unsigned s);
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

/* stdio.h */
typedef struct qjs_file FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int putchar(int c);
int puts(const char *s);
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int fflush(FILE *f);
#define putc fputc

/* errno.h */
extern int errno;
#define EINVAL 22
#define ENOMEM 12
#define ERANGE 34
#define EAGAIN 11
#define ENOENT 2
#define EDOM 33
#define EINTR 4

/* assert.h */
void qjs_assert_fail(const char *expr, const char *file, int line) __attribute__((noreturn));
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : qjs_assert_fail(#e, __FILE__, __LINE__))
#endif

/* time.h, sys/time.h */
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
struct timespec { time_t tv_sec; long tv_nsec; };
struct timezone { int tz_minuteswest, tz_dsttime; };
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; long tm_gmtoff; const char *tm_zone; };
int gettimeofday(struct timeval *tv, void *tz);
time_t time(time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);
clock_t clock(void);
#define CLOCKS_PER_SEC 1000
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0
int clock_gettime(int clk, struct timespec *ts);

/* alloca */
#define alloca __builtin_alloca

/* math.h */
#define NAN __builtin_nan("")
#define INFINITY __builtin_inf()
#define HUGE_VAL __builtin_huge_val()
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define M_PI 3.14159265358979323846
#define M_E 2.7182818284590452354
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440
double fabs(double); double floor(double); double ceil(double); double trunc(double); double round(double);
double rint(double); double nearbyint(double); long lrint(double); long long llrint(double);
double sqrt(double); double cbrt(double); double hypot(double, double);
double sin(double); double cos(double); double tan(double); double asin(double); double acos(double);
double atan(double); double atan2(double, double); double sinh(double); double cosh(double); double tanh(double);
double asinh(double); double acosh(double); double atanh(double);
double exp(double); double expm1(double); double exp2(double); double log(double); double log1p(double);
double log2(double); double log10(double); double pow(double, double); double fmod(double, double);
double fmin(double, double); double fmax(double, double); double copysign(double, double);
double ldexp(double, int); double frexp(double, int *); double modf(double, double *); double scalbn(double, int);
float fabsf(float); float sqrtf(float); float roundf(float); float truncf(float);
#endif
