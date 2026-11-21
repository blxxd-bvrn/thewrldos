/* Linux software packages: .deb (Debian, Ubuntu, Mint...) and .rpm (Fedora, openSUSE,
 * RHEL...). Reading a package (its control file or RPM header, and its files: a tar or
 * cpio archive compressed with gzip, xz or Zstandard) and checking it are in Rust:
 * rust/src/pkgfmt.rs, with pkg_load and pkg_check in rust/src/kernel/pkgk.rs. This file
 * installs and removes packages, keeps their records and menu entries, and has the
 * dpkg/rpm/apt commands. Installing writes the files under C:\Linux (/ for Linux programs,
 * with /bin, /lib and /sbin folded into /usr as the Linux layer does) and records them
 * in C:\Linux\var\lib\wrld\packages\NAME.list, so the package can be removed again.
 *
 * Before installing, the package is checked against what is here: the packages it
 * depends on (those installed with it and those the OS image carries, listed in
 * base-packages at build time), the shared libraries its programs load, the library
 * versions they were built against (GLIBC_2.39 ...) and their script interpreters.
 * Missing pieces are reported; installing goes ahead if the user wants it.
 *
 * Not done: maintainer scripts (preinst/postinst need a Linux shell), downloading
 * dependencies from a distribution's servers, and true symbolic links (a link is
 * installed as a copy of its target). */
#include "kernel.h"
#include "fs.h"
#include "pkg.h"
#include "unpack.h"
#include "wm.h"
#include "apps.h"

int pkg_db_generation = 1;

/* ------------------------------------------------------------------ text helpers */
static void trim(char *s) {
    char *a = s; while (*a == ' ' || *a == '\t') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    int l = strlen(s); while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0;
}
static void say(pkg_out_fn out, void *ctx, const char *fmt, ...) {
    if (!out) return;
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    out(ctx, b);
}
/* append "s" to a ", "-separated list without repeating an item */
static void list_add(char *list, int cap, const char *s) {
    int l = strlen(s);
    for (const char *p = list; (p = strstr(p, s)); p += l)
        if ((p == list || (p >= list + 2 && p[-2] == ',')) && (p[l] == ',' || !p[l])) return;
    if (list[0]) strlcat(list, ", ", cap);
    strlcat(list, s, cap);
}
/* the value of "Field:" in an RFC 822-style block (deb control, .desktop with '='),
 * with continuation lines joined by '\n'; a new heap string or NULL */
static char *field(const char *text, const char *name, char sep) {
    int nl = strlen(name);
    for (const char *p = text; p && *p; ) {
        if (!strncasecmp(p, name, nl) && p[nl] == sep) {
            const char *v = p + nl + 1, *e = v;
            for (;;) {
                const char *eol = strchr(e, '\n');
                if (!eol) { e += strlen(e); break; }
                if (sep == ':' && (eol[1] == ' ' || eol[1] == '\t')) { e = eol + 1; continue; }
                e = eol; break;
            }
            char *r = strndup(v, e - v);
            trim(r);
            return r;
        }
        p = strchr(p, '\n'); if (p) p++;
    }
    return NULL;
}
static void field_to(const char *text, const char *name, char *dst, int cap) {
    char *v = field(text, name, ':');
    if (v) { char *nl = strchr(v, '\n'); if (nl) *nl = 0; strlcpy(dst, v, cap); kfree(v); }
}

/* paths and the parts of a package the installer reads: Rust (rust/src/pkgfmt.rs) */
void pkg_canon(char *p, int cap);
void pkg_join_link(const char *linkpath, const char *target, char *out, int cap);
void pkg_lx_to_wrld(const char *lpath, char *out, int cap);
bool pkg_desktop_entry(const uint8_t *data, uint32_t size, char *name, int ncap, char *cmd, int ccap, bool *terminal);
void pkg_alternatives(const char *script, void (*fn)(void *ctx, const char *link, const char *path), void *ctx);
#define canon pkg_canon
#define join_link pkg_join_link
#define lx_to_wrld pkg_lx_to_wrld
static vnode_t *lx_resolve(const char *lp) { char w[600]; lx_to_wrld(lp, w, sizeof w); return vfs_resolve(w, NULL); }

/* The layout of pkg_t, pkg_entry_t and pkg_check_t for the Rust copies in
 * rust/src/kernel/pkgk.rs, which rust_init checks at boot. */
