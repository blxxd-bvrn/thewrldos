/* Linux processes: ELF loading (position-independent executables and the
 * dynamic loader they name), the initial stack (argv, envp, auxv), threads as
 * kernel tasks, ring-3 faults, preemption, and teardown. */
#ifdef __x86_64__
#include "lx.h"

typedef struct PACKED {
    uint8_t ident[16]; uint16_t type, machine; uint32_t version;
    uint64_t entry, phoff, shoff; uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ehdr_t;
typedef struct PACKED { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } phdr_t;
enum { PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6 };
enum { ET_EXEC = 2, ET_DYN = 3 };

bool lx_is_elf(const uint8_t *d, uint32_t n) { return n >= 64 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F'; }

lxthread_t *lx_current(void) { task_t *t = task_current(); return t ? LX(t) : NULL; }

typedef struct { uint64_t base, entry, phdr, phnum, end; char interp[256]; } loaded_t;

static bool load_elf(uvm_t *vm, vnode_t *n, uint64_t dyn_base, loaded_t *out, char *err, int ecap) {
    const uint8_t *d = n->data;
    if (!lx_is_elf(d, n->size)) { snprintf(err, ecap, "%s is not an ELF program.", n->name); return false; }
    const ehdr_t *eh = (const ehdr_t *)d;
    if (d[4] != 2 || d[5] != 1 || eh->machine != 62) { snprintf(err, ecap, "%s is not a 64-bit x86-64 Linux program.", n->name); return false; }
    if (eh->type != ET_EXEC && eh->type != ET_DYN) { snprintf(err, ecap, "%s is not an executable.", n->name); return false; }
    if (eh->phoff + (uint64_t)eh->phnum * sizeof(phdr_t) > n->size || eh->phentsize != sizeof(phdr_t)) { snprintf(err, ecap, "%s: bad program headers.", n->name); return false; }
    const phdr_t *ph = (const phdr_t *)(d + eh->phoff);
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < eh->phnum; i++) if (ph[i].type == PT_LOAD) { lo = MIN(lo, ph[i].vaddr & ~(PAGE - 1)); hi = MAX(hi, ph[i].vaddr + ph[i].memsz); }
    if (lo == ~0ull) { snprintf(err, ecap, "%s has nothing to load.", n->name); return false; }
    uint64_t base = eh->type == ET_DYN ? dyn_base - lo : 0;
    if (eh->type == ET_EXEC && lo < USER_LO) {
        snprintf(err, ecap, "%s is not position-independent (it was linked at 0x%llx). Only PIE programs, the default on current Linux distributions, can run.", n->name, (unsigned long long)lo);
        return false;
    }
    memset(out, 0, sizeof *out);
    for (int i = 0; i < eh->phnum; i++) {
        const phdr_t *p = &ph[i];
        if (p->type == PT_INTERP && p->filesz < sizeof out->interp && p->offset + p->filesz <= n->size) {
            memcpy(out->interp, d + p->offset, p->filesz); out->interp[p->filesz] = 0;
        }
        if (p->type == PT_PHDR) out->phdr = base + p->vaddr;
        if (p->type != PT_LOAD || !p->memsz) continue;
        if (p->offset + p->filesz > n->size) { snprintf(err, ecap, "%s is truncated.", n->name); return false; }
        uint64_t va = base + p->vaddr, start = va & ~(PAGE - 1), end = PALIGN(va + p->memsz);
        int prot = (p->flags & 4 ? VM_READ : 0) | (p->flags & 2 ? VM_WRITE : 0) | (p->flags & 1 ? VM_EXEC : 0);
        /* bytes past filesz come out as zeros (.bss), because the file data ends there */
        int64_t r = uvm_map(vm, start, end - start, prot, VMF_PRIVATE, true, d, p->offset + p->filesz, p->offset - (va - start), n);
        if (r < 0) { snprintf(err, ecap, "Could not map %s.", n->name); return false; }
    }
    if (!out->phdr) out->phdr = base + lo + eh->phoff;        /* the headers are inside the first segment */
    out->base = base; out->entry = base + eh->entry; out->phnum = eh->phnum; out->end = base + hi;
    return true;
}

/* write into a process that is not running yet, through the identity map */
static bool poke(uvm_t *vm, uint64_t va, const void *src, uint64_t len) {
    const uint8_t *s = src;
    while (len) {
        if (!uvm_fault(vm, va, true)) return false;
        extern uint64_t *lx_pte_lookup(uvm_t *vm, uint64_t va);
        uint64_t *pte = lx_pte_lookup(vm, va);
        if (!pte) return false;
        uint8_t *pg = (uint8_t *)(uintptr_t)(*pte & 0x000FFFFFFFFFF000ull);
        uint64_t o = va & (PAGE - 1), k = MIN(len, PAGE - o);
        memcpy(pg + o, s, k);
        va += k; s += k; len -= k;
    }
    return true;
}

static lxproc_t *proc_new(void) {
    lxproc_t *p = kzalloc(sizeof *p);
    p->vm = uvm_new();
    if (!p->vm) { kfree(p); return NULL; }
    strcpy(p->cwd, "/");
    p->umask = 022;
    p->dumpable = 1;
    for (int i = 0; i < 16; i++) p->rlim[i][0] = p->rlim[i][1] = ~0ull;
    p->rlim[3][0] = p->rlim[3][1] = STACK_SIZE;
    p->rlim[7][0] = p->rlim[7][1] = LX_MAXFD;
    return p;
}

static void lx_thread_start(void *arg) {
    lxthread_t *th = arg;
    th->slice_start = timer_ticks();
    extern void arch_set_user_fs(uint64_t base);
    arch_set_user_fs(th->fsbase);
    enter_user(&th->start_regs);
}

static task_t *thread_new(lxproc_t *p, const char *name, const regs_t *regs, uint64_t fsbase) {
    lxthread_t *th = kzalloc(sizeof *th);
    th->proc = p;
    strlcpy(th->comm, name, sizeof th->comm);
    th->start_regs = *regs;
    th->fsbase = fsbase;
    task_t *t = task_create(name, lx_thread_start, th, 64 * 1024);
    t->lx = th; t->kind = 4;
    t->cr3 = uvm_cr3(p->vm);
    t->console = p->con;
    th->tid = t->pid;
    p->threads++;
    return t;
}

/* clone() for threads: same address space, files and signal handlers */
#define CLONE_VM 0x100
#define CLONE_THREAD 0x10000
#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID 0x1000000
int64_t lx_clone(regs_t *r, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls) {
    task_t *self = task_current();
    lxthread_t *th = LX(self);
    lxproc_t *p = th->proc;
    if (!(flags & CLONE_THREAD)) {
        /* A new process. fork() is clone(SIGCHLD); posix_spawn() is clone(CLONE_VM |
         * CLONE_VFORK) on a stack of its own, where the child only prepares and calls execve.
         * Processes here cannot share one address space, so the child gets a copy either way,
         * starting on the stack it was given. The parent of a failed posix_spawn exec then
         * learns of it from the child's exit status (127) rather than from the call. */
        extern int64_t lx_fork_ex(regs_t *r, uint64_t stack, bool set_tls, uint64_t tls, uint64_t flags, uint64_t ptid, uint64_t ctid);
        return lx_fork_ex(r, stack, (flags & CLONE_SETTLS) != 0, tls, flags, ptid, ctid);
    }
    if (!(flags & CLONE_VM)) return -EINVAL;                                /* a thread shares the memory */
    regs_t nr = *r;
    nr.eax = 0;                                                             /* the child sees 0 */
    if (stack) nr.esp = stack;
    char name[48]; snprintf(name, sizeof name, "%s:thread", self->name);
    char *colon = strstr(self->name, ":thread"); if (colon) strlcpy(name, self->name, sizeof name);
    task_t *t = thread_new(p, name, &nr, (flags & CLONE_SETTLS) ? tls : th->fsbase);
    lxthread_t *ct = LX(t);
    ct->sigmask = th->sigmask;
    if (flags & CLONE_CHILD_CLEARTID) ct->clear_child_tid = ctid;
    if ((flags & CLONE_PARENT_SETTID) && uvm_check(p->vm, ptid, 4, true)) *(volatile int32_t *)(uintptr_t)ptid = ct->tid;
    if ((flags & CLONE_CHILD_SETTID) && uvm_check(p->vm, ctid, 4, true)) *(volatile int32_t *)(uintptr_t)ctid = ct->tid;
    return ct->tid;
}

/* Load a program and its loader into `vm`, then build the Linux start-up stack.
 * Used both by the shell (a new process) and by execve (an old one). */
static bool build_image(lxproc_t *p, vnode_t *n, uvm_t *vm, int argc, char **argv, char **envp,
                        regs_t *out, char *err, int ecap) {
    loaded_t exe, in;
    if (!load_elf(vm, n, EXE_BASE, &exe, err, ecap)) return false;
    uint64_t entry = exe.entry, interp_base = 0;
    if (exe.interp[0]) {
        char ip[512];
        lx_path(p, -100, exe.interp, ip, sizeof ip);
        vnode_t *in_n = vfs_resolve(ip, NULL);
        if (!in_n || in_n->dir) { snprintf(err, ecap, "%s needs %s (the Linux dynamic loader), which is not installed.", n->name, exe.interp); return false; }
        if (!load_elf(vm, in_n, INTERP_BASE, &in, err, ecap)) return false;
        entry = in.entry; interp_base = in.base;
    }
    /* program break just past the executable */
    vm->brk_start = vm->brk = PALIGN(exe.end) + 16 * PAGE;
    /* Linux path of the program */
    {
        char wp[512]; vfs_path(n, wp, sizeof wp);
        const char *lin = "C:\\Linux";
        if (!strncasecmp(wp, lin, 8) && (wp[8] == '\\' || !wp[8])) { snprintf(p->exe, sizeof p->exe, "%s", wp[8] ? wp + 8 : "/"); }
        else if (wp[1] == ':') snprintf(p->exe, sizeof p->exe, "/mnt/%c%s", tolower(wp[0]), wp + 2);
        else strlcpy(p->exe, wp, sizeof p->exe);
        for (char *c = p->exe; *c; c++) if (*c == '\\') *c = '/';
    }
    /* stack */
    if (uvm_map(vm, STACK_TOP - STACK_SIZE, STACK_SIZE, VM_READ | VM_WRITE, VMF_STACK, true, NULL, 0, 0, NULL) < 0) { snprintf(err, ecap, "Could not map the stack."); return false; }
    {
        static const char *defenv[] = { "PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/home/user", "USER=user", "LOGNAME=user",
                                        "SHELL=/bin/sh", "TERM=dumb", "LANG=C.UTF-8", "PWD=/", NULL };
        char **env = envp ? envp : (char **)defenv;
        int envc = 0; while (env[envc]) envc++;
        uint64_t sp = STACK_TOP;
        uint64_t *argp = kmalloc(sizeof(uint64_t) * (argc + envc + 2));
        if (!argp) { snprintf(err, ecap, "Not enough memory for the program's arguments."); return false; }
        /* strings */
        #define PUSH_STR(s) ({ uint64_t l_ = strlen(s) + 1; sp -= l_; poke(vm, sp, (s), l_); sp; })
        uint64_t execfn = PUSH_STR(p->exe);
        for (int i = 0; i < envc; i++) argp[argc + i] = PUSH_STR(env[i]);
        for (int i = argc - 1; i >= 0; i--) argp[i] = PUSH_STR(argv[i]);
        uint64_t platform = PUSH_STR("x86_64");
        uint8_t rnd[16]; for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(rdtsc() >> (i % 8 * 3)) ^ (uint8_t)(i * 73);
        sp -= 16; poke(vm, sp, rnd, 16); uint64_t random = sp;
        sp &= ~15ull;
        uint32_t ca, cb, cc, cd; __asm__ volatile("cpuid" : "=a"(ca), "=b"(cb), "=c"(cc), "=d"(cd) : "a"(1));
        uint64_t aux[] = {
            3, exe.phdr, 4, sizeof(phdr_t), 5, exe.phnum, 6, PAGE, 7, interp_base, 8, 0, 9, exe.entry,
            11, 1000, 12, 1000, 13, 1000, 14, 1000, 15, platform, 16, cd, 17, 100, 23, 0, 25, random,
            26, 0, 31, execfn, 51, 4096, 0, 0 };
        uint64_t words = 1 + argc + 1 + envc + 1 + ARRAY_LEN(aux);
        if (words & 1) sp -= 8;                                 /* rsp % 16 == 0 at argc */
        sp -= words * 8;
        uint64_t *blk = kmalloc(words * 8), w = 0;
        if (!blk) { kfree(argp); snprintf(err, ecap, "Not enough memory for the program's stack."); return false; }
        blk[w++] = argc;
        for (int i = 0; i < argc; i++) blk[w++] = argp[i];
        blk[w++] = 0;
        for (int i = 0; i < envc; i++) blk[w++] = argp[argc + i];
        blk[w++] = 0;
        for (unsigned i = 0; i < ARRAY_LEN(aux); i++) blk[w++] = aux[i];
        bool ok = poke(vm, sp, blk, words * 8);
        kfree(blk); kfree(argp);
        if (!ok) { snprintf(err, ecap, "Not enough memory for the program's stack."); return false; }
        memset(out, 0, sizeof *out);
        out->eip = entry; out->esp = sp; out->cs = 0x2B; out->ss = 0x23; out->eflags = 0x202;
    }
    return true;
}

int lx_spawn(const char *wrld_path, int argc, char **argv, char **envp, console_t *in_con, console_t *con, char *err, int ecap) {
    vnode_t *n = vfs_resolve(wrld_path, NULL);
    if (!n || n->dir) { snprintf(err, ecap, "File not found: %s", wrld_path); return -1; }
    lxproc_t *p = proc_new();
    if (!p) { snprintf(err, ecap, "Not enough memory to start the program."); return -1; }
    p->con = con;
    regs_t r;
    if (!build_image(p, n, p->vm, argc, argv, envp, &r, err, ecap)) goto fail;
    /* stdin/stdout/stderr: the console */
    lxfile_t *cf = kzalloc(sizeof *cf);
    if (!cf) { snprintf(err, ecap, "Not enough memory to start the program."); goto fail; }
    cf->type = FD_CONSOLE; cf->con = con; cf->oflags = 2; strcpy(cf->path, "/dev/tty");
    for (int i = 1; i < 3; i++) { p->fds[i] = cf; cf->refs++; }
    lxfile_t *ci = cf;
    if (in_con && in_con != con) { ci = kzalloc(sizeof *ci); ci->type = FD_CONSOLE; ci->con = in_con; strcpy(ci->path, "/dev/tty"); }
    p->fds[0] = ci; ci->refs++;
    const char *base = path_basename(wrld_path);
    task_t *t = thread_new(p, base, &r, 0);
    p->pid = t->pid;
    klog("linux: started %s as pid %d (entry %llx)", p->exe, p->pid, (unsigned long long)r.eip);
    return p->pid;
fail:
    for (int i = 0; i < LX_MAXFD; i++) if (p->fds[i]) { lx_file_put(p->fds[i]); p->fds[i] = NULL; }
    uvm_free(p->vm); kfree(p);
    return -1;
}

/* a program started from the desktop has no console window: what it writes goes to
 * the boot log a line at a time, and it reads end-of-file */
static char logcon_buf[256]; static int logcon_len;
static void logcon_write(console_t *c, const char *s, int n) {
    (void)c;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\n' || logcon_len == (int)sizeof logcon_buf - 1) {
            logcon_buf[logcon_len] = 0;
            if (logcon_len) klog("linux: %s", logcon_buf);
            logcon_len = 0;
            if (s[i] == '\n') continue;
        }
        if (s[i] != '\r') logcon_buf[logcon_len++] = s[i];
    }
}
static int logcon_readline(console_t *c, char *buf, int cap) { (void)c; if (cap) buf[0] = 0; return 0; }
static int logcon_getkey(console_t *c) { (void)c; return -1; }
static void logcon_none(console_t *c) { (void)c; }
static void logcon_title(console_t *c, const char *t) { (void)c; (void)t; }
static void logcon_color(console_t *c, int fg, int bg) { (void)c; (void)fg; (void)bg; }
static bool logcon_alive(console_t *c) { (void)c; return true; }
static console_t log_console = { logcon_write, logcon_readline, logcon_getkey, logcon_none, logcon_title, logcon_color, logcon_alive };
/* start a Linux program in the background (for the desktop: no console) */
int lx_launch(const char *wrld_path, int argc, char **argv, char **envp, char *err, int ecap) {
    return lx_spawn(wrld_path, argc, argv, envp, &log_console, &log_console, err, ecap);
}

