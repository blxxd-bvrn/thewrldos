/* The registry (advapi32 Reg* functions): one tree for all programs, kept in memory and
 * saved to C:\WRLD\registry.dat after every change, so settings survive restarts when C:
 * does. Key and value names compare without case; strings are stored as UTF-16, and the
 * ANSI functions convert. The predefined roots are HKEY_CLASSES_ROOT, HKEY_CURRENT_USER,
 * HKEY_LOCAL_MACHINE, HKEY_USERS and HKEY_CURRENT_CONFIG (HKEY_CLASSES_ROOT is also
 * HKLM\SOFTWARE\Classes, as on Windows).
 *
 * registry.dat is text: "K <path>" starts a key, "V <type> <name> <hex data>" adds a value
 * to it (names percent-escaped). */
#include "w32.h"

#define P w32_cur()
#define REG_PATH "C:\\WRLD\\registry.dat"
enum { E_OK = 0, E_NOTFOUND = 2, E_ACCESS = 5, E_BADKEY = 6, E_PARAM = 87, E_MORE = 234, E_NOMORE = 259 };
enum { REG_NONE, REG_SZ, REG_EXPAND_SZ, REG_BINARY, REG_DWORD, REG_DWORD_BE, REG_LINK, REG_MULTI_SZ, REG_QWORD = 11 };

typedef struct rval { struct rval *next; char name[128]; uint32_t type, len; uint8_t *data; } rval_t;
typedef struct rkey { struct rkey *parent, *child, *next; char name[128]; rval_t *vals; rtc_time_t mtime; } rkey_t;

static rkey_t *roots[6];               /* HKCR (unused: HKLM\SOFTWARE\Classes), HKCU, HKLM, HKU, HKPD, HKCC */
static const char *root_names[6] = { "HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS", "HKEY_PERFORMANCE_DATA", "HKEY_CURRENT_CONFIG" };
static bool loaded, loading;

/* ---------------------------------------------------------------- tree */
static rkey_t *new_key(rkey_t *parent, const char *name) {
    rkey_t *k = kzalloc(sizeof *k);
    strlcpy(k->name, name, sizeof k->name);
    k->parent = parent; rtc_now(&k->mtime);
    if (parent) {                                     /* children kept in name order, like regedit shows */
        rkey_t **pp = &parent->child;
        while (*pp && strcasecmp((*pp)->name, name) < 0) pp = &(*pp)->next;
        k->next = *pp; *pp = k;
    }
    return k;
}
static rkey_t *find_child(rkey_t *k, const char *name, int len) {
    for (rkey_t *c = k->child; c; c = c->next) if ((int)strlen(c->name) == len && !strncasecmp(c->name, name, len)) return c;
    return NULL;
}
/* walk "A\B\C" below k; create the missing keys when asked */
static rkey_t *walk(rkey_t *k, const char *path, bool create) {
    while (k && path && *path) {
        while (*path == '\\') path++;
        if (!*path) break;
        const char *e = strchr(path, '\\'); int len = e ? (int)(e - path) : (int)strlen(path);
        rkey_t *c = find_child(k, path, len);
        if (!c) {
            if (!create) return NULL;
            char n[128]; int l = MIN(len, 127); memcpy(n, path, l); n[l] = 0;
            c = new_key(k, n);
        }
        k = c; path += len;
    }
    return k;
}
static rval_t *find_val(rkey_t *k, const char *name) {
    for (rval_t *v = k->vals; v; v = v->next) if (!strcasecmp(v->name, name ? name : "")) return v;
    return NULL;
}
static void set_val(rkey_t *k, const char *name, uint32_t type, const void *data, uint32_t len) {
    rval_t *v = find_val(k, name);
    if (!v) {
        v = kzalloc(sizeof *v); strlcpy(v->name, name ? name : "", sizeof v->name);
        rval_t **pp = &k->vals; while (*pp) pp = &(*pp)->next; *pp = v;
    }
    kfree(v->data);
    v->data = kmalloc(len ? len : 1); if (len) memcpy(v->data, data, len);
    v->len = len; v->type = type; rtc_now(&k->mtime);
}
static void free_key(rkey_t *k) {
    while (k->child) { rkey_t *c = k->child; k->child = c->next; free_key(c); }
    while (k->vals) { rval_t *v = k->vals; k->vals = v->next; kfree(v->data); kfree(v); }
    kfree(k);
}
static void unlink_key(rkey_t *k) {
    if (!k->parent) return;
    rkey_t **pp = &k->parent->child; while (*pp && *pp != k) pp = &(*pp)->next;
    if (*pp) *pp = k->next;
}

