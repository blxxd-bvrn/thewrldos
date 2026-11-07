/* 64-bit integer helpers normally provided by libgcc (so no multilib is needed) */
#include "types.h"
uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem) {
    if (d == 0) { if (rem) *rem = 0; return 0; }
    uint64_t q = 0, r = 0;
    if ((d >> 32) == 0 && (n >> 32) == 0) { uint32_t a = (uint32_t)n, b = (uint32_t)d; if (rem) *rem = a % b; return a / b; }
    if ((d >> 32) == 0) {                     /* 64 / 32: two hardware divisions */
        uint32_t b = (uint32_t)d, hi = (uint32_t)(n >> 32), lo = (uint32_t)n;
        uint32_t qh = hi / b, rh = hi % b, ql, rl;
        __asm__("divl %4" : "=a"(ql), "=d"(rl) : "a"(lo), "d"(rh), "rm"(b));
        if (rem) *rem = rl;
        return ((uint64_t)qh << 32) | ql;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; __udivmoddi4(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d) {
    int neg = (n < 0) ^ (d < 0);
    uint64_t q = __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t r;
    __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
int64_t __divmoddi4(int64_t n, int64_t d, int64_t *rem) {
    uint64_t r;
    uint64_t q = __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    if (rem) *rem = n < 0 ? -(int64_t)r : (int64_t)r;
    return ((n < 0) ^ (d < 0)) ? -(int64_t)q : (int64_t)q;
}
