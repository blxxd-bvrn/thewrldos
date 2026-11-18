/* shlwapi (path and string helpers), more of shell32 (special folders, item ID lists,
 * ShellExecuteEx, CommandLineToArgvW, SHFileOperation, SHGetFileInfo), version.dll (version
 * resources read from the file itself) and winmm (time, PlaySound, waveOut on the kernel's
 * sound streams). Looked up before the older tables (w32_extra_find). */
#include "w32.h"
#include "audio.h"
#include "theme.h"

#define P w32_cur()
static inline void set_err(uint32_t e) { w32_proc_t *p = P; if (p) p->last_error = e; }
static void *K(const char *name) { void *fn = NULL; w32_lookup("kernel32.dll", name, &fn); return fn; }
static void w2a(const WCHAR *w, char *a, int cap) { int i = 0; if (w) for (; w[i] && i < cap - 1; i++) a[i] = w[i] < 256 ? (char)w[i] : '?'; a[i] = 0; }
static int a2w(const char *a, WCHAR *w, int cap) { int i = 0; for (; a[i] && i < cap - 1; i++) w[i] = (uint8_t)a[i]; if (cap > 0) w[i] = 0; return i; }
static int wlen(const WCHAR *w) { int n = 0; while (w && w[n]) n++; return n; }
static WCHAR wlow(WCHAR c) { return c < 128 ? tolower(c) : c; }
#define S_OK 0
#define E_FAIL ((LONG)0x80004005)
#define E_INVALIDARG ((LONG)0x80070057)
#define E_NOTIMPL ((LONG)0x80004001)
#define MAXP 260
typedef uint32_t ULONG;

/* ================================================================ shlwapi: paths (wide) */
static bool is_sep(WCHAR c) { return c == '\\' || c == '/'; }
static WCHAR *WINAPI PathFindFileNameW(const WCHAR *p) { const WCHAR *r = p; for (const WCHAR *c = p; *c; c++) if ((is_sep(*c) || *c == ':') && c[1]) r = c + 1; return (WCHAR *)r; }
static char *WINAPI PathFindFileNameA(const char *p) { const char *r = p; for (const char *c = p; *c; c++) if ((*c == '\\' || *c == '/' || *c == ':') && c[1]) r = c + 1; return (char *)r; }
static WCHAR *WINAPI PathFindExtensionW(const WCHAR *p) { const WCHAR *d = NULL; for (const WCHAR *c = p; *c; c++) { if (is_sep(*c) || *c == ' ') d = NULL; else if (*c == '.') d = c; } return (WCHAR *)(d ? d : p + wlen(p)); }
static char *WINAPI PathFindExtensionA(const char *p) { const char *d = NULL; for (const char *c = p; *c; c++) { if (*c == '\\' || *c == '/' || *c == ' ') d = NULL; else if (*c == '.') d = c; } return (char *)(d ? d : p + strlen(p)); }
static BOOL WINAPI PathFileExistsA(const char *p) { return p && ((DWORD (WINAPI *)(const char *))K("GetFileAttributesA"))(p) != 0xFFFFFFFF; }
static BOOL WINAPI PathFileExistsW(const WCHAR *p) { char a[520]; w2a(p, a, sizeof a); return p && PathFileExistsA(a); }
static BOOL WINAPI PathIsDirectoryA(const char *p) { DWORD a = ((DWORD (WINAPI *)(const char *))K("GetFileAttributesA"))(p); return a != 0xFFFFFFFF && (a & 0x10) ? 0x10 : 0; }
static BOOL WINAPI PathIsDirectoryW(const WCHAR *p) { char a[520]; w2a(p, a, sizeof a); return PathIsDirectoryA(a); }
static BOOL WINAPI PathIsRelativeW(const WCHAR *p) { return !(p && (is_sep(p[0]) || (p[0] && p[1] == ':'))); }
static BOOL WINAPI PathIsRelativeA(const char *p) { return !(p && (p[0] == '\\' || p[0] == '/' || (p[0] && p[1] == ':'))); }
static BOOL WINAPI PathIsRootW(const WCHAR *p) { if (!p) return 0; if (is_sep(p[0]) && !p[1]) return 1; if (p[0] && p[1] == ':' && is_sep(p[2]) && !p[3]) return 1; if (is_sep(p[0]) && is_sep(p[1])) { int s = 0; for (const WCHAR *c = p + 2; *c; c++) if (is_sep(*c)) s++; return s <= 1; } return 0; }
static BOOL WINAPI PathIsRootA(const char *p) { WCHAR w[MAXP]; a2w(p, w, MAXP); return PathIsRootW(w); }
static BOOL WINAPI PathIsUNCW(const WCHAR *p) { return p && is_sep(p[0]) && is_sep(p[1]); }
static BOOL WINAPI PathIsUNCA(const char *p) { return p && p[0] == '\\' && p[1] == '\\'; }
static BOOL WINAPI PathIsURLW(const WCHAR *p) { for (int i = 0; p && p[i] && i < 12; i++) if (p[i] == ':') return i > 1 && p[i + 1] == '/'; return 0; }
static BOOL WINAPI PathIsURLA(const char *p) { WCHAR w[MAXP]; a2w(p, w, MAXP); return PathIsURLW(w); }
static BOOL WINAPI PathIsFileSpecW(const WCHAR *p) { for (; *p; p++) if (is_sep(*p) || *p == ':') return 0; return 1; }
static int WINAPI PathGetDriveNumberW(const WCHAR *p) { return p && p[0] && p[1] == ':' && isalpha(p[0] & 0x7F) ? tolower(p[0]) - 'a' : -1; }
static int WINAPI PathGetDriveNumberA(const char *p) { return p && p[0] && p[1] == ':' && isalpha(p[0]) ? tolower(p[0]) - 'a' : -1; }
static WCHAR *WINAPI PathAddBackslashW(WCHAR *p) { int l = wlen(p); if (l + 1 >= MAXP) return NULL; if (!l || !is_sep(p[l - 1])) { p[l++] = '\\'; p[l] = 0; } return p + l; }
static char *WINAPI PathAddBackslashA(char *p) { int l = strlen(p); if (l + 1 >= MAXP) return NULL; if (!l || (p[l - 1] != '\\' && p[l - 1] != '/')) { p[l++] = '\\'; p[l] = 0; } return p + l; }
static WCHAR *WINAPI PathRemoveBackslashW(WCHAR *p) { int l = wlen(p); if (l && is_sep(p[l - 1]) && !PathIsRootW(p)) p[--l] = 0; return p + (l ? l - 1 : 0); }
static char *WINAPI PathRemoveBackslashA(char *p) { int l = strlen(p); if (l && (p[l - 1] == '\\' || p[l - 1] == '/') && !(l == 3 && p[1] == ':')) p[--l] = 0; return p + (l ? l - 1 : 0); }
static BOOL WINAPI PathRemoveFileSpecW(WCHAR *p) {
    WCHAR *f = PathFindFileNameW(p);
    if (f == p) { if (p[0] && is_sep(p[0]) && p[1]) { p[1] = 0; return 1; } if (!is_sep(p[0]) && p[0]) { p[0] = 0; return 1; } return 0; }
    WCHAR *e = f; if (e > p && is_sep(e[-1])) e--;
    if (e == p || (e == p + 2 && p[1] == ':')) e++;      /* keep the root's backslash */
    bool ch = *e != 0; *e = 0; return ch;
}
static BOOL WINAPI PathRemoveFileSpecA(char *p) { WCHAR w[MAXP]; a2w(p, w, MAXP); BOOL r = PathRemoveFileSpecW(w); w2a(w, p, MAXP); return r; }
static void WINAPI PathRemoveExtensionW(WCHAR *p) { WCHAR *e = PathFindExtensionW(p); *e = 0; }
static void WINAPI PathRemoveExtensionA(char *p) { char *e = PathFindExtensionA(p); *e = 0; }
static BOOL WINAPI PathRenameExtensionW(WCHAR *p, const WCHAR *ext) { WCHAR *e = PathFindExtensionW(p); if ((e - p) + wlen(ext) >= MAXP) return 0; for (int i = 0; i <= wlen(ext); i++) e[i] = ext[i]; return 1; }
static BOOL WINAPI PathRenameExtensionA(char *p, const char *ext) { char *e = PathFindExtensionA(p); if ((size_t)(e - p) + strlen(ext) >= MAXP) return 0; strcpy(e, ext); return 1; }
static BOOL WINAPI PathAddExtensionW(WCHAR *p, const WCHAR *ext) { if (*PathFindExtensionW(p)) return 0; static const WCHAR exe[] = { '.', 'e', 'x', 'e', 0 }; if (!ext) ext = exe; int l = wlen(p); for (int i = 0; i <= wlen(ext); i++) p[l + i] = ext[i]; return 1; }
static void WINAPI PathStripPathW(WCHAR *p) { WCHAR *f = PathFindFileNameW(p); if (f != p) { int l = wlen(f); memmove(p, f, (l + 1) * 2); } }
static void WINAPI PathStripPathA(char *p) { char *f = PathFindFileNameA(p); if (f != p) memmove(p, f, strlen(f) + 1); }
static BOOL WINAPI PathStripToRootW(WCHAR *p) { if (p[0] && p[1] == ':') { if (is_sep(p[2])) p[3] = 0; else p[2] = 0; return 1; } if (is_sep(p[0])) { p[1] = 0; return 1; } return 0; }
static BOOL WINAPI PathStripToRootA(char *p) { if (p[0] && p[1] == ':') { if (p[2] == '\\') p[3] = 0; else p[2] = 0; return 1; } if (p[0] == '\\') { p[1] = 0; return 1; } return 0; }
static WCHAR *WINAPI PathSkipRootW(const WCHAR *p) { if (p[0] && p[1] == ':' && is_sep(p[2])) return (WCHAR *)p + 3; if (is_sep(p[0]) && !is_sep(p[1])) return (WCHAR *)p + 1; return NULL; }
static char *WINAPI PathSkipRootA(const char *p) { if (p[0] && p[1] == ':' && p[2] == '\\') return (char *)p + 3; if (p[0] == '\\' && p[1] != '\\') return (char *)p + 1; return NULL; }
static WCHAR *WINAPI PathFindNextComponentW(const WCHAR *p) { if (!p || !*p) return NULL; while (*p && !is_sep(*p)) p++; if (*p) p++; return (WCHAR *)p; }
/* resolve "." and ".." */
static BOOL WINAPI PathCanonicalizeW(WCHAR *out, const WCHAR *in) {
    WCHAR t[MAXP]; int n = 0;
    if (!in || !*in) { out[0] = '\\'; out[1] = 0; return 1; }
    int root = 0;
    if (in[0] && in[1] == ':') { t[n++] = in[0]; t[n++] = ':'; root = 2; if (is_sep(in[2])) { t[n++] = '\\'; root = 3; } }
    else if (is_sep(in[0])) { t[n++] = '\\'; root = 1; if (is_sep(in[1])) { t[n++] = '\\'; root = 2; } }
    const WCHAR *s = in + root;
    while (*s) {
        const WCHAR *e = s; while (*e && !is_sep(*e)) e++;
        int l = e - s;
        if (l == 1 && s[0] == '.') {}
        else if (l == 2 && s[0] == '.' && s[1] == '.') { if (n > root) { n--; if (n > root && t[n] == '\\') {} while (n > root && t[n - 1] != '\\') n--; if (n > root) n--; if (n < root) n = root; } }
        else { if (n > root && t[n - 1] != '\\') t[n++] = '\\'; for (int i = 0; i < l && n < MAXP - 2; i++) t[n++] = s[i]; }
        s = *e ? e + 1 : e;
        if (!*e && l && is_sep(e[-1])) {}
    }
    if (n > root && n < MAXP - 1 && in[wlen(in) - 1] == '\\' && t[n - 1] != '\\') t[n++] = '\\';
    t[n] = 0;
    if (!n) { t[0] = '\\'; t[1] = 0; }
    for (int i = 0; i <= wlen(t); i++) out[i] = t[i];
    return 1;
}
static BOOL WINAPI PathCanonicalizeA(char *out, const char *in) { WCHAR a[MAXP], b[MAXP]; a2w(in, a, MAXP); PathCanonicalizeW(b, a); w2a(b, out, MAXP); return 1; }
static WCHAR *WINAPI PathCombineW(WCHAR *out, const WCHAR *dir, const WCHAR *file) {
    WCHAR t[MAXP * 2]; int n = 0;
    if (!dir && !file) { if (out) out[0] = 0; return NULL; }
    if (file && (!PathIsRelativeW(file) || !dir || !*dir)) { for (int i = 0; file[i] && n < MAXP * 2 - 1; i++) t[n++] = file[i]; }
    else {
        for (int i = 0; dir[i] && n < MAXP * 2 - 2; i++) t[n++] = dir[i];
        if (file && *file) { if (n && !is_sep(t[n - 1])) t[n++] = '\\'; for (int i = 0; file[i] && n < MAXP * 2 - 1; i++) t[n++] = file[i]; }
    }
    t[n] = 0;
    if (n >= MAXP) { out[0] = 0; return NULL; }
    PathCanonicalizeW(out, t);
    return out;
}
static char *WINAPI PathCombineA(char *out, const char *dir, const char *file) { WCHAR a[MAXP], b[MAXP], c[MAXP]; a2w(dir ? dir : "", a, MAXP); a2w(file ? file : "", b, MAXP); if (!PathCombineW(c, dir ? a : NULL, file ? b : NULL)) { out[0] = 0; return NULL; } w2a(c, out, MAXP); return out; }
static BOOL WINAPI PathAppendW(WCHAR *p, const WCHAR *more) { while (more && is_sep(*more)) more++; WCHAR t[MAXP]; if (!PathCombineW(t, p, more)) return 0; for (int i = 0; i <= wlen(t); i++) p[i] = t[i]; return 1; }
static BOOL WINAPI PathAppendA(char *p, const char *more) { while (more && (*more == '\\' || *more == '/')) more++; char t[MAXP]; if (!PathCombineA(t, p, more)) return 0; strcpy(p, t); return 1; }
static int WINAPI PathCommonPrefixW(const WCHAR *a, const WCHAR *b, WCHAR *out) {
    int last = 0, i = 0;
    for (; a[i] && b[i] && wlow(a[i]) == wlow(b[i]); i++) if (is_sep(a[i])) last = i;
    if (!a[i] && (!b[i] || is_sep(b[i]))) last = i; else if (!b[i] && is_sep(a[i])) last = i;
    if (last == 2 && a[1] == ':') last = 3;
    if (out) { for (int k = 0; k < last; k++) out[k] = a[k]; out[last] = 0; }
    return last;
}
static BOOL WINAPI PathIsPrefixW(const WCHAR *pre, const WCHAR *p) { return PathCommonPrefixW(pre, p, NULL) == wlen(pre); }
static BOOL WINAPI PathIsSameRootW(const WCHAR *a, const WCHAR *b) { return a[0] && b[0] && wlow(a[0]) == wlow(b[0]) && a[1] == ':' && b[1] == ':'; }
static BOOL WINAPI PathRelativePathToW(WCHAR *out, const WCHAR *from, DWORD fa, const WCHAR *to, DWORD ta) {
    WCHAR f[MAXP]; for (int i = 0; i <= wlen(from); i++) f[i] = from[i];
    if (!(fa & 0x10)) PathRemoveFileSpecW(f);
    int c = PathCommonPrefixW(f, to, NULL);
    if (!c) return 0;
    int n = 0; out[0] = 0;
    const WCHAR *r = f + c; if (is_sep(*r)) r++;
    if (*r) { n += 2; out[0] = '.'; out[1] = '.'; for (; *r; r++) if (is_sep(*r)) { out[n++] = '\\'; out[n++] = '.'; out[n++] = '.'; } }
    else { out[n++] = '.'; }
    const WCHAR *t = to + c; if (is_sep(*t)) t++;
    if (*t) { out[n++] = '\\'; while (*t && n < MAXP - 1) out[n++] = *t++; }
    out[n] = 0;
    return 1;
}
static BOOL wild(const WCHAR *pat, const WCHAR *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') { while (pat[1] == '*') pat++; if (!pat[1]) return 1; for (; *s; s++) if (wild(pat + 1, s)) return 1; return wild(pat + 1, s); }
        if (!*s || (*pat != '?' && wlow(*pat) != wlow(*s))) return 0;
    }
    return !*s;
}
static BOOL WINAPI PathMatchSpecW(const WCHAR *file, const WCHAR *spec) {
    const WCHAR *name = PathFindFileNameW(file);
    WCHAR one[MAXP];
    while (*spec) {
        while (*spec == ' ' || *spec == ';') spec++;
        int k = 0; while (*spec && *spec != ';' && k < MAXP - 1) one[k++] = *spec++;
        while (k && one[k - 1] == ' ') k--;
        one[k] = 0;
        if (k && (wild(one, name) || (one[0] == '*' && one[1] == '.' && one[2] == '*' && !one[3]))) return 1;
    }
    return 0;
}
static BOOL WINAPI PathMatchSpecA(const char *f, const char *s) { WCHAR a[MAXP], b[MAXP]; a2w(f, a, MAXP); a2w(s, b, MAXP); return PathMatchSpecW(a, b); }
static BOOL WINAPI PathQuoteSpacesW(WCHAR *p) { int l = wlen(p); bool sp = false; for (int i = 0; i < l; i++) if (p[i] == ' ') sp = true; if (!sp || l + 3 > MAXP) return 0; memmove(p + 1, p, (l + 1) * 2); p[0] = '"'; p[l + 1] = '"'; p[l + 2] = 0; return 1; }
static void WINAPI PathUnquoteSpacesW(WCHAR *p) { int l = wlen(p); if (l >= 2 && p[0] == '"' && p[l - 1] == '"') { memmove(p, p + 1, (l - 2) * 2); p[l - 2] = 0; } }
static void WINAPI PathUnquoteSpacesA(char *p) { int l = strlen(p); if (l >= 2 && p[0] == '"' && p[l - 1] == '"') { memmove(p, p + 1, l - 2); p[l - 2] = 0; } }
static BOOL WINAPI PathUnExpandEnvStringsW(const WCHAR *p, WCHAR *out, UINT n) { return 0; }
static BOOL WINAPI PathCompactPathExW(WCHAR *out, const WCHAR *p, UINT n, DWORD f) {
    int l = wlen(p);
    if ((UINT)l < n) { for (int i = 0; i <= l; i++) out[i] = p[i]; return 1; }
    if (n < 5) { if (n) out[0] = 0; return 0; }
    const WCHAR *name = PathFindFileNameW(p); int nl = wlen(name);
    if ((UINT)nl + 4 >= n) { for (UINT i = 0; i < n - 4; i++) out[i] = name[i]; out[n - 4] = '.'; out[n - 3] = '.'; out[n - 2] = '.'; out[n - 1] = 0; return 1; }
    int keep = n - 1 - nl - 4; int k = 0;
    for (int i = 0; i < keep; i++) out[k++] = p[i];
    out[k++] = '.'; out[k++] = '.'; out[k++] = '.'; out[k++] = '\\';
    for (int i = 0; i <= nl; i++) out[k++] = name[i];
    return 1;
}
static BOOL WINAPI PathCompactPathExA(char *out, const char *p, UINT n, DWORD f) { WCHAR a[MAXP], b[MAXP]; a2w(p, a, MAXP); BOOL r = PathCompactPathExW(b, a, MIN(n, MAXP), f); w2a(b, out, n); return r; }
static LONG WINAPI SHAutoComplete(HWND h, DWORD f) { return S_OK; }
static BOOL WINAPI IsOS(DWORD os) { return os == 0 || os == 2 || os == 25; }   /* OS_WINDOWS, OS_NT, OS_WOW6432? no: 25 is OS_ANYSERVER... kept for NT checks */