/* ---------------------------------------------------------------- saving and loading */
typedef struct { char *b; uint32_t n, cap; } sbuf_t;
static void sb_put(sbuf_t *s, const char *t, uint32_t n) {
    if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2 + 4096; s->b = krealloc(s->b, s->cap); }
    memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0;
}
static void sb_str(sbuf_t *s, const char *t) { sb_put(s, t, strlen(t)); }
static void sb_esc(sbuf_t *s, const char *t) {
    if (!*t) { sb_str(s, "%"); return; }                 /* the default value: a lone % */
    for (; *t; t++) { char e[4]; if ((uint8_t)*t <= ' ' || *t == '%' || (uint8_t)*t >= 127) { snprintf(e, sizeof e, "%%%02X", (uint8_t)*t); sb_str(s, e); } else sb_put(s, t, 1); }
}
static void save_key(sbuf_t *s, rkey_t *k, const char *path) {
    sb_str(s, "K "); sb_str(s, path); sb_str(s, "\n");
    for (rval_t *v = k->vals; v; v = v->next) {
        char h[16]; snprintf(h, sizeof h, "V %u ", v->type); sb_str(s, h); sb_esc(s, v->name); sb_str(s, " ");
        for (uint32_t i = 0; i < v->len; i++) { snprintf(h, sizeof h, "%02x", v->data[i]); sb_put(s, h, 2); }
        sb_str(s, "\n");
    }
    for (rkey_t *c = k->child; c; c = c->next) {
        char sub[1024]; snprintf(sub, sizeof sub, "%s\\%s", path, c->name);
        save_key(s, c, sub);
    }
}
static void reg_save(void) {
    if (loading) return;
    sbuf_t s = { 0 };
    sb_str(&s, "THE WRLD OS registry 1\n");
    for (int r = 1; r < 6; r++) if (roots[r]) save_key(&s, roots[r], root_names[r]);
    vfs_write_file(REG_PATH, s.b, s.n);
    kfree(s.b);
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; }
static rkey_t *open_root_path(const char *path, bool create);
static void reg_load(void) {
    vnode_t *n = vfs_resolve(REG_PATH, NULL);
    if (!n || n->dir || !n->size) return;
    loading = true;
    char *buf = kmalloc(n->size + 1); memcpy(buf, n->data, n->size); buf[n->size] = 0;
    rkey_t *cur = NULL;
    for (char *line = buf, *nx; line && *line; line = nx) {
        nx = strchr(line, '\n'); if (nx) *nx++ = 0;
        size_t l = strlen(line); if (l && line[l - 1] == '\r') line[l - 1] = 0;
        if (line[0] == 'K' && line[1] == ' ') cur = open_root_path(line + 2, true);
        else if (line[0] == 'V' && line[1] == ' ' && cur) {
            char *q = line + 2; uint32_t type = strtoul(q, &q, 10); while (*q == ' ') q++;
            char name[128]; int k = 0;
            if (q[0] == '%' && (q[1] == ' ' || !q[1])) q++;
            else while (*q && *q != ' ' && k < 127) { if (*q == '%' && q[1] && q[2]) { name[k++] = hexv(q[1]) << 4 | hexv(q[2]); q += 3; } else name[k++] = *q++; }
            name[k] = 0; while (*q == ' ') q++;
            uint32_t len = strlen(q) / 2; uint8_t *d = kmalloc(len + 1);
            for (uint32_t i = 0; i < len; i++) d[i] = hexv(q[2 * i]) << 4 | hexv(q[2 * i + 1]);
            set_val(cur, name, type, d, len); kfree(d);
        }
    }
    kfree(buf);
    loading = false;
}
static void wstr_val(rkey_t *k, const char *name, const char *s) {
    WCHAR w[300]; int i = 0; for (; s[i] && i < 299; i++) w[i] = (uint8_t)s[i]; w[i] = 0;
    set_val(k, name, REG_SZ, w, (i + 1) * 2);
}
static void dword_val(rkey_t *k, const char *name, uint32_t v) { set_val(k, name, REG_DWORD, &v, 4); }
static void reg_init(void) {
    if (loaded) return;
    loaded = true; loading = true;
    for (int r = 1; r < 6; r++) roots[r] = new_key(NULL, root_names[r]);
    /* what programs commonly look up */
    rkey_t *nt = walk(roots[2], "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", true);
    wstr_val(nt, "ProductName", OS_NAME); wstr_val(nt, "CurrentVersion", "6.1"); wstr_val(nt, "CurrentBuild", "7601");
    wstr_val(nt, "CurrentBuildNumber", "7601"); wstr_val(nt, "CSDVersion", "Service Pack 1"); wstr_val(nt, "RegisteredOwner", "User");
    wstr_val(nt, "SystemRoot", "C:\\WRLD"); dword_val(nt, "CurrentMajorVersionNumber", 6); dword_val(nt, "CurrentMinorVersionNumber", 1);
    rkey_t *cv = walk(roots[2], "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", true);
    wstr_val(cv, "ProgramFilesDir", "C:\\Program Files"); wstr_val(cv, "CommonFilesDir", "C:\\Program Files\\Common Files");
    wstr_val(cv, "ProgramFilesDir (x86)", "C:\\Program Files");
    walk(roots[2], "SOFTWARE\\Classes", true);
    walk(roots[2], "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", true);
    rkey_t *cpu = walk(roots[2], "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", true);
    wstr_val(cpu, "ProcessorNameString", "x86 processor"); dword_val(cpu, "~MHz", 2000);
    rkey_t *sf = walk(roots[1], "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", true);
    wstr_val(sf, "Desktop", USER_DESKTOP); wstr_val(sf, "Personal", USER_DOCS); wstr_val(sf, "AppData", USER_HOME "\\Application Data");
    wstr_val(sf, "Local AppData", USER_HOME "\\Local Settings\\Application Data");
    rkey_t *usf = walk(roots[1], "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", true);
    wstr_val(usf, "Desktop", USER_DESKTOP); wstr_val(usf, "Personal", USER_DOCS);
    rkey_t *desk = walk(roots[1], "Control Panel\\Desktop", true);
    wstr_val(desk, "Wallpaper", ""); wstr_val(desk, "WheelScrollLines", "3");
    rkey_t *intl = walk(roots[1], "Control Panel\\International", true);
    wstr_val(intl, "sDecimal", "."); wstr_val(intl, "sThousand", ","); wstr_val(intl, "sShortDate", "M/d/yyyy"); wstr_val(intl, "Locale", "00000409");
    walk(roots[1], "Software", true); walk(roots[1], "Environment", true);
    walk(roots[3], ".DEFAULT", true);
    loading = false;
    reg_load();
}

