/* Linux software packages: .deb (Debian, Ubuntu, Mint...) and .rpm (Fedora, openSUSE,
 * RHEL...). A package is read whole: its metadata (control file or RPM header) and its
 * files (a tar or cpio archive compressed with gzip, xz or Zstandard, unpacked by
 * src/pkg/unpack.c). Installing writes the files under C:\Linux (/ for Linux programs,
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
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
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

static void canon(char *p, int cap);
/* a path from an archive as an absolute, normalised Linux path; false for ".." */
static bool norm_path(const char *in, char *out, int cap) {
    char tmp[512]; int o = 0;
    tmp[o++] = '/';
    for (const char *p = in; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/'); int l = e ? (int)(e - p) : (int)strlen(p);
        if (l == 1 && p[0] == '.') { p += l; continue; }
        if (l == 2 && p[0] == '.' && p[1] == '.') return false;
        if (o > 1) tmp[o++] = '/';
        if (o + l >= (int)sizeof tmp - 1) return false;
        memcpy(tmp + o, p, l); o += l; p += l;
    }
    tmp[o] = 0;
    canon(tmp, sizeof tmp);
    if ((int)strlen(tmp) >= cap) return false;
    strlcpy(out, tmp, cap);
    return true;
}
/* /bin, /sbin, /lib and /lib64 are /usr/bin ... (merged /usr, as the Linux layer has it) */
static void canon(char *p, int cap) {
    static const char *merged[] = { "/lib", "/lib64", "/bin", "/sbin" };
    for (unsigned i = 0; i < ARRAY_LEN(merged); i++) {
        int l = strlen(merged[i]);
        if (!strncmp(p, merged[i], l) && (p[l] == '/' || !p[l])) {
            if ((int)strlen(p) + 4 >= cap) return;
            memmove(p + 4, p, strlen(p) + 1); memcpy(p, "/usr", 4);
            return;
        }
    }
}
/* resolve "target" seen from the directory of "linkpath" ('..' allowed, stops at /) */
static void join_link(const char *linkpath, const char *target, char *out, int cap) {
    char buf[600];
    if (target[0] == '/') strlcpy(buf, target, sizeof buf);
    else { char dir[512]; strlcpy(dir, linkpath, sizeof dir); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; snprintf(buf, sizeof buf, "%s/%s", dir, target); }
    char parts[64][96]; int np = 0;
    for (char *p = buf; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = strchr(p, '/'); int l = e ? (int)(e - p) : (int)strlen(p);
        if (l == 2 && p[0] == '.' && p[1] == '.') { if (np) np--; }
        else if (!(l == 1 && p[0] == '.') && np < 64) { int k = MIN(l, 95); memcpy(parts[np], p, k); parts[np][k] = 0; np++; }
        p += l;
    }
    out[0] = 0;
    for (int i = 0; i < np; i++) { strlcat(out, "/", cap); strlcat(out, parts[i], cap); }
    if (!out[0]) strlcpy(out, "/", cap);
}
/* Linux path -> WRLD path: / is C:\Linux and /bin, /sbin, /lib, /lib64 live under /usr */
static void lx_to_wrld(const char *lp, char *out, int cap) {
    static const char *merged[] = { "/lib", "/lib64", "/bin", "/sbin" };
    char tmp[600]; strlcpy(tmp, lp, sizeof tmp);
    for (unsigned i = 0; i < ARRAY_LEN(merged); i++) {
        int l = strlen(merged[i]);
        if (!strncmp(lp, merged[i], l) && (lp[l] == '/' || !lp[l])) { snprintf(tmp, sizeof tmp, "/usr%s", lp); break; }
    }
    snprintf(out, cap, "C:\\Linux%s", strcmp(tmp, "/") ? tmp : "");
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
}
static vnode_t *lx_resolve(const char *lp) { char w[600]; lx_to_wrld(lp, w, sizeof w); return vfs_resolve(w, NULL); }

