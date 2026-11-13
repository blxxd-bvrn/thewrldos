/* Linux signals (64-bit edition): dispositions, pending sets, the x86-64
 * rt_sigframe that runs a handler in ring 3, sigreturn, and the kill family.
 *
 * Delivery happens in lx_return_check, on the way back to ring 3, the same
 * place preemption is decided. A handler gets Linux's frame layout: the
 * restorer address, the ucontext the restorer hands to rt_sigreturn, and the
 * siginfo, exactly as glibc expects to find them. */
#ifdef __x86_64__
#include "lx.h"

enum {
    SIGHUP = 1, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE,
    SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, SIGSTKFLT,
    SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG, SIGXCPU,
    SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, SIGPWR, SIGSYS
};
#define SIG_DFL_ 0
#define SIG_IGN_ 1
#define SIG_UNBLOCKABLE (1ull << (SIGKILL - 1) | 1ull << (SIGSTOP - 1))

enum {
    SA_NOCLDSTOP = 0x00000001, SA_NOCLDWAIT = 0x00000002, SA_SIGINFO = 0x00000004,
    SA_RESTORER = 0x04000000, SA_ONSTACK = 0x08000000, SA_RESTART = 0x10000000,
    SA_NODEFER = 0x40000000, SA_RESETHAND = 0x80000000
};
enum { SS_ONSTACK = 1, SS_DISABLE = 2 };

/* Linux's sigcontext on x86-64 (arch/x86/include/uapi/asm/sigcontext.h).
 * glibc's gregset_t is this same array of 23 registers, in this order. */
typedef struct PACKED {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint64_t csgsfs, err, trapno, oldmask, cr2, fpstate;
    uint64_t reserved1[8];
} sigctx_t;
typedef struct PACKED { void *ss_sp; int32_t ss_flags; uint32_t pad; uint64_t ss_size; } kstack_t;
typedef struct PACKED {
    uint64_t uc_flags, uc_link;
    kstack_t uc_stack;
    sigctx_t uc_mcontext;
    uint64_t uc_sigmask;                       /* the kernel's sigset_t is one word */
} kucontext_t;
typedef struct PACKED {
    uint64_t pretcode;
    kucontext_t uc;
    uint8_t info[128];                         /* siginfo_t */
} rtsigframe_t;

_Static_assert(sizeof(sigctx_t) == 256, "sigcontext layout");
_Static_assert(sizeof(kucontext_t) == 304, "ucontext layout");
_Static_assert(sizeof(rtsigframe_t) == 440, "rt_sigframe layout");

#define SIGFRAME_ALLOC 512                     /* frame, plus room for a restorer stub */
#define SIGFRAME_STUB  448

/* ---------------- user memory ---------------- */
static bool uwritable(uvm_t *vm, uint64_t addr, uint64_t n) {
    if (addr < USER_LO || addr + n > USER_HI || addr + n < addr) return false;
    for (uint64_t a = addr & ~(PAGE - 1); a < addr + n; a += PAGE)
        if (!uvm_fault(vm, a, true)) return false;
    return uvm_check(vm, addr, n, true);
}
static bool uwrite(uvm_t *vm, uint64_t addr, const void *src, uint64_t n) {
    if (!uwritable(vm, addr, n)) return false;
    memcpy((void *)(uintptr_t)addr, src, n);
    return true;
}
static bool uread(uvm_t *vm, void *dst, uint64_t addr, uint64_t n) {
    if (addr < USER_LO || addr + n > USER_HI || addr + n < addr) return false;
    for (uint64_t a = addr & ~(PAGE - 1); a < addr + n; a += PAGE)
        if (!uvm_fault(vm, a, false)) return false;
    if (!uvm_check(vm, addr, n, false)) return false;
    memcpy(dst, (const void *)(uintptr_t)addr, n);
    return true;
}

/* ---------------- disposition helpers ---------------- */
/* 0: ignored by default, 1: terminates, 2: stops (no job control here, so
 * treated like an ignored signal). */
static int sig_default(int sig) {
    switch (sig) {
    case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH: return 0;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU: return 2;
    default: return sig >= 1 && sig <= LX_NSIG ? 1 : 0;
    }
}
static uint64_t sig_bit(int sig) { return 1ull << (sig - 1); }
static bool sig_ignored_by_default(int sig) { return sig_default(sig) != 1; }

