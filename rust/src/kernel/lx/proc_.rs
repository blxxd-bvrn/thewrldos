//! Linux processes (was src/linux/proc.c): ELF loading (position-independent executables
//! and the dynamic loader they name), the initial stack (argv, envp, auxv), threads as
//! kernel tasks, ring-3 faults, preemption, and teardown. The entry points the assembly
//! and the scheduler call are at the end.

use super::*;
use super::mem::*;
use super::sig::*;
use super::fs::{lx_file_put, lx_path};
use crate::lxpath::{self as lp, ElfError};
use alloc::vec::Vec;
use core::ptr::{self, null_mut};

#[allow(clashing_extern_declarations)]
extern "C" {
    fn vfs_resolve(path: *const c_char, cwd: *const c_char) -> *mut Vnode;
    fn vfs_path(n: *mut Vnode, buf: *mut c_char, cap: c_int);
}
pub unsafe fn resolve_w(w: &[u8]) -> *mut Vnode { let z = zstr(w); vfs_resolve(z.as_ptr() as *const c_char, ptr::null()) }
pub unsafe fn node_path(n: *mut Vnode) -> Vec<u8> { let mut b = [0u8; 512]; vfs_path(n, b.as_mut_ptr() as *mut c_char, 512); cstr(&b).to_vec() }
pub unsafe fn node_data<'a>(n: *mut Vnode) -> &'a [u8] {
    let d = (*n).data();
    if d.is_null() { &[] } else { core::slice::from_raw_parts(d, (*n).size() as usize) }
}
pub unsafe fn node_name<'a>(n: *mut Vnode) -> &'a [u8] { cstr((*n).name_bytes()) }
/// path_basename (the WRLD one: after the last \\ or /).
pub fn basename(p: &[u8]) -> &[u8] { let p = cstr(p); &p[crate::vfspath::basename_at(p)..] }

pub fn lx_is_elf_bytes(d: &[u8]) -> bool { lp::is_elf(d) }
#[no_mangle]
pub unsafe extern "C" fn lx_is_elf(d: *const u8, n: u32) -> CBool {
    if d.is_null() { return 0; }
    lp::is_elf(core::slice::from_raw_parts(d, n as usize)) as CBool
}

#[derive(Default)]
struct Loaded { base: u64, entry: u64, phdr: u64, phnum: u64, end: u64, interp: Vec<u8> }

unsafe fn load_elf(vm: *mut Uvm, n: *mut Vnode, dyn_base: u64, out: &mut Loaded, err: &mut [u8]) -> bool {
    let d = node_data(n);
    let name = B(node_name(n));
    let (eh, ph) = match lp::elf_headers(d) {
        Ok(x) => x,
        Err(ElfError::NotElf) => { bprintf(err, format_args!("{} is not an ELF program.", name)); return false; }
        Err(ElfError::NotX86_64) => { bprintf(err, format_args!("{} is not a 64-bit x86-64 Linux program.", name)); return false; }
        Err(ElfError::NotExecutable) => { bprintf(err, format_args!("{} is not an executable.", name)); return false; }
        Err(ElfError::BadHeaders) => { bprintf(err, format_args!("{}: bad program headers.", name)); return false; }
    };
    let (mut lo, mut hi) = (!0u64, 0u64);
    for p in &ph { if p.ty == lp::PT_LOAD { lo = lo.min(p.vaddr & !(PAGE - 1)); hi = hi.max(p.vaddr.wrapping_add(p.memsz)); } }
    if lo == !0 { bprintf(err, format_args!("{} has nothing to load.", name)); return false; }
    let base = if eh.ty == lp::ET_DYN { dyn_base.wrapping_sub(lo) } else { 0 };
    if eh.ty == lp::ET_EXEC && lo < USER_LO {
        bprintf(err, format_args!("{} is not position-independent (it was linked at 0x{:x}). Only PIE programs, the default on current Linux distributions, can run.", name, lo));
        return false;
    }
    *out = Loaded::default();
    for p in &ph {
        if p.ty == lp::PT_INTERP && p.filesz < 256 && p.offset.wrapping_add(p.filesz) <= d.len() as u64 {
            out.interp = cstr(&d[p.offset as usize..(p.offset + p.filesz) as usize]).to_vec();
        }
        if p.ty == lp::PT_PHDR { out.phdr = base.wrapping_add(p.vaddr); }
        if p.ty != lp::PT_LOAD || p.memsz == 0 { continue; }
        if p.offset.wrapping_add(p.filesz) > d.len() as u64 { bprintf(err, format_args!("{} is truncated.", name)); return false; }
        let va = base.wrapping_add(p.vaddr);
        let start = va & !(PAGE - 1);
        let end = palign(va.wrapping_add(p.memsz));
        let prot = (if p.flags & 4 != 0 { VM_READ } else { 0 }) | (if p.flags & 2 != 0 { VM_WRITE } else { 0 }) | (if p.flags & 1 != 0 { VM_EXEC } else { 0 });
        /* bytes past filesz come out as zeros (.bss), because the file data ends there */
        let r = uvm_map(vm, start, end.wrapping_sub(start), prot, VMF_PRIVATE, true, d.as_ptr(), p.offset + p.filesz, p.offset.wrapping_sub(va - start), n);
        if r < 0 { bprintf(err, format_args!("Could not map {}.", name)); return false; }
    }
    if out.phdr == 0 { out.phdr = base.wrapping_add(lo).wrapping_add(eh.phoff); }   /* the headers are inside the first segment */
    out.base = base; out.entry = base.wrapping_add(eh.entry); out.phnum = eh.phnum as u64; out.end = base.wrapping_add(hi);
    true
}

