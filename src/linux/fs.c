/* Linux file system calls. Paths: "/" is C:\Linux, /mnt/<letter>/ is that
 * drive, /lib, /lib64, /bin and /sbin are the merged-/usr links of current
 * distributions, and a few /dev and /proc entries are built in. Files live in
 * the WRLD VFS (whole files in memory), so reads and mmap() copy from there. */
#ifdef __x86_64__
#include "lx.h"
#include "net.h"

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
    if (!strcmp(a, "/proc")) return LP_PROC_DIR;
    if (!strcmp(a, "/proc/self")) return LP_PROC_SELF_DIR;
    if (!strcmp(a, "/proc/self/exe")) return LP_PROC_SELF_EXE;
    if (!strcmp(a, "/proc/self/maps")) return LP_PROC_MAPS;
    if (!strcmp(a, "/proc/self/stat")) return LP_PROC_STAT;
    if (!strcmp(a, "/proc/self/status")) return LP_PROC_STATUS;
    if (!strcmp(a, "/proc/self/cmdline")) return LP_PROC_CMDLINE;
    if (!strcmp(a, "/proc/self/fd")) return LP_PROC_FD_DIR;
    if (!strncmp(a, "/proc/self/fd/", 14)) return LP_PROC_SELF_FD;
    if (!strcmp(a, "/proc/meminfo")) return LP_PROC_MEMINFO;
    if (!strcmp(a, "/proc/cpuinfo")) return LP_PROC_CPUINFO;
    if (!strcmp(a, "/proc/uptime")) return LP_PROC_UPTIME;
    if (!strcmp(a, "/proc/version")) return LP_PROC_VERSION;
    if (!strcmp(a, "/sys")) return LP_SYS;
    if (!strcmp(a, "/sys/devices")) return LP_SYS_DEVICES;
    if (!strcmp(a, "/sys/devices/system")) return LP_SYS_DEV_SYSTEM;
    if (!strcmp(a, "/sys/devices/system/cpu")) return LP_SYS_CPU;
    if (!strcmp(a, "/sys/kernel")) return LP_SYS_KERNEL;
    if (!strcmp(a, "/sys/devices/system/cpu/online")) return LP_SYS_CPU_ONLINE;
    if (!strcmp(a, "/sys/devices/system/cpu/possible")) return LP_SYS_CPU_POSSIBLE;
    if (!strcmp(a, "/sys/devices/system/cpu/present")) return LP_SYS_CPU_PRESENT;
    if (!strcmp(a, "/sys/kernel/osrelease")) return LP_SYS_OSRELEASE;
    if (!strcmp(a, "/sys/kernel/ostype")) return LP_SYS_OSTYPE;
    if (!strcmp(a, "/sys/kernel/version")) return LP_SYS_VERSION;
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
    int lim = (int)MIN(p->rlim[7][0], (uint64_t)LX_MAXFD);      /* RLIMIT_NOFILE */
    for (int i = MAX(min, 0); i < lim; i++) if (!p->fds[i]) { p->fds[i] = f; p->fdflags[i] = 0; return i; }
    return -EMFILE;
}
lxfile_t *lx_fd_get(lxproc_t *p, int fd) { return fd >= 0 && fd < LX_MAXFD ? p->fds[fd] : NULL; }
void lx_file_put(lxfile_t *f) { if (f && --f->refs <= 0) { lx_ipc_release(f); kfree(f->pdata); kfree(f); } }
static lxfile_t *file_new(int type) { lxfile_t *f = kzalloc(sizeof *f); f->type = type; f->refs = 1; return f; }

