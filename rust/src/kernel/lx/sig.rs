//! Linux signals (was src/linux/signal.c): dispositions, pending sets, the x86-64
//! rt_sigframe that runs a handler in ring 3, sigreturn, and the kill family.
//!
//! Delivery happens in lx_return_check, on the way back to ring 3, the same place
//! preemption is decided. A handler gets Linux's frame layout: the restorer address, the
//! ucontext the restorer hands to rt_sigreturn, and the siginfo, exactly as glibc expects.

use super::*;
use super::mem::*;
use super::proc_::lx_exit_group;
use core::ptr;

pub const SIGINT: c_int = 2;
pub const SIGKILL: c_int = 9;
pub const SIGSEGV: c_int = 11;
pub const SIGPIPE: c_int = 13;
pub const SIGALRM: c_int = 14;
pub const SIGCHLD: c_int = 17;
pub const SIGCONT: c_int = 18;
pub const SIGSTOP: c_int = 19;
const SIGTSTP: c_int = 20;
const SIGTTIN: c_int = 21;
const SIGTTOU: c_int = 22;
const SIGURG: c_int = 23;
const SIGWINCH: c_int = 28;
const SIG_DFL: u64 = 0;
const SIG_IGN: u64 = 1;
pub const SIG_UNBLOCKABLE: u64 = 1 << (SIGKILL - 1) | 1 << (SIGSTOP - 1);

const SA_SIGINFO_UNUSED: u64 = 0x4;
const SA_RESTORER: u64 = 0x0400_0000;
const SA_ONSTACK: u64 = 0x0800_0000;
const SA_RESTART: u64 = 0x1000_0000;
const SA_NODEFER: u64 = 0x4000_0000;
const SA_RESETHAND: u64 = 0x8000_0000;
const SS_ONSTACK: i32 = 1;
const SS_DISABLE: c_int = 2;

/// Linux's sigcontext on x86-64; glibc's gregset_t is this same array of 23 registers.
#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
struct SigCtx {
    r8: u64, r9: u64, r10: u64, r11: u64, r12: u64, r13: u64, r14: u64, r15: u64,
    rdi: u64, rsi: u64, rbp: u64, rbx: u64, rdx: u64, rax: u64, rcx: u64, rsp: u64, rip: u64, eflags: u64,
    csgsfs: u64, err: u64, trapno: u64, oldmask: u64, cr2: u64, fpstate: u64,
    reserved1: [u64; 8],
}
#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
struct KStack { ss_sp: u64, ss_flags: i32, pad: u32, ss_size: u64 }
#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
struct KUcontext { uc_flags: u64, uc_link: u64, uc_stack: KStack, uc_mcontext: SigCtx, uc_sigmask: u64 }
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RtSigFrame { pretcode: u64, uc: KUcontext, info: [u8; 128] }
const _: () = assert!(core::mem::size_of::<SigCtx>() == 256);
const _: () = assert!(core::mem::size_of::<KUcontext>() == 304);
const _: () = assert!(core::mem::size_of::<RtSigFrame>() == 440);
const SIGFRAME_ALLOC: u64 = 512;                     /* frame, plus room for a restorer stub */

/* The interrupted code's FPU, SSE and AVX registers go into the frame (uc_mcontext.fpstate)
 * and come back on rt_sigreturn. Without this a handler, and the C library it calls (memcpy
 * uses SSE/AVX), left its own values in the vector registers of the code it interrupted:
 * JavaScriptCore's garbage collector stops threads with a signal, and JIT code that kept a
 * number or a copy in xmm registers carried on with garbage (wrong results, crashes).
 * The kernel saved that state on entry (FPU_SAVE in src/arch/x86_64/lx.asm and isr.asm) at
 * (regs - 1088) & ~63: 512 bytes of FXSAVE, or 832 of XSAVE (x87, SSE, AVX) when AVX is on. */
