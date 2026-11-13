/* Linux compatibility layer (64-bit edition): unmodified x86-64 Linux programs
 * run in ring 3 and call the kernel through the `syscall` instruction with
 * Linux's numbers and structures.
 *
 * Address space: the kernel keeps its identity map of the low 4 GiB in PML4
 * slot 0 (shared by every process, supervisor-only). Programs live above
 * 512 GiB (slots 1..255), so position-independent executables, ld-linux and
 * shared libraries load where Linux would put them. Pages are allocated on
 * first touch.
 *
 * Scheduling: each Linux thread is a kernel task. Kernel code stays
 * cooperative; a thread running in ring 3 is preempted by the timer. */
#ifndef LX_H
#define LX_H
#include "kernel.h"
#include "fs.h"
#include "apps.h"

#define USER_LO      0x0000008000000000ull     /* 512 GiB: PML4 slot 1 */
#define USER_HI      0x0000800000000000ull     /* end of the lower canonical half */
#define MMAP_TOP     0x00007F0000000000ull     /* mmap() grows down from here */
#define EXE_BASE     0x0000555555554000ull     /* where a PIE executable goes */
#define INTERP_BASE  0x00007FFFF7000000ull     /* ld-linux */
#define STACK_TOP    0x00007FFFFFFFF000ull
#define STACK_SIZE   (8ull << 20)
#define PAGE         4096ull
#define PALIGN(x)    (((x) + PAGE - 1) & ~(PAGE - 1))

/* ---- physical pages ---- */
void *pmm_alloc(void);                         /* one zeroed 4 KiB page (identity-mapped), NULL if none */
void pmm_free(void *p);
uint32_t pmm_used_pages(void);
uint32_t pmm_total_pages(void);
uint32_t pmm_free_pages(void);

/* ---- user address spaces ---- */
enum { VM_READ = 1, VM_WRITE = 2, VM_EXEC = 4 };
enum { VMF_PRIVATE = 0, VMF_SHARED = 1, VMF_STACK = 2, VMF_HEAP = 4 };
typedef struct vma {
    uint64_t start, end;                       /* page aligned, end exclusive */
    int prot, flags;
    const uint8_t *fdata; uint64_t fsize;       /* file contents behind the mapping (copied on first touch) */
    uint64_t foff;                              /* file offset of `start` */
    vnode_t *fnode;
    struct vma *next;
} vma_t;
typedef struct uvm {
    uint64_t *pml4;
    vma_t *vmas;                               /* sorted by address */
    uint64_t brk_start, brk;
    uint32_t pages;                            /* resident pages */
} uvm_t;
uvm_t *uvm_new(void);
uvm_t *uvm_clone(uvm_t *src);                  /* copy every mapping and resident page */
bool uvm_poke32(uvm_t *vm, uint64_t va, uint32_t v);   /* write into another process's memory */
void uvm_free(uvm_t *vm);
uintptr_t uvm_cr3(uvm_t *vm);
vma_t *uvm_find(uvm_t *vm, uint64_t addr);
uint64_t *uvm_pte(uvm_t *vm, uint64_t va, bool create);
/* map [start, start+len): fixed placement replaces what was there */
int64_t uvm_map(uvm_t *vm, uint64_t hint, uint64_t len, int prot, int flags, bool fixed,
                const uint8_t *fdata, uint64_t fsize, uint64_t foff, vnode_t *fnode);
int uvm_unmap(uvm_t *vm, uint64_t start, uint64_t len);
int uvm_protect(uvm_t *vm, uint64_t start, uint64_t len, int prot);
bool uvm_fault(uvm_t *vm, uint64_t addr, bool write);   /* demand paging; false = segmentation fault */
bool uvm_check(uvm_t *vm, uint64_t addr, uint64_t len, bool write);   /* covered by mappings that allow it */
uint64_t uvm_find_free(uvm_t *vm, uint64_t len, uint64_t below);

/* ---- files ---- */
enum { FD_FILE = 1, FD_DIR, FD_CONSOLE, FD_NULL, FD_ZERO, FD_RANDOM,
       FD_PIPE, FD_SOCKET, FD_EPOLL, FD_EVENTFD, FD_TIMERFD };
