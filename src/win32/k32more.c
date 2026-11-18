/* More of kernel32: the process environment, version checks, INI files, atoms, Tool Help
 * snapshots, SearchPath, console input records, file information classes, the wide
 * lstr* functions and the serial-port and debugging calls programs probe for. Entries are
 * looked up before the older tables (w32_extra_find), so these replace earlier stubs. */
#include "w32.h"

#define P w32_cur()
typedef uint32_t ULONG;
static inline void set_err(uint32_t e) { w32_proc_t *p = P; if (p) p->last_error = e; }
static void *K(const char *name) { void *fn = NULL; w32_lookup("kernel32.dll", name, &fn); return fn; }
static void w2a(const WCHAR *w, char *a, int cap) { int i = 0; if (w) for (; w[i] && i < cap - 1; i++) a[i] = w[i] < 256 ? (char)w[i] : '?'; a[i] = 0; }
static int a2w(const char *a, WCHAR *w, int cap) { int i = 0; for (; a[i] && i < cap - 1; i++) w[i] = (uint8_t)a[i]; if (cap > 0) w[i] = 0; return i; }
static int wlen(const WCHAR *w) { int n = 0; while (w && w[n]) n++; return n; }

/* ---------------------------------------------------------------- environment */
static const char *env_get(w32_proc_t *p, const char *n) {
    size_t l = strlen(n);
    for (int i = 0; p->envp && p->envp[i]; i++) if (!strncasecmp(p->envp[i], n, l) && p->envp[i][l] == '=') return p->envp[i] + l + 1;
    return NULL;
}
static void env_set(w32_proc_t *p, const char *name, const char *val) {
    int n = 0; while (p->envp && p->envp[n]) n++;
    size_t nl = strlen(name);
    for (int i = 0; i < n; i++) if (!strncasecmp(p->envp[i], name, nl) && p->envp[i][nl] == '=') {
        if (!val) { for (int k = i; k < n; k++) p->envp[k] = p->envp[k + 1]; }
        else { char *e = kmalloc(nl + strlen(val) + 2); sprintf(e, "%s=%s", name, val); p->envp[i] = e; }
        p->wenvp = NULL; return;
    }
    if (!val) return;
    char **ne = kzalloc((n + 2) * sizeof(char *)); if (n) memcpy(ne, p->envp, n * sizeof(char *));
    char *e = kmalloc(nl + strlen(val) + 2); sprintf(e, "%s=%s", name, val); ne[n] = e;
    kfree(p->envp); p->envp = ne; p->wenvp = NULL;
}
static DWORD WINAPI GetEnvironmentVariableA(const char *n, char *b, DWORD sz) {
    const char *v = n ? env_get(P, n) : NULL;
    if (!v) { set_err(203); return 0; }
    DWORD l = strlen(v);
    if (!b || sz <= l) return l + 1;
    memcpy(b, v, l + 1); return l;
}
static DWORD WINAPI GetEnvironmentVariableW(const WCHAR *n, WCHAR *b, DWORD sz) {
    char a[256]; w2a(n, a, sizeof a);
    const char *v = env_get(P, a);
    if (!v) { set_err(203); return 0; }
    DWORD l = strlen(v);
    if (!b || sz <= l) return l + 1;
    a2w(v, b, sz); return l;
}
static BOOL WINAPI SetEnvironmentVariableA(const char *n, const char *v) { if (!n || !*n || strchr(n, '=')) { set_err(87); return 0; } env_set(P, n, v); return 1; }
static BOOL WINAPI SetEnvironmentVariableW(const WCHAR *n, const WCHAR *v) { char a[256], b[2048]; w2a(n, a, sizeof a); if (v) w2a(v, b, sizeof b); return SetEnvironmentVariableA(a, v ? b : NULL); }
static char *WINAPI GetEnvironmentStringsA(void) {
    w32_proc_t *p = P; size_t total = 2;
    for (int i = 0; p->envp && p->envp[i]; i++) total += strlen(p->envp[i]) + 1;
    char *b = w32_alloc(p, total), *o = b;
    for (int i = 0; p->envp && p->envp[i]; i++) { size_t l = strlen(p->envp[i]) + 1; memcpy(o, p->envp[i], l); o += l; }
    *o++ = 0; *o = 0;
    return b;
}
static WCHAR *WINAPI GetEnvironmentStringsW(void) {
    w32_proc_t *p = P; size_t total = 2;
    for (int i = 0; p->envp && p->envp[i]; i++) total += strlen(p->envp[i]) + 1;
    WCHAR *b = w32_alloc(p, total * 2), *o = b;
    for (int i = 0; p->envp && p->envp[i]; i++) { for (const char *c = p->envp[i]; *c; c++) *o++ = (uint8_t)*c; *o++ = 0; }
    *o++ = 0; *o = 0;
    return b;
}
static BOOL WINAPI FreeEnvironmentStringsA(char *e) { w32_free(P, e); return 1; }
static BOOL WINAPI FreeEnvironmentStringsW(WCHAR *e) { w32_free(P, e); return 1; }
static BOOL WINAPI SetEnvironmentStringsW(WCHAR *e) { return 1; }
static DWORD expand(const char *s, char *d, DWORD n) {
    char out[4096]; DWORD k = 0;
    while (*s && k < sizeof out - 1) {
        if (*s == '%') {
            const char *e = strchr(s + 1, '%');
            if (e && e > s + 1) {
                char name[128]; int l = MIN((int)(e - s - 1), 127); memcpy(name, s + 1, l); name[l] = 0;
                const char *v = env_get(P, name);
                if (v) { while (*v && k < sizeof out - 1) out[k++] = *v++; s = e + 1; continue; }
            }
        }
        out[k++] = *s++;
    }
    out[k] = 0;
    if (d && n > k) memcpy(d, out, k + 1);
    return k + 1;
}
static DWORD WINAPI ExpandEnvironmentStringsA(const char *s, char *d, DWORD n) { return expand(s, d, n); }
static DWORD WINAPI ExpandEnvironmentStringsW(const WCHAR *s, WCHAR *d, DWORD n) {
    char a[4096], o[4096]; w2a(s, a, sizeof a);
    DWORD r = expand(a, o, sizeof o);
    if (d && n >= r) a2w(o, d, n);
    return r;
}

/* ---------------------------------------------------------------- version: Windows 7 SP1 */
static DWORD WINAPI GetVersion(void) { return 0x1DB10106; }                         /* 6.1 build 7601 */
typedef struct { DWORD size, major, minor, build, platform; char csd[128]; WORD spmaj, spmin, suite; BYTE type, res; } OSVIA;
typedef struct { DWORD size, major, minor, build, platform; WCHAR csd[128]; WORD spmaj, spmin, suite; BYTE type, res; } OSVIW;
static BOOL WINAPI GetVersionExA(OSVIA *v) {
    DWORD sz = v->size; if (sz < 148) { set_err(122); return 0; }
    memset((uint8_t *)v + 4, 0, sz - 4);
    v->major = 6; v->minor = 1; v->build = 7601; v->platform = 2; strcpy(v->csd, "Service Pack 1");
    if (sz >= sizeof(OSVIA)) { v->spmaj = 1; v->suite = 0x100; v->type = 1; }
    return 1;
}
static BOOL WINAPI GetVersionExW(OSVIW *v) {
    DWORD sz = v->size; if (sz < 276) { set_err(122); return 0; }
    memset((uint8_t *)v + 4, 0, sz - 4);
    v->major = 6; v->minor = 1; v->build = 7601; v->platform = 2; a2w("Service Pack 1", v->csd, 128);
    if (sz >= sizeof(OSVIW)) { v->spmaj = 1; v->suite = 0x100; v->type = 1; }
    return 1;
}
static LONG WINAPI RtlGetVersion(OSVIW *v) { GetVersionExW(v); return 0; }
static uint64_t WINAPI VerSetConditionMask(uint64_t mask, DWORD type, BYTE cond) {
    for (int b = 0; b < 8; b++) if (type & (1u << b)) mask |= (uint64_t)(cond & 7) << (b * 3);
    return mask;
}
static int cmp_cond(int64_t have, int64_t want, int cond) {
    switch (cond) { case 1: return have == want; case 2: return have > want; case 3: return have >= want; case 4: return have < want; case 5: return have <= want; }
    return 1;
}
static BOOL verify(DWORD major, DWORD minor, DWORD build, DWORD platform, WORD spmaj, WORD spmin, DWORD type, uint64_t mask) {
    const DWORD M = 6, m = 1, B = 7601; const WORD SM = 1, Sm = 0;
    bool ok = true;
    if (type & 2) {                                     /* major, minor and service pack compared as one version */
        int c = (mask >> 3) & 7;
        int64_t have = ((int64_t)M << 32) | (m << 16), want = ((int64_t)major << 32) | ((type & 1 ? minor : 0) << 16);
        if (type & 0x20) { have |= SM << 8; want |= spmaj << 8; }
        if (type & 0x10) { have |= Sm; want |= spmin; }
        if (!(type & 1)) have &= ~((int64_t)0xFFFF << 16), want &= ~((int64_t)0xFFFF << 16);
        ok = cmp_cond(have, want, c);
    } else {
        if (type & 1) ok = ok && cmp_cond(m, minor, mask & 7);
        if (type & 0x20) ok = ok && cmp_cond(SM, spmaj, (mask >> 15) & 7);
        if (type & 0x10) ok = ok && cmp_cond(Sm, spmin, (mask >> 12) & 7);
    }
    if (type & 4) ok = ok && cmp_cond(B, build, (mask >> 6) & 7);
    if (type & 8) ok = ok && platform == 2;
    if (!ok) set_err(1150);                            /* ERROR_OLD_WIN_VERSION */
    return ok;
}
static BOOL WINAPI VerifyVersionInfoW(OSVIW *v, DWORD type, uint64_t mask) { return verify(v->major, v->minor, v->build, v->platform, v->spmaj, v->spmin, type, mask); }
static BOOL WINAPI VerifyVersionInfoA(OSVIA *v, DWORD type, uint64_t mask) { return verify(v->major, v->minor, v->build, v->platform, v->spmaj, v->spmin, type, mask); }
static BOOL WINAPI GetProductInfo(DWORD a, DWORD b, DWORD c, DWORD d, DWORD *t) { *t = 1; return 1; }   /* PRODUCT_ULTIMATE */