/// Write into a process that is not running yet, through the identity map.
unsafe fn poke(vm: *mut Uvm, mut va: u64, src: &[u8]) -> bool {
    let mut s = 0usize;
    let mut len = src.len() as u64;
    while len != 0 {
        if !uvm_fault(vm, va, true) { return false; }
        let pte = pte_lookup(vm, va);
        if pte.is_null() { return false; }
        let pg = (*pte & ADDR_MASK) as *mut u8;
        let o = va & (PAGE - 1);
        let k = len.min(PAGE - o);
        ptr::copy_nonoverlapping(src.as_ptr().add(s), pg.add(o as usize), k as usize);
        va += k; s += k as usize; len -= k;
    }
    true
}

unsafe fn proc_new() -> *mut LxProc {
    let p = znew::<LxProc>();
    if p.is_null() { return null_mut(); }
    (*p).vm = uvm_new();
    if (*p).vm.is_null() { zfree(p); return null_mut(); }
    setstr(&mut (*p).cwd, b"/");
    (*p).umask = 0o22;
    (*p).dumpable = 1;
    for i in 0..16 { (*p).rlim[i] = [!0, !0]; }
    (*p).rlim[3] = [STACK_SIZE, STACK_SIZE];
    (*p).rlim[7] = [LX_MAXFD as u64, LX_MAXFD as u64];
    p
}

unsafe extern "C" fn lx_thread_start(arg: *mut c_void) {
    let th = arg as *mut LxThread;
    (*th).slice_start = timer_ticks();
    arch_set_user_fs((*th).fsbase);
    enter_user(ptr::addr_of_mut!((*th).start_regs));
}
unsafe fn thread_new(p: *mut LxProc, name: &[u8], regs: &Regs, fsbase: u64) -> *mut Task {
    let th = znew::<LxThread>();
    if th.is_null() { return null_mut(); }
    (*th).proc_ = p;
    (*th).in_sys = -1;
    setstr(&mut (*th).comm, name);
    (*th).start_regs = *regs;
    (*th).fsbase = fsbase;
    let zn = zstr(name);
    let t = task_create(zn.as_ptr() as *const c_char, lx_thread_start, th as *mut c_void, 64 * 1024);
    if t.is_null() { zfree(th); return null_mut(); }
    (*t).lx = th as *mut c_void; (*t).kind = 4;
    (*t).cr3 = uvm_cr3((*p).vm);
    (*t).console = (*p).con as *mut c_void;
    (*th).tid = (*t).pid;
    (*p).threads += 1;
    t
}

/* clone() flags */
const CLONE_VM: u64 = 0x100;
const CLONE_VFORK: u64 = 0x4000;
const CLONE_THREAD: u64 = 0x10000;
const CLONE_SETTLS: u64 = 0x80000;
const CLONE_PARENT_SETTID: u64 = 0x100000;
const CLONE_CHILD_CLEARTID: u64 = 0x200000;
const CLONE_CHILD_SETTID: u64 = 0x1000000;

/// clone() for threads: same address space, files and signal handlers.
pub unsafe fn lx_clone(r: *mut Regs, flags: u64, stack: u64, ptid: u64, ctid: u64, tls: u64) -> i64 {
    let self_ = task_current();
    let th = lx_of(self_);
    let p = (*th).proc_;
    if flags & CLONE_THREAD == 0 {
        /* A new process. fork() is clone(SIGCHLD) and gets a copy of the memory;
         * posix_spawn() and vfork() are clone(CLONE_VM | CLONE_VFORK), where the child runs
         * in its parent's memory (on a stack of its own) and only prepares and calls execve,
         * while the parent waits. Nothing is copied, which matters for a big program. */
        return lx_fork_ex(r, stack, flags & CLONE_SETTLS != 0, tls, flags, ptid, ctid);
    }
    if flags & CLONE_VM == 0 { return -EINVAL; }                /* a thread shares the memory */
    let mut nr = *r;
    nr.eax = 0;                                                 /* the child sees 0 */
    if stack != 0 { nr.esp = stack; }
    let sname = cstr(&(*self_).name);
    let mut name = [0u8; 48];
    bprintf(&mut name, format_args!("{}:thread", B(sname)));
    if sname.windows(7).any(|w| w == b":thread") { setstr(&mut name, sname); }
    let t = thread_new(p, &name, &nr, if flags & CLONE_SETTLS != 0 { tls } else { (*th).fsbase });
    if t.is_null() { return -ENOMEM; }
    let ct = lx_of(t);
    (*ct).sigmask = (*th).sigmask;
    if flags & CLONE_CHILD_CLEARTID != 0 { (*ct).clear_child_tid = ctid; }
    if flags & CLONE_PARENT_SETTID != 0 && uvm_check((*p).vm, ptid, 4, true) { ptr::write_volatile(ptid as *mut i32, (*ct).tid); }
    if flags & CLONE_CHILD_SETTID != 0 && uvm_check((*p).vm, ctid, 4, true) { ptr::write_volatile(ctid as *mut i32, (*ct).tid); }
    (*ct).tid as i64
}