/* ---------------------------------------------------------------- handles */
#define NHANDLE 256
#define HBASE 0x52000000u
static rkey_t *open_keys[NHANDLE];
static int root_index(uintptr_t h) { uint32_t v = (uint32_t)h; return v >= 0x80000000u && v <= 0x80000005u ? (int)(v - 0x80000000u) : -1; }
static rkey_t *key_of(uintptr_t h) {
    reg_init();
    int r = root_index(h);
    if (r == 0) return walk(roots[2], "SOFTWARE\\Classes", true);
    if (r > 0) return roots[r];
    uint32_t v = (uint32_t)h;
    if (v >= HBASE && v < HBASE + NHANDLE * 4 && !((v - HBASE) & 3)) return open_keys[(v - HBASE) / 4];
    return NULL;
}
static uintptr_t new_handle(rkey_t *k) {
    for (int i = 1; i < NHANDLE; i++) if (!open_keys[i]) { open_keys[i] = k; return HBASE + i * 4; }
    /* every slot taken: reuse the oldest (programs that never close keys) */
    static int rr = 1; int i = rr; rr = rr % (NHANDLE - 1) + 1; open_keys[i] = k; return HBASE + i * 4;
}
/* "HKEY_CURRENT_USER\..." (also HKCU\...) to a key */
static rkey_t *open_root_path(const char *path, bool create) {
    static const char *abbr[6] = { "HKCR", "HKCU", "HKLM", "HKU", "HKPD", "HKCC" };
    for (int r = 0; r < 6; r++) {
        size_t l1 = strlen(root_names[r]), l2 = strlen(abbr[r]); size_t l = 0;
        if (!strncasecmp(path, root_names[r], l1) && (path[l1] == '\\' || !path[l1])) l = l1;
        else if (!strncasecmp(path, abbr[r], l2) && (path[l2] == '\\' || !path[l2])) l = l2;
        if (!l) continue;
        rkey_t *base = r == 0 ? walk(roots[2], "SOFTWARE\\Classes", true) : roots[r];
        return walk(base, path + l, create);
    }
    return NULL;
}

/* ---------------------------------------------------------------- conversions */
static void w2a(const WCHAR *w, char *a, int cap) { int i = 0; if (w) for (; w[i] && i < cap - 1; i++) a[i] = w[i] < 256 ? (char)w[i] : '?'; a[i] = 0; }
static bool is_str(uint32_t t) { return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ; }
/* a value's data as the caller sees it: wide (as stored) or converted to ANSI */
static uint32_t out_len(rval_t *v, bool wide) { return wide || !is_str(v->type) ? v->len : v->len / 2; }
static void out_copy(rval_t *v, bool wide, uint8_t *dst) {
    if (wide || !is_str(v->type)) { memcpy(dst, v->data, v->len); return; }
    const WCHAR *w = (const WCHAR *)v->data;
    for (uint32_t i = 0; i < v->len / 2; i++) dst[i] = w[i] < 256 ? (uint8_t)w[i] : '?';
}
/* data from the caller: ANSI strings are widened (with their terminator) */
static void store(rkey_t *k, const char *name, uint32_t type, const uint8_t *d, uint32_t len, bool wide) {
    if (!wide && is_str(type)) {
        if (len && d) { uint32_t n = len; if (type != REG_MULTI_SZ && d[n - 1]) n++; WCHAR *w = kmalloc(n * 2); for (uint32_t i = 0; i < n; i++) w[i] = i < len ? d[i] : 0; set_val(k, name, type, w, n * 2); kfree(w); }
        else { WCHAR z = 0; set_val(k, name, type, &z, 2); }
    } else if (wide && is_str(type) && len >= 2 && d && (((const WCHAR *)d)[len / 2 - 1])) {   /* not terminated: add one */
        WCHAR *w = kmalloc(len + 2); memcpy(w, d, len); w[len / 2] = 0; set_val(k, name, type, w, len + 2); kfree(w);
    } else set_val(k, name, type, d, len);
    reg_save();
}