/* ---------------------------------------------------------------- INI files */
/* a file name without a folder is in C:\WRLD, as Windows keeps them in the Windows folder */
static vnode_t *ini_node(const char *file, char *path, int cap, bool create) {
    if (!file || !*file) file = "win.ini";
    if (!strchr(file, '\\') && !strchr(file, '/') && !strchr(file, ':')) snprintf(path, cap, "C:\\WRLD\\%s", file);
    else if (isalpha(file[0]) && file[1] == ':') strlcpy(path, file, cap);
    else path_join(path, cap, P->cwd, file);
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
    path_normalize(path);
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n && create) n = vfs_write_file(path, "", 0);
    return n && !n->dir ? n : NULL;
}
/* the text of the file as lines; callbacks see each [section] and key=value */
typedef struct { const char *sec_s; int sec_l; const char *key_s; int key_l; const char *val_s; int val_l; const char *line_s; int line_l; } ini_line_t;
static int ini_lines(const char *d, uint32_t n, ini_line_t *out, int max) {
    int k = 0; const char *sec = NULL; int secl = 0;
    for (uint32_t i = 0; i < n && k < max; ) {
        uint32_t e = i; while (e < n && d[e] != '\n') e++;
        const char *s = d + i; int l = e - i; if (l && s[l - 1] == '\r') l--;
        const char *t = s; int tl = l; while (tl && (*t == ' ' || *t == '\t')) { t++; tl--; }
        ini_line_t L = { sec, secl, NULL, 0, NULL, 0, s, l };
        if (tl && t[0] == '[') {
            const char *c = memchr(t, ']', tl);
            sec = t + 1; secl = c ? (int)(c - t - 1) : tl - 1;
            L.sec_s = sec; L.sec_l = secl; L.key_s = NULL;
        } else if (tl && t[0] != ';' && t[0] != '#') {
            const char *eq = memchr(t, '=', tl);
            if (eq) {
                int kl = eq - t; while (kl && (t[kl - 1] == ' ' || t[kl - 1] == '\t')) kl--;
                const char *v = eq + 1; int vl = tl - (eq + 1 - t); while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
                while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) { v++; vl -= 2; }
                L.key_s = t; L.key_l = kl; L.val_s = v; L.val_l = vl;
            }
        }
        out[k++] = L;
        i = e + 1;
    }
    return k;
}
static bool name_eq(const char *s, int l, const char *n) { return n && (int)strlen(n) == l && !strncasecmp(s, n, l); }
#define INI_MAX 4096
static DWORD ini_get(const char *sec, const char *key, const char *def, char *buf, DWORD size, const char *file) {
    if (!buf || !size) return 0;
    char path[300]; vnode_t *n = ini_node(file, path, sizeof path, false);
    ini_line_t *L = kmalloc(sizeof(ini_line_t) * INI_MAX);
    int nl = n ? ini_lines((const char *)n->data, n->size, L, INI_MAX) : 0;
    DWORD o = 0;
    if (!sec) {                                         /* all section names, each terminated, then another NUL */
        for (int i = 0; i < nl; i++) if (L[i].sec_s && !L[i].key_s && (i == 0 || L[i].sec_s != L[i - 1].sec_s)) {
            if (o + L[i].sec_l + 2 > size) break;
            memcpy(buf + o, L[i].sec_s, L[i].sec_l); o += L[i].sec_l; buf[o++] = 0;
        }
        buf[o < size ? o : size - 1] = 0; kfree(L);
        return o ? o - 0 : 0;
    }
    if (!key) {                                         /* all keys of the section */
        for (int i = 0; i < nl; i++) if (L[i].key_s && name_eq(L[i].sec_s, L[i].sec_l, sec)) {
            if (o + L[i].key_l + 2 > size) break;
            memcpy(buf + o, L[i].key_s, L[i].key_l); o += L[i].key_l; buf[o++] = 0;
        }
        buf[o < size ? o : size - 1] = 0; kfree(L);
        return o;
    }
    for (int i = 0; i < nl; i++) if (L[i].key_s && name_eq(L[i].sec_s, L[i].sec_l, sec) && name_eq(L[i].key_s, L[i].key_l, key)) {
        DWORD l = MIN((DWORD)L[i].val_l, size - 1); memcpy(buf, L[i].val_s, l); buf[l] = 0; kfree(L);
        return l;
    }
    kfree(L);
    if (!def) def = "";
    DWORD l = strlen(def); while (l && def[l - 1] == ' ') l--;
    l = MIN(l, size - 1); memcpy(buf, def, l); buf[l] = 0;
    return l;
}
static BOOL ini_put(const char *sec, const char *key, const char *val, const char *file) {
    char path[300]; vnode_t *n = ini_node(file, path, sizeof path, true);
    if (!n) { set_err(3); return 0; }
    if (!sec) return 1;                                  /* flush the cache: nothing cached */
    ini_line_t *L = kmalloc(sizeof(ini_line_t) * INI_MAX);
    int nl = ini_lines((const char *)n->data, n->size, L, INI_MAX);
    char *out = kmalloc(n->size + strlen(sec) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 16); uint32_t o = 0;
    bool done = false; int last_in_sec = -1; bool sec_found = false;
    for (int i = 0; i < nl; i++) if (name_eq(L[i].sec_s, L[i].sec_l, sec)) { sec_found = true; if (L[i].line_l) last_in_sec = i; }
    for (int i = 0; i < nl; i++) {
        bool in_sec = name_eq(L[i].sec_s, L[i].sec_l, sec);
        bool is_header = in_sec && !L[i].key_s && L[i].line_l && memchr(L[i].line_s, '[', L[i].line_l);
        if (in_sec && !key) continue;                    /* delete the whole section */
        if (in_sec && key && L[i].key_s && name_eq(L[i].key_s, L[i].key_l, key)) {
            if (val) { o += sprintf(out + o, "%s=%s\r\n", key, val); }
            done = true; continue;
        }
        (void)is_header;
        memcpy(out + o, L[i].line_s, L[i].line_l); o += L[i].line_l; out[o++] = '\r'; out[o++] = '\n';
        if (i == last_in_sec && key && val && !done) { o += sprintf(out + o, "%s=%s\r\n", key, val); done = true; }
    }
    if (!sec_found && key && val) { o += sprintf(out + o, "[%s]\r\n%s=%s\r\n", sec, key, val); }
    vfs_write_file(path, out, o);
    kfree(out); kfree(L);
    return 1;
}
static DWORD WINAPI GetPrivateProfileStringA(const char *s, const char *k, const char *d, char *b, DWORD n, const char *f) { return ini_get(s, k, d, b, n, f); }
static DWORD WINAPI GetPrivateProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *d, WCHAR *b, DWORD n, const WCHAR *f) {
    char as[256], ak[256], ad[1024], af[300]; w2a(s, as, sizeof as); w2a(k, ak, sizeof ak); w2a(d, ad, sizeof ad); w2a(f, af, sizeof af);
    if (!b || !n) return 0;
    char *o = kmalloc(n); DWORD r = ini_get(s ? as : NULL, k ? ak : NULL, d ? ad : NULL, o, n, f ? af : NULL);
    for (DWORD i = 0; i <= r && i < n; i++) b[i] = (uint8_t)o[i];
    if (!s || !k) for (DWORD i = 0; i < n; i++) { b[i] = (uint8_t)o[i]; if (!o[i] && i && !o[i - 1]) break; }
    kfree(o); return r;
}
static UINT WINAPI GetPrivateProfileIntA(const char *s, const char *k, int d, const char *f) { char b[64]; if (!ini_get(s, k, "", b, sizeof b, f)) return d; return (UINT)strtol(b, NULL, 0); }
static UINT WINAPI GetPrivateProfileIntW(const WCHAR *s, const WCHAR *k, int d, const WCHAR *f) { char as[256], ak[256], af[300]; w2a(s, as, sizeof as); w2a(k, ak, sizeof ak); w2a(f, af, sizeof af); return GetPrivateProfileIntA(as, ak, d, f ? af : NULL); }
static BOOL WINAPI WritePrivateProfileStringA(const char *s, const char *k, const char *v, const char *f) { return ini_put(s, k, v, f); }
static BOOL WINAPI WritePrivateProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *v, const WCHAR *f) {
    char as[256], ak[256], av[2048], af[300]; w2a(s, as, sizeof as); w2a(k, ak, sizeof ak); w2a(v, av, sizeof av); w2a(f, af, sizeof af);
    return ini_put(s ? as : NULL, k ? ak : NULL, v ? av : NULL, f ? af : NULL);
}
/* a section as "key=value\0...\0\0" */
static DWORD section_get(const char *sec, char *buf, DWORD size, const char *file) {
    char path[300]; vnode_t *n = ini_node(file, path, sizeof path, false);
    if (!buf || size < 2) return 0;
    DWORD o = 0;
    if (n) {
        ini_line_t *L = kmalloc(sizeof(ini_line_t) * INI_MAX);
        int nl = ini_lines((const char *)n->data, n->size, L, INI_MAX);
        for (int i = 0; i < nl; i++) if (L[i].key_s && name_eq(L[i].sec_s, L[i].sec_l, sec)) {
            DWORD need = L[i].key_l + 1 + L[i].val_l + 1;
            if (o + need + 1 > size) break;
            memcpy(buf + o, L[i].key_s, L[i].key_l); o += L[i].key_l; buf[o++] = '=';
            memcpy(buf + o, L[i].val_s, L[i].val_l); o += L[i].val_l; buf[o++] = 0;
        }
        kfree(L);
    }
    buf[o] = 0;
    return o;
}
static DWORD WINAPI GetPrivateProfileSectionA(const char *s, char *b, DWORD n, const char *f) { return section_get(s, b, n, f); }
static DWORD WINAPI GetPrivateProfileSectionW(const WCHAR *s, WCHAR *b, DWORD n, const WCHAR *f) {
    char as[256], af[300]; w2a(s, as, sizeof as); w2a(f, af, sizeof af);
    char *o = kmalloc(n + 1); DWORD r = section_get(as, o, n, f ? af : NULL);
    for (DWORD i = 0; i <= r && i < n; i++) b[i] = (uint8_t)o[i];
    kfree(o); return r;
}
static BOOL section_put(const char *sec, const char *data, const char *file) {
    ini_put(sec, NULL, NULL, file);
    for (const char *p = data; p && *p; p += strlen(p) + 1) {
        const char *eq = strchr(p, '=');
        if (!eq) continue;
        char k[256]; int l = MIN((int)(eq - p), 255); memcpy(k, p, l); k[l] = 0;
        ini_put(sec, k, eq + 1, file);
    }
    return 1;
}
static BOOL WINAPI WritePrivateProfileSectionA(const char *s, const char *d, const char *f) { return section_put(s, d, f); }
static BOOL WINAPI WritePrivateProfileSectionW(const WCHAR *s, const WCHAR *d, const WCHAR *f) {
    char as[256], af[300]; w2a(s, as, sizeof as); w2a(f, af, sizeof af);
    int total = 0; while (d && (d[total] || d[total + 1])) total++;
    char *a = kmalloc(total + 3); for (int i = 0; i < total + 2; i++) a[i] = d && d[i] < 256 ? (char)d[i] : (d && d[i] ? '?' : 0);
    BOOL r = section_put(as, a, f ? af : NULL); kfree(a); return r;
}
static DWORD WINAPI GetPrivateProfileSectionNamesA(char *b, DWORD n, const char *f) { return ini_get(NULL, NULL, NULL, b, n, f); }
static DWORD WINAPI GetPrivateProfileSectionNamesW(WCHAR *b, DWORD n, const WCHAR *f) { return GetPrivateProfileStringW(NULL, NULL, NULL, b, n, f); }
static DWORD WINAPI GetProfileStringA(const char *s, const char *k, const char *d, char *b, DWORD n) { return ini_get(s, k, d, b, n, "win.ini"); }
static DWORD WINAPI GetProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *d, WCHAR *b, DWORD n) { static const WCHAR w[] = { 'w', 'i', 'n', '.', 'i', 'n', 'i', 0 }; return GetPrivateProfileStringW(s, k, d, b, n, w); }
static UINT WINAPI GetProfileIntA(const char *s, const char *k, int d) { return GetPrivateProfileIntA(s, k, d, "win.ini"); }
static UINT WINAPI GetProfileIntW(const WCHAR *s, const WCHAR *k, int d) { static const WCHAR w[] = { 'w', 'i', 'n', '.', 'i', 'n', 'i', 0 }; return GetPrivateProfileIntW(s, k, d, w); }
static BOOL WINAPI WriteProfileStringA(const char *s, const char *k, const char *v) { return ini_put(s, k, v, "win.ini"); }
static BOOL WINAPI WriteProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *v) { static const WCHAR w[] = { 'w', 'i', 'n', '.', 'i', 'n', 'i', 0 }; return WritePrivateProfileStringW(s, k, v, w); }
static BOOL WINAPI GetPrivateProfileStructA(const char *s, const char *k, void *d, UINT n, const char *f) {
    char b[1024]; DWORD l = ini_get(s, k, "", b, sizeof b, f);
    if (l != n * 2 + 2) return 0;
    for (UINT i = 0; i < n; i++) { char h[3] = { b[2 * i], b[2 * i + 1], 0 }; ((uint8_t *)d)[i] = strtoul(h, NULL, 16); }
    return 1;
}
static BOOL WINAPI WritePrivateProfileStructA(const char *s, const char *k, const void *d, UINT n, const char *f) {
    char *b = kmalloc(n * 2 + 3); uint8_t sum = 0;
    for (UINT i = 0; i < n; i++) { sprintf(b + 2 * i, "%02X", ((const uint8_t *)d)[i]); sum += ((const uint8_t *)d)[i]; }
    sprintf(b + 2 * n, "%02X", sum);
    BOOL r = ini_put(s, k, b, f); kfree(b); return r;
}

