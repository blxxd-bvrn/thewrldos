/* The less common DLLs programs import and probe: ole32 and oleaut32 basics (GUID strings,
 * SAFEARRAYs), advapi32 security and event-log calls, bcrypt's random numbers, crypt32
 * certificate stores, setupapi, winspool (no printers), wldap32 (no directory), netapi32,
 * normaliz, imm32, wtsapi32, userenv. Where THE WRLD OS has no such service the calls fail
 * the way Windows reports it, so programs carry on instead of stopping on a missing import. */
#include "w32.h"

#define P w32_cur()
typedef uint32_t ULONG;
static inline void set_err(uint32_t e) { w32_proc_t *p = P; if (p) p->last_error = e; }
static void *K(const char *dll, const char *name) { void *fn = NULL; w32_lookup(dll, name, &fn); return fn; }
static int a2w(const char *a, WCHAR *w, int cap) { int i = 0; for (; a[i] && i < cap - 1; i++) w[i] = (uint8_t)a[i]; if (cap > 0) w[i] = 0; return i; }
#define S_OK 0
#define S_FALSE 1
#define E_FAIL ((LONG)0x80004005)
#define E_NOTIMPL ((LONG)0x80004001)
#define E_INVALIDARG ((LONG)0x80070057)
#define E_OUTOFMEMORY ((LONG)0x8007000E)