/* ---------------------------------------------------------------- API (wide and ANSI share the A path names) */
static LONG open_key(uintptr_t h, const char *sub, uintptr_t *res, bool create, DWORD *disp) {
    rkey_t *k = key_of(h);
    if (!k) return E_BADKEY;
    if (!res) return E_PARAM;
    rkey_t *t = sub && *sub ? walk(k, sub, false) : k;
    if (!t && create) { t = walk(k, sub, true); if (disp) *disp = 1; reg_save(); }
    else if (disp) *disp = 2;
    if (!t) { *res = 0; return E_NOTFOUND; }
    *res = new_handle(t);
    return E_OK;
}
static LONG WINAPI RegOpenKeyExA(uintptr_t h, const char *sub, DWORD o, DWORD sam, uintptr_t *res) { return open_key(h, sub, res, false, NULL); }
static LONG WINAPI RegOpenKeyExW(uintptr_t h, const WCHAR *sub, DWORD o, DWORD sam, uintptr_t *res) { char a[512]; w2a(sub, a, sizeof a); return open_key(h, sub ? a : NULL, res, false, NULL); }
static LONG WINAPI RegOpenKeyA(uintptr_t h, const char *sub, uintptr_t *res) { return open_key(h, sub, res, false, NULL); }
static LONG WINAPI RegOpenKeyW(uintptr_t h, const WCHAR *sub, uintptr_t *res) { return RegOpenKeyExW(h, sub, 0, 0, res); }
static LONG WINAPI RegOpenCurrentUser(DWORD sam, uintptr_t *res) { *res = 0x80000001u; return E_OK; }
static LONG WINAPI RegCreateKeyExA(uintptr_t h, const char *sub, DWORD r, char *cls, DWORD o, DWORD sam, void *sa, uintptr_t *res, DWORD *disp) { return open_key(h, sub, res, true, disp); }
static LONG WINAPI RegCreateKeyExW(uintptr_t h, const WCHAR *sub, DWORD r, WCHAR *cls, DWORD o, DWORD sam, void *sa, uintptr_t *res, DWORD *disp) { char a[512]; w2a(sub, a, sizeof a); return open_key(h, a, res, true, disp); }
static LONG WINAPI RegCreateKeyA(uintptr_t h, const char *sub, uintptr_t *res) { return open_key(h, sub, res, true, NULL); }
static LONG WINAPI RegCreateKeyW(uintptr_t h, const WCHAR *sub, uintptr_t *res) { char a[512]; w2a(sub, a, sizeof a); return open_key(h, a, res, true, NULL); }
static LONG WINAPI RegCloseKey(uintptr_t h) {
    uint32_t v = (uint32_t)h;
    if (v >= HBASE && v < HBASE + NHANDLE * 4) open_keys[(v - HBASE) / 4] = NULL;
    return E_OK;
}
static LONG WINAPI RegFlushKey(uintptr_t h) { return E_OK; }
static LONG query(uintptr_t h, const char *name, DWORD *type, BYTE *data, DWORD *size, bool wide) {
    rkey_t *k = key_of(h);
    if (!k) return E_BADKEY;
    rval_t *v = find_val(k, name);
    if (!v) return E_NOTFOUND;
    if (type) *type = v->type;
    uint32_t n = out_len(v, wide);
    if (!data) { if (size) *size = n; return E_OK; }
    if (!size) return E_PARAM;
    if (*size < n) { *size = n; return E_MORE; }
    out_copy(v, wide, data); *size = n;
    return E_OK;
}
static LONG WINAPI RegQueryValueExA(uintptr_t h, const char *n, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) { return query(h, n, t, d, sz, false); }
static LONG WINAPI RegQueryValueExW(uintptr_t h, const WCHAR *n, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) { char a[256]; w2a(n, a, sizeof a); return query(h, a, t, d, sz, true); }
/* RegQueryValue: the default value of a subkey */
static LONG WINAPI RegQueryValueA(uintptr_t h, const char *sub, char *d, LONG *sz) {
    uintptr_t k; LONG e = open_key(h, sub, &k, false, NULL); if (e) return e;
    DWORD n = sz ? *sz : 0; e = query(k, "", NULL, (BYTE *)d, d ? &n : (DWORD *)sz, false);
    if (e == E_NOTFOUND) { if (d && sz && *sz) d[0] = 0; if (sz) *sz = 1; e = E_OK; } else if (sz && d) *sz = n;
    RegCloseKey(k); return e;
}
static LONG WINAPI RegQueryValueW(uintptr_t h, const WCHAR *sub, WCHAR *d, LONG *sz) {
    char a[512]; w2a(sub, a, sizeof a);
    uintptr_t k; LONG e = open_key(h, sub ? a : NULL, &k, false, NULL); if (e) return e;
    DWORD n = sz ? *sz : 0; e = query(k, "", NULL, (BYTE *)d, d ? &n : (DWORD *)sz, true);
    if (e == E_NOTFOUND) { if (d && sz && *sz >= 2) d[0] = 0; if (sz) *sz = 2; e = E_OK; } else if (sz && d) *sz = n;
    RegCloseKey(k); return e;
}
static LONG WINAPI RegSetValueExA(uintptr_t h, const char *n, DWORD r, DWORD t, const BYTE *d, DWORD sz) { rkey_t *k = key_of(h); if (!k) return E_BADKEY; store(k, n, t, d, sz, false); return E_OK; }
static LONG WINAPI RegSetValueExW(uintptr_t h, const WCHAR *n, DWORD r, DWORD t, const BYTE *d, DWORD sz) { rkey_t *k = key_of(h); if (!k) return E_BADKEY; char a[256]; w2a(n, a, sizeof a); store(k, a, t, d, sz, true); return E_OK; }
static LONG WINAPI RegSetValueA(uintptr_t h, const char *sub, DWORD t, const char *d, DWORD sz) {
    rkey_t *k = key_of(h); if (!k) return E_BADKEY;
    if (sub && *sub) { k = walk(k, sub, true); }
    store(k, "", REG_SZ, (const uint8_t *)d, strlen(d) + 1, false); return E_OK;
}
static LONG WINAPI RegSetValueW(uintptr_t h, const WCHAR *sub, DWORD t, const WCHAR *d, DWORD sz) {
    rkey_t *k = key_of(h); if (!k) return E_BADKEY;
    char a[512]; w2a(sub, a, sizeof a); if (sub && *sub) k = walk(k, a, true);
    int l = 0; while (d[l]) l++;
    store(k, "", REG_SZ, (const uint8_t *)d, (l + 1) * 2, true); return E_OK;
}
static LONG del_value(uintptr_t h, const char *n) {
    rkey_t *k = key_of(h); if (!k) return E_BADKEY;
    for (rval_t **pp = &k->vals; *pp; pp = &(*pp)->next) if (!strcasecmp((*pp)->name, n ? n : "")) {
        rval_t *v = *pp; *pp = v->next; kfree(v->data); kfree(v); reg_save(); return E_OK;
    }
    return E_NOTFOUND;
}
static LONG WINAPI RegDeleteValueA(uintptr_t h, const char *n) { return del_value(h, n); }
static LONG WINAPI RegDeleteValueW(uintptr_t h, const WCHAR *n) { char a[256]; w2a(n, a, sizeof a); return del_value(h, a); }
static void forget_handles(rkey_t *k) {
    for (int i = 0; i < NHANDLE; i++) for (rkey_t *o = open_keys[i]; o; o = o->parent) if (o == k) { open_keys[i] = NULL; break; }
}
static LONG del_key(uintptr_t h, const char *sub, bool tree) {
    rkey_t *k = key_of(h); if (!k) return E_BADKEY;
    rkey_t *t = sub && *sub ? walk(k, sub, false) : k;
    if (!t) return E_NOTFOUND;
    if (!t->parent) return E_ACCESS;                       /* never a root */
    if (t->child && !tree) return E_ACCESS;                /* RegDeleteKey: only a key without subkeys */
    if (tree && t == k) {                                   /* RegDeleteTree(h, NULL): the contents only */
        while (t->child) { rkey_t *c = t->child; t->child = c->next; forget_handles(c); free_key(c); }
        while (t->vals) { rval_t *v = t->vals; t->vals = v->next; kfree(v->data); kfree(v); }
        reg_save(); return E_OK;
    }
    unlink_key(t); forget_handles(t); free_key(t); reg_save();
    return E_OK;
}
static LONG WINAPI RegDeleteKeyA(uintptr_t h, const char *s) { return del_key(h, s, false); }
static LONG WINAPI RegDeleteKeyW(uintptr_t h, const WCHAR *s) { char a[512]; w2a(s, a, sizeof a); return del_key(h, a, false); }
static LONG WINAPI RegDeleteKeyExA(uintptr_t h, const char *s, DWORD sam, DWORD r) { return del_key(h, s, false); }
static LONG WINAPI RegDeleteKeyExW(uintptr_t h, const WCHAR *s, DWORD sam, DWORD r) { return RegDeleteKeyW(h, s); }
static LONG WINAPI RegDeleteTreeA(uintptr_t h, const char *s) { return del_key(h, s, true); }
static LONG WINAPI RegDeleteTreeW(uintptr_t h, const WCHAR *s) { char a[512]; w2a(s, a, sizeof a); return del_key(h, s ? a : NULL, true); }
static LONG enum_key(uintptr_t h, DWORD i, void *name, DWORD *cc, bool wide) {
    rkey_t *k = key_of(h); if (!k) return E_BADKEY;
    rkey_t *c = k->child; while (c && i) { c = c->next; i--; }
    if (!c) return E_NOMORE;
    DWORD l = strlen(c->name);
    if (!name || !cc || *cc <= l) { if (cc) *cc = l + 1; return E_MORE; }
    if (wide) { WCHAR *w = name; for (DWORD j = 0; j <= l; j++) w[j] = (uint8_t)c->name[j]; } else memcpy(name, c->name, l + 1);
    *cc = l;
    return E_OK;
}
static LONG WINAPI RegEnumKeyExA(uintptr_t h, DWORD i, char *n, DWORD *cc, DWORD *r, char *cls, DWORD *clc, void *ft) { if (clc) *clc = 0; return enum_key(h, i, n, cc, false); }
static LONG WINAPI RegEnumKeyExW(uintptr_t h, DWORD i, WCHAR *n, DWORD *cc, DWORD *r, WCHAR *cls, DWORD *clc, void *ft) { if (clc) *clc = 0; return enum_key(h, i, n, cc, true); }
static LONG WINAPI RegEnumKeyA(uintptr_t h, DWORD i, char *n, DWORD c) { DWORD cc = c; return enum_key(h, i, n, &cc, false); }
static LONG WINAPI RegEnumKeyW(uintptr_t h, DWORD i, WCHAR *n, DWORD c) { DWORD cc = c; return enum_key(h, i, n, &cc, true); }
static LONG enum_value(uintptr_t h, DWORD i, void *name, DWORD *cc, DWORD *type, BYTE *data, DWORD *size, bool wide) {
    rkey_t *k = key_of(h); if (!k) return E_BADKEY;
    rval_t *v = k->vals; while (v && i) { v = v->next; i--; }
    if (!v) return E_NOMORE;
    DWORD l = strlen(v->name);
    if (!name || !cc || *cc <= l) { if (cc) *cc = l + 1; return E_MORE; }
    if (wide) { WCHAR *w = name; for (DWORD j = 0; j <= l; j++) w[j] = (uint8_t)v->name[j]; } else memcpy(name, v->name, l + 1);
    *cc = l;
    if (type) *type = v->type;
    uint32_t n = out_len(v, wide);
    if (data) { if (!size || *size < n) { if (size) *size = n; return E_MORE; } out_copy(v, wide, data); }
    if (size) *size = n;
    return E_OK;
}
static LONG WINAPI RegEnumValueA(uintptr_t h, DWORD i, char *n, DWORD *cc, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) { return enum_value(h, i, n, cc, t, d, sz, false); }
static LONG WINAPI RegEnumValueW(uintptr_t h, DWORD i, WCHAR *n, DWORD *cc, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) { return enum_value(h, i, n, cc, t, d, sz, true); }
static LONG info(uintptr_t h, DWORD *nsub, DWORD *maxsub, DWORD *nval, DWORD *maxname, DWORD *maxdata, void *ft) {
    rkey_t *k = key_of(h); if (!k) return E_BADKEY;
    DWORD ns = 0, ms = 0, nv = 0, mn = 0, md = 0;
    for (rkey_t *c = k->child; c; c = c->next) { ns++; ms = MAX(ms, (DWORD)strlen(c->name)); }
    for (rval_t *v = k->vals; v; v = v->next) { nv++; mn = MAX(mn, (DWORD)strlen(v->name)); md = MAX(md, v->len); }
    if (nsub) *nsub = ns; if (maxsub) *maxsub = ms; if (nval) *nval = nv; if (maxname) *maxname = mn; if (maxdata) *maxdata = md;
    if (ft) { uint64_t t = 116444736000000000ull; memcpy(ft, &t, 8); }
    return E_OK;
}
static LONG WINAPI RegQueryInfoKeyA(uintptr_t h, char *cls, DWORD *clc, DWORD *r, DWORD *ns, DWORD *ms, DWORD *mc, DWORD *nv, DWORD *mn, DWORD *md, DWORD *sd, void *ft) {
    if (clc) { if (cls && *clc) cls[0] = 0; *clc = 0; } if (mc) *mc = 0; if (sd) *sd = 0;
    return info(h, ns, ms, nv, mn, md, ft);
}
static LONG WINAPI RegQueryInfoKeyW(uintptr_t h, WCHAR *cls, DWORD *clc, DWORD *r, DWORD *ns, DWORD *ms, DWORD *mc, DWORD *nv, DWORD *mn, DWORD *md, DWORD *sd, void *ft) {
    if (clc) { if (cls && *clc) cls[0] = 0; *clc = 0; } if (mc) *mc = 0; if (sd) *sd = 0;
    return info(h, ns, ms, nv, mn, md, ft);
}
/* RegGetValue: flags RRF_RT_* restrict the type; RRF_NOEXPAND ignored */
static LONG get_value(uintptr_t h, const char *sub, const char *name, DWORD fl, DWORD *type, void *data, DWORD *size, bool wide) {
    uintptr_t k = h; bool opened = false;
    if (sub && *sub) { LONG e = open_key(h, sub, &k, false, NULL); if (e) return e; opened = true; }
    DWORD t = 0; LONG e = query(k, name, &t, data, size, wide);
    if (!e && fl & 0xFFFF) {
        DWORD bit = t == REG_NONE ? 1 : t == REG_SZ ? 2 : t == REG_EXPAND_SZ ? 4 : t == REG_BINARY ? 8 : t == REG_DWORD ? 0x10 : t == REG_MULTI_SZ ? 0x20 : t == REG_QWORD ? 0x40 : 0;
        if (!(fl & bit)) e = 1630;                         /* ERROR_UNSUPPORTED_TYPE */
    }
    if (type) *type = t;
    if (opened) RegCloseKey(k);
    return e;
}
static LONG WINAPI RegGetValueA(uintptr_t h, const char *sub, const char *n, DWORD fl, DWORD *t, void *d, DWORD *sz) { return get_value(h, sub, n, fl, t, d, sz, false); }
static LONG WINAPI RegGetValueW(uintptr_t h, const WCHAR *sub, const WCHAR *n, DWORD fl, DWORD *t, void *d, DWORD *sz) { char a[512], b[256]; w2a(sub, a, sizeof a); w2a(n, b, sizeof b); return get_value(h, sub ? a : NULL, b, fl, t, d, sz, true); }
static LONG WINAPI RegNotifyChangeKeyValue(uintptr_t h, BOOL sub, DWORD f, HANDLE ev, BOOL async) { return E_OK; }
static LONG WINAPI RegConnectRegistryW(const WCHAR *m, uintptr_t h, uintptr_t *res) { if (m && *m) return 53; *res = h; return E_OK; }
static LONG WINAPI RegConnectRegistryA(const char *m, uintptr_t h, uintptr_t *res) { if (m && *m) return 53; *res = h; return E_OK; }
static LONG WINAPI RegOverridePredefKey(uintptr_t h, uintptr_t n) { return E_OK; }
static LONG WINAPI RegDisablePredefinedCache(void) { return E_OK; }
static LONG WINAPI RegLoadKeyW(uintptr_t h, const WCHAR *s, const WCHAR *f) { return E_ACCESS; }
static LONG WINAPI RegSaveKeyW(uintptr_t h, const WCHAR *f, void *sa) { return E_ACCESS; }
static LONG WINAPI RegSaveKeyA(uintptr_t h, const char *f, void *sa) { return E_ACCESS; }
/* shlwapi's registry helpers */
static LONG WINAPI SHGetValueA(uintptr_t h, const char *sub, const char *n, DWORD *t, void *d, DWORD *sz) { return get_value(h, sub, n, 0, t, d, sz, false); }
static LONG WINAPI SHGetValueW(uintptr_t h, const WCHAR *sub, const WCHAR *n, DWORD *t, void *d, DWORD *sz) { return RegGetValueW(h, sub, n, 0, t, d, sz); }
static LONG WINAPI SHSetValueA(uintptr_t h, const char *sub, const char *n, DWORD t, const void *d, DWORD sz) { uintptr_t k; LONG e = open_key(h, sub, &k, true, NULL); if (e) return e; e = RegSetValueExA(k, n, 0, t, d, sz); RegCloseKey(k); return e; }
static LONG WINAPI SHSetValueW(uintptr_t h, const WCHAR *sub, const WCHAR *n, DWORD t, const void *d, DWORD sz) { char a[512]; w2a(sub, a, sizeof a); uintptr_t k; LONG e = open_key(h, a, &k, true, NULL); if (e) return e; e = RegSetValueExW(k, n, 0, t, d, sz); RegCloseKey(k); return e; }
static LONG WINAPI SHDeleteKeyA(uintptr_t h, const char *s) { return del_key(h, s, true); }
static LONG WINAPI SHDeleteKeyW(uintptr_t h, const WCHAR *s) { return RegDeleteTreeW(h, s); }
static LONG WINAPI SHDeleteValueA(uintptr_t h, const char *sub, const char *n) { uintptr_t k; LONG e = open_key(h, sub, &k, false, NULL); if (e) return e; e = del_value(k, n); RegCloseKey(k); return e; }
static LONG WINAPI SHDeleteValueW(uintptr_t h, const WCHAR *sub, const WCHAR *n) { char a[512], b[256]; w2a(sub, a, sizeof a); w2a(n, b, sizeof b); return SHDeleteValueA(h, a, b); }
static LONG WINAPI SHQueryValueExA(uintptr_t h, const char *n, DWORD *r, DWORD *t, void *d, DWORD *sz) { return query(h, n, t, d, sz, false); }
static LONG WINAPI SHQueryValueExW(uintptr_t h, const WCHAR *n, DWORD *r, DWORD *t, void *d, DWORD *sz) { return RegQueryValueExW(h, n, r, t, d, sz); }
static LONG WINAPI SHEnumKeyExW(uintptr_t h, DWORD i, WCHAR *n, DWORD *cc) { return enum_key(h, i, n, cc, true); }
static LONG WINAPI SHEnumKeyExA(uintptr_t h, DWORD i, char *n, DWORD *cc) { return enum_key(h, i, n, cc, false); }

