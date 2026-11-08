/* Linux file system calls. Paths: "/" is C:\Linux, /mnt/<letter>/ is that
 * drive, /lib, /lib64, /bin and /sbin are the merged-/usr links of current
 * distributions, and a few /dev and /proc entries are built in. Files live in
 * the WRLD VFS (whole files in memory), so reads and mmap() copy from there. */
#ifdef __x86_64__
#include "lx.h"

#define AT_FDCWD -100
#define O_ACCMODE 3
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC 02000000
#define O_PATH 010000000
#define O_TMPFILE 020200000

/* ---------------- paths ---------------- */
static void normalize(char *p) {
    /* p is absolute: collapse //, ., .. */
    char out[512]; int o = 0;
    const char *s = p;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s; while (*e && *e != '/') e++;
        int l = e - s;
        if (l == 1 && s[0] == '.') { s = e; continue; }
        if (l == 2 && s[0] == '.' && s[1] == '.') { while (o > 0 && out[o - 1] != '/') o--; if (o > 0) o--; s = e; continue; }
        if (o + l + 2 >= (int)sizeof out) break;
        out[o++] = '/'; memcpy(out + o, s, l); o += l;
        s = e;
    }
    if (!o) out[o++] = '/';
    out[o] = 0;
    strcpy(p, out);
}
void lx_abspath(lxproc_t *p, const char *upath, char *out, int cap) {
    char tmp[512];
    if (upath[0] == '/') strlcpy(tmp, upath, sizeof tmp);
    else snprintf(tmp, sizeof tmp, "%s/%s", p->cwd, upath);
    normalize(tmp);
    strlcpy(out, tmp, cap);
}
static int classify(const char *a) {
    if (!strcmp(a, "/dev/null")) return LP_DEV_NULL;
    if (!strcmp(a, "/dev/zero")) return LP_DEV_ZERO;
    if (!strcmp(a, "/dev/urandom") || !strcmp(a, "/dev/random")) return LP_DEV_RANDOM;
    if (!strcmp(a, "/dev/tty") || !strcmp(a, "/dev/console") || !strcmp(a, "/dev/stdout") || !strcmp(a, "/dev/stderr") || !strcmp(a, "/dev/stdin")) return LP_DEV_TTY;
    if (!strcmp(a, "/proc/self/exe")) return LP_PROC_SELF_EXE;
    return LP_FILE;
}
/* absolute Linux path -> WRLD path */
static void to_wrld(const char *a, char *out, int cap) {
    char tmp[512];
    static const char *merged[] = { "/lib", "/lib64", "/bin", "/sbin" };
    strlcpy(tmp, a, sizeof tmp);
    for (unsigned i = 0; i < ARRAY_LEN(merged); i++) {
        int l = strlen(merged[i]);
        if (!strncmp(a, merged[i], l) && (a[l] == '/' || !a[l])) { snprintf(tmp, sizeof tmp, "/usr%s", a); break; }
    }
    if (!strncmp(tmp, "/mnt/", 5) && isalpha(tmp[5]) && (tmp[6] == '/' || !tmp[6])) snprintf(out, cap, "%c:\\%s", toupper(tmp[5]), tmp[6] ? tmp + 7 : "");
    else snprintf(out, cap, "C:\\Linux%s", strcmp(tmp, "/") ? tmp : "");
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
}
int lx_path(lxproc_t *p, int dirfd, const char *upath, char *out, int cap) {
    char a[512];
    if (upath[0] != '/' && dirfd != AT_FDCWD) {
        lxfile_t *d = lx_fd_get(p, dirfd);
        char base[512]; strlcpy(base, d ? d->path : p->cwd, sizeof base);
        snprintf(a, sizeof a, "%s/%s", base, upath);
        normalize(a);
    } else lx_abspath(p, upath, a, sizeof a);
    int k = classify(a);
    to_wrld(a, out, cap);
    return k;
}
static int lx_path2(lxproc_t *p, int dirfd, const char *upath, char *wrld, int cap, char *linux_abs, int lcap) {
    char a[512];
    if (upath[0] != '/' && dirfd != AT_FDCWD) {
        lxfile_t *d = lx_fd_get(p, dirfd);
        snprintf(a, sizeof a, "%s/%s", d ? d->path : p->cwd, upath);
        normalize(a);
    } else lx_abspath(p, upath, a, sizeof a);
    strlcpy(linux_abs, a, lcap);
    to_wrld(a, wrld, cap);
    return classify(a);
}