typedef struct lxfile {
    int type, refs;
    int oflags;
    vnode_t *node;
    uint64_t pos;
    console_t *con;
    void *obj;                                 /* src/linux/ipc.c state for pipes, sockets, ... */
    char path[256];                            /* Linux path it was opened as */
    char line[512]; int line_len, line_pos;    /* console input not yet read */
    char *pdata; uint32_t plen;                /* generated /proc contents (node is NULL) */
} lxfile_t;
#define LX_MAXFD 256

/* ---- signals ---- */
#define LX_NSIG 64
/* Linux's kernel sigaction as x86-64 passes it through rt_sigaction:
 * handler, flags, restorer, and the mask applied while the handler runs. */
typedef struct { uint64_t handler, flags, restorer, mask; } lxsigaction_t;

/* ---- processes and threads ---- */
typedef struct lxproc {
    int pid;                                   /* = pid of the main thread's task */
    int ppid;                                  /* pid of the parent process, 0 for the shell's children */
    uvm_t *vm;
    lxfile_t *fds[LX_MAXFD];
    int fdflags[LX_MAXFD];                     /* FD_CLOEXEC */
    char cwd[256];                             /* Linux path */
    char exe[256];
    console_t *con;
    int threads;
    int exit_code;
    int term_sig;                              /* signal that killed the process, 0 for a normal exit */
    bool exiting;
    uint32_t umask;
    uint64_t rlim[16][2];                      /* rlimit soft/hard, RLIM_INFINITY by default */
    bool no_new_privs;                         /* PR_SET_NO_NEW_PRIVS */
    int dumpable;                              /* PR_SET_DUMPABLE, 1 at start */
    uint32_t pdeathsig;                        /* PR_SET_PDEATHSIG */
    lxsigaction_t acts[LX_NSIG];                /* per-process dispositions, like Linux */
    uint64_t sig_pending;                      /* process-directed signals */
    uint32_t alarm_at; int alarm_sig;          /* alarm() and ITIMER_REAL */
    uint32_t alarm_iv;                         /* ITIMER_REAL interval */
    int sig_child_pid, sig_child_status;       /* what the next SIGCHLD reports */
} lxproc_t;
typedef struct lxthread {
    lxproc_t *proc;
    int tid;
    char comm[16];                             /* PR_SET_NAME */
    uint64_t fsbase;
    uint64_t clear_child_tid;                  /* set_tid_address / CLONE_CHILD_CLEARTID */
    uint64_t robust_list;
    uint64_t sigmask;
    uint64_t sig_pending;                      /* thread-directed signals (tkill/tgkill) */
    uint64_t alt_sp; uint32_t alt_size; int alt_flags;   /* sigaltstack */
    uint32_t sys_nr;                           /* number of the system call in progress */
    bool sys_eintr;                            /* ... and it returned -EINTR (SA_RESTART) */
    bool sig_frame;                            /* a frame was just pushed on this return */
    bool sig_suspend;                          /* sigsuspend: restore an older mask after the handler */
    uint64_t sig_suspend_old;
    uint32_t slice_start;                      /* for preemption */
    bool exited;                               /* left through exit/exit_group, not killed */
    regs_t start_regs;                         /* first entry into ring 3 */
} lxthread_t;

#define LX(t) ((lxthread_t *)(t)->lx)
lxthread_t *lx_current(void);

/* path translation: Linux path -> WRLD path (C:\Linux\..., C:\..., or a device) */
enum { LP_FILE = 0, LP_DEV_NULL, LP_DEV_ZERO, LP_DEV_RANDOM, LP_DEV_TTY, LP_PROC_SELF_EXE, LP_PROC_SELF_FD,
       LP_PROC_DIR, LP_PROC_SELF_DIR, LP_PROC_FD_DIR,
       LP_PROC_MAPS, LP_PROC_STAT, LP_PROC_STATUS, LP_PROC_CMDLINE,
       LP_PROC_MEMINFO, LP_PROC_CPUINFO, LP_PROC_UPTIME, LP_PROC_VERSION,
       LP_SYS, LP_SYS_DEVICES, LP_SYS_DEV_SYSTEM, LP_SYS_CPU, LP_SYS_KERNEL,
       LP_SYS_CPU_ONLINE, LP_SYS_CPU_POSSIBLE, LP_SYS_CPU_PRESENT,
       LP_SYS_OSRELEASE, LP_SYS_OSTYPE, LP_SYS_VERSION };
