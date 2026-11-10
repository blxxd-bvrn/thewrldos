/* Linux system call dispatcher (x86-64 numbers): memory, time, identity,
 * thread-local storage, futexes, signals (bookkeeping only for now), exit.
 * File calls are in fs.c. Unknown calls return -ENOSYS and are logged once. */
#ifdef __x86_64__
#include "lx.h"

static uvm_t *VM(void) { return lx_current()->proc->vm; }
static bool uin(void *dst, uint64_t src, uint64_t n) { if (!uvm_check(VM(), src, n, false)) return false; memcpy(dst, (void *)(uintptr_t)src, n); return true; }
static bool uout(uint64_t dst, const void *src, uint64_t n) { if (!uvm_check(VM(), dst, n, true)) return false; memcpy((void *)(uintptr_t)dst, src, n); return true; }

/* signal handlers are remembered per process; execve drops them all */
static uint8_t sig_acts[64][32];
void lx_sig_reset(void) { memset(sig_acts, 0, sizeof sig_acts); }

/* ---------------- time ---------------- */
static int64_t boot_epoch = -1; static uint32_t boot_tick;
static int64_t epoch_ms(void) {
    if (boot_epoch < 0) {
        rtc_time_t t; rtc_read(&t);                                  /* the RTC keeps UTC */
        int y = t.year, m = t.month, d = t.day; y -= m <= 2;
        int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        boot_epoch = ((era * 146097 + (int64_t)doe - 719468) * 86400 + t.hour * 3600 + t.minute * 60 + t.second) * 1000;
        boot_tick = timer_ticks();
    }
    return boot_epoch + (uint32_t)(timer_ticks() - boot_tick);
}
/* nanoseconds since boot, finer than the 1 ms tick when the TSC is calibrated */
static uint64_t tsc_per_ms;
static uint64_t mono_ns(void) {
    static uint64_t base_tsc; static uint32_t base_ms;
    if (!tsc_per_ms) { uint64_t a = rdtsc(); uint32_t t0 = timer_ticks(); while (timer_ticks() - t0 < 5) {} tsc_per_ms = (rdtsc() - a) / MAX(1u, timer_ticks() - t0); base_tsc = rdtsc(); base_ms = timer_ticks(); }
    uint64_t ms = timer_ticks();
    uint64_t sub = tsc_per_ms ? ((rdtsc() - base_tsc) % tsc_per_ms) * 1000000ull / tsc_per_ms : 0;
    (void)base_ms;
    return ms * 1000000ull + (sub % 1000000ull);
}
static void ns_to_ts(uint64_t ns, int64_t ts[2]) { ts[0] = ns / 1000000000ull; ts[1] = ns % 1000000000ull; }
static int64_t sys_clock_gettime(uint64_t clk, uint64_t ut) {
    int64_t ts[2];
    switch (clk) {
    case 0: case 5: case 8: case 11: { int64_t ms = epoch_ms(); ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000; break; }   /* REALTIME, _COARSE, _ALARM, TAI */
    case 1: case 4: case 6: case 7: case 9: ns_to_ts(mono_ns(), ts); break;                                                /* MONOTONIC, RAW, COARSE, BOOTTIME */
    case 2: case 3: { task_t *t = task_current(); ns_to_ts((uint64_t)t->cpu_ms * 1000000ull, ts); break; }                  /* process / thread CPU time */
    default: return -EINVAL;
    }
    return uout(ut, ts, 16) ? 0 : -EFAULT;
}
static void sleep_ns(uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    if (!ms) { task_yield(); return; }
    task_sleep(ms > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)ms);
}