/// Load a program and its loader into `vm`, then build the Linux start-up stack. Used both
/// by the shell (a new process) and by execve (an old one).
unsafe fn build_image(p: *mut LxProc, n: *mut Vnode, vm: *mut Uvm, argv: &[Vec<u8>], envp: Option<&[Vec<u8>]>,
                      out: &mut Regs, err: &mut [u8]) -> bool {
    let mut exe = Loaded::default();
    if !load_elf(vm, n, EXE_BASE, &mut exe, err) { return false; }
    let mut entry = exe.entry;
    let mut interp_base = 0;
    if !exe.interp.is_empty() {
        let ip = lx_path(p, -100, &exe.interp);
        let in_n = resolve_w(&ip.1);
        if in_n.is_null() || (*in_n).is_dir() {
            bprintf(err, format_args!("{} needs {} (the Linux dynamic loader), which is not installed.", B(node_name(n)), B(&exe.interp)));
            return false;
        }
        let mut inl = Loaded::default();
        if !load_elf(vm, in_n, INTERP_BASE, &mut inl, err) { return false; }
        entry = inl.entry; interp_base = inl.base;
    }
    /* program break just past the executable */
    (*vm).brk_start = palign(exe.end).wrapping_add(16 * PAGE);
    (*vm).brk = (*vm).brk_start;
    /* Linux path of the program */
    {
        let wp = node_path(n);
        let lin = lp::wrld_to_linux(&wp, 256);
        setstr(&mut (*p).exe, &lin);
    }
    /* stack */
    if uvm_map(vm, STACK_TOP - STACK_SIZE, STACK_SIZE, VM_READ | VM_WRITE, VMF_STACK, true, ptr::null(), 0, 0, null_mut()) < 0 {
        bprintf(err, format_args!("Could not map the stack.")); return false;
    }
    const DEFENV: [&[u8]; 8] = [b"PATH=/usr/local/bin:/usr/bin:/bin", b"HOME=/home/user", b"USER=user", b"LOGNAME=user",
                                b"SHELL=/bin/sh", b"TERM=dumb", b"LANG=C.UTF-8", b"PWD=/"];
    let defenv: Vec<Vec<u8>>;
    let env: &[Vec<u8>] = match envp { Some(e) => e, None => { defenv = DEFENV.iter().map(|s| s.to_vec()).collect(); &defenv } };
    let (argc, envc) = (argv.len(), env.len());
    let mut sp = STACK_TOP;
    let mut argp: Vec<u64> = Vec::new();
    if argp.try_reserve_exact(argc + envc + 2).is_err() { bprintf(err, format_args!("Not enough memory for the program's arguments.")); return false; }
    argp.resize(argc + envc, 0);
    let push_str = |sp: &mut u64, s: &[u8]| -> u64 { let s = cstr(s); let l = s.len() as u64 + 1; *sp -= l; let mut v = s.to_vec(); v.push(0); poke(vm, *sp, &v); *sp };
    let exe_path = cstr(&(*p).exe).to_vec();
    let execfn = push_str(&mut sp, &exe_path);
    for i in 0..envc { argp[argc + i] = push_str(&mut sp, &env[i]); }
    for i in (0..argc).rev() { argp[i] = push_str(&mut sp, &argv[i]); }
    let platform = push_str(&mut sp, b"x86_64");
    let mut rnd = [0u8; 16];
    for (i, b) in rnd.iter_mut().enumerate() { *b = ((rdtsc() >> (i % 8 * 3)) as u8) ^ ((i * 73) as u8); }
    sp -= 16; poke(vm, sp, &rnd); let random = sp;
    sp &= !15;
    let (_, _, _, cd) = cpuid1();
    let aux: [u64; 38] = [3, exe.phdr, 4, lp::PHDR_SIZE, 5, exe.phnum, 6, PAGE, 7, interp_base, 8, 0, 9, exe.entry,
                          11, 1000, 12, 1000, 13, 1000, 14, 1000, 15, platform, 16, cd as u64, 17, 100, 23, 0, 25, random,
                          26, 0, 31, execfn, 51, 4096];
    let aux_full: Vec<u64> = aux.iter().copied().chain([0u64, 0u64]).collect();
    let words = 1 + argc + 1 + envc + 1 + aux_full.len();
    if words & 1 != 0 { sp -= 8; }                              /* rsp % 16 == 0 at argc */
    sp -= words as u64 * 8;
    let mut blk: Vec<u64> = Vec::new();
    if blk.try_reserve_exact(words).is_err() { bprintf(err, format_args!("Not enough memory for the program's stack.")); return false; }
    blk.push(argc as u64);
    blk.extend_from_slice(&argp[..argc]);
    blk.push(0);
    blk.extend_from_slice(&argp[argc..argc + envc]);
    blk.push(0);
    blk.extend_from_slice(&aux_full);
    let bytes = core::slice::from_raw_parts(blk.as_ptr() as *const u8, words * 8);
    if !poke(vm, sp, bytes) { bprintf(err, format_args!("Not enough memory for the program's stack.")); return false; }
    *out = Regs::default();
    out.eip = entry; out.esp = sp; out.cs = 0x2B; out.ss = 0x23; out.eflags = 0x202;
    true
}

const LX_MAXARG: usize = 256;