/* fork(): a second process with a copy of this address space, the same open
 * files and the same thread state. The child returns 0 from the same call. */
int64_t lx_fork_ex(regs_t *r, uint64_t stack, bool set_tls, uint64_t tls, uint64_t flags, uint64_t ptid, uint64_t ctid);
int64_t lx_fork(regs_t *r) { return lx_fork_ex(r, 0, false, 0, 0, 0, 0); }
int64_t lx_fork_ex(regs_t *r, uint64_t stack, bool set_tls, uint64_t tls, uint64_t flags, uint64_t ptid, uint64_t ctid) {
    task_t *self = task_current();
    lxthread_t *th = LX(self);
    lxproc_t *p = th->proc;
    lxproc_t *c = kzalloc(sizeof *c);
    if (!c) return -ENOMEM;
    c->vm = uvm_clone(p->vm);
    if (!c->vm) { kfree(c); return -ENOMEM; }
    c->con = p->con; c->umask = p->umask; c->ppid = p->pid;
    memcpy(c->acts, p->acts, sizeof c->acts);        /* fork inherits the dispositions */
    memcpy(c->rlim, p->rlim, sizeof c->rlim);
    c->no_new_privs = p->no_new_privs; c->dumpable = p->dumpable; c->pdeathsig = p->pdeathsig;
    strlcpy(c->cwd, p->cwd, sizeof c->cwd);
    strlcpy(c->exe, p->exe, sizeof c->exe);
    for (int i = 0; i < LX_MAXFD; i++) {
        c->fds[i] = p->fds[i];
        c->fdflags[i] = p->fdflags[i];
        if (c->fds[i]) c->fds[i]->refs++;
    }
    regs_t nr = *r;
    nr.eax = 0;                                                 /* the child sees 0 */
    if (stack) nr.esp = stack;
    char name[48]; snprintf(name, sizeof name, "%s:fork", self->name);
    task_t *t = thread_new(c, name, &nr, set_tls ? tls : th->fsbase);
    lxthread_t *ct = LX(t);
    ct->sigmask = th->sigmask;
    c->pid = t->pid;
    /* the child's tid in the parent's memory (CLONE_PARENT_SETTID) and in the child's copy */
    if ((flags & CLONE_PARENT_SETTID) && uvm_check(p->vm, ptid, 4, true)) *(volatile int32_t *)(uintptr_t)ptid = ct->tid;
    if (flags & CLONE_CHILD_CLEARTID) ct->clear_child_tid = ctid;
    if ((flags & CLONE_CHILD_SETTID) && ctid) uvm_poke32(c->vm, ctid, (uint32_t)ct->tid);
    klog("linux: pid %d forked pid %d", p->pid, c->pid);
    return c->pid;
}