/* ------------------------------------------------------------------ archives */
static pkg_entry_t *add_entry(pkg_t *p) {
    if (p->nents == p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        pkg_entry_t *n = krealloc(p->ents, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        p->ents = n; p->cap = nc;
    }
    pkg_entry_t *e = &p->ents[p->nents++];
    memset(e, 0, sizeof *e);
    return e;
}
static uint64_t tar_num(const char *f, int n) {
    if ((uint8_t)f[0] & 0x80) {                              /* GNU base-256 */
        uint64_t v = 0; for (int i = 1; i < n; i++) v = v << 8 | (uint8_t)f[i]; return v;
    }
    uint64_t v = 0;
    for (int i = 0; i < n && f[i]; i++) { if (f[i] >= '0' && f[i] <= '7') v = v * 8 + (f[i] - '0'); else if (f[i] != ' ') break; }
    return v;
}
/* walk a tar archive (ustar, GNU long names, pax). Calls fn for each member. */
typedef void (*tar_fn)(void *ctx, const char *name, char type, const uint8_t *data, uint32_t size, const char *link);
static bool tar_walk(const uint8_t *d, uint32_t n, tar_fn fn, void *ctx) {
    char longname[512] = "", longlink[512] = "";
    uint32_t off = 0;
    while (off + 512 <= n) {
        const char *h = (const char *)d + off;
        if (!h[0]) break;
        uint64_t size = tar_num(h + 124, 12);
        char type = h[156];
        if (off + 512 + size > n) return false;
        const uint8_t *data = d + off + 512;
        if (type == 'L' || type == 'K') {                       /* GNU long name / link for the next member */
            char *dst = type == 'L' ? longname : longlink;
            uint32_t l = MIN((uint32_t)size, 511u); memcpy(dst, data, l); dst[l] = 0;
        } else if (type == 'x') {                               /* pax: "LEN key=value\n" records */
            const char *p = (const char *)data, *end = p + size;
            while (p < end) {
                char *sp; unsigned long len = strtoul(p, &sp, 10);
                if (!len || p + len > end || *sp != ' ') break;
                const char *kv = sp + 1, *rec_end = p + len - 1;
                if (!strncmp(kv, "path=", 5)) { int l = MIN((int)(rec_end - kv - 5), 511); memcpy(longname, kv + 5, l); longname[l] = 0; }
                else if (!strncmp(kv, "linkpath=", 9)) { int l = MIN((int)(rec_end - kv - 9), 511); memcpy(longlink, kv + 9, l); longlink[l] = 0; }
                p += len;
            }
        } else if (type != 'g') {
            char name[512], link[512];
            if (longname[0]) strlcpy(name, longname, sizeof name);
            /* POSIX ustar has a name prefix at 345; old GNU tar ("ustar  ", what dpkg writes) keeps times there */
            else if (!memcmp(h + 257, "ustar\0", 6) && h[345]) snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h);
            else snprintf(name, sizeof name, "%.100s", h);
            if (longlink[0]) strlcpy(link, longlink, sizeof link); else snprintf(link, sizeof link, "%.100s", h + 157);
            fn(ctx, name, type ? type : '0', data, (uint32_t)size, link);
            longname[0] = longlink[0] = 0;
        }
        off += 512 + (uint32_t)((size + 511) & ~511ull);
    }
    return true;
}
static void collect_tar(void *ctx, const char *name, char type, const uint8_t *data, uint32_t size, const char *link) {
    pkg_t *p = ctx;
    char path[256];
    if (!norm_path(name, path, sizeof path) || !strcmp(path, "/")) return;
    pkg_entry_t *e = add_entry(p); if (!e) return;
    strlcpy(e->path, path, sizeof e->path);
    switch (type) {
    case '5': e->type = PE_DIR; break;
    case '2': e->type = PE_SYMLINK; strlcpy(e->link, link, sizeof e->link); break;
    case '1': e->type = PE_HARDLINK; norm_path(link, e->link, sizeof e->link); break;
    case '0': case '7': e->type = PE_FILE; e->data = data; e->size = size; p->nfiles++; break;
    default: p->nents--; break;                                  /* devices, fifos */
    }
}
/* cpio "newc" (070701/070702), the payload of an .rpm */
static uint32_t hex8(const uint8_t *s) { uint32_t v = 0; for (int i = 0; i < 8; i++) { int c = s[i]; v = v << 4 | (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); } return v; }
static bool cpio_walk(pkg_t *p, const uint8_t *d, uint32_t n) {
    uint32_t off = 0;
    while (off + 110 <= n) {
        if (memcmp(d + off, "07070", 5)) return false;
        uint32_t mode = hex8(d + off + 14), size = hex8(d + off + 54), namesize = hex8(d + off + 94), nlink = hex8(d + off + 38);
        uint32_t ino = hex8(d + off + 6);
        const char *name = (const char *)d + off + 110;
        if (off + 110 + namesize > n) return false;
        uint32_t data_off = (off + 110 + namesize + 3) & ~3u;
        if (data_off + size > n) return false;
        if (!strcmp(name, "TRAILER!!!")) break;
        char path[256];
        if (norm_path(name, path, sizeof path) && strcmp(path, "/")) {
            pkg_entry_t *e = add_entry(p);
            if (e) {
                strlcpy(e->path, path, sizeof e->path);
                uint32_t fmt = mode & 0170000;
                if (fmt == 0040000) e->type = PE_DIR;
                else if (fmt == 0120000) { e->type = PE_SYMLINK; int l = MIN(size, 255u); memcpy(e->link, d + data_off, l); e->link[l] = 0; }
                else if (fmt == 0100000) {
                    e->type = PE_FILE; e->data = d + data_off; e->size = size; p->nfiles++;
                    /* hard links: the data comes with the last name of the group; earlier ones are empty */
                    if (nlink > 1 && !size) { e->type = PE_HARDLINK; snprintf(e->link, sizeof e->link, "#ino:%u", ino); p->nfiles--; }
                    else if (nlink > 1) snprintf(e->link, sizeof e->link, "#ino:%u", ino);
                } else p->nents--;
            }
        }
        off = (data_off + size + 3) & ~3u;
    }
    return true;
}

