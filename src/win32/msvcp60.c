/* msvcp60.dll: the parts of Visual C++ 6's C++ library that programs import, which is
 * std::basic_string<char> (Notepad2's editing component uses it) and _Lockit.
 *
 * Programs inline much of the class (c_str, size, operator[] checks), so the object has
 * VC6's own layout: an empty allocator (one byte, padded), then _Ptr, _Len and _Res. The
 * character buffer is allocated with operator new and has a reference-count byte in front
 * of the characters (_Ptr[-1]: 0 owned, 255 "frozen"); strings made here are never shared,
 * which VC6's rules allow, so a count is always 0 or 255. Members are __thiscall on x86
 * (this in ECX) and the usual convention on x64; the comparison operators are __cdecl. */
#include "w32.h"

#define P w32_cur()
#ifdef __x86_64__
#define THISCALL __attribute__((ms_abi))
#else
#define THISCALL __attribute__((thiscall))
#endif

typedef struct { uint8_t alloc; char *ptr; size_t len, res; } vcstr_t;
#define NPOS ((size_t)-1)
#define REFCNT(p) (((uint8_t *)(p))[-1])
static const size_t npos_v = NPOS;
static const char nullstr_c = 0;                 /* basic_string::_Nullstr()'s static character */

static const char *cstr(const vcstr_t *s) { return s->ptr ? s->ptr : ""; }
static void release(vcstr_t *s) {
    if (s->ptr) {
        uint8_t r = REFCNT(s->ptr);
        if (r == 0 || r == 255) w32_free(P, s->ptr - 1);
        else REFCNT(s->ptr) = r - 1;                 /* shared by inlined code: leave it to the other owner */
    }
    s->ptr = NULL; s->len = s->res = 0;
}
/* an owned buffer for at least n characters, keeping the first min(len, n) */
static void reserve(vcstr_t *s, size_t n) {
    if (s->ptr && (REFCNT(s->ptr) == 0 || REFCNT(s->ptr) == 255) && s->res >= n) return;
    size_t cap = n | 31;
    char *b = w32_alloc(P, cap + 2);
    b[0] = 0; b++;
    size_t keep = MIN(s->len, n);
    if (s->ptr && keep) memcpy(b, s->ptr, keep);
    b[keep] = 0;
    release(s);
    s->ptr = b; s->len = keep; s->res = cap;
}
static void set_len(vcstr_t *s, size_t n) { s->len = n; if (s->ptr) s->ptr[n] = 0; }
static vcstr_t *assign_n(vcstr_t *s, const char *src, size_t n) {
    if (!n) { if (s->ptr) set_len(s, 0); return s; }
    if (s->ptr && src >= s->ptr && src < s->ptr + s->len) {   /* part of itself */
        memmove(s->ptr, src, n); set_len(s, n); return s;
    }
    reserve(s, n); memcpy(s->ptr, src, n); set_len(s, n);
    return s;
}
static vcstr_t *append_n(vcstr_t *s, const char *src, size_t n) {
    if (!n) return s;
    char tmp[256]; char *heap = NULL;
    if (s->ptr && src >= s->ptr && src < s->ptr + s->res + 1) { if (n <= sizeof tmp) { memcpy(tmp, src, n); src = tmp; } else { heap = kmalloc(n); memcpy(heap, src, n); src = heap; } }
    size_t l = s->len;
    reserve(s, l + n); memcpy(s->ptr + l, src, n); set_len(s, l + n);
    kfree(heap);
    return s;
}
static void init(vcstr_t *s) { s->alloc = 0; s->ptr = NULL; s->len = s->res = 0; }
static int compare(const char *a, size_t na, const char *b, size_t nb) {
    int c = memcmp(a, b, MIN(na, nb));
    return c ? c : na < nb ? -1 : na > nb ? 1 : 0;
}