lxproc_t *lx_proc_by_pid(int pid) {
    for (task_t *t = task_list(); t; t = t->next)
        if (t->lx && LX(t)->proc->pid == pid) return LX(t)->proc;
    return NULL;
}

void lx_sig_reset(lxproc_t *p) { if (p) memset(p->acts, 0, sizeof p->acts); }

void lx_signal_raise(lxproc_t *p, int sig) {
    if (p && sig >= 1 && sig <= LX_NSIG) p->sig_pending |= sig_bit(sig);
}
void lx_signal_raise_thread(lxthread_t *th, int sig) {
    if (th && sig >= 1 && sig <= LX_NSIG) th->sig_pending |= sig_bit(sig);
}

/* SIGCHLD for a parent that is still around; the pid and status go into the
 * siginfo its handler sees. */
void lx_signal_child_exit(lxproc_t *p, int pid, int status) {
    lxproc_t *par = p ? lx_proc_by_pid(p->ppid) : NULL;
    if (!par) return;
    par->sig_child_pid = pid;
    par->sig_child_status = status;
    lx_signal_raise(par, SIGCHLD);
}

void lx_signal_alarm_check(lxproc_t *p) {
    if (!p->alarm_at) return;
    if ((int32_t)(timer_ticks() - p->alarm_at) < 0) return;
    p->alarm_at = p->alarm_iv ? timer_ticks() + p->alarm_iv : 0;
    lx_signal_raise(p, p->alarm_sig ? p->alarm_sig : SIGALRM);
}

/* A signal that is not blocked, not ignored, and not SIG_DFL-ignored would
 * interrupt a blocking system call. Ignored signals are dropped here. */
bool lx_signal_check(lxthread_t *th) {
    if (!th) return false;
    lxproc_t *p = th->proc;
    if (p->exiting) return true;
    lx_signal_alarm_check(p);
    uint64_t all = th->sig_pending | p->sig_pending;
    if (!all) return false;
    for (int sig = 1; sig <= LX_NSIG; sig++) {
        uint64_t bit = sig_bit(sig);
        if (!(all & bit)) continue;
        if (!(sig == SIGKILL || sig == SIGSTOP) && (th->sigmask & bit)) continue;
        lxsigaction_t *a = &p->acts[sig - 1];
        if (a->handler == SIG_IGN_) { th->sig_pending &= ~bit; p->sig_pending &= ~bit; continue; }
        if (a->handler == SIG_DFL_ && sig_ignored_by_default(sig)) {
            th->sig_pending &= ~bit; p->sig_pending &= ~bit; continue;
        }
        return true;
    }
    return false;
}

bool lx_block_interrupted(lxthread_t *th) {
    if (!th) return false;
    if (th->proc->exiting) return true;
    return lx_signal_check(th);
}

/* ---------------- the frame ---------------- */
static void fill_info(uint8_t *si, int sig, int code, uint64_t addr, int sender, int status) {
    memset(si, 0, 128);
    int32_t head[3] = { sig, 0, code };
    memcpy(si, head, sizeof head);
    if (sig == SIGCHLD) {
        int32_t pid = sender, uid = 1000, st = status;
        memcpy(si + 16, &pid, 4); memcpy(si + 20, &uid, 4); memcpy(si + 24, &st, 4);
    } else if (code > 0 && addr) {
        memcpy(si + 16, &addr, 8);             /* si_addr */
    } else {
        int32_t pid = sender, uid = 1000;
        memcpy(si + 16, &pid, 4); memcpy(si + 20, &uid, 4);
    }
}

/* Build the frame and point regs_t at the handler. False if the stack cannot
 * hold it (the caller then takes the fatal path). */