/* ---------------- user memory ---------------- */
static uvm_t *VM(void) { return lx_current()->proc->vm; }
static bool uin(void *dst, uint64_t src, uint64_t n) { if (!uvm_check(VM(), src, n, false)) return false; memcpy(dst, (void *)(uintptr_t)src, n); return true; }
static bool uout(uint64_t dst, const void *src, uint64_t n) { if (!uvm_check(VM(), dst, n, true)) return false; memcpy((void *)(uintptr_t)dst, src, n); return true; }
static int ustr(char *dst, uint64_t src, int cap) {
    for (int i = 0; i < cap; i++) {
        if (((src + i) & (PAGE - 1)) == 0 || i == 0) if (!uvm_check(VM(), src + i, 1, false)) return -EFAULT;
        char c = *(volatile char *)(uintptr_t)(src + i);
        dst[i] = c;
        if (!c) return i;
    }
    return -ENAMETOOLONG;
}

/* ---------------- descriptors ---------------- */
int lx_fd_alloc(lxproc_t *p, lxfile_t *f, int min) {
    for (int i = MAX(min, 0); i < LX_MAXFD; i++) if (!p->fds[i]) { p->fds[i] = f; p->fdflags[i] = 0; return i; }
    return -EMFILE;
}
lxfile_t *lx_fd_get(lxproc_t *p, int fd) { return fd >= 0 && fd < LX_MAXFD ? p->fds[fd] : NULL; }
void lx_file_put(lxfile_t *f) { if (f && --f->refs <= 0) kfree(f); }
static lxfile_t *file_new(int type) { lxfile_t *f = kzalloc(sizeof *f); f->type = type; f->refs = 1; return f; }

/* ---------------- stat ---------------- */
typedef struct PACKED {
    uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad0; uint64_t rdev; int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; int64_t reserved[3];
} lstat_t;
static int64_t node_time(vnode_t *n) {
    rtc_time_t t = n->mtime;
    if (!t.year) return 1700000000;
    int y = t.year, m = t.month, d = t.day;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + (int64_t)doe - 719468) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}
static void fill_stat(lstat_t *st, vnode_t *n, int kind) {
    memset(st, 0, sizeof *st);
    st->dev = 0x801; st->nlink = 1; st->uid = st->gid = 1000; st->blksize = 4096;
    if (kind != LP_FILE && kind != LP_PROC_SELF_EXE) { st->mode = 020666; st->rdev = 0x103; st->ino = 100 + kind; return; }
    st->ino = ((uintptr_t)n >> 4) & 0xFFFFFFFF;
    st->mode = n->dir ? 040755 : 0100755;
    st->size = n->dir ? 4096 : n->size;
    st->blocks = (st->size + 511) / 512;
    st->atime = st->mtime = st->ctime = node_time(n);
}

/* resolve a Linux path to a node (NULL: not found) */
static vnode_t *resolve(lxproc_t *p, int dirfd, const char *upath, int *kind, char *labs, int lcap) {
    char w[512];
    int k = lx_path2(p, dirfd, upath, w, sizeof w, labs, lcap);
    if (kind) *kind = k;
    if (k == LP_PROC_SELF_EXE) { char we[512]; to_wrld(p->exe, we, sizeof we); return vfs_resolve(we, NULL); }
    if (k != LP_FILE) return NULL;
    return vfs_resolve(w, NULL);
}