/// Start a Linux program from the shell or the desktop.
#[no_mangle]
pub unsafe extern "C" fn lx_spawn(wrld_path: *const c_char, argc: c_int, argv: *mut *mut c_char, envp: *mut *mut c_char,
                                  in_con: *mut Console, con: *mut Console, err: *mut c_char, ecap: c_int) -> c_int {
    let errb = core::slice::from_raw_parts_mut(err as *mut u8, ecap.max(0) as usize);
    let wpath = cbytes(wrld_path as *const u8).to_vec();
    let mut n = resolve_w(&wpath);
    if n.is_null() || (*n).is_dir() { bprintf(errb, format_args!("File not found: {}", B(&wpath))); return -1; }
    let mut args: Vec<Vec<u8>> = (0..argc.max(0) as usize).map(|i| cbytes(*argv.add(i) as *const u8).to_vec()).collect();
    let envs: Option<Vec<Vec<u8>>> = if envp.is_null() { None } else {
        let mut v = Vec::new(); let mut i = 0;
        while !(*envp.add(i)).is_null() { v.push(cbytes(*envp.add(i) as *const u8).to_vec()); i += 1; }
        Some(v)
    };
    /* a script: start its interpreter with the script's path (up to 4 levels, as Linux allows) */
    for _depth in 0..4 {
        let Some((interp, arg)) = lp::shebang(node_data(n)) else { break };
        if node_data(n).starts_with(b"\x7FELF") { break; }
        let script = lp::wrld_to_linux(&node_path(n), 512);
        let mut na: Vec<Vec<u8>> = Vec::new();
        na.push(interp.clone());
        if !arg.is_empty() { na.push(arg.clone()); }
        na.push(script);
        for a in args.iter().skip(1) { if na.len() >= LX_MAXARG + 6 { break; } na.push(a.clone()); }
        if interp.first() != Some(&b'/') {
            bprintf(errb, format_args!("{} names its interpreter without a full path ({}).", B(basename(&wpath)), B(&interp))); return -1;
        }
        let ip = lx_path(null_mut(), -100, &interp);
        let inn = resolve_w(&ip.1);
        if inn.is_null() || (*inn).is_dir() { bprintf(errb, format_args!("{} needs {}, which is not installed.", B(basename(&wpath)), B(&interp))); return -1; }
        n = inn; args = na;
    }
    let p = proc_new();
    if p.is_null() { bprintf(errb, format_args!("Not enough memory to start the program.")); return -1; }
    (*p).con = con;
    let mut r = Regs::default();
    let fail = |p: *mut LxProc| {
        for i in 0..LX_MAXFD { if !(*p).fds[i].is_null() { lx_file_put((*p).fds[i]); (*p).fds[i] = null_mut(); } }
        uvm_free((*p).vm); zfree(p);
        -1
    };
    if !build_image(p, n, (*p).vm, &args, envs.as_deref(), &mut r, errb) { return fail(p); }
    /* stdin/stdout/stderr: the console */
    let cf = znew::<LxFile>();
    if cf.is_null() { bprintf(errb, format_args!("Not enough memory to start the program.")); return fail(p); }
    (*cf).ty = FD_CONSOLE; (*cf).con = con; (*cf).oflags = 2; setstr(&mut (*cf).path, b"/dev/tty");
    for i in 1..3 { (*p).fds[i] = cf; (*cf).refs += 1; }
    let mut ci = cf;
    if !in_con.is_null() && in_con != con { ci = znew::<LxFile>(); (*ci).ty = FD_CONSOLE; (*ci).con = in_con; setstr(&mut (*ci).path, b"/dev/tty"); }
    (*p).fds[0] = ci; (*ci).refs += 1;
    let t = thread_new(p, basename(&wpath), &r, 0);
    if t.is_null() { bprintf(errb, format_args!("Not enough memory to start the program.")); return fail(p); }
    (*p).pid = (*t).pid;
    lxlog!("linux: started {} as pid {} (entry {:x})", B(&(*p).exe), (*p).pid, r.eip);
    (*p).pid
}

/* a program started from the desktop has no console window: what it writes goes to the
 * boot log a line at a time, and it reads end-of-file */
static mut LOGCON_BUF: [u8; 256] = [0; 256];
static mut LOGCON_LEN: usize = 0;
unsafe extern "C" fn logcon_write(_c: *mut Console, s: *const c_char, n: c_int) {
    for i in 0..n.max(0) as usize {
        let ch = *s.add(i) as u8;
        if ch == b'\n' || LOGCON_LEN == LOGCON_BUF.len() - 1 {
            if LOGCON_LEN != 0 { let l = LOGCON_LEN; lxlog!("linux: {}", B(&LOGCON_BUF[..l])); }
            LOGCON_LEN = 0;
            if ch == b'\n' { continue; }
        }
        if ch != b'\r' { LOGCON_BUF[LOGCON_LEN] = ch; LOGCON_LEN += 1; }
    }
}
unsafe extern "C" fn logcon_readline(_c: *mut Console, buf: *mut c_char, cap: c_int) -> c_int { if cap != 0 { *buf = 0; } 0 }
unsafe extern "C" fn logcon_getkey(_c: *mut Console) -> c_int { -1 }
unsafe extern "C" fn logcon_none(_c: *mut Console) {}
unsafe extern "C" fn logcon_title(_c: *mut Console, _t: *const c_char) {}
unsafe extern "C" fn logcon_color(_c: *mut Console, _f: c_int, _b: c_int) {}
unsafe extern "C" fn logcon_alive(_c: *mut Console) -> CBool { 1 }
static mut LOG_CONSOLE: Console = Console {
    write: Some(logcon_write), readline: Some(logcon_readline), getkey: Some(logcon_getkey), clear: Some(logcon_none),
    set_title: Some(logcon_title), set_color: Some(logcon_color), alive: Some(logcon_alive), imp: null_mut(),
};
/// Start a Linux program in the background (for the desktop: no console).
#[no_mangle]
pub unsafe extern "C" fn lx_launch(wrld_path: *const c_char, argc: c_int, argv: *mut *mut c_char, envp: *mut *mut c_char, err: *mut c_char, ecap: c_int) -> c_int {
    let c = ptr::addr_of_mut!(LOG_CONSOLE);
    lx_spawn(wrld_path, argc, argv, envp, c, c, err, ecap)
}