static int user_str(lxproc_t *p, char *dst, uint64_t src, int cap) {
    for (int i = 0; i < cap; i++) {
        if (!uvm_check(p->vm, src + i, 1, false)) return -EFAULT;
        char c = *(volatile char *)(uintptr_t)(src + i);
        dst[i] = c;
        if (!c) return i;
    }
    return -ENAMETOOLONG;
}
#define LX_MAXARG 256
static int copy_vec(lxproc_t *p, uint64_t uvec, char **out) {
    int n = 0;
    if (!uvec) { out[0] = NULL; return 0; }
    for (int i = 0; i < LX_MAXARG; i++) {
        uint64_t sp;
        if (!uvm_check(p->vm, uvec + i * 8, 8, false)) return -EFAULT;
        memcpy(&sp, (void *)(uintptr_t)(uvec + i * 8), 8);
        if (!sp) { out[n] = NULL; return n; }
        char tmp[512];
        int l = user_str(p, tmp, sp, sizeof tmp);
        if (l < 0) return l;
        out[n] = kmalloc(l + 1);
        if (!out[n]) return -ENOMEM;
        memcpy(out[n], tmp, l + 1);
        n++;
    }
    return -E2BIG;
}
static void free_vec(char **v, int n) { for (int i = 0; i < n; i++) if (v[i]) kfree(v[i]); }