/* ---------------- /proc contents ---------------- */
typedef struct { char *b; int len, cap; } pbuf_t;
static bool pb_need(pbuf_t *b, int n) {
    if (b->len + n + 1 <= b->cap) return true;
    int nc = b->cap * 2;
    while (nc < b->len + n + 1) nc *= 2;
    char *nb = kmalloc(nc);
    if (!nb) return false;
    memcpy(nb, b->b, b->len);
    nb[b->len] = 0;
    kfree(b->b);
    b->b = nb; b->cap = nc;
    return true;
}
static void pb_add(pbuf_t *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (!pb_need(b, n)) return;
    n = MIN(n, (int)sizeof tmp - 1);
    memcpy(b->b + b->len, tmp, n);
    b->len += n;
    b->b[b->len] = 0;
}
static const char *proc_comm(lxproc_t *p, char *buf, int cap) {
    const char *n = p->exe, *s = n;
    for (const char *c = n; *c; c++) if (*c == '/' || *c == '\\') s = c + 1;
    strlcpy(buf, *s ? s : "?", cap);
    return buf;
}
static char *proc_text(lxproc_t *p, int kind, uint32_t *out)
{
    pbuf_t b = { kmalloc(4096), 0, 4096 };
    if (!b.b) return NULL;
    b.b[0] = 0;
    char comm[64]; proc_comm(p, comm, sizeof comm);
    switch (kind) {
    case LP_PROC_MAPS: {
        for (vma_t *v = p->vm->vmas; v; v = v->next) {
            const char *name = (v->flags & VMF_STACK) ? "[stack]" : (v->flags & VMF_HEAP) ? "[heap]" : "";
            pb_add(&b, "%016llx-%016llx %c%c%c%c %08llx %02x:%02x %-8u %s\n",
                   (unsigned long long)v->start, (unsigned long long)v->end,
                   (v->prot & VM_READ) ? 'r' : '-', (v->prot & VM_WRITE) ? 'w' : '-', (v->prot & VM_EXEC) ? 'x' : '-',
                   (v->flags & VMF_SHARED) ? 's' : 'p', (unsigned long long)v->foff, 8, 1,
                   v->fnode ? (unsigned)(((uintptr_t)v->fnode >> 4) & 0xFFFFFF) : 0u, name);
        }
        break;
    }
    case LP_PROC_STAT: {
        uint64_t vsize = 0, rss = (uint64_t)p->vm->pages * PAGE;
        for (vma_t *v = p->vm->vmas; v; v = v->next) vsize += v->end - v->start;
        task_t *self = task_current();
        lxthread_t *ct = lx_current();
        unsigned long ut = (self && ct && ct->proc == p) ? self->cpu_ms / 10 : 0;
        pb_add(&b, "%d (%s) R %d %d %d 0 -1 4194304 0 0 0 0 %lu 0 0 0 20 0 %d 0 0 %llu %llu 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
               p->pid, comm, p->ppid, p->pid, p->pid, ut, p->threads ? p->threads : 1,
               (unsigned long long)vsize, (unsigned long long)rss);
        break;
    }
    case LP_PROC_STATUS: {
        uint64_t vsize = 0;
        for (vma_t *v = p->vm->vmas; v; v = v->next) vsize += v->end - v->start;
        pb_add(&b, "Name:\t%s\nUmask:\t%04o\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nThreads:\t%d\n",
               comm, p->umask, p->pid, p->pid, p->ppid, p->threads ? p->threads : 1);
        pb_add(&b, "SigQ:\t0/0\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\nSigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n");
        pb_add(&b, "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\nCapBnd:\t0000000000000000\nCapAmb:\t0000000000000000\nNoNewPrivs:\t%d\nSeccomp:\t0\n",
               p->no_new_privs ? 1 : 0);
        pb_add(&b, "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\nVmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\n",
               (unsigned long long)(vsize / 1024), (unsigned long long)(vsize / 1024),
               (unsigned long long)((uint64_t)p->vm->pages * PAGE / 1024), (unsigned long long)((uint64_t)p->vm->pages * PAGE / 1024));
        pb_add(&b, "RssAnon:\t       0 kB\nRssFile:\t       0 kB\nRssShmem:\t       0 kB\nVmData:\t       0 kB\nVmStk:\t       0 kB\nVmExe:\t       0 kB\nVmLib:\t       0 kB\nVmPTE:\t       0 kB\nVmSwap:\t       0 kB\nHugetlbPages:\t       0 kB\n");
        break;
    }
    case LP_PROC_CMDLINE: {
        int n = strlen(p->exe) + 1;
        if (pb_need(&b, n)) { memcpy(b.b, p->exe, n - 1); b.b[n - 1] = 0; b.len = n; }
        break;
    }
    case LP_PROC_MEMINFO: {
        uint64_t totalkb = 256ull << 10;
        uint64_t usedkb = (uint64_t)pmm_used_pages() * PAGE / 1024;
        uint64_t freekb = totalkb > usedkb ? totalkb - usedkb : 0;
        pb_add(&b, "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\nBuffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\nActive:                0 kB\nInactive:              0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\nDirty:                 0 kB\nWriteback:             0 kB\nAnonPages:             0 kB\nMapped:                0 kB\nShmem:                 0 kB\nSlab:                  0 kB\nSReclaimable:          0 kB\nSUnreclaim:            0 kB\nPageTables:            0 kB\n",
               (unsigned long long)totalkb, (unsigned long long)freekb,
               (unsigned long long)freekb);
        break;
    }
    case LP_PROC_CPUINFO: {
        char vendor[16] = "", brand[64] = "";
        cpu_get_info(vendor, brand);
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        pb_add(&b, "processor\t: 0\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n",
               vendor[0] ? vendor : "GenuineIntel", (eax >> 8) & 0xF, (eax >> 4) & 0xF, brand[0] ? brand : "Unknown", eax & 0xF);
        pb_add(&b, "physical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\nflags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss syscall nx lm\n");
        break;
    }
    case LP_PROC_UPTIME: {
        uint32_t ms = timer_ticks();
        uint32_t idle = idle_ticks;
        pb_add(&b, "%u.%02u %u.%02u\n", ms / 1000, (ms % 1000) / 10, idle / 100, idle % 100);
        break;
    }
    case LP_PROC_VERSION:
        pb_add(&b, "Linux version 6.0.0-wrld (wrld@wrld) (gcc version 13.2.0) #1 SMP %s\n", OS_NAME);
        break;
    case LP_SYS_CPU_ONLINE: case LP_SYS_CPU_POSSIBLE: case LP_SYS_CPU_PRESENT:
        pb_add(&b, "0\n");
        break;
    case LP_SYS_OSRELEASE:
        pb_add(&b, "6.0.0-wrld\n");
        break;
    case LP_SYS_OSTYPE:
        pb_add(&b, "Linux\n");
        break;
    case LP_SYS_VERSION:
        pb_add(&b, "#1 SMP %s\n", OS_NAME);
        break;
    }
    *out = b.len;
    return b.b;
}
static char *proc_listing(lxproc_t *p, int kind, uint32_t *out)
{
    pbuf_t b = { kmalloc(1024), 0, 1024 };
    if (!b.b) return NULL;
    b.b[0] = 0;
    if (kind == LP_PROC_DIR) {
        pb_add(&b, "self 4\n%d 4\nmeminfo 8\ncpuinfo 8\nuptime 8\nversion 8\n", p->pid);
    } else if (kind == LP_PROC_SELF_DIR) {
        pb_add(&b, "fd 4\nmaps 8\nstat 8\nstatus 8\ncmdline 8\nexe 10\n");
    } else if (kind == LP_SYS) {
        pb_add(&b, "devices 4\nkernel 4\n");
    } else if (kind == LP_SYS_DEVICES) {
        pb_add(&b, "system 4\n");
    } else if (kind == LP_SYS_DEV_SYSTEM) {
        pb_add(&b, "cpu 4\n");
    } else if (kind == LP_SYS_CPU) {
        pb_add(&b, "online 8\npossible 8\npresent 8\n");
    } else if (kind == LP_SYS_KERNEL) {
        pb_add(&b, "osrelease 8\nostype 8\nversion 8\n");
    } else {
        for (int i = 0; i < LX_MAXFD; i++) if (p->fds[i]) pb_add(&b, "%d 10\n", i);
    }
    *out = b.len;
    return b.b;
}


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
    if (kind == LP_PROC_DIR || kind == LP_PROC_SELF_DIR || kind == LP_PROC_FD_DIR ||
        (kind >= LP_SYS && kind <= LP_SYS_KERNEL)) { st->mode = 040555; st->nlink = 2; st->ino = 200 + kind; return; }
    if (kind == LP_PROC_SELF_FD) { st->mode = 0120777; st->ino = 200 + kind; return; }
    if ((kind >= LP_PROC_MAPS && kind <= LP_PROC_VERSION) ||
        (kind >= LP_SYS_CPU_ONLINE && kind <= LP_SYS_VERSION)) { st->mode = 0100444; st->ino = 200 + kind; return; }
    if (kind != LP_FILE && kind != LP_PROC_SELF_EXE) { st->mode = 020666; st->rdev = 0x103; st->ino = 100 + kind; return; }
    st->ino = ((uintptr_t)n >> 4) & 0xFFFFFFFF;
    st->mode = n->dir ? 040755 : 0100755;
    st->size = n->dir ? 4096 : n->size;
    st->blocks = (st->size + 511) / 512;
    st->atime = st->mtime = st->ctime = node_time(n);
}