/* the shell's "reg" command and the registry editor use these */
bool reg_get_string(const char *path, const char *name, char *out, int cap) {
    reg_init();
    rkey_t *k = open_root_path(path, false);
    rval_t *v = k ? find_val(k, name) : NULL;
    if (!v || !is_str(v->type)) return false;
    int i = 0; const WCHAR *w = (const WCHAR *)v->data;
    for (; i < cap - 1 && i < (int)(v->len / 2) && w[i]; i++) out[i] = w[i] < 256 ? (char)w[i] : '?';
    out[i] = 0;
    return true;
}
bool reg_set_string(const char *path, const char *name, const char *val) {
    reg_init();
    rkey_t *k = open_root_path(path, true);
    if (!k) return false;
    store(k, name, REG_SZ, (const uint8_t *)val, strlen(val) + 1, false);
    return true;
}
/* print a key and its values (reg query) */
int reg_dump(const char *path, bool recurse, void (*out)(void *ctx, const char *line), void *ctx) {
    reg_init();
    rkey_t *k = open_root_path(path, false);
    if (!k) return -1;
    char line[600];
    snprintf(line, sizeof line, "%s", path); out(ctx, line);
    for (rval_t *v = k->vals; v; v = v->next) {
        static const char *tn[] = { "REG_NONE", "REG_SZ", "REG_EXPAND_SZ", "REG_BINARY", "REG_DWORD", "REG_DWORD_BE", "REG_LINK", "REG_MULTI_SZ" };
        const char *t = v->type < 8 ? tn[v->type] : v->type == REG_QWORD ? "REG_QWORD" : "REG_?";
        char val[300] = "";
        if (is_str(v->type)) { const WCHAR *w = (const WCHAR *)v->data; int i = 0; for (; i < 299 && i < (int)(v->len / 2) && (w[i] || (v->type == REG_MULTI_SZ && i + 1 < (int)(v->len / 2) && w[i + 1])); i++) val[i] = w[i] ? (w[i] < 256 ? (char)w[i] : '?') : ' '; val[i] = 0; }
        else if (v->type == REG_DWORD && v->len >= 4) snprintf(val, sizeof val, "0x%x", *(uint32_t *)v->data);
        else if (v->type == REG_QWORD && v->len >= 8) snprintf(val, sizeof val, "0x%llx", (unsigned long long)*(uint64_t *)v->data);
        else for (uint32_t i = 0; i < v->len && i < 140; i++) snprintf(val + 2 * i, 3, "%02X", v->data[i]);
        snprintf(line, sizeof line, "    %s    %s    %s", v->name[0] ? v->name : "(Default)", t, val); out(ctx, line);
    }
    for (rkey_t *c = k->child; c; c = c->next) {
        char sub[600]; snprintf(sub, sizeof sub, "%s\\%s", path, c->name);
        if (recurse) { out(ctx, ""); reg_dump(sub, true, out, ctx); } else out(ctx, sub);
    }
    return 0;
}
bool reg_delete(const char *path, const char *name) {
    reg_init();
    rkey_t *k = open_root_path(path, false);
    if (!k) return false;
    if (name) { for (rval_t **pp = &k->vals; *pp; pp = &(*pp)->next) if (!strcasecmp((*pp)->name, name)) { rval_t *v = *pp; *pp = v->next; kfree(v->data); kfree(v); reg_save(); return true; } return false; }
    if (!k->parent) return false;
    unlink_key(k); forget_handles(k); free_key(k); reg_save();
    return true;
}