/* ------------------------------------------------------------------ .deb */
typedef struct { const uint8_t *data; uint32_t size; char name[17]; } armember_t;
static int ar_members(const uint8_t *d, uint32_t n, armember_t *m, int max) {
    if (n < 8 || memcmp(d, "!<arch>\n", 8)) return -1;
    uint32_t off = 8; int k = 0;
    while (off + 60 <= n && k < max) {
        const char *h = (const char *)d + off;
        char sz[11]; memcpy(sz, h + 48, 10); sz[10] = 0;
        uint32_t size = strtoul(sz, NULL, 10);
        if (off + 60 + size > n) return -1;
        memcpy(m[k].name, h, 16); m[k].name[16] = 0;
        for (int i = 15; i >= 0 && (m[k].name[i] == ' ' || m[k].name[i] == '/'); i--) m[k].name[i] = 0;
        m[k].data = d + off + 60; m[k].size = size; k++;
        off += 60 + size + (size & 1);
    }
    return k;
}
typedef struct { char *control, *postinst; char scripts[96]; } debctl_t;
static void collect_control(void *ctx, const char *name, char type, const uint8_t *data, uint32_t size, const char *link) {
    debctl_t *c = ctx;
    const char *b = strrchr(name, '/'); b = b ? b + 1 : name;
    if (!strcmp(b, "control") && !c->control) { c->control = kmalloc(size + 1); if (c->control) { memcpy(c->control, data, size); c->control[size] = 0; } }
    else if (!strcmp(b, "preinst") || !strcmp(b, "postinst") || !strcmp(b, "prerm") || !strcmp(b, "postrm")) {
        list_add(c->scripts, sizeof c->scripts, b);
        if (!strcmp(b, "postinst") && !c->postinst && size < (1u << 20)) { c->postinst = kmalloc(size + 1); if (c->postinst) { memcpy(c->postinst, data, size); c->postinst[size] = 0; } }
    }
}
static uint32_t unpack_limit(void) {
    uint64_t free_mb = mm_total() > mm_used() ? mm_total() - mm_used() : 0;
    uint64_t lim = free_mb * 1024 * 1024 / 2;                    /* leave room for the installed copies */
    if (lim > 0x60000000ull) lim = 0x60000000ull;
    return (uint32_t)lim;
}
static bool load_deb(pkg_t *p, const uint8_t *d, uint32_t n) {
    armember_t m[8];
    int k = ar_members(d, n, m, 8);
    if (k < 3 || strcmp(m[0].name, "debian-binary")) { snprintf(p->err, sizeof p->err, "This is not a Debian package (the ar archive is not valid)."); return false; }
    if (m[0].size < 3 || m[0].data[0] != '2') { snprintf(p->err, sizeof p->err, "This Debian package uses format %.4s, which is not supported (2.0 is).", (const char *)m[0].data); return false; }
    const armember_t *ctl = NULL, *dat = NULL;
    for (int i = 1; i < k; i++) {
        if (!strncmp(m[i].name, "control.tar", 11)) ctl = &m[i];
        else if (!strncmp(m[i].name, "data.tar", 8)) dat = &m[i];
    }
    if (!ctl || !dat) { snprintf(p->err, sizeof p->err, "The package has no control or data archive."); return false; }
    uint32_t cl; char err[120];
    uint8_t *ct = unpack_all(ctl->data, ctl->size, 16u << 20, &cl, err, sizeof err);
    if (!ct) { snprintf(p->err, sizeof p->err, "Could not read the package information: %s.", err); return false; }
    debctl_t c = { 0 };
    tar_walk(ct, cl, collect_control, &c);
    kfree(ct);
    if (!c.control) { snprintf(p->err, sizeof p->err, "The package has no control file."); return false; }
    strlcpy(p->scripts, c.scripts, sizeof p->scripts);
    p->postinst = c.postinst;
    field_to(c.control, "Package", p->name, sizeof p->name);
    field_to(c.control, "Version", p->version, sizeof p->version);
    field_to(c.control, "Architecture", p->arch, sizeof p->arch);
    field_to(c.control, "Maintainer", p->maint, sizeof p->maint);
    field_to(c.control, "Homepage", p->homepage, sizeof p->homepage);
    field_to(c.control, "Section", p->section, sizeof p->section);
    char isz[24] = ""; field_to(c.control, "Installed-Size", isz, sizeof isz); p->inst_kb = strtoul(isz, NULL, 10);
    char *desc = field(c.control, "Description", ':');
    if (desc) {
        char *nl = strchr(desc, '\n');
        if (nl) { *nl = 0; strlcpy(p->summary, desc, sizeof p->summary); }
        else strlcpy(p->summary, desc, sizeof p->summary);
        /* the long description: one space of indent; " ." is an empty line */
        const char *body = nl ? nl + 1 : "";
        char *o = kmalloc(strlen(body) + 2); int w = 0;
        for (const char *q = body; *q; ) {
            const char *e = strchr(q, '\n'); int l = e ? (int)(e - q) : (int)strlen(q);
            const char *s = q; while (l && (*s == ' ' || *s == '\t')) { s++; l--; }
            if (l == 1 && s[0] == '.') l = 0;
            memcpy(o + w, s, l); w += l; o[w++] = '\n';
            q += (e ? (int)(e - q) + 1 : (int)strlen(q));
        }
        o[w] = 0; p->desc = o;
        kfree(desc);
    }
    char *dep = field(c.control, "Depends", ':'), *pre = field(c.control, "Pre-Depends", ':');
    int dl = (dep ? strlen(dep) : 0) + (pre ? strlen(pre) : 0) + 4;
    p->depends = kzalloc(dl);
    if (pre) strlcat(p->depends, pre, dl);
    if (dep) { if (pre) strlcat(p->depends, ", ", dl); strlcat(p->depends, dep, dl); }
    for (char *q = p->depends; *q; q++) if (*q == '\n') *q = ' ';
    kfree(dep); kfree(pre);
    p->provides = field(c.control, "Provides", ':');
    kfree(c.control);
    if (!p->name[0]) { snprintf(p->err, sizeof p->err, "The package has no name."); return false; }
    strlcpy(p->comp, unpack_kind_name(unpack_kind(dat->data, dat->size)), sizeof p->comp);
    p->payload = unpack_all(dat->data, dat->size, unpack_limit(), &p->payload_len, err, sizeof err);
    if (!p->payload) { snprintf(p->err, sizeof p->err, "Could not unpack the files: %s.", err); return false; }
    if (!tar_walk(p->payload, p->payload_len, collect_tar, p)) { snprintf(p->err, sizeof p->err, "The file archive in the package is damaged."); return false; }
    return true;
}