static bool push_frame(lxthread_t *th, regs_t *r, int sig, lxsigaction_t *a,
                       int code, uint64_t addr, int sender, int status) {
    lxproc_t *p = th->proc;
    uint64_t sp = r->esp;
    bool on_alt = false;
    if ((a->flags & SA_ONSTACK) && th->alt_size && !(th->alt_flags & SS_DISABLE)) {
        uint64_t top = th->alt_sp + th->alt_size;
        if (sp < th->alt_sp || sp >= top) { sp = top; on_alt = true; }
    }
    if (sp < 128 + SIGFRAME_ALLOC + 64) return false;
    uint64_t top = ((sp - 128 - SIGFRAME_ALLOC) & ~15ull) - 8;

    bool restart = th->sys_eintr && (a->flags & SA_RESTART) && r->int_no == 0x100 &&
                   !(th->sys_nr == 34 || th->sys_nr == 128 || th->sys_nr == 130);
    uint64_t saved_rip = restart ? r->eip - 2 : r->eip;
    uint64_t saved_rax = restart ? (uint64_t)th->sys_nr : r->eax;

    uint8_t buf[SIGFRAME_ALLOC];
    memset(buf, 0, sizeof buf);
    rtsigframe_t *f = (rtsigframe_t *)buf;

    /* x86-64 has no in-kernel restorer: the C library installs one (SA_RESTORER,
     * glibc's __restore_rt), and a trampoline on the stack cannot work because
     * the stack is not executable. Without a restorer the signal is fatal,
     * which is what Linux does too. */
    if (!(a->flags & SA_RESTORER) || !a->restorer) return false;
    f->pretcode = a->restorer;
    f->uc.uc_flags = 0;
    f->uc.uc_link = 0;
    f->uc.uc_stack.ss_sp = on_alt ? (void *)(uintptr_t)th->alt_sp : NULL;
    f->uc.uc_stack.ss_flags = on_alt ? SS_ONSTACK : 0;
    f->uc.uc_stack.ss_size = on_alt ? th->alt_size : 0;

    sigctx_t *mc = &f->uc.uc_mcontext;
    mc->r8 = r->r8; mc->r9 = r->r9; mc->r10 = r->r10; mc->r11 = r->r11;
    mc->r12 = r->r12; mc->r13 = r->r13; mc->r14 = r->r14; mc->r15 = r->r15;
    mc->rdi = r->edi; mc->rsi = r->esi; mc->rbp = r->ebp; mc->rbx = r->ebx;
    mc->rdx = r->edx; mc->rax = saved_rax; mc->rcx = r->ecx;
    mc->rsp = r->esp; mc->rip = saved_rip; mc->eflags = r->eflags;
    mc->csgsfs = (r->cs & 0xFFFF) | ((r->ss & 0xFFFF) << 48);
    mc->err = r->err_code; mc->trapno = r->int_no;
    mc->cr2 = addr; mc->fpstate = 0; mc->oldmask = 0;

    uint64_t restore = th->sigmask;
    if (th->sig_suspend) { restore = th->sig_suspend_old; th->sig_suspend = false; }
    uint64_t active = th->sigmask | a->mask;
    if (!(a->flags & SA_NODEFER)) active |= sig_bit(sig);
    th->sigmask = active & ~SIG_UNBLOCKABLE;
    f->uc.uc_sigmask = restore & ~SIG_UNBLOCKABLE;

    fill_info(f->info, sig, code, addr, sender, status);
    if (!uwrite(p->vm, top, buf, sizeof buf)) return false;

    uint64_t handler = a->handler;
    if (a->flags & SA_RESETHAND) memset(a, 0, sizeof *a);
    r->eip = handler;
    r->esp = top;
    r->edi = (uint64_t)sig;
    r->esi = top + 8 + sizeof(kucontext_t);    /* &siginfo */
    r->edx = top + 8;                          /* &ucontext */
    return true;
}

/* ---------------- delivering, returning ---------------- */
void lx_signal_deliver(regs_t *r) {
    lxthread_t *th = lx_current();
    if (!th) return;
    lxproc_t *p = th->proc;
    lx_signal_alarm_check(p);
    for (int sig = 1; sig <= LX_NSIG; sig++) {
        uint64_t bit = sig_bit(sig);
        bool thread_dir = (th->sig_pending & bit) != 0;
        bool proc_dir = !thread_dir && (p->sig_pending & bit) != 0;
        if (!thread_dir && !proc_dir) continue;
        if (!(sig == SIGKILL || sig == SIGSTOP) && (th->sigmask & bit)) continue;
        lxsigaction_t *a = &p->acts[sig - 1];
        if (a->handler == SIG_IGN_) { if (thread_dir) th->sig_pending &= ~bit; else p->sig_pending &= ~bit; continue; }
        if (a->handler == SIG_DFL_) {
            int d = sig_default(sig);
            if (d != 1) { if (thread_dir) th->sig_pending &= ~bit; else p->sig_pending &= ~bit; continue; }
            if (thread_dir) th->sig_pending &= ~bit; else p->sig_pending &= ~bit;
            p->term_sig = sig;
            lx_exit_group(p, 128 + sig);
            return;
        }
        int sender = thread_dir ? th->tid : p->pid;
        int code = 0, status = 0;
        if (sig == SIGCHLD) { sender = p->sig_child_pid; status = p->sig_child_status; code = 1; }
        if (thread_dir) th->sig_pending &= ~bit; else p->sig_pending &= ~bit;
        if (!push_frame(th, r, sig, a, code, 0, sender, status)) {
            p->term_sig = SIGSEGV;
            lx_exit_group(p, 128 + SIGSEGV);
        }
        return;                                /* one frame per return; next signal later */
    }
}