extern "C" { static fpu_xsave: u8; }
const FP_BYTES: u64 = 832;
unsafe fn fpu_area(r: *const Regs) -> *mut u8 { (((r as u64) - 1088) & !63) as *mut u8 }
unsafe fn fpu_len() -> usize { if ptr::read_volatile(ptr::addr_of!(fpu_xsave)) != 0 { 832 } else { 512 } }

/* ---------------- user memory ---------------- */
unsafe fn uwritable(vm: *mut Uvm, a: u64, n: u64) -> bool {
    if a < USER_LO || a.wrapping_add(n) > USER_HI || a.wrapping_add(n) < a { return false; }
    let mut p = a & !(PAGE - 1);
    while p < a + n { if !uvm_fault(vm, p, true) { return false; } p += PAGE; }
    uvm_check(vm, a, n, true)
}
pub unsafe fn uwrite(vm: *mut Uvm, a: u64, src: *const u8, n: u64) -> bool {
    if !uwritable(vm, a, n) { return false; }
    ptr::copy(src, a as *mut u8, n as usize);
    true
}
pub unsafe fn uread(vm: *mut Uvm, dst: *mut u8, a: u64, n: u64) -> bool {
    if a < USER_LO || a.wrapping_add(n) > USER_HI || a.wrapping_add(n) < a { return false; }
    let mut p = a & !(PAGE - 1);
    while p < a + n { if !uvm_fault(vm, p, false) { return false; } p += PAGE; }
    if !uvm_check(vm, a, n, false) { return false; }
    ptr::copy(a as *const u8, dst, n as usize);
    true
}

/* ---------------- dispositions ---------------- */
/// 0: ignored by default, 1: terminates, 2: stops (no job control: treated as ignored).
fn sig_default(sig: c_int) -> i32 {
    match sig {
        SIGCHLD | SIGCONT | SIGURG | SIGWINCH => 0,
        SIGSTOP | SIGTSTP | SIGTTIN | SIGTTOU => 2,
        _ => if (1..=LX_NSIG).contains(&sig) { 1 } else { 0 },
    }
}
#[inline] pub fn sig_bit(sig: c_int) -> u64 { 1u64 << (sig - 1) }
fn ignored_by_default(sig: c_int) -> bool { sig_default(sig) != 1 }