int lx_path(lxproc_t *p, int dirfd, const char *upath, char *out, int cap);
void lx_abspath(lxproc_t *p, const char *upath, char *out, int cap);

/* syscalls */
int64_t lx_syscall(regs_t *r);
int lx_fd_alloc(lxproc_t *p, lxfile_t *f, int min);
lxfile_t *lx_fd_get(lxproc_t *p, int fd);
void lx_file_put(lxfile_t *f);
int64_t lx_sys_file(lxthread_t *t, int nr, regs_t *r, bool *handled);

/* processes */
int lx_spawn(const char *wrld_path, int argc, char **argv, char **envp, console_t *in, console_t *out, char *err, int ecap);
int64_t lx_fork(regs_t *r);
int64_t lx_execve(regs_t *r, uint64_t upath, uint64_t uargv, uint64_t uenvp);
int64_t lx_wait4(lxproc_t *p, int pid, uint64_t ustatus, int options, uint64_t urusage);
void lx_sig_reset(lxproc_t *p);                /* execve drops every handler */
void lx_exit_group(lxproc_t *p, int code);
void lx_thread_exit(int code);
bool lx_is_elf(const uint8_t *d, uint32_t n);

/* signals (src/linux/signal.c) */
void lx_signal_deliver(regs_t *r);             /* before a return to ring 3 */
bool lx_signal_check(lxthread_t *th);          /* a signal that deserves EINTR is pending */
bool lx_signal_fault(regs_t *r, int sig, int code, uint64_t addr);   /* a ring-3 fault: handler? */
bool lx_block_interrupted(lxthread_t *th);     /* ... or the process is going away */
void lx_signal_raise(lxproc_t *p, int sig);    /* set a process-directed pending signal */
void lx_signal_raise_thread(lxthread_t *th, int sig);
void lx_signal_child_exit(lxproc_t *p, int pid, int status);   /* SIGCHLD to p */
lxproc_t *lx_proc_by_pid(int pid);
int64_t lx_signal_kill(lxproc_t *self, int pid, int sig);
int64_t lx_signal_tkill(lxproc_t *self, int tid, int sig);
int64_t lx_signal_tgkill(lxproc_t *self, int tgid, int tid, int sig);
int64_t lx_rt_sigreturn(regs_t *r);             /* restores the frame at r->esp - 8 */
int64_t lx_signal_sigsuspend(regs_t *r, uint64_t uset, uint64_t setsize);
int64_t lx_signal_sigtimedwait(lxproc_t *p, uint64_t uset, uint64_t uinfo, uint64_t utime, uint64_t setsize);
int64_t lx_signal_pending(lxthread_t *th, uint64_t uset, uint64_t setsize);
void lx_signal_alarm_check(lxproc_t *p);

/* pipes, sockets, epoll, eventfd and timerfd (src/linux/ipc.c) */
int64_t lx_sys_ipc(lxthread_t *t, int nr, regs_t *r, bool *handled);
int64_t lx_ipc_read(lxfile_t *f, void *dst, uint64_t n, bool nonblock);
int64_t lx_ipc_write(lxfile_t *f, const void *src, uint64_t n, bool nonblock);
void lx_ipc_release(lxfile_t *f);
bool lx_ipc_ready(lxfile_t *f, uint32_t want);
void lx_ipc_wake_all(void);                    /* a peer changed state: nothing to do but let sleeps expire */

/* glue in the arch code */
void enter_user(regs_t *r);                    /* never returns */
void lx_return_check(regs_t *r);               /* called before every return to ring 3 */

/* errno values */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define ENODATA 61
#define EOVERFLOW 75
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EALREADY 114
#define EINPROGRESS 115
#endif