/* ---------------- futex (single address space for now) ---------------- */
typedef struct fwait { lxproc_t *p; uint64_t addr; task_t *t; bool woken; uint32_t bitset; struct fwait *next; } fwait_t;
static fwait_t *fwaiters;
void lx_futex_wake(lxproc_t *p, uint64_t addr, int n) {
    for (fwait_t *w = fwaiters; w && n > 0; w = w->next)
        if (w->p == p && w->addr == addr && !w->woken) { w->woken = true; n--; }
}
static int64_t sys_futex(lxproc_t *p, uint64_t uaddr, int op, uint32_t val, uint64_t utime, uint64_t uaddr2, uint32_t val3) {
    int cmd = op & 0x7F;
    if (!uvm_check(p->vm, uaddr, 4, false)) return -EFAULT;
    switch (cmd) {
    case 0: case 9: {                                                /* FUTEX_WAIT, FUTEX_WAIT_BITSET */
        if (*(volatile uint32_t *)(uintptr_t)uaddr != val) return -EAGAIN;
        uint32_t deadline = 0; bool timed = false;
        if (utime) {
            int64_t ts[2]; if (!uin(ts, utime, 16)) return -EFAULT;
            uint64_t ms = ts[0] * 1000 + ts[1] / 1000000;
            if (cmd == 9) {                                           /* absolute */
                bool real = op & 256;
                uint64_t now = real ? (uint64_t)epoch_ms() : mono_ns() / 1000000;
                ms = ms > now ? ms - now : 0;
            }
            deadline = timer_ticks() + (uint32_t)MIN(ms, 0x7FFFFFFFull); timed = true;
        }
        fwait_t *w = kmalloc(sizeof *w);
        *w = (fwait_t){ p, uaddr, task_current(), false, cmd == 9 ? val3 : 0xFFFFFFFF, fwaiters };
        fwaiters = w;
        int64_t r = 0;
        while (!w->woken) {
            if (p->exiting) break;
            if (timed && (int32_t)(timer_ticks() - deadline) >= 0) { r = -ETIMEDOUT; break; }
            task_sleep(1);
        }
        for (fwait_t **pp = &fwaiters; *pp; pp = &(*pp)->next) if (*pp == w) { *pp = w->next; break; }
        kfree(w);
        return r;
    }
    case 1: case 10: {                                               /* FUTEX_WAKE, FUTEX_WAKE_BITSET */
        int n = 0;
        for (fwait_t *w = fwaiters; w && n < (int)val; w = w->next)
            if (w->p == p && w->addr == uaddr && !w->woken && (cmd == 1 || (w->bitset & val3))) { w->woken = true; n++; }
        return n;
    }
    case 3: case 4: {                                                /* FUTEX_REQUEUE, FUTEX_CMP_REQUEUE */
        if (cmd == 4 && *(volatile uint32_t *)(uintptr_t)uaddr != val3) return -EAGAIN;
        int n = 0, moved = 0;
        for (fwait_t *w = fwaiters; w; w = w->next) {
            if (w->p != p || w->addr != uaddr || w->woken) continue;
            if (n < (int)val) { w->woken = true; n++; }
            else if (moved < (int)utime) { w->addr = uaddr2; moved++; }
        }
        return n;
    }
    }
    return -ENOSYS;
}

/* ---------------- memory ---------------- */
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_FIXED_NOREPLACE 0x100000
static int prot_of(uint64_t p) { return (p & 1 ? VM_READ : 0) | (p & 2 ? VM_WRITE | VM_READ : 0) | (p & 4 ? VM_EXEC | VM_READ : 0); }
static int64_t sys_mmap(lxproc_t *p, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
    if (!len || (off & (PAGE - 1))) return -EINVAL;
    if (len > (1ull << 46)) return -ENOMEM;
    const uint8_t *data = NULL; uint64_t size = 0; vnode_t *node = NULL;
    if (!(flags & MAP_ANON)) {
        lxfile_t *f = lx_fd_get(p, fd);
        if (!f) return -EBADF;
        if (f->type == FD_ZERO) { /* like anonymous */ }
        else if (f->type != FD_FILE) return -ENODEV;
        else { data = f->node->data; size = f->node->size; node = f->node; }
    }
    bool fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if ((flags & MAP_FIXED_NOREPLACE) && uvm_find(p->vm, addr)) return -EEXIST;
    int64_t r = uvm_map(p->vm, addr, len, prot_of(prot), (flags & MAP_SHARED) ? VMF_SHARED : VMF_PRIVATE, fixed, data, size, off, node);
    return r;
}
static int64_t sys_brk(lxproc_t *p, uint64_t want) {
    uvm_t *vm = p->vm;
    if (want < vm->brk_start) return vm->brk;
    uint64_t old_end = PALIGN(vm->brk), new_end = PALIGN(want);
    if (new_end > old_end) {
        for (vma_t *v = vm->vmas; v; v = v->next) if (v->start < new_end && v->end > old_end) return vm->brk;   /* would run into a mapping */
        if (uvm_map(vm, old_end, new_end - old_end, VM_READ | VM_WRITE, VMF_HEAP, true, NULL, 0, 0, NULL) < 0) return vm->brk;
    } else if (new_end < old_end) uvm_unmap(vm, new_end, old_end - new_end);
    vm->brk = want;
    return want;
}
static int64_t sys_mremap(lxproc_t *p, uint64_t old, uint64_t olen, uint64_t nlen, uint64_t flags, uint64_t naddr) {
    olen = PALIGN(olen); nlen = PALIGN(nlen);
    vma_t *v = uvm_find(p->vm, old);
    if (!v || (old & (PAGE - 1))) return -EFAULT;
    if (nlen <= olen) { if (nlen < olen) uvm_unmap(p->vm, old + nlen, olen - nlen); return old; }
    /* grow in place if the space after it is free */
    bool free_after = !(flags & 2);
    for (vma_t *w = p->vm->vmas; w && free_after; w = w->next) if (w->start < old + nlen && w->end > old + olen) free_after = false;
    if (free_after && v->end == old + olen && !v->fdata) { v->end = old + nlen; return old; }
    if (!(flags & 1)) return -ENOMEM;                               /* MREMAP_MAYMOVE */
    int64_t dst = uvm_map(p->vm, (flags & 2) ? naddr : 0, nlen, v->prot | VM_WRITE, v->flags, flags & 2, NULL, 0, 0, NULL);
    if (dst < 0) return dst;
    memcpy((void *)(uintptr_t)dst, (void *)(uintptr_t)old, olen);   /* both are mapped in the current address space */
    int prot = v->prot;
    uvm_unmap(p->vm, old, olen);
    if (!(prot & VM_WRITE)) uvm_protect(p->vm, dst, nlen, prot);
    return dst;
}