pub unsafe fn lx_proc_by_pid(pid: c_int) -> *mut LxProc {
    let mut t = task_list();
    while !t.is_null() {
        let l = lx_of(t);
        if !l.is_null() && (*(*l).proc_).pid == pid { return (*l).proc_; }
        t = (*t).next;
    }
    ptr::null_mut()
}
pub unsafe fn lx_sig_reset(p: *mut LxProc) { if !p.is_null() { (*p).acts = [SigAction::default(); LX_NSIG as usize]; } }
pub unsafe fn lx_signal_raise(p: *mut LxProc, sig: c_int) {
    if !p.is_null() && (1..=LX_NSIG).contains(&sig) { (*p).sig_pending |= sig_bit(sig); }
}
pub unsafe fn lx_signal_raise_thread(th: *mut LxThread, sig: c_int) {
    if !th.is_null() && (1..=LX_NSIG).contains(&sig) { (*th).sig_pending |= sig_bit(sig); }
}
/// SIGCHLD for a parent that is still around; the pid and status go into the siginfo.
pub unsafe fn lx_signal_child_exit(p: *mut LxProc, pid: c_int, status: c_int) {
    let par = if p.is_null() { ptr::null_mut() } else { lx_proc_by_pid((*p).ppid) };
    if par.is_null() { return; }
    (*par).sig_child_pid = pid;
    (*par).sig_child_status = status;
    lx_signal_raise(par, SIGCHLD);
}
pub unsafe fn lx_signal_alarm_check(p: *mut LxProc) {
    if (*p).alarm_at == 0 { return; }
    if (timer_ticks().wrapping_sub((*p).alarm_at) as i32) < 0 { return; }
    (*p).alarm_at = if (*p).alarm_iv != 0 { timer_ticks().wrapping_add((*p).alarm_iv) } else { 0 };
    lx_signal_raise(p, if (*p).alarm_sig != 0 { (*p).alarm_sig } else { SIGALRM });
}
/// A signal that is not blocked and not ignored would interrupt a blocking call; ignored
/// signals are dropped here.
pub unsafe fn lx_signal_check(th: *mut LxThread) -> bool {
    if th.is_null() { return false; }
    let p = (*th).proc_;
    if (*p).exiting { return true; }
    lx_signal_alarm_check(p);
    let all = (*th).sig_pending | (*p).sig_pending;
    if all == 0 { return false; }
    for sig in 1..=LX_NSIG {
        let bit = sig_bit(sig);
        if all & bit == 0 { continue; }
        if !(sig == SIGKILL || sig == SIGSTOP) && (*th).sigmask & bit != 0 { continue; }
        let a = &(*p).acts[(sig - 1) as usize];
        if a.handler == SIG_IGN || (a.handler == SIG_DFL && ignored_by_default(sig)) {
            (*th).sig_pending &= !bit; (*p).sig_pending &= !bit; continue;
        }
        return true;
    }
    false
}
pub unsafe fn lx_block_interrupted(th: *mut LxThread) -> bool {
    if th.is_null() { return false; }
    if (*(*th).proc_).exiting { return true; }
    lx_signal_check(th)
}
/// The usual check in a blocking loop.
pub unsafe fn interrupted() -> bool { lx_block_interrupted(lx_current()) }