const uint32_t rust_pkg_layout[11] = {
    sizeof(pkg_t), offsetof(pkg_t, desc), offsetof(pkg_t, payload), offsetof(pkg_t, ents), offsetof(pkg_t, nfiles), offsetof(pkg_t, err),
    sizeof(pkg_entry_t), offsetof(pkg_entry_t, data), offsetof(pkg_entry_t, type), sizeof(pkg_check_t), offsetof(pkg_check_t, foreign_elf),
};
static uint32_t hstr(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
void pkg_free(pkg_t *p) {
    kfree(p->desc); kfree(p->depends); kfree(p->provides); kfree(p->payload); kfree(p->ents); kfree(p->pidx); kfree(p->bidx); kfree(p->postinst);
    p->desc = p->depends = p->provides = p->postinst = NULL; p->payload = NULL; p->ents = NULL; p->nents = p->cap = 0;
    p->pidx = p->bidx = NULL; p->icap = 0;
}
bool pkg_arch_ok(const pkg_t *p) {
    return !strcmp(p->arch, "amd64") || !strcmp(p->arch, "x86_64") || !strcmp(p->arch, "all") || !strcmp(p->arch, "noarch") || !p->arch[0];
}

/* ------------------------------------------------------------------ the package database */
typedef struct {
    char name[64], version[96], arch[24], summary[160];
    char fmt;                     /* 'd' / 'r' */
    char *provides, *depends;
} pkgrec_t;
static pkgrec_t *db; static int db_n, db_loaded_gen;
static char *base_list;           /* "name version\n..." of the packages the OS image carries */
static pkg_menu_t menu[48]; static int menu_n;

static void cut_lists(char *t) {
    char *a = strstr(t, "\nFile: "), *b = strstr(t, "\nDir: ");
    char *c = a && b ? (a < b ? a : b) : a ? a : b;
    if (c) c[1] = 0;
}
static void db_clear(void) {
    for (int i = 0; i < db_n; i++) { kfree(db[i].provides); kfree(db[i].depends); }
    kfree(db); db = NULL; db_n = 0; menu_n = 0;
}
static void db_load(void) {
    if (db_loaded_gen == pkg_db_generation) return;
    db_clear();
    db_loaded_gen = pkg_db_generation;
    if (!base_list) { uint32_t sz; base_list = (char *)vfs_read_file(PKG_BASE_LIST, &sz); if (!base_list) base_list = strdup(""); }
    vnode_t *dir = vfs_resolve(PKG_DB_DIR, NULL);
    int cnt = 0; for (vnode_t *k = dir ? dir->child : NULL; k; k = k->next) cnt++;
    if (!cnt) return;
    db = kzalloc(sizeof *db * cnt);
    for (vnode_t *k = dir->child; k; k = k->next) {
        if (k->dir || !str_ends_with_ci(k->name, ".list") || !k->size) continue;
        char *t = kmalloc(k->size + 1); memcpy(t, k->data, k->size); t[k->size] = 0;
        /* the header ends where the lists of folders and files begin */
        cut_lists(t);
        pkgrec_t *r = &db[db_n++];
        field_to(t, "Package", r->name, sizeof r->name);
        field_to(t, "Version", r->version, sizeof r->version);
        field_to(t, "Architecture", r->arch, sizeof r->arch);
        field_to(t, "Summary", r->summary, sizeof r->summary);
        char f[8] = ""; field_to(t, "Format", f, sizeof f); r->fmt = f[0] == 'r' ? 'r' : 'd';
        r->provides = field(t, "Provides", ':'); r->depends = field(t, "Depends", ':');
        if (!r->name[0]) db_n--;
        /* menu entries: "Desktop: name\texec\tterminal" */
        for (char *q = t; (q = strstr(q, "\nDesktop: ")) && menu_n < (int)ARRAY_LEN(menu); q++) {
            char line[400]; const char *v = q + 10; const char *e = strchr(v, '\n'); int l = MIN(e ? (int)(e - v) : (int)strlen(v), 399);
            memcpy(line, v, l); line[l] = 0;
            char *t1 = strchr(line, '\t'); if (!t1) continue; *t1++ = 0;
            char *t2 = strchr(t1, '\t'); if (t2) *t2++ = 0;
            pkg_menu_t *m = &menu[menu_n++];
            strlcpy(m->name, line, sizeof m->name); strlcpy(m->exec, t1, sizeof m->exec);
            m->terminal = t2 && t2[0] == '1'; strlcpy(m->pkg, r->name, sizeof m->pkg);
        }
        kfree(t);
    }
}
int pkg_installed_count(void) { db_load(); return db_n; }
bool pkg_installed_get(int i, char *name, int ncap, char *version, int vcap, char *summary, int scap, char *fmt) {
    db_load();
    if (i < 0 || i >= db_n) return false;
    if (name) strlcpy(name, db[i].name, ncap);
    if (version) strlcpy(version, db[i].version, vcap);
    if (summary) strlcpy(summary, db[i].summary, scap);
    if (fmt) *fmt = db[i].fmt;
    return true;
}
static pkgrec_t *db_find(const char *name) { db_load(); for (int i = 0; i < db_n; i++) if (!strcmp(db[i].name, name)) return &db[i]; return NULL; }
bool pkg_is_installed(const char *name, char *version, int vcap) {
    pkgrec_t *r = db_find(name);
    if (r && version) strlcpy(version, r->version, vcap);
    return r != NULL;
}
static bool in_list(const char *list, const char *name) {   /* "a, b (= 1), c" */
    if (!list) return false;
    int l = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += l) {
        bool start = p == list || p[-1] == ' ' || p[-1] == ',';
        bool end = !p[l] || p[l] == ',' || p[l] == ' ' || p[l] == '(' || p[l] == ':';
        if (start && end) return true;
    }
    return false;
}
/* a dependency is met by an installed package, something one provides, or the OS image
 * (pkg_check in Rust asks) */
