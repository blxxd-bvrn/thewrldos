/* Math library for QuickJS on i686, built on x87 instructions.
 * Special cases (NaN, infinities, signed zeros) follow C99 Annex F, which is
 * what QuickJS expects; precision is that of the x87 (close to 1 ulp for the
 * common functions, which is ample for web pages). */
#include "qjs_libc.h"

typedef union { double d; uint64_t u; struct { uint32_t lo, hi; } w; } dbits;

double fabs(double x) { return __builtin_fabs(x); }
double copysign(double x, double y) { dbits a = { x }, b = { y }; a.u = (a.u & ~(1ull << 63)) | (b.u & (1ull << 63)); return a.d; }
float fabsf(float x) { return __builtin_fabsf(x); }

/* round with an explicit x87 rounding mode: 0 nearest, 1 down, 2 up, 3 toward zero */
static double rnd(double x, int mode) {
    if (!isfinite(x) || x == 0) return x;
    unsigned short cw, ncw; double r;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    ncw = (cw & ~0x0C00) | (mode << 10);
    __asm__ volatile("fldcw %0" :: "m"(ncw));
    __asm__ volatile("frndint" : "=t"(r) : "0"(x));
    __asm__ volatile("fldcw %0" :: "m"(cw));
    return copysign(r, x);                 /* keep -0 for e.g. ceil(-0.5) */
}
double floor(double x) { return rnd(x, 1); }
double ceil(double x) { return rnd(x, 2); }
double trunc(double x) { return rnd(x, 3); }
double rint(double x) { return rnd(x, 0); }
double nearbyint(double x) { return rnd(x, 0); }
double round(double x) {                    /* halfway cases away from zero */
    if (!isfinite(x)) return x;
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += x > 0 ? 1 : -1;
    return copysign(t, x);
}
long lrint(double x) { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }
float roundf(float x) { return (float)round(x); }
float truncf(float x) { return (float)trunc(x); }

double sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
float sqrtf(float x) { return (float)sqrt(x); }

double fmod(double x, double y) {
    if (isnan(x) || isnan(y) || isinf(x) || y == 0) return NAN;
    if (isinf(y)) return x;
    double r; unsigned short sw;
    do { __asm__ volatile("fprem; fnstsw %1" : "=t"(r), "=a"(sw) : "0"(x), "u"(y)); x = r; } while (sw & 0x400);
    return r;
}