/* ---------------- the frame ---------------- */
pub fn fill_info(si: &mut [u8; 128], sig: c_int, code: c_int, a: u64, sender: c_int, status: c_int) {
    *si = [0; 128];
    si[0..4].copy_from_slice(&sig.to_le_bytes());
    si[8..12].copy_from_slice(&code.to_le_bytes());
    if sig == SIGCHLD {
        si[16..20].copy_from_slice(&sender.to_le_bytes()); si[20..24].copy_from_slice(&1000i32.to_le_bytes());
        si[24..28].copy_from_slice(&status.to_le_bytes());
    } else if code > 0 && a != 0 {
        si[16..24].copy_from_slice(&a.to_le_bytes());     /* si_addr */
    } else {
        si[16..20].copy_from_slice(&sender.to_le_bytes()); si[20..24].copy_from_slice(&1000i32.to_le_bytes());
    }
}
/// Build the frame and point regs at the handler; false if the stack cannot hold it.
unsafe fn push_frame(th: *mut LxThread, r: *mut Regs, sig: c_int, a: *mut SigAction, code: c_int, a_: u64, sender: c_int, status: c_int) -> bool {
    let p = (*th).proc_;
    let r = &mut *r;
    let mut sp = r.esp;
    let mut on_alt = false;
    if (*a).flags & SA_ONSTACK != 0 && (*th).alt_size != 0 && (*th).alt_flags & SS_DISABLE == 0 {
        let top = (*th).alt_sp.wrapping_add((*th).alt_size as u64);
        if sp < (*th).alt_sp || sp >= top { sp = top; on_alt = true; }
    }
    if sp < 128 + FP_BYTES + SIGFRAME_ALLOC + 128 { return false; }
    let fp_at = (sp - 128 - FP_BYTES) & !63;                     /* the saved vector registers, above the frame */
    let top = ((fp_at - SIGFRAME_ALLOC) & !15) - 8;
    let restart = (*th).sys_eintr && (*a).flags & SA_RESTART != 0 && r.int_no == 0x100 &&
                  !((*th).sys_nr == 34 || (*th).sys_nr == 128 || (*th).sys_nr == 130);
    let saved_rip = if restart { r.eip.wrapping_sub(2) } else { r.eip };
    let saved_rax = if restart { (*th).sys_nr as u64 } else { r.eax };
    /* x86-64 has no in-kernel restorer: the C library installs one (SA_RESTORER, glibc's
     * __restore_rt), and a trampoline on the stack cannot work because the stack is not
     * executable. Without a restorer the signal is fatal, which is what Linux does too. */
    if (*a).flags & SA_RESTORER == 0 || (*a).restorer == 0 { return false; }
    let mut buf = [0u8; SIGFRAME_ALLOC as usize];
    let f = buf.as_mut_ptr() as *mut RtSigFrame;
    let mut fr: RtSigFrame = RtSigFrame { pretcode: (*a).restorer, uc: KUcontext::default(), info: [0; 128] };
    fr.uc.uc_stack = KStack { ss_sp: if on_alt { (*th).alt_sp } else { 0 }, ss_flags: if on_alt { SS_ONSTACK } else { 0 }, pad: 0,
                              ss_size: if on_alt { (*th).alt_size as u64 } else { 0 } };
    fr.uc.uc_mcontext = SigCtx {
        r8: r.r8, r9: r.r9, r10: r.r10, r11: r.r11, r12: r.r12, r13: r.r13, r14: r.r14, r15: r.r15,
        rdi: r.edi, rsi: r.esi, rbp: r.ebp, rbx: r.ebx, rdx: r.edx, rax: saved_rax, rcx: r.ecx,
        rsp: r.esp, rip: saved_rip, eflags: r.eflags,
        csgsfs: (r.cs & 0xFFFF) | ((r.ss & 0xFFFF) << 48), err: r.err_code, trapno: r.int_no,
        oldmask: 0, cr2: a_, fpstate: fp_at, reserved1: [0; 8],
    };
    {
        /* XSAVE leaves a component the program has not touched since its last reset (XSTATE_BV bit
         * clear) unwritten: the save area holds whatever was on the kernel stack there. Write the
         * reset values instead, so rt_sigreturn (which loads all three components) restores them. */
        let n = fpu_len();
        let mut fp = [0u8; 832];
        ptr::copy_nonoverlapping(fpu_area(r as *const Regs), fp.as_mut_ptr(), n);
        if n == 832 {
            let bv = u64::from_le_bytes([fp[512], fp[513], fp[514], fp[515], fp[516], fp[517], fp[518], fp[519]]);
            if bv & 1 == 0 { for b in &mut fp[0..24] { *b = 0; } fp[0] = 0x7f; fp[1] = 0x03; for b in &mut fp[32..160] { *b = 0; } }   /* x87: FCW 0x037F */
            if bv & 2 == 0 { for b in &mut fp[160..416] { *b = 0; } }                                                                  /* xmm0-15 */
            if bv & 4 == 0 { for b in &mut fp[576..832] { *b = 0; } }                                                                  /* upper halves of ymm0-15 */
        }
        if !uwrite((*p).vm, fp_at, fp.as_ptr(), n as u64) { return false; }
    }
    let mut restore = (*th).sigmask;
    if (*th).sig_suspend { restore = (*th).sig_suspend_old; (*th).sig_suspend = false; }
    let mut active = (*th).sigmask | (*a).mask;
    if (*a).flags & SA_NODEFER == 0 { active |= sig_bit(sig); }
    (*th).sigmask = active & !SIG_UNBLOCKABLE;
    fr.uc.uc_sigmask = restore & !SIG_UNBLOCKABLE;
    fill_info(&mut fr.info, sig, code, a_, sender, status);
    ptr::write_unaligned(f, fr);
    if !uwrite((*p).vm, top, buf.as_ptr(), SIGFRAME_ALLOC) { return false; }
    let handler = (*a).handler;
    if (*a).flags & SA_RESETHAND != 0 { *a = SigAction::default(); }
    let _ = SA_SIGINFO_UNUSED;
    r.eip = handler;
    r.esp = top;
    r.edi = sig as u64;
    r.esi = top + 8 + core::mem::size_of::<KUcontext>() as u64;   /* &siginfo */
    r.edx = top + 8;                                              /* &ucontext */
    true
}