/* ---------------------------------------------------------------- atoms */
#define NATOM 512
static char *atoms[NATOM]; static int atom_refs[NATOM];
static WORD atom_find(const char *n) {
    if (n && n[0] == '#') return (WORD)atoi(n + 1);
    for (int i = 0; i < NATOM; i++) if (atoms[i] && !strcasecmp(atoms[i], n)) return 0xC000 + i;
    return 0;
}
static WORD atom_add(const char *n) {
    if ((uintptr_t)n < 0x10000) return (WORD)(uintptr_t)n;
    WORD a = atom_find(n);
    if (a >= 0xC000) { atom_refs[a - 0xC000]++; return a; }
    if (a) return a;
    for (int i = 0; i < NATOM; i++) if (!atoms[i]) { atoms[i] = strdup(n); atom_refs[i] = 1; return 0xC000 + i; }
    return 0;
}
static WORD WINAPI GlobalAddAtomA(const char *n) { return atom_add(n); }
static WORD WINAPI GlobalAddAtomW(const WCHAR *n) { if ((uintptr_t)n < 0x10000) return (WORD)(uintptr_t)n; char a[256]; w2a(n, a, sizeof a); return atom_add(a); }
static WORD WINAPI GlobalFindAtomA(const char *n) { return (uintptr_t)n < 0x10000 ? (WORD)(uintptr_t)n : atom_find(n); }
static WORD WINAPI GlobalFindAtomW(const WCHAR *n) { if ((uintptr_t)n < 0x10000) return (WORD)(uintptr_t)n; char a[256]; w2a(n, a, sizeof a); return atom_find(a); }
static WORD WINAPI GlobalDeleteAtom(WORD a) {
    if (a >= 0xC000 && a < 0xC000 + NATOM && atoms[a - 0xC000] && --atom_refs[a - 0xC000] <= 0) { kfree(atoms[a - 0xC000]); atoms[a - 0xC000] = NULL; }
    return 0;
}
static UINT WINAPI GlobalGetAtomNameA(WORD a, char *b, int n) {
    if (a < 0xC000) { snprintf(b, n, "#%u", a); return strlen(b); }
    if (a >= 0xC000 + NATOM || !atoms[a - 0xC000]) { set_err(6); return 0; }
    strlcpy(b, atoms[a - 0xC000], n); return strlen(b);
}
static UINT WINAPI GlobalGetAtomNameW(WORD a, WCHAR *b, int n) { char t[256]; UINT l = GlobalGetAtomNameA(a, t, sizeof t); if (!l) return 0; return a2w(t, b, n); }