static int64_t do_open(lxproc_t *p, int dirfd, uint64_t upath, int flags, int mode) {
    char path[512];
    int r = ustr(path, upath, sizeof path); if (r < 0) return r;
    if (!path[0]) return -ENOENT;
    int kind; char labs[512];
    vnode_t *n = resolve(p, dirfd, path, &kind, labs, sizeof labs);
    lxfile_t *f = NULL;
    if (kind == LP_DEV_NULL) f = file_new(FD_NULL);
    else if (kind == LP_DEV_ZERO) f = file_new(FD_ZERO);
    else if (kind == LP_DEV_RANDOM) f = file_new(FD_RANDOM);
    else if (kind == LP_DEV_TTY) { f = file_new(FD_CONSOLE); f->con = p->con; }
    else {
        if (!n) {
            if (!(flags & O_CREAT)) return -ENOENT;
            char w[512]; to_wrld(labs, w, sizeof w);
            char dir[512]; path_dirname(w, dir, sizeof dir);
            vnode_t *dn = vfs_resolve(dir, NULL);
            if (!dn || !dn->dir) return -ENOENT;
            n = vfs_create(w, NULL, false);
            if (!n) return -EACCES;
        } else if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
        if ((flags & O_DIRECTORY) && !n->dir) return -ENOTDIR;
        if (n->dir && (flags & O_ACCMODE) != 0 && !(flags & O_PATH)) return -EISDIR;
        f = file_new(n->dir ? FD_DIR : FD_FILE);
        f->node = n;
        if ((flags & O_TRUNC) && !n->dir && (flags & O_ACCMODE)) vfs_write_node(n, "", 0);
    }
    f->oflags = flags;
    strlcpy(f->path, labs, sizeof f->path);
    int fd = lx_fd_alloc(p, f, 0);
    if (fd < 0) { kfree(f); return fd; }
    if (flags & O_CLOEXEC) p->fdflags[fd] = 1;
    (void)mode;
    return fd;
}

static int64_t file_read(lxproc_t *p, lxfile_t *f, uint64_t buf, uint64_t n, int64_t at) {
    if (!uvm_check(p->vm, buf, n, true)) return -EFAULT;
    uint8_t *dst = (uint8_t *)(uintptr_t)buf;
    switch (f->type) {
    case FD_NULL: return 0;
    case FD_ZERO: memset(dst, 0, n); return n;
    case FD_RANDOM: { static uint64_t s = 0x9E3779B97F4A7C15ull; for (uint64_t i = 0; i < n; i++) { s ^= rdtsc(); s ^= s << 13; s ^= s >> 7; s ^= s << 17; dst[i] = (uint8_t)s; } return n; }
    case FD_CONSOLE: {
        if (!f->con) return 0;
        if (f->line_pos >= f->line_len) {
            int l = f->con->readline(f->con, f->line, sizeof f->line - 1);
            if (l < 0) return 0;                                      /* end of input */
            f->line[l++] = '\n'; f->line_len = l; f->line_pos = 0;
        }
        uint64_t k = MIN(n, (uint64_t)(f->line_len - f->line_pos));
        memcpy(dst, f->line + f->line_pos, k); f->line_pos += k;
        return k;
    }
    case FD_DIR: return -EISDIR;
    case FD_FILE: {
        uint64_t pos = at >= 0 ? (uint64_t)at : f->pos, size = f->node->size;
        if (pos >= size) return 0;
        uint64_t k = MIN(n, size - pos);
        memcpy(dst, f->node->data + pos, k);
        if (at < 0) f->pos = pos + k;
        return k;
    }
    }
    return -EBADF;
}
static int64_t file_write(lxproc_t *p, lxfile_t *f, uint64_t buf, uint64_t n, int64_t at) {
    if (!uvm_check(p->vm, buf, n, false)) return -EFAULT;
    const uint8_t *src = (const uint8_t *)(uintptr_t)buf;
    switch (f->type) {
    case FD_NULL: case FD_ZERO: case FD_RANDOM: return n;
    case FD_CONSOLE: if (f->con && n) f->con->write(f->con, (const char *)src, n); return n;
    case FD_DIR: return -EISDIR;
    case FD_FILE: {
        if (!(f->oflags & O_ACCMODE)) return -EBADF;
        vnode_t *v = f->node;
        uint64_t pos = at >= 0 ? (uint64_t)at : ((f->oflags & O_APPEND) ? v->size : f->pos);
        if (pos + n > 0x7FFFFFFF) return -EFBIG;
        uint64_t ns = MAX((uint64_t)v->size, pos + n);
        if (pos == v->size && at < 0) { if (!vfs_append_node(v, src, n)) return -ENOSPC; }
        else {
            uint8_t *b = kmalloc(ns ? ns : 1);
            if (!b) return -ENOMEM;
            memcpy(b, v->data, v->size);
            if (pos > v->size) memset(b + v->size, 0, pos - v->size);
            memcpy(b + pos, src, n);
            bool ok = vfs_write_node(v, b, ns);
            kfree(b);
            if (!ok) return -ENOSPC;
        }
        if (at < 0) f->pos = pos + n;
        return n;
    }
    }
    return -EBADF;
}

