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

int lx_spawn(const char *wrld_path, int argc, char **argv, char **envp, console_t *in_con, console_t *con, char *err, int ecap) {
    vnode_t *n = vfs_resolve(wrld_path, NULL);
    if (!n || n->dir) { snprintf(err, ecap, "File not found: %s", wrld_path); return -1; }
    lxproc_t *p = proc_new();
    if (!p) { snprintf(err, ecap, "Not enough memory to start the program."); return -1; }
    p->con = con;
    loaded_t exe, in;
    if (!load_elf(p->vm, n, EXE_BASE, &exe, err, ecap)) goto fail;
    uint64_t entry = exe.entry, interp_base = 0;
    if (exe.interp[0]) {
        char ip[512];
        lx_path(p, -100, exe.interp, ip, sizeof ip);
        vnode_t *in_n = vfs_resolve(ip, NULL);
        if (!in_n || in_n->dir) { snprintf(err, ecap, "%s needs %s (the Linux dynamic loader), which is not installed.", n->name, exe.interp); goto fail; }
        if (!load_elf(p->vm, in_n, INTERP_BASE, &in, err, ecap)) goto fail;
        entry = in.entry; interp_base = in.base;
    }
    /* program break just past the executable */
    p->vm->brk_start = p->vm->brk = PALIGN(exe.end) + 16 * PAGE;
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
    if (uvm_map(p->vm, STACK_TOP - STACK_SIZE, STACK_SIZE, VM_READ | VM_WRITE, VMF_STACK, true, NULL, 0, 0, NULL) < 0) { snprintf(err, ecap, "Could not map the stack."); goto fail; }
    {
        static const char *defenv[] = { "PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/home/user", "USER=user", "LOGNAME=user",
                                        "SHELL=/bin/sh", "TERM=dumb", "LANG=C.UTF-8", "PWD=/", NULL };
        char **env = envp ? envp : (char **)defenv;
        int envc = 0; while (env[envc]) envc++;
        uint64_t sp = STACK_TOP;
        uint64_t *argp = kmalloc(sizeof(uint64_t) * (argc + envc + 2));
        /* strings */
        #define PUSH_STR(s) ({ uint64_t l_ = strlen(s) + 1; sp -= l_; poke(p->vm, sp, (s), l_); sp; })
        uint64_t execfn = PUSH_STR(p->exe);
        for (int i = 0; i < envc; i++) argp[argc + i] = PUSH_STR(env[i]);
        for (int i = argc - 1; i >= 0; i--) argp[i] = PUSH_STR(argv[i]);
        uint64_t platform = PUSH_STR("x86_64");
        uint8_t rnd[16]; for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(rdtsc() >> (i % 8 * 3)) ^ (uint8_t)(i * 73);
        sp -= 16; poke(p->vm, sp, rnd, 16); uint64_t random = sp;
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
        blk[w++] = argc;
        for (int i = 0; i < argc; i++) blk[w++] = argp[i];
        blk[w++] = 0;
        for (int i = 0; i < envc; i++) blk[w++] = argp[argc + i];
        blk[w++] = 0;
        for (unsigned i = 0; i < ARRAY_LEN(aux); i++) blk[w++] = aux[i];
        bool ok = poke(p->vm, sp, blk, words * 8);
        kfree(blk); kfree(argp);
        if (!ok) { snprintf(err, ecap, "Not enough memory for the program's stack."); goto fail; }
        regs_t r; memset(&r, 0, sizeof r);
        r.eip = entry; r.esp = sp; r.cs = 0x2B; r.ss = 0x23; r.eflags = 0x202;
        /* stdin/stdout/stderr: the console */
        lxfile_t *cf = kzalloc(sizeof *cf);
        cf->type = FD_CONSOLE; cf->con = con; cf->oflags = 2; strcpy(cf->path, "/dev/tty");
        for (int i = 1; i < 3; i++) { p->fds[i] = cf; cf->refs++; }
        lxfile_t *ci = cf;
        if (in_con && in_con != con) { ci = kzalloc(sizeof *ci); ci->type = FD_CONSOLE; ci->con = in_con; strcpy(ci->path, "/dev/tty"); }
        p->fds[0] = ci; ci->refs++;
        const char *base = path_basename(wrld_path);
        task_t *t = thread_new(p, base, &r, 0);
        p->pid = t->pid;
        klog("linux: started %s as pid %d (entry %llx%s)", p->exe, p->pid, (unsigned long long)entry, exe.interp[0] ? ", dynamic" : "");
        return p->pid;
    }
fail:
    uvm_free(p->vm); kfree(p);
    return -1;
}

static void proc_free(lxproc_t *p) {
    for (int i = 0; i < LX_MAXFD; i++) if (p->fds[i]) { lx_file_put(p->fds[i]); p->fds[i] = NULL; }
    uvm_free(p->vm);
    klog("linux: pid %d ended (%d)", p->pid, p->exit_code);
    kfree(p);
}

/* scheduler hooks (task.c) */
void lx_task_switched(task_t *t) { extern void arch_set_user_fs(uint64_t); arch_set_user_fs(LX(t)->fsbase); }
void lx_task_reaped(task_t *t) {
    lxthread_t *th = LX(t);
    lxproc_t *p = th->proc;
    t->lx = NULL;
    kfree(th);
    if (!p->exiting) {                                       /* killed from outside (Ctrl+C): the whole process goes */
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
    r->eflags = (r->eflags & 0x0CD5) | 0x202;              /* user flags only, interrupts on */
    r->cs = 0x2B; r->ss = 0x23;
    uint32_t now = timer_ticks();
    if (now - th->slice_start >= 10) { task_yield(); th->slice_start = timer_ticks(); }
}

void lx_syscall_c(regs_t *r) {
    r->eax = (uint64_t)lx_syscall(r);
}

/* exceptions: demand paging, or the end of a program that crashed */
bool lx_exception(regs_t *r, uintptr_t cr2) {
    task_t *t = task_current();
    if (!t || !t->lx) return false;
    lxproc_t *p = LX(t)->proc;
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
    klog("linux: pid %d: %s at %llx (address %llx, error %llx)", p->pid, what, (unsigned long long)r->eip, (unsigned long long)cr2, (unsigned long long)r->err_code);
    if (p->con) { char m[128]; snprintf(m, sizeof m, "%s\n", what); p->con->write(p->con, m, strlen(m)); }
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