bool pkg_dep_met(const char *name) {
    db_load();
    char n[96]; strlcpy(n, name, sizeof n);
    char *colon = strchr(n, ':'); if (colon) *colon = 0;                 /* libc6:any */
    if (db_find(n)) return true;
    for (int i = 0; i < db_n; i++) if (in_list(db[i].provides, n)) return true;
    /* tools only the setup scripts use, and the scripts are not run here */
    static const char *setup_only[] = { "debconf", "debconf-2.0", "cdebconf", "dpkg", "install-info", "ucf",
                                        "init-system-helpers", "lsb-base", "adduser", "sysvinit-utils" };
    for (unsigned i = 0; i < ARRAY_LEN(setup_only); i++) if (!strcmp(n, setup_only[i])) return true;
    if (base_list) {
        int l = strlen(n);
        for (const char *p = base_list; (p = strstr(p, n)); p += l)
            if ((p == base_list || p[-1] == '\n') && (p[l] == ' ' || p[l] == '\n' || !p[l])) return true;
    }
    return false;
}
int pkg_dependents(const char *name, char *out, int cap) {
    db_load();
    int k = 0; if (out && cap) out[0] = 0;
    for (int i = 0; i < db_n; i++) {
        if (!strcmp(db[i].name, name) || !db[i].depends) continue;
        if (in_list(db[i].depends, name)) { if (out) list_add(out, cap, db[i].name); k++; }
    }
    return k;
}

/* a set of paths (the files other packages own) */
typedef struct { char **v; int n, cap; } pathset_t;
static void set_add(pathset_t *s, const char *p) {
    if (s->n * 2 >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 1024;
        char **nv = kzalloc(sizeof(char *) * nc);
        for (int i = 0; i < s->cap; i++) if (s->v[i]) { uint32_t h = hstr(s->v[i]) & (nc - 1); while (nv[h]) h = (h + 1) & (nc - 1); nv[h] = s->v[i]; }
        kfree(s->v); s->v = nv; s->cap = nc;
    }
    uint32_t h = hstr(p) & (s->cap - 1);
    while (s->v[h]) { if (!strcmp(s->v[h], p)) return; h = (h + 1) & (s->cap - 1); }
    s->v[h] = strdup(p); s->n++;
}
static bool set_has(const pathset_t *s, const char *p) {
    if (!s->cap) return false;
    uint32_t h = hstr(p) & (s->cap - 1);
    while (s->v[h]) { if (!strcmp(s->v[h], p)) return true; h = (h + 1) & (s->cap - 1); }
    return false;
}
static void set_free(pathset_t *s) { for (int i = 0; i < s->cap; i++) kfree(s->v[i]); kfree(s->v); memset(s, 0, sizeof *s); }
/* ------------------------------------------------------------------ installing */
static int split_words(char *s, char **argv, int max);
/* the "File:" lines of a record into a set */
static void record_files(const char *name, pathset_t *s, bool dirs) {
    char rp[300]; snprintf(rp, sizeof rp, "%s\\%s.list", PKG_DB_DIR, name);
    vnode_t *n = vfs_resolve(rp, NULL);
    if (!n || !n->size) return;
    const char *key = dirs ? "Dir: " : "File: "; int kl = strlen(key);
    for (uint32_t off = 0; off < n->size; ) {
        const char *line = (const char *)n->data + off;
        const char *e = memchr(line, '\n', n->size - off); uint32_t l = e ? (uint32_t)(e - line) : n->size - off;
        if (l > (uint32_t)kl && !memcmp(line, key, kl) && l - kl < 400) { char p[400]; memcpy(p, line + kl, l - kl); p[l - kl] = 0; set_add(s, p); }
        off += l + 1;
    }
}

typedef struct { char *buf; int len, cap; } sbuf_t;
static void sb_add(sbuf_t *b, const char *s) {
    int l = strlen(s);
    if (b->len + l + 1 > b->cap) { int nc = MAX(b->cap * 2, b->len + l + 4096); char *n = krealloc(b->buf, nc); if (!n) return; b->buf = n; b->cap = nc; }
    memcpy(b->buf + b->len, s, l + 1); b->len += l;
}
static void sb_addf(sbuf_t *b, const char *fmt, ...) { char t[700]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap); sb_add(b, t); }

/* create the folders above a WRLD path, noting the ones that are new (Linux paths) */
static bool make_parents(const char *lpath, sbuf_t *rec) {
    char dir[512]; strlcpy(dir, lpath, sizeof dir);
    char *sl = strrchr(dir, '/'); if (!sl || sl == dir) return true;
    *sl = 0;
    if (lx_resolve(dir)) return true;
    /* record each new level from the top down */
    char part[512] = "";
    for (char *p = dir + 1; ; ) {
        char *e = strchr(p, '/');
        strlcat(part, "/", sizeof part);
        if (e) { char c = *e; *e = 0; strlcat(part, p, sizeof part); *e = c; } else strlcat(part, p, sizeof part);
        if (!lx_resolve(part)) {
            char w[600]; lx_to_wrld(part, w, sizeof w);
            if (!vfs_mkdir_p(w)) return false;
            sb_addf(rec, "Dir: %s\n", part);
        }
        if (!e) break;
        p = e + 1;
    }
    return true;
}
static bool name_ok(const char *lpath) {
    for (const char *p = lpath; *p; ) {
        const char *e = strchr(p + 1, '/'); int l = e ? (int)(e - p - 1) : (int)strlen(p + 1);
        if (l >= 95) return false;                                   /* the WRLD file system's name limit */
        if (!e) break;
        p = e;
    }
    return true;
}