/* fork(): a second process with a copy of this address space, the same open files and the
 * same thread state. The child returns 0 from the same call. */
pub unsafe fn lx_fork(r: *mut Regs) -> i64 { lx_fork_ex(r, 0, false, 0, 0, 0, 0) }
unsafe fn vfork_child_running(cpid: c_int) -> bool {
    let mut o = task_list();
    while !o.is_null() {
        let l = lx_of(o);
        if !l.is_null() && (*(*l).proc_).pid == cpid { return (*(*l).proc_).vm_borrowed; }
        o = (*o).next;
    }
    false
}
pub unsafe fn lx_fork_ex(r: *mut Regs, stack: u64, set_tls: bool, tls: u64, flags: u64, ptid: u64, ctid: u64) -> i64 {
    let self_ = task_current();
    let th = lx_of(self_);
    let p = (*th).proc_;
    let c = znew::<LxProc>();
    if c.is_null() { return -ENOMEM; }
    let vfork = flags & (CLONE_VM | CLONE_VFORK) == (CLONE_VM | CLONE_VFORK);
    if vfork { (*c).vm = (*p).vm; (*c).vm_borrowed = true; } else { (*c).vm = uvm_clone((*p).vm); }
    if (*c).vm.is_null() { zfree(c); return -ENOMEM; }
    (*c).con = (*p).con; (*c).umask = (*p).umask; (*c).ppid = (*p).pid;
    (*c).acts = (*p).acts;                                       /* fork inherits the dispositions */
    (*c).rlim = (*p).rlim;
    (*c).no_new_privs = (*p).no_new_privs; (*c).dumpable = (*p).dumpable; (*c).pdeathsig = (*p).pdeathsig;
    (*c).cwd = (*p).cwd;
    (*c).exe = (*p).exe;
    for i in 0..LX_MAXFD {
        (*c).fds[i] = (*p).fds[i];
        (*c).fdflags[i] = (*p).fdflags[i];
        if !(*c).fds[i].is_null() { (*(*c).fds[i]).refs += 1; }
    }
    let mut nr = *r;
    nr.eax = 0;                                                 /* the child sees 0 */
    if stack != 0 { nr.esp = stack; }
    let mut name = [0u8; 48];
    bprintf(&mut name, format_args!("{}:fork", B(&(*self_).name)));
    let t = thread_new(c, &name, &nr, if set_tls { tls } else { (*th).fsbase });
    if t.is_null() {
        for i in 0..LX_MAXFD { if !(*c).fds[i].is_null() { lx_file_put((*c).fds[i]); } }
        if !(*c).vm_borrowed { uvm_free((*c).vm); }
        zfree(c);
        return -ENOMEM;
    }
    let ct = lx_of(t);
    (*ct).sigmask = (*th).sigmask;
    (*c).pid = (*t).pid;
    /* the child's tid in the parent's memory (CLONE_PARENT_SETTID) and in the child's copy */
    if flags & CLONE_PARENT_SETTID != 0 && uvm_check((*p).vm, ptid, 4, true) { ptr::write_volatile(ptid as *mut i32, (*ct).tid); }
    if flags & CLONE_CHILD_CLEARTID != 0 { (*ct).clear_child_tid = ctid; }
    if flags & CLONE_CHILD_SETTID != 0 && ctid != 0 { uvm_poke32((*c).vm, ctid, (*ct).tid as u32); }
    let cpid = (*c).pid;
    lxlog!("linux: pid {} {} pid {}", (*p).pid, if vfork { "vforked" } else { "forked" }, cpid);
    /* vfork: the parent goes on once the child has its own memory (execve) or has ended */
    while vfork && vfork_child_running(cpid) { task_sleep(1); }
    cpid as i64
}

/// A string from user memory: Ok(bytes) or Err(-EFAULT / -ENAMETOOLONG).
pub unsafe fn user_str(vm: *mut Uvm, src: u64, cap: usize) -> Result<Vec<u8>, i64> {
    let mut v = Vec::new();
    for i in 0..cap as u64 {
        if !uvm_check(vm, src.wrapping_add(i), 1, false) { return Err(-EFAULT); }
        let c = ptr::read_volatile((src + i) as *const u8);
        if c == 0 { return Ok(v); }
        v.push(c);
    }
    Err(-ENAMETOOLONG)
}
unsafe fn copy_vec(vm: *mut Uvm, uvec: u64) -> Result<Vec<Vec<u8>>, i64> {
    let mut out = Vec::new();
    if uvec == 0 { return Ok(out); }
    for i in 0..LX_MAXARG as u64 {
        if !uvm_check(vm, uvec + i * 8, 8, false) { return Err(-EFAULT); }
        let sp = ptr::read_unaligned((uvec + i * 8) as *const u64);
        if sp == 0 { return Ok(out); }
        out.push(user_str(vm, sp, 512)?);
    }
    Err(-E2BIG)
}