/* A fault in ring 3: run the handler if the program installed one. */
bool lx_signal_fault(regs_t *r, int sig, int code, uint64_t addr) {
    lxthread_t *th = lx_current();
    if (!th || sig < 1 || sig > LX_NSIG) return false;
    lxproc_t *p = th->proc;
    lxsigaction_t *a = &p->acts[sig - 1];
    if (a->handler == SIG_DFL_ || a->handler == SIG_IGN_) return false;
    if (th->sigmask & sig_bit(sig)) return false;               /* blocked: force the default instead */
    if (!push_frame(th, r, sig, a, code, addr, p->pid, 0)) {
        klog("linux: pid %d: no room for the signal %d frame (stack %llx)", p->pid, sig, (unsigned long long)r->esp);
        p->term_sig = SIGSEGV;
        lx_exit_group(p, 128 + SIGSEGV);
    }
    th->sig_frame = true;                        /* lx_return_check must not build a second frame */
    return true;
}

int64_t lx_rt_sigreturn(regs_t *r) {
    lxthread_t *th = lx_current();
    if (!th) return -EINVAL;
    if (r->esp < 8) { klog("linux: pid %d: rt_sigreturn with no stack", th->proc->pid); th->proc->term_sig = SIGSEGV; lx_exit_group(th->proc, 128 + SIGSEGV); return 0; }
    uint64_t base = r->esp - 8;
    rtsigframe_t f;
    if (!uread(th->proc->vm, &f, base, sizeof f)) {
        klog("linux: pid %d: rt_sigreturn frame at %llx unreadable", th->proc->pid, (unsigned long long)base);
        th->proc->term_sig = SIGSEGV;
        lx_exit_group(th->proc, 128 + SIGSEGV);
        return 0;
    }
    sigctx_t *mc = &f.uc.uc_mcontext;
    if (mc->rip < USER_LO || mc->rip >= USER_HI) {
        klog("linux: pid %d: rt_sigreturn to %llx, outside the program", th->proc->pid, (unsigned long long)mc->rip);
        th->proc->term_sig = SIGSEGV;
        lx_exit_group(th->proc, 128 + SIGSEGV);
        return 0;
    }
    r->r8 = mc->r8; r->r9 = mc->r9; r->r10 = mc->r10; r->r11 = mc->r11;
    r->r12 = mc->r12; r->r13 = mc->r13; r->r14 = mc->r14; r->r15 = mc->r15;
    r->edi = mc->rdi; r->esi = mc->rsi; r->ebp = mc->rbp; r->ebx = mc->rbx;
    r->edx = mc->rdx; r->ecx = mc->rcx;
    r->eip = mc->rip; r->esp = mc->rsp; r->eflags = mc->eflags;
    r->cs = 0x2B; r->ss = 0x23;              /* the only user selectors we have */
    th->sigmask = f.uc.uc_sigmask & ~SIG_UNBLOCKABLE;
    th->sig_suspend = false;
    return (int64_t)mc->rax;
}