/* ---------------- delivering, returning ---------------- */
pub unsafe fn lx_signal_deliver(r: *mut Regs) {
    let th = lx_current();
    if th.is_null() { return; }
    let p = (*th).proc_;
    lx_signal_alarm_check(p);
    for sig in 1..=LX_NSIG {
        let bit = sig_bit(sig);
        let thread_dir = (*th).sig_pending & bit != 0;
        let proc_dir = !thread_dir && (*p).sig_pending & bit != 0;
        if !thread_dir && !proc_dir { continue; }
        if !(sig == SIGKILL || sig == SIGSTOP) && (*th).sigmask & bit != 0 { continue; }
        let a = ptr::addr_of_mut!((*p).acts[(sig - 1) as usize]);
        let clear = || if thread_dir { (*th).sig_pending &= !bit; } else { (*p).sig_pending &= !bit; };
        if (*a).handler == SIG_IGN { clear(); continue; }
        if (*a).handler == SIG_DFL {
            if sig_default(sig) != 1 { clear(); continue; }
            clear();
            (*p).term_sig = sig;
            lx_exit_group(p, 128 + sig);
            return;
        }
        let mut sender = if thread_dir { (*th).tid } else { (*p).pid };
        let (mut code, mut status) = (0, 0);
        if sig == SIGCHLD { sender = (*p).sig_child_pid; status = (*p).sig_child_status; code = 1; }
        clear();
        if !push_frame(th, r, sig, a, code, 0, sender, status) {
            (*p).term_sig = SIGSEGV;
            lx_exit_group(p, 128 + SIGSEGV);
        }
        return;                                      /* one frame per return; next signal later */
    }
}
/// A fault in ring 3: run the handler if the program installed one.
pub unsafe fn lx_signal_fault(r: *mut Regs, sig: c_int, code: c_int, a_: u64) -> bool {
    let th = lx_current();
    if th.is_null() || !(1..=LX_NSIG).contains(&sig) { return false; }
    let p = (*th).proc_;
    let a = ptr::addr_of_mut!((*p).acts[(sig - 1) as usize]);
    if (*a).handler == SIG_DFL || (*a).handler == SIG_IGN { return false; }
    if (*th).sigmask & sig_bit(sig) != 0 { return false; }       /* blocked: force the default instead */
    if !push_frame(th, r, sig, a, code, a_, (*p).pid, 0) {
        lxlog!("linux: pid {}: no room for the signal {} frame (stack {:x})", (*p).pid, sig, (*r).esp);
        (*p).term_sig = SIGSEGV;
        lx_exit_group(p, 128 + SIGSEGV);
    }
    (*th).sig_frame = true;                          /* lx_return_check must not build a second frame */
    true
}
pub unsafe fn lx_rt_sigreturn(r: *mut Regs) -> i64 {
    let th = lx_current();
    if th.is_null() { return -EINVAL; }
    let p = (*th).proc_;
    let r = &mut *r;
    if r.esp < 8 { lxlog!("linux: pid {}: rt_sigreturn with no stack", (*p).pid); (*p).term_sig = SIGSEGV; lx_exit_group(p, 128 + SIGSEGV); return 0; }
    let base = r.esp - 8;
    let mut raw = [0u8; 440];
    if !uread((*p).vm, raw.as_mut_ptr(), base, 440) {
        lxlog!("linux: pid {}: rt_sigreturn frame at {:x} unreadable", (*p).pid, base);
        (*p).term_sig = SIGSEGV;
        lx_exit_group(p, 128 + SIGSEGV);
        return 0;
    }
    let f: RtSigFrame = ptr::read_unaligned(raw.as_ptr() as *const RtSigFrame);
    let mc = f.uc.uc_mcontext;
    let rip = mc.rip;
    if !(USER_LO..USER_HI).contains(&rip) {
        lxlog!("linux: pid {}: rt_sigreturn to {:x}, outside the program", (*p).pid, rip);
        (*p).term_sig = SIGSEGV;
        lx_exit_group(p, 128 + SIGSEGV);
        return 0;
    }
    r.r8 = mc.r8; r.r9 = mc.r9; r.r10 = mc.r10; r.r11 = mc.r11;
    r.r12 = mc.r12; r.r13 = mc.r13; r.r14 = mc.r14; r.r15 = mc.r15;
    r.edi = mc.rdi; r.esi = mc.rsi; r.ebp = mc.rbp; r.ebx = mc.rbx;
    r.edx = mc.rdx; r.ecx = mc.rcx;
    r.eip = mc.rip; r.esp = mc.rsp; r.eflags = mc.eflags;
    r.cs = 0x2B; r.ss = 0x23;                        /* the only user selectors we have */
    if mc.fpstate != 0 {
        /* the interrupted code's vector registers, into the area FPU_RESTORE loads on the way out */
        let n = fpu_len();
        let mut fp = [0u8; 832];
        if uread((*p).vm, fp.as_mut_ptr(), mc.fpstate, n as u64) {
            let area = fpu_area(r as *const Regs);
            /* MXCSR bits the processor does not have would fault in FXRSTOR/XRSTOR */
            let mask = { let m = u32::from_le_bytes([*area.add(28), *area.add(29), *area.add(30), *area.add(31)]); if m == 0 { 0xFFBF } else { m } };
            let mx = u32::from_le_bytes([fp[24], fp[25], fp[26], fp[27]]) & mask;
            fp[24..28].copy_from_slice(&mx.to_le_bytes());
            ptr::copy_nonoverlapping(fp.as_ptr(), area, 512);   /* x87, MXCSR, xmm0-15 */
            if n == 832 {
                ptr::copy_nonoverlapping(fp.as_ptr().add(576), area.add(576), 256);   /* the upper halves of ymm0-15 */
                let bv = area.add(512) as *mut u64;               /* XSTATE_BV: load all three, not the init state */
                ptr::write_unaligned(bv, ptr::read_unaligned(bv) | 7);
            }
        }
    }
    (*th).sigmask = f.uc.uc_sigmask & !SIG_UNBLOCKABLE;
    (*th).sig_suspend = false;
    mc.rax as i64
}

