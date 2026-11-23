//! The Linux system call dispatcher (was src/linux/syscall.c, x86-64 numbers): memory,
//! time, identity, thread-local storage, futexes, signals, exit. File calls are in fs.rs,
//! pipes and sockets in ipc.rs. Unknown calls return -ENOSYS and are logged once.

use super::*;
use super::mem::*;
use super::sig::*;
use super::fs::{lx_fd_get, lx_fd_alloc, lx_file_put, epoch_of, vfs_unshare};
use super::proc_::*;
use core::ptr::{self, null_mut};

/* ---------------- time ---------------- */
static mut BOOT_EPOCH: i64 = -1;
static mut BOOT_TICK: u32 = 0;
unsafe fn epoch_ms() -> i64 {
    if BOOT_EPOCH < 0 {
        let mut t = RtcTime::default();
        rtc_read(&mut t);                                       /* the RTC keeps UTC */
        BOOT_EPOCH = epoch_of([t.year, t.month, t.day, t.hour, t.minute, t.second]) * 1000;
        BOOT_TICK = timer_ticks();
    }
    BOOT_EPOCH + timer_ticks().wrapping_sub(BOOT_TICK) as i64
}
/// Nanoseconds since boot, finer than the 1 ms tick when the TSC is calibrated.
static mut TSC_PER_MS: u64 = 0;
static mut BASE_TSC: u64 = 0;
unsafe fn mono_ns() -> u64 {
    if TSC_PER_MS == 0 {
        let a = rdtsc();
        let t0 = timer_ticks();
        while timer_ticks().wrapping_sub(t0) < 5 {}
        TSC_PER_MS = (rdtsc() - a) / timer_ticks().wrapping_sub(t0).max(1) as u64;
        BASE_TSC = rdtsc();
    }
    let ms = timer_ticks() as u64;
    let sub = if TSC_PER_MS != 0 { ((rdtsc().wrapping_sub(BASE_TSC)) % TSC_PER_MS) * 1_000_000 / TSC_PER_MS } else { 0 };
    ms * 1_000_000 + sub % 1_000_000
}
fn ns_ts(ns: u64) -> [i64; 2] { [(ns / 1_000_000_000) as i64, (ns % 1_000_000_000) as i64] }
unsafe fn sys_clock_gettime(vm: *mut Uvm, clk: u64, ut: u64) -> i64 {
    let ts = match clk {
        0 | 5 | 8 | 11 => { let ms = epoch_ms(); [ms / 1000, (ms % 1000) * 1_000_000] }   /* REALTIME, _COARSE, _ALARM, TAI */
        1 | 4 | 6 | 7 | 9 => ns_ts(mono_ns()),                                        /* MONOTONIC, RAW, COARSE, BOOTTIME */
        2 | 3 => ns_ts((*task_current()).cpu_ms as u64 * 1_000_000),                   /* process / thread CPU time */
        _ => return -EINVAL,
    };
    if uput(vm, ut, &ts) { 0 } else { -EFAULT }
}
unsafe fn sleep_ns(ns: u64) -> i64 {
    let mut ms = ns.wrapping_add(999_999) / 1_000_000;
    if ms == 0 {
        if interrupted() { return -EINTR; }
        task_yield();
        return 0;
    }
    if ms > 0x7FFF_FFFF { ms = 0x7FFF_FFFF; }
    let end = timer_ticks().wrapping_add(ms as u32);
    while end.wrapping_sub(timer_ticks()) as i32 > 0 {
        if interrupted() { return -EINTR; }
        let left = end.wrapping_sub(timer_ticks());
        task_sleep(left.min(50));
    }
    0
}