/* execve(): replace this process's image. Everything that belongs to the old
 * program is dropped, and the call returns into the new entry point. */
int64_t lx_execve(regs_t *r, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    task_t *self = task_current();
    lxthread_t *th = LX(self);
    lxproc_t *p = th->proc;
    char path[512];
    int rr = user_str(p, path, upath, sizeof path);
    if (rr < 0) return rr;
    if (!path[0]) return -ENOENT;
    char *argv[LX_MAXARG + 1], *envp[LX_MAXARG + 1];
    int argc = copy_vec(p, uargv, argv);
    if (argc < 0) return argc;
    int envc = copy_vec(p, uenvp, envp);
    if (envc < 0) { free_vec(argv, argc); return envc; }
    if (!argc) { argv[0] = kmalloc(strlen(path) + 1); if (argv[0]) { strcpy(argv[0], path); argc = 1; } }
    /* the Linux path of the program */
    char wrld[512];
    int kind = lx_path(p, -100, path, wrld, sizeof wrld);
    vnode_t *n = kind == LP_FILE ? vfs_resolve(wrld, NULL) : NULL;
    int64_t ret = -ENOEXEC;
    if (!n || n->dir) { ret = -ENOENT; goto out; }
    uvm_t *nv = uvm_new();
    if (!nv) { ret = -ENOMEM; goto out; }
    regs_t nr; char err[128];
    if (!build_image(p, n, nv, argc, argv, envc ? envp : NULL, &nr, err, sizeof err)) {
        klog("linux: execve %s: %s", path, err);
        uvm_free(nv);
        ret = -ENOEXEC;
        goto out;
    }
    /* every other thread of this process ends here. Mark them as having left
     * through exit_group, so reaping one does not take the process down. */
    for (task_t *o = task_list(); o;) {
        task_t *nx = o->next;
        if (o != self && o->lx && LX(o)->proc == p) { LX(o)->exited = true; task_kill(o->pid); }
        o = nx;
    }
    /* close-on-exec descriptors */
    for (int i = 0; i < LX_MAXFD; i++)
        if (p->fds[i] && p->fdflags[i]) { lx_file_put(p->fds[i]); p->fds[i] = NULL; p->fdflags[i] = 0; }
    /* switch address spaces before releasing the old one */
    uvm_t *old = p->vm;
    p->vm = nv;
    self->cr3 = uvm_cr3(nv);
    __asm__ volatile("mov %0, %%cr3" :: "r"(p->vm->pml4) : "memory");
    uvm_free(old);
    /* thread state that belonged to the old image */
    th->clear_child_tid = 0; th->robust_list = 0; th->sigmask = 0; th->fsbase = 0;
    { extern void arch_set_user_fs(uint64_t); arch_set_user_fs(0); }
    lx_sig_reset(p);
    p->exiting = false; p->exit_code = 0; p->term_sig = 0;
    strlcpy(self->name, path_basename(p->exe), sizeof self->name);
    *r = nr;
    ret = 0;
out:
    free_vec(argv, argc);
    free_vec(envp, envc);
    return ret;
}