/* ------------------------------------------------------------------ .rpm */
typedef struct { const uint8_t *idx; uint32_t n; const uint8_t *store; uint32_t slen; } rpmhdr_t;
static bool rpm_header(const uint8_t *d, uint32_t avail, rpmhdr_t *h, uint32_t *total) {
    if (avail < 16 || d[0] != 0x8E || d[1] != 0xAD || d[2] != 0xE8) return false;
    uint32_t n = be32(d + 8), sl = be32(d + 12);
    if (n > 100000 || 16 + (uint64_t)n * 16 + sl > avail) return false;
    h->idx = d + 16; h->n = n; h->store = d + 16 + n * 16; h->slen = sl;
    *total = 16 + n * 16 + sl;
    return true;
}
static bool rpm_find(const rpmhdr_t *h, uint32_t tag, uint32_t *type, uint32_t *off, uint32_t *count) {
    for (uint32_t i = 0; i < h->n; i++) {
        const uint8_t *e = h->idx + i * 16;
        if (be32(e) == tag) { *type = be32(e + 4); *off = be32(e + 8); *count = be32(e + 12); return *off < h->slen; }
    }
    return false;
}
/* the k-th string of a STRING / STRING_ARRAY / I18NSTRING tag */
static const char *rpm_str(const rpmhdr_t *h, uint32_t tag, uint32_t k) {
    uint32_t type, off, count;
    if (!rpm_find(h, tag, &type, &off, &count) || (type != 6 && type != 8 && type != 9) || k >= count) return NULL;
    const char *s = (const char *)h->store + off, *end = (const char *)h->store + h->slen;
    for (uint32_t i = 0; i < k; i++) { const char *z = memchr(s, 0, end - s); if (!z) return NULL; s = z + 1; }
    return memchr(s, 0, end - s) ? s : NULL;
}
static uint32_t rpm_count(const rpmhdr_t *h, uint32_t tag) { uint32_t t, o, c; return rpm_find(h, tag, &t, &o, &c) ? c : 0; }
static uint64_t rpm_int(const rpmhdr_t *h, uint32_t tag, uint32_t k) {
    uint32_t type, off, count;
    if (!rpm_find(h, tag, &type, &off, &count) || k >= count) return 0;
    const uint8_t *p = h->store + off;
    if (type == 4 && off + (k + 1) * 4 <= h->slen) return be32(p + k * 4);
    if (type == 5 && off + (k + 1) * 8 <= h->slen) return (uint64_t)be32(p + k * 8) << 32 | be32(p + k * 8 + 4);
    if (type == 3 && off + (k + 1) * 2 <= h->slen) return (uint32_t)p[k * 2] << 8 | p[k * 2 + 1];
    return 0;
}
enum { RPMTAG_NAME = 1000, RPMTAG_VERSION = 1001, RPMTAG_RELEASE = 1002, RPMTAG_EPOCH = 1003, RPMTAG_SUMMARY = 1004,
       RPMTAG_DESCRIPTION = 1005, RPMTAG_SIZE = 1009, RPMTAG_VENDOR = 1011, RPMTAG_LICENSE = 1014, RPMTAG_PACKAGER = 1015,
       RPMTAG_GROUP = 1016, RPMTAG_URL = 1020, RPMTAG_ARCH = 1022, RPMTAG_PREIN = 1023, RPMTAG_POSTIN = 1024,
       RPMTAG_PREUN = 1025, RPMTAG_POSTUN = 1026, RPMTAG_PROVIDENAME = 1047, RPMTAG_REQUIREFLAGS = 1048,
       RPMTAG_REQUIRENAME = 1049, RPMTAG_PAYLOADFORMAT = 1124, RPMTAG_PAYLOADCOMPRESSOR = 1125, RPMTAG_LONGSIZE = 5009 };