/* glibc's name resolver insists on reading a few configuration files before it
 * will use our sockets.  Create them in the RAM file system the first time they
 * are looked up; resolv.conf follows whatever DHCP handed us. */
static vnode_t *etc_seed(const char *abs, const char *wrld) {
    static const struct { const char *path, *text; } fixed[] = {
        { "/etc/host.conf",     "order hosts,bind\nmulti on\n" },
        { "/etc/nsswitch.conf", "hosts: files dns\n" },
        { "/etc/hosts",         "# The only names this machine knows without DNS.\n"
                                "127.0.0.1\tlocalhost\n::1\t\tlocalhost\n" },
        { "/etc/services",      "ftp 21/tcp\nssh 22/tcp\nsmtp 25/tcp\ndomain 53/udp\n"
                                "http 80/tcp\nhttps 443/tcp\nntp 123/udp\n" },
    };
    if (strncmp(abs, "/etc", 4) || (abs[4] && abs[4] != '/')) return NULL;
    if (!vfs_resolve("C:\\Linux", NULL)) return NULL;      /* no Linux root installed */
    vfs_mkdir_p("C:\\Linux\\etc");
    for (unsigned i = 0; i < ARRAY_LEN(fixed); i++)
        if (!strcmp(abs, fixed[i].path) && !vfs_resolve(wrld, NULL))
            vfs_write_file(wrld, fixed[i].text, strlen(fixed[i].text));
    if (!strcmp(abs, "/etc/resolv.conf")) {
        char text[64];
        if (netif.dns) {
            char a[16]; ip_to_str(netif.dns, a);
            snprintf(text, sizeof text, "nameserver %s\n", a);
        } else strlcpy(text, "# no DNS server configured\n", sizeof text);
        vfs_write_file(wrld, text, strlen(text));
    }
    return vfs_resolve(wrld, NULL);
}