/* wait4(): collect a child that has ended. There are no signals yet, so a
 * parent without WNOHANG sleeps until one of its children is reaped. */
typedef struct lxzombie { int pid, ppid, status; struct lxzombie *next; } lxzombie_t;
static lxzombie_t *zombies;

static void record_zombie(lxproc_t *p) {
    if (p->ppid <= 0) return;
    lxzombie_t *z = kmalloc(sizeof *z);
    if (!z) return;
    z->pid = p->pid; z->ppid = p->ppid;
    z->status = p->term_sig ? (p->term_sig & 0x7F) : ((p->exit_code & 0xFF) << 8);
    z->next = zombies; zombies = z;
}
static bool child_exists(lxproc_t *p, int pid) {
    for (task_t *t = task_list(); t; t = t->next)
        if (t->lx && LX(t)->proc != p && LX(t)->proc->ppid == p->pid && (pid <= 0 || LX(t)->proc->pid == pid)) return true;
    return false;
}
static bool put_user(lxproc_t *p, uint64_t dst, const void *src, uint64_t n) {
    if (!uvm_check(p->vm, dst, n, true)) return false;
    memcpy((void *)(uintptr_t)dst, src, n);
    return true;
}
int64_t lx_wait4(lxproc_t *p, int pid, uint64_t ustatus, int options, uint64_t urusage) {
    bool nohang = (options & 1) != 0;                            /* WNOHANG */
    for (;;) {
        lxzombie_t **pp = &zombies, *z = NULL;
        while (*pp) {
            if ((*pp)->ppid == p->pid && (pid <= 0 || (*pp)->pid == pid)) { z = *pp; break; }
            pp = &(*pp)->next;
        }
        if (z) {
            *pp = z->next;
            int status = z->status, reaped = z->pid;
            kfree(z);
            if (ustatus && !put_user(p, ustatus, &status, 4)) return -EFAULT;
            if (urusage) { uint8_t ru[144]; memset(ru, 0, sizeof ru); if (!put_user(p, urusage, ru, sizeof ru)) return -EFAULT; }
            return reaped;
        }
        if (!child_exists(p, pid)) return -ECHILD;
        if (nohang) return 0;
        if (lx_block_interrupted(lx_current())) return -EINTR;
        task_sleep(1);
    }
}