/// execve(): replace this process's image. Everything that belongs to the old program is
/// dropped, and the call returns into the new entry point.
pub unsafe fn lx_execve(r: *mut Regs, upath: u64, uargv: u64, uenvp: u64) -> i64 {
    let self_ = task_current();
    let th = lx_of(self_);
    let p = (*th).proc_;
    let mut path = match user_str((*p).vm, upath, 512) { Ok(v) => v, Err(e) => return e };
    if path.is_empty() { return -ENOENT; }
    let mut argv = match copy_vec((*p).vm, uargv) { Ok(v) => v, Err(e) => return e };
    let envp = match copy_vec((*p).vm, uenvp) { Ok(v) => v, Err(e) => return e };
    if argv.is_empty() { argv.push(path.clone()); }
    /* the Linux path of the program */
    let (kind, wrld) = lx_path(p, -100, &path);
    let mut n = if kind == lp::kind::FILE { resolve_w(&wrld) } else { null_mut() };
    if n.is_null() || (*n).is_dir() { return -ENOENT; }
    /* "#!interpreter [arg]": run the interpreter with the script's path in front of the arguments */
    for _depth in 0..4 {
        let Some((interp, arg)) = lp::shebang(node_data(n)) else { break };
        let extra = if arg.is_empty() { 1 } else { 2 };
        if argv.len() + extra > LX_MAXARG { return -E2BIG; }
        let mut na = Vec::new();
        na.push(interp.clone());
        if !arg.is_empty() { na.push(arg); }
        na.push(path.clone());                                  /* the script, as the caller named it */
        na.extend(argv.drain(1..));
        argv = na;
        let (k2, w2) = lx_path(p, -100, &interp);
        n = if k2 == lp::kind::FILE { resolve_w(&w2) } else { null_mut() };
        if n.is_null() || (*n).is_dir() { return -ENOENT; }
        path = lp::cut(interp, 512);
    }
    let nv = uvm_new();
    if nv.is_null() { return -ENOMEM; }
    let mut nr = Regs::default();
    let mut err = [0u8; 128];
    if !build_image(p, n, nv, &argv, if envp.is_empty() { None } else { Some(&envp) }, &mut nr, &mut err) {
        lxlog!("linux: execve {}: {}", B(&path), B(&err));
        uvm_free(nv);
        return -ENOEXEC;
    }
    /* every other thread of this process ends here. Mark them as having left through
     * exit_group, so reaping one does not take the process down. */
    let mut o = task_list();
    while !o.is_null() {
        let nx = (*o).next;
        let l = lx_of(o);
        if o != self_ && !l.is_null() && (*l).proc_ == p { (*l).exited = true; task_kill((*o).pid); }
        o = nx;
    }
    /* close-on-exec descriptors */
    for i in 0..LX_MAXFD {
        if !(*p).fds[i].is_null() && (*p).fdflags[i] != 0 { lx_file_put((*p).fds[i]); (*p).fds[i] = null_mut(); (*p).fdflags[i] = 0; }
    }
    /* switch address spaces before releasing the old one */
    let old = (*p).vm;
    (*p).vm = nv;
    (*self_).cr3 = uvm_cr3(nv);
    set_cr3((*nv).pml4 as usize);
    if (*p).vm_borrowed { (*p).vm_borrowed = false; }          /* a vfork child gives its parent's memory back */
    else { uvm_free(old); }
    /* thread state that belonged to the old image */
    (*th).clear_child_tid = 0; (*th).robust_list = 0; (*th).sigmask = 0; (*th).fsbase = 0;
    arch_set_user_fs(0);
    lx_sig_reset(p);
    (*p).exiting = false; (*p).exit_code = 0; (*p).term_sig = 0;
    let exe = (*p).exe;
    setstr(&mut (*self_).name, basename(&exe));
    *r = nr;
    0
}

/* wait4(): collect a child that has ended */
struct Zombie { pid: c_int, ppid: c_int, status: c_int, next: *mut Zombie }
static mut ZOMBIES: *mut Zombie = null_mut();

unsafe fn record_zombie(p: *mut LxProc) {
    if (*p).ppid <= 0 { return; }
    let z = kmalloc(core::mem::size_of::<Zombie>()) as *mut Zombie;
    if z.is_null() { return; }
    (*z).pid = (*p).pid; (*z).ppid = (*p).ppid;
    (*z).status = if (*p).term_sig != 0 { (*p).term_sig & 0x7F } else { ((*p).exit_code & 0xFF) << 8 };
    (*z).next = ZOMBIES; ZOMBIES = z;
}
unsafe fn child_exists(p: *mut LxProc, pid: c_int) -> bool {
    let mut t = task_list();
    while !t.is_null() {
        let l = lx_of(t);
        if !l.is_null() && (*l).proc_ != p && (*(*l).proc_).ppid == (*p).pid && (pid <= 0 || (*(*l).proc_).pid == pid) { return true; }
        t = (*t).next;
    }
    false
}
pub unsafe fn lx_wait4(p: *mut LxProc, pid: c_int, ustatus: u64, options: c_int, urusage: u64) -> i64 {
    let nohang = options & 1 != 0;                               /* WNOHANG */
    loop {
        let mut pp: *mut *mut Zombie = ptr::addr_of_mut!(ZOMBIES);
        let mut z: *mut Zombie = null_mut();
        while !(*pp).is_null() {
            if (**pp).ppid == (*p).pid && (pid <= 0 || (**pp).pid == pid) { z = *pp; break; }
            pp = ptr::addr_of_mut!((**pp).next);
        }
        if !z.is_null() {
            *pp = (*z).next;
            let (status, reaped) = ((*z).status, (*z).pid);
            kfree(z as *mut c_void);
            if ustatus != 0 && !uput((*p).vm, ustatus, &status) { return -EFAULT; }
            if urusage != 0 { let ru = [0u8; 144]; if !uout((*p).vm, urusage, ru.as_ptr(), 144) { return -EFAULT; } }
            return reaped as i64;
        }
        if !child_exists(p, pid) { return -ECHILD; }
        if nohang { return 0; }
        if interrupted() { return -EINTR; }
        task_sleep(1);
    }
}