/* ---------------------------------------------------------------- memory blocks */
static SIZE_T WINAPI GlobalSize(void *p) { return w32_msize(p); }
static void *WINAPI GlobalReAlloc(void *p, SIZE_T n, UINT f) {
    if (f & 0x80) return p;                              /* GMEM_MODIFY */
    SIZE_T old = w32_msize(p);
    void *q = w32_realloc(P, p, n ? n : 1);
    if (q && (f & 0x40) && n > old) memset((uint8_t *)q + old, 0, n - old);
    return q;
}
static UINT WINAPI GlobalFlags(void *p) { return 0; }
static void *WINAPI GlobalHandle(const void *p) { return (void *)p; }
static void *WINAPI LocalHandle(const void *p) { return (void *)p; }
static UINT WINAPI LocalFlags(void *p) { return 0; }
static void *WINAPI LocalLock(void *p) { return p; }
static BOOL WINAPI LocalUnlock(void *p) { return 1; }
static SIZE_T WINAPI GlobalCompact(DWORD n) { return 64 * 1024 * 1024; }
static void WINAPI GlobalFix(void *p) {}
static void WINAPI GlobalUnfix(void *p) {}

/* ---------------------------------------------------------------- Tool Help snapshots */
typedef struct { DWORD size, usage, pid; uintptr_t heap; DWORD module, threads, ppid; LONG pri; DWORD flags; char exe[260]; } PE32A;
typedef struct { DWORD size, usage, pid; uintptr_t heap; DWORD module, threads, ppid; LONG pri; DWORD flags; WCHAR exe[260]; } PE32W;
typedef struct { DWORD size, mid, pid, gusage, pusage; uint8_t *base; DWORD bsize; uintptr_t hmod; char name[256], path[260]; } ME32A;
typedef struct { DWORD size, mid, pid, gusage, pusage; uint8_t *base; DWORD bsize; uintptr_t hmod; WCHAR name[256], path[260]; } ME32W;
typedef struct { DWORD size, usage, tid, owner; LONG base, delta; DWORD flags; } TE32;
#define SNAP_MAGIC 0x534E4150u
typedef struct { uint32_t magic; int npid; int pids[64]; int pos, mpos, tpos; } snap_t;
static HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid) {
    snap_t *s = w32_alloc(P, sizeof *s); memset(s, 0, sizeof *s);
    s->magic = SNAP_MAGIC;
    s->npid = pe_proc_list(s->pids, 64);
    return (HANDLE)s;
}
static snap_t *snap(HANDLE h) { snap_t *s = (snap_t *)h; return s && (uintptr_t)s > 0x10000 && w32_msize(s) >= sizeof *s && s->magic == SNAP_MAGIC ? s : NULL; }
static bool proc_entry(snap_t *s, int i, DWORD *pid, char *exe, int cap) {
    if (i >= s->npid) return false;
    w32_proc_t *p = pe_find(s->pids[i]);
    *pid = s->pids[i];
    strlcpy(exe, p ? path_basename(p->path) : "?", cap);
    return true;
}
static BOOL WINAPI Process32Next(HANDLE h, PE32A *e);
static BOOL WINAPI Process32First(HANDLE h, PE32A *e) { snap_t *s = snap(h); if (!s) { set_err(6); return 0; } s->pos = 0; return Process32Next(h, e); }
static BOOL WINAPI Process32Next(HANDLE h, PE32A *e) {
    snap_t *s = snap(h); if (!s) { set_err(6); return 0; }
    DWORD pid; char exe[260];
    if (!proc_entry(s, s->pos++, &pid, exe, sizeof exe)) { set_err(18); return 0; }
    DWORD sz = e->size; memset(e, 0, sizeof *e); e->size = sz; e->pid = pid; e->threads = 1; e->pri = 8; strlcpy(e->exe, exe, 260);
    return 1;
}
static BOOL WINAPI Process32NextW(HANDLE h, PE32W *e) {
    snap_t *s = snap(h); if (!s) { set_err(6); return 0; }
    DWORD pid; char exe[260];
    if (!proc_entry(s, s->pos++, &pid, exe, sizeof exe)) { set_err(18); return 0; }
    DWORD sz = e->size; memset(e, 0, sizeof *e); e->size = sz; e->pid = pid; e->threads = 1; e->pri = 8; a2w(exe, e->exe, 260);
    return 1;
}
static BOOL WINAPI Process32FirstW(HANDLE h, PE32W *e) { snap_t *s = snap(h); if (!s) { set_err(6); return 0; } s->pos = 0; return Process32NextW(h, e); }
static BOOL WINAPI Module32NextW(HANDLE h, ME32W *e) {
    snap_t *s = snap(h); w32_proc_t *p = P; if (!s) { set_err(6); return 0; }
    if (s->mpos >= p->nmods) { set_err(18); return 0; }
    w32_mod_t *m = p->mods[s->mpos++];
    DWORD sz = e->size; memset(e, 0, sizeof *e); e->size = sz; e->mid = 1; e->pid = p->pid; e->gusage = e->pusage = 1;
    e->base = (uint8_t *)m->base; e->bsize = m->size; e->hmod = m->base; a2w(m->name, e->name, 256); a2w(m->path, e->path, 260);
    return 1;
}
static BOOL WINAPI Module32FirstW(HANDLE h, ME32W *e) { snap_t *s = snap(h); if (!s) { set_err(6); return 0; } s->mpos = 0; return Module32NextW(h, e); }
static BOOL WINAPI Module32Next(HANDLE h, ME32A *e) {
    snap_t *s = snap(h); w32_proc_t *p = P; if (!s) { set_err(6); return 0; }
    if (s->mpos >= p->nmods) { set_err(18); return 0; }
    w32_mod_t *m = p->mods[s->mpos++];
    DWORD sz = e->size; memset(e, 0, sizeof *e); e->size = sz; e->mid = 1; e->pid = p->pid; e->gusage = e->pusage = 1;
    e->base = (uint8_t *)m->base; e->bsize = m->size; e->hmod = m->base; strlcpy(e->name, m->name, 256); strlcpy(e->path, m->path, 260);
    return 1;
}
static BOOL WINAPI Module32First(HANDLE h, ME32A *e) { snap_t *s = snap(h); if (!s) { set_err(6); return 0; } s->mpos = 0; return Module32Next(h, e); }
static BOOL WINAPI Thread32Next(HANDLE h, TE32 *e) {
    snap_t *s = snap(h); if (!s) { set_err(6); return 0; }
    if (s->tpos >= s->npid) { set_err(18); return 0; }
    int pid = s->pids[s->tpos++];
    DWORD sz = e->size; memset(e, 0, sizeof *e); e->size = sz; e->tid = pid; e->owner = pid; e->base = 8;
    return 1;
}
static BOOL WINAPI Thread32First(HANDLE h, TE32 *e) { snap_t *s = snap(h); if (!s) { set_err(6); return 0; } s->tpos = 0; return Thread32Next(h, e); }