static void proc_free(lxproc_t *p) {
    int status = p->term_sig ? (p->term_sig & 0x7F) : ((p->exit_code & 0xFF) << 8);
    /* children that asked for a signal when their parent dies (PR_SET_PDEATHSIG) */
    for (task_t *o = task_list(); o;) {
        task_t *nx = o->next;
        if (o->lx && LX(o)->proc != p && LX(o)->proc->ppid == p->pid) {
            lxproc_t *c = LX(o)->proc;
            c->ppid = 1;
            if (c->pdeathsig) { int sig = (int)c->pdeathsig; c->pdeathsig = 0; lx_signal_kill(c, c->pid, sig); }
        }
        o = nx;
    }
    for (int i = 0; i < LX_MAXFD; i++) if (p->fds[i]) { lx_file_put(p->fds[i]); p->fds[i] = NULL; }
    record_zombie(p);
    lx_signal_child_exit(p, p->pid, status);
    uvm_free(p->vm);
    klog("linux: pid %d ended (%d)", p->pid, p->exit_code);
    kfree(p);
}

/* scheduler hooks (task.c) */
void lx_task_switched(task_t *t) { extern void arch_set_user_fs(uint64_t); arch_set_user_fs(LX(t)->fsbase); }
void lx_task_reaped(task_t *t) {
    lxthread_t *th = LX(t);
    lxproc_t *p = th->proc;
    bool killed = !th->exited;
    t->lx = NULL;
    kfree(th);
    if (killed && !p->exiting) {                                       /* killed from outside (Ctrl+C): the whole process goes */
        p->exiting = true; p->exit_code = 130;
        for (task_t *o = task_list(); o;) { task_t *nx = o->next; if (o->lx && LX(o)->proc == p) task_kill(o->pid); o = nx; }
    }
    if (--p->threads == 0) proc_free(p);
}