unsafe fn proc_free(p: *mut LxProc) {
    let status = if (*p).term_sig != 0 { (*p).term_sig & 0x7F } else { ((*p).exit_code & 0xFF) << 8 };
    /* children that asked for a signal when their parent dies (PR_SET_PDEATHSIG) */
    let mut o = task_list();
    while !o.is_null() {
        let nx = (*o).next;
        let l = lx_of(o);
        if !l.is_null() && (*l).proc_ != p && (*(*l).proc_).ppid == (*p).pid {
            let c = (*l).proc_;
            (*c).ppid = 1;
            if (*c).pdeathsig != 0 { let sig = (*c).pdeathsig as c_int; (*c).pdeathsig = 0; lx_signal_kill(c, (*c).pid, sig); }
            if (*c).vm_borrowed && (*c).vm == (*p).vm { (*l).exited = true; task_kill((*o).pid); }   /* a vfork child cannot outlive its memory */
        }
        o = nx;
    }
    for i in 0..LX_MAXFD { if !(*p).fds[i].is_null() { lx_file_put((*p).fds[i]); (*p).fds[i] = null_mut(); } }
    record_zombie(p);
    lx_signal_child_exit(p, (*p).pid, status);
    if !(*p).vm_borrowed { uvm_free((*p).vm); }
    (*p).vm_borrowed = false;
    lxlog!("linux: pid {} ended ({})", (*p).pid, (*p).exit_code);
    zfree(p);
}

/* ---------------- scheduler hooks (task.c) ---------------- */
#[no_mangle]
pub unsafe extern "C" fn lx_task_switched(t: *mut Task) { arch_set_user_fs((*lx_of(t)).fsbase); }
#[no_mangle]
pub unsafe extern "C" fn lx_task_reaped(t: *mut Task) {
    let th = lx_of(t);
    let p = (*th).proc_;
    let killed = !(*th).exited;
    (*t).lx = null_mut();
    zfree(th);
    if killed && !(*p).exiting {                         /* killed from outside (Ctrl+C): the whole process goes */
        (*p).exiting = true; (*p).exit_code = 130;
        let mut o = task_list();
        while !o.is_null() { let nx = (*o).next; let l = lx_of(o); if !l.is_null() && (*l).proc_ == p { task_kill((*o).pid); } o = nx; }
    }
    (*p).threads -= 1;
    if (*p).threads == 0 { proc_free(p); }
}

pub unsafe fn lx_thread_exit(code: c_int) -> ! {
    let t = task_current();
    let th = lx_of(t);
    let ctid = (*th).clear_child_tid;
    if ctid != 0 && uvm_check((*(*th).proc_).vm, ctid, 4, true) {
        ptr::write_volatile(ctid as *mut u32, 0);
        super::sys::lx_futex_wake((*th).proc_, ctid, 1);
    }
    (*th).exited = true;
    /* run on the kernel's own address space from here: the process may be freed */
    set_cr3(paging_kernel_root());
    (*t).cr3 = paging_kernel_root();
    task_exit(code)
}
pub unsafe fn lx_exit_group(p: *mut LxProc, code: c_int) {
    (*p).exiting = true; (*p).exit_code = code;
    let self_ = task_current();
    let mut t = task_list();
    while !t.is_null() {
        let nx = (*t).next;
        let l = lx_of(t);
        if t != self_ && !l.is_null() && (*l).proc_ == p { task_kill((*t).pid); }
        t = nx;
    }
    let sl = lx_of(self_);
    if !sl.is_null() && (*sl).proc_ == p { lx_thread_exit(code); }
}

/// Before every return to ring 3.
#[no_mangle]
pub unsafe extern "C" fn lx_return_check(r: *mut Regs) {
    let t = task_current();
    let th = lx_of(t);
    if th.is_null() { return; }
    if (*(*th).proc_).exiting { lx_thread_exit((*(*th).proc_).exit_code); }
    if (*th).sig_frame { (*th).sig_frame = false; }      /* a fault already built the frame */
    else { lx_signal_deliver(r); }
    (*th).sys_eintr = false;                             /* one restart opportunity per call */
    if (*(*th).proc_).exiting { lx_thread_exit((*(*th).proc_).exit_code); }
    (*r).eflags = ((*r).eflags & 0x0CD5) | 0x202;        /* user flags only, interrupts on */
    (*r).cs = 0x2B; (*r).ss = 0x23;
    let now = timer_ticks();
    if now.wrapping_sub((*th).slice_start) >= 10 { task_yield(); (*th).slice_start = timer_ticks(); }
}

