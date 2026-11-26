//! The Linux compatibility layer of the 64-bit edition (was src/linux): unmodified x86-64
//! Linux programs run in ring 3 and call the kernel through `syscall` with Linux's numbers
//! and structures.
//!
//! Address space: the kernel keeps its identity map of the low 4 GiB in PML4 slot 0 (shared
//! by every process, supervisor-only). Programs live above 512 GiB (slots 1..255), so
//! position-independent executables, ld-linux and shared libraries load where Linux would
//! put them. Pages are allocated on first touch.
//!
//! Scheduling: each Linux thread is a kernel task. Kernel code stays cooperative; a thread
//! running in ring 3 is preempted by the timer.
//!
//! The structures here (processes, threads, open files, mappings, shared objects) belong to
//! this module alone: C only holds a task's `lx` pointer. They are allocated with the C
//! allocator and linked with raw pointers, as the C code did, because they are shared
//! between processes and threads (open files, shared memory) and freed by reference counts.
//! The pure parts (paths, "#!" lines, ELF headers) are in rust/src/lxpath.rs.

#![allow(clippy::missing_safety_doc, clippy::too_many_arguments, static_mut_refs)]

pub mod mem;
pub mod sig;
pub mod sys;
pub mod proc_;
pub mod fs;
pub mod ipc;
pub mod dsp;
pub mod win;
pub mod locks;

use core::ffi::{c_char, c_int, c_void};
use core::fmt;
use crate::CBool;
pub use crate::kernel::Vnode;

/* ---------------- the C kernel ---------------- */

/// regs_t (x86-64): what the interrupt and syscall entry code saves.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Regs {
    pub r15: u64, pub r14: u64, pub r13: u64, pub r12: u64, pub r11: u64, pub r10: u64, pub r9: u64, pub r8: u64,
    pub edi: u64, pub esi: u64, pub ebp: u64, pub ebx: u64, pub edx: u64, pub ecx: u64, pub eax: u64,
    pub int_no: u64, pub err_code: u64,
    pub eip: u64, pub cs: u64, pub eflags: u64, pub esp: u64, pub ss: u64,
}

/// task_t (src/include/kernel.h)
#[repr(C)]
pub struct Task {
    pub pid: c_int,
    pub name: [u8; 48],
    pub state: c_int,
    pub esp: usize,
    pub stack: *mut u8,
    pub stack_size: u32,
    pub cr3: usize,
    pub wake_at: u32,
    pub cpu_ms: u32,
    pub started: u32,
    pub mem_bytes: u32,
    pub entry: Option<unsafe extern "C" fn(*mut c_void)>,
    pub arg: *mut c_void,
    pub proc_: *mut c_void,
    pub console: *mut c_void,
    pub teb: usize,
    pub exit_code: c_int,
    pub kind: c_int,
    pub lx: *mut c_void,
    pub next: *mut Task,
}