/* range-reduced trig: fsin/fcos/fptan only accept |x| < 2^63 */
static double reduce(double x) { return fabs(x) < 9.2e18 ? x : fmod(x, 2 * M_PI); }
double sin(double x) { if (!isfinite(x)) return NAN; double r; __asm__("fsin" : "=t"(r) : "0"(reduce(x))); return r; }
double cos(double x) { if (!isfinite(x)) return NAN; double r; __asm__("fcos" : "=t"(r) : "0"(reduce(x))); return r; }
double tan(double x) { if (!isfinite(x)) return NAN; double r, one; __asm__("fptan" : "=t"(one), "=u"(r) : "0"(reduce(x))); (void)one; return r; }
double atan2(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
double atan(double x) { if (isnan(x)) return x; return atan2(x, 1.0); }
double asin(double x) { if (isnan(x) || fabs(x) > 1) return NAN; return atan2(x, sqrt((1 - x) * (1 + x))); }
double acos(double x) { if (isnan(x) || fabs(x) > 1) return NAN; return atan2(sqrt((1 - x) * (1 + x)), x); }

/* log2 via fyl2x; exp2 via f2xm1 + fscale */
double log2(double x) {
    if (isnan(x) || x < 0) return NAN;
    if (x == 0) return -INFINITY;
    if (isinf(x)) return x;
    double r; __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x)); return r;
}
double log(double x) {
    if (isnan(x) || x < 0) return NAN;
    if (x == 0) return -INFINITY;
    if (isinf(x)) return x;
    double r; __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"(x)); return r;
}
double log10(double x) {
    if (isnan(x) || x < 0) return NAN;
    if (x == 0) return -INFINITY;
    if (isinf(x)) return x;
    double r; __asm__("fldlg2; fxch; fyl2x" : "=t"(r) : "0"(x)); return r;
}
double log1p(double x) {
    if (isnan(x) || x < -1) return NAN;
    if (x == -1) return -INFINITY;
    if (fabs(x) < 0.29) { double r; __asm__("fldln2; fxch; fyl2xp1" : "=t"(r) : "0"(x)); return r; }   /* accurate near 0 */
    return log(1 + x);
}
static double exp2_l(long double x) {        /* x finite */
    long double ip, fp, r;
    __asm__("frndint" : "=t"(ip) : "0"(x));
    fp = x - ip;                             /* |fp| <= 0.5 */
    __asm__("f2xm1" : "=t"(r) : "0"(fp));
    r += 1;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(ip));
    return (double)r;
}
double exp2(double x) {
    if (isnan(x)) return x;
    if (x > 1024) return INFINITY;
    if (x < -1080) return 0;
    return exp2_l(x);
}
double exp(double x) {
    if (isnan(x)) return x;
    if (x > 710) return INFINITY;
    if (x < -750) return 0;
    return exp2_l((long double)x * 1.44269504088896340735992468100189214L);
}
double expm1(double x) {
    if (fabs(x) < 1e-5) return x + x * x / 2 + x * x * x / 6;
    return exp(x) - 1;
}
double sinh(double x) { if (fabs(x) < 1e-5) return x; double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(fabs(x)); return (e + 1 / e) / 2; }
double tanh(double x) {
    if (isnan(x)) return x;
    if (x > 22) return 1;
    if (x < -22) return -1;
    if (fabs(x) < 1e-5) return x;
    double e = expm1(2 * x); return e / (e + 2);
}
double asinh(double x) { if (!isfinite(x) || x == 0) return x; double a = fabs(x); double r = a > 1e8 ? log(a) + M_LN2 : log1p(a + a * a / (1 + sqrt(1 + a * a))); return copysign(r, x); }
double acosh(double x) { if (isnan(x) || x < 1) return NAN; return x > 1e8 ? log(x) + M_LN2 : log(x + sqrt(x * x - 1)); }
double atanh(double x) { if (isnan(x) || fabs(x) > 1) return NAN; if (fabs(x) == 1) return copysign(INFINITY, x); return copysign(0.5 * log1p(2 * fabs(x) / (1 - fabs(x))), x); }
double cbrt(double x) {
    if (!isfinite(x) || x == 0) return x;
    double r = exp2(log2(fabs(x)) / 3);
    r = r - (r * r * r - fabs(x)) / (3 * r * r);   /* one Newton step */
    return copysign(r, x);
}
double hypot(double a, double b) {
    if (isinf(a) || isinf(b)) return INFINITY;
    if (isnan(a) || isnan(b)) return NAN;
    a = fabs(a); b = fabs(b);
    if (a < b) { double t = a; a = b; b = t; }
    if (a == 0) return 0;
    double q = b / a; return a * sqrt(1 + q * q);
}
static int is_int(double y) { return isfinite(y) && trunc(y) == y; }
static int is_odd_int(double y) { return is_int(y) && fabs(y) < 9007199254740992.0 && fmod(fabs(y), 2) == 1; }
double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 1) return 1;
    if (isnan(x) || isnan(y)) return NAN;
    if (isinf(y)) {
        double ax = fabs(x);
        if (ax == 1) return 1;
        return (ax > 1) == (y > 0) ? INFINITY : 0;
    }
    if (x == 0) {
        if (y < 0) return is_odd_int(y) ? copysign(INFINITY, x) : INFINITY;
        return is_odd_int(y) ? x : 0;
    }
    if (isinf(x)) {
        if (x > 0) return y > 0 ? INFINITY : 0;
        if (y > 0) return is_odd_int(y) ? -INFINITY : INFINITY;
        return is_odd_int(y) ? -0.0 : 0;
    }
    if (x < 0 && !is_int(y)) return NAN;
    /* small integer powers by squaring: exact where the result is representable */
    if (is_int(y) && fabs(y) <= 64) {
        int n = (int)fabs(y); double r = 1, b = x;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return y < 0 ? 1 / r : r;
    }
    long double l; double ax = fabs(x);
    __asm__("fld1; fxch; fyl2x" : "=t"(l) : "0"((long double)ax));
    long double e = l * y;
    double r = e > 1100 ? INFINITY : e < -1100 ? 0 : exp2_l(e);
    return x < 0 && is_odd_int(y) ? -r : r;
}
double fmin(double a, double b) { if (isnan(a)) return b; if (isnan(b)) return a; return a < b ? a : b; }
double fmax(double a, double b) { if (isnan(a)) return b; if (isnan(b)) return a; return a > b ? a : b; }
double scalbn(double x, int n) {
    if (!isfinite(x) || x == 0) return x;
    if (n > 2100) n = 2100; if (n < -2100) n = -2100;
    long double r = x, e = n;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(e));
    return (double)r;
}
double ldexp(double x, int n) { return scalbn(x, n); }
double frexp(double x, int *e) {
    dbits b = { x };
    int ex = (b.w.hi >> 20) & 0x7FF;
    if (!ex) { if (x == 0) { *e = 0; return x; } x *= 18014398509481984.0; b.d = x; ex = ((b.w.hi >> 20) & 0x7FF) - 54; }
    else if (ex == 0x7FF) { *e = 0; return x; }
    else ex -= 0;
    *e = ex - 1022;
    b.w.hi = (b.w.hi & 0x800FFFFF) | (1022u << 20);
    return b.d;
}
double modf(double x, double *ip) { double i = trunc(x); *ip = i; return isinf(x) ? copysign(0, x) : x - i; }