/// For the watchdog: the Linux system call a task is stuck in, if any.
#[no_mangle]
pub unsafe extern "C" fn lx_task_syscall(t: *mut Task, out: *mut c_char, cap: c_int) -> CBool {
    let th = lx_of(t);
    if th.is_null() || (*th).in_sys < 0 { return 0; }
    let o = core::slice::from_raw_parts_mut(out as *mut u8, cap.max(0) as usize);
    bprintf(o, format_args!("Linux system call {}({:x}, {:x}, {:x})", (*th).in_sys, (*th).in_args[0], (*th).in_args[1], (*th).in_args[2]));
    1
}
/// "lxtrace on" in the Command Prompt: every system call to the log.
#[no_mangle]
pub static mut lx_strace: CBool = 0;

#[no_mangle]
pub unsafe extern "C" fn lx_syscall_c(r: *mut Regs) {
    let th = lx_current();
    let nr = (*r).eax;
    let (a0, a1, a2) = ((*r).edi, (*r).esi, (*r).edx);
    if !th.is_null() { (*th).in_sys = nr as i64; (*th).in_args = [a0, a1, a2]; }
    let v = super::sys::lx_syscall(r);
    if !th.is_null() { (*th).in_sys = -1; }
    if lx_strace != 0 && !th.is_null() && nr != 24 && nr != 228 {   /* not sched_yield / clock_gettime: too many */
        /* calls that take a path: show it */
        let pa = if [2u64, 4, 6, 21, 89, 87, 83, 80, 59].contains(&nr) { a0 }
                 else if [257u64, 262, 269, 263, 258, 267, 332, 439].contains(&nr) { a1 } else { 0 };
        let mut path: Vec<u8> = Vec::new();
        let p = (*th).proc_;
        if pa != 0 && !p.is_null() && uvm_check((*p).vm, pa, 1, false) {
            while path.len() < 95 && uvm_check((*p).vm, pa + path.len() as u64, 1, false) {
                let ch = *((pa + path.len() as u64) as *const u8);
                if ch == 0 { break; }
                path.push(ch);
            }
        }
        lxlog!("lx {}: {}({:x}, {:x}, {:x}) = {}{}{}", if p.is_null() { -1 } else { (*p).pid }, nr, a0, a1, a2, v,
               if path.is_empty() { "" } else { "  " }, B(&path));
    }
    if !th.is_null() { (*th).sys_nr = nr as u32; (*th).sys_eintr = nr != 15 && v == -EINTR; }
    (*r).eax = v as u64;
}

/// Exceptions: demand paging, a signal handler, or the end of a program that crashed.
#[no_mangle]
pub unsafe extern "C" fn lx_exception(r: *mut Regs, cr2: usize) -> CBool {
    let t = task_current();
    let th = lx_of(t);
    if th.is_null() { return 0; }
    let p = (*th).proc_;
    let cr2 = cr2 as u64;
    if (*r).int_no == 14 {
        let user_addr = (USER_LO..USER_HI).contains(&cr2);
        if user_addr && (*r).err_code & 1 == 0 && uvm_fault((*p).vm, cr2, (*r).err_code & 2 != 0) { return 1; }
    }
    if (*r).cs & 3 == 0 { return 0; }                    /* a kernel bug: let the normal handler report it */
    const MAP: [(u64, c_int, &str); 8] = [(0, 8, "Floating point exception"), (6, 4, "Illegal instruction"), (13, 11, "Segmentation fault"),
        (14, 11, "Segmentation fault"), (16, 8, "Floating point exception"), (19, 8, "Floating point exception"),
        (17, 7, "Bus error"), (3, 5, "Trace/breakpoint trap")];
    let (mut sig, mut what) = (11, "Segmentation fault");
    for (vec, s, nm) in MAP { if vec == (*r).int_no { sig = s; what = nm; } }
    let mut code = 1;
    let mut a = cr2;
    if (*r).int_no == 14 { code = if (*r).err_code & 1 != 0 { 2 } else { 1 }; }   /* SEGV_ACCERR / SEGV_MAPERR */
    else if sig == 4 || sig == 8 { a = (*r).eip; }
    if lx_signal_fault(r, sig, code, a) { return 1; }    /* the program installed a handler */
    lxlog!("linux: pid {}: {} at {:x} (address {:x}, error {:x})", (*p).pid, what, (*r).eip, cr2, (*r).err_code);
    {   /* where: the file mapped there and the offset in it, and the instruction's bytes */
        let v = uvm_find((*p).vm, (*r).eip);
        if !v.is_null() && !(*v).fnode.is_null() {
            lxlog!("linux: pid {}:   in {} + {:x}", (*p).pid, B(node_name((*v).fnode)), (*r).eip.wrapping_sub((*v).start).wrapping_add((*v).foff));
        }
        if uvm_check((*p).vm, (*r).eip, 12, false) {
            let c = core::slice::from_raw_parts((*r).eip as *const u8, 12);
            lxlog!("linux: pid {}:   code {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}", (*p).pid,
                   c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]);
        }
    }
    if !(*p).con.is_null() {
        let mut m = [0u8; 128];
        bprintf(&mut m, format_args!("{}\n", what));
        let l = cstr(&m).len();
        if let Some(w) = (*(*p).con).write { w((*p).con, m.as_ptr() as *const c_char, l as c_int); }
    }
    (*p).term_sig = sig;
    lx_exit_group(p, 128 + sig);
    1
}