typedef struct {
    pkg_t *p; sbuf_t hdr, rec; pathset_t others, mine_old, written;
    int written_n, kept, conflicts, links_copied, links_skipped, long_names;
    char conflict_with[64];
    pkg_out_fn out; void *ctx;
} inst_t;
/* write one file, unless another package or the OS owns that path */
static void put_file(inst_t *s, const char *lpath, const uint8_t *data, uint32_t size) {
    if (!name_ok(lpath)) { s->long_names++; return; }
    vnode_t *old = lx_resolve(lpath);
    if (old && old->dir) { s->kept++; return; }
    if (old && set_has(&s->others, lpath)) { s->conflicts++; return; }
    if (old && !set_has(&s->mine_old, lpath) && !set_has(&s->written, lpath)) { s->kept++; return; }   /* the OS's own copy stays */
    if (!make_parents(lpath, &s->rec)) return;
    char w[600]; lx_to_wrld(lpath, w, sizeof w);
    if (!vfs_write_file(w, data, size)) return;
    if (!set_has(&s->written, lpath)) { sb_addf(&s->rec, "File: %s\n", lpath); set_add(&s->written, lpath); s->written_n++; }
}
static void desktop_entries(inst_t *s) {
    for (int i = 0; i < s->p->nents; i++) {
        pkg_entry_t *e = &s->p->ents[i];
        if (e->type != PE_FILE || strncmp(e->path, "/usr/share/applications/", 24) || !str_ends_with_ci(e->path, ".desktop")) continue;
        char name[700], cmd[256]; bool term = false;
        if (pkg_desktop_entry(e->data, e->size, name, sizeof name, cmd, sizeof cmd, &term)) sb_addf(&s->hdr, "Desktop: %s\t%s\t%d\n", name, cmd, term);
    }
}
/* What setup scripts most often do for a command-line program is choose its command
 * name: "update-alternatives --install /usr/bin/figlet figlet /usr/bin/figlet-figlet 50"
 * (with --slave pairs for its manual pages). Those lines are carried out here, as copies,
 * when the name is still free; the rest of the scripts is not run. */
typedef struct { inst_t *s; int made; } altctx_t;
static void alternative(void *ctx, const char *lc, const char *pc) {
    altctx_t *a = ctx;
    if (lx_resolve(lc)) return;                                     /* that name is taken */
    vnode_t *t = lx_resolve(pc);
    if (!t || t->dir) return;
    put_file(a->s, lc, t->data, t->size);
    if (lx_resolve(lc)) a->made++;
}
static int alternatives(inst_t *s) {
    if (!s->p->postinst) return 0;
    altctx_t a = { s, 0 };
    pkg_alternatives(s->p->postinst, alternative, &a);
    return a.made;
}
static bool installing;
bool pkg_install(pkg_t *p, pkg_out_fn out, void *ctx, volatile int *pct, char *msg, int mcap) {
    if (ARCH_BITS != 64) { snprintf(msg, mcap, "Linux programs run on the 64-bit edition of " OS_NAME "."); return false; }
    if (!pkg_arch_ok(p)) { snprintf(msg, mcap, "This package is for %s processors; " OS_NAME " runs x86-64 (amd64) Linux programs.", p->arch); return false; }
    if (installing) { snprintf(msg, mcap, "Another package is being installed. Try again when it has finished."); return false; }
    installing = true;
    db_load();
    inst_t s = { .p = p, .out = out, .ctx = ctx };
    /* what other packages own, and what an older version of this one installed */
    for (int i = 0; i < db_n; i++) {
        if (!strcmp(db[i].name, p->name)) record_files(db[i].name, &s.mine_old, false);
        else record_files(db[i].name, &s.others, false);
    }
    pathset_t old_dirs = { 0 };
    record_files(p->name, &old_dirs, true);
    say(out, ctx, "Unpacking %s (%s) ...", p->name, p->version);
    sb_addf(&s.hdr, "Package: %s\nVersion: %s\nArchitecture: %s\nFormat: %s\nSummary: %s\nInstalled-Size: %u\nSource-File: %s\n",
            p->name, p->version, p->arch, p->fmt == 'r' ? "rpm" : "deb", p->summary, p->inst_kb, path_basename(p->file));
    if (p->provides && p->provides[0]) { char *v = strdup(p->provides); for (char *q = v; *q; q++) if (*q == '\n') *q = ' '; sb_addf(&s.hdr, "Provides: %s\n", v); kfree(v); }
    if (p->depends && p->depends[0]) sb_addf(&s.hdr, "Depends: %s\n", p->depends);
    /* folders kept from an older version stay recorded */
    for (int i = 0; i < old_dirs.cap; i++) if (old_dirs.v[i]) sb_addf(&s.rec, "Dir: %s\n", old_dirs.v[i]);
    set_free(&old_dirs);
    /* 1: folders and files */
    int done = 0, total = p->nents ? p->nents : 1;
    for (int i = 0; i < p->nents; i++) {
        pkg_entry_t *e = &p->ents[i];
        if (e->type == PE_DIR) {
            if (!name_ok(e->path)) { s.long_names++; continue; }
            if (!lx_resolve(e->path)) { char probe[520]; snprintf(probe, sizeof probe, "%s/x", e->path); make_parents(probe, &s.rec); }
        } else if (e->type == PE_FILE) put_file(&s, e->path, e->data, e->size);
        if (pct) *pct = 5 + 80 * ++done / total;
        if (!(i & 31)) unpack_yield();
    }
    /* 2: hard links: another name for a file in the package */
    for (int i = 0; i < p->nents; i++) {
        pkg_entry_t *e = &p->ents[i];
        if (e->type != PE_HARDLINK) continue;
        const pkg_entry_t *t = pkg_find_entry(p, e->link);
        if (t && t->type == PE_FILE) put_file(&s, e->path, t->data, t->size);
    }
    /* 3: symbolic links, as copies of what they point to (links to links: a few rounds) */
    bool *done_link = kzalloc(sizeof(bool) * (p->nents + 1));      /* bool is an int here */
    for (int round = 0; round < 4; round++) {
        bool progress = false;
        for (int i = 0; i < p->nents; i++) {
            pkg_entry_t *e = &p->ents[i];
            if (e->type != PE_SYMLINK || done_link[i]) continue;
            char target[512]; join_link(e->path, e->link, target, sizeof target);
            vnode_t *t = lx_resolve(target);
            if (!t) continue;                                            /* maybe a later link makes it */
            done_link[i] = true; progress = true;
            if (!t->dir) { put_file(&s, e->path, t->data, t->size); s.links_copied++; continue; }
            /* a link to a folder: copy it when small and not there yet */
            if (lx_resolve(e->path) || !name_ok(e->path)) { s.links_skipped++; continue; }
            if (vfs_tree_size(t) > (16u << 20)) { s.links_skipped++; continue; }
            make_parents(e->path, &s.rec);
            char pw[600]; lx_to_wrld(e->path, pw, sizeof pw);
            char parent[600]; path_dirname(pw, parent, sizeof parent);
            vnode_t *pd = vfs_resolve(parent, NULL);
            if (pd && vfs_copy(t, pd, path_basename(pw))) { sb_addf(&s.rec, "File: %s\n", e->path); s.links_copied++; }
        }
        if (!progress) break;
    }
    for (int i = 0; i < p->nents; i++) if (p->ents[i].type == PE_SYMLINK && !done_link[i]) s.links_skipped++;
    kfree(done_link);
    int alts = alternatives(&s);
    if (pct) *pct = 90;
    /* 4: an older version's files that the new one no longer has */
    int removed_old = 0;
    for (int i = 0; i < s.mine_old.cap; i++) {
        const char *f = s.mine_old.v[i];
        if (!f || set_has(&s.written, f)) continue;
        vnode_t *n = lx_resolve(f); if (n && vfs_delete(n)) removed_old++;
    }
    desktop_entries(&s);
    /* 5: the record */
    vfs_mkdir_p(PKG_DB_DIR);
    char rp[300]; snprintf(rp, sizeof rp, "%s\\%s.list", PKG_DB_DIR, p->name);
    if (s.rec.buf) sb_add(&s.hdr, s.rec.buf);                   /* the header first: the list is read up to the files */
    bool ok = s.hdr.buf && vfs_write_file(rp, s.hdr.buf, s.hdr.len);
    pkg_db_generation++;
    if (pct) *pct = 100;
    say(out, ctx, "Setting up %s (%s): %d files installed.", p->name, p->version, s.written_n);
    if (s.kept) say(out, ctx, "  %d files the system already has were kept.", s.kept);
    if (s.conflicts) say(out, ctx, "  %d files belong to other packages and were not replaced.", s.conflicts);
    if (s.links_copied) say(out, ctx, "  %d symbolic links were installed as copies of their targets.", s.links_copied);
    if (s.links_skipped) say(out, ctx, "  %d symbolic links point to things that are not installed and were left out.", s.links_skipped);
    if (s.long_names) say(out, ctx, "  %d files have names too long for the file system and were left out.", s.long_names);
    if (removed_old) say(out, ctx, "  %d files of the previous version were removed.", removed_old);
    if (alts) say(out, ctx, "  %d commands were set up from the package's update-alternatives lines.", alts);
    if (p->scripts[0]) say(out, ctx, "  The package's setup scripts (%s) were not run.", p->scripts);
    snprintf(msg, mcap, ok ? "%s %s was installed." : "%s %s was installed, but its record could not be saved.", p->name, p->version);
    kfree(s.rec.buf); kfree(s.hdr.buf); set_free(&s.others); set_free(&s.mine_old); set_free(&s.written);
    installing = false;
    return ok;
}