/* ---------------- misc ---------------- */
static int64_t sys_uname(uint64_t u) {
    char b[390]; memset(b, 0, sizeof b);
    strcpy(b + 0, "Linux"); strcpy(b + 65, "wrld"); strcpy(b + 130, "6.6.0-wrld");
    strcpy(b + 195, "#1 SMP THE WRLD OS"); strcpy(b + 260, "x86_64"); strcpy(b + 325, "(none)");
    return uout(u, b, sizeof b) ? 0 : -EFAULT;
}

int64_t lx_syscall(regs_t *r) {
    lxthread_t *t = lx_current();
    if (!t) return -ENOSYS;
    lxproc_t *p = t->proc;
    int nr = (int)r->eax;
    uint64_t a0 = r->edi, a1 = r->esi, a2 = r->edx, a3 = r->r10, a4 = r->r8, a5 = r->r9;
    bool handled;
    int64_t fr = lx_sys_file(t, nr, r, &handled);
    if (handled) return fr;
    int64_t ir = lx_sys_ipc(t, nr, r, &handled);
    if (handled) return ir;
    switch (nr) {
    case 9: return sys_mmap(p, a0, a1, a2, a3, (int64_t)a4, a5);
    case 10: return uvm_protect(p->vm, a0, a1, prot_of(a2));
    case 11: if (a0 & (PAGE - 1)) return -EINVAL; return uvm_unmap(p->vm, a0, a1);
    case 12: return sys_brk(p, a0);
    case 25: return sys_mremap(p, a0, a1, a2, a3, a4);
    case 28:                                                          /* madvise */
        if (a2 == 4 || a2 == 8) {                                     /* MADV_DONTNEED / MADV_FREE: zero-fill on next touch */
            for (uint64_t va = a0 & ~(PAGE - 1); va < a0 + a1; va += PAGE) {
                vma_t *v = uvm_find(p->vm, va); if (!v || v->fdata) continue;
                extern uint64_t *lx_pte_lookup(uvm_t *vm, uint64_t va);
                uint64_t *pte = lx_pte_lookup(p->vm, va);
                if (pte) { pmm_free((void *)(uintptr_t)(*pte & 0x000FFFFFFFFFF000ull)); *pte = 0; p->vm->pages--; __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }
            }
        }
        return 0;
    case 26: case 27: case 149: case 150: case 151: case 152: return 0;   /* msync, mincore, mlock... */
    case 13: {                                                        /* rt_sigaction: remembered, not delivered yet */
        if (a0 < 1 || a0 > 64) return -EINVAL;
        if (a2 && !uout(a2, sig_acts[a0 - 1], 32)) return -EFAULT;
        if (a1 && !uin(sig_acts[a0 - 1], a1, 32)) return -EFAULT;
        return 0;
    }
    case 14: {                                                        /* rt_sigprocmask */
        uint64_t old = t->sigmask, set;
        if (a1) { if (!uin(&set, a1, 8)) return -EFAULT; if (a0 == 0) t->sigmask |= set; else if (a0 == 1) t->sigmask &= ~set; else if (a0 == 2) t->sigmask = set; else return -EINVAL; }
        if (a2 && !uout(a2, &old, 8)) return -EFAULT;
        return 0;
    }
    case 131: return 0;                                               /* sigaltstack */
    case 127: case 128: case 130: return -EINTR;                      /* rt_sigpending / timedwait / sigsuspend */
    case 15: lx_thread_exit(0); return 0;                             /* rt_sigreturn: nothing was delivered */
    case 16: {                                                        /* ioctl */
        lxfile_t *f = lx_fd_get(p, a0); if (!f) return -EBADF;
        if (f->type != FD_CONSOLE) return -ENOTTY;
        if (a1 == 0x5401) {                                                                  /* TCGETS: struct termios (kernel layout, 36 bytes) */
            uint8_t tio[36]; memset(tio, 0, sizeof tio);
            uint32_t iflag = 0x500, oflag = 0x5, cflag = 0xBF, lflag = 0x8A3B;            /* ICRNL|IXON, OPOST|ONLCR, B38400|CS8|CREAD, ISIG|ICANON|ECHO... */
            memcpy(tio, &iflag, 4); memcpy(tio + 4, &oflag, 4); memcpy(tio + 8, &cflag, 4); memcpy(tio + 12, &lflag, 4);
            return uout(a2, tio, sizeof tio) ? 0 : -EFAULT;
        }
        if (a1 == 0x5402 || a1 == 0x5403 || a1 == 0x5404) return 0;                         /* TCSETS* */
        if (a1 == 0x5413) { uint16_t ws[4] = { 25, 80, 640, 400 }; return uout(a2, ws, 8) ? 0 : -EFAULT; }   /* TIOCGWINSZ */
        if (a1 == 0x540F) { int32_t pg = p->pid; return uout(a2, &pg, 4) ? 0 : -EFAULT; }   /* TIOCGPGRP */
        return -ENOTTY;
    }
    case 32: case 33: case 292: {                                     /* dup, dup2, dup3 */
        lxfile_t *f = lx_fd_get(p, a0); if (!f) return -EBADF;
        if (nr == 32) { int fd = lx_fd_alloc(p, f, 0); if (fd >= 0) f->refs++; return fd; }
        if (a1 >= LX_MAXFD) return -EBADF;
        if (a0 == a1) return nr == 292 ? -EINVAL : (int64_t)a1;
        if (p->fds[a1]) lx_file_put(p->fds[a1]);
        p->fds[a1] = f; f->refs++; p->fdflags[a1] = (nr == 292 && (a2 & 02000000)) ? 1 : 0;
        return a1;
    }
    case 72: {                                                        /* fcntl */
        lxfile_t *f = lx_fd_get(p, a0); if (!f) return -EBADF;
        switch (a1) {
        case 0: case 1030: { int fd = lx_fd_alloc(p, f, a2); if (fd >= 0) { f->refs++; p->fdflags[fd] = a1 == 1030; } return fd; }
        case 1: return p->fdflags[a0];
        case 2: p->fdflags[a0] = a2 & 1; return 0;
        case 3: return f->oflags;
        case 4: f->oflags = (f->oflags & ~(02000 | 04000)) | (a2 & (02000 | 04000)); return 0;
        case 5: case 6: case 7: return 0;                                                   /* locks: always granted */
        default: return -EINVAL;
        }
    }
    case 7: {                                                         /* poll */
        uint32_t deadline = timer_ticks() + ((int64_t)a2 > 0 ? (uint32_t)a2 : 0);
        for (;;) {
            int ready = 0;
            for (uint64_t i = 0; i < a1 && i < 1024; i++) {
                struct { int32_t fd; int16_t ev, rev; } pf;
                if (!uin(&pf, a0 + i * 8, 8)) return -EFAULT;
                lxfile_t *f = pf.fd < 0 ? NULL : lx_fd_get(p, pf.fd);
                int16_t rev = 0;
                if (pf.fd >= 0) {
                    if (!f) rev = 0x20;                               /* POLLNVAL */
                    else if (f->type >= FD_FILE && f->type <= FD_RANDOM)
                        rev = pf.ev & (1 | 4);                        /* files and devices never block */
                    else {
                        if (lx_ipc_ready(f, 0x001)) rev |= 0x001;     /* POLLIN */
                        if (lx_ipc_ready(f, 0x004)) rev |= 0x004;     /* POLLOUT */
                        if (lx_ipc_ready(f, 0x008)) rev |= 0x008;     /* POLLERR */
                        if (lx_ipc_ready(f, 0x010)) rev |= 0x010;     /* POLLHUP */
                        rev &= pf.ev | 0x018;
                    }
                }
                pf.rev = rev;
                if (rev) ready++;
                if (!uout(a0 + i * 8, &pf, 8)) return -EFAULT;
            }
            if (ready) return ready;
            if ((int64_t)a2 == 0) return 0;
            if ((int64_t)a2 > 0 && (int32_t)(timer_ticks() - deadline) >= 0) return 0;
            if (p->exiting) return -EINTR;
            task_sleep(1);
        }
    }
    case 24: task_yield(); return 0;                                  /* sched_yield */
    case 35: case 230: {                                              /* nanosleep, clock_nanosleep */
        int64_t ts[2]; if (!uin(ts, nr == 35 ? a0 : a2, 16)) return -EFAULT;
        uint64_t ns = ts[0] * 1000000000ull + ts[1];
        if (nr == 230 && (a1 & 1)) { uint64_t now = a0 == 0 ? (uint64_t)epoch_ms() * 1000000ull : mono_ns(); ns = ns > now ? ns - now : 0; }   /* TIMER_ABSTIME */
        sleep_ns(ns);
        return 0;
    }
    case 228: return sys_clock_gettime(a0, a1);
    case 229: { int64_t ts[2] = { 0, 1000000 }; return a1 ? (uout(a1, ts, 16) ? 0 : -EFAULT) : 0; }
    case 96: { int64_t ms = epoch_ms(); int64_t tv[2] = { ms / 1000, (ms % 1000) * 1000 }; if (a0 && !uout(a0, tv, 16)) return -EFAULT; return 0; }
    case 201: { int64_t s = epoch_ms() / 1000; if (a0) uout(a0, &s, 8); return s; }               /* time */
    case 39: return p->pid;
    case 186: return t->tid;
    case 110: return p->ppid;                                         /* getppid */
    case 102: case 104: case 107: case 108: return 1000;              /* getuid, getgid, geteuid, getegid */
    case 118: case 120: { uint32_t id = 1000; uint64_t outs[3] = { a0, a1, a2 }; for (int i = 0; i < 3; i++) if (!uout(outs[i], &id, 4)) return -EFAULT; return 0; }   /* getres[ug]id */
    case 105: case 106: case 113: case 114: case 117: case 119: return 0;                         /* set*id */
    case 111: case 121: case 124: return p->pid;                      /* getpgrp, getpgid, getsid */
    case 109: case 112: return 0;                                     /* setpgid, setsid */
    case 115: return 0;                                               /* getgroups: none */
    case 63: return sys_uname(a0);
    case 95: { uint32_t o = p->umask; p->umask = a0 & 0777; return o; }
    case 97: case 302: {                                              /* getrlimit, prlimit64 */
        uint64_t res = nr == 97 ? a0 : a1, out = nr == 97 ? a1 : a3;
        uint64_t lim[2] = { ~0ull, ~0ull };
        if (res == 3) { lim[0] = STACK_SIZE; }                        /* RLIMIT_STACK */
        else if (res == 7) { lim[0] = lim[1] = LX_MAXFD; }            /* RLIMIT_NOFILE */
        if (out && !uout(out, lim, 16)) return -EFAULT;
        return 0;
    }
    case 160: return 0;                                               /* setrlimit */
    case 99: {                                                        /* sysinfo */
        uint8_t si[112]; memset(si, 0, sizeof si);
        int64_t up = timer_ticks() / 1000; memcpy(si, &up, 8);
        uint64_t total = 256ull << 20, freem = 128ull << 20; memcpy(si + 32, &total, 8); memcpy(si + 40, &freem, 8);
        uint16_t procs = task_count(); memcpy(si + 80, &procs, 2);
        uint32_t unit = 1; memcpy(si + 104, &unit, 4);
        return uout(a0, si, sizeof si) ? 0 : -EFAULT;
    }
    case 98: { uint8_t ru[144]; memset(ru, 0, sizeof ru); return uout(a1, ru, 144) ? 0 : -EFAULT; }   /* getrusage */
    case 100: { int64_t tm[4] = { task_current()->cpu_ms / 10, 0, 0, 0 }; if (a0) uout(a0, tm, 32); return timer_ticks() / 10; }   /* times */
    case 158: {                                                       /* arch_prctl */
        extern void arch_set_user_fs(uint64_t base);
        if (a0 == 0x1002) { if (a1 >= USER_HI) return -EPERM; t->fsbase = a1; arch_set_user_fs(a1); return 0; }   /* ARCH_SET_FS */
        if (a0 == 0x1003) return uout(a1, &t->fsbase, 8) ? 0 : -EFAULT;                                     /* ARCH_GET_FS */
        return -EINVAL;
    }
    case 218: t->clear_child_tid = a0; return t->tid;                 /* set_tid_address */
    case 273: t->robust_list = a0; return 0;                          /* set_robust_list */
    case 274: return -ENOSYS;                                         /* get_robust_list */
    case 202: return sys_futex(p, a0, (int)a1, (uint32_t)a2, a3, a4, (uint32_t)a5);
    case 203: return 0;                                               /* sched_setaffinity */
    case 204: { uint64_t m = 1; if (a1 < 8) return -EINVAL; return uout(a2, &m, 8) ? 8 : -EFAULT; }   /* sched_getaffinity: one CPU */
    case 143: case 144: case 145: case 146: case 147: case 148: return 0;   /* sched_* params */
    case 318: {                                                       /* getrandom */
        if (!uvm_check(p->vm, a0, a1, true)) return -EFAULT;
        static uint64_t s = 0xD1B54A32D192ED03ull;
        uint8_t *d = (uint8_t *)(uintptr_t)a0;
        for (uint64_t i = 0; i < a1; i++) { s ^= rdtsc() + i; s ^= s << 13; s ^= s >> 7; s ^= s << 17; d[i] = (uint8_t)(s >> 24); }
        return a1;
    }
    case 157: return 0;                                               /* prctl */
    case 62: case 200: case 234: {                                    /* kill, tkill, tgkill */
        int sig = nr == 234 ? (int)a2 : (int)a1;
        if (!sig) return 0;
        int target = nr == 234 ? (int)a0 : (int)a0;
        if (target == p->pid || target == 0 || target == -1 || (nr != 62 && task_find(target) && task_find(target)->lx && LX(task_find(target))->proc == p)) {
            static const char *names[32] = { 0, "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap", "Aborted", "Bus error",
                "Floating point exception", "Killed", "User defined signal 1", "Segmentation fault", "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated" };
            if (sig == 17 || sig == 28 || sig == 23 || sig == 18) return 0;          /* SIGCHLD, SIGWINCH, SIGURG, SIGCONT: ignored by default */
            if (p->con && sig < 16 && names[sig]) { char m[64]; snprintf(m, sizeof m, "%s\n", names[sig]); p->con->write(p->con, m, strlen(m)); }
            p->term_sig = sig;
            lx_exit_group(p, 128 + sig);
        }
        return -ESRCH;
    }
    case 56: { extern int64_t lx_clone(regs_t *r, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t); return lx_clone(r, a0, a1, a2, a3, a4); }   /* clone */
    case 57: case 58: return lx_fork(r);                              /* fork, vfork (vfork does not suspend the parent) */
    case 59: return lx_execve(r, a0, a1, a2);                         /* execve */
    case 61: return lx_wait4(p, (int)a0, a1, (int)a2, a3);            /* wait4 */
    case 60: lx_thread_exit((int)a0 & 0xFF); return 0;                /* exit */
    case 231: lx_exit_group(p, (int)a0 & 0xFF); return 0;             /* exit_group */
    case 334: case 435: case 425: case 426: case 427: return -ENOSYS; /* rseq, clone3, io_uring */
    }
    static uint8_t warned[512];
    if (nr >= 0 && nr < 512 && !warned[nr]) { warned[nr] = 1; klog("linux: pid %d: system call %d is not implemented", p->pid, nr); }
    return -ENOSYS;
}
#endif