/* ---------------- kill, tkill, tgkill ---------------- */
pub unsafe fn lx_signal_kill(self_: *mut LxProc, pid: c_int, sig: c_int) -> i64 {
    if !(0..=LX_NSIG).contains(&sig) { return -EINVAL; }
    if pid > 0 {
        let t = lx_proc_by_pid(pid);
        if t.is_null() { return -ESRCH; }
        if sig != 0 { lx_signal_raise(t, sig); }
        return 0;
    }
    let mut n = 0;
    let mut t = task_list();
    while !t.is_null() {
        let l = lx_of(t);
        if !l.is_null() {
            let q = (*l).proc_;
            if !(pid == -1 && q == self_) {                /* -1 spares the caller */
                n += 1;
                if sig != 0 { lx_signal_raise(q, sig); }
            }
        }
        t = (*t).next;
    }
    if n != 0 { 0 } else { -ESRCH }
}
pub unsafe fn lx_signal_tkill(tid: c_int, sig: c_int) -> i64 {
    if !(0..=LX_NSIG).contains(&sig) { return -EINVAL; }
    if tid == 0 { return -EINVAL; }
    let t = task_find(tid);
    if t.is_null() || (*t).lx.is_null() { return -ESRCH; }
    if sig != 0 { lx_signal_raise_thread(lx_of(t), sig); }
    0
}
pub unsafe fn lx_signal_tgkill(tgid: c_int, tid: c_int, sig: c_int) -> i64 {
    if !(0..=LX_NSIG).contains(&sig) { return -EINVAL; }
    let t = task_find(tid);
    if t.is_null() || (*t).lx.is_null() { return -ESRCH; }
    let q = (*lx_of(t)).proc_;
    if tgid > 0 && (*q).pid != tgid { return -ESRCH; }
    if sig != 0 { lx_signal_raise_thread(lx_of(t), sig); }
    0
}