/* ---------------------------------------------------------------- SearchPath */
static DWORD search(const char *path, const char *file, const char *ext, char *out, DWORD n, char **part) {
    char name[300]; strlcpy(name, file, sizeof name);
    if (ext && !strchr(path_basename(name), '.')) strlcat(name, ext, sizeof name);
    char cand[600];
    if ((isalpha(name[0]) && name[1] == ':') || name[0] == '\\' || name[0] == '/') {
        if (name[0] == '\\' || name[0] == '/') snprintf(cand, sizeof cand, "C:%s", name); else strlcpy(cand, name, sizeof cand);
        path_normalize(cand);
        if (vfs_resolve(cand, NULL)) goto found;
        set_err(2); return 0;
    }
    w32_proc_t *p = P;
    char dirs[2048] = "";
    if (path) strlcpy(dirs, path, sizeof dirs);
    else {
        char exedir[300]; path_dirname(p->path, exedir, sizeof exedir);
        const char *pe = env_get(p, "PATH");
        snprintf(dirs, sizeof dirs, "%s;C:\\WRLD\\System32;C:\\WRLD;%s;%s", exedir, p->cwd, pe ? pe : "");
    }
    for (char *d = dirs, *e; d && *d; d = e) {
        e = strchr(d, ';'); if (e) *e++ = 0;
        if (!*d) continue;
        path_join(cand, sizeof cand, d, name); path_normalize(cand);
        vnode_t *v = vfs_resolve(cand, NULL);
        if (v && !v->dir) goto found;
    }
    set_err(2); return 0;
found:;
    DWORD l = strlen(cand);
    if (!out || n <= l) return l + 1;
    memcpy(out, cand, l + 1);
    if (part) *part = (char *)path_basename(out);
    return l;
}
static DWORD WINAPI SearchPathA(const char *path, const char *file, const char *ext, DWORD n, char *out, char **part) { return search(path, file, ext, out, n, part); }
static DWORD WINAPI SearchPathW(const WCHAR *path, const WCHAR *file, const WCHAR *ext, DWORD n, WCHAR *out, WCHAR **part) {
    char ap[1024], af[300], ae[32], o[600]; w2a(path, ap, sizeof ap); w2a(file, af, sizeof af); w2a(ext, ae, sizeof ae);
    DWORD r = search(path ? ap : NULL, af, ext ? ae : NULL, o, sizeof o, NULL);
    if (!r) return 0;
    if (!out || n <= r) return r + 1;
    a2w(o, out, n);
    if (part) { int b = (int)((const char *)path_basename(o) - o); *part = out + b; }
    return r;
}

/* ---------------------------------------------------------------- files and volumes */
static BOOL WINAPI GetFileInformationByHandleEx(HANDLE h, int cls, void *buf, DWORD n) {
    vnode_t *v = w32_handle_node(P, h);
    if (!v) { set_err(6); return 0; }
    if (cls == 0 && n >= 40) {                           /* FILE_BASIC_INFO: 4 times, attributes */
        memset(buf, 0, 40); *(DWORD *)((uint8_t *)buf + 32) = v->dir ? 0x10 : 0x80; return 1;
    }
    if (cls == 1 && n >= 24) {                           /* FILE_STANDARD_INFO */
        memset(buf, 0, 24); *(int64_t *)buf = v->size; *(int64_t *)((uint8_t *)buf + 8) = v->size;
        *(DWORD *)((uint8_t *)buf + 16) = 1; ((uint8_t *)buf)[21] = v->dir; return 1;
    }
    if (cls == 2 && n >= 4) {                            /* FILE_NAME_INFO: the path without the drive */
        char path[300]; vfs_path(v, path, sizeof path); const char *s = isalpha(path[0]) && path[1] == ':' ? path + 2 : path;
        DWORD l = strlen(s); *(DWORD *)buf = l * 2;
        if (n < 4 + l * 2) { set_err(234); return 0; }
        WCHAR *w = (WCHAR *)((uint8_t *)buf + 4); for (DWORD i = 0; i < l; i++) w[i] = (uint8_t)s[i];
        return 1;
    }
    set_err(87); return 0;
}
static DWORD WINAPI GetFinalPathNameByHandleW(HANDLE h, WCHAR *b, DWORD n, DWORD fl) {
    vnode_t *v = w32_handle_node(P, h);
    if (!v) { set_err(6); return 0; }
    char path[300], full[310]; vfs_path(v, path, sizeof path);
    snprintf(full, sizeof full, "\\\\?\\%s", path);
    DWORD l = strlen(full);
    if (!b || n <= l) return l + 1;
    a2w(full, b, n); return l;
}
static DWORD WINAPI GetFinalPathNameByHandleA(HANDLE h, char *b, DWORD n, DWORD fl) {
    vnode_t *v = w32_handle_node(P, h);
    if (!v) { set_err(6); return 0; }
    char path[300], full[310]; vfs_path(v, path, sizeof path); snprintf(full, sizeof full, "\\\\?\\%s", path);
    DWORD l = strlen(full); if (!b || n <= l) return l + 1; memcpy(b, full, l + 1); return l;
}
static BOOL WINAPI GetVolumeInformationA(const char *root, char *name, DWORD nn, DWORD *serial, DWORD *maxlen, DWORD *flags, char *fs, DWORD fsn) {
    char d = root && isalpha(root[0]) ? toupper(root[0]) : 'C';
    if (!vfs_drive_kind(d)) { set_err(3); return 0; }
    if (name && nn) strlcpy(name, d == 'C' ? "WRLD" : "", nn);
    if (serial) *serial = 0x57524C44 + d;
    if (maxlen) *maxlen = 255;
    if (flags) *flags = 0x3 | 0x4;                       /* case-preserved and Unicode names */
    if (fs && fsn) strlcpy(fs, vfs_drive_kind(d) == 2 ? "CDFS" : "FAT32", fsn);
    return 1;
}
static BOOL WINAPI GetVolumePathNameW(const WCHAR *f, WCHAR *out, DWORD n) {
    if (n < 4) { set_err(206); return 0; }
    out[0] = f && f[0] && f[1] == ':' ? f[0] : 'C'; out[1] = ':'; out[2] = '\\'; out[3] = 0; return 1;
}
static BOOL WINAPI GetVolumePathNameA(const char *f, char *out, DWORD n) { if (n < 4) return 0; snprintf(out, n, "%c:\\", f && f[1] == ':' ? f[0] : 'C'); return 1; }
static BOOL WINAPI GetVolumeNameForVolumeMountPointW(const WCHAR *m, WCHAR *out, DWORD n) { set_err(1); return 0; }
static DWORD WINAPI GetShortPathNameA(const char *l, char *s, DWORD n) { DWORD k = strlen(l); if (!s || n <= k) return k + 1; memcpy(s, l, k + 1); return k; }
static BOOL WINAPI CreateSymbolicLinkW(const WCHAR *a, const WCHAR *b, DWORD f) { set_err(1314); return 0; }
static BOOL WINAPI CreateSymbolicLinkA(const char *a, const char *b, DWORD f) { set_err(1314); return 0; }
static HANDLE WINAPI FindFirstChangeNotificationW(const WCHAR *p, BOOL sub, DWORD f) { return ((HANDLE (WINAPI *)(void *, BOOL, BOOL, const char *))K("CreateEventA"))(NULL, 1, 0, NULL); }
static HANDLE WINAPI FindFirstChangeNotificationA(const char *p, BOOL sub, DWORD f) { return FindFirstChangeNotificationW(NULL, sub, f); }
static BOOL WINAPI FindNextChangeNotification(HANDLE h) { return 1; }
static BOOL WINAPI FindCloseChangeNotification(HANDLE h) { return ((BOOL (WINAPI *)(HANDLE))K("CloseHandle"))(h); }
static BOOL WINAPI ReadDirectoryChangesW(HANDLE h, void *b, DWORD n, BOOL s, DWORD f, DWORD *r, void *ov, void *cr) { set_err(50); return 0; }