/* ---------------- futex ---------------- */
struct FWait { p: *mut LxProc, addr: u64, _t: *mut Task, woken: bool, bitset: u32, next: *mut FWait }
static mut FWAITERS: *mut FWait = null_mut();
pub unsafe fn lx_futex_wake(p: *mut LxProc, a: u64, mut n: i32) {
    let mut w = FWAITERS;
    while !w.is_null() && n > 0 {
        if (*w).p == p && (*w).addr == a && !(*w).woken { (*w).woken = true; n -= 1; }
        w = (*w).next;
    }
}
unsafe fn sys_futex(p: *mut LxProc, uaddr: u64, op: c_int, val: u32, utime: u64, uaddr2: u64, val3: u32) -> i64 {
    let cmd = op & 0x7F;
    let vm = (*p).vm;
    if !uvm_check(vm, uaddr, 4, false) { return -EFAULT; }
    match cmd {
        0 | 9 => {                                               /* FUTEX_WAIT, FUTEX_WAIT_BITSET */
            if ptr::read_volatile(uaddr as *const u32) != val { return -EAGAIN; }
            let mut deadline = 0u32;
            let mut timed = false;
            if utime != 0 {
                let Some(ts) = uget::<[i64; 2]>(vm, utime) else { return -EFAULT };
                let mut ms = (ts[0].wrapping_mul(1000).wrapping_add(ts[1] / 1_000_000)) as u64;
                if cmd == 9 {                                    /* absolute */
                    let real = op & 256 != 0;
                    let now = if real { epoch_ms() as u64 } else { mono_ns() / 1_000_000 };
                    ms = if ms > now { ms - now } else { 0 };
                }
                deadline = timer_ticks().wrapping_add(ms.min(0x7FFF_FFFF) as u32);
                timed = true;
            }
            let w = kmalloc(core::mem::size_of::<FWait>()) as *mut FWait;
            ptr::write(w, FWait { p, addr: uaddr, _t: task_current(), woken: false, bitset: if cmd == 9 { val3 } else { 0xFFFF_FFFF }, next: FWAITERS });
            FWAITERS = w;
            let mut r = 0;
            while !(*w).woken {
                if (*p).exiting { break; }
                if interrupted() { r = -EINTR; break; }
                if timed && timer_ticks().wrapping_sub(deadline) as i32 >= 0 { r = -ETIMEDOUT; break; }
                task_sleep(1);
            }
            let mut pp: *mut *mut FWait = ptr::addr_of_mut!(FWAITERS);
            while !(*pp).is_null() { if *pp == w { *pp = (*w).next; break; } pp = ptr::addr_of_mut!((**pp).next); }
            kfree(w as *mut c_void);
            r
        }
        1 | 10 => {                                              /* FUTEX_WAKE, FUTEX_WAKE_BITSET */
            let mut n = 0;
            let mut w = FWAITERS;
            while !w.is_null() && n < val as i32 {
                if (*w).p == p && (*w).addr == uaddr && !(*w).woken && (cmd == 1 || (*w).bitset & val3 != 0) { (*w).woken = true; n += 1; }
                w = (*w).next;
            }
            n as i64
        }
        3 | 4 => {                                               /* FUTEX_REQUEUE, FUTEX_CMP_REQUEUE */
            if cmd == 4 && ptr::read_volatile(uaddr as *const u32) != val3 { return -EAGAIN; }
            let (mut n, mut moved) = (0i32, 0i32);
            let mut w = FWAITERS;
            while !w.is_null() {
                if (*w).p == p && (*w).addr == uaddr && !(*w).woken {
                    if n < val as i32 { (*w).woken = true; n += 1; }
                    else if moved < utime as i32 { (*w).addr = uaddr2; moved += 1; }
                }
                w = (*w).next;
            }
            n as i64
        }
        _ => -ENOSYS,
    }
}