/* ---------------- sigpending, sigsuspend, sigtimedwait ---------------- */
pub unsafe fn lx_signal_pending(th: *mut LxThread, uset: u64, setsize: u64) -> i64 {
    if th.is_null() { return -EINVAL; }
    if setsize != 0 && setsize != 8 { return -EINVAL; }
    let set = ((*th).sig_pending | (*(*th).proc_).sig_pending) & (*th).sigmask;
    if uwrite((*(*th).proc_).vm, uset, &set as *const u64 as *const u8, 8) { 0 } else { -EFAULT }
}
pub unsafe fn lx_signal_sigsuspend(uset: u64, setsize: u64) -> i64 {
    let th = lx_current();
    if th.is_null() { return -EINTR; }
    if setsize != 0 && setsize != 8 { return -EINVAL; }
    let mut set = 0u64;
    if !uread((*(*th).proc_).vm, &mut set as *mut u64 as *mut u8, uset, 8) { return -EFAULT; }
    (*th).sig_suspend_old = (*th).sigmask;
    (*th).sig_suspend = true;
    (*th).sigmask = set & !SIG_UNBLOCKABLE;
    loop {
        if (*(*th).proc_).exiting { return -EINTR; }
        if lx_signal_check(th) { return -EINTR; }    /* the handler runs on the way back */
        task_sleep(1);
    }
}
pub unsafe fn lx_signal_sigtimedwait(p: *mut LxProc, uset: u64, uinfo: u64, utime: u64, setsize: u64) -> i64 {
    let th = lx_current();
    if th.is_null() { return -EINVAL; }
    if setsize != 0 && setsize != 8 { return -EINVAL; }
    let mut set = 0u64;
    if !uread((*p).vm, &mut set as *mut u64 as *mut u8, uset, 8) { return -EFAULT; }
    let mut timed = false;
    let mut deadline = 0u32;
    if utime != 0 {
        let mut ts = [0i64; 2];
        if !uread((*p).vm, ts.as_mut_ptr() as *mut u8, utime, 16) { return -EFAULT; }
        if ts[0] < 0 || ts[1] < 0 { return -EINVAL; }
        deadline = timer_ticks().wrapping_add(ts[0].wrapping_mul(1000).wrapping_add(ts[1] / 1000000).min(0x7FFF_FFFF) as u32);
        timed = true;
    }
    loop {
        for sig in 1..=LX_NSIG {
            let bit = sig_bit(sig);
            if set & bit == 0 { continue; }
            let in_thread = (*th).sig_pending & bit != 0;
            if !in_thread && (*p).sig_pending & bit == 0 { continue; }
            if in_thread { (*th).sig_pending &= !bit; } else { (*p).sig_pending &= !bit; }
            if uinfo != 0 {
                let mut si = [0u8; 128];
                fill_info(&mut si, sig, 0, 0, (*p).pid, 0);
                if !uwrite((*p).vm, uinfo, si.as_ptr(), 128) { return 0; }   /* the signal is consumed either way */
            }
            return sig as i64;
        }
        if timed && timer_ticks().wrapping_sub(deadline) as i32 >= 0 { return -EAGAIN; }
        if !timed { return -EAGAIN; }
        if (*p).exiting { return -EINTR; }
        if lx_signal_check(th) { return -EINTR; }
        task_sleep(1);
    }
}