/* ---------------------------------------------------------------- console input records */
typedef struct PACKED { WORD type, pad; BOOL down; WORD repeat, vk, scan; WCHAR ch; DWORD ctrl; } INREC;
static int pending_key = -1;
static int map_vk(int c, WCHAR *ch) {
    *ch = 0;
    if (c >= 32 && c < 127) { *ch = c; return isalpha(c) ? toupper(c) : isdigit(c) ? c : c == ' ' ? 0x20 : 0; }
    switch (c) { case '\n': case '\r': *ch = '\r'; return 0x0D; case '\b': case 127: *ch = 8; return 0x08; case '\t': *ch = 9; return 0x09; case 27: *ch = 27; return 0x1B; }
    if (c >= 0x100) {                                    /* the console's arrow and editing keys */
        switch (c & 0xFF) { case 'A': return 0x26; case 'B': return 0x28; case 'C': return 0x27; case 'D': return 0x25; case 'H': return 0x24; case 'F': return 0x23; }
    }
    return 0;
}
static BOOL WINAPI ReadConsoleInputW(HANDLE h, INREC *r, DWORD n, DWORD *got) {
    w32_proc_t *p = P;
    if (!n) { if (got) *got = 0; return 1; }
    int c = pending_key >= 0 ? pending_key : (p->in ? p->in->getkey(p->in) : -1);
    pending_key = -1;
    memset(r, 0, sizeof *r); r->type = 1; r->down = 1; r->repeat = 1;
    WCHAR ch; r->vk = map_vk(c, &ch); r->ch = ch;
    if (got) *got = 1;
    return 1;
}
static BOOL WINAPI ReadConsoleInputA(HANDLE h, INREC *r, DWORD n, DWORD *got) { return ReadConsoleInputW(h, r, n, got); }
static BOOL WINAPI PeekConsoleInputW(HANDLE h, INREC *r, DWORD n, DWORD *got) { if (got) *got = 0; return 1; }
static BOOL WINAPI PeekConsoleInputA(HANDLE h, INREC *r, DWORD n, DWORD *got) { if (got) *got = 0; return 1; }
static BOOL WINAPI WriteConsoleInputW(HANDLE h, const INREC *r, DWORD n, DWORD *w) { if (n && r->type == 1 && r->down) pending_key = r->ch; if (w) *w = n; return 1; }
static BOOL WINAPI AttachConsole(DWORD pid) { set_err(6); return 0; }
static BOOL WINAPI GenerateConsoleCtrlEvent(DWORD e, DWORD g) { return 1; }

/* ---------------------------------------------------------------- serial ports (none) */
static BOOL WINAPI BuildCommDCBW(const WCHAR *s, void *dcb) { return 1; }
static BOOL WINAPI BuildCommDCBA(const char *s, void *dcb) { return 1; }
static BOOL WINAPI ClearCommError(HANDLE h, DWORD *e, void *st) { if (e) *e = 0; if (st) memset(st, 0, 12); return 1; }
static BOOL WINAPI EscapeCommFunction(HANDLE h, DWORD f) { return 1; }
static BOOL WINAPI GetCommModemStatus(HANDLE h, DWORD *s) { if (s) *s = 0; return 1; }
static BOOL WINAPI PurgeComm(HANDLE h, DWORD f) { return 1; }
static BOOL WINAPI SetupComm(HANDLE h, DWORD i, DWORD o) { return 1; }
static BOOL WINAPI GetCommTimeouts(HANDLE h, void *t) { memset(t, 0, 20); return 1; }
static BOOL WINAPI SetCommMask(HANDLE h, DWORD m) { return 1; }
static BOOL WINAPI WaitCommEvent(HANDLE h, DWORD *m, void *ov) { set_err(1); return 0; }