void lx_thread_exit(int code) {
    task_t *t = task_current();
    lxthread_t *th = LX(t);
    if (th->clear_child_tid && uvm_check(th->proc->vm, th->clear_child_tid, 4, true)) {
        *(volatile uint32_t *)(uintptr_t)th->clear_child_tid = 0;
        extern void lx_futex_wake(lxproc_t *p, uint64_t addr, int n);
        lx_futex_wake(th->proc, th->clear_child_tid, 1);
    }
    th->exited = true;
    /* run on the kernel's own address space from here: the process may be freed */
    __asm__ volatile("mov %0, %%cr3" :: "r"(paging_kernel_root()) : "memory");
    t->cr3 = paging_kernel_root();
    task_exit(code);
}
void lx_exit_group(lxproc_t *p, int code) {
    p->exiting = true; p->exit_code = code;
    task_t *self = task_current();
    for (task_t *t = task_list(); t;) {
        task_t *nx = t->next;
        if (t != self && t->lx && LX(t)->proc == p) task_kill(t->pid);
        t = nx;
    }
    if (self->lx && LX(self)->proc == p) lx_thread_exit(code);
}

/* before every return to ring 3 */
void lx_return_check(regs_t *r) {
    task_t *t = task_current();
    if (!t || !t->lx) return;
    lxthread_t *th = LX(t);
    if (th->proc->exiting) lx_thread_exit(th->proc->exit_code);
    if (th->sig_frame) th->sig_frame = false;         /* a fault already built the frame */
    else lx_signal_deliver(r);
    th->sys_eintr = false;                            /* one restart opportunity per call */
    if (th->proc->exiting) lx_thread_exit(th->proc->exit_code);
    r->eflags = (r->eflags & 0x0CD5) | 0x202;              /* user flags only, interrupts on */
    r->cs = 0x2B; r->ss = 0x23;
    uint32_t now = timer_ticks();
    if (now - th->slice_start >= 10) { task_yield(); th->slice_start = timer_ticks(); }
}