/* ------------------------------------------------------------------ removing */
static int path_depth(const char *s) { int d = 0; while (*s) if (*s++ == '/') d++; return d; }
bool pkg_remove(const char *name, pkg_out_fn out, void *ctx, char *msg, int mcap) {
    if (!db_find(name)) { snprintf(msg, mcap, "%s is not installed.", name); return false; }
    if (installing) { snprintf(msg, mcap, "A package is being installed. Try again when it has finished."); return false; }
    pathset_t files = { 0 }, dirs = { 0 };
    record_files(name, &files, false);
    record_files(name, &dirs, true);
    say(out, ctx, "Removing %s ...", name);
    int removed = 0, k = 0;
    for (int i = 0; i < files.cap; i++) {
        if (!files.v[i]) continue;
        vnode_t *n = lx_resolve(files.v[i]);
        if (n && vfs_delete(n)) removed++;
        if (!(++k & 63)) unpack_yield();
    }
    /* the folders it created, deepest first, when they are empty */
    for (int pass = 0; pass < 2; pass++) {
        int maxd = 0; for (int i = 0; i < dirs.cap; i++) if (dirs.v[i]) maxd = MAX(maxd, path_depth(dirs.v[i]));
        for (int dpt = maxd; dpt > 1; dpt--)
            for (int i = 0; i < dirs.cap; i++) {
                if (!dirs.v[i] || path_depth(dirs.v[i]) != dpt) continue;
                vnode_t *n = lx_resolve(dirs.v[i]);
                if (n && n->dir && !n->child) vfs_delete(n);
            }
    }
    set_free(&files); set_free(&dirs);
    char rp[300]; snprintf(rp, sizeof rp, "%s\\%s.list", PKG_DB_DIR, name);
    vnode_t *r = vfs_resolve(rp, NULL); if (r) vfs_delete(r);
    pkg_db_generation++;
    snprintf(msg, mcap, "%s was removed (%d files).", name, removed);
    say(out, ctx, "%s", msg);
    return true;
}