/* ---------------------------------------------------------------- odds and ends */
static void *WINAPI AddVectoredExceptionHandler(ULONG first, void *fn) { static int dummy; return &dummy; }
static ULONG WINAPI RemoveVectoredExceptionHandler(void *h) { return 1; }
static void *WINAPI AddVectoredContinueHandler(ULONG first, void *fn) { static int dummy; return &dummy; }
static ULONG WINAPI RemoveVectoredContinueHandler(void *h) { return 1; }
static BOOL WINAPI RtlAddFunctionTable(void *t, DWORD n, uintptr_t base) { return 1; }
static BOOL WINAPI RtlDeleteFunctionTable(void *t) { return 1; }
static BOOL WINAPI ContinueDebugEvent(DWORD p, DWORD t, DWORD s) { return 0; }
static BOOL WINAPI WaitForDebugEvent(void *e, DWORD ms) { return 0; }
static BOOL WINAPI DebugActiveProcess(DWORD p) { set_err(5); return 0; }
static BOOL WINAPI CheckRemoteDebuggerPresent(HANDLE h, BOOL *d) { *d = 0; return 1; }
static DWORD WINAPI ConvertDefaultLocale(DWORD l) { return l ? l : 0x409; }
static BOOL WINAPI EnumCalendarInfoW(void *fn, DWORD l, DWORD c, DWORD t) { return 0; }
static BOOL WINAPI EnumResourceLanguagesA(uintptr_t m, const char *t, const char *n, void *fn, LONG_PTR p) { set_err(1814); return 0; }
static BOOL WINAPI EnumResourceNamesW(uintptr_t m, const WCHAR *t, void *fn, LONG_PTR p) { set_err(1813); return 0; }
static BOOL WINAPI EnumResourceNamesA(uintptr_t m, const char *t, void *fn, LONG_PTR p) { set_err(1813); return 0; }
static BOOL WINAPI EnumResourceTypesW(uintptr_t m, void *fn, LONG_PTR p) { return 0; }
static DWORD WINAPI GetActiveProcessorCount(WORD g) { return 1; }
static DWORD WINAPI GetMaximumProcessorCount(WORD g) { return 1; }
static WORD WINAPI GetActiveProcessorGroupCount(void) { return 1; }
static BOOL WINAPI GetLogicalProcessorInformation(void *b, DWORD *n) { if (n) { if (*n < 24) { *n = 24; set_err(122); return 0; } memset(b, 0, 24); *(uintptr_t *)b = 1; *n = 24; } return 1; }
typedef struct { DWORD max; BYTE def[2]; BYTE lead[12]; WCHAR udef; UINT cp; WCHAR name[260]; } CPINFOEXW;
static BOOL WINAPI GetCPInfoExW(UINT cp, DWORD f, CPINFOEXW *c) {
    memset(c, 0, sizeof *c); c->max = cp == 65001 ? 4 : 1; c->def[0] = '?'; c->udef = '?';
    c->cp = cp == 0 || cp == 1 ? 1252 : cp; char n[64]; snprintf(n, sizeof n, "%u (%s)", c->cp, c->cp == 65001 ? "UTF-8" : c->cp == 437 ? "OEM - United States" : "ANSI - Latin I"); a2w(n, c->name, 260);
    return 1;
}
static BOOL WINAPI GetStringTypeExA(DWORD lc, DWORD t, const char *s, int n, WORD *o) {
    if (n < 0) n = strlen(s) + 1;
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i]; WORD v = 0;
        if (t == 1) { if (c >= 'A' && c <= 'Z') v |= 1; if (c >= 'a' && c <= 'z') v |= 2; if (c >= '0' && c <= '9') v |= 4; if (c == ' ' || (c >= 9 && c <= 13)) v |= 8;
            if (c > 32 && c < 127 && !isalnum(c)) v |= 0x10; if (c < 32 || c == 127) v |= 0x20; if (c == ' ' || c == '\t') v |= 0x40; if (isxdigit(c)) v |= 0x80; if (isalpha(c) || c >= 0xC0) v |= 0x100; }
        o[i] = v;
    }
    return 1;
}
static BOOL WINAPI GetStringTypeA(DWORD lc, DWORD t, const char *s, int n, WORD *o) { return GetStringTypeExA(lc, t, s, n, o); }
static WORD WINAPI GetSystemDefaultUILanguage(void) { return 0x409; }
static WORD WINAPI GetUserDefaultUILanguage(void) { return 0x409; }
static BOOL WINAPI SetThreadLocale(DWORD l) { return 1; }
static WORD WINAPI SetThreadUILanguage(WORD l) { return 0x409; }
static WORD WINAPI GetThreadUILanguage(void) { return 0x409; }
typedef struct { BYTE ac, flag, pct, res; DWORD life, full; } SPS;
static BOOL WINAPI GetSystemPowerStatus(SPS *s) { s->ac = 1; s->flag = 128; s->pct = 255; s->res = 0; s->life = s->full = 0xFFFFFFFF; return 1; }
static BOOL WINAPI GetSystemTimeAdjustment(DWORD *adj, DWORD *inc, BOOL *dis) { *adj = 156250; *inc = 156250; *dis = 1; return 1; }
static BOOL WINAPI GetThreadContext(HANDLE h, void *c) { set_err(5); return 0; }
static BOOL WINAPI SetThreadContext(HANDLE h, const void *c) { set_err(5); return 0; }
static DWORD WINAPI SetThreadExecutionState(DWORD f) { return 0x80000000; }
static UINT WINAPI SetHandleCount(UINT n) { return n; }
static BOOL WINAPI SetNamedPipeHandleState(HANDLE h, DWORD *m, DWORD *c, DWORD *t) { return 1; }
static BOOL WINAPI GetNamedPipeInfo(HANDLE h, DWORD *f, DWORD *o, DWORD *i, DWORD *m) { set_err(1); return 0; }
static BOOL WINAPI InitializeProcThreadAttributeList(void *l, DWORD n, DWORD f, SIZE_T *sz) {
    if (!l) { *sz = 64 + n * 24; set_err(122); return 0; }
    memset(l, 0, *sz); return 1;
}
static BOOL WINAPI UpdateProcThreadAttribute(void *l, DWORD f, uintptr_t a, void *v, SIZE_T n, void *prev, SIZE_T *rs) { return 1; }
static void WINAPI DeleteProcThreadAttributeList(void *l) {}
static bool is_self(HANDLE h) { return (uint32_t)h == 0xFFFFFFFFu; }
static BOOL WINAPI ReadProcessMemory(HANDLE h, const void *a, void *b, SIZE_T n, SIZE_T *got) {
    if (!is_self(h)) { set_err(5); return 0; }
    memcpy(b, a, n); if (got) *got = n; return 1;
}
static BOOL WINAPI WriteProcessMemory(HANDLE h, void *a, const void *b, SIZE_T n, SIZE_T *done) {
    if (!is_self(h)) { set_err(5); return 0; }
    memcpy(a, b, n); if (done) *done = n; return 1;
}
static BOOL WINAPI VirtualProtectEx(HANDLE h, void *a, SIZE_T n, DWORD np, DWORD *op) { if (!is_self(h)) { set_err(5); return 0; } if (op) *op = 0x40; return 1; }
static SIZE_T WINAPI VirtualQueryEx(HANDLE h, const void *a, void *m, SIZE_T n) { if (!is_self(h)) { set_err(5); return 0; } return ((SIZE_T (WINAPI *)(const void *, void *, SIZE_T))K("VirtualQuery"))(a, m, n); }
static void *WINAPI VirtualAllocEx(HANDLE h, void *a, SIZE_T n, DWORD t, DWORD p) { if (!is_self(h)) { set_err(5); return NULL; } return ((void *(WINAPI *)(void *, SIZE_T, DWORD, DWORD))K("VirtualAlloc"))(a, n, t, p); }
static BOOL WINAPI VirtualFreeEx(HANDLE h, void *a, SIZE_T n, DWORD t) { if (!is_self(h)) { set_err(5); return 0; } return ((BOOL (WINAPI *)(void *, SIZE_T, DWORD))K("VirtualFree"))(a, n, t); }
static BOOL WINAPI VirtualLock(void *a, SIZE_T n) { return 1; }
static BOOL WINAPI VirtualUnlock(void *a, SIZE_T n) { return 1; }
static BOOL WINAPI SetProcessWorkingSetSize(HANDLE h, SIZE_T a, SIZE_T b) { return 1; }
static BOOL WINAPI GetProcessWorkingSetSize(HANDLE h, SIZE_T *a, SIZE_T *b) { *a = 204800; *b = 1413120; return 1; }
static BOOL WINAPI GetProcessHandleCount(HANDLE h, DWORD *n) { *n = 16; return 1; }
static DWORD WINAPI GetProcessVersion(DWORD pid) { return 0x00060001; }
static BOOL WINAPI GetProcessIoCounters(HANDLE h, void *c) { memset(c, 0, 48); return 1; }
static BOOL WINAPI QueryProcessCycleTime(HANDLE h, uint64_t *c) { *c = (uint64_t)timer_ticks() * 2000000; return 1; }
static BOOL WINAPI QueryThreadCycleTime(HANDLE h, uint64_t *c) { *c = (uint64_t)timer_ticks() * 2000000; return 1; }
static BOOL WINAPI QueryUnbiasedInterruptTime(uint64_t *t) { *t = (uint64_t)timer_ticks() * 10000; return 1; }
static BOOL WINAPI GetNumaHighestNodeNumber(ULONG *n) { *n = 0; return 1; }
static BOOL WINAPI Wow64DisableWow64FsRedirection(void **o) { if (o) *o = NULL; return 1; }
static BOOL WINAPI Wow64RevertWow64FsRedirection(void *o) { return 1; }
static UINT WINAPI SetErrorMode(UINT m) { static UINT mode; UINT o = mode; mode = m; return o; }
static UINT WINAPI GetErrorMode(void) { return 0; }
static BOOL WINAPI SetThreadErrorMode(DWORD m, DWORD *o) { if (o) *o = 0; return 1; }
static DWORD WINAPI GetThreadErrorMode(void) { return 0; }
static void WINAPI RaiseFailFastException(void *r, void *c, DWORD f) { w32_exit_process(0xC0000602); }
static void WINAPI FatalAppExitA(UINT a, const char *m) {
    w32_proc_t *p = P; msgbox(path_basename(p->path), m ? m : "Fatal application exit", MBI_ERROR, MBX_OK, NULL, NULL); w32_exit_process(255);
}
static void WINAPI FatalAppExitW(UINT a, const WCHAR *m) { char b[300]; w2a(m, b, sizeof b); FatalAppExitA(a, b); }
static int WINAPI lstrlenW(const WCHAR *s) { return s ? wlen(s) : 0; }
static WCHAR *WINAPI lstrcpyW(WCHAR *d, const WCHAR *s) { int i = 0; do d[i] = s[i]; while (s[i++]); return d; }
static WCHAR *WINAPI lstrcpynW(WCHAR *d, const WCHAR *s, int n) { if (n <= 0) return d; int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; return d; }
static WCHAR *WINAPI lstrcatW(WCHAR *d, const WCHAR *s) { lstrcpyW(d + wlen(d), s); return d; }
static int wcmp(const WCHAR *a, const WCHAR *b, bool ci) {
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (ci) { if (x < 128) x = tolower(x); if (y < 128) y = tolower(y); }
        if (x != y || !x) return x < y ? -1 : x > y ? 1 : 0;
    }
}
static const WCHAR wempty[1];
static int WINAPI lstrcmpW(const WCHAR *a, const WCHAR *b) { return wcmp(a ? a : wempty, b ? b : wempty, false); }
static int WINAPI lstrcmpiW(const WCHAR *a, const WCHAR *b) { return wcmp(a ? a : wempty, b ? b : wempty, true); }
static BOOL WINAPI IsBadReadPtr(const void *p, uintptr_t n) { return !p; }
static BOOL WINAPI IsBadWritePtr(void *p, uintptr_t n) { return !p; }
static BOOL WINAPI IsBadCodePtr(void *p) { return !p; }
static BOOL WINAPI IsBadStringPtrA(const char *p, uintptr_t n) { return !p; }
static BOOL WINAPI IsBadStringPtrW(const WCHAR *p, uintptr_t n) { return !p; }
static DWORD WINAPI WTSGetActiveConsoleSessionId(void) { return 1; }
static BOOL WINAPI ProcessIdToSessionId(DWORD pid, DWORD *s) { *s = 1; return 1; }
static HANDLE WINAPI CreateWaitableTimerW(void *sa, BOOL m, const WCHAR *n) { return ((HANDLE (WINAPI *)(void *, BOOL, BOOL, const char *))K("CreateEventA"))(NULL, m, 0, NULL); }
static HANDLE WINAPI CreateWaitableTimerA(void *sa, BOOL m, const char *n) { return CreateWaitableTimerW(sa, m, NULL); }
static BOOL WINAPI SetWaitableTimer(HANDLE h, const int64_t *due, LONG per, void *fn, void *arg, BOOL r) { return ((BOOL (WINAPI *)(HANDLE))K("SetEvent"))(h); }
static BOOL WINAPI CancelWaitableTimer(HANDLE h) { return 1; }
static BOOL WINAPI GetComputerNameExA(int t, char *b, DWORD *n) { DWORD l = strlen(computer_name); if (*n <= l) { *n = l + 1; set_err(234); return 0; } strcpy(b, computer_name); *n = l; return 1; }
static BOOL WINAPI SetComputerNameA(const char *n) { return 1; }
static DWORD WINAPI GetLongPathNameA2(const char *s, char *l, DWORD n) { DWORD k = strlen(s); if (!l || n <= k) return k + 1; memcpy(l, s, k + 1); return k; }
static UINT WINAPI GetSystemWow64DirectoryA(char *b, UINT n) { set_err(120); return 0; }
static UINT WINAPI GetSystemWow64DirectoryW(WCHAR *b, UINT n) { set_err(120); return 0; }
static BOOL WINAPI GetUserNameExW(int f, WCHAR *b, DWORD *n) { DWORD l = strlen(user_name); if (*n <= l) { *n = l + 1; set_err(234); return 0; } a2w(user_name, b, *n); *n = l; return 1; }
static BOOL WINAPI GetUserNameExA(int f, char *b, DWORD *n) { DWORD l = strlen(user_name); if (*n <= l) { *n = l + 1; set_err(234); return 0; } strcpy(b, user_name); *n = l; return 1; }