typedef struct PACKED { uint64_t base, len; } iovec_t;

static int64_t getdents(lxproc_t *p, lxfile_t *f, uint64_t buf, uint64_t cap) {
    if (f->type != FD_DIR) return -ENOTDIR;
    if (!uvm_check(p->vm, buf, cap, true)) return -EFAULT;
    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    uint64_t o = 0;
    for (;;) {
        const char *name; int type; uint64_t ino;
        if (f->pos == 0) { name = "."; type = 4; ino = 1; }
        else if (f->pos == 1) { name = ".."; type = 4; ino = 2; }
        else {
            vnode_t *c = f->node->child;
            for (uint64_t i = 2; c && i < f->pos; i++) c = c->next;
            if (!c) break;
            name = c->name; type = c->dir ? 4 : 8; ino = ((uintptr_t)c >> 4) & 0xFFFFFFFF;
        }
        uint64_t nl = strlen(name), rl = (19 + nl + 1 + 7) & ~7ull;
        if (o + rl > cap) { if (!o) return -EINVAL; break; }
        memset(out + o, 0, rl);
        memcpy(out + o, &ino, 8);
        int64_t off = f->pos + 1; memcpy(out + o + 8, &off, 8);
        uint16_t r16 = rl; memcpy(out + o + 16, &r16, 2);
        out[o + 18] = type;
        memcpy(out + o + 19, name, nl);
        o += rl; f->pos++;
    }
    return o;
}