/* constructors and destructor */
static vcstr_t *THISCALL ctor_copy(vcstr_t *s, const vcstr_t *x) { init(s); return assign_n(s, cstr(x), x->len); }
static vcstr_t *THISCALL ctor_sub(vcstr_t *s, const vcstr_t *x, size_t pos, size_t n, const void *al) {
    init(s);
    if (pos > x->len) pos = x->len;
    if (n > x->len - pos) n = x->len - pos;
    return assign_n(s, cstr(x) + pos, n);
}
static vcstr_t *THISCALL ctor_alloc(vcstr_t *s, const void *al) { init(s); return s; }
static vcstr_t *THISCALL ctor_fill(vcstr_t *s, size_t n, char c, const void *al) { init(s); if (n) { reserve(s, n); memset(s->ptr, c, n); set_len(s, n); } return s; }
static vcstr_t *THISCALL ctor_cstr(vcstr_t *s, const char *p, const void *al) { init(s); return p ? assign_n(s, p, strlen(p)) : s; }
static vcstr_t *THISCALL ctor_cstrn(vcstr_t *s, const char *p, size_t n, const void *al) { init(s); return p ? assign_n(s, p, n) : s; }
static void THISCALL dtor(vcstr_t *s) { release(s); }
/* assignment, appending, element access */
static vcstr_t *THISCALL op_assign(vcstr_t *s, const vcstr_t *x) { if (s == x) return s; return assign_n(s, cstr(x), x->len); }
static vcstr_t *THISCALL op_assign_cstr(vcstr_t *s, const char *p) { return assign_n(s, p ? p : "", p ? strlen(p) : 0); }
static vcstr_t *THISCALL op_append_char(vcstr_t *s, char c) { return append_n(s, &c, 1); }
static vcstr_t *THISCALL op_append_cstr(vcstr_t *s, const char *p) { return p ? append_n(s, p, strlen(p)) : s; }
static char *THISCALL op_index(vcstr_t *s, size_t i) {
    if (!s->ptr || s->len < i) return (char *)&nullstr_c;
    REFCNT(s->ptr) = 255;                            /* _Freeze: a reference may be kept */
    return s->ptr + i;
}
/* erase, find, insert */
static vcstr_t *THISCALL erase(vcstr_t *s, size_t pos, size_t n) {
    if (pos > s->len) pos = s->len;
    if (n > s->len - pos) n = s->len - pos;
    if (n && s->ptr) { memmove(s->ptr + pos, s->ptr + pos + n, s->len - pos - n); set_len(s, s->len - n); }
    return s;
}
static size_t THISCALL find(const vcstr_t *s, const char *p, size_t pos, size_t n) {
    if (n == 0 && pos <= s->len) return pos;
    if (!s->ptr || pos >= s->len || n > s->len - pos) return NPOS;
    for (size_t i = pos; i + n <= s->len; i++) if (s->ptr[i] == p[0] && !memcmp(s->ptr + i, p, n)) return i;
    return NPOS;
}
static vcstr_t *THISCALL insert(vcstr_t *s, size_t pos, const char *p, size_t n) {
    if (pos > s->len) pos = s->len;
    if (!n) return s;
    char *copy = kmalloc(n); memcpy(copy, p, n);       /* p may point into the string itself */
    size_t l = s->len;
    reserve(s, l + n);
    memmove(s->ptr + pos + n, s->ptr + pos, l - pos);
    memcpy(s->ptr + pos, copy, n);
    set_len(s, l + n);
    kfree(copy);
    return s;
}
/* the comparison operators (free functions) */
static bool CDECL op_eq_cstr(const vcstr_t *a, const char *b) { return !compare(cstr(a), a->len, b, strlen(b)); }
static bool CDECL op_ne(const vcstr_t *a, const vcstr_t *b) { return compare(cstr(a), a->len, cstr(b), b->len) != 0; }
static bool CDECL op_ne_cstr(const vcstr_t *a, const char *b) { return compare(cstr(a), a->len, b, strlen(b)) != 0; }
static bool CDECL op_lt(const vcstr_t *a, const vcstr_t *b) { return compare(cstr(a), a->len, cstr(b), b->len) < 0; }
/* _Lockit: one thread runs a program's C++ code at a time here, so nothing to lock */
static void *THISCALL lockit_ctor(void *t) { return t; }
static void THISCALL lockit_dtor(void *t) {}