#define X(n) { "kernel32", #n, (void *)n }
#define XA(n, f) { "kernel32", n, (void *)f }
const w32_api_t k32more_api[] = {
    X(GetEnvironmentVariableA), X(GetEnvironmentVariableW), X(SetEnvironmentVariableA), X(SetEnvironmentVariableW), XA("GetEnvironmentStrings", GetEnvironmentStringsA),
    X(GetEnvironmentStringsA), X(GetEnvironmentStringsW), X(FreeEnvironmentStringsA), X(FreeEnvironmentStringsW), X(SetEnvironmentStringsW),
    X(ExpandEnvironmentStringsA), X(ExpandEnvironmentStringsW), X(GetVersion), X(GetVersionExA), X(GetVersionExW), X(RtlGetVersion), X(VerSetConditionMask),
    X(VerifyVersionInfoW), X(VerifyVersionInfoA), X(GetProductInfo),
    X(GetPrivateProfileStringA), X(GetPrivateProfileStringW), X(GetPrivateProfileIntA), X(GetPrivateProfileIntW), X(WritePrivateProfileStringA),
    X(WritePrivateProfileStringW), X(GetPrivateProfileSectionA), X(GetPrivateProfileSectionW), X(WritePrivateProfileSectionA), X(WritePrivateProfileSectionW),
    X(GetPrivateProfileSectionNamesA), X(GetPrivateProfileSectionNamesW), X(GetProfileStringA), X(GetProfileStringW), X(GetProfileIntA), X(GetProfileIntW),
    X(WriteProfileStringA), X(WriteProfileStringW), X(GetPrivateProfileStructA), X(WritePrivateProfileStructA),
    X(GlobalAddAtomA), X(GlobalAddAtomW), X(GlobalFindAtomA), X(GlobalFindAtomW), X(GlobalDeleteAtom), X(GlobalGetAtomNameA), X(GlobalGetAtomNameW),
    XA("AddAtomA", GlobalAddAtomA), XA("AddAtomW", GlobalAddAtomW), XA("FindAtomA", GlobalFindAtomA), XA("FindAtomW", GlobalFindAtomW), XA("DeleteAtom", GlobalDeleteAtom),
    XA("GetAtomNameA", GlobalGetAtomNameA), XA("GetAtomNameW", GlobalGetAtomNameW),
    X(GlobalSize), X(GlobalReAlloc), X(GlobalFlags), X(GlobalHandle), X(LocalHandle), X(LocalFlags), X(LocalLock), X(LocalUnlock), X(GlobalCompact), X(GlobalFix),
    X(GlobalUnfix), XA("LocalSize", GlobalSize), XA("LocalReAlloc", GlobalReAlloc),
    X(CreateToolhelp32Snapshot), X(Process32First), X(Process32Next), X(Process32FirstW), X(Process32NextW), X(Module32First), X(Module32Next),
    X(Module32FirstW), X(Module32NextW), X(Thread32First), X(Thread32Next),
    X(SearchPathA), X(SearchPathW), X(GetFileInformationByHandleEx), X(GetFinalPathNameByHandleW), X(GetFinalPathNameByHandleA), X(GetVolumeInformationA),
    X(GetVolumePathNameW), X(GetVolumePathNameA), X(GetVolumeNameForVolumeMountPointW), X(GetShortPathNameA), X(CreateSymbolicLinkW), X(CreateSymbolicLinkA),
    X(FindFirstChangeNotificationW), X(FindFirstChangeNotificationA), X(FindNextChangeNotification), X(FindCloseChangeNotification), X(ReadDirectoryChangesW),
    X(ReadConsoleInputW), X(ReadConsoleInputA), X(PeekConsoleInputW), X(PeekConsoleInputA), X(WriteConsoleInputW), X(AttachConsole), X(GenerateConsoleCtrlEvent),
    X(BuildCommDCBW), X(BuildCommDCBA), X(ClearCommError), X(EscapeCommFunction), X(GetCommModemStatus), X(PurgeComm), X(SetupComm), X(GetCommTimeouts),
    X(SetCommMask), X(WaitCommEvent),
    X(AddVectoredExceptionHandler), X(RemoveVectoredExceptionHandler), X(AddVectoredContinueHandler), X(RemoveVectoredContinueHandler), X(RtlAddFunctionTable),
    X(RtlDeleteFunctionTable), X(ContinueDebugEvent), X(WaitForDebugEvent), X(DebugActiveProcess), X(CheckRemoteDebuggerPresent), X(ConvertDefaultLocale),
    X(EnumCalendarInfoW), X(EnumResourceLanguagesA), X(EnumResourceNamesW), X(EnumResourceNamesA), X(EnumResourceTypesW), X(GetActiveProcessorCount),
    X(GetMaximumProcessorCount), X(GetActiveProcessorGroupCount), X(GetLogicalProcessorInformation), X(GetCPInfoExW), X(GetStringTypeExA), X(GetStringTypeA),
    X(GetSystemDefaultUILanguage), X(GetUserDefaultUILanguage), X(SetThreadLocale), X(SetThreadUILanguage), X(GetThreadUILanguage), X(GetSystemPowerStatus),
    X(GetSystemTimeAdjustment), X(GetThreadContext), X(SetThreadContext), X(SetThreadExecutionState), X(SetHandleCount), X(SetNamedPipeHandleState),
    X(GetNamedPipeInfo), X(InitializeProcThreadAttributeList), X(UpdateProcThreadAttribute), X(DeleteProcThreadAttributeList), X(ReadProcessMemory),
    X(WriteProcessMemory), X(VirtualProtectEx), X(VirtualQueryEx), X(VirtualAllocEx), X(VirtualFreeEx), X(VirtualLock), X(VirtualUnlock),
    X(SetProcessWorkingSetSize), X(GetProcessWorkingSetSize), X(GetProcessHandleCount), X(GetProcessVersion), X(GetProcessIoCounters), X(QueryProcessCycleTime),
    X(QueryThreadCycleTime), X(QueryUnbiasedInterruptTime), X(GetNumaHighestNodeNumber), X(Wow64DisableWow64FsRedirection), X(Wow64RevertWow64FsRedirection),
    X(SetErrorMode), X(GetErrorMode), X(SetThreadErrorMode), X(GetThreadErrorMode), X(RaiseFailFastException), X(FatalAppExitA), X(FatalAppExitW),
    X(lstrlenW), X(lstrcpyW), X(lstrcpynW), X(lstrcatW), X(lstrcmpW), X(lstrcmpiW), X(IsBadReadPtr), X(IsBadWritePtr), X(IsBadCodePtr), X(IsBadStringPtrA),
    X(IsBadStringPtrW), X(WTSGetActiveConsoleSessionId), X(ProcessIdToSessionId), X(CreateWaitableTimerW), X(CreateWaitableTimerA), X(SetWaitableTimer),
    X(CancelWaitableTimer), X(GetComputerNameExA), X(SetComputerNameA), XA("GetLongPathNameA", GetLongPathNameA2), X(GetSystemWow64DirectoryA),
    X(GetSystemWow64DirectoryW),
    { "advapi32", "GetUserNameExW", (void *)GetUserNameExW }, { "advapi32", "GetUserNameExA", (void *)GetUserNameExA },
    { "secur32", "GetUserNameExW", (void *)GetUserNameExW }, { "secur32", "GetUserNameExA", (void *)GetUserNameExA },
    { NULL, NULL, NULL }
};