/* ---------------- kill, tkill, tgkill ---------------- */
int64_t lx_signal_kill(lxproc_t *self, int pid, int sig) {
    if (sig < 0 || sig > LX_NSIG) return -EINVAL;
    if (pid > 0) {
        lxproc_t *t = lx_proc_by_pid(pid);
        if (!t) return -ESRCH;
        if (sig) lx_signal_raise(t, sig);
        return 0;
    }
    int n = 0;
    for (task_t *t = task_list(); t; t = t->next) {
        if (!t->lx) continue;
        lxproc_t *q = LX(t)->proc;
        if (pid == -1 && q == self) continue;  /* -1 spares the caller */
        n++;
        if (sig) lx_signal_raise(q, sig);
    }
    return n ? 0 : -ESRCH;
}
int64_t lx_signal_tkill(lxproc_t *self, int tid, int sig) {
    (void)self;
    if (sig < 0 || sig > LX_NSIG) return -EINVAL;
    if (!tid) return -EINVAL;
    task_t *t = task_find(tid);
    if (!t || !t->lx) return -ESRCH;
    if (sig) lx_signal_raise_thread(LX(t), sig);
    return 0;
}
int64_t lx_signal_tgkill(lxproc_t *self, int tgid, int tid, int sig) {
    if (sig < 0 || sig > LX_NSIG) return -EINVAL;
    task_t *t = task_find(tid);
    if (!t || !t->lx) return -ESRCH;
    lxproc_t *q = LX(t)->proc;
    if (tgid > 0 && q->pid != tgid) return -ESRCH;
    (void)self;
    if (sig) lx_signal_raise_thread(LX(t), sig);
    return 0;
}

/* ---------------- sigpending, sigsuspend, sigtimedwait ---------------- */
int64_t lx_signal_pending(lxthread_t *th, uint64_t uset, uint64_t setsize) {
    if (!th) return -EINVAL;
    if (setsize && setsize != 8) return -EINVAL;
    uint64_t set = (th->sig_pending | th->proc->sig_pending) & th->sigmask;
    return uwrite(th->proc->vm, uset, &set, 8) ? 0 : -EFAULT;
}

int64_t lx_signal_sigsuspend(regs_t *r, uint64_t uset, uint64_t setsize) {
    (void)r;
    lxthread_t *th = lx_current();
    if (!th) return -EINTR;
    if (setsize && setsize != 8) return -EINVAL;
    uint64_t set;
    if (!uread(th->proc->vm, &set, uset, 8)) return -EFAULT;
    th->sig_suspend_old = th->sigmask;
    th->sig_suspend = true;
    th->sigmask = set & ~SIG_UNBLOCKABLE;
    for (;;) {
        if (th->proc->exiting) return -EINTR;
        if (lx_signal_check(th)) return -EINTR;    /* the handler runs on the way back */
        task_sleep(1);
    }
}

int64_t lx_signal_sigtimedwait(lxproc_t *p, uint64_t uset, uint64_t uinfo, uint64_t utime, uint64_t setsize) {
    lxthread_t *th = lx_current();
    if (!th) return -EINVAL;
    if (setsize && setsize != 8) return -EINVAL;
    uint64_t set;
    if (!uread(p->vm, &set, uset, 8)) return -EFAULT;
    bool timed = false;
    uint32_t deadline = 0;
    if (utime) {
        int64_t ts[2];
        if (!uread(p->vm, ts, utime, 16)) return -EFAULT;
        if (ts[0] < 0 || ts[1] < 0) return -EINVAL;
        deadline = timer_ticks() + (uint32_t)MIN(ts[0] * 1000 + ts[1] / 1000000, 0x7FFFFFFFll);
        timed = true;
    }
    for (;;) {
        for (int sig = 1; sig <= LX_NSIG; sig++) {
            uint64_t bit = sig_bit(sig);
            if (!(set & bit)) continue;
            bool in_thread = (th->sig_pending & bit) != 0;
            if (!in_thread && !(p->sig_pending & bit)) continue;
            if (in_thread) th->sig_pending &= ~bit; else p->sig_pending &= ~bit;
            if (uinfo) {
                uint8_t si[128];
                fill_info(si, sig, 0, 0, p->pid, 0);
                if (!uwrite(p->vm, uinfo, si, sizeof si)) return 0;   /* the signal is consumed either way */
            }
            return sig;
        }
        if (timed && (int32_t)(timer_ticks() - deadline) >= 0) return -EAGAIN;
        if (!timed) return -EAGAIN;
        if (p->exiting) return -EINTR;
        if (lx_signal_check(th)) return -EINTR;
        task_sleep(1);
    }
}
#endif