/* ---------------------------------------------------------------- ole32 */
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } GUID32;
static int hexv(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static LONG WINAPI CLSIDFromString(const WCHAR *s, GUID32 *g) {
    memset(g, 0, sizeof *g);
    if (!s || s[0] != '{') return (LONG)0x800401F3;           /* CO_E_CLASSSTRING */
    static const int pos[] = { 1, 10, 15, 20, 22, 25, 27, 29, 31, 33, 35 };
    uint64_t v[11] = { 0 };
    int len[] = { 8, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2 };
    for (int k = 0; k < 11; k++) for (int i = 0; i < len[k]; i++) { int h = hexv(s[pos[k] + i]); if (h < 0) return (LONG)0x800401F3; v[k] = v[k] << 4 | h; }
    g->d1 = (uint32_t)v[0]; g->d2 = (uint16_t)v[1]; g->d3 = (uint16_t)v[2];
    for (int i = 0; i < 8; i++) g->d4[i] = (uint8_t)v[3 + i];
    return S_OK;
}
static LONG WINAPI IIDFromString(const WCHAR *s, GUID32 *g) { return CLSIDFromString(s, g); }
static int guid_text(const GUID32 *g, char *b) {
    return sprintf(b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->d1, g->d2, g->d3, g->d4[0], g->d4[1], g->d4[2], g->d4[3], g->d4[4], g->d4[5], g->d4[6], g->d4[7]);
}
static LONG WINAPI StringFromCLSID(const GUID32 *g, WCHAR **out) {
    char b[40]; guid_text(g, b);
    void *(WINAPI *alloc)(uintptr_t) = K("ole32.dll", "CoTaskMemAlloc");
    *out = alloc ? alloc(80) : w32_alloc(P, 80);
    a2w(b, *out, 40);
    return S_OK;
}
static int WINAPI StringFromGUID2(const GUID32 *g, WCHAR *b, int n) { char a[40]; int l = guid_text(g, a); if (n <= l) return 0; a2w(a, b, n); return l + 1; }
static LONG WINAPI CoCreateGuid(GUID32 *g) { uint8_t *b = (uint8_t *)g; for (int i = 0; i < 16; i++) b[i] = rand_next(); g->d3 = (g->d3 & 0x0FFF) | 0x4000; g->d4[0] = (g->d4[0] & 0x3F) | 0x80; return S_OK; }
static BOOL WINAPI IsEqualGUID(const GUID32 *a, const GUID32 *b) { return !memcmp(a, b, 16); }
static LONG WINAPI CoDisconnectObject(void *u, DWORD r) { return S_OK; }
static LONG WINAPI CreateDataAdviseHolder(void **out) { *out = NULL; return E_NOTIMPL; }
static LONG WINAPI CreateStreamOnHGlobal(void *h, BOOL del, void **out) { *out = NULL; return E_NOTIMPL; }
static LONG WINAPI GetHGlobalFromStream(void *s, void **h) { return E_INVALIDARG; }
static LONG WINAPI DoDragDrop(void *d, void *s, DWORD ok, DWORD *eff) { if (eff) *eff = 0; return 0x00040101; }   /* DRAGDROP_S_CANCEL */
static LONG WINAPI RegisterDragDrop(HWND h, void *t) { return S_OK; }
static LONG WINAPI RevokeDragDrop(HWND h) { return S_OK; }
static LONG WINAPI OleGetClipboard(void **out) { *out = NULL; return E_FAIL; }
static LONG WINAPI OleSetClipboard(void *d) { return d ? E_FAIL : S_OK; }
static LONG WINAPI OleFlushClipboard(void) { return S_OK; }
static LONG WINAPI OleIsCurrentClipboard(void *d) { return S_FALSE; }
static LONG WINAPI PropVariantClear(void *v) { memset(v, 0, 16); return S_OK; }
static void WINAPI ReleaseStgMedium(void *m) {}
static LONG WINAPI CoRegisterClassObject(const void *c, void *u, DWORD ctx, DWORD f, DWORD *cookie) { if (cookie) *cookie = 1; return S_OK; }
static LONG WINAPI CoRevokeClassObject(DWORD c) { return S_OK; }
static LONG WINAPI CoInitializeSecurity(void *a, LONG b, void *c, void *d, DWORD e, DWORD f, void *g, DWORD h, void *i) { return S_OK; }
static LONG WINAPI CoGetClassObject(const void *c, DWORD ctx, void *s, const void *iid, void **out) { *out = NULL; return (LONG)0x80040154; }
static LONG WINAPI CoLockObjectExternal(void *u, BOOL l, BOOL r) { return S_OK; }
static LONG WINAPI CoSetProxyBlanket(void *p, DWORD a, DWORD b, void *c, DWORD d, DWORD e, void *f, DWORD g) { return S_OK; }
static LONG WINAPI OleInitialize2(void *r) { return S_OK; }
static LONG WINAPI CoFreeUnusedLibraries(void) { return S_OK; }
static LONG WINAPI ProgIDFromCLSID(const void *c, WCHAR **out) { *out = NULL; return (LONG)0x800401F3; }
static LONG WINAPI CLSIDFromProgID(const WCHAR *p, GUID32 *g) { memset(g, 0, 16); return (LONG)0x800401F3; }

/* ---------------------------------------------------------------- oleaut32: SAFEARRAY */
typedef struct { ULONG n; LONG lb; } SABOUND;
typedef struct { uint16_t dims, features; ULONG elem, locks; void *data; SABOUND b[1]; } SAFEARRAY32;
static int vt_size(uint16_t vt) {
    switch (vt) { case 2: case 11: case 18: return 2; case 3: case 4: case 10: case 19: case 22: case 23: return 4; case 5: case 6: case 7: case 20: case 21: return 8;
    case 8: case 9: case 13: return sizeof(void *); case 12: return sizeof(void *) == 8 ? 24 : 16; case 16: case 17: return 1; }
    return 0;
}
static SAFEARRAY32 *WINAPI SafeArrayCreate(uint16_t vt, UINT dims, const SABOUND *b) {
    int es = vt_size(vt); if (!es || !dims || dims > 16) return NULL;
    SAFEARRAY32 *a = w32_alloc(P, sizeof(SAFEARRAY32) + (dims - 1) * sizeof(SABOUND));
    memset(a, 0, sizeof(SAFEARRAY32) + (dims - 1) * sizeof(SABOUND));
    a->dims = dims; a->elem = es; a->features = vt == 8 ? 0x100 : vt == 12 ? 0x800 : (vt == 9 || vt == 13) ? 0x200 : 0;
    size_t total = es;
    for (UINT i = 0; i < dims; i++) { a->b[dims - 1 - i] = b[i]; total *= b[i].n; }
    a->data = w32_alloc(P, total ? total : 1); memset(a->data, 0, total ? total : 1);
    return a;
}
static SAFEARRAY32 *WINAPI SafeArrayCreateVector(uint16_t vt, LONG lb, ULONG n) { SABOUND b = { n, lb }; return SafeArrayCreate(vt, 1, &b); }
static LONG WINAPI SafeArrayDestroy(SAFEARRAY32 *a) { if (!a) return S_OK; if (a->locks) return (LONG)0x8002000D; w32_free(P, a->data); w32_free(P, a); return S_OK; }
static LONG WINAPI SafeArrayAccessData(SAFEARRAY32 *a, void **d) { if (!a) return E_INVALIDARG; a->locks++; *d = a->data; return S_OK; }
static LONG WINAPI SafeArrayUnaccessData(SAFEARRAY32 *a) { if (!a) return E_INVALIDARG; if (a->locks) a->locks--; return S_OK; }
static LONG WINAPI SafeArrayLock(SAFEARRAY32 *a) { if (!a) return E_INVALIDARG; a->locks++; return S_OK; }
static LONG WINAPI SafeArrayUnlock(SAFEARRAY32 *a) { if (!a) return E_INVALIDARG; if (a->locks) a->locks--; return S_OK; }
static UINT WINAPI SafeArrayGetDim(SAFEARRAY32 *a) { return a ? a->dims : 0; }
static UINT WINAPI SafeArrayGetElemsize(SAFEARRAY32 *a) { return a ? a->elem : 0; }
static LONG WINAPI SafeArrayGetLBound(SAFEARRAY32 *a, UINT d, LONG *v) { if (!a || !d || d > a->dims) return (LONG)0x8002000B; *v = a->b[a->dims - d].lb; return S_OK; }
static LONG WINAPI SafeArrayGetUBound(SAFEARRAY32 *a, UINT d, LONG *v) { if (!a || !d || d > a->dims) return (LONG)0x8002000B; *v = a->b[a->dims - d].lb + (LONG)a->b[a->dims - d].n - 1; return S_OK; }
static LONG elem_offset(SAFEARRAY32 *a, const LONG *idx, size_t *off) {
    size_t o = 0, mul = 1;
    for (UINT i = 0; i < a->dims; i++) {
        SABOUND *b = &a->b[a->dims - 1 - i];
        LONG k = idx[i] - b->lb; if (k < 0 || (ULONG)k >= b->n) return (LONG)0x8002000B;
        o += k * mul; mul *= b->n;
    }
    *off = o * a->elem; return S_OK;
}
static LONG WINAPI SafeArrayPtrOfIndex(SAFEARRAY32 *a, const LONG *idx, void **p) { size_t o; LONG r = a ? elem_offset(a, idx, &o) : E_INVALIDARG; if (!r) *p = (uint8_t *)a->data + o; return r; }
static LONG WINAPI SafeArrayGetElement(SAFEARRAY32 *a, const LONG *idx, void *v) { size_t o; LONG r = a ? elem_offset(a, idx, &o) : E_INVALIDARG; if (!r) memcpy(v, (uint8_t *)a->data + o, a->elem); return r; }
static LONG WINAPI SafeArrayPutElement(SAFEARRAY32 *a, const LONG *idx, const void *v) {
    size_t o; LONG r = a ? elem_offset(a, idx, &o) : E_INVALIDARG; if (r) return r;
    if (a->features & 0x100) {                             /* BSTR: a copy of the string */
        WCHAR *(WINAPI *dup)(const WCHAR *) = K("oleaut32.dll", "SysAllocString");
        *(WCHAR **)((uint8_t *)a->data + o) = dup ? dup(v) : (WCHAR *)v;
    } else memcpy((uint8_t *)a->data + o, a->features & 0x200 ? &v : v, a->elem);
    return S_OK;
}
static LONG WINAPI SafeArrayCopy(SAFEARRAY32 *a, SAFEARRAY32 **out) {
    if (!a) { *out = NULL; return S_OK; }
    size_t n = a->elem; for (UINT i = 0; i < a->dims; i++) n *= a->b[i].n;
    size_t hs = sizeof(SAFEARRAY32) + (a->dims - 1) * sizeof(SABOUND);
    SAFEARRAY32 *c = w32_alloc(P, hs); memcpy(c, a, hs); c->locks = 0;
    c->data = w32_alloc(P, n ? n : 1); memcpy(c->data, a->data, n);
    *out = c; return S_OK;
}
static BOOL WINAPI SysReAllocStringLen(WCHAR **b, const WCHAR *s, UINT n) {
    void (WINAPI *fr)(WCHAR *) = K("oleaut32.dll", "SysFreeString");
    WCHAR *(WINAPI *al)(const WCHAR *, UINT) = K("oleaut32.dll", "SysAllocStringLen");
    if (!al) return 0;
    WCHAR *nb = al(s, n); if (!nb) return 0;
    if (fr && *b) fr(*b);
    *b = nb; return 1;
}
static BOOL WINAPI SysReAllocString(WCHAR **b, const WCHAR *s) { UINT n = 0; while (s && s[n]) n++; return SysReAllocStringLen(b, s, n); }
static LONG WINAPI VariantCopyInd(void *d, const void *s) { LONG (WINAPI *cp)(void *, const void *) = K("oleaut32.dll", "VariantCopy"); return cp ? cp(d, s) : E_FAIL; }
static LONG WINAPI VariantChangeType(void *d, const void *s, uint16_t f, uint16_t vt) {
    const uint16_t *sv = s; uint16_t *dv = d;
    if (sv[0] == vt) return VariantCopyInd(d, s);
    int64_t n = 0;
    switch (sv[0]) { case 2: n = *(int16_t *)(sv + 4); break; case 3: n = *(int32_t *)(sv + 4); break; case 11: n = *(int16_t *)(sv + 4) ? 1 : 0; break; case 17: n = *(uint8_t *)(sv + 4); break; case 19: n = *(uint32_t *)(sv + 4); break; default: return (LONG)0x80020005; }
    memset(dv, 0, 16); dv[0] = vt;
    switch (vt) { case 2: *(int16_t *)(dv + 4) = (int16_t)n; break; case 3: case 22: *(int32_t *)(dv + 4) = (int32_t)n; break; case 11: *(int16_t *)(dv + 4) = n ? -1 : 0; break; case 19: case 23: *(uint32_t *)(dv + 4) = (uint32_t)n; break; case 5: *(double *)(dv + 4) = (double)n; break; default: return (LONG)0x80020005; }
    return S_OK;
}
static LONG WINAPI VariantChangeTypeEx(void *d, const void *s, DWORD l, uint16_t f, uint16_t vt) { return VariantChangeType(d, s, f, vt); }
static LONG WINAPI GetErrorInfo(DWORD r, void **e) { *e = NULL; return S_FALSE; }
static LONG WINAPI SetErrorInfo(DWORD r, void *e) { return S_OK; }
static LONG WINAPI CreateErrorInfo(void **e) { *e = NULL; return E_OUTOFMEMORY; }
static LONG WINAPI LoadTypeLib(const WCHAR *f, void **t) { *t = NULL; return (LONG)0x80029C4A; }
static LONG WINAPI LoadRegTypeLib(const void *g, WORD a, WORD b, DWORD l, void **t) { *t = NULL; return (LONG)0x8002801D; }
static LONG WINAPI RegisterTypeLib(void *t, const WCHAR *a, const WCHAR *b) { return S_OK; }
static LONG WINAPI VarBstrCmp(const WCHAR *a, const WCHAR *b, DWORD l, DWORD f) { int i = 0; if (!a || !b) return a == b ? 1 : a ? 2 : 0; for (; a[i] && a[i] == b[i]; i++); return a[i] == b[i] ? 1 : a[i] < b[i] ? 0 : 2; }
static int WINAPI SystemTimeToVariantTime(const WORD *st, double *v) {
    int y = st[0], m = st[1], d = st[3];
    int64_t days = 0; { int yy = y - (m <= 2), era = (yy >= 0 ? yy : yy - 399) / 400, yoe = yy - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; days = (int64_t)era * 146097 + doe - 719468; }
    *v = (double)(days + 25569) + (st[4] * 3600 + st[5] * 60 + st[6]) / 86400.0;
    return 1;
}

/* ---------------------------------------------------------------- advapi32: security and the event log */
static BOOL WINAPI AccessCheck(void *sd, HANDLE t, DWORD want, void *map, void *ps, DWORD *pslen, DWORD *granted, BOOL *ok) { if (granted) *granted = want; if (ok) *ok = 1; return 1; }
static BOOL WINAPI ImpersonateSelf(int level) { return 1; }
static BOOL WINAPI RevertToSelf(void) { return 1; }
static BOOL WINAPI GetFileSecurityA(const char *f, DWORD i, void *sd, DWORD n, DWORD *need) { if (need) *need = 20; if (n < 20 || !sd) { set_err(122); return 0; } memset(sd, 0, 20); ((uint8_t *)sd)[0] = 1; return 1; }
static BOOL WINAPI SetFileSecurityA(const char *f, DWORD i, void *sd) { return 1; }
static DWORD WINAPI GetNamedSecurityInfoW(const WCHAR *n, int type, DWORD info, void **owner, void **group, void **dacl, void **sacl, void **sd) {
    if (owner) *owner = NULL; if (group) *group = NULL; if (dacl) *dacl = NULL; if (sacl) *sacl = NULL;
    if (sd) { void *(WINAPI *la)(UINT, uintptr_t) = K("kernel32.dll", "LocalAlloc"); *sd = la ? la(0x40, 20) : NULL; if (*sd) ((uint8_t *)*sd)[0] = 1; }
    return 0;
}
static DWORD WINAPI SetNamedSecurityInfoW(WCHAR *n, int type, DWORD info, void *o, void *g, void *d, void *s) { return 0; }
static BOOL WINAPI GetSecurityDescriptorOwner(void *sd, void **owner, BOOL *def) { *owner = NULL; if (def) *def = 0; return 1; }
static BOOL WINAPI GetSecurityDescriptorDacl(void *sd, BOOL *present, void **dacl, BOOL *def) { *present = 0; *dacl = NULL; if (def) *def = 0; return 1; }
static uint8_t *WINAPI GetSidIdentifierAuthority(void *sid) { return (uint8_t *)sid + 2; }
static DWORD *WINAPI GetSidSubAuthority(void *sid, DWORD i) { return (DWORD *)((uint8_t *)sid + 8 + 4 * i); }
static uint8_t *WINAPI GetSidSubAuthorityCount(void *sid) { return (uint8_t *)sid + 1; }
static BOOL WINAPI ConvertSidToStringSidW(void *sid, WCHAR **out) { void *(WINAPI *la)(UINT, uintptr_t) = K("kernel32.dll", "LocalAlloc"); *out = la(0x40, 64); a2w("S-1-5-21-1000-1000-1000-1000", *out, 32); return 1; }
static BOOL WINAPI ConvertSidToStringSidA(void *sid, char **out) { void *(WINAPI *la)(UINT, uintptr_t) = K("kernel32.dll", "LocalAlloc"); *out = la(0x40, 32); strcpy(*out, "S-1-5-21-1000-1000-1000-1000"); return 1; }
static BOOL WINAPI LookupAccountSidW(const WCHAR *s, void *sid, WCHAR *name, DWORD *nn, WCHAR *dom, DWORD *dn, int *use) {
    DWORD l = strlen(user_name), dl = strlen(computer_name);
    if (!name || *nn <= l || !dom || *dn <= dl) { *nn = l + 1; *dn = dl + 1; set_err(122); return 0; }
    a2w(user_name, name, *nn); a2w(computer_name, dom, *dn); *nn = l; *dn = dl; if (use) *use = 1; return 1;
}
static BOOL WINAPI IsTextUnicode(const void *b, int n, int *r) {
    const uint8_t *p = b; int zeros_odd = 0, zeros_even = 0;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { if (r) *r = 8; return 1; }
    for (int i = 0; i + 1 < n; i += 2) { if (!p[i + 1]) zeros_odd++; if (!p[i]) zeros_even++; }
    bool u = n >= 4 && zeros_odd > n / 4 && zeros_even < n / 16;
    if (r) *r = u ? 2 : 0;
    return u;
}
static BOOL WINAPI CryptAcquireContextW(HANDLE *h, const WCHAR *a, const WCHAR *b, DWORD t, DWORD f) { *h = 1; return 1; }
static HANDLE WINAPI RegisterEventSourceW(const WCHAR *s, const WCHAR *n) { return 0x45564C31; }
static HANDLE WINAPI RegisterEventSourceA(const char *s, const char *n) { return 0x45564C31; }
static BOOL WINAPI DeregisterEventSource(HANDLE h) { return 1; }
static BOOL WINAPI ReportEventW(HANDLE h, WORD type, WORD cat, DWORD id, void *sid, WORD n, DWORD dl, const WCHAR **s, void *d) { return 1; }
static BOOL WINAPI ReportEventA(HANDLE h, WORD type, WORD cat, DWORD id, void *sid, WORD n, DWORD dl, const char **s, void *d) { if (n && s && s[0]) klog("w32: event log: %s", s[0]); return 1; }
static ULONG WINAPI EventRegister(const void *g, void *cb, void *ctx, uint64_t *h) { if (h) *h = 1; return 0; }
static ULONG WINAPI EventUnregister(uint64_t h) { return 0; }
static ULONG WINAPI EventWrite(uint64_t h, const void *d, ULONG n, void *data) { return 0; }
static BOOL WINAPI EventEnabled(uint64_t h, const void *d) { return 0; }
static HANDLE WINAPI OpenSCManagerW(const WCHAR *m, const WCHAR *d, DWORD a) { set_err(5); return 0; }
static HANDLE WINAPI OpenSCManagerA(const char *m, const char *d, DWORD a) { set_err(5); return 0; }
static BOOL WINAPI CloseServiceHandle(HANDLE h) { return 1; }
static BOOL WINAPI SystemFunction040(void *m, ULONG n, ULONG f) { return 0; }
static BOOL WINAPI SystemFunction041(void *m, ULONG n, ULONG f) { return 0; }
/* bcrypt: random numbers, the rest not provided */
static LONG WINAPI BCryptGenRandom(HANDLE alg, uint8_t *b, ULONG n, ULONG f) { for (ULONG i = 0; i < n; i++) b[i] = rand_next(); return 0; }
static LONG WINAPI BCryptOpenAlgorithmProvider(HANDLE *h, const WCHAR *id, const WCHAR *impl, ULONG f) { *h = 0; return (LONG)0xC0000225; }
static LONG WINAPI BCryptCloseAlgorithmProvider(HANDLE h, ULONG f) { return 0; }
static BOOL WINAPI ProcessPrng(uint8_t *b, uintptr_t n) { for (uintptr_t i = 0; i < n; i++) b[i] = rand_next(); return 1; }
/* crypt32: no certificate stores */
static HANDLE WINAPI CertOpenSystemStoreA(uintptr_t p, const char *n) { set_err(2); return 0; }
static HANDLE WINAPI CertOpenSystemStoreW(uintptr_t p, const WCHAR *n) { set_err(2); return 0; }
static HANDLE WINAPI CertOpenStore(const char *p, DWORD e, uintptr_t h, DWORD f, const void *para) { set_err(2); return 0; }
static BOOL WINAPI CertCloseStore(HANDLE h, DWORD f) { return 1; }
static const void *WINAPI CertEnumCertificatesInStore(HANDLE h, const void *prev) { set_err(0x80092004); return NULL; }
static BOOL WINAPI CertFreeCertificateContext(const void *c) { return 1; }
static BOOL WINAPI CertGetEnhancedKeyUsage(const void *c, DWORD f, void *u, DWORD *n) { set_err(0x80092004); return 0; }
static BOOL WINAPI CertGetIntendedKeyUsage(DWORD e, void *info, BYTE *u, DWORD n) { return 0; }
static DWORD WINAPI CertGetNameStringA(const void *c, DWORD t, DWORD f, void *p, char *b, DWORD n) { if (b && n) b[0] = 0; return 1; }
static DWORD WINAPI CertGetNameStringW(const void *c, DWORD t, DWORD f, void *p, WCHAR *b, DWORD n) { if (b && n) b[0] = 0; return 1; }
static BOOL WINAPI CryptStringToBinaryA(const char *s, DWORD n, DWORD f, BYTE *b, DWORD *bn, DWORD *skip, DWORD *ff) { return 0; }
static BOOL WINAPI CryptProtectData(void *in, const WCHAR *d, void *e, void *r, void *p, DWORD f, void *out) { return 0; }
static BOOL WINAPI CryptUnprotectData(void *in, WCHAR **d, void *e, void *r, void *p, DWORD f, void *out) { return 0; }
/* setupapi and cfgmgr32: no device enumeration for programs */
#define INVALID_HV ((HANDLE)(intptr_t)-1)
static HANDLE WINAPI SetupDiGetClassDevsA(const void *g, const char *e, HWND h, DWORD f) { set_err(13); return INVALID_HV; }
static HANDLE WINAPI SetupDiGetClassDevsW(const void *g, const WCHAR *e, HWND h, DWORD f) { set_err(13); return INVALID_HV; }
static BOOL WINAPI SetupDiDestroyDeviceInfoList(HANDLE h) { return 1; }
static BOOL WINAPI SetupDiEnumDeviceInfo(HANDLE h, DWORD i, void *d) { set_err(259); return 0; }
static BOOL WINAPI SetupDiEnumDeviceInterfaces(HANDLE h, void *d, const void *g, DWORD i, void *o) { set_err(259); return 0; }
static BOOL WINAPI SetupDiGetDeviceInterfaceDetailA(HANDLE h, void *i, void *d, DWORD n, DWORD *need, void *di) { set_err(259); return 0; }
static BOOL WINAPI SetupDiGetDeviceInterfaceDetailW(HANDLE h, void *i, void *d, DWORD n, DWORD *need, void *di) { set_err(259); return 0; }
static BOOL WINAPI SetupDiGetDeviceRegistryPropertyA(HANDLE h, void *d, DWORD p, DWORD *t, BYTE *b, DWORD n, DWORD *need) { set_err(13); return 0; }
static BOOL WINAPI SetupDiGetDeviceRegistryPropertyW(HANDLE h, void *d, DWORD p, DWORD *t, BYTE *b, DWORD n, DWORD *need) { set_err(13); return 0; }
static DWORD WINAPI CM_Locate_DevNodeA(DWORD *n, const char *id, ULONG f) { return 0xD; }          /* CR_NO_SUCH_DEVNODE */
static DWORD WINAPI CM_Locate_DevNodeW(DWORD *n, const WCHAR *id, ULONG f) { return 0xD; }
static DWORD WINAPI CM_Get_Parent(DWORD *p, DWORD n, ULONG f) { return 0xD; }
static DWORD WINAPI CM_Get_Device_IDA(DWORD n, char *b, ULONG l, ULONG f) { return 0xD; }
static DWORD WINAPI CM_Get_Device_IDW(DWORD n, WCHAR *b, ULONG l, ULONG f) { return 0xD; }
/* winspool: no printers */
static BOOL WINAPI OpenPrinterA(char *n, HANDLE *h, void *d) { *h = 0; set_err(1801); return 0; }
static BOOL WINAPI OpenPrinterW(WCHAR *n, HANDLE *h, void *d) { *h = 0; set_err(1801); return 0; }
static BOOL WINAPI ClosePrinter(HANDLE h) { return 1; }
static LONG WINAPI DocumentPropertiesA(HWND w, HANDLE h, char *n, void *o, void *i, DWORD m) { return -1; }
static LONG WINAPI DocumentPropertiesW(HWND w, HANDLE h, WCHAR *n, void *o, void *i, DWORD m) { return -1; }
static BOOL WINAPI EnumPrintersW(DWORD f, WCHAR *n, DWORD l, BYTE *b, DWORD c, DWORD *need, DWORD *ret) { if (need) *need = 0; if (ret) *ret = 0; return 1; }
static BOOL WINAPI EnumPrintersA(DWORD f, char *n, DWORD l, BYTE *b, DWORD c, DWORD *need, DWORD *ret) { if (need) *need = 0; if (ret) *ret = 0; return 1; }
static BOOL WINAPI GetDefaultPrinterW(WCHAR *b, DWORD *n) { set_err(1801); return 0; }
static BOOL WINAPI GetDefaultPrinterA(char *b, DWORD *n) { set_err(1801); return 0; }
/* wldap32: no directory server */
static void *WINAPI ldap_init(const char *h, ULONG p) { set_err(0x51); return NULL; }
static void *WINAPI ldap_sslinit(const char *h, ULONG p, int s) { set_err(0x51); return NULL; }
static ULONG WINAPI ldap_err_ul(void) { return 0x51; }                /* LDAP_SERVER_DOWN */
static ULONG WINAPI ldap_bind_s(void *l, const char *dn, const char *c, ULONG m) { return 0x51; }
static ULONG WINAPI ldap_simple_bind_s(void *l, const char *dn, const char *pw) { return 0x51; }
static ULONG WINAPI ldap_unbind_s(void *l) { return 0; }
static ULONG WINAPI ldap_search_s(void *l, const char *b, ULONG s, const char *f, char **a, ULONG o, void **res) { if (res) *res = NULL; return 0x51; }
static ULONG WINAPI ldap_set_option(void *l, int o, const void *v) { return 0; }
static char *WINAPI ldap_err2string(ULONG e) { static char s[] = "The LDAP server is unavailable."; return s; }
static void *WINAPI ldap_null(void) { return NULL; }
static ULONG WINAPI ldap_zero(void) { return 0; }
/* netapi32: not in a domain */
static DWORD WINAPI NetApiBufferFree(void *b) { return 0; }
static DWORD WINAPI NetGetDCName(const WCHAR *s, const WCHAR *d, BYTE **b) { *b = NULL; return 2453; }   /* NERR_DCNotFound */
static DWORD WINAPI NetUserGetInfo(const WCHAR *s, const WCHAR *u, DWORD l, BYTE **b) { *b = NULL; return 2221; }   /* NERR_UserNotFound */
static DWORD WINAPI NetWkstaGetInfo(WCHAR *s, DWORD l, BYTE **b) { *b = NULL; return 2102; }
/* normaliz: host names are kept as they are (ASCII names need no conversion) */
static int WINAPI IdnToAscii(DWORD f, const WCHAR *in, int n, WCHAR *out, int on) {
    if (n < 0) { n = 0; while (in[n]) n++; n++; }
    for (int i = 0; i < n; i++) if (in[i] > 127) { set_err(1113); return 0; }
    if (!on) return n;
    if (on < n) { set_err(122); return 0; }
    memcpy(out, in, n * 2); return n;
}
static int WINAPI IdnToUnicode(DWORD f, const WCHAR *in, int n, WCHAR *out, int on) { if (n < 0) { n = 0; while (in[n]) n++; n++; } if (!on) return n; if (on < n) { set_err(122); return 0; } memcpy(out, in, n * 2); return n; }
/* imm32: no input method editor */
static DWORD WINAPI ImmGetCandidateListW(HANDLE c, DWORD i, void *l, DWORD n) { return 0; }
static UINT WINAPI ImmGetIMEFileNameA(HANDLE k, char *b, UINT n) { if (b && n) b[0] = 0; return 0; }
static BOOL WINAPI ImmNotifyIME(HANDLE c, DWORD a, DWORD i, DWORD v) { return 0; }
static BOOL WINAPI ImmSetCandidateWindow(HANDLE c, void *cf) { return 1; }
static BOOL WINAPI ImmSetCompositionStringW(HANDLE c, DWORD i, const void *a, DWORD n, const void *b, DWORD m) { return 0; }
static BOOL WINAPI ImmIsIME(HANDLE k) { return 0; }
static HWND WINAPI ImmGetDefaultIMEWnd(HWND h) { return 0; }
static BOOL WINAPI ImmDisableIME(DWORD t) { return 1; }
/* wtsapi32, userenv, secur32, oleacc, iphlpapi */
static BOOL WINAPI WTSRegisterSessionNotification(HWND h, DWORD f) { return 1; }
static BOOL WINAPI WTSUnRegisterSessionNotification(HWND h) { return 1; }
static BOOL WINAPI WTSQuerySessionInformationW(HANDLE s, DWORD id, int c, WCHAR **b, DWORD *n) { set_err(1); return 0; }
static void WINAPI WTSFreeMemory(void *p) {}
static BOOL WINAPI GetProfilesDirectoryW(WCHAR *b, DWORD *n) { const char *d = "C:\\Documents and Settings"; DWORD l = strlen(d); if (!b || *n <= l) { *n = l + 1; set_err(122); return 0; } a2w(d, b, *n); *n = l + 1; return 1; }
static BOOL WINAPI GetUserProfileDirectoryW(HANDLE t, WCHAR *b, DWORD *n) { const char *d = USER_HOME; DWORD l = strlen(d); if (!b || *n <= l) { *n = l + 1; set_err(122); return 0; } a2w(d, b, *n); *n = l + 1; return 1; }
static BOOL WINAPI GetUserProfileDirectoryA(HANDLE t, char *b, DWORD *n) { const char *d = USER_HOME; DWORD l = strlen(d); if (!b || *n <= l) { *n = l + 1; set_err(122); return 0; } strcpy(b, d); *n = l + 1; return 1; }
static void *WINAPI InitSecurityInterfaceA(void) { return NULL; }
static void *WINAPI InitSecurityInterfaceW(void) { return NULL; }
static LONG WINAPI LresultFromObject(const void *iid, uintptr_t w, void *o) { return E_NOTIMPL; }
static LONG WINAPI AccessibleObjectFromWindow(HWND h, DWORD id, const void *iid, void **o) { *o = NULL; return E_NOTIMPL; }
static ULONG WINAPI if_nametoindex(const char *n) { return 0; }
static DWORD WINAPI GetAdaptersInfo(void *a, ULONG *n) { if (n) *n = 0; return 232; }   /* ERROR_NO_DATA */
static DWORD WINAPI GetAdaptersAddresses(ULONG f, ULONG fl, void *r, void *a, ULONG *n) { if (n) *n = 0; return 232; }
static DWORD WINAPI GetBestInterface(DWORD a, DWORD *i) { *i = 1; return 0; }
static DWORD WINAPI GetNetworkParams(void *p, ULONG *n) { if (n) *n = 0; return 232; }
/* fibers: a program asking for them gets a refusal it can report */
static void *WINAPI ConvertThreadToFiber(void *p) { static uintptr_t fiber[4]; fiber[0] = (uintptr_t)p; return fiber; }
static void *WINAPI ConvertThreadToFiberEx(void *p, DWORD f) { return ConvertThreadToFiber(p); }
static BOOL WINAPI ConvertFiberToThread(void) { return 1; }
static void *WINAPI CreateFiber(SIZE_T s, void *fn, void *p) { set_err(50); return NULL; }
static void *WINAPI CreateFiberEx(SIZE_T a, SIZE_T b, DWORD f, void *fn, void *p) { set_err(50); return NULL; }
static void WINAPI DeleteFiber(void *f) {}
static void WINAPI SwitchToFiber(void *f) {}

#define M(g, n) { g, #n, (void *)n }
#define MA(g, n, f) { g, n, (void *)f }
const w32_api_t misc_api[] = {
    M("ole32", CLSIDFromString), M("ole32", IIDFromString), M("ole32", StringFromCLSID), MA("ole32", "StringFromIID", StringFromCLSID), M("ole32", StringFromGUID2),
    M("ole32", CoCreateGuid), M("ole32", IsEqualGUID), M("ole32", CoDisconnectObject), M("ole32", CreateDataAdviseHolder), M("ole32", CreateStreamOnHGlobal),
    M("ole32", GetHGlobalFromStream), M("ole32", DoDragDrop), M("ole32", RegisterDragDrop), M("ole32", RevokeDragDrop), M("ole32", OleGetClipboard), M("ole32", OleSetClipboard),
    M("ole32", OleFlushClipboard), M("ole32", OleIsCurrentClipboard), M("ole32", PropVariantClear), M("ole32", ReleaseStgMedium), M("ole32", CoRegisterClassObject),
    M("ole32", CoRevokeClassObject), M("ole32", CoInitializeSecurity), M("ole32", CoGetClassObject), M("ole32", CoLockObjectExternal), M("ole32", CoSetProxyBlanket),
    MA("ole32", "OleInitialize", OleInitialize2), M("ole32", CoFreeUnusedLibraries), M("ole32", ProgIDFromCLSID), M("ole32", CLSIDFromProgID),
    M("oleaut32", SafeArrayCreate), M("oleaut32", SafeArrayCreateVector), M("oleaut32", SafeArrayDestroy), M("oleaut32", SafeArrayAccessData), M("oleaut32", SafeArrayUnaccessData),
    M("oleaut32", SafeArrayLock), M("oleaut32", SafeArrayUnlock), M("oleaut32", SafeArrayGetDim), M("oleaut32", SafeArrayGetElemsize), M("oleaut32", SafeArrayGetLBound),
    M("oleaut32", SafeArrayGetUBound), M("oleaut32", SafeArrayPtrOfIndex), M("oleaut32", SafeArrayGetElement), M("oleaut32", SafeArrayPutElement), M("oleaut32", SafeArrayCopy),
    M("oleaut32", SysReAllocStringLen), M("oleaut32", SysReAllocString), M("oleaut32", VariantCopyInd), M("oleaut32", VariantChangeType), M("oleaut32", VariantChangeTypeEx),
    M("oleaut32", GetErrorInfo), M("oleaut32", SetErrorInfo), M("oleaut32", CreateErrorInfo), M("oleaut32", LoadTypeLib), M("oleaut32", LoadRegTypeLib), M("oleaut32", RegisterTypeLib),
    M("oleaut32", VarBstrCmp), M("oleaut32", SystemTimeToVariantTime),
    MA("oleaut32", "Ordinal3", SysReAllocString), MA("oleaut32", "Ordinal5", SysReAllocStringLen), MA("oleaut32", "Ordinal11", VariantCopyInd), MA("oleaut32", "Ordinal12", VariantChangeType),
    MA("oleaut32", "Ordinal15", SafeArrayCreate), MA("oleaut32", "Ordinal16", SafeArrayDestroy), MA("oleaut32", "Ordinal17", SafeArrayGetDim), MA("oleaut32", "Ordinal18", SafeArrayGetElemsize),
    MA("oleaut32", "Ordinal19", SafeArrayGetUBound), MA("oleaut32", "Ordinal20", SafeArrayGetLBound), MA("oleaut32", "Ordinal21", SafeArrayLock), MA("oleaut32", "Ordinal22", SafeArrayUnlock),
    MA("oleaut32", "Ordinal23", SafeArrayAccessData), MA("oleaut32", "Ordinal24", SafeArrayUnaccessData), MA("oleaut32", "Ordinal25", SafeArrayGetElement),
    MA("oleaut32", "Ordinal26", SafeArrayPutElement), MA("oleaut32", "Ordinal27", SafeArrayCopy), MA("oleaut32", "Ordinal148", SafeArrayPtrOfIndex), MA("oleaut32", "Ordinal200", GetErrorInfo),
    MA("oleaut32", "Ordinal201", SetErrorInfo), MA("oleaut32", "Ordinal147", VariantChangeTypeEx), MA("oleaut32", "Ordinal411", SafeArrayCreateVector), MA("oleaut32", "Ordinal161", LoadTypeLib),
    M("advapi32", AccessCheck), M("advapi32", ImpersonateSelf), M("advapi32", RevertToSelf), M("advapi32", GetFileSecurityA), M("advapi32", SetFileSecurityA),
    M("advapi32", GetNamedSecurityInfoW), M("advapi32", SetNamedSecurityInfoW), M("advapi32", GetSecurityDescriptorOwner), M("advapi32", GetSecurityDescriptorDacl),
    M("advapi32", GetSidIdentifierAuthority), M("advapi32", GetSidSubAuthority), M("advapi32", GetSidSubAuthorityCount), M("advapi32", ConvertSidToStringSidW),
    M("advapi32", ConvertSidToStringSidA), M("advapi32", LookupAccountSidW), M("advapi32", IsTextUnicode), M("advapi32", CryptAcquireContextW), M("advapi32", RegisterEventSourceW),
    M("advapi32", RegisterEventSourceA), M("advapi32", DeregisterEventSource), M("advapi32", ReportEventW), M("advapi32", ReportEventA), M("advapi32", EventRegister),
    M("advapi32", EventUnregister), M("advapi32", EventWrite), M("advapi32", EventEnabled), M("advapi32", OpenSCManagerW), M("advapi32", OpenSCManagerA),
    M("advapi32", CloseServiceHandle), M("advapi32", SystemFunction040), M("advapi32", SystemFunction041),
    M("bcrypt", BCryptGenRandom), M("bcrypt", BCryptOpenAlgorithmProvider), M("bcrypt", BCryptCloseAlgorithmProvider), M("bcryptprimitives", ProcessPrng),
    M("crypt32", CertOpenSystemStoreA), M("crypt32", CertOpenSystemStoreW), M("crypt32", CertOpenStore), M("crypt32", CertCloseStore), M("crypt32", CertEnumCertificatesInStore),
    M("crypt32", CertFreeCertificateContext), M("crypt32", CertGetEnhancedKeyUsage), M("crypt32", CertGetIntendedKeyUsage), M("crypt32", CertGetNameStringA),
    M("crypt32", CertGetNameStringW), M("crypt32", CryptStringToBinaryA), M("crypt32", CryptProtectData), M("crypt32", CryptUnprotectData),
    M("setupapi", SetupDiGetClassDevsA), M("setupapi", SetupDiGetClassDevsW), M("setupapi", SetupDiDestroyDeviceInfoList), M("setupapi", SetupDiEnumDeviceInfo),
    M("setupapi", SetupDiEnumDeviceInterfaces), M("setupapi", SetupDiGetDeviceInterfaceDetailA), M("setupapi", SetupDiGetDeviceInterfaceDetailW),
    M("setupapi", SetupDiGetDeviceRegistryPropertyA), M("setupapi", SetupDiGetDeviceRegistryPropertyW), M("setupapi", CM_Locate_DevNodeA), M("setupapi", CM_Locate_DevNodeW),
    M("setupapi", CM_Get_Parent), M("setupapi", CM_Get_Device_IDA), M("setupapi", CM_Get_Device_IDW), M("cfgmgr32", CM_Locate_DevNodeW), M("cfgmgr32", CM_Get_Parent),
    M("cfgmgr32", CM_Get_Device_IDW),
    M("winspool", OpenPrinterA), M("winspool", OpenPrinterW), M("winspool", ClosePrinter), M("winspool", DocumentPropertiesA), M("winspool", DocumentPropertiesW),
    M("winspool", EnumPrintersW), M("winspool", EnumPrintersA), M("winspool", GetDefaultPrinterW), M("winspool", GetDefaultPrinterA),
    M("wldap32", ldap_init), M("wldap32", ldap_sslinit), M("wldap32", ldap_bind_s), M("wldap32", ldap_simple_bind_s), M("wldap32", ldap_unbind_s), M("wldap32", ldap_search_s),
    M("wldap32", ldap_set_option), M("wldap32", ldap_err2string), MA("wldap32", "ldap_first_entry", ldap_null), MA("wldap32", "ldap_next_entry", ldap_null),
    MA("wldap32", "ldap_first_attribute", ldap_null), MA("wldap32", "ldap_next_attribute", ldap_null), MA("wldap32", "ldap_get_dn", ldap_null),
    MA("wldap32", "ldap_get_values_len", ldap_null), MA("wldap32", "ldap_value_free_len", ldap_zero), MA("wldap32", "ldap_memfree", ldap_zero),
    MA("wldap32", "ldap_msgfree", ldap_zero), MA("wldap32", "ber_free", ldap_zero), MA("wldap32", "LdapGetLastError", ldap_err_ul),
    M("netapi32", NetApiBufferFree), M("netapi32", NetGetDCName), M("netapi32", NetUserGetInfo), M("netapi32", NetWkstaGetInfo),
    M("normaliz", IdnToAscii), M("normaliz", IdnToUnicode), M("kernel32", IdnToAscii), M("kernel32", IdnToUnicode),
    M("imm32", ImmGetCandidateListW), M("imm32", ImmGetIMEFileNameA), M("imm32", ImmNotifyIME), M("imm32", ImmSetCandidateWindow), M("imm32", ImmSetCompositionStringW),
    M("imm32", ImmIsIME), M("imm32", ImmGetDefaultIMEWnd), M("imm32", ImmDisableIME),
    M("wtsapi32", WTSRegisterSessionNotification), M("wtsapi32", WTSUnRegisterSessionNotification), M("wtsapi32", WTSQuerySessionInformationW), M("wtsapi32", WTSFreeMemory),
    M("userenv", GetProfilesDirectoryW), M("userenv", GetUserProfileDirectoryW), M("userenv", GetUserProfileDirectoryA), M("secur32", InitSecurityInterfaceA),
    M("secur32", InitSecurityInterfaceW), M("oleacc", LresultFromObject), M("oleacc", AccessibleObjectFromWindow), M("iphlpapi", if_nametoindex), M("iphlpapi", GetAdaptersInfo),
    M("iphlpapi", GetAdaptersAddresses), M("iphlpapi", GetBestInterface), M("iphlpapi", GetNetworkParams),
    M("kernel32", ConvertThreadToFiber), M("kernel32", ConvertThreadToFiberEx), M("kernel32", ConvertFiberToThread), M("kernel32", CreateFiber), M("kernel32", CreateFiberEx),
    M("kernel32", DeleteFiber), M("kernel32", SwitchToFiber),
    { NULL, NULL, NULL }
};