/* ---------------- memory ---------------- */
const MAP_SHARED: u64 = 1;
const MAP_FIXED: u64 = 0x10;
const MAP_ANON: u64 = 0x20;
const MAP_FIXED_NOREPLACE: u64 = 0x100000;
fn prot_of(p: u64) -> c_int {
    (if p & 1 != 0 { VM_READ } else { 0 }) | (if p & 2 != 0 { VM_WRITE | VM_READ } else { 0 }) | (if p & 4 != 0 { VM_EXEC | VM_READ } else { 0 })
}
unsafe fn sys_mmap(p: *mut LxProc, a: u64, len: u64, prot: u64, flags: u64, fd: i64, off: u64) -> i64 {
    if len == 0 || off & (PAGE - 1) != 0 { return -EINVAL; }
    if len > (1u64 << 46) { return -ENOMEM; }
    let fixed = flags & (MAP_FIXED | MAP_FIXED_NOREPLACE) != 0;
    let vm = (*p).vm;
    if flags & MAP_FIXED_NOREPLACE != 0 && !uvm_find(vm, a).is_null() { return -EEXIST; }
    let (mut data, mut size, mut node): (*const u8, u64, *mut Vnode) = (ptr::null(), 0, null_mut());
    let mut anon = flags & MAP_ANON != 0;
    if !anon {
        let f = lx_fd_get(p, fd);
        if f.is_null() { return -EBADF; }
        if (*f).ty == FD_SHM {                                   /* memfd or /dev/shm */
            return uvm_map_shm(vm, a, len, prot_of(prot), flags & MAP_SHARED != 0, fixed, (*f).obj as *mut ShmObj, off);
        }
        if (*f).ty == FD_WINDOW {                                /* a window's pixels */
            let o = super::win::lxwin_buffer((*f).obj);
            if o.is_null() { return -ENODEV; }
            return uvm_map_shm(vm, a, len, prot_of(prot), true, fixed, o, off);
        }
        if (*f).ty == FD_ZERO { anon = true; }                   /* like anonymous */
        else if (*f).ty != FD_FILE { return -ENODEV; }
        else {
            if flags & MAP_SHARED != 0 && prot & 2 != 0 && vfs_unshare((*f).node) == 0 { return -ENOMEM; }   /* PROT_WRITE */
            data = (*(*f).node).data(); size = (*(*f).node).size() as u64; node = (*f).node;
        }
    }
    if anon && flags & MAP_SHARED != 0 {                         /* shared anonymous: shared with children after fork */
        let o = shm_new(None);
        if o.is_null() { return -ENOMEM; }
        let rr = shm_resize(o, palign(len));
        let r = if rr != 0 { rr } else { uvm_map_shm(vm, a, len, prot_of(prot), true, fixed, o, 0) };
        shm_unref(o);                                            /* the mapping holds it now */
        return r;
    }
    uvm_map(vm, a, len, prot_of(prot), if flags & MAP_SHARED != 0 { VMF_SHARED } else { VMF_PRIVATE }, fixed, data, size, off, node)
}
unsafe fn sys_brk(p: *mut LxProc, want: u64) -> i64 {
    let vm = (*p).vm;
    if want < (*vm).brk_start { return (*vm).brk as i64; }
    let old_end = palign((*vm).brk);
    let new_end = palign(want);
    if new_end > old_end {
        let mut v = (*vm).vmas;
        while !v.is_null() { if (*v).start < new_end && (*v).end > old_end { return (*vm).brk as i64; } v = (*v).next; }   /* would run into a mapping */
        if uvm_map(vm, old_end, new_end - old_end, VM_READ | VM_WRITE, VMF_HEAP, true, ptr::null(), 0, 0, null_mut()) < 0 { return (*vm).brk as i64; }
    } else if new_end < old_end { uvm_unmap(vm, new_end, old_end - new_end); }
    (*vm).brk = want;
    want as i64
}
unsafe fn sys_mremap(p: *mut LxProc, old: u64, olen: u64, nlen: u64, flags: u64, naddr: u64) -> i64 {
    let (olen, nlen) = (palign(olen), palign(nlen));
    let vm = (*p).vm;
    let v = uvm_find(vm, old);
    if v.is_null() || old & (PAGE - 1) != 0 { return -EFAULT; }
    if nlen <= olen { if nlen < olen { uvm_unmap(vm, old + nlen, olen - nlen); } return old as i64; }
    /* grow in place if the space after it is free */
    let mut free_after = flags & 2 == 0;
    let mut w = (*vm).vmas;
    while !w.is_null() && free_after { if (*w).start < old.wrapping_add(nlen) && (*w).end > old.wrapping_add(olen) { free_after = false; } w = (*w).next; }
    if free_after && (*v).end == old.wrapping_add(olen) && (*v).fdata.is_null() { (*v).end = old + nlen; return old as i64; }
    if flags & 1 == 0 { return -ENOMEM; }                       /* MREMAP_MAYMOVE */
    let dst = uvm_map(vm, if flags & 2 != 0 { naddr } else { 0 }, nlen, (*v).prot | VM_WRITE, (*v).flags, flags & 2 != 0, ptr::null(), 0, 0, null_mut());
    if dst < 0 { return dst; }
    ptr::copy(old as *const u8, dst as *mut u8, olen as usize);   /* both are mapped in the current address space */
    let prot = (*v).prot;
    uvm_unmap(vm, old, olen);
    if prot & VM_WRITE == 0 { uvm_protect(vm, dst as u64, nlen, prot); }
    dst
}

/* ---------------- misc ---------------- */
unsafe fn sys_uname(vm: *mut Uvm, u: u64) -> i64 {
    let mut b = [0u8; 390];
    for (at, s) in [(0usize, &b"Linux"[..]), (65, b"wrld"), (130, b"6.6.0-wrld"), (195, b"#1 SMP THE WRLD OS"), (260, b"x86_64"), (325, b"(none)")] {
        b[at..at + s.len()].copy_from_slice(s);
    }
    if uout(vm, u, b.as_ptr(), 390) { 0 } else { -EFAULT }
}
static mut WARNED: [u8; 512] = [0; 512];
static mut RND_S: u64 = 0xD1B5_4A32_D192_ED03;