/* file-related system calls; *handled = false for numbers that are not ours */
int64_t lx_sys_file(lxthread_t *t, int nr, regs_t *r, bool *handled) {
    lxproc_t *p = t->proc;
    uint64_t a0 = r->edi, a1 = r->esi, a2 = r->edx, a3 = r->r10;
    *handled = true;
    switch (nr) {
    case 0: { lxfile_t *f = lx_fd_get(p, a0); return f ? file_read(p, f, a1, a2, -1) : -EBADF; }          /* read */
    case 1: { lxfile_t *f = lx_fd_get(p, a0); return f ? file_write(p, f, a1, a2, -1) : -EBADF; }         /* write */
    case 17: { lxfile_t *f = lx_fd_get(p, a0); return f ? file_read(p, f, a1, a2, a3) : -EBADF; }         /* pread64 */
    case 18: { lxfile_t *f = lx_fd_get(p, a0); return f ? file_write(p, f, a1, a2, a3) : -EBADF; }        /* pwrite64 */
    case 19: case 20: {                                                                                    /* readv, writev */
        lxfile_t *f = lx_fd_get(p, a0); if (!f) return -EBADF;
        int64_t total = 0;
        for (uint64_t i = 0; i < a2 && i < 1024; i++) {
            iovec_t v; if (!uin(&v, a1 + i * 16, 16)) return -EFAULT;
            if (!v.len) continue;
            int64_t k = nr == 19 ? file_read(p, f, v.base, v.len, -1) : file_write(p, f, v.base, v.len, -1);
            if (k < 0) return total ? total : k;
            total += k;
            if ((uint64_t)k < v.len) break;
        }
        return total;
    }
    case 2: return do_open(p, AT_FDCWD, a0, a1, a2);                                                        /* open */
    case 257: return do_open(p, (int)a0, a1, a2, a3);                                                       /* openat */
    case 85: return do_open(p, AT_FDCWD, a0, 01101, a1);                                                    /* creat */
    case 3: {                                                                                               /* close */
        lxfile_t *f = lx_fd_get(p, a0); if (!f) return -EBADF;
        p->fds[a0] = NULL; lx_file_put(f); return 0;
    }
    case 8: {                                                                                               /* lseek */
        lxfile_t *f = lx_fd_get(p, a0); if (!f) return -EBADF;
        if (f->type == FD_CONSOLE) return -ESPIPE;
        int64_t base = a2 == 0 ? 0 : a2 == 1 ? (int64_t)f->pos : a2 == 2 ? (f->node ? (int64_t)f->node->size : 0) : -1;
        if (base < 0) return -EINVAL;
        int64_t np = base + (int64_t)a1;
        if (np < 0) return -EINVAL;
        f->pos = np; return np;
    }
    case 4: case 6: case 5: case 262: {                                                                     /* stat, lstat, fstat, newfstatat */
        lstat_t st;
        if (nr == 5 || (nr == 262 && (a3 & 0x1000))) {                                                      /* fd, or AT_EMPTY_PATH */
            char tmp[2] = ""; if (nr == 262) { int rr = ustr(tmp, a1, 2); if (rr < 0) return rr; }
            if (nr == 5 || !tmp[0]) {
                lxfile_t *f = lx_fd_get(p, nr == 5 ? a0 : a0); if (!f) return -EBADF;
                fill_stat(&st, f->node, f->node ? LP_FILE : (f->type == FD_CONSOLE ? LP_DEV_TTY : LP_DEV_NULL));
                return uout(nr == 5 ? a1 : a2, &st, sizeof st) ? 0 : -EFAULT;
            }
        }
        char path[512]; int rr = ustr(path, nr == 262 ? a1 : a0, sizeof path); if (rr < 0) return rr;
        if (!path[0]) return -ENOENT;
        int kind; char labs[512];
        vnode_t *n = resolve(p, nr == 262 ? (int)a0 : AT_FDCWD, path, &kind, labs, sizeof labs);
        if (!n && (kind == LP_FILE || kind == LP_PROC_SELF_EXE)) return -ENOENT;
        fill_stat(&st, n, kind);
        return uout(nr == 262 ? a2 : a1, &st, sizeof st) ? 0 : -EFAULT;
    }
    case 332: {                                                                                             /* statx */
        char path[512]; int rr = ustr(path, a1, sizeof path); if (rr < 0) return rr;
        vnode_t *n; int kind = LP_FILE; char labs[512];
        if (!path[0] && (a2 & 0x1000)) { lxfile_t *f = lx_fd_get(p, a0); if (!f) return -EBADF; n = f->node; if (!n) kind = LP_DEV_TTY; }
        else { n = resolve(p, (int)a0, path, &kind, labs, sizeof labs); if (!n && kind == LP_FILE) return -ENOENT; }
        lstat_t st; fill_stat(&st, n, kind);
        uint8_t sx[256]; memset(sx, 0, sizeof sx);
        uint32_t mask = 0x7FF; memcpy(sx, &mask, 4);
        uint32_t blk = 4096; memcpy(sx + 4, &blk, 4);
        uint32_t nlink = 1, uid = 1000, gid = 1000; memcpy(sx + 16, &nlink, 4); memcpy(sx + 20, &uid, 4); memcpy(sx + 24, &gid, 4);
        uint16_t mode = st.mode; memcpy(sx + 28, &mode, 2);
        memcpy(sx + 32, &st.ino, 8); memcpy(sx + 40, &st.size, 8); memcpy(sx + 48, &st.blocks, 8);
        for (int k = 0; k < 4; k++) memcpy(sx + 64 + k * 16, &st.mtime, 8);   /* atime, btime, ctime, mtime */
        return uout(r->r8, sx, 256) ? 0 : -EFAULT;
    }
    case 21: case 269: case 439: {                                                                          /* access, faccessat, faccessat2 */
        char path[512]; int rr = ustr(path, nr == 21 ? a0 : a1, sizeof path); if (rr < 0) return rr;
        int kind; char labs[512];
        vnode_t *n = resolve(p, nr == 21 ? AT_FDCWD : (int)a0, path, &kind, labs, sizeof labs);
        return (n || (kind != LP_FILE && kind != LP_PROC_SELF_EXE)) ? 0 : -ENOENT;
    }
    case 217: { lxfile_t *f = lx_fd_get(p, a0); return f ? getdents(p, f, a1, a2) : -EBADF; }              /* getdents64 */
    case 89: case 267: {                                                                                    /* readlink, readlinkat */
        char path[512]; int rr = ustr(path, nr == 89 ? a0 : a1, sizeof path); if (rr < 0) return rr;
        uint64_t buf = nr == 89 ? a1 : a2, cap = nr == 89 ? a2 : a3;
        char labs[512]; int kind = lx_path2(p, nr == 89 ? AT_FDCWD : (int)a0, path, labs, sizeof labs, labs, sizeof labs);
        if (kind != LP_PROC_SELF_EXE) {
            if (!strncmp(labs, "/proc/self/fd/", 14)) { lxfile_t *f = lx_fd_get(p, atoi(labs + 14)); if (!f) return -ENOENT; uint64_t l = MIN(strlen(f->path), cap); return uout(buf, f->path, l) ? (int64_t)l : -EFAULT; }
            return -EINVAL;                                                                                 /* not a symbolic link */
        }
        uint64_t l = MIN(strlen(p->exe), cap);
        return uout(buf, p->exe, l) ? (int64_t)l : -EFAULT;
    }
    case 79: {                                                                                              /* getcwd */
        uint64_t l = strlen(p->cwd) + 1;
        if (l > a1) return -ERANGE;
        return uout(a0, p->cwd, l) ? (int64_t)l : -EFAULT;
    }
    case 80: case 81: {                                                                                     /* chdir, fchdir */
        char labs[512];
        if (nr == 81) { lxfile_t *f = lx_fd_get(p, a0); if (!f || f->type != FD_DIR) return -ENOTDIR; strlcpy(labs, f->path, sizeof labs); }
        else {
            char path[512]; int rr = ustr(path, a0, sizeof path); if (rr < 0) return rr;
            int kind; vnode_t *n = resolve(p, AT_FDCWD, path, &kind, labs, sizeof labs);
            if (!n) return -ENOENT;
            if (!n->dir) return -ENOTDIR;
        }
        strlcpy(p->cwd, labs, sizeof p->cwd);
        return 0;
    }
    case 83: case 258: {                                                                                    /* mkdir, mkdirat */
        char path[512]; int rr = ustr(path, nr == 83 ? a0 : a1, sizeof path); if (rr < 0) return rr;
        char w[512], labs[512]; lx_path2(p, nr == 83 ? AT_FDCWD : (int)a0, path, w, sizeof w, labs, sizeof labs);
        if (vfs_resolve(w, NULL)) return -EEXIST;
        return vfs_create(w, NULL, true) ? 0 : -ENOENT;
    }
    case 87: case 84: case 263: {                                                                           /* unlink, rmdir, unlinkat */
        char path[512]; int rr = ustr(path, nr == 263 ? a1 : a0, sizeof path); if (rr < 0) return rr;
        int kind; char labs[512];
        vnode_t *n = resolve(p, nr == 263 ? (int)a0 : AT_FDCWD, path, &kind, labs, sizeof labs);
        if (!n) return -ENOENT;
        bool want_dir = nr == 84 || (nr == 263 && (a2 & 0x200));
        if (want_dir && !n->dir) return -ENOTDIR;
        if (!want_dir && n->dir) return -EISDIR;
        if (n->dir && n->child) return -ENOTEMPTY;
        return vfs_delete(n) ? 0 : -EACCES;
    }
    case 82: case 264: case 316: {                                                                          /* rename, renameat, renameat2 */
        char from[512], to[512];
        int rr = ustr(from, nr == 82 ? a0 : a1, sizeof from); if (rr < 0) return rr;
        rr = ustr(to, nr == 82 ? a1 : a3, sizeof to); if (rr < 0) return rr;
        int kind; char la[512], lb[512];
        vnode_t *n = resolve(p, nr == 82 ? AT_FDCWD : (int)a0, from, &kind, la, sizeof la);
        if (!n) return -ENOENT;
        char wt[512]; lx_path2(p, nr == 82 ? AT_FDCWD : (int)a2, to, wt, sizeof wt, lb, sizeof lb);
        vnode_t *old = vfs_resolve(wt, NULL);
        if (old && old != n) { if (old->dir && old->child) return -ENOTEMPTY; vfs_delete(old); }
        char dir[512]; path_dirname(wt, dir, sizeof dir);
        vnode_t *dn = vfs_resolve(dir, NULL);
        if (!dn || !dn->dir) return -ENOENT;
        if (n->parent != dn && !vfs_move(n, dn)) return -EXDEV;
        return vfs_rename(n, path_basename(wt)) ? 0 : -EACCES;
    }
    case 76: case 77: {                                                                                     /* truncate, ftruncate */
        vnode_t *n;
        if (nr == 77) { lxfile_t *f = lx_fd_get(p, a0); if (!f || !f->node) return -EBADF; n = f->node; }
        else { char path[512]; int rr = ustr(path, a0, sizeof path); if (rr < 0) return rr; int kind; char labs[512]; n = resolve(p, AT_FDCWD, path, &kind, labs, sizeof labs); if (!n) return -ENOENT; }
        if (n->dir) return -EISDIR;
        uint64_t ns = a1;
        if (ns > 0x7FFFFFFF) return -EFBIG;
        uint8_t *b = kzalloc(ns ? ns : 1);
        memcpy(b, n->data, MIN(ns, (uint64_t)n->size));
        bool ok = vfs_write_node(n, b, ns); kfree(b);
        return ok ? 0 : -ENOSPC;
    }
    case 74: case 75: return lx_fd_get(p, a0) ? 0 : -EBADF;                                                 /* fsync, fdatasync */
    case 137: case 138: {                                                                                    /* statfs, fstatfs */
        uint64_t sf[15]; memset(sf, 0, sizeof sf);
        sf[0] = 0xEF53; sf[1] = 4096; sf[2] = 1u << 20; sf[3] = sf[4] = 1u << 19; sf[5] = 1u << 20; sf[6] = 1u << 19; sf[8] = 255; sf[9] = 4096;
        return uout(a1, sf, 120) ? 0 : -EFAULT;
    }
    case 90: case 91: case 92: case 93: case 94: case 268: case 260: case 280: case 235: case 132:          /* chmod, fchmod, chown... utimes */
        return 0;
    }
    *handled = false;
    return -ENOSYS;
}
#endif