/* ================================================================ shlwapi: strings */
static WCHAR *WINAPI StrChrW(const WCHAR *s, WCHAR c) { for (; s && *s; s++) if (*s == c) return (WCHAR *)s; return NULL; }
static char *WINAPI StrChrA(const char *s, WORD c) { return s ? strchr(s, c) : NULL; }
static WCHAR *WINAPI StrChrIW(const WCHAR *s, WCHAR c) { for (; s && *s; s++) if (wlow(*s) == wlow(c)) return (WCHAR *)s; return NULL; }
static char *WINAPI StrChrIA(const char *s, WORD c) { for (; s && *s; s++) if (tolower(*s) == tolower(c)) return (char *)s; return NULL; }
static WCHAR *WINAPI StrRChrW(const WCHAR *s, const WCHAR *end, WCHAR c) { const WCHAR *r = NULL; if (!end) end = s + wlen(s); for (; s && s < end; s++) if (*s == c) r = s; return (WCHAR *)r; }
static char *WINAPI StrRChrA(const char *s, const char *end, WORD c) { const char *r = NULL; if (!end) end = s + strlen(s); for (; s && s < end; s++) if (*s == (char)c) r = s; return (char *)r; }
static int wcmpn(const WCHAR *a, const WCHAR *b, int n, bool ci) {
    for (int i = 0; n < 0 || i < n; i++) {
        WCHAR x = ci ? wlow(a[i]) : a[i], y = ci ? wlow(b[i]) : b[i];
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static int WINAPI StrCmpW(const WCHAR *a, const WCHAR *b) { return wcmpn(a, b, -1, false); }
static int WINAPI StrCmpIW(const WCHAR *a, const WCHAR *b) { return wcmpn(a, b, -1, true); }
static int WINAPI StrCmpNW(const WCHAR *a, const WCHAR *b, int n) { return wcmpn(a, b, n, false); }
static int WINAPI StrCmpNIW(const WCHAR *a, const WCHAR *b, int n) { return wcmpn(a, b, n, true); }
static int WINAPI StrCmpNA(const char *a, const char *b, int n) { int r = strncmp(a, b, n); return r < 0 ? -1 : r > 0; }
static int WINAPI StrCmpNIA(const char *a, const char *b, int n) { int r = strncasecmp(a, b, n); return r < 0 ? -1 : r > 0; }
static int WINAPI StrCmpIA(const char *a, const char *b) { int r = strcasecmp(a, b); return r < 0 ? -1 : r > 0; }
static int WINAPI StrCmpLogicalW(const WCHAR *a, const WCHAR *b) {
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            uint64_t x = 0, y = 0; while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0'); while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        WCHAR x = wlow(*a), y = wlow(*b);
        if (x != y) return x < y ? -1 : 1;
        a++; b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}
static WCHAR *WINAPI StrCpyW(WCHAR *d, const WCHAR *s) { int i = 0; do d[i] = s[i]; while (s[i++]); return d; }
static WCHAR *WINAPI StrCpyNW(WCHAR *d, const WCHAR *s, int n) { if (n <= 0) return d; int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; return d; }
static char *WINAPI StrCpyNA(char *d, const char *s, int n) { if (n > 0) strlcpy(d, s, n); return d; }
static WCHAR *WINAPI StrCatW(WCHAR *d, const WCHAR *s) { StrCpyW(d + wlen(d), s); return d; }
static WCHAR *WINAPI StrNCatW(WCHAR *d, const WCHAR *s, int n) { int l = wlen(d), i = 0; for (; i < n && s[i]; i++) d[l + i] = s[i]; d[l + i] = 0; return d; }
static char *WINAPI StrNCatA(char *d, const char *s, int n) { size_t l = strlen(d); int i = 0; for (; i < n && s[i]; i++) d[l + i] = s[i]; d[l + i] = 0; return d; }
static WCHAR *WINAPI StrCatBuffW(WCHAR *d, const WCHAR *s, int n) { int l = wlen(d); if (l < n - 1) StrCpyNW(d + l, s, n - l); return d; }
static char *WINAPI StrCatBuffA(char *d, const char *s, int n) { int l = strlen(d); if (l < n - 1) strlcpy(d + l, s, n - l); return d; }
static WCHAR *WINAPI StrDupW(const WCHAR *s) { int l = wlen(s); WCHAR *d = ((void *(WINAPI *)(UINT, SIZE_T))K("LocalAlloc"))(0, (l + 1) * 2); if (d) memcpy(d, s, (l + 1) * 2); return d; }
static char *WINAPI StrDupA(const char *s) { size_t l = strlen(s); char *d = ((void *(WINAPI *)(UINT, SIZE_T))K("LocalAlloc"))(0, l + 1); if (d) memcpy(d, s, l + 1); return d; }
static WCHAR *wstrstr(const WCHAR *h, const WCHAR *n, bool ci) { int l = wlen(n); if (!l) return (WCHAR *)h; for (; *h; h++) if (!wcmpn(h, n, l, ci)) return (WCHAR *)h; return NULL; }
static WCHAR *WINAPI StrStrW(const WCHAR *h, const WCHAR *n) { return h && n ? wstrstr(h, n, false) : NULL; }
static WCHAR *WINAPI StrStrIW(const WCHAR *h, const WCHAR *n) { return h && n ? wstrstr(h, n, true) : NULL; }
static char *WINAPI StrStrA(const char *h, const char *n) { return h && n ? strstr(h, n) : NULL; }
static char *WINAPI StrStrIA(const char *h, const char *n) { if (!h || !n) return NULL; size_t l = strlen(n); for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h; return NULL; }
static BOOL WINAPI StrTrimW(WCHAR *s, const WCHAR *trim) {
    int l = wlen(s), a = 0, b = l;
    while (a < b && StrChrW(trim, s[a])) a++;
    while (b > a && StrChrW(trim, s[b - 1])) b--;
    if (a == 0 && b == l) return 0;
    memmove(s, s + a, (b - a) * 2); s[b - a] = 0; return 1;
}
static BOOL WINAPI StrTrimA(char *s, const char *trim) {
    int l = strlen(s), a = 0, b = l;
    while (a < b && strchr(trim, s[a])) a++;
    while (b > a && strchr(trim, s[b - 1])) b--;
    if (a == 0 && b == l) return 0;
    memmove(s, s + a, b - a); s[b - a] = 0; return 1;
}
static int WINAPI StrToIntW(const WCHAR *s) { int sg = 1, v = 0; if (*s == '-') { sg = -1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v * sg; }
static int WINAPI StrToIntA(const char *s) { return atoi(s); }
static BOOL WINAPI StrToIntExW(const WCHAR *s, DWORD f, int *out) {
    while (*s == ' ' || *s == '\t') s++;
    int sg = 1; if (*s == '-') { sg = -1; s++; } else if (*s == '+') s++;
    int v = 0; bool any = false;
    if ((f & 1) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; for (; isxdigit(*s & 0x7F) && *s < 128; s++) { v = v * 16 + (isdigit(*s) ? *s - '0' : tolower(*s) - 'a' + 10); any = true; } }
    else for (; *s >= '0' && *s <= '9'; s++) { v = v * 10 + (*s - '0'); any = true; }
    *out = v * sg; return any;
}
static BOOL WINAPI StrToIntExA(const char *s, DWORD f, int *out) { WCHAR w[64]; a2w(s, w, 64); return StrToIntExW(w, f, out); }
static void fmt_bytes(int64_t n, char *b, int cap) {
    if (n < 1024) snprintf(b, cap, "%d bytes", (int)n);
    else { const char *u[] = { "KB", "MB", "GB", "TB" }; double v = n / 1024.0; int i = 0; while (v >= 1024 && i < 3) { v /= 1024; i++; }
        int w = (int)v, f = (int)((v - w) * 100);
        if (v < 10) snprintf(b, cap, "%d.%02d %s", w, f, u[i]); else if (v < 100) snprintf(b, cap, "%d.%d %s", w, f / 10, u[i]); else snprintf(b, cap, "%d %s", w, u[i]); }
}
static WCHAR *WINAPI StrFormatByteSizeW(int64_t n, WCHAR *b, UINT cap) { char a[64]; fmt_bytes(n, a, sizeof a); a2w(a, b, cap); return b; }
static char *WINAPI StrFormatByteSizeA(DWORD n, char *b, UINT cap) { fmt_bytes(n, b, cap); return b; }
static char *WINAPI StrFormatByteSize64A(int64_t n, char *b, UINT cap) { fmt_bytes(n, b, cap); return b; }
static WCHAR *WINAPI StrFormatKBSizeW(int64_t n, WCHAR *b, UINT cap) { char a[64]; snprintf(a, sizeof a, "%lld KB", (long long)((n + 1023) / 1024)); a2w(a, b, cap); return b; }
/* STRRET: uType (WSTR 0, OFFSET 1, CSTR 2) then a union */
typedef struct { UINT type; union { WCHAR *wstr; UINT off; char cstr[260]; }; } STRRET;
static LONG WINAPI StrRetToBufW(STRRET *s, const void *pidl, WCHAR *b, UINT n) {
    if (s->type == 0) { StrCpyNW(b, s->wstr, n); w32_free(P, s->wstr); }
    else if (s->type == 2) a2w(s->cstr, b, n);
    else if (s->type == 1 && pidl) a2w((const char *)pidl + s->off, b, n);
    return S_OK;
}
static LONG WINAPI StrRetToStrW(STRRET *s, const void *pidl, WCHAR **out) { WCHAR b[MAXP]; StrRetToBufW(s, pidl, b, MAXP); *out = w32_alloc(P, (wlen(b) + 1) * 2); StrCpyW(*out, b); return S_OK; }
static LONG WINAPI SHStrDupW(const WCHAR *s, WCHAR **out) { int l = wlen(s); *out = w32_alloc(P, (l + 1) * 2); memcpy(*out, s, (l + 1) * 2); return S_OK; }
static LONG WINAPI SHStrDupA(const char *s, WCHAR **out) { int l = strlen(s); *out = w32_alloc(P, (l + 1) * 2); a2w(s, *out, l + 1); return S_OK; }
static int WINAPI wvnsprintfW(WCHAR *b, int n, const WCHAR *fmt, w32_va_list ap) {
    void *fn = NULL; w32_lookup("msvcrt.dll", "_vsnwprintf", &fn);
    int r = ((int (CDECL *)(WCHAR *, size_t, const WCHAR *, w32_va_list))fn)(b, n, fmt, ap);
    if (r < 0 && n > 0) { b[n - 1] = 0; r = n - 1; }
    return r;
}
static int CDECL wnsprintfW(WCHAR *b, int n, const WCHAR *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = wvnsprintfW(b, n, fmt, ap); w32_va_end(ap); return r; }
static int WINAPI wvnsprintfA(char *b, int n, const char *fmt, w32_va_list ap) { int r = w32_vformat(b, n, fmt, ap); return r >= n ? n - 1 : r; }
static int CDECL wnsprintfA(char *b, int n, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = wvnsprintfA(b, n, fmt, ap); w32_va_end(ap); return r; }
/* URLs: percent-escape what is not allowed, and back */
static LONG WINAPI UrlEscapeW(const WCHAR *in, WCHAR *out, DWORD *n, DWORD f) {
    WCHAR t[2048]; DWORD k = 0;
    for (; *in && k < 2040; in++) {
        WCHAR c = *in;
        bool keep = (c < 128 && (isalnum(c) || strchr("-_.~!*'();:@&=+$,/?#[]", c)));
        if (f & 0x00040000) keep = keep && c != '#' && c != '?';   /* URL_ESCAPE_SEGMENT_ONLY-ish */
        if (keep) t[k++] = c; else { static const char hx[] = "0123456789ABCDEF"; t[k++] = '%'; t[k++] = hx[(c >> 4) & 15]; t[k++] = hx[c & 15]; }
    }
    t[k] = 0;
    if (*n <= k) { *n = k + 1; return (LONG)0x80004003; }
    memcpy(out, t, (k + 1) * 2); *n = k;
    return S_OK;
}
static int hexv(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static LONG WINAPI UrlUnescapeW(WCHAR *in, WCHAR *out, DWORD *n, DWORD f) {
    WCHAR t[2048]; DWORD k = 0;
    for (WCHAR *s = in; *s && k < 2047; s++) { if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { t[k++] = hexv(s[1]) << 4 | hexv(s[2]); s += 2; } else t[k++] = *s; }
    t[k] = 0;
    if (f & 0x00100000) { memcpy(in, t, (k + 1) * 2); return S_OK; }       /* URL_UNESCAPE_INPLACE */
    if (!out || !n || *n <= k) { if (n) *n = k + 1; return (LONG)0x80004003; }
    memcpy(out, t, (k + 1) * 2); *n = k;
    return S_OK;
}
static DWORD WINAPI SHGetObjectCompatFlags(void *u, const void *c) { return 0; }
static LONG WINAPI SHCreateStreamOnFileW(const WCHAR *f, DWORD m, void **out) { *out = NULL; return E_NOTIMPL; }
static LONG WINAPI SHCreateStreamOnFileEx(const WCHAR *f, DWORD m, DWORD a, BOOL c, void *t, void **out) { *out = NULL; return E_NOTIMPL; }
static DWORD WINAPI SHAnsiToUnicode(const char *a, WCHAR *w, int n) { return a2w(a, w, n) + 1; }
static DWORD WINAPI SHUnicodeToAnsi(const WCHAR *w, char *a, int n) { w2a(w, a, n); return strlen(a) + 1; }
static COLORREF WINAPI ColorHLSToRGB(WORD h, WORD l, WORD s) {
    if (!s) { int v = l * 255 / 240; return v | v << 8 | v << 16; }
    int m2 = l <= 120 ? (l * (240 + s) + 120) / 240 : l + s - (l * s + 120) / 240, m1 = 2 * l - m2;
    int c[3]; int hh[3] = { h + 80, h, h - 80 };
    for (int i = 0; i < 3; i++) {
        int x = hh[i]; if (x < 0) x += 240; if (x > 240) x -= 240;
        int v = x < 40 ? m1 + ((m2 - m1) * x + 20) / 40 : x < 120 ? m2 : x < 160 ? m1 + ((m2 - m1) * (160 - x) + 20) / 40 : m1;
        c[i] = (v * 255 + 120) / 240;
    }
    return c[0] | c[1] << 8 | c[2] << 16;
}
static void WINAPI ColorRGBToHLS(COLORREF rgb, WORD *h, WORD *l, WORD *s) {
    int r = rgb & 255, g = (rgb >> 8) & 255, b = (rgb >> 16) & 255, mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b));
    *l = (WORD)(((mx + mn) * 240 + 255) / 510);
    if (mx == mn) { *s = 0; *h = 160; return; }
    int d = mx - mn;
    *s = (WORD)(*l <= 120 ? (d * 240 + (mx + mn) / 2) / (mx + mn) : (d * 240 + (510 - mx - mn) / 2) / (510 - mx - mn));
    int hh = r == mx ? (40 * (g - b)) / d : g == mx ? 80 + (40 * (b - r)) / d : 160 + (40 * (r - g)) / d;
    if (hh < 0) hh += 240;
    *h = (WORD)hh;
}

/* ================================================================ shell32 */
static const char *csidl_path(int c, char *buf, int cap) {
    switch (c & 0xFF) {
    case 0x00: case 0x10: case 0x19: return USER_DESKTOP;
    case 0x05: case 0x2E: return USER_DOCS;
    case 0x0D: snprintf(buf, cap, "%s\\My Music", USER_DOCS); return buf;
    case 0x27: snprintf(buf, cap, "%s\\My Pictures", USER_DOCS); return buf;
    case 0x0E: snprintf(buf, cap, "%s\\My Videos", USER_DOCS); return buf;
    case 0x1A: return USER_HOME "\\Application Data";
    case 0x1C: return USER_HOME "\\Local Settings\\Application Data";
    case 0x23: return "C:\\Documents and Settings\\All Users\\Application Data";
    case 0x28: return USER_HOME;
    case 0x24: return "C:\\WRLD";
    case 0x25: case 0x29: return "C:\\WRLD\\System32";
    case 0x26: case 0x2A: return "C:\\Program Files";
    case 0x2B: case 0x2C: return "C:\\Program Files\\Common Files";
    case 0x14: return "C:\\WRLD\\Fonts";
    case 0x06: return USER_HOME "\\Favorites";
    case 0x07: case 0x18: return USER_HOME "\\Start Menu\\Programs\\Startup";
    case 0x08: return USER_HOME "\\Recent";
    case 0x09: return USER_HOME "\\SendTo";
    case 0x0B: case 0x16: return USER_HOME "\\Start Menu";
    case 0x02: case 0x17: return USER_HOME "\\Start Menu\\Programs";
    case 0x15: return USER_HOME "\\Templates";
    case 0x20: return USER_HOME "\\Local Settings\\Temporary Internet Files";
    case 0x21: return USER_HOME "\\Cookies";
    case 0x22: return USER_HOME "\\Local Settings\\History";
    case 0x2F: return "C:\\Documents and Settings\\All Users\\Start Menu\\Programs\\Administrative Tools";
    }
    return NULL;
}
static void mkdirs(const char *path) {
    char b[300]; strlcpy(b, path, sizeof b);
    for (char *c = b + 3; ; c++) if (*c == '\\' || !*c) { char t = *c; *c = 0; if (!vfs_resolve(b, NULL)) vfs_create(b, NULL, true); *c = t; if (!t) break; }
}
static LONG folder_path(int csidl, char *out, int cap) {
    char tmp[300]; const char *p = csidl_path(csidl, tmp, sizeof tmp);
    if (!p) return E_INVALIDARG;
    if (csidl & 0x8000) mkdirs(p);                        /* CSIDL_FLAG_CREATE */
    strlcpy(out, p, cap);
    return S_OK;
}
static LONG WINAPI SHGetFolderPathA(HWND h, int csidl, HANDLE t, DWORD f, char *out) { return folder_path(csidl, out, MAXP); }
static LONG WINAPI SHGetFolderPathW(HWND h, int csidl, HANDLE t, DWORD f, WCHAR *out) { char a[MAXP]; LONG r = folder_path(csidl, a, sizeof a); if (!r) a2w(a, out, MAXP); else out[0] = 0; return r; }
static LONG WINAPI SHGetFolderPathAndSubDirW(HWND h, int csidl, HANDLE t, DWORD f, const WCHAR *sub, WCHAR *out) {
    char a[MAXP], s[MAXP]; LONG r = folder_path(csidl, a, sizeof a); if (r) return r;
    if (sub) { w2a(sub, s, sizeof s); strlcat(a, "\\", sizeof a); strlcat(a, s, sizeof a); if (csidl & 0x8000) mkdirs(a); }
    a2w(a, out, MAXP); return S_OK;
}
static BOOL WINAPI SHGetSpecialFolderPathA(HWND h, char *out, int csidl, BOOL create) { return folder_path(csidl | (create ? 0x8000 : 0), out, MAXP) == S_OK; }
static BOOL WINAPI SHGetSpecialFolderPathW(HWND h, WCHAR *out, int csidl, BOOL create) { char a[MAXP]; if (folder_path(csidl | (create ? 0x8000 : 0), a, sizeof a)) return 0; a2w(a, out, MAXP); return 1; }
/* known folders by the first 32 bits of their GUID */
static int known_csidl(const uint32_t *g) {
    switch (g[0]) {
    case 0x3EB685DB: return 0x1A; case 0xF1B32785: return 0x1C; case 0xA520A1A4: return 0x1C; case 0xFDD39AD0: return 0x05; case 0xB4BFCC3A: return 0x10;
    case 0x905E63B6: case 0x7C5A40EF: case 0x6D809377: return 0x26; case 0x5E6C858F: return 0x28; case 0xF38BF404: return 0x24; case 0x1AC14E77: case 0xD65231B0: return 0x25;
    case 0x62AB5D82: return 0x23; case 0xFD228CB7: return 0x14; case 0x4BD8D571: return 0x0D; case 0x33E28130: return 0x27; case 0x18989B1D: return 0x0E;
    case 0x1777F761: return 0x06; case 0xAE50C081: return 0x08; case 0x625B53C3: return 0x0B; case 0xA77F5D77: return 0x02; case 0xB97D20BB: return 0x07;
    case 0xA63293E8: return 0x15; case 0x374DE290: return 0x05;      /* Downloads: the documents folder */
    }
    return -1;
}
static LONG WINAPI SHGetKnownFolderPath(const uint32_t *guid, DWORD f, HANDLE t, WCHAR **out) {
    int c = guid ? known_csidl(guid) : -1;
    char a[MAXP];
    if (c < 0 || folder_path(c | ((f & 0x8000) ? 0x8000 : 0), a, sizeof a)) { *out = NULL; return (LONG)0x80070002; }
    int l = strlen(a); *out = w32_alloc(P, (l + 1) * 2); a2w(a, *out, l + 1);
    return S_OK;
}
/* item ID lists: one item holding the path, then a zero WORD */
static void *make_pidl(const char *path) {
    int l = strlen(path) + 1;
    uint8_t *p = w32_alloc(P, l + 4);
    *(WORD *)p = (WORD)(l + 2); memcpy(p + 2, path, l); *(WORD *)(p + 2 + l) = 0;
    return p;
}
static const char *pidl_path(const void *pidl) { return pidl && *(const WORD *)pidl > 2 ? (const char *)pidl + 2 : "C:\\"; }
static LONG WINAPI SHGetSpecialFolderLocation(HWND h, int csidl, void **out) { char a[MAXP]; if (folder_path(csidl, a, sizeof a)) { *out = NULL; return E_INVALIDARG; } *out = make_pidl(a); return S_OK; }
static LONG WINAPI SHGetFolderLocation(HWND h, int csidl, HANDLE t, DWORD r, void **out) { return SHGetSpecialFolderLocation(h, csidl, out); }
static LONG WINAPI SHGetKnownFolderIDList(const uint32_t *g, DWORD f, HANDLE t, void **out) { int c = known_csidl(g); return c < 0 ? (LONG)0x80070002 : SHGetSpecialFolderLocation(0, c, out); }
static BOOL WINAPI SHGetPathFromIDListA(const void *pidl, char *out) { strlcpy(out, pidl_path(pidl), MAXP); return 1; }
static BOOL WINAPI SHGetPathFromIDListW(const void *pidl, WCHAR *out) { a2w(pidl_path(pidl), out, MAXP); return 1; }
static BOOL WINAPI SHGetPathFromIDListEx(const void *pidl, WCHAR *out, DWORD n, int f) { a2w(pidl_path(pidl), out, n); return 1; }
static void *WINAPI ILCreateFromPathW(const WCHAR *p) { char a[MAXP]; w2a(p, a, sizeof a); return make_pidl(a); }
static void *WINAPI ILCreateFromPathA(const char *p) { return make_pidl(p); }
static void WINAPI ILFree(void *p) { if (p) w32_free(P, p); }
static void *WINAPI ILClone(const void *p) { return make_pidl(pidl_path(p)); }
static UINT WINAPI ILGetSize(const void *p) { return p ? *(const WORD *)p + 2 : 0; }
static LONG WINAPI SHParseDisplayName(const WCHAR *n, void *bc, void **out, DWORD a, DWORD *ao) { char s[MAXP]; w2a(n, s, sizeof s); *out = make_pidl(s); if (ao) *ao = 0; return S_OK; }
static LONG WINAPI SHGetDataFromIDListW(void *sf, const void *pidl, int fmt, void *out, int n) { return E_INVALIDARG; }
/* IMalloc on the program heap */
static LONG WINAPI im_qi(void *t, const void *iid, void **o) { *o = t; return S_OK; }
static ULONG_PTR WINAPI im_ref(void *t) { return 1; }
static void *WINAPI im_alloc(void *t, SIZE_T n) { return w32_alloc(P, n ? n : 1); }
static void *WINAPI im_realloc(void *t, void *p, SIZE_T n) { return w32_realloc(P, p, n); }
static void WINAPI im_free(void *t, void *p) { if (p) w32_free(P, p); }
static SIZE_T WINAPI im_size(void *t, void *p) { return w32_msize(p); }
static int WINAPI im_did(void *t, void *p) { return -1; }
static void WINAPI im_min(void *t) {}
static void *const imalloc_vt[] = { (void *)im_qi, (void *)im_ref, (void *)im_ref, (void *)im_alloc, (void *)im_realloc, (void *)im_free, (void *)im_size, (void *)im_did, (void *)im_min };
static struct { void *const *vt; } imalloc = { imalloc_vt };
static LONG WINAPI SHGetMalloc(void **out) { *out = &imalloc; return S_OK; }
static LONG WINAPI CoGetMalloc2(DWORD c, void **out) { *out = &imalloc; return S_OK; }
static LONG WINAPI SHGetDesktopFolder(void **out) { *out = NULL; return E_NOTIMPL; }
/* the folder picker: the system's file dialog, opened on the folder (its path is the answer) */
typedef struct { DWORD x; } BROWSEINFO_unused;
static void *browse(HWND owner, const char *title) {
    void *fn = NULL; w32_lookup("comdlg32.dll", "GetSaveFileNameA", &fn);
    char file[MAXP] = "(this folder)";
    struct { DWORD size; HWND owner; HINSTANCE inst; const char *filter; char *cust; DWORD ncust, findex; char *file; DWORD nfile; char *ftitle; DWORD nftitle;
             const char *initdir, *title; DWORD flags; WORD fo, fe; const char *defext; LPARAM cd; void *hook; const char *tpl; void *r1; DWORD r2, fx; } o;
    memset(&o, 0, sizeof o); o.size = sizeof o; o.owner = owner; o.file = file; o.nfile = sizeof file; o.title = title && *title ? title : "Browse For Folder"; o.initdir = USER_DOCS;
    if (!fn || !((BOOL (WINAPI *)(void *))fn)(&o)) return NULL;
    char dir[MAXP]; strlcpy(dir, file, sizeof dir);
    vnode_t *v = vfs_resolve(dir, NULL);
    if (!v || !v->dir) { char *s = strrchr(dir, '\\'); if (s && s > dir + 2) *s = 0; else if (s) s[1] = 0; }
    return make_pidl(dir);
}
typedef struct { HWND owner; void *root; char *display; const char *title; UINT flags; void *fn; LPARAM lp; int image; } BIA;
typedef struct { HWND owner; void *root; WCHAR *display; const WCHAR *title; UINT flags; void *fn; LPARAM lp; int image; } BIW;
static void *WINAPI SHBrowseForFolderA(BIA *b) { void *r = browse(b->owner, b->title); if (r && b->display) strlcpy(b->display, path_basename(pidl_path(r)), MAXP); return r; }
static void *WINAPI SHBrowseForFolderW(BIW *b) { char t[128]; w2a(b->title, t, sizeof t); void *r = browse(b->owner, t); if (r && b->display) a2w(path_basename(pidl_path(r)), b->display, MAXP); return r; }
/* CommandLineToArgvW: one block, the pointers then the strings (freed with LocalFree) */
static WCHAR **WINAPI CommandLineToArgvW(const WCHAR *cmd, int *argc) {
    int n = wlen(cmd), cnt = 0;
    WCHAR *tmp = kmalloc((n + 2) * 2 * 2); int *starts = kmalloc((n + 2) * sizeof(int)); int k = 0;
    const WCHAR *s = cmd;
    if (!*s) { tmp[k++] = 0; starts[cnt++] = 0; }        /* no command line: the program's own path */
    bool first = true;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        starts[cnt++] = k;
        bool q = false;
        if (first) {                                     /* the program name: quotes only delimit */
            while (*s && (q || (*s != ' ' && *s != '\t'))) { if (*s == '"') q = !q; else tmp[k++] = *s; s++; }
        } else while (*s && (q || (*s != ' ' && *s != '\t'))) {
            if (*s == '\\') { int bs = 0; while (*s == '\\') { bs++; s++; } if (*s == '"') { for (int i = 0; i < bs / 2; i++) tmp[k++] = '\\'; if (bs & 1) { tmp[k++] = '"'; s++; } } else for (int i = 0; i < bs; i++) tmp[k++] = '\\'; continue; }
            if (*s == '"') { if (q && s[1] == '"') { tmp[k++] = '"'; s += 2; continue; } q = !q; s++; continue; }
            tmp[k++] = *s++;
        }
        tmp[k++] = 0; first = false;
    }
    if (!*cmd) { char a[MAXP]; strlcpy(a, P->path, sizeof a); k = 0; for (int i = 0; a[i]; i++) tmp[k++] = (uint8_t)a[i]; tmp[k++] = 0; }
    size_t ptrs = (cnt + 1) * sizeof(WCHAR *);
    uint8_t *blk = ((void *(WINAPI *)(UINT, SIZE_T))K("LocalAlloc"))(0, ptrs + k * 2);
    WCHAR **argv = (WCHAR **)blk; WCHAR *str = (WCHAR *)(blk + ptrs);
    memcpy(str, tmp, k * 2);
    for (int i = 0; i < cnt; i++) argv[i] = str + starts[i];
    argv[cnt] = NULL;
    kfree(tmp); kfree(starts);
    if (argc) *argc = cnt;
    return argv;
}
/* SHELLEXECUTEINFO: the fields up to hInstApp are what these use */
typedef struct { DWORD size; ULONG mask; HWND hwnd; const void *verb, *file, *params, *dir; int show; HINSTANCE inst; void *idl; const void *cls; void *key; DWORD hotkey; HANDLE icon; HANDLE proc; } SEI;
static uintptr_t exec(HWND h, const char *verb, const char *file, const char *params, const char *dir, int show, HANDLE *proc) {
    if (proc) *proc = 0;
    if (!file || !*file) return 2;
    if (strstr(file, "://") || !strncasecmp(file, "mailto:", 7)) { void *fn = NULL; w32_lookup("shell32.dll", "ShellExecuteA", &fn); return fn ? ((uintptr_t (WINAPI *)(HWND, const char *, const char *, const char *, const char *, int))fn)(h, verb, file, params, dir, show) : 2; }
    const char *ext = strrchr(path_basename(file), '.');
    if (ext && (!strcasecmp(ext, ".exe") || !strcasecmp(ext, ".com")) && (!verb || !strcasecmp(verb, "open") || !strcasecmp(verb, "runas"))) {
        char cmd[1024]; snprintf(cmd, sizeof cmd, "\"%s\"%s%s", file, params && *params ? " " : "", params ? params : "");
        struct { DWORD cb; uint8_t rest[100]; } si; memset(&si, 0, sizeof si); si.cb = sizeof si;
        struct { HANDLE p, t; DWORD pid, tid; } pi;
        if (!((BOOL (WINAPI *)(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, void *, void *))K("CreateProcessA"))(NULL, cmd, NULL, NULL, 0, 0, NULL, dir, &si, &pi)) return 2;
        if (proc) *proc = pi.p;
        return 42;
    }
    void *fn = NULL; w32_lookup("shell32.dll", "ShellExecuteA", &fn);
    return fn ? ((uintptr_t (WINAPI *)(HWND, const char *, const char *, const char *, const char *, int))fn)(h, verb, file, params, dir, show) : 2;
}
static uintptr_t WINAPI ShellExecuteW(HWND h, const WCHAR *verb, const WCHAR *file, const WCHAR *params, const WCHAR *dir, int show) {
    char v[32], f[MAXP * 2], p[1024], d[MAXP]; w2a(verb, v, sizeof v); w2a(file, f, sizeof f); w2a(params, p, sizeof p); w2a(dir, d, sizeof d);
    return exec(h, verb ? v : NULL, f, params ? p : NULL, dir ? d : NULL, show, NULL);
}
static BOOL WINAPI ShellExecuteExA(SEI *e) {
    HANDLE pr; uintptr_t r = exec(e->hwnd, e->verb, e->file, e->params, e->dir, e->show, &pr);
    e->inst = r; if (e->mask & 0x40) e->proc = pr;
    if (r <= 32) { set_err(2); return 0; }
    return 1;
}
static BOOL WINAPI ShellExecuteExW(SEI *e) {
    char v[32], f[MAXP * 2], p[1024], d[MAXP]; w2a(e->verb, v, sizeof v); w2a(e->file, f, sizeof f); w2a(e->params, p, sizeof p); w2a(e->dir, d, sizeof d);
    HANDLE pr; uintptr_t r = exec(e->hwnd, e->verb ? v : NULL, f, e->params ? p : NULL, e->dir ? d : NULL, e->show, &pr);
    e->inst = r; if (e->mask & 0x40) e->proc = pr;
    if (r <= 32) { set_err(2); return 0; }
    return 1;
}
static uintptr_t WINAPI FindExecutableA(const char *f, const char *d, char *out) { out[0] = 0; return 31; }
static uintptr_t WINAPI FindExecutableW(const WCHAR *f, const WCHAR *d, WCHAR *out) { out[0] = 0; return 31; }
static int WINAPI SHCreateDirectoryExA(HWND h, const char *p, void *sa) {
    if (vfs_resolve(p, NULL)) return 183;
    mkdirs(p); return vfs_resolve(p, NULL) ? 0 : 3;
}
static int WINAPI SHCreateDirectoryExW(HWND h, const WCHAR *p, void *sa) { char a[MAXP]; w2a(p, a, sizeof a); return SHCreateDirectoryExA(h, a, sa); }
static int WINAPI SHCreateDirectory(HWND h, const WCHAR *p) { return SHCreateDirectoryExW(h, p, NULL); }
/* SHFILEOPSTRUCT: delete, rename, move and copy of double-NUL lists (first file only for move/copy/rename) */
typedef struct { HWND hwnd; UINT func; const void *from, *to; WORD flags; BOOL aborted; void *map; const void *title; } SHFOS;
static int file_op(UINT func, const char *from, const char *to) {
    BOOL (WINAPI *del)(const char *) = K("DeleteFileA");
    for (const char *f = from; *f; f += strlen(f) + 1) {
        switch (func) {
        case 3: {                                          /* FO_DELETE (files, and empty or full folders) */
            vnode_t *v = vfs_resolve(f, NULL); if (!v) return 2;
            if (v->dir) { while (v->child) { char c[MAXP]; vfs_path(v->child, c, sizeof c); char l[MAXP + 2]; memset(l, 0, sizeof l); strlcpy(l, c, MAXP); file_op(3, l, NULL); } vfs_delete(v); }
            else del(f);
            break; }
        case 4: case 1: {                                  /* FO_RENAME, FO_MOVE */
            char dst[MAXP]; strlcpy(dst, to, sizeof dst);
            vnode_t *d = vfs_resolve(dst, NULL); if (d && d->dir && func == 1) { strlcat(dst, "\\", sizeof dst); strlcat(dst, path_basename(f), sizeof dst); }
            if (!((BOOL (WINAPI *)(const char *, const char *))K("MoveFileA"))(f, dst)) return 2;
            return 0; }
        case 2: {                                          /* FO_COPY */
            char dst[MAXP]; strlcpy(dst, to, sizeof dst);
            vnode_t *d = vfs_resolve(dst, NULL); if (d && d->dir) { strlcat(dst, "\\", sizeof dst); strlcat(dst, path_basename(f), sizeof dst); }
            if (!((BOOL (WINAPI *)(const char *, const char *, BOOL))K("CopyFileA"))(f, dst, 0)) return 2;
            break; }
        default: return 0x78;
        }
    }
    return 0;
}
static int WINAPI SHFileOperationA(SHFOS *o) { o->aborted = 0; return file_op(o->func, o->from, o->to ? o->to : ""); }
static int WINAPI SHFileOperationW(SHFOS *o) {
    const WCHAR *w = o->from; int n = 0; while (w[n] || w[n + 1]) n++;
    char *a = kmalloc(n + 3); for (int i = 0; i < n + 2; i++) a[i] = w[i] < 256 ? (char)w[i] : '?';
    char to[MAXP] = ""; w2a(o->to, to, sizeof to);
    o->aborted = 0; int r = file_op(o->func, a, to); kfree(a); return r;
}
/* SHFILEINFO: hIcon, iIcon, dwAttributes, szDisplayName[260], szTypeName[80] */
typedef struct { HANDLE icon; int iicon; DWORD attr; char name[260]; char type[80]; } SFIA;
typedef struct { HANDLE icon; int iicon; DWORD attr; WCHAR name[260]; WCHAR type[80]; } SFIW;
static void file_type(const char *path, bool dir, char *t, int cap) {
    const char *e = strrchr(path_basename(path), '.');
    if (dir) strlcpy(t, "File Folder", cap);
    else if (e && !strcasecmp(e, ".exe")) strlcpy(t, "Application", cap);
    else if (e && e[1]) { char u[16]; int i = 0; for (const char *c = e + 1; *c && i < 15; c++) u[i++] = toupper(*c); u[i] = 0; snprintf(t, cap, "%s File", u); }
    else strlcpy(t, "File", cap);
}
static uintptr_t WINAPI SHGetFileInfoA(const char *path, DWORD attr, SFIA *fi, UINT n, UINT fl) {
    if (fl & 0x8) path = pidl_path(path);                  /* SHGFI_PIDL */
    vnode_t *v = (fl & 0x10) ? NULL : vfs_resolve(path, NULL);          /* SHGFI_USEFILEATTRIBUTES */
    bool dir = v ? v->dir : (attr & 0x10);
    if (!v && !(fl & 0x10)) return 0;
    if (fi) {
        memset(fi, 0, sizeof *fi);
        strlcpy(fi->name, path_basename(path), sizeof fi->name);
        file_type(path, dir, fi->type, sizeof fi->type);
        fi->attr = dir ? 0x20000000 | 0x40000000 : 0x40000000;
        fi->iicon = dir ? 3 : 0;
    }
    if (fl & 0x2000) {                                    /* SHGFI_EXETYPE */
        const char *e = strrchr(path, '.'); return e && !strcasecmp(e, ".exe") ? 0x4550 | (0x0400 << 16) : 0;
    }
    return 1;
}
static uintptr_t WINAPI SHGetFileInfoW(const WCHAR *path, DWORD attr, SFIW *fi, UINT n, UINT fl) {
    char a[MAXP]; if (fl & 0x8) strlcpy(a, pidl_path(path), sizeof a); else w2a(path, a, sizeof a);
    SFIA x; uintptr_t r = SHGetFileInfoA(a, attr, fi ? &x : NULL, sizeof x, fl & ~0x8);
    if (fi && r) { fi->icon = x.icon; fi->iicon = x.iicon; fi->attr = x.attr; a2w(x.name, fi->name, 260); a2w(x.type, fi->type, 80); }
    return r;
}
static HANDLE WINAPI ExtractIconA(HINSTANCE h, const char *f, UINT i) { return 0; }
static HANDLE WINAPI ExtractIconW(HINSTANCE h, const WCHAR *f, UINT i) { return 0; }
static UINT WINAPI ExtractIconExW(const WCHAR *f, int i, HANDLE *l, HANDLE *s, UINT n) { if (l) *l = 0; if (s) *s = 0; return 0; }
static UINT WINAPI ExtractIconExA(const char *f, int i, HANDLE *l, HANDLE *s, UINT n) { if (l) *l = 0; if (s) *s = 0; return 0; }
static void WINAPI DragAcceptFiles(HWND h, BOOL a) {}
static UINT WINAPI DragQueryFileA(HANDLE d, UINT i, char *b, UINT n) { return 0; }
static UINT WINAPI DragQueryFileW(HANDLE d, UINT i, WCHAR *b, UINT n) { return 0; }
static BOOL WINAPI DragQueryPoint(HANDLE d, void *pt) { return 0; }
static void WINAPI DragFinish(HANDLE d) {}
static BOOL WINAPI IsUserAnAdmin(void) { return 1; }
static void WINAPI SHAddToRecentDocs(UINT f, const void *p) {}
static void WINAPI SHChangeNotify(LONG e, UINT f, const void *a, const void *b) {}
static uintptr_t WINAPI SHAppBarMessage(DWORD msg, void *d) {
    if (msg == 5 && d) {                                  /* ABM_GETTASKBARPOS: bottom edge */
        int32_t *r = (int32_t *)((uint8_t *)d + 4 + 2 * sizeof(uintptr_t) + 4);
        r[0] = 0; r[1] = SCREEN_H - T.taskbar_h; r[2] = SCREEN_W; r[3] = SCREEN_H; *(UINT *)((uint8_t *)d + 4 + 2 * sizeof(uintptr_t)) = 3;
        return 1;
    }
    return msg == 4 ? 2 : 0;                              /* ABM_GETSTATE: always on top */
}
static BOOL WINAPI Shell_NotifyIconW(DWORD m, void *d) { return 1; }
static BOOL WINAPI Shell_NotifyIconA(DWORD m, void *d) { return 1; }
static int WINAPI ShellAboutW(HWND h, const WCHAR *app, const WCHAR *other, HANDLE icon) {
    char a[200], o[300], t[600]; w2a(app, a, sizeof a); w2a(other, o, sizeof o);
    char *hash = strchr(a, '#'); const char *title = a; if (hash) { *hash = 0; title = hash + 1; }
    snprintf(t, sizeof t, "%s\n" OS_NAME "\n\n%s", title, o);
    void *fn = NULL; w32_lookup("user32.dll", "MessageBoxA", &fn);
    char cap[220]; snprintf(cap, sizeof cap, "About %s", a);
    return fn ? ((int (WINAPI *)(HWND, const char *, const char *, UINT))fn)(h, t, cap, 0x40) : 1;
}
static int WINAPI ShellAboutA(HWND h, const char *app, const char *other, HANDLE icon) { WCHAR a[200], o[300]; a2w(app ? app : "", a, 200); a2w(other ? other : "", o, 300); return ShellAboutW(h, a, o, icon); }
static LONG WINAPI SetCurrentProcessExplicitAppUserModelID(const WCHAR *id) { return S_OK; }
static LONG WINAPI SHGetStockIconInfo(int id, UINT f, void *s) { return E_INVALIDARG; }
static int WINAPI PickIconDlg(HWND h, WCHAR *f, UINT n, int *i) { return 0; }
static LONG WINAPI SHOpenFolderAndSelectItems(const void *pidl, UINT n, const void **a, DWORD f) { void *fn = NULL; w32_lookup("shell32.dll", "ShellExecuteA", &fn); if (fn) ((uintptr_t (WINAPI *)(HWND, const char *, const char *, const char *, const char *, int))fn)(0, "open", pidl_path(pidl), NULL, NULL, 1); return S_OK; }

/* ================================================================ version.dll: VS_VERSIONINFO from the file */
/* the RT_VERSION resource of a PE file on disk (any architecture) */
static const uint8_t *version_resource(const char *file, uint32_t *len) {
    char path[MAXP];
    if (!file || !*file) return NULL;
    if (!strchr(file, '\\') && !strchr(file, '/') && !strchr(file, ':')) {
        w32_mod_t *m = w32_mod_by_name(P, file);
        if (m) strlcpy(path, m->path, sizeof path);
        else if (!strcasecmp(file, path_basename(P->path))) strlcpy(path, P->path, sizeof path);
        else path_join(path, sizeof path, P->cwd, file);
    } else strlcpy(path, file, sizeof path);
    vnode_t *v = vfs_resolve(path, NULL);
    if (!v || v->dir || v->size < 0x100) return NULL;
    const uint8_t *d = v->data; uint32_t n = v->size;
    if (d[0] != 'M' || d[1] != 'Z') return NULL;
    uint32_t pe = *(const uint32_t *)(d + 0x3C);
    if (pe + 24 > n || memcmp(d + pe, "PE\0\0", 4)) return NULL;
    uint16_t nsec = *(const uint16_t *)(d + pe + 6), osz = *(const uint16_t *)(d + pe + 20);
    const uint8_t *opt = d + pe + 24; bool is64 = *(const uint16_t *)opt == 0x20B;
    const uint8_t *dd = opt + (is64 ? 112 : 96);
    uint32_t rva = *(const uint32_t *)(dd + 16);
    if (!rva) return NULL;
    const uint8_t *sec = opt + osz;
    #define OFF(r) ({ uint32_t _o = 0; for (int _i = 0; _i < nsec; _i++) { const uint8_t *_s = sec + _i * 40; uint32_t _va = *(const uint32_t *)(_s + 12), _vs = MAX(*(const uint32_t *)(_s + 8), *(const uint32_t *)(_s + 16)); if ((r) >= _va && (r) < _va + _vs) { _o = (r) - _va + *(const uint32_t *)(_s + 20); break; } } _o; })
    uint32_t root = OFF(rva);
    if (!root || root + 16 > n) return NULL;
    /* type 16, then the first name and the first language */
    uint32_t dir = root;
    uint32_t want[3] = { 16, 0xFFFFFFFF, 0xFFFFFFFF };
    for (int lvl = 0; lvl < 3; lvl++) {
        if (dir + 16 > n) return NULL;
        int cnt = *(const uint16_t *)(d + dir + 12) + *(const uint16_t *)(d + dir + 14);
        const uint32_t *e = (const uint32_t *)(d + dir + 16), *hit = NULL;
        for (int i = 0; i < cnt; i++) if (want[lvl] == 0xFFFFFFFF || e[2 * i] == want[lvl]) { hit = e + 2 * i; break; }
        if (!hit) return NULL;
        if (lvl < 2) { if (!(hit[1] & 0x80000000)) return NULL; dir = root + (hit[1] & 0x7FFFFFFF); }
        else {
            if (hit[1] & 0x80000000) return NULL;
            const uint32_t *de = (const uint32_t *)(d + root + hit[1]);
            uint32_t off = OFF(de[0]);
            if (!off || off + de[1] > n) return NULL;
            *len = de[1];
            return d + off;
        }
    }
    #undef OFF
    return NULL;
}
static DWORD WINAPI GetFileVersionInfoSizeA(const char *f, DWORD *h) { uint32_t l; if (h) *h = 0; if (!version_resource(f, &l)) { set_err(1813); return 0; } return l * 2 + 4; }
static DWORD WINAPI GetFileVersionInfoSizeW(const WCHAR *f, DWORD *h) { char a[MAXP]; w2a(f, a, sizeof a); return GetFileVersionInfoSizeA(a, h); }
static DWORD WINAPI GetFileVersionInfoSizeExW(DWORD fl, const WCHAR *f, DWORD *h) { return GetFileVersionInfoSizeW(f, h); }
static BOOL WINAPI GetFileVersionInfoA(const char *f, DWORD h, DWORD n, void *b) {
    uint32_t l; const uint8_t *r = version_resource(f, &l);
    if (!r) { set_err(1813); return 0; }
    memset(b, 0, n); memcpy(b, r, MIN(l, n));
    return 1;
}
static BOOL WINAPI GetFileVersionInfoW(const WCHAR *f, DWORD h, DWORD n, void *b) { char a[MAXP]; w2a(f, a, sizeof a); return GetFileVersionInfoA(a, h, n, b); }
static BOOL WINAPI GetFileVersionInfoExW(DWORD fl, const WCHAR *f, DWORD h, DWORD n, void *b) { return GetFileVersionInfoW(f, h, n, b); }
/* the tree: WORD length, WORD value length, WORD type (1 text), key, padding, value, padding, children */
static bool ver_find(const uint8_t *blk, uint32_t avail, const WCHAR *path, const uint8_t **val, uint32_t *vlen, bool *text) {
    const uint8_t *node = blk;
    for (;;) {
        uint16_t len = *(const uint16_t *)node, vl = *(const uint16_t *)(node + 2), ty = *(const uint16_t *)(node + 4);
        if (len < 6 || len > avail) return false;
        const WCHAR *key = (const WCHAR *)(node + 6); int kl = wlen(key);
        const uint8_t *v = (const uint8_t *)(((uintptr_t)(key + kl + 1) + 3) & ~(uintptr_t)3);
        uint32_t vbytes = ty == 1 ? vl * 2 : vl;
        while (*path == '\\') path++;
        if (!*path) { *val = v; *vlen = ty == 1 ? vl : vl; *text = ty == 1; return true; }
        const WCHAR *e = path; while (*e && *e != '\\') e++;
        int pl = e - path;
        const uint8_t *child = (const uint8_t *)(((uintptr_t)(v + vbytes) + 3) & ~(uintptr_t)3), *end = node + len;
        bool found = false;
        while (child + 6 <= end) {
            uint16_t cl = *(const uint16_t *)child;
            if (cl < 6) break;
            const WCHAR *ck = (const WCHAR *)(child + 6);
            if (wlen(ck) == pl && !wcmpn(ck, path, pl, true)) { node = child; avail = cl; path = e; found = true; break; }
            child = (const uint8_t *)(((uintptr_t)(child + cl) + 3) & ~(uintptr_t)3);
        }
        if (!found) return false;
    }
}
static BOOL WINAPI VerQueryValueW(const void *blk, const WCHAR *sub, void **out, UINT *len) {
    const uint8_t *v; uint32_t vl; bool text;
    if (!blk || !sub || !ver_find(blk, *(const uint16_t *)blk, sub, &v, &vl, &text)) { if (out) *out = NULL; if (len) *len = 0; return 0; }
    if (out) *out = (void *)v; if (len) *len = vl;
    return 1;
}
static BOOL WINAPI VerQueryValueA(const void *blk, const char *sub, void **out, UINT *len) {
    WCHAR w[256]; a2w(sub, w, 256);
    const uint8_t *v; uint32_t vl; bool text;
    if (!blk || !sub || !ver_find(blk, *(const uint16_t *)blk, w, &v, &vl, &text)) { if (out) *out = NULL; if (len) *len = 0; return 0; }
    if (text) {                                            /* an ANSI copy, kept in the second half of the caller's block */
        uint16_t total = *(const uint16_t *)blk;
        uint32_t off = (uint32_t)(v - (const uint8_t *)blk) / 2;
        char *dst = (char *)blk + total + 4 + off;
        const WCHAR *ws = (const WCHAR *)v; for (uint32_t i = 0; i < vl; i++) dst[i] = ws[i] < 256 ? (char)ws[i] : '?';
        if (out) *out = dst;
    } else if (out) *out = (void *)v;
    if (len) *len = vl;
    return 1;
}
static DWORD WINAPI VerLanguageNameA(DWORD l, char *b, DWORD n) { strlcpy(b, l == 0x409 ? "English (United States)" : "Language Neutral", n); return strlen(b); }
static DWORD WINAPI VerLanguageNameW(DWORD l, WCHAR *b, DWORD n) { return a2w(l == 0x409 ? "English (United States)" : "Language Neutral", b, n); }

/* ================================================================ winmm */
static DWORD WINAPI timeGetTime(void) { return timer_ticks(); }
static UINT WINAPI timeBeginPeriod(UINT p) { return 0; }
static UINT WINAPI timeEndPeriod(UINT p) { return 0; }
static UINT WINAPI timeGetDevCaps(UINT *caps, UINT n) { if (n >= 8) { caps[0] = 1; caps[1] = 1000000; } return 0; }
static UINT WINAPI timeGetSystemTime(void *mmt, UINT n) { if (n >= 8) { ((UINT *)mmt)[0] = 1; ((UINT *)mmt)[1] = timer_ticks(); } return 0; }
/* timeSetEvent: a thread of the program that calls back every period (or once) */
typedef struct { uint32_t delay; void (WINAPI *fn)(UINT, UINT, uintptr_t, uintptr_t, uintptr_t); uintptr_t user; UINT flags; int id; volatile bool stop; } mmtimer_t;
static mmtimer_t *mmtimers[16];
static DWORD WINAPI mmtimer_thread(void *arg) {
    mmtimer_t *t = arg;
    do {
        task_sleep(MAX(t->delay, 1u));
        if (t->stop) break;
        if (t->flags & 0x10) ((BOOL (WINAPI *)(HANDLE))K("SetEvent"))((HANDLE)(uintptr_t)t->fn);   /* TIME_CALLBACK_EVENT_SET */
        else t->fn(t->id, 0, t->user, 0, 0);
    } while ((t->flags & 1) && !t->stop);                      /* TIME_PERIODIC */
    return 0;
}
static UINT WINAPI timeSetEvent(UINT delay, UINT res, void *fn, uintptr_t user, UINT flags) {
    int slot = -1; for (int i = 0; i < 16; i++) if (!mmtimers[i]) { slot = i; break; }
    if (slot < 0 || !fn) return 0;
    mmtimer_t *t = kzalloc(sizeof *t); t->delay = delay; t->fn = fn; t->user = user; t->flags = flags; t->id = slot + 1;
    mmtimers[slot] = t;
    ((HANDLE (WINAPI *)(void *, SIZE_T, void *, void *, DWORD, DWORD *))K("CreateThread"))(NULL, 0, (void *)mmtimer_thread, t, 0, NULL);
    return t->id;
}
static UINT WINAPI timeKillEvent(UINT id) { if (!id || id > 16 || !mmtimers[id - 1]) return 97; mmtimers[id - 1]->stop = true; mmtimers[id - 1] = NULL; return 0; }
/* PlaySound: SND_ASYNC 1, SND_NODEFAULT 2, SND_MEMORY 4, SND_LOOP 8, SND_PURGE 0x40, SND_ALIAS 0x10000, SND_FILENAME 0x20000, SND_RESOURCE 0x40004 */
static BOOL play(const char *name, const void *mem, uint32_t memlen, DWORD fl) {
    if (!name && !mem) { audio_stop_all(); return 1; }
    char err[120];
    if (mem) {
        uint32_t len = memlen ? memlen : (((const uint8_t *)mem)[0] == 'R' ? *(const uint32_t *)((const uint8_t *)mem + 4) + 8 : 0);
        sound_t *s = sound_load_wav(mem, len, err, sizeof err);
        if (!s) return 0;
        audio_play(s);
        return 1;                                          /* the voice keeps its sound; it stays loaded */
    }
    if (fl & 0x10000 || !strchr(name, '.')) {              /* system sounds by alias */
        static const struct { const char *alias, *file; } al[] = { { "SystemStart", "Windows XP Startup.wav" }, { "SystemExit", "Windows XP Shutdown.wav" } };
        for (unsigned i = 0; i < ARRAY_LEN(al); i++) if (!strcasecmp(name, al[i].alias)) { char p[MAXP]; snprintf(p, sizeof p, "C:\\WRLD\\Media\\%s", al[i].file); return audio_play_file(p, err, sizeof err); }
        speaker_beep(800, 60); return 1;
    }
    char p[MAXP];
    if (isalpha(name[0]) && name[1] == ':') strlcpy(p, name, sizeof p); else path_join(p, sizeof p, P->cwd, name);
    if (!vfs_resolve(p, NULL)) { if (fl & 2) return 0; speaker_beep(800, 60); return 0; }
    bool ok = audio_play_file(p, err, sizeof err);
    if (ok && !(fl & 1)) audio_wait(60000);
    return ok;
}
static BOOL WINAPI PlaySoundA(const char *n, HINSTANCE m, DWORD f) {
    if (f & 0x40) { audio_stop_all(); return 1; }
    if ((f & 0x40004) == 0x40004 && n) {                    /* a WAVE resource of the module */
        uintptr_t r = w32_find_resource_in(P, m, (uintptr_t)"WAVE", (uintptr_t)n, false, 0);
        uint32_t sz = 0; void *d = r ? w32_resource_data(P, r, &sz) : NULL;
        return d ? play(NULL, d, sz, f) : 0;
    }
    return play((f & 4) ? NULL : n, (f & 4) ? n : NULL, 0, f);
}
static BOOL WINAPI PlaySoundW(const WCHAR *n, HINSTANCE m, DWORD f) {
    if (f & 0x40) { audio_stop_all(); return 1; }
    if ((f & 4) || (uintptr_t)n < 0x10000) return PlaySoundA((const char *)n, m, f);
    char a[MAXP]; w2a(n, a, sizeof a); return PlaySoundA(a, m, f);
}
static BOOL WINAPI sndPlaySoundA(const char *n, UINT f) { return PlaySoundA(n, 0, (f & 0xF) | (n ? 0x20000 : 0)); }
static BOOL WINAPI sndPlaySoundW(const WCHAR *n, UINT f) { return PlaySoundW(n, 0, (f & 0xF) | (n ? 0x20000 : 0)); }
/* waveOut: 16-bit PCM in, the kernel's sound stream out (converted to its stereo rate) */
typedef struct { void *data; DWORD len, recorded; uintptr_t user; DWORD flags, loops; void *next; uintptr_t res; } WAVEHDR;
typedef struct { WORD tag, ch; DWORD rate, bps; WORD align, bits, cb; } WFX;
typedef struct { int stream; WFX fmt; uintptr_t cb, inst; DWORD flags; WAVEHDR *q[64]; int qn; uint64_t played; bool paused, open; HANDLE thread; } wout_t;
#define WHDR_DONE 1
#define WHDR_PREPARED 2
#define WHDR_INQUEUE 0x10
static void wout_notify(wout_t *w, UINT msg, uintptr_t p1) {
    DWORD kind = w->flags & 0x70000;
    if (kind == 0x30000 && w->cb) ((void (WINAPI *)(uintptr_t, UINT, uintptr_t, uintptr_t, uintptr_t))w->cb)((uintptr_t)w, msg, w->inst, p1, 0);   /* CALLBACK_FUNCTION */
    else if (kind == 0x50000 && w->cb) ((BOOL (WINAPI *)(HANDLE))K("SetEvent"))((HANDLE)w->cb);                                                      /* CALLBACK_EVENT */
    else if (kind == 0x10000 && w->cb) { void *fn = NULL; w32_lookup("user32.dll", "PostMessageA", &fn); if (fn) ((BOOL (WINAPI *)(HWND, UINT, WPARAM, LPARAM))fn)(w->cb, msg + 0x3BB - 0x3BB, (WPARAM)w, p1); }   /* CALLBACK_WINDOW: MM_WOM_* */
}
/* convert a buffer to 16-bit stereo at the output rate and queue it */
static void wout_push(wout_t *w, WAVEHDR *h) {
    int rate = audio_rate(), ch = w->fmt.ch, bits = w->fmt.bits;
    uint32_t frames_in = h->len / (ch * bits / 8);
    uint64_t frames_out = (uint64_t)frames_in * rate / (w->fmt.rate ? w->fmt.rate : rate);
    int16_t *out = kmalloc(frames_out * 4 + 4);
    const uint8_t *in = h->data;
    for (uint64_t i = 0; i < frames_out; i++) {
        uint32_t s = (uint32_t)(i * w->fmt.rate / rate); if (s >= frames_in) s = frames_in - 1;
        int l, r;
        if (bits == 16) { const int16_t *p = (const int16_t *)in + s * ch; l = p[0]; r = ch > 1 ? p[1] : l; }
        else { const uint8_t *p = in + s * ch; l = (p[0] - 128) << 8; r = ch > 1 ? (p[1] - 128) << 8 : l; }
        out[2 * i] = l; out[2 * i + 1] = r;
    }
    uint64_t done = 0;
    while (done < frames_out && w->open) {
        int room = audio_stream_room(w->stream);
        if (room <= 0) { task_sleep(5); continue; }
        int k = audio_stream_write(w->stream, out + done * 2, (int)MIN((uint64_t)room, frames_out - done));
        if (k <= 0) { task_sleep(5); continue; }
        done += k;
    }
    kfree(out);
    w->played += frames_in;
}
static DWORD WINAPI wout_thread(void *arg) {
    wout_t *w = arg;
    while (w->open) {
        if (w->paused || !w->qn) { task_sleep(5); continue; }
        WAVEHDR *h = w->q[0];
        wout_push(w, h);
        if (!w->open) break;
        memmove(w->q, w->q + 1, (--w->qn) * sizeof w->q[0]);
        h->flags = (h->flags & ~WHDR_INQUEUE) | WHDR_DONE;
        wout_notify(w, 0x3BD, (uintptr_t)h);                    /* WOM_DONE */
    }
    return 0;
}
static UINT WINAPI waveOutGetNumDevs(void) { return audio_present() ? 1 : 0; }
typedef struct { WORD mid, pid; UINT ver; WCHAR name[32]; DWORD formats; WORD ch, res; DWORD support; } WOCW;
typedef struct { WORD mid, pid; UINT ver; char name[32]; DWORD formats; WORD ch, res; DWORD support; } WOCA;
static UINT WINAPI waveOutGetDevCapsW(uintptr_t id, WOCW *c, UINT n) { if (!audio_present()) return 2; memset(c, 0, MIN(n, sizeof *c)); c->mid = 1; c->pid = 1; c->ver = 0x100; a2w("Speakers", c->name, 32); c->formats = 0xFFFFF; c->ch = 2; c->support = 0x2C; return 0; }
static UINT WINAPI waveOutGetDevCapsA(uintptr_t id, WOCA *c, UINT n) { if (!audio_present()) return 2; memset(c, 0, MIN(n, sizeof *c)); c->mid = 1; c->pid = 1; c->ver = 0x100; strlcpy(c->name, "Speakers", 32); c->formats = 0xFFFFF; c->ch = 2; c->support = 0x2C; return 0; }
static UINT WINAPI waveOutOpen(uintptr_t *out, UINT id, const WFX *f, uintptr_t cb, uintptr_t inst, DWORD fl) {
    if (!audio_present()) return 6;                            /* MMSYSERR_NODRIVER */
    if (!f || (f->tag != 1 && f->tag != 0xFFFE) || (f->bits != 8 && f->bits != 16) || !f->ch || f->ch > 2) return 32;   /* WAVERR_BADFORMAT */
    if (fl & 1) return 0;                                      /* WAVE_FORMAT_QUERY */
    wout_t *w = kzalloc(sizeof *w);
    w->stream = audio_stream_open();
    if (w->stream < 0) { kfree(w); return 4; }                 /* MMSYSERR_ALLOCATED */
    w->fmt = *f; w->cb = cb; w->inst = inst; w->flags = fl; w->open = true;
    if (out) *out = (uintptr_t)w;
    w->thread = ((HANDLE (WINAPI *)(void *, SIZE_T, void *, void *, DWORD, DWORD *))K("CreateThread"))(NULL, 0, (void *)wout_thread, w, 0, NULL);
    wout_notify(w, 0x3BB, 0);                                  /* WOM_OPEN */
    return 0;
}
static UINT WINAPI waveOutPrepareHeader(uintptr_t h, WAVEHDR *hd, UINT n) { hd->flags |= WHDR_PREPARED; return 0; }
static UINT WINAPI waveOutUnprepareHeader(uintptr_t h, WAVEHDR *hd, UINT n) { if (hd->flags & WHDR_INQUEUE) return 33; hd->flags &= ~WHDR_PREPARED; return 0; }
static UINT WINAPI waveOutWrite(uintptr_t h, WAVEHDR *hd, UINT n) {
    wout_t *w = (wout_t *)h; if (!w || !w->open) return 5;
    if (!(hd->flags & WHDR_PREPARED)) return 34;
    if (w->qn >= 64) { while (w->qn >= 64 && w->open) task_sleep(5); }
    hd->flags = (hd->flags & ~WHDR_DONE) | WHDR_INQUEUE;
    w->q[w->qn++] = hd;
    return 0;
}
static UINT WINAPI waveOutReset(uintptr_t h) {
    wout_t *w = (wout_t *)h; if (!w) return 5;
    while (w->qn) { WAVEHDR *hd = w->q[--w->qn]; hd->flags = (hd->flags & ~WHDR_INQUEUE) | WHDR_DONE; wout_notify(w, 0x3BD, (uintptr_t)hd); }
    return 0;
}
static UINT WINAPI waveOutClose(uintptr_t h) {
    wout_t *w = (wout_t *)h; if (!w) return 5;
    if (w->qn) return 33;                                      /* WAVERR_STILLPLAYING */
    w->open = false;
    ((DWORD (WINAPI *)(HANDLE, DWORD))K("WaitForSingleObject"))(w->thread, 500);
    audio_stream_close(w->stream);
    wout_notify(w, 0x3BC, 0);                                  /* WOM_CLOSE */
    return 0;
}
static UINT WINAPI waveOutPause(uintptr_t h) { wout_t *w = (wout_t *)h; if (w) w->paused = true; return 0; }
static UINT WINAPI waveOutRestart(uintptr_t h) { wout_t *w = (wout_t *)h; if (w) w->paused = false; return 0; }
typedef struct { UINT type; DWORD v; DWORD pad[2]; } MMTIME;
static UINT WINAPI waveOutGetPosition(uintptr_t h, MMTIME *t, UINT n) {
    wout_t *w = (wout_t *)h; if (!w) return 5;
    uint64_t queued = audio_stream_queued(w->stream), heard = w->played > queued * w->fmt.rate / audio_rate() ? w->played - queued * w->fmt.rate / audio_rate() : 0;
    if (t->type == 1) t->v = (DWORD)heard;                     /* TIME_SAMPLES */
    else if (t->type == 1 + 1) t->v = (DWORD)(heard * 1000 / w->fmt.rate);   /* TIME_MS */
    else { t->type = 4; t->v = (DWORD)(heard * w->fmt.align); }            /* TIME_BYTES */
    return 0;
}
static UINT WINAPI waveOutGetVolume(uintptr_t h, DWORD *v) { *v = 0xFFFFFFFF; return 0; }
static UINT WINAPI waveOutSetVolume(uintptr_t h, DWORD v) { return 0; }
static UINT WINAPI waveOutGetErrorTextA(UINT e, char *b, UINT n) { snprintf(b, n, e == 32 ? "The format is not supported." : e == 6 ? "No sound driver is installed." : "Sound error %u.", e); return 0; }
static UINT WINAPI waveOutGetErrorTextW(UINT e, WCHAR *b, UINT n) { char a[128]; waveOutGetErrorTextA(e, a, sizeof a); a2w(a, b, n); return 0; }
static UINT WINAPI waveOutMessage(uintptr_t h, UINT m, uintptr_t a, uintptr_t b) { return 8; }
/* no recording, MIDI, mixer or joystick devices */
static UINT WINAPI waveInGetNumDevs(void) { return 0; }
static UINT WINAPI waveInGetDevCapsW(uintptr_t id, void *c, UINT n) { return 2; }
static UINT WINAPI waveInGetDevCapsA(uintptr_t id, void *c, UINT n) { return 2; }
static UINT WINAPI waveInOpen(uintptr_t *h, UINT id, const void *f, uintptr_t cb, uintptr_t i, DWORD fl) { return 6; }
static UINT WINAPI waveIn_stub(uintptr_t h) { return 5; }
static UINT WINAPI waveIn_stub3(uintptr_t h, void *x, UINT n) { return 5; }
static UINT WINAPI midiOutGetNumDevs(void) { return 0; }
static UINT WINAPI midiOutOpen(uintptr_t *h, UINT id, uintptr_t cb, uintptr_t i, DWORD f) { return 6; }
static UINT WINAPI midiInGetNumDevs(void) { return 0; }
static UINT WINAPI mixerGetNumDevs(void) { return 0; }
static UINT WINAPI mixerOpen(uintptr_t *h, UINT id, uintptr_t cb, uintptr_t i, DWORD f) { return 6; }
static UINT WINAPI auxGetNumDevs(void) { return 0; }
static UINT WINAPI joyGetNumDevs(void) { return 0; }
static UINT WINAPI joyGetPos(UINT id, void *p) { return 167; }                 /* JOYERR_UNPLUGGED */
static UINT WINAPI joyGetPosEx(UINT id, void *p) { return 167; }
static UINT WINAPI joyGetDevCapsA(uintptr_t id, void *c, UINT n) { return 167; }
static UINT WINAPI joyGetDevCapsW(uintptr_t id, void *c, UINT n) { return 167; }
/* MCI: "play <file>" style commands on .wav files; the rest reports no device */
static UINT WINAPI mciSendStringA(const char *cmd, char *ret, UINT n, HWND cb) {
    if (ret && n) ret[0] = 0;
    char c[MAXP]; strlcpy(c, cmd ? cmd : "", sizeof c);
    if (!strncasecmp(c, "play ", 5)) {
        char *f = c + 5; while (*f == ' ') f++;
        if (*f == '"') { f++; char *e = strchr(f, '"'); if (e) *e = 0; } else { char *e = strchr(f, ' '); if (e) *e = 0; }
        const char *dot = strrchr(f, '.'); if (dot && !strcasecmp(dot, ".wav")) return play(f, NULL, 0, 1) ? 0 : 275;
    }
    if (!strncasecmp(c, "stop", 4) || !strncasecmp(c, "close", 5)) { audio_stop_all(); return 0; }
    return 263;                                                    /* MCIERR_INVALID_DEVICE_NAME */
}
static UINT WINAPI mciSendStringW(const WCHAR *cmd, WCHAR *ret, UINT n, HWND cb) { char a[MAXP]; w2a(cmd, a, sizeof a); if (ret && n) ret[0] = 0; return mciSendStringA(a, NULL, 0, cb); }
static UINT WINAPI mciSendCommandW(UINT id, UINT msg, uintptr_t f, uintptr_t p) { return 263; }
static UINT WINAPI mciSendCommandA(UINT id, UINT msg, uintptr_t f, uintptr_t p) { return 263; }
static BOOL WINAPI mciGetErrorStringA(UINT e, char *b, UINT n) { snprintf(b, n, "The specified device is not open or is not recognized by MCI."); return 1; }
static BOOL WINAPI mciGetErrorStringW(UINT e, WCHAR *b, UINT n) { a2w("The specified device is not open or is not recognized by MCI.", b, n); return 1; }

#define G(g, n) { g, #n, (void *)n }
#define GA(g, n, f) { g, n, (void *)f }
const w32_api_t shell_api[] = {
    /* shlwapi */
    G("shlwapi", PathFindFileNameW), G("shlwapi", PathFindFileNameA), G("shlwapi", PathFindExtensionW), G("shlwapi", PathFindExtensionA), G("shlwapi", PathFileExistsA),
    G("shlwapi", PathFileExistsW), G("shlwapi", PathIsDirectoryA), G("shlwapi", PathIsDirectoryW), G("shlwapi", PathIsRelativeW), G("shlwapi", PathIsRelativeA),
    G("shlwapi", PathIsRootW), G("shlwapi", PathIsRootA), G("shlwapi", PathIsUNCW), G("shlwapi", PathIsUNCA), G("shlwapi", PathIsURLW), G("shlwapi", PathIsURLA),
    G("shlwapi", PathIsFileSpecW), G("shlwapi", PathGetDriveNumberW), G("shlwapi", PathGetDriveNumberA), G("shlwapi", PathAddBackslashW), G("shlwapi", PathAddBackslashA),
    G("shlwapi", PathRemoveBackslashW), G("shlwapi", PathRemoveBackslashA), G("shlwapi", PathRemoveFileSpecW), G("shlwapi", PathRemoveFileSpecA),
    G("shlwapi", PathRemoveExtensionW), G("shlwapi", PathRemoveExtensionA), G("shlwapi", PathRenameExtensionW), G("shlwapi", PathRenameExtensionA),
    G("shlwapi", PathAddExtensionW), G("shlwapi", PathStripPathW), G("shlwapi", PathStripPathA), G("shlwapi", PathStripToRootW), G("shlwapi", PathStripToRootA),
    G("shlwapi", PathSkipRootW), G("shlwapi", PathSkipRootA), G("shlwapi", PathFindNextComponentW), G("shlwapi", PathCanonicalizeW), G("shlwapi", PathCanonicalizeA),
    G("shlwapi", PathCombineW), G("shlwapi", PathCombineA), G("shlwapi", PathAppendW), G("shlwapi", PathAppendA), G("shlwapi", PathCommonPrefixW),
    G("shlwapi", PathIsPrefixW), G("shlwapi", PathIsSameRootW), G("shlwapi", PathRelativePathToW), G("shlwapi", PathMatchSpecW), G("shlwapi", PathMatchSpecA),
    G("shlwapi", PathQuoteSpacesW), G("shlwapi", PathUnquoteSpacesW), G("shlwapi", PathUnquoteSpacesA), G("shlwapi", PathUnExpandEnvStringsW),
    G("shlwapi", PathCompactPathExW), G("shlwapi", PathCompactPathExA), G("shlwapi", SHAutoComplete), G("shlwapi", IsOS), GA("shlwapi", "Ordinal437", IsOS),
    G("shlwapi", StrChrW), G("shlwapi", StrChrA), G("shlwapi", StrChrIW), G("shlwapi", StrChrIA), G("shlwapi", StrRChrW), G("shlwapi", StrRChrA), G("shlwapi", StrCmpW),
    G("shlwapi", StrCmpIW), G("shlwapi", StrCmpNW), G("shlwapi", StrCmpNIW), G("shlwapi", StrCmpNA), G("shlwapi", StrCmpNIA), G("shlwapi", StrCmpIA), G("shlwapi", StrCmpLogicalW),
    G("shlwapi", StrCpyW), G("shlwapi", StrCpyNW), G("shlwapi", StrCpyNA), G("shlwapi", StrCatW), G("shlwapi", StrNCatW), G("shlwapi", StrNCatA), G("shlwapi", StrCatBuffW),
    G("shlwapi", StrCatBuffA), G("shlwapi", StrDupW), G("shlwapi", StrDupA), G("shlwapi", StrStrW), G("shlwapi", StrStrIW), G("shlwapi", StrStrA), G("shlwapi", StrStrIA),
    G("shlwapi", StrTrimW), G("shlwapi", StrTrimA), G("shlwapi", StrToIntW), G("shlwapi", StrToIntA), G("shlwapi", StrToIntExW), G("shlwapi", StrToIntExA),
    G("shlwapi", StrFormatByteSizeW), G("shlwapi", StrFormatByteSizeA), G("shlwapi", StrFormatByteSize64A), G("shlwapi", StrFormatKBSizeW), G("shlwapi", StrRetToBufW),
    G("shlwapi", StrRetToStrW), G("shlwapi", SHStrDupW), G("shlwapi", SHStrDupA), G("shlwapi", wvnsprintfW), G("shlwapi", wnsprintfW), G("shlwapi", wvnsprintfA),
    G("shlwapi", wnsprintfA), G("shlwapi", UrlEscapeW), G("shlwapi", UrlUnescapeW), G("shlwapi", SHGetObjectCompatFlags), G("shlwapi", SHCreateStreamOnFileW),
    G("shlwapi", SHCreateStreamOnFileEx), G("shlwapi", SHAnsiToUnicode), G("shlwapi", SHUnicodeToAnsi), G("shlwapi", ColorHLSToRGB), G("shlwapi", ColorRGBToHLS),
    GA("shlwapi", "StrCmpLogicalW", StrCmpLogicalW),
    /* shell32 (some of these lived in shlwapi too, and Windows 7 forwards them) */
    G("shell32", SHGetFolderPathA), G("shell32", SHGetFolderPathW), G("shell32", SHGetFolderPathAndSubDirW), G("shell32", SHGetSpecialFolderPathA),
    G("shell32", SHGetSpecialFolderPathW), G("shell32", SHGetKnownFolderPath), G("shell32", SHGetSpecialFolderLocation), G("shell32", SHGetFolderLocation),
    G("shell32", SHGetKnownFolderIDList), G("shell32", SHGetPathFromIDListA), G("shell32", SHGetPathFromIDListW), G("shell32", SHGetPathFromIDListEx),
    G("shell32", ILCreateFromPathW), G("shell32", ILCreateFromPathA), G("shell32", ILFree), G("shell32", ILClone), G("shell32", ILGetSize), GA("shell32", "Ordinal155", ILFree),
    GA("shell32", "Ordinal18", ILClone), GA("shell32", "Ordinal152", ILGetSize), GA("shell32", "Ordinal157", ILCreateFromPathW), GA("shell32", "SHFree", ILFree),
    GA("shell32", "Ordinal195", ILFree), G("shell32", SHParseDisplayName), G("shell32", SHGetDataFromIDListW), G("shell32", SHGetMalloc), G("shell32", SHGetDesktopFolder),
    G("shell32", SHBrowseForFolderA), G("shell32", SHBrowseForFolderW), GA("shell32", "SHBrowseForFolder", SHBrowseForFolderA), G("shell32", CommandLineToArgvW),
    G("shell32", ShellExecuteW), G("shell32", ShellExecuteExA), G("shell32", ShellExecuteExW), G("shell32", FindExecutableA), G("shell32", FindExecutableW),
    G("shell32", SHCreateDirectoryExA), G("shell32", SHCreateDirectoryExW), G("shell32", SHCreateDirectory), G("shell32", SHFileOperationA), G("shell32", SHFileOperationW),
    G("shell32", SHGetFileInfoA), G("shell32", SHGetFileInfoW), G("shell32", ExtractIconA), G("shell32", ExtractIconW), G("shell32", ExtractIconExW), G("shell32", ExtractIconExA),
    G("shell32", DragAcceptFiles), G("shell32", DragQueryFileA), G("shell32", DragQueryFileW), G("shell32", DragQueryPoint), G("shell32", DragFinish),
    G("shell32", IsUserAnAdmin), GA("shell32", "Ordinal680", IsUserAnAdmin), G("shell32", SHAddToRecentDocs), G("shell32", SHChangeNotify), G("shell32", SHAppBarMessage),
    G("shell32", Shell_NotifyIconW), G("shell32", Shell_NotifyIconA), G("shell32", ShellAboutW), G("shell32", ShellAboutA), G("shell32", SetCurrentProcessExplicitAppUserModelID),
    G("shell32", SHGetStockIconInfo), G("shell32", PickIconDlg), G("shell32", SHOpenFolderAndSelectItems), GA("shell32", "DragQueryFile", DragQueryFileA),
    GA("shell32", "SHGetFileInfo", SHGetFileInfoA), GA("shell32", "ShellExecuteEx", ShellExecuteExA), GA("ole32", "CoGetMalloc", CoGetMalloc2),
    GA("shlwapi", "SHGetFolderPathW", SHGetFolderPathW), GA("shlwapi", "SHGetFolderPathA", SHGetFolderPathA),
    /* version */
    G("version", GetFileVersionInfoSizeA), G("version", GetFileVersionInfoSizeW), G("version", GetFileVersionInfoSizeExW), G("version", GetFileVersionInfoA),
    G("version", GetFileVersionInfoW), G("version", GetFileVersionInfoExW), G("version", VerQueryValueA), G("version", VerQueryValueW), G("version", VerLanguageNameA),
    G("version", VerLanguageNameW), GA("kernel32", "VerLanguageNameA", VerLanguageNameA), GA("kernel32", "VerLanguageNameW", VerLanguageNameW),
    /* winmm */
    G("winmm", timeGetTime), G("winmm", timeBeginPeriod), G("winmm", timeEndPeriod), G("winmm", timeGetDevCaps), G("winmm", timeGetSystemTime), G("winmm", timeSetEvent),
    G("winmm", timeKillEvent), G("winmm", PlaySoundA), G("winmm", PlaySoundW), GA("winmm", "PlaySound", PlaySoundA), G("winmm", sndPlaySoundA), G("winmm", sndPlaySoundW),
    G("winmm", waveOutGetNumDevs), G("winmm", waveOutGetDevCapsW), G("winmm", waveOutGetDevCapsA), G("winmm", waveOutOpen), G("winmm", waveOutPrepareHeader),
    G("winmm", waveOutUnprepareHeader), G("winmm", waveOutWrite), G("winmm", waveOutReset), G("winmm", waveOutClose), G("winmm", waveOutPause), G("winmm", waveOutRestart),
    G("winmm", waveOutGetPosition), G("winmm", waveOutGetVolume), G("winmm", waveOutSetVolume), G("winmm", waveOutGetErrorTextA), G("winmm", waveOutGetErrorTextW),
    G("winmm", waveOutMessage), G("winmm", waveInGetNumDevs), G("winmm", waveInGetDevCapsW), G("winmm", waveInGetDevCapsA), G("winmm", waveInOpen),
    GA("winmm", "waveInClose", waveIn_stub), GA("winmm", "waveInStart", waveIn_stub), GA("winmm", "waveInStop", waveIn_stub), GA("winmm", "waveInReset", waveIn_stub),
    GA("winmm", "waveInAddBuffer", waveIn_stub3), GA("winmm", "waveInPrepareHeader", waveIn_stub3), GA("winmm", "waveInUnprepareHeader", waveIn_stub3),
    G("winmm", midiOutGetNumDevs), G("winmm", midiOutOpen), G("winmm", midiInGetNumDevs), G("winmm", mixerGetNumDevs), G("winmm", mixerOpen), G("winmm", auxGetNumDevs),
    G("winmm", joyGetNumDevs), G("winmm", joyGetPos), G("winmm", joyGetPosEx), G("winmm", joyGetDevCapsA), G("winmm", joyGetDevCapsW), G("winmm", mciSendStringA),
    G("winmm", mciSendStringW), G("winmm", mciSendCommandW), G("winmm", mciSendCommandA), G("winmm", mciGetErrorStringA), G("winmm", mciGetErrorStringW),
    { NULL, NULL, NULL }
};