pub unsafe fn lx_syscall(r: *mut Regs) -> i64 {
    let t = lx_current();
    if t.is_null() { return -ENOSYS; }
    let p = (*t).proc_;
    let nr = (*r).eax as i32 as i64;
    let (a0, a1, a2, a3, a4, a5) = ((*r).edi, (*r).esi, (*r).edx, (*r).r10, (*r).r8, (*r).r9);
    let mut handled = false;
    let fr = super::fs::lx_sys_file(t, nr, r, &mut handled);
    if handled { return fr; }
    let ir = super::ipc::lx_sys_ipc(t, nr, r, &mut handled);
    if handled { return ir; }
    let vm = (*p).vm;
    match nr {
        9 => sys_mmap(p, a0, a1, a2, a3, a4 as i64, a5),
        10 => uvm_protect(vm, a0, a1, prot_of(a2)),
        11 => { if a0 & (PAGE - 1) != 0 { return -EINVAL; } uvm_unmap(vm, a0, a1) }
        12 => sys_brk(p, a0),
        25 => sys_mremap(p, a0, a1, a2, a3, a4),
        28 => {                                                  /* madvise */
            if a2 == 4 || a2 == 8 { uvm_dontneed(vm, a0 & !(PAGE - 1), a0.wrapping_add(a1)); }   /* MADV_DONTNEED / MADV_FREE */
            0
        }
        26 | 27 | 149 | 150 | 151 | 152 => 0,                    /* msync, mincore, mlock... */
        13 => {                                                  /* rt_sigaction */
            if a0 < 1 || a0 > LX_NSIG as u64 || a0 == 9 || a0 == 19 { return -EINVAL; }
            if a3 != 0 && a3 != 8 { return -EINVAL; }
            let slot = ptr::addr_of_mut!((*p).acts[(a0 - 1) as usize]);
            if a2 != 0 && !uput(vm, a2, &*slot) { return -EFAULT; }
            if a1 != 0 {
                let Some(mut na) = uget::<SigAction>(vm, a1) else { return -EFAULT };
                if na.handler == 0 || na.handler == 1 { na.flags = 0; na.restorer = 0; na.mask = 0; }
                *slot = na;
            }
            0
        }
        14 => {                                                  /* rt_sigprocmask */
            if a3 != 0 && a3 != 8 { return -EINVAL; }
            let old = (*t).sigmask;
            if a1 != 0 {
                let Some(mut set) = uget::<u64>(vm, a1) else { return -EFAULT };
                set &= !((1u64 << 8) | (1u64 << 18));            /* SIGKILL, SIGSTOP */
                match a0 { 0 => (*t).sigmask |= set, 1 => (*t).sigmask &= !set, 2 => (*t).sigmask = set, _ => return -EINVAL }
            }
            if a2 != 0 && !uput(vm, a2, &old) { return -EFAULT; }
            0
        }
        131 => {                                                 /* sigaltstack */
            #[repr(C)] #[derive(Clone, Copy, Default)] struct UStack { sp: u64, flags: i32, pad: u32, size: u64 }
            let old = UStack { sp: (*t).alt_sp, flags: if (*t).alt_size != 0 { 0 } else { 2 }, pad: 0, size: (*t).alt_size as u64 };
            if a1 != 0 && !uput(vm, a1, &old) { return -EFAULT; }
            if a0 != 0 {
                let Some(ns) = uget::<UStack>(vm, a0) else { return -EFAULT };
                if ns.flags & 2 != 0 { (*t).alt_sp = 0; (*t).alt_size = 0; (*t).alt_flags = 2; }
                else {
                    if ns.flags & !(1 | 0x8000_0000u32 as i32) != 0 { return -EINVAL; }
                    if ns.sp == 0 || ns.size < 2048 { return -ENOMEM; }
                    (*t).alt_sp = ns.sp; (*t).alt_size = ns.size as u32; (*t).alt_flags = 0;
                }
            }
            0
        }
        127 => lx_signal_pending(t, a0, a1),
        128 => lx_signal_sigtimedwait(p, a0, a1, a2, a3),
        130 => lx_signal_sigsuspend(a0, a1),
        15 => lx_rt_sigreturn(r),
        16 => {                                                  /* ioctl */
            let f = lx_fd_get(p, a0 as i64); if f.is_null() { return -EBADF; }
            if (*f).ty == FD_WINDOW { return super::win::lxwin_ioctl((*f).obj, a1, a2, vm); }
            if (*f).ty == FD_DSP { return super::dsp::dsp_ioctl((*f).obj, a1 as u32 as u64, a2, vm); }   /* the request is an unsigned int */
            if (*f).ty != FD_CONSOLE { return -ENOTTY; }
            match a1 {
                0x5401 => {                                      /* TCGETS: struct termios (kernel layout, 36 bytes) */
                    /* ICRNL|IXON, OPOST|ONLCR, B38400|CS8|CREAD, ISIG|ICANON|IEXTEN. No ECHO: the console
                     * window edits and echoes the line itself, so a program's own line editor must
                     * not echo it a second time; they read lines as typed. */
                    let mut tio = [0u8; 36];
                    tio[0..4].copy_from_slice(&0x500u32.to_le_bytes()); tio[4..8].copy_from_slice(&0x5u32.to_le_bytes());
                    tio[8..12].copy_from_slice(&0xBFu32.to_le_bytes()); tio[12..16].copy_from_slice(&0x8003u32.to_le_bytes());
                    if uout(vm, a2, tio.as_ptr(), 36) { 0 } else { -EFAULT }
                }
                0x5402 | 0x5403 | 0x5404 => 0,                   /* TCSETS* */
                0x5413 => { let ws: [u16; 4] = [25, 80, 640, 400]; if uput(vm, a2, &ws) { 0 } else { -EFAULT } }   /* TIOCGWINSZ */
                0x540F | 0x5429 => { let pg: i32 = (*p).pid; if uput(vm, a2, &pg) { 0 } else { -EFAULT } }       /* TIOCGPGRP, TIOCGSID */
                /* a shell taking the terminal, flushing it or setting its size: one console per program here */
                0x5410 | 0x540E | 0x5422 | 0x540B | 0x540A | 0x5409 | 0x5414 => 0,
                0x541B => { let avail: i32 = 0; if uput(vm, a2, &avail) { 0 } else { -EFAULT } }                 /* FIONREAD */
                _ => -ENOTTY,
            }
        }
        32 | 33 | 292 => {                                       /* dup, dup2, dup3 */
            let f = lx_fd_get(p, a0 as i64); if f.is_null() { return -EBADF; }
            if nr == 32 { let fd = lx_fd_alloc(p, f, 0); if fd >= 0 { (*f).refs += 1; } return fd; }
            if a1 >= LX_MAXFD as u64 { return -EBADF; }
            if a0 == a1 { return if nr == 292 { -EINVAL } else { a1 as i64 }; }
            if !(*p).fds[a1 as usize].is_null() { lx_file_put((*p).fds[a1 as usize]); }
            (*p).fds[a1 as usize] = f; (*f).refs += 1; (*p).fdflags[a1 as usize] = if nr == 292 && a2 & 0o2000000 != 0 { 1 } else { 0 };
            a1 as i64
        }
        73 => if lx_fd_get(p, a0 as i64).is_null() { -EBADF } else { 0 },   /* flock: granted */
        72 => {                                                  /* fcntl */
            let f = lx_fd_get(p, a0 as i64); if f.is_null() { return -EBADF; }
            match a1 {
                0 | 1030 => { let fd = lx_fd_alloc(p, f, a2 as i32 as i64); if fd >= 0 { (*f).refs += 1; (*p).fdflags[fd as usize] = (a1 == 1030) as c_int; } fd }
                1 => (*p).fdflags[a0 as usize] as i64,
                2 => { (*p).fdflags[a0 as usize] = (a2 & 1) as c_int; 0 }
                3 => (*f).oflags as i64,
                4 => { (*f).oflags = ((*f).oflags & !(0o2000 | 0o4000)) | (a2 as c_int & (0o2000 | 0o4000)); 0 }
                5 | 6 | 7 | 36 | 37 | 38 => 0,                   /* locks (and OFD locks): always granted */
                1033 => {                                        /* F_ADD_SEALS */
                    if (*f).ty != FD_SHM { return -EINVAL; }
                    let o = (*f).obj as *mut ShmObj;
                    if (*o).seals & SHM_SEAL_SEAL != 0 { return -EPERM; }
                    if a2 & !31 != 0 { return -EINVAL; }
                    (*o).seals |= a2 as u32;
                    0
                }
                1034 => if (*f).ty == FD_SHM { (*((*f).obj as *mut ShmObj)).seals as i64 } else { -EINVAL },   /* F_GET_SEALS */
                _ => -EINVAL,
            }
        }
        7 => {                                                   /* poll */
            let tmo = a2 as i32;                                 /* an int: -1 waits for ever */
            let deadline = timer_ticks().wrapping_add(if tmo > 0 { tmo as u32 } else { 0 });
            loop {
                let mut ready = 0i64;
                let mut i = 0u64;
                while i < a1 && i < 1024 {
                    let Some(mut pf) = uget::<[i16; 4]>(vm, a0.wrapping_add(i * 8)) else { return -EFAULT };
                    let fd = (pf[0] as u16 as u32 | (pf[1] as u16 as u32) << 16) as i32;
                    let ev = pf[2];
                    let f = if fd < 0 { null_mut() } else { lx_fd_get(p, fd as i64) };
                    let mut rev: i16 = 0;
                    if fd >= 0 {
                        if f.is_null() { rev = 0x20; }                         /* POLLNVAL */
                        else if (FD_FILE..=FD_RANDOM).contains(&(*f).ty) { rev = ev & (1 | 4); }   /* files and devices never block */
                        else {
                            use super::ipc::lx_ipc_ready as rd;
                            if rd(f, 0x001) { rev |= 0x001; }
                            if rd(f, 0x004) { rev |= 0x004; }
                            if rd(f, 0x008) { rev |= 0x008; }
                            if rd(f, 0x010) { rev |= 0x010; }
                            rev &= ev | 0x018;
                        }
                    }
                    pf[3] = rev;
                    if rev != 0 { ready += 1; }
                    if !uput(vm, a0 + i * 8, &pf) { return -EFAULT; }
                    i += 1;
                }
                if ready != 0 { return ready; }
                if tmo == 0 { return 0; }
                if tmo > 0 && timer_ticks().wrapping_sub(deadline) as i32 >= 0 { return 0; }
                if (*p).exiting { return -EINTR; }
                task_sleep(1);
            }
        }
        24 => { task_yield(); 0 }                                /* sched_yield */
        34 => loop { if lx_block_interrupted(t) { return -EINTR; } task_sleep(1); },   /* pause */
        35 | 230 => {                                            /* nanosleep, clock_nanosleep */
            let Some(ts) = uget::<[i64; 2]>(vm, if nr == 35 { a0 } else { a2 }) else { return -EFAULT };
            let mut ns = (ts[0] as u64).wrapping_mul(1_000_000_000).wrapping_add(ts[1] as u64);
            if nr == 230 && a1 & 1 != 0 { let now = if a0 == 0 { epoch_ms() as u64 * 1_000_000 } else { mono_ns() }; ns = if ns > now { ns - now } else { 0 }; }   /* TIMER_ABSTIME */
            sleep_ns(ns)
        }
        36 => {                                                  /* getitimer: only ITIMER_REAL is real here */
            if a0 != 0 { return -EINVAL; }
            let mut it = [0i64; 4];                              /* { it_interval, it_value } */
            let now = timer_ticks();
            if (*p).alarm_at != 0 && (*p).alarm_at.wrapping_sub(now) as i32 > 0 {
                let left = (*p).alarm_at.wrapping_sub(now);
                it[2] = (left / 1000) as i64; it[3] = ((left % 1000) * 1000) as i64;
            }
            if (*p).alarm_iv != 0 { it[0] = ((*p).alarm_iv / 1000) as i64; it[1] = (((*p).alarm_iv % 1000) * 1000) as i64; }
            if uput(vm, a1, &it) { 0 } else { -EFAULT }
        }
        37 => {                                                  /* alarm */
            let now = timer_ticks();
            let mut old = 0u32;
            if (*p).alarm_at != 0 && (*p).alarm_at.wrapping_sub(now) as i32 > 0 { old = ((*p).alarm_at.wrapping_sub(now) + 999) / 1000; }
            if a0 != 0 { (*p).alarm_at = now.wrapping_add(a0.wrapping_mul(1000).min(0x7FFF_FFFF) as u32); (*p).alarm_sig = 14; (*p).alarm_iv = 0; }
            else { (*p).alarm_at = 0; }
            old as i64
        }
        38 => {                                                  /* setitimer */
            if a0 != 0 { return -EINVAL; }                       /* ITIMER_REAL only */
            let Some(it) = uget::<[i64; 4]>(vm, a1) else { return -EFAULT };
            let now = timer_ticks();
            let mut left = 0u32;
            if (*p).alarm_at != 0 && (*p).alarm_at.wrapping_sub(now) as i32 > 0 { left = (*p).alarm_at.wrapping_sub(now); }
            if a2 != 0 {
                let mut old = [0i64; 4];
                old[2] = (left / 1000) as i64; old[3] = ((left % 1000) * 1000) as i64;
                if (*p).alarm_iv != 0 { old[0] = ((*p).alarm_iv / 1000) as i64; old[1] = (((*p).alarm_iv % 1000) * 1000) as i64; }
                if !uput(vm, a2, &old) { return -EFAULT; }
            }
            let iv = (it[0].max(0) as u64).wrapping_mul(1000).wrapping_add((it[1].max(0) / 1000) as u64);
            let mut ms = (it[2].max(0) as u64).wrapping_mul(1000).wrapping_add((it[3].max(0) / 1000) as u64);
            if ms == 0 && it[3] > 0 { ms = 1; }                  /* under a millisecond still fires */
            (*p).alarm_at = if ms != 0 { now.wrapping_add(ms.min(0x7FFF_FFFF) as u32) } else { 0 };
            (*p).alarm_iv = iv.min(0x7FFF_FFFF) as u32;
            (*p).alarm_sig = 14;
            0
        }
        228 => sys_clock_gettime(vm, a0, a1),
        229 => { let ts: [i64; 2] = [0, 1_000_000]; if a1 != 0 { if uput(vm, a1, &ts) { 0 } else { -EFAULT } } else { 0 } }
        96 => { let ms = epoch_ms(); let tv: [i64; 2] = [ms / 1000, (ms % 1000) * 1000]; if a0 != 0 && !uput(vm, a0, &tv) { return -EFAULT; } 0 }
        201 => { let s = epoch_ms() / 1000; if a0 != 0 { uput(vm, a0, &s); } s }   /* time */
        39 => (*p).pid as i64,
        186 => (*t).tid as i64,
        110 => (*p).ppid as i64,                                 /* getppid */
        102 | 104 | 107 | 108 => 1000,                           /* getuid, getgid, geteuid, getegid */
        118 | 120 => { let id: u32 = 1000; for o in [a0, a1, a2] { if !uput(vm, o, &id) { return -EFAULT; } } 0 }   /* getres[ug]id */
        105 | 106 | 113 | 114 | 117 | 119 => 0,                  /* set*id */
        111 | 121 | 124 => (*p).pid as i64,                      /* getpgrp, getpgid, getsid */
        109 | 112 => 0,                                         /* setpgid, setsid */
        115 => 0,                                               /* getgroups: none */
        63 => sys_uname(vm, a0),
        95 => { let o = (*p).umask; (*p).umask = (a0 & 0o777) as u32; o as i64 }
        97 | 302 => {                                           /* getrlimit, prlimit64 */
            let (res, out, inp) = if nr == 97 { (a0, a1, 0) } else { (a1, a3, a2) };
            let pid = if nr == 97 { (*p).pid } else { a0 as c_int };
            if res > 15 { return -EINVAL; }
            if pid != 0 && pid != (*p).pid { return -EINVAL; }   /* only the caller's limits */
            if inp != 0 {
                let Some(nl) = uget::<[u64; 2]>(vm, inp) else { return -EFAULT };
                if nl[0] > nl[1] { return -EINVAL; }
                (*p).rlim[res as usize] = nl;
            }
            let lim = (*p).rlim[res as usize];
            if out != 0 && !uput(vm, out, &lim) { return -EFAULT; }
            0
        }
        160 => {                                                 /* setrlimit */
            if a0 > 15 { return -EINVAL; }
            let Some(nl) = uget::<[u64; 2]>(vm, a1) else { return -EFAULT };
            if nl[0] > nl[1] { return -EINVAL; }
            (*p).rlim[a0 as usize] = nl;
            0
        }
        99 => {                                                  /* sysinfo */
            let mut si = [0u8; 112];
            si[0..8].copy_from_slice(&((timer_ticks() / 1000) as i64).to_le_bytes());
            si[32..40].copy_from_slice(&(256u64 << 20).to_le_bytes()); si[40..48].copy_from_slice(&(128u64 << 20).to_le_bytes());
            si[80..82].copy_from_slice(&(task_count() as u16).to_le_bytes());
            si[104..108].copy_from_slice(&1u32.to_le_bytes());
            if uout(vm, a0, si.as_ptr(), 112) { 0 } else { -EFAULT }
        }
        98 => { let ru = [0u8; 144]; if uout(vm, a1, ru.as_ptr(), 144) { 0 } else { -EFAULT } }   /* getrusage */
        100 => { let tm: [i64; 4] = [((*task_current()).cpu_ms / 10) as i64, 0, 0, 0]; if a0 != 0 { uput(vm, a0, &tm); } (timer_ticks() / 10) as i64 }   /* times */
        158 => {                                                 /* arch_prctl */
            if a0 == 0x1002 { if a1 >= USER_HI { return -EPERM; } (*t).fsbase = a1; arch_set_user_fs(a1); return 0; }   /* ARCH_SET_FS */
            if a0 == 0x1003 { return if uput(vm, a1, &(*t).fsbase) { 0 } else { -EFAULT }; }                         /* ARCH_GET_FS */
            -EINVAL
        }
        218 => { (*t).clear_child_tid = a0; (*t).tid as i64 }    /* set_tid_address */
        273 => { (*t).robust_list = a0; 0 }                      /* set_robust_list */
        274 => -ENOSYS,                                          /* get_robust_list */
        202 => sys_futex(p, a0, a1 as c_int, a2 as u32, a3, a4, a5 as u32),
        203 => 0,                                                /* sched_setaffinity */
        204 => { let m: u64 = 1; if a1 < 8 { return -EINVAL; } if uput(vm, a2, &m) { 8 } else { -EFAULT } }   /* sched_getaffinity: one CPU */
        143..=148 => 0,                                          /* sched_* params */
        318 => {                                                 /* getrandom */
            if !uvm_check(vm, a0, a1, true) { return -EFAULT; }
            let d = a0 as *mut u8;
            for i in 0..a1 { RND_S ^= rdtsc().wrapping_add(i); RND_S ^= RND_S << 13; RND_S ^= RND_S >> 7; RND_S ^= RND_S << 17; *d.add(i as usize) = (RND_S >> 24) as u8; }
            a1 as i64
        }
        157 => match a0 as c_int {                              /* prctl */
            1 => { if a1 > LX_NSIG as u64 { return -EINVAL; } (*p).pdeathsig = a1 as u32; 0 }   /* PR_SET_PDEATHSIG */
            2 => { let v = (*p).pdeathsig; if uput(vm, a1, &v) { 0 } else { -EFAULT } }
            3 => { let v: i32 = (*p).dumpable; if uput(vm, a1, &v) { 0 } else { -EFAULT } }
            4 => { (*p).dumpable = (a1 != 0) as c_int; 0 }
            13 => 0,
            14 => if a1 == 0 { 0 } else { -EINVAL },
            15 => {                                              /* PR_SET_NAME */
                let Some(mut nm) = uget::<[u8; 16]>(vm, a1) else { return -EFAULT };
                nm[15] = 0;
                setstr(&mut (*t).comm, &nm); 0
            }
            16 => { let mut nm = [0u8; 16]; setstr(&mut nm, &(*t).comm); if uput(vm, a1, &nm) { 0 } else { -EFAULT } }   /* PR_GET_NAME */
            21 => 0,                                             /* PR_GET_SECCOMP: none */
            22 => -EINVAL,                                       /* PR_SET_SECCOMP: not implemented */
            29 => 0,                                             /* PR_SET_TIMERSLACK (hint) */
            30 => 50000,                                         /* PR_GET_TIMERSLACK */
            38 => { if a1 != 1 || a2 != 0 || a3 != 0 || a4 != 0 { return -EINVAL; } (*p).no_new_privs = true; 0 }   /* PR_SET_NO_NEW_PRIVS */
            39 => (*p).no_new_privs as i64,
            40 => if uput(vm, a1, &(*t).clear_child_tid) { (*t).tid as i64 } else { -EFAULT },   /* PR_GET_TID_ADDRESS */
            _ => -EINVAL,                                        /* not implemented, honestly */
        },
        62 => lx_signal_kill(p, a0 as c_int, a1 as c_int),
        200 => lx_signal_tkill(a0 as c_int, a1 as c_int),
        234 => lx_signal_tgkill(a0 as c_int, a1 as c_int, a2 as c_int),
        56 => lx_clone(r, a0, a1, a2, a3, a4),
        57 | 58 => lx_fork(r),                                   /* fork, vfork (vfork does not suspend the parent) */
        59 => lx_execve(r, a0, a1, a2),
        61 => lx_wait4(p, a0 as c_int, a1, a2 as c_int, a3),
        60 => lx_thread_exit(a0 as c_int & 0xFF),
        231 => { lx_exit_group(p, a0 as c_int & 0xFF); 0 }
        334 | 435 | 425 | 426 | 427 => -ENOSYS,                  /* rseq, clone3, io_uring */
        _ => {
            if (0..512).contains(&nr) && WARNED[nr as usize] == 0 { WARNED[nr as usize] = 1; lxlog!("linux: pid {}: system call {} is not implemented", (*p).pid, nr); }
            -ENOSYS
        }
    }
}