bool lx_strace;                          /* "lxtrace on" in the Command Prompt: every system call to the log */
void lx_syscall_c(regs_t *r) {
    lxthread_t *th = lx_current();
    uint64_t nr = r->eax;
    uint64_t a0 = r->edi, a1 = r->esi, a2 = r->edx;
    int64_t v = lx_syscall(r);
    if (lx_strace && th && nr != 24 && nr != 228)          /* not sched_yield / clock_gettime: too many */
        klog("lx %d: %llu(%llx, %llx, %llx) = %lld", th->proc ? th->proc->pid : -1, (unsigned long long)nr,
             (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (long long)v);
    if (th) { th->sys_nr = (uint32_t)nr; th->sys_eintr = nr != 15 && v == -EINTR; }
    r->eax = (uint64_t)v;
}

/* exceptions: demand paging, a signal handler, or the end of a program that
 * crashed */
bool lx_exception(regs_t *r, uintptr_t cr2) {
    task_t *t = task_current();
    if (!t || !t->lx) return false;
    lxthread_t *th = LX(t);
    lxproc_t *p = th->proc;
    if (r->int_no == 14) {
        bool user_addr = cr2 >= USER_LO && cr2 < USER_HI;
        if (user_addr && !(r->err_code & 1) && uvm_fault(p->vm, cr2, r->err_code & 2)) return true;
    }
    if (!(r->cs & 3)) return false;                         /* a kernel bug: let the normal handler report it */
    static const struct { int vec, sig; const char *name; } map[] = {
        { 0, 8, "Floating point exception" }, { 6, 4, "Illegal instruction" }, { 13, 11, "Segmentation fault" },
        { 14, 11, "Segmentation fault" }, { 16, 8, "Floating point exception" }, { 19, 8, "Floating point exception" },
        { 17, 7, "Bus error" }, { 3, 5, "Trace/breakpoint trap" } };
    int sig = 11; const char *what = "Segmentation fault";
    for (unsigned i = 0; i < ARRAY_LEN(map); i++) if (map[i].vec == (int)r->int_no) { sig = map[i].sig; what = map[i].name; }
    int code = 1; uint64_t addr = cr2;
    if (r->int_no == 14) code = (r->err_code & 1) ? 2 : 1;  /* SEGV_ACCERR / SEGV_MAPERR */
    else if (sig == 4 || sig == 8) addr = r->eip;
    if (lx_signal_fault(r, sig, code, addr)) return true;   /* the program installed a handler */
    klog("linux: pid %d: %s at %llx (address %llx, error %llx)", p->pid, what, (unsigned long long)r->eip, (unsigned long long)cr2, (unsigned long long)r->err_code);
    {   /* where: the file mapped there and the offset in it, and the instruction's bytes */
        vma_t *v = uvm_find(p->vm, r->eip);
        if (v && v->fnode) klog("linux: pid %d:   in %s + %llx", p->pid, v->fnode->name, (unsigned long long)(r->eip - v->start + v->foff));
        uint8_t code[12];
        if (uvm_check(p->vm, r->eip, sizeof code, false)) {
            memcpy(code, (void *)(uintptr_t)r->eip, sizeof code);
            klog("linux: pid %d:   code %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", p->pid,
                 code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7], code[8], code[9], code[10], code[11]);
        }
    }
    if (p->con) { char m[128]; snprintf(m, sizeof m, "%s\n", what); p->con->write(p->con, m, strlen(m)); }
    p->term_sig = sig;
    lx_exit_group(p, 128 + sig);
    return true;
}
#endif

#ifndef __x86_64__
#include "kernel.h"
#include "apps.h"
int lx_spawn(const char *wrld_path, int argc, char **argv, char **envp, console_t *in, console_t *con, char *err, int ecap) {
    (void)wrld_path; (void)argc; (void)argv; (void)envp; (void)con; (void)in;
    snprintf(err, ecap, "Linux programs need the 64-bit edition of THE WRLD OS.");
    return -1;
}
bool lx_is_elf(const uint8_t *d, uint32_t n) { return n >= 4 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F'; }
#endif