static bool load_rpm(pkg_t *p, const uint8_t *d, uint32_t n) {
    if (n < 96 + 16 || be32(d) != 0xEDABEEDB) { snprintf(p->err, sizeof p->err, "This is not an RPM package."); return false; }
    if (d[6] == 0 && d[7] == 1) { snprintf(p->err, sizeof p->err, "This is a source RPM (.src.rpm): it holds source code to build, not a program to install."); return false; }
    rpmhdr_t sig, h; uint32_t sl, hl;
    if (!rpm_header(d + 96, n - 96, &sig, &sl)) { snprintf(p->err, sizeof p->err, "The RPM signature header is damaged."); return false; }
    uint32_t hoff = 96 + ((sl + 7) & ~7u);
    if (hoff >= n || !rpm_header(d + hoff, n - hoff, &h, &hl)) { snprintf(p->err, sizeof p->err, "The RPM header is damaged."); return false; }
    const char *s;
    if ((s = rpm_str(&h, RPMTAG_NAME, 0))) strlcpy(p->name, s, sizeof p->name);
    const char *ver = rpm_str(&h, RPMTAG_VERSION, 0), *rel = rpm_str(&h, RPMTAG_RELEASE, 0);
    uint64_t epoch = rpm_int(&h, RPMTAG_EPOCH, 0);
    if (epoch) snprintf(p->version, sizeof p->version, "%u:%s-%s", (unsigned)epoch, ver ? ver : "?", rel ? rel : "?");
    else snprintf(p->version, sizeof p->version, "%s-%s", ver ? ver : "?", rel ? rel : "?");
    if ((s = rpm_str(&h, RPMTAG_ARCH, 0))) strlcpy(p->arch, s, sizeof p->arch);
    if ((s = rpm_str(&h, RPMTAG_SUMMARY, 0))) strlcpy(p->summary, s, sizeof p->summary);
    if ((s = rpm_str(&h, RPMTAG_DESCRIPTION, 0))) p->desc = strdup(s);
    if ((s = rpm_str(&h, RPMTAG_PACKAGER, 0)) || (s = rpm_str(&h, RPMTAG_VENDOR, 0))) strlcpy(p->maint, s, sizeof p->maint);
    if ((s = rpm_str(&h, RPMTAG_URL, 0))) strlcpy(p->homepage, s, sizeof p->homepage);
    if ((s = rpm_str(&h, RPMTAG_GROUP, 0))) strlcpy(p->section, s, sizeof p->section);
    if ((s = rpm_str(&h, RPMTAG_LICENSE, 0))) strlcpy(p->license, s, sizeof p->license);
    uint64_t size = rpm_int(&h, RPMTAG_LONGSIZE, 0); if (!size) size = rpm_int(&h, RPMTAG_SIZE, 0);
    p->inst_kb = (uint32_t)((size + 1023) / 1024);
    static const struct { uint32_t tag; const char *name; } scr[] = { { RPMTAG_PREIN, "%pre" }, { RPMTAG_POSTIN, "%post" }, { RPMTAG_PREUN, "%preun" }, { RPMTAG_POSTUN, "%postun" } };
    for (unsigned i = 0; i < ARRAY_LEN(scr); i++) if (rpm_count(&h, scr[i].tag)) list_add(p->scripts, sizeof p->scripts, scr[i].name);
    if ((s = rpm_str(&h, RPMTAG_POSTIN, 0))) p->postinst = strdup(s);
    /* requirements, without rpm's own (rpmlib(...)) and those the loader meets (rtld(...)) */
    uint32_t nreq = rpm_count(&h, RPMTAG_REQUIRENAME);
    int cap = 64; for (uint32_t i = 0; i < nreq; i++) { const char *r = rpm_str(&h, RPMTAG_REQUIRENAME, i); if (r) cap += strlen(r) + 2; }
    p->depends = kzalloc(cap);
    for (uint32_t i = 0; i < nreq; i++) {
        const char *r = rpm_str(&h, RPMTAG_REQUIRENAME, i);
        if (!r || !strncmp(r, "rpmlib(", 7) || !strncmp(r, "rtld(", 5) || !strncmp(r, "config(", 7)) continue;
        if (rpm_int(&h, RPMTAG_REQUIREFLAGS, i) & (1u << 24)) continue;      /* RPMSENSE_RPMLIB */
        list_add(p->depends, cap, r);
    }
    uint32_t nprov = rpm_count(&h, RPMTAG_PROVIDENAME);
    cap = 16; for (uint32_t i = 0; i < nprov; i++) { const char *r = rpm_str(&h, RPMTAG_PROVIDENAME, i); if (r) cap += strlen(r) + 2; }
    p->provides = kzalloc(cap);
    for (uint32_t i = 0; i < nprov; i++) { const char *r = rpm_str(&h, RPMTAG_PROVIDENAME, i); if (r) list_add(p->provides, cap, r); }
    if (!p->name[0]) { snprintf(p->err, sizeof p->err, "The package has no name."); return false; }
    const char *pf = rpm_str(&h, RPMTAG_PAYLOADFORMAT, 0);
    if (pf && strcmp(pf, "cpio")) { snprintf(p->err, sizeof p->err, "The files are in a %s archive, which is not supported (cpio is).", pf); return false; }
    uint32_t poff = hoff + hl;
    const uint8_t *pay = d + poff; uint32_t plen = n - poff;
    strlcpy(p->comp, unpack_kind_name(unpack_kind(pay, plen)), sizeof p->comp);
    char err[120];
    p->payload = unpack_all(pay, plen, unpack_limit(), &p->payload_len, err, sizeof err);
    if (!p->payload) { snprintf(p->err, sizeof p->err, "Could not unpack the files: %s.", err); return false; }
    if (p->payload_len >= 6 && !memcmp(p->payload, "07070", 5)) {
        if (!cpio_walk(p, p->payload, p->payload_len)) { snprintf(p->err, sizeof p->err, "The file archive in the package is damaged."); return false; }
    } else { snprintf(p->err, sizeof p->err, "The files are not in a cpio archive this installer can read."); return false; }
    /* hard links in cpio: point each empty name at the member that carries the data */
    for (int i = 0; i < p->nents; i++) {
        pkg_entry_t *e = &p->ents[i];
        if (e->type != PE_HARDLINK || strncmp(e->link, "#ino:", 5)) continue;
        for (int j = 0; j < p->nents; j++)
            if (p->ents[j].type == PE_FILE && !strcmp(p->ents[j].link, e->link)) { strlcpy(e->link, p->ents[j].path, sizeof e->link); break; }
    }
    for (int i = 0; i < p->nents; i++) if (p->ents[i].type == PE_FILE) p->ents[i].link[0] = 0;
    return true;
}