/* ------------------------------------------------------------------ menu entries */
int pkg_menu_count(void) { db_load(); return menu_n; }
const pkg_menu_t *pkg_menu_get(int i) { db_load(); return i >= 0 && i < menu_n ? &menu[i] : NULL; }
/* split a command line into words (double and single quotes) */
static int split_words(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ') s++;
        if (!*s) break;
        char *w = s, *o = s; char q = 0;
        while (*s && (q || *s != ' ')) {
            if (!q && (*s == '"' || *s == '\'')) { q = *s++; continue; }
            if (q && *s == q) { q = 0; s++; continue; }
            if (*s == '\\' && s[1] && q != '\'') s++;
            *o++ = *s++;
        }
        if (*s) s++;
        *o = 0; argv[n++] = w;
    }
    argv[n] = NULL;
    return n;
}
/* a command name -> its Linux path (absolute, or found in the usual folders) */
static bool find_program(const char *cmd, char *lpath, int cap) {
    if (cmd[0] == '/') { strlcpy(lpath, cmd, cap); return lx_resolve(cmd) != NULL; }
    static const char *dirs[] = { "/usr/local/bin", "/usr/bin", "/usr/games", "/usr/sbin" };
    for (unsigned i = 0; i < ARRAY_LEN(dirs); i++) { snprintf(lpath, cap, "%s/%s", dirs[i], cmd); if (lx_resolve(lpath)) return true; }
    return false;
}
bool pkg_menu_launch(int i, char *err, int ecap) {
    const pkg_menu_t *m = pkg_menu_get(i);
    if (!m) { snprintf(err, ecap, "That program is no longer installed."); return false; }
#ifdef __x86_64__
    char cmd[256]; strlcpy(cmd, m->exec, sizeof cmd);
    char *argv[24]; int argc = split_words(cmd, argv, 24);
    if (!argc) { snprintf(err, ecap, "The program's menu entry has no command."); return false; }
    char lp[300];
    if (!find_program(argv[0], lp, sizeof lp)) { snprintf(err, ecap, "%s was not found. Reinstalling %s may help.", argv[0], m->pkg); return false; }
    if (m->terminal) {                                           /* a text program: in a Command Prompt */
        char line[600]; char w[400]; lx_to_wrld(lp, w, sizeof w);
        snprintf(line, sizeof line, "\"%s\"", w);
        for (int k = 1; k < argc; k++) { strlcat(line, " \"", sizeof line); strlcat(line, argv[k], sizeof line); strlcat(line, "\"", sizeof line); }
        terminal_run(line, USER_HOME, m->name);
        return true;
    }
    /* a window program: through the Wayland compositor when the image has it */
    extern int lx_launch(const char *wrld_path, int argc, char **argv, char **envp, char *err, int ecap);
    static const char comp[] = "C:\\Linux\\usr\\local\\bin\\wrldcomp";
    char *env[] = { "PATH=/usr/local/bin:/usr/bin:/bin:/usr/games", "HOME=/home/user", "USER=user", "LOGNAME=user", "SHELL=/bin/sh",
                    "LANG=C.UTF-8", "PWD=/home/user", "XDG_DATA_DIRS=/usr/local/share:/usr/share", "GDK_BACKEND=wayland",
                    "QT_QPA_PLATFORM=wayland", "SDL_VIDEODRIVER=wayland", NULL };
    char *av[28]; int ac = 0;
    bool use_comp = vfs_resolve(comp, NULL) != NULL;
    if (use_comp) av[ac++] = "wrldcomp";
    av[ac++] = lp;
    for (int k = 1; k < argc && ac < 27; k++) av[ac++] = argv[k];
    av[ac] = NULL;
    char w[400]; lx_to_wrld(lp, w, sizeof w);
    int pid = lx_launch(use_comp ? comp : w, ac, av, env, err, ecap);
    return pid >= 0;
#else
    snprintf(err, ecap, "Linux programs run on the 64-bit edition of " OS_NAME ".");
    return false;
#endif
}

