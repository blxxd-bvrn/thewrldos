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
void uvm_free(uvm_t *vm);
uintptr_t uvm_cr3(uvm_t *vm);
vma_t *uvm_find(uvm_t *vm, uint64_t addr);
/* map [start, start+len): fixed placement replaces what was there */
int64_t uvm_map(uvm_t *vm, uint64_t hint, uint64_t len, int prot, int flags, bool fixed,
                const uint8_t *fdata, uint64_t fsize, uint64_t foff, vnode_t *fnode);
int uvm_unmap(uvm_t *vm, uint64_t start, uint64_t len);
int uvm_protect(uvm_t *vm, uint64_t start, uint64_t len, int prot);
bool uvm_fault(uvm_t *vm, uint64_t addr, bool write);   /* demand paging; false = segmentation fault */
bool uvm_check(uvm_t *vm, uint64_t addr, uint64_t len, bool write);   /* covered by mappings that allow it */
uint64_t uvm_find_free(uvm_t *vm, uint64_t len, uint64_t below);

/* ---- files ---- */
enum { FD_FILE = 1, FD_DIR, FD_CONSOLE, FD_NULL, FD_ZERO, FD_RANDOM };
typedef struct lxfile {
    int type, refs;
    int oflags;
    vnode_t *node;
    uint64_t pos;
    console_t *con;
    char path[256];                            /* Linux path it was opened as */
    char line[512]; int line_len, line_pos;    /* console input not yet read */
} lxfile_t;
#define LX_MAXFD 256

/* ---- processes and threads ---- */
typedef struct lxproc {
    int pid;                                   /* = pid of the main thread's task */
    uvm_t *vm;
    lxfile_t *fds[LX_MAXFD];
    int fdflags[LX_MAXFD];                     /* FD_CLOEXEC */
    char cwd[256];                             /* Linux path */
    char exe[256];
    console_t *con;
    int threads;
    int exit_code;
    bool exiting;
    uint32_t umask;
} lxproc_t;
typedef struct lxthread {
    lxproc_t *proc;
    int tid;
    uint64_t fsbase;
    uint64_t clear_child_tid;                  /* set_tid_address / CLONE_CHILD_CLEARTID */
    uint64_t robust_list;
    uint64_t sigmask;
    uint32_t slice_start;                      /* for preemption */
    regs_t start_regs;                         /* first entry into ring 3 */
} lxthread_t;

#define LX(t) ((lxthread_t *)(t)->lx)
lxthread_t *lx_current(void);

/* path translation: Linux path -> WRLD path (C:\Linux\..., C:\..., or a device) */
enum { LP_FILE = 0, LP_DEV_NULL, LP_DEV_ZERO, LP_DEV_RANDOM, LP_DEV_TTY, LP_PROC_SELF_EXE, LP_PROC_SELF_FD };
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
void lx_exit_group(lxproc_t *p, int code);
void lx_thread_exit(int code);
bool lx_is_elf(const uint8_t *d, uint32_t n);

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
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#endif