/* resolve a Linux path to a node (NULL: not found) */
static vnode_t *resolve(lxproc_t *p, int dirfd, const char *upath, int *kind, char *labs, int lcap) {
    char w[512];
    int k = lx_path2(p, dirfd, upath, w, sizeof w, labs, lcap);
    if (kind) *kind = k;
    if (k == LP_PROC_SELF_EXE) { char we[512]; to_wrld(p->exe, we, sizeof we); return vfs_resolve(we, NULL); }
    if (k != LP_FILE) return NULL;
    vnode_t *n = vfs_resolve(w, NULL);
    if (!n) n = etc_seed(labs, w);
    return n;
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
    else if (kind == LP_PROC_SELF_FD) {                     /* opening an fd link is a dup */
        int fd = atoi(labs + 14);
        lxfile_t *src = lx_fd_get(p, fd);
        if (!src) return -ENOENT;
        src->refs++;
        int nfd = lx_fd_alloc(p, src, 0);
        if (nfd < 0) { lx_file_put(src); return nfd; }
        if (flags & O_CLOEXEC) p->fdflags[nfd] = 1;
        return nfd;
    } else if (kind == LP_PROC_DIR || kind == LP_PROC_SELF_DIR || kind == LP_PROC_FD_DIR ||
               (kind >= LP_SYS && kind <= LP_SYS_KERNEL)) {
        if ((flags & O_ACCMODE) && !(flags & O_PATH)) return -EISDIR;
        f = file_new(FD_DIR);
        f->pdata = proc_listing(p, kind, &f->plen);
        if (!f->pdata) { kfree(f); return -ENOMEM; }
    } else if ((kind >= LP_PROC_MAPS && kind <= LP_PROC_VERSION) ||
               (kind >= LP_SYS_CPU_ONLINE && kind <= LP_SYS_VERSION)) {
        if (flags & O_DIRECTORY) return -ENOTDIR;
        if ((flags & O_ACCMODE) && !(flags & O_PATH)) return -EACCES;
        f = file_new(FD_FILE);
        f->pdata = proc_text(p, kind, &f->plen);
        if (!f->pdata) { kfree(f); return -ENOMEM; }
    } else {
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
    case FD_PIPE: case FD_SOCKET: case FD_EVENTFD: case FD_TIMERFD:
        return lx_ipc_read(f, dst, n, (f->oflags & O_NONBLOCK) != 0);
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
        const uint8_t *data = f->node ? f->node->data : (const uint8_t *)f->pdata;
        uint64_t pos = at >= 0 ? (uint64_t)at : f->pos, size = f->node ? f->node->size : f->plen;
        if (!data || pos >= size) return 0;
        uint64_t k = MIN(n, size - pos);
        memcpy(dst, data + pos, k);
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
    case FD_PIPE: case FD_SOCKET: case FD_EVENTFD: case FD_TIMERFD:
        return lx_ipc_write(f, src, n, (f->oflags & O_NONBLOCK) != 0);
    case FD_CONSOLE: if (f->con && n) f->con->write(f->con, (const char *)src, n); return n;
    case FD_DIR: return -EISDIR;
    case FD_FILE: {
        if (!f->node) return -EACCES;                       /* generated /proc files are read-only */
        if (!(f->oflags & O_ACCMODE)) return -EBADF;
        vnode_t *v = f->node;
        uint64_t pos = at >= 0 ? (uint64_t)at : ((f->oflags & O_APPEND) ? v->size : f->pos);
        if (pos + n > 0x7FFFFFFF || pos + n > p->rlim[1][0]) return -EFBIG;   /* RLIMIT_FSIZE */
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
        char nm[64];
        if (f->pos == 0) { name = "."; type = 4; ino = 1; }
        else if (f->pos == 1) { name = ".."; type = 4; ino = 2; }
        else {
            vnode_t *c = NULL;
            if (f->node) {
                c = f->node->child;
                for (uint64_t i = 2; c && i < f->pos; i++) c = c->next;
                if (!c) break;
                name = c->name; type = c->dir ? 4 : 8; ino = ((uintptr_t)c >> 4) & 0xFFFFFFFF;
            } else if (f->pdata && f->pos >= 2) {
                const char *s = f->pdata;
                for (uint64_t i = 2; s && i < f->pos; i++) {
                    while (*s && *s != '\n') s++;
                    if (*s) s++;
                    else s = NULL;
                }
                if (!s || !*s) break;
                const char *e = s; while (*e && *e != '\n') e++;
                int l = MIN((int)(e - s), (int)sizeof nm - 1);
                memcpy(nm, s, l); nm[l] = 0;
                type = 8;
                char *sp = strchr(nm, ' ');
                if (sp) { *sp = 0; type = atoi(sp + 1); }
                name = nm; ino = 0x200 + (uintptr_t)f->pos;
            } else break;
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
        if (f->type == FD_CONSOLE || f->type >= FD_PIPE) return -ESPIPE;
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