/// console_t (src/include/apps.h)
#[repr(C)]
pub struct Console {
    pub write: Option<unsafe extern "C" fn(*mut Console, *const c_char, c_int)>,
    pub readline: Option<unsafe extern "C" fn(*mut Console, *mut c_char, c_int) -> c_int>,
    pub getkey: Option<unsafe extern "C" fn(*mut Console) -> c_int>,
    pub clear: Option<unsafe extern "C" fn(*mut Console)>,
    pub set_title: Option<unsafe extern "C" fn(*mut Console, *const c_char)>,
    pub set_color: Option<unsafe extern "C" fn(*mut Console, c_int, c_int)>,
    pub alive: Option<unsafe extern "C" fn(*mut Console) -> CBool>,
    pub imp: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RtcTime { pub year: c_int, pub month: c_int, pub day: c_int, pub hour: c_int, pub minute: c_int, pub second: c_int, pub weekday: c_int }

#[allow(clashing_extern_declarations)]
extern "C" {
    pub fn kmalloc(n: usize) -> *mut c_void;
    pub fn kzalloc(n: usize) -> *mut c_void;
    pub fn krealloc(p: *mut c_void, n: usize) -> *mut c_void;
    pub fn kfree(p: *mut c_void);
    pub fn kmalloc_aligned(n: usize, align: usize) -> *mut c_void;
    pub fn timer_ticks() -> u32;
    pub static idle_ticks: u32;
    pub fn rtc_read(t: *mut RtcTime);
    pub fn cpu_get_info(vendor: *mut c_char, brand: *mut c_char);
    pub fn paging_kernel_root() -> usize;
    pub fn task_create(name: *const c_char, entry: unsafe extern "C" fn(*mut c_void), arg: *mut c_void, stack: u32) -> *mut Task;
    pub fn task_yield();
    pub fn task_sleep(ms: u32);
    pub fn task_exit(code: c_int) -> !;
    pub fn task_kill(pid: c_int) -> CBool;
    pub fn task_current() -> *mut Task;
    pub fn task_find(pid: c_int) -> *mut Task;
    pub fn task_list() -> *mut Task;
    pub fn task_count() -> c_int;
    pub fn enter_user(r: *mut Regs) -> !;
    pub fn arch_set_user_fs(base: u64);
    pub static boot_pml4: [u64; 512];
    pub static rust_lx_layout: [u32; 16];
    fn klog(fmt: *const c_char, ...);
}

/// The layout C reports for the structures mirrored here (src/kernel/rustlayout.c).
pub fn layout_ok() -> bool {
    use core::mem::{offset_of, size_of};
    let want = unsafe { rust_lx_layout };
    want == [size_of::<Regs>() as u32, offset_of!(Regs, eip) as u32, size_of::<Task>() as u32, offset_of!(Task, cr3) as u32,
             offset_of!(Task, cpu_ms) as u32, offset_of!(Task, console) as u32, offset_of!(Task, kind) as u32, offset_of!(Task, lx) as u32,
             offset_of!(Task, next) as u32, size_of::<Console>() as u32, size_of::<RtcTime>() as u32,
             win::wlayout()[0], win::wlayout()[1], win::wlayout()[2], win::wlayout()[3], win::wlayout()[4]]
}

/* ---------------- the layer's own constants ---------------- */

pub const USER_LO: u64 = 0x0000_0080_0000_0000;      /* 512 GiB: PML4 slot 1 */
pub const USER_HI: u64 = 0x0000_8000_0000_0000;      /* end of the lower canonical half */
pub const MMAP_TOP: u64 = 0x0000_7F00_0000_0000;     /* mmap() grows down from here */
pub const EXE_BASE: u64 = 0x0000_5555_5555_4000;     /* where a PIE executable goes */
pub const INTERP_BASE: u64 = 0x0000_7FFF_F700_0000;  /* ld-linux */
pub const STACK_TOP: u64 = 0x0000_7FFF_FFFF_F000;
pub const STACK_SIZE: u64 = 8 << 20;
pub const PAGE: u64 = 4096;
#[inline] pub fn palign(x: u64) -> u64 { x.wrapping_add(PAGE - 1) & !(PAGE - 1) }

pub const VM_READ: c_int = 1;
pub const VM_WRITE: c_int = 2;
pub const VM_EXEC: c_int = 4;
pub const VMF_PRIVATE: c_int = 0;
pub const VMF_SHARED: c_int = 1;
pub const VMF_STACK: c_int = 2;
pub const VMF_HEAP: c_int = 4;

pub const FD_FILE: c_int = 1;
pub const FD_DIR: c_int = 2;
pub const FD_CONSOLE: c_int = 3;
pub const FD_NULL: c_int = 4;
pub const FD_ZERO: c_int = 5;
pub const FD_RANDOM: c_int = 6;
pub const FD_PIPE: c_int = 7;
pub const FD_SOCKET: c_int = 8;
pub const FD_EPOLL: c_int = 9;
pub const FD_EVENTFD: c_int = 10;
pub const FD_TIMERFD: c_int = 11;
pub const FD_SHM: c_int = 12;
pub const FD_WINDOW: c_int = 13;
pub const FD_DSP: c_int = 14;

pub const LX_MAXFD: usize = 256;
pub const LX_NSIG: c_int = 64;

pub const SHM_SEAL_SEAL: u32 = 1;
pub const SHM_SEAL_SHRINK: u32 = 2;
pub const SHM_SEAL_GROW: u32 = 4;
pub const SHM_SEAL_WRITE: u32 = 8;

/* errno */
pub const EPERM: i64 = 1; pub const ENOENT: i64 = 2; pub const ESRCH: i64 = 3; pub const EINTR: i64 = 4;
pub const EIO: i64 = 5; pub const E2BIG: i64 = 7; pub const ENOEXEC: i64 = 8; pub const EBADF: i64 = 9;
pub const ECHILD: i64 = 10; pub const EAGAIN: i64 = 11; pub const ENOMEM: i64 = 12; pub const EACCES: i64 = 13;
pub const EFAULT: i64 = 14; pub const EBUSY: i64 = 16; pub const EEXIST: i64 = 17; pub const EXDEV: i64 = 18;
pub const ENODEV: i64 = 19; pub const ENOTDIR: i64 = 20; pub const EISDIR: i64 = 21; pub const EINVAL: i64 = 22;
pub const EMFILE: i64 = 24; pub const ENOTTY: i64 = 25; pub const EFBIG: i64 = 27; pub const ENOSPC: i64 = 28;
pub const ESPIPE: i64 = 29; pub const EPIPE: i64 = 32; pub const ERANGE: i64 = 34; pub const ENAMETOOLONG: i64 = 36;
pub const ENOSYS: i64 = 38; pub const ENOTEMPTY: i64 = 39; pub const ENOTSOCK: i64 = 88; pub const EDESTADDRREQ: i64 = 89;
pub const EMSGSIZE: i64 = 90; pub const EPROTOTYPE: i64 = 91; pub const ENOPROTOOPT: i64 = 92; pub const EPROTONOSUPPORT: i64 = 93;
pub const EOPNOTSUPP: i64 = 95; pub const EAFNOSUPPORT: i64 = 97; pub const EADDRINUSE: i64 = 98; pub const EADDRNOTAVAIL: i64 = 99;
pub const ENETUNREACH: i64 = 101; pub const ECONNRESET: i64 = 104; pub const ENOBUFS: i64 = 105; pub const EISCONN: i64 = 106;
pub const ENOTCONN: i64 = 107; pub const ETIMEDOUT: i64 = 110; pub const ECONNREFUSED: i64 = 111; pub const EALREADY: i64 = 114;
pub const EINPROGRESS: i64 = 115;

/* ---------------- the structures ---------------- */

/// A shared memory object (memfd_create, /dev/shm, MAP_SHARED | MAP_ANONYMOUS): a
/// reference-counted array of physical pages.
#[repr(C)]
pub struct ShmObj {
    pub refs: c_int,
    pub size: u64,
    pub npages: u64,
    pub pages: *mut *mut u8,
    pub seals: u32,
    pub is_named: bool,
    pub name: [u8; 96],
    pub next: *mut ShmObj,
}

/// One mapping of a user address space.
#[repr(C)]
pub struct Vma {
    pub start: u64, pub end: u64,                   /* page aligned, end exclusive */
    pub prot: c_int, pub flags: c_int,
    pub fdata: *const u8, pub fsize: u64,           /* file contents behind the mapping (copied on first touch) */
    pub foff: u64,                                  /* file offset of `start` */
    pub fnode: *mut Vnode,
    pub shm: *mut ShmObj,                           /* a shared object behind the mapping; foff is the offset in it */
    pub next: *mut Vma,
}
#[repr(C)]
pub struct Uvm {
    pub pml4: *mut u64,
    pub vmas: *mut Vma,                             /* sorted by address */
    pub brk_start: u64, pub brk: u64,
    pub pages: u32,                                 /* resident pages */
}

/// An open file description, shared by descriptors (dup, fork, SCM_RIGHTS).
#[repr(C)]
pub struct LxFile {
    pub ty: c_int, pub refs: c_int,
    pub oflags: c_int,
    pub node: *mut Vnode,
    pub pos: u64,
    pub con: *mut Console,
    pub obj: *mut c_void,                           /* ipc.rs state for pipes, sockets, ... */
    pub path: [u8; 256],                            /* Linux path it was opened as */
    pub line: [u8; 512], pub line_len: c_int, pub line_pos: c_int,   /* console input not yet read */
    pub pdata: *mut u8, pub plen: u32,              /* generated /proc contents (node is NULL) */
}

/// Linux's kernel sigaction as x86-64 passes it through rt_sigaction.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SigAction { pub handler: u64, pub flags: u64, pub restorer: u64, pub mask: u64 }

#[repr(C)]
pub struct LxProc {
    pub pid: c_int,                                 /* = pid of the main thread's task */
    pub ppid: c_int,                                /* pid of the parent process, 0 for the shell's children */
    pub vm: *mut Uvm,
    pub fds: [*mut LxFile; LX_MAXFD],
    pub fdflags: [c_int; LX_MAXFD],                 /* FD_CLOEXEC */
    pub cwd: [u8; 256],                             /* Linux path */
    pub exe: [u8; 256],
    pub con: *mut Console,
    pub threads: c_int,
    pub exit_code: c_int,
    pub term_sig: c_int,                            /* signal that killed the process, 0 for a normal exit */
    pub exiting: bool,
    pub umask: u32,
    pub rlim: [[u64; 2]; 16],                       /* rlimit soft/hard */
    pub no_new_privs: bool,
    pub dumpable: c_int,
    pub pdeathsig: u32,
    pub acts: [SigAction; LX_NSIG as usize],         /* per-process dispositions, like Linux */
    pub sig_pending: u64,                           /* process-directed signals */
    pub alarm_at: u32, pub alarm_sig: c_int,        /* alarm() and ITIMER_REAL */
    pub alarm_iv: u32,
    pub sig_child_pid: c_int, pub sig_child_status: c_int,   /* what the next SIGCHLD reports */
    pub vm_borrowed: bool,                          /* vfork child: runs in its parent's memory until execve or exit */
}

#[repr(C)]
pub struct LxThread {
    pub proc_: *mut LxProc,
    pub tid: c_int,
    pub comm: [u8; 16],                             /* PR_SET_NAME */
    pub fsbase: u64,
    pub clear_child_tid: u64,
    pub robust_list: u64,
    pub sigmask: u64,
    pub sig_pending: u64,                           /* thread-directed signals (tkill/tgkill) */
    pub alt_sp: u64, pub alt_size: u32, pub alt_flags: c_int,   /* sigaltstack */
    pub sys_nr: u32,                                /* number of the system call in progress */
    pub in_sys: i64,                                /* the call being run right now (-1: none), and its arguments */
    pub in_args: [u64; 3],
    pub sys_eintr: bool,                            /* ... and it returned -EINTR (SA_RESTART) */
    pub sig_frame: bool,                            /* a frame was just pushed on this return */
    pub sig_suspend: bool,                          /* sigsuspend: restore an older mask after the handler */
    pub sig_suspend_old: u64,
    pub slice_start: u32,                           /* for preemption */
    pub exited: bool,                               /* left through exit/exit_group, not killed */
    pub start_regs: Regs,                           /* first entry into ring 3 */
}

/* ---------------- helpers ---------------- */

/// A zero-filled `T` from the C allocator; every structure here is valid all-zero.
pub unsafe fn znew<T>() -> *mut T { kzalloc(core::mem::size_of::<T>()) as *mut T }
pub unsafe fn zfree<T>(p: *mut T) { kfree(p as *mut c_void) }

/// The Linux thread a task runs, if any.
#[inline] pub unsafe fn lx_of(t: *mut Task) -> *mut LxThread { if t.is_null() { core::ptr::null_mut() } else { (*t).lx as *mut LxThread } }
#[no_mangle]
pub unsafe extern "C" fn lx_current() -> *mut LxThread { lx_of(task_current()) }

pub fn cstr(b: &[u8]) -> &[u8] { crate::lxpath::cstr(b) }
/// strlcpy into a fixed buffer.
pub fn setstr(dst: &mut [u8], src: &[u8]) {
    let src = cstr(src);
    let n = src.len().min(dst.len().saturating_sub(1));
    dst[..n].copy_from_slice(&src[..n]);
    if n < dst.len() { dst[n] = 0; }
}
/// A NUL-terminated C string as bytes.
pub unsafe fn cbytes<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0;
    while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
/// A byte string with a NUL, for C calls.
pub fn zstr(b: &[u8]) -> alloc::vec::Vec<u8> { let mut v = cstr(b).to_vec(); v.push(0); v }

/// snprintf into a fixed buffer (C's truncation).
pub struct BufW<'a> { pub b: &'a mut [u8], pub n: usize }
impl fmt::Write for BufW<'_> {
    fn write_str(&mut self, s: &str) -> fmt::Result { self.bytes(s.as_bytes()); Ok(()) }
}
impl BufW<'_> {
    pub fn bytes(&mut self, s: &[u8]) {
        for &c in s { if self.n + 1 < self.b.len() { self.b[self.n] = c; self.n += 1; } }
        if self.n < self.b.len() { self.b[self.n] = 0; }
    }
}
pub fn bprintf(dst: &mut [u8], args: fmt::Arguments) { if dst.is_empty() { return; } dst[0] = 0; let mut w = BufW { b: dst, n: 0 }; let _ = fmt::Write::write_fmt(&mut w, args); }
/// A byte string in a format string (paths need not be UTF-8).
pub struct B<'a>(pub &'a [u8]);
impl fmt::Display for B<'_> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        for &c in cstr(self.0) { fmt::Write::write_char(f, c as char)?; }
        Ok(())
    }
}
/// Messages that keep raw bytes: klog formats with %s from a C buffer.
pub fn klog_bytes(msg: &[u8]) {
    let z = zstr(msg);
    unsafe { klog(c"%s".as_ptr(), z.as_ptr()) }
}
macro_rules! lxlog {
    ($($a:tt)*) => {{
        let mut buf = [0u8; 512];
        $crate::kernel::lx::bprintf(&mut buf, format_args!($($a)*));
        $crate::kernel::lx::klog_bytes(&buf);
    }};
}
pub(crate) use lxlog;

#[inline] pub fn rdtsc() -> u64 { unsafe { core::arch::x86_64::_rdtsc() } }
#[inline] pub unsafe fn invlpg(va: u64) { core::arch::asm!("invlpg [{}]", in(reg) va, options(nostack)); }
#[inline] pub unsafe fn set_cr3(v: usize) { core::arch::asm!("mov cr3, {}", in(reg) v, options(nostack)); }
pub fn cpuid1() -> (u32, u32, u32, u32) {
    let r = core::arch::x86_64::__cpuid(1);
    (r.eax, r.ebx, r.ecx, r.edx)
}