static uint32_t hstr(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static const char *base_of(const char *path) { const char *b = strrchr(path, '/'); return b ? b + 1 : path; }
/* index the entries by path and by base name (-1 = empty slot) */
static void build_index(pkg_t *p) {
    int cap = 64; while (cap < p->nents * 2 + 2) cap *= 2;
    p->pidx = kmalloc(sizeof(int) * cap); p->bidx = kmalloc(sizeof(int) * cap);
    if (!p->pidx || !p->bidx) { kfree(p->pidx); kfree(p->bidx); p->pidx = p->bidx = NULL; return; }
    for (int i = 0; i < cap; i++) p->pidx[i] = p->bidx[i] = -1;
    p->icap = cap;
    for (int i = 0; i < p->nents; i++) {
        uint32_t h = hstr(p->ents[i].path) & (cap - 1);
        while (p->pidx[h] >= 0) h = (h + 1) & (cap - 1);
        p->pidx[h] = i;
        if (p->ents[i].type == PE_DIR) continue;
        h = hstr(base_of(p->ents[i].path)) & (cap - 1);
        while (p->bidx[h] >= 0) h = (h + 1) & (cap - 1);
        p->bidx[h] = i;
    }
}
bool pkg_load(const char *wrld_path, pkg_t *p) {
    memset(p, 0, sizeof *p);
    strlcpy(p->file, wrld_path, sizeof p->file);
    vnode_t *n = vfs_resolve(wrld_path, NULL);
    if (!n || n->dir) { snprintf(p->err, sizeof p->err, "The file %s was not found.", wrld_path); return false; }
    bool ok;
    if (n->size >= 8 && !memcmp(n->data, "!<arch>\n", 8)) { p->fmt = 'd'; ok = load_deb(p, n->data, n->size); }
    else if (n->size >= 4 && be32(n->data) == 0xEDABEEDB) { p->fmt = 'r'; ok = load_rpm(p, n->data, n->size); }
    else { snprintf(p->err, sizeof p->err, "%s is not a .deb or .rpm package.", path_basename(wrld_path)); return false; }
    if (ok) build_index(p);
    return ok;
}
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
/* a dependency is met by an installed package, something one provides, or the OS image */
static bool dep_met(const char *name) {
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
/* ------------------------------------------------------------------ checking a package */
/* the basenames the package itself installs, to find its own libraries */
static bool pkg_has_basename(const pkg_t *p, const char *base) {
    if (!p->icap) return false;
    for (uint32_t h = hstr(base) & (p->icap - 1); p->bidx[h] >= 0; h = (h + 1) & (p->icap - 1))
        if (!strcmp(base_of(p->ents[p->bidx[h]].path), base)) return true;
    return false;
}
static const pkg_entry_t *pkg_find_entry(const pkg_t *p, const char *path) {
    if (!p->icap) return NULL;
    char c[300]; strlcpy(c, path, sizeof c); canon(c, sizeof c);
    for (uint32_t h = hstr(c) & (p->icap - 1); p->pidx[h] >= 0; h = (h + 1) & (p->icap - 1))
        if (!strcmp(p->ents[p->pidx[h]].path, c)) return &p->ents[p->pidx[h]];
    return NULL;
}
static const char *lib_dirs[] = { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", "/usr/local/lib", "/usr/lib/x86_64-linux-gnu/pulseaudio" };
/* the data of a library: in the package, or installed */
static const uint8_t *find_lib(const pkg_t *p, const char *name, const char *runpath, const char *origin, uint32_t *size) {
    char lp[512];
    /* RUNPATH / RPATH, with $ORIGIN */
    if (runpath && runpath[0]) {
        char rp[512]; strlcpy(rp, runpath, sizeof rp);
        for (char *d = rp; d && *d; ) {
            char *e = strchr(d, ':'); if (e) *e = 0;
            char dir[512];
            if (!strncmp(d, "$ORIGIN", 7)) snprintf(dir, sizeof dir, "%s%s", origin, d + 7);
            else if (!strncmp(d, "${ORIGIN}", 9)) snprintf(dir, sizeof dir, "%s%s", origin, d + 9);
            else strlcpy(dir, d, sizeof dir);
            char full[600]; snprintf(full, sizeof full, "%s/%s", dir, name);
            join_link("/x", full, lp, sizeof lp);
            const pkg_entry_t *e2 = pkg_find_entry(p, lp);
            if (e2 && e2->type == PE_FILE) { *size = e2->size; return e2->data; }
            vnode_t *n = lx_resolve(lp); if (n && !n->dir) { *size = n->size; return n->data; }
            d = e ? e + 1 : NULL;
        }
    }
    for (unsigned i = 0; i < ARRAY_LEN(lib_dirs); i++) {
        snprintf(lp, sizeof lp, "%s/%s", lib_dirs[i], name);
        const pkg_entry_t *e = pkg_find_entry(p, lp);
        if (!e) { char alt[520]; snprintf(alt, sizeof alt, "/lib%s", lp + 8); e = !strncmp(lp, "/usr/lib", 8) ? pkg_find_entry(p, alt) : NULL; }
        if (e && e->type == PE_FILE) { *size = e->size; return e->data; }
        if (e) { *size = 0; return (const uint8_t *)""; }            /* a link in the package: present */
        vnode_t *n = lx_resolve(lp); if (n && !n->dir) { *size = n->size; return n->data; }
    }
    if (pkg_has_basename(p, name)) { *size = 0; return (const uint8_t *)""; }
    return NULL;
}
static bool mem_has(const uint8_t *d, uint32_t n, const char *s) {
    int l = strlen(s) + 1;                                       /* with its NUL: "GLIBC_2.3" is not "GLIBC_2.34" */
    for (uint32_t i = 0; i + l <= n; i++) if (d[i] == s[0] && !memcmp(d + i, s, l)) return true;
    return false;
}
/* look at one ELF file: its loader, the libraries it needs and their versions */
static void check_elf(const pkg_t *p, const char *path, const uint8_t *d, uint32_t n, pkg_check_t *c, pathset_t *seen) {
    if (n < 64 || memcmp(d, "\x7F" "ELF", 4)) return;
    if (d[4] != 2 || d[5] != 1 || (d[18] | d[19] << 8) != 62) { c->foreign_elf = true; return; }   /* not x86-64 */
    uint64_t phoff; memcpy(&phoff, d + 32, 8);
    uint16_t phentsize = d[54] | d[55] << 8, phnum = d[56] | d[57] << 8;
    if (phoff + (uint64_t)phnum * phentsize > n || phentsize < 56) return;
    uint64_t dyn_off = 0, dyn_sz = 0;
    struct { uint64_t va, off, sz; } loads[16]; int nl = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + i * phentsize;
        uint32_t type; memcpy(&type, ph, 4);
        uint64_t off, va, fsz; memcpy(&off, ph + 8, 8); memcpy(&va, ph + 16, 8); memcpy(&fsz, ph + 32, 8);
        if (type == 1 && nl < 16) { loads[nl].va = va; loads[nl].off = off; loads[nl].sz = fsz; nl++; }
        else if (type == 2) { dyn_off = off; dyn_sz = fsz; }
        else if (type == 3 && off + fsz <= n && fsz > 1 && fsz < 256) {   /* PT_INTERP */
            char ip[256]; memcpy(ip, d + off, fsz); ip[fsz - 1] = 0;
            if (!pkg_find_entry(p, ip) && !lx_resolve(ip)) list_add(c->missing_interp, sizeof c->missing_interp, ip);
        }
    }
    if (!dyn_off || dyn_off + dyn_sz > n) return;
    #define VA2OFF(a) ({ uint64_t r_ = 0; for (int k_ = 0; k_ < nl; k_++) if ((a) >= loads[k_].va && (a) < loads[k_].va + loads[k_].sz) { r_ = (a) - loads[k_].va + loads[k_].off; break; } r_; })
    uint64_t strtab = 0, strsz = 0, runpath = 0, verneed = 0, verneednum = 0; bool has_rp = false;
    uint64_t needed[64]; int nn = 0;
    for (uint64_t o = dyn_off; o + 16 <= dyn_off + dyn_sz; o += 16) {
        int64_t tag; uint64_t val; memcpy(&tag, d + o, 8); memcpy(&val, d + o + 8, 8);
        if (tag == 0) break;
        if (tag == 1 && nn < 64) needed[nn++] = val;
        else if (tag == 5) strtab = val;
        else if (tag == 10) strsz = val;
        else if (tag == 29 || tag == 15) { runpath = val; has_rp = true; }
        else if (tag == 0x6ffffffe) verneed = val;
        else if (tag == 0x6fffffff) verneednum = val;
    }
    uint64_t so = VA2OFF(strtab);
    if (!so || so + strsz > n) return;
    const char *str = (const char *)d + so;
    #define STR(i) ((i) < strsz && memchr(str + (i), 0, strsz - (i)) ? str + (i) : "")
    char origin[256]; strlcpy(origin, path, sizeof origin); char *sl = strrchr(origin, '/'); if (sl) *sl = 0;
    const char *rp = has_rp ? STR(runpath) : NULL;
    for (int i = 0; i < nn; i++) {
        const char *lib = STR(needed[i]);
        uint32_t sz;
        if (lib[0] && !find_lib(p, lib, rp, origin, &sz)) list_add(c->missing_libs, sizeof c->missing_libs, lib);
    }
    /* symbol versions: each library must define the versions the program asks for */
    uint64_t vo = verneed ? VA2OFF(verneed) : 0;
    for (uint64_t k = 0; vo && k < verneednum && vo + 16 <= n; k++) {
        uint16_t cnt = d[vo + 2] | d[vo + 3] << 8; uint32_t file, aux, next;
        memcpy(&file, d + vo + 4, 4); memcpy(&aux, d + vo + 8, 4); memcpy(&next, d + vo + 12, 4);
        const char *lib = STR(file); uint32_t lsz = 0;
        const uint8_t *ld = lib[0] ? find_lib(p, lib, rp, origin, &lsz) : NULL;
        uint64_t ao = vo + aux;
        for (int a = 0; a < cnt && ld && lsz && ao + 16 <= n; a++) {
            uint32_t name, anext; memcpy(&name, d + ao + 8, 4); memcpy(&anext, d + ao + 12, 4);
            const char *v = STR(name);
            char key[160]; snprintf(key, sizeof key, "%s|%s", lib, v);
            if (v[0] && !set_has(seen, key)) {
                set_add(seen, key);
                if (!mem_has(ld, lsz, v)) { char b[96]; snprintf(b, sizeof b, "%s (%s)", v, lib); list_add(c->missing_vers, sizeof c->missing_vers, b); }
            }
            if (!anext) break;
            ao += anext;
        }
        if (!next) break;
        vo += next;
    }
    #undef STR
    #undef VA2OFF
}
static bool is_bin_dir(const char *path) {
    static const char *dirs[] = { "/usr/bin/", "/usr/games/", "/usr/sbin/", "/usr/local/bin/", "/bin/", "/sbin/" };
    for (unsigned i = 0; i < ARRAY_LEN(dirs); i++) { int l = strlen(dirs[i]); if (!strncmp(path, dirs[i], l) && !strchr(path + l, '/')) return true; }
    return false;
}
void pkg_check(const pkg_t *p, pkg_check_t *c) {
    memset(c, 0, sizeof *c);
    /* package dependencies (Debian names; an .rpm's are mostly libraries and paths) */
    if (p->depends && p->depends[0]) {
        char *deps = strdup(p->depends);
        for (char *g = deps; g && *g; ) {
            char *e = strchr(g, ','); if (e) *e = 0;
            bool met = false; char first[96] = "";
            char alt[256]; strlcpy(alt, g, sizeof alt);
            for (char *a = alt; a && *a; ) {
                char *bar = strchr(a, '|'); if (bar) *bar = 0;
                char nm[96]; strlcpy(nm, a, sizeof nm); trim(nm);
                char *paren = strchr(nm, '('); if (paren) { *paren = 0; trim(nm); }
                char *sp = strchr(nm, ' '); if (sp) *sp = 0;
                if (!first[0]) strlcpy(first, nm, sizeof first);
                if (p->fmt == 'r') {
                    if (nm[0] == '/') met = pkg_find_entry(p, nm) || lx_resolve(nm);
                    else if (strstr(nm, ".so")) { char so[96]; strlcpy(so, nm, sizeof so); char *q = strchr(so, '('); if (q) *q = 0; uint32_t sz; met = find_lib(p, so, NULL, "/", &sz) != NULL; }
                    else met = true;                                     /* package names differ between distributions */
                } else met = nm[0] && dep_met(nm);
                if (met) break;
                a = bar ? bar + 1 : NULL;
            }
            if (!met && first[0]) list_add(c->missing_pkgs, sizeof c->missing_pkgs, first);
            g = e ? e + 1 : NULL;
        }
        kfree(deps);
    }
    /* the programs: libraries, versions, interpreters */
    int checked = 0;
    pathset_t seen = { 0 };
    for (int i = 0; i < p->nents && checked < 400; i++) {
        const pkg_entry_t *e = &p->ents[i];
        if (e->type != PE_FILE && e->type != PE_SYMLINK) continue;
        bool bin = is_bin_dir(e->path);
        if (bin) { const char *b = strrchr(e->path, '/') + 1; list_add(c->programs, sizeof c->programs, b); }
        if (e->type != PE_FILE || e->size < 4) continue;
        if (!memcmp(e->data, "\x7F" "ELF", 4)) { check_elf(p, e->path, e->data, e->size, c, &seen); checked++; if (!(checked & 15)) unpack_yield(); }
        else if (bin && e->data[0] == '#' && e->data[1] == '!') {          /* a script: is its interpreter here? */
            char line[128]; int l = 0;
            for (uint32_t k = 2; k < e->size && l < 127 && e->data[k] != '\n'; k++) line[l++] = e->data[k];
            line[l] = 0; trim(line);
            char *sp = strchr(line, ' '); char *arg = sp ? sp + 1 : NULL; if (sp) *sp = 0;
            char interp[160];
            if (!strcmp(line, "/usr/bin/env") && arg) { char *s2 = strchr(arg, ' '); if (s2) *s2 = 0; snprintf(interp, sizeof interp, "/usr/bin/%s", arg); }
            else strlcpy(interp, line, sizeof interp);
            if (interp[0] == '/' && !pkg_find_entry(p, interp) && !lx_resolve(interp)) list_add(c->missing_interp, sizeof c->missing_interp, interp);
        }
    }
    set_free(&seen);
    c->ok = !c->missing_pkgs[0] && !c->missing_libs[0] && !c->missing_vers[0] && !c->missing_interp[0] && !c->foreign_elf;
}

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
        char *t = kmalloc(e->size + 1); if (!t) continue;
        memcpy(t, e->data, e->size); t[e->size] = 0;
        /* only the [Desktop Entry] group */
        char *grp = strstr(t, "[Desktop Entry]"); char *body = grp ? grp : t;
        char *next = strstr(body + 1, "\n["); if (next) next[1] = 0;
        char *name = field(body, "Name", '='), *exec = field(body, "Exec", '='), *term = field(body, "Terminal", '=');
        char *nod = field(body, "NoDisplay", '='), *type = field(body, "Type", '=');
        if (name && exec && (!nod || strcasecmp(nod, "true")) && (!type || !strcmp(type, "Application"))) {
            /* drop the field codes (%f %u %F %U %i %c %k) */
            char cmd[256]; int o = 0;
            for (const char *q = exec; *q && o < 255; q++) {
                if (q[0] == '%' && q[1]) { q++; if (*q == '%') cmd[o++] = '%'; continue; }
                cmd[o++] = *q;
            }
            cmd[o] = 0; trim(cmd);
            sb_addf(&s->hdr, "Desktop: %s\t%s\t%d\n", name, cmd, term && !strcasecmp(term, "true"));
        }
        kfree(name); kfree(exec); kfree(term); kfree(nod); kfree(type); kfree(t);
    }
}
/* What setup scripts most often do for a command-line program is choose its command
 * name: "update-alternatives --install /usr/bin/figlet figlet /usr/bin/figlet-figlet 50"
 * (with --slave pairs for its manual pages). Those lines are carried out here, as copies,
 * when the name is still free; the rest of the scripts is not run. */