#define S "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"
#define V(n, f) { "msvcp60", n, (void *)f }
const w32_api_t msvcp60_api[] = {
#ifndef __x86_64__
    V("??0" S "QAE@ABV01@@Z", ctor_copy), V("??0" S "QAE@ABV01@IIABV?$allocator@D@1@@Z", ctor_sub), V("??0" S "QAE@ABV?$allocator@D@1@@Z", ctor_alloc),
    V("??0" S "QAE@IDABV?$allocator@D@1@@Z", ctor_fill), V("??0" S "QAE@PBDABV?$allocator@D@1@@Z", ctor_cstr), V("??0" S "QAE@PBDIABV?$allocator@D@1@@Z", ctor_cstrn),
    V("??1" S "QAE@XZ", dtor), V("??4" S "QAEAAV01@ABV01@@Z", op_assign), V("??4" S "QAEAAV01@PBD@Z", op_assign_cstr),
    V("??8std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@PBD@Z", op_eq_cstr),
    V("??9std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", op_ne),
    V("??9std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@PBD@Z", op_ne_cstr),
    V("??Mstd@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", op_lt),
    V("??A" S "QAEAADI@Z", op_index), V("??Y" S "QAEAAV01@D@Z", op_append_char), V("??Y" S "QAEAAV01@PBD@Z", op_append_cstr),
    V("?_C@?1??_Nullstr@" S "CAPBDXZ@4DB", &nullstr_c), V("?erase@" S "QAEAAV12@II@Z", erase), V("?find@" S "QBEIPBDI@Z", find),
    V("?insert@" S "QAEAAV12@IPBDI@Z", insert), V("?npos@" S "2IB", &npos_v),
    V("??0_Lockit@std@@QAE@XZ", lockit_ctor), V("??1_Lockit@std@@QAE@XZ", lockit_dtor),
#else
    V("??0" S "QEAA@AEBV01@@Z", ctor_copy), V("??0" S "QEAA@AEBV01@_K1AEBV?$allocator@D@1@@Z", ctor_sub), V("??0" S "QEAA@AEBV?$allocator@D@1@@Z", ctor_alloc),
    V("??0" S "QEAA@_KDAEBV?$allocator@D@1@@Z", ctor_fill), V("??0" S "QEAA@PEBDAEBV?$allocator@D@1@@Z", ctor_cstr), V("??0" S "QEAA@PEBD_KAEBV?$allocator@D@1@@Z", ctor_cstrn),
    V("??1" S "QEAA@XZ", dtor), V("??4" S "QEAAAEAV01@AEBV01@@Z", op_assign), V("??4" S "QEAAAEAV01@PEBD@Z", op_assign_cstr),
    V("??8std@@YA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@PEBD@Z", op_eq_cstr),
    V("??9std@@YA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", op_ne),
    V("??9std@@YA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@PEBD@Z", op_ne_cstr),
    V("??Mstd@@YA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", op_lt),
    V("??A" S "QEAAAEAD_K@Z", op_index), V("??Y" S "QEAAAEAV01@D@Z", op_append_char), V("??Y" S "QEAAAEAV01@PEBD@Z", op_append_cstr),
    V("?_C@?1??_Nullstr@" S "CAPEBDXZ@4DB", &nullstr_c), V("?erase@" S "QEAAAEAV12@_K0@Z", erase), V("?find@" S "QEBA_KPEBD_K@Z", find),
    V("?insert@" S "QEAAAEAV12@_KPEBD0@Z", insert), V("?npos@" S "2_KB", &npos_v),
    V("??0_Lockit@std@@QEAA@XZ", lockit_ctor), V("??1_Lockit@std@@QEAA@XZ", lockit_dtor),
#endif
    { NULL, NULL, NULL }
};
