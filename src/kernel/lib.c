/* Freestanding C library routines for the kernel */
#include "kernel.h"

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
        size_t w = n >> 2;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(w) :: "memory");
        n &= 3;
    }
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd == ss || n == 0) return d;
    if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
    while (n--) dd[n] = ss[n];
    return d;
}
void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
void *memset32(uint32_t *d, uint32_t v, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(v) : "memory");
    return r;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i] || !a[i]) return (uint8_t)a[i] - (uint8_t)b[i]; }
    return 0;
}
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((uint8_t)a[i]), y = tolower((uint8_t)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strlen(d);
    if (dl >= n) return dl + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? NULL : (char *)s; }
char *strrchr(const char *s, int c) { const char *r = NULL; for (; *s; s++) if (*s == (char)c) r = s; return c ? (char *)r : (char *)s; }
char *strstr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return NULL;
}
char *strdup(const char *s) { size_t l = strlen(s); char *r = kmalloc(l + 1); memcpy(r, s, l + 1); return r; }
char *strndup(const char *s, size_t n) {
    size_t l = 0; while (l < n && s[l]) l++;
    char *r = kmalloc(l + 1); memcpy(r, s, l); r[l] = 0; return r;
}
unsigned long strtoul(const char *s, char **end, int base) {
    unsigned long v = 0;
    while (isspace(*s)) s++;
    if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0' && s[1]) { base = 8; }
    else if (base == 0) base = 10;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return v;
}
long strtol(const char *s, char **end, int base) {
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    long v = (long)strtoul(s, end, base);
    return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

const char *str_ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return NULL;
    return strcasecmp(s + a - b, suffix) == 0 ? s + a - b : NULL;
}

/* ---------------- printf ---------------- */
typedef struct { char *buf; size_t cap, len; } outbuf_t;
static void ob_put(outbuf_t *o, char c) { if (o->len + 1 < o->cap) o->buf[o->len] = c; o->len++; }
static void ob_num(outbuf_t *o, uint64_t v, int base, int upper, int width, int zero, int left, int neg, int plus) {
    char tmp[24]; int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % base]; v /= base; }
    int sign = neg || plus;
    int pad = width - n - sign;
    if (!left && !zero) while (pad-- > 0) ob_put(o, ' ');
    if (neg) ob_put(o, '-'); else if (plus) ob_put(o, '+');
    if (!left && zero) while (pad-- > 0) ob_put(o, '0');
    while (n) ob_put(o, tmp[--n]);
    if (left) while (pad-- > 0) ob_put(o, ' ');
}
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    outbuf_t o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { ob_put(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, width = 0, prec = -1, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1; else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ' || *fmt == '#') {} else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { fmt++; prec = 0; if (*fmt == '*') { prec = va_arg(ap, int); fmt++; } else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, int64_t) : va_arg(ap, int);
            ob_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, 0, width, zero, left, v < 0, plus); break;
        }
        case 'u': { uint64_t v = lng >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); ob_num(&o, v, 10, 0, width, zero, left, 0, 0); break; }
        case 'x': case 'X': { uint64_t v = lng >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); ob_num(&o, v, 16, *fmt == 'X', width, zero, left, 0, 0); break; }
        case 'o': { unsigned v = va_arg(ap, unsigned); ob_num(&o, v, 8, 0, width, zero, left, 0, 0); break; }
        case 'p': { ob_put(&o, '0'); ob_put(&o, 'x'); ob_num(&o, va_arg(ap, uint32_t), 16, 0, 8, 1, 0, 0, 0); break; }
        case 'c': ob_put(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
            int l = strlen(s); if (prec >= 0 && l > prec) l = prec;
            if (!left) for (int i = l; i < width; i++) ob_put(&o, ' ');
            for (int i = 0; i < l; i++) ob_put(&o, s[i]);
            if (left) for (int i = l; i < width; i++) ob_put(&o, ' ');
            break;
        }
        case 'f': {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (v < 0) { ob_put(&o, '-'); v = -v; }
            uint64_t ip = (uint64_t)v; double fp = v - (double)ip;
            ob_num(&o, ip, 10, 0, 0, 0, 0, 0, 0);
            if (prec) { ob_put(&o, '.'); for (int i = 0; i < prec; i++) { fp *= 10; int d = (int)fp; ob_put(&o, '0' + d); fp -= d; } }
            break;
        }
        case '%': ob_put(&o, '%'); break;
        default: ob_put(&o, '%'); ob_put(&o, *fmt); break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return o.len;
}
int snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, 0x7fffffff, fmt, ap); va_end(ap); return r; }

void format_size(char *buf, size_t n, uint32_t b) {
    if (b < 1024) snprintf(buf, n, "%u bytes", b);
    else if (b < 1024 * 1024) snprintf(buf, n, "%u KB", (b + 1023) / 1024);
    else snprintf(buf, n, "%u.%u MB", b / (1024 * 1024), (b % (1024 * 1024)) * 10 / (1024 * 1024));
}

static uint32_t rng = 0x12345678;
void rand_seed(uint32_t s) { rng = s ? s : 1; }
uint32_t rand_next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