static int alternatives(inst_t *s) {
    const char *sc = s->p->postinst;
    if (!sc) return 0;
    int made = 0;
    for (const char *q = sc; (q = strstr(q, "update-alternatives")); ) {
        /* the whole command: continuation lines joined */
        char cmd[1024]; int o = 0;
        const char *r = q + 19;                                     /* past "update-alternatives" */
        for (; *r && o < (int)sizeof cmd - 1; r++) {
            if (r[0] == '\\' && r[1] == '\n') { r++; cmd[o++] = ' '; continue; }
            if (*r == '\n' || *r == ';' || *r == '|' || *r == '&') break;
            cmd[o++] = *r;
        }
        cmd[o] = 0; q = r;
        char *w[48]; int n = split_words(cmd, w, 48);
        for (int i = 0; i < n; i++) {
            int args = !strcmp(w[i], "--install") ? 4 : !strcmp(w[i], "--slave") ? 3 : 0;
            if (!args || i + 3 >= n) continue;
            const char *link = w[i + 1], *path = w[i + 3];
            i += args;
            if (link[0] != '/' || path[0] != '/' || strchr(link, '$') || strchr(path, '$')) continue;
            char lc[300], pc[300]; strlcpy(lc, link, sizeof lc); canon(lc, sizeof lc); strlcpy(pc, path, sizeof pc); canon(pc, sizeof pc);
            if (lx_resolve(lc)) continue;                                  /* that name is taken */
            vnode_t *t = lx_resolve(pc);
            if (!t || t->dir) continue;
            put_file(s, lc, t->data, t->size);
            if (lx_resolve(lc)) made++;
        }
    }
    return made;
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