#define R(g, n) { g, #n, (void *)n }
const w32_api_t registry_api[] = {
    R("advapi32", RegOpenKeyExA), R("advapi32", RegOpenKeyExW), R("advapi32", RegOpenKeyA), R("advapi32", RegOpenKeyW), R("advapi32", RegOpenCurrentUser),
    R("advapi32", RegCreateKeyExA), R("advapi32", RegCreateKeyExW), R("advapi32", RegCreateKeyA), R("advapi32", RegCreateKeyW), R("advapi32", RegCloseKey),
    R("advapi32", RegFlushKey), R("advapi32", RegQueryValueExA), R("advapi32", RegQueryValueExW), R("advapi32", RegQueryValueA), R("advapi32", RegQueryValueW),
    R("advapi32", RegSetValueExA), R("advapi32", RegSetValueExW), R("advapi32", RegSetValueA), R("advapi32", RegSetValueW), R("advapi32", RegDeleteValueA),
    R("advapi32", RegDeleteValueW), R("advapi32", RegDeleteKeyA), R("advapi32", RegDeleteKeyW), R("advapi32", RegDeleteKeyExA), R("advapi32", RegDeleteKeyExW),
    R("advapi32", RegDeleteTreeA), R("advapi32", RegDeleteTreeW), R("advapi32", RegEnumKeyExA), R("advapi32", RegEnumKeyExW), R("advapi32", RegEnumKeyA),
    R("advapi32", RegEnumKeyW), R("advapi32", RegEnumValueA), R("advapi32", RegEnumValueW), R("advapi32", RegQueryInfoKeyA), R("advapi32", RegQueryInfoKeyW),
    R("advapi32", RegGetValueA), R("advapi32", RegGetValueW), R("advapi32", RegNotifyChangeKeyValue), R("advapi32", RegConnectRegistryW),
    R("advapi32", RegConnectRegistryA), R("advapi32", RegOverridePredefKey), R("advapi32", RegDisablePredefinedCache), R("advapi32", RegLoadKeyW),
    R("advapi32", RegSaveKeyW), R("advapi32", RegSaveKeyA),
    /* Windows 7 moved these into kernel32 too (through the api-ms-win-core-registry set) */
    R("kernel32", RegOpenKeyExA), R("kernel32", RegOpenKeyExW), R("kernel32", RegCreateKeyExA), R("kernel32", RegCreateKeyExW), R("kernel32", RegCloseKey),
    R("kernel32", RegQueryValueExA), R("kernel32", RegQueryValueExW), R("kernel32", RegSetValueExA), R("kernel32", RegSetValueExW), R("kernel32", RegDeleteValueW),
    R("kernel32", RegEnumKeyExW), R("kernel32", RegEnumValueW), R("kernel32", RegQueryInfoKeyW), R("kernel32", RegGetValueW), R("kernel32", RegDeleteTreeW),
    R("shlwapi", SHGetValueA), R("shlwapi", SHGetValueW), R("shlwapi", SHSetValueA), R("shlwapi", SHSetValueW), R("shlwapi", SHDeleteKeyA), R("shlwapi", SHDeleteKeyW),
    R("shlwapi", SHDeleteValueA), R("shlwapi", SHDeleteValueW), R("shlwapi", SHQueryValueExA), R("shlwapi", SHQueryValueExW), R("shlwapi", SHEnumKeyExW),
    R("shlwapi", SHEnumKeyExA), { "shlwapi", "SHRegGetValueW", (void *)RegGetValueW }, { "shlwapi", "SHRegGetValueA", (void *)RegGetValueA },
    { NULL, NULL, NULL }
};