/* ------------------------------------------------------------------ dpkg, rpm, apt */
typedef struct { pkg_out_fn out; void *ctx; } cli_t;
#define P(...) say(c->out, c->ctx, __VA_ARGS__)
static void resolve_arg(const char *cwd, const char *a, char *out, int cap) {
    vnode_t *n = vfs_resolve(a, cwd);
    if (n) vfs_path(n, out, cap); else strlcpy(out, a, cap);
}
static void print_info(cli_t *c, const pkg_t *p) {
    P(" Package: %s", p->name);
    P(" Version: %s", p->version);
    P(" Architecture: %s", p->arch);
    if (p->maint[0]) P(" %s: %s", p->fmt == 'r' ? "Packager" : "Maintainer", p->maint);
    if (p->inst_kb) P(" Installed-Size: %u KB", p->inst_kb);
    if (p->section[0]) P(" %s: %s", p->fmt == 'r' ? "Group" : "Section", p->section);
    if (p->license[0]) P(" License: %s", p->license);
    if (p->homepage[0]) P(" Homepage: %s", p->homepage);
    if (p->depends && p->depends[0]) P(" %s: %s", p->fmt == 'r' ? "Requires" : "Depends", p->depends);
    P(" Files: %d (%s compressed)", p->nfiles, p->comp);
    if (p->scripts[0]) P(" Scripts: %s (not run by this installer)", p->scripts);
    P(" Summary: %s", p->summary);
    if (p->desc) {
        char *d = strdup(p->desc), *q = d;
        for (int lines = 0; q && *q && lines < 20; lines++) { char *e = strchr(q, '\n'); if (e) *e = 0; P("  %s", q); q = e ? e + 1 : NULL; }
        kfree(d);
    }
}
static void print_check(cli_t *c, const pkg_check_t *k) {
    if (k->missing_pkgs[0]) P("  Needs packages that are not installed: %s", k->missing_pkgs);
    if (k->missing_libs[0]) P("  Needs libraries that are not installed: %s", k->missing_libs);
    if (k->missing_vers[0]) P("  Needs newer libraries than these: %s", k->missing_vers);
    if (k->missing_interp[0]) P("  Needs interpreters that are not installed: %s", k->missing_interp);
    if (k->foreign_elf) P("  Holds programs for another processor (32-bit or not x86-64), which cannot run.");
}
static int cli_install(cli_t *c, const char *cwd, int argc, char **argv, int from) {
    int fails = 0;
    for (int i = from; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char path[300]; resolve_arg(cwd, argv[i], path, sizeof path);
        pkg_t *p = kzalloc(sizeof *p);
        P("Reading %s ...", path_basename(path));
        if (!pkg_load(path, p)) { P("error: %s", p->err); pkg_free(p); kfree(p); fails++; continue; }
        if (!pkg_arch_ok(p)) { P("error: %s is for %s; " OS_NAME " runs amd64 / x86_64 packages.", p->name, p->arch); pkg_free(p); kfree(p); fails++; continue; }
        pkg_check_t k; pkg_check(p, &k);
        if (!k.ok) { P("warning: %s may not work until what it needs is installed:", p->name); print_check(c, &k); }
        char msg[200];
        if (!pkg_install(p, c->out, c->ctx, NULL, msg, sizeof msg)) { P("error: %s", msg); fails++; }
        else if (k.programs[0]) P("Commands: %s", k.programs);
        pkg_free(p); kfree(p);
    }
    return fails ? 1 : 0;
}
static int cli_list(cli_t *c, const char *pattern) {
    int n = pkg_installed_count();
    if (!n) { P("No Linux packages are installed. Install one with: dpkg -i FILE.deb  or  rpm -i FILE.rpm"); return 0; }
    P("Name                   Version          Fmt  Description");
    for (int i = 0; i < n; i++) {
        char nm[64], ver[96], sum[160], f;
        pkg_installed_get(i, nm, sizeof nm, ver, sizeof ver, sum, sizeof sum, &f);
        if (pattern && !strstr(nm, pattern)) continue;
        P("%-22.22s %-16.16s %-4s %.34s", nm, ver, f == 'r' ? "rpm" : "deb", sum);
    }
    return 0;
}
static int cli_files(cli_t *c, const char *name) {
    pathset_t s = { 0 };
    if (!db_find(name)) { P("%s is not installed.", name); return 1; }
    record_files(name, &s, false);
    /* in a stable order: sorted */
    char **v = kmalloc(sizeof(char *) * (s.n + 1)); int k = 0;
    for (int i = 0; i < s.cap; i++) if (s.v[i]) v[k++] = s.v[i];
    for (int i = 1; i < k; i++) { char *t = v[i]; int j = i - 1; while (j >= 0 && strcmp(v[j], t) > 0) { v[j + 1] = v[j]; j--; } v[j + 1] = t; }
    for (int i = 0; i < k; i++) P("%s", v[i]);
    kfree(v); set_free(&s);
    return 0;
}
static int cli_remove(cli_t *c, int argc, char **argv, int from) {
    bool force = false; int fails = 0;
    for (int i = from; i < argc; i++) if (!strcmp(argv[i], "--force-depends") || !strcmp(argv[i], "--nodeps") || !strcmp(argv[i], "-f")) force = true;
    for (int i = from; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char deps[256];
        if (!force && pkg_dependents(argv[i], deps, sizeof deps)) { P("error: %s is needed by %s (use --force-depends to remove it anyway).", argv[i], deps); fails++; continue; }
        char msg[200];
        if (!pkg_remove(argv[i], c->out, c->ctx, msg, sizeof msg)) { P("error: %s", msg); fails++; }
    }
    return fails ? 1 : 0;
}
static int cli_show_installed(cli_t *c, const char *name) {
    char rp[300]; snprintf(rp, sizeof rp, "%s\\%s.list", PKG_DB_DIR, name);
    vnode_t *n = vfs_resolve(rp, NULL);
    if (!n) { P("%s is not installed.", name); return 1; }
    char *t = kmalloc(n->size + 1); memcpy(t, n->data, n->size); t[n->size] = 0;
    cut_lists(t);
    for (char *q = t; q && *q; ) { char *e = strchr(q, '\n'); if (e) *e = 0; if (strncmp(q, "Desktop:", 8) && strncmp(q, "Dir:", 4)) P("%s", q); q = e ? e + 1 : NULL; }
    P("Status: installed");
    kfree(t);
    return 0;
}
static int cli_info_file(cli_t *c, const char *cwd, const char *arg, bool contents) {
    char path[300]; resolve_arg(cwd, arg, path, sizeof path);
    pkg_t *p = kzalloc(sizeof *p);
    int r = 0;
    if (!pkg_load(path, p)) { P("error: %s", p->err); r = 1; }
    else if (contents) {
        for (int i = 0; i < p->nents; i++) {
            pkg_entry_t *e = &p->ents[i];
            if (e->type == PE_SYMLINK) P("%s -> %s", e->path, e->link);
            else if (e->type == PE_DIR) P("%s/", e->path);
            else P("%s", e->path);
        }
    } else {
        print_info(c, p);
        pkg_check_t k; pkg_check(p, &k);
        if (k.ok) P(" Compatibility: everything it needs is installed.");
        else { P(" Compatibility:"); print_check(c, &k); }
    }
    pkg_free(p); kfree(p);
    return r;
}
static int usage(cli_t *c, const char *cmd) {
    if (!strcmp(cmd, "rpm")) {
        P("usage: rpm -i FILE.rpm...      install (also -U, -ivh)");
        P("       rpm -e NAME...          remove");
        P("       rpm -qa                 list installed packages");
        P("       rpm -qi NAME / -ql NAME information / files of an installed package");
        P("       rpm -qip FILE / -qlp FILE  information / files of a package file");
    } else if (!strcmp(cmd, "apt") || !strcmp(cmd, "apt-get")) {
        P("usage: %s install ./FILE.deb...   install package files", cmd);
        P("       %s remove NAME...          remove", cmd);
        P("       %s list --installed        list installed packages", cmd);
        P("       %s show NAME               details of an installed package", cmd);
    } else {
        P("usage: dpkg -i FILE.deb...      install (also .rpm files)");
        P("       dpkg -r NAME...          remove (-P purges: the same here)");
        P("       dpkg -l [PATTERN]        list installed packages");
        P("       dpkg -L NAME             files of an installed package");
        P("       dpkg -s NAME             status of an installed package");
        P("       dpkg -I FILE / -c FILE   information / contents of a package file");
    }
    return 2;
}
int pkg_cli(int argc, char **argv, const char *cwd, pkg_out_fn out, void *ctx) {
    cli_t cc = { out, ctx }, *c = &cc;
    const char *cmd = path_basename(argv[0]);
    if (argc < 2) return usage(c, cmd);
    const char *op = argv[1];
    if (!strcmp(cmd, "apt") || !strcmp(cmd, "apt-get")) {
        if (!strcmp(op, "install") || !strcmp(op, "reinstall")) {
            bool any_name = false;
            for (int i = 2; i < argc; i++) if (argv[i][0] != '-' && !str_ends_with_ci(argv[i], ".deb") && !str_ends_with_ci(argv[i], ".rpm")) any_name = true;
            if (any_name) {
                P("%s on " OS_NAME " installs package files: download the .deb (from the distribution's", cmd);
                P("website or packages.ubuntu.com / packages.debian.org) and run: %s install ./FILE.deb", cmd);
                P("Downloading from online package repositories is not supported.");
                return 100;
            }
            return cli_install(c, cwd, argc, argv, 2);
        }
        if (!strcmp(op, "remove") || !strcmp(op, "purge")) return cli_remove(c, argc, argv, 2);
        if (!strcmp(op, "list")) return cli_list(c, NULL);
        if (!strcmp(op, "show") && argc > 2) return cli_show_installed(c, argv[2]);
        if (!strcmp(op, "update") || !strcmp(op, "upgrade")) { P("There are no online package sources to update from: packages are installed from files."); return 0; }
        return usage(c, cmd);
    }
    if (!strcmp(cmd, "rpm")) {
        if (op[0] == '-' && (op[1] == 'i' || op[1] == 'U' || op[1] == 'F')) return cli_install(c, cwd, argc, argv, 2);
        if (!strcmp(op, "-e") || !strcmp(op, "--erase")) return cli_remove(c, argc, argv, 2);
        if (!strncmp(op, "-q", 2)) {
            bool all = strchr(op, 'a') != NULL, info = strchr(op, 'i') != NULL, list = strchr(op, 'l') != NULL, file = strchr(op, 'p') != NULL;
            if (all) return cli_list(c, argc > 2 ? argv[2] : NULL);
            if (argc < 3) return usage(c, cmd);
            if (file) return cli_info_file(c, cwd, argv[2], list && !info);
            if (list) return cli_files(c, argv[2]);
            if (info) return cli_show_installed(c, argv[2]);
            char v[96]; if (pkg_is_installed(argv[2], v, sizeof v)) { P("%s-%s", argv[2], v); return 0; }
            P("package %s is not installed", argv[2]); return 1;
        }
        return usage(c, cmd);
    }
    /* dpkg */
    if (!strcmp(op, "-i") || !strcmp(op, "--install")) return cli_install(c, cwd, argc, argv, 2);
    if (!strcmp(op, "-r") || !strcmp(op, "--remove") || !strcmp(op, "-P") || !strcmp(op, "--purge")) return cli_remove(c, argc, argv, 2);
    if (!strcmp(op, "-l") || !strcmp(op, "--list")) return cli_list(c, argc > 2 ? argv[2] : NULL);
    if ((!strcmp(op, "-L") || !strcmp(op, "--listfiles")) && argc > 2) return cli_files(c, argv[2]);
    if ((!strcmp(op, "-s") || !strcmp(op, "--status")) && argc > 2) return cli_show_installed(c, argv[2]);
    if ((!strcmp(op, "-I") || !strcmp(op, "--info")) && argc > 2) return cli_info_file(c, cwd, argv[2], false);
    if ((!strcmp(op, "-c") || !strcmp(op, "--contents")) && argc > 2) return cli_info_file(c, cwd, argv[2], true);
    return usage(c, cmd);
}
