//! Pipes, sockets, epoll, eventfd and timerfd (was src/linux/ipc.c).
//!
//! Everything here is polled from the cooperative scheduler: a call that would block sleeps
//! for a millisecond and looks again. That is enough for an event loop, and it keeps the
//! kernel free of wait queues for now.
//!
//! AF_UNIX sockets are local to this machine and move messages, not bytes, so SOCK_STREAM
//! reads may return less than was written; SOCK_DGRAM and SOCK_SEQPACKET keep boundaries.
//! AF_INET sockets (TCP and UDP) hand their bytes to the network stack (rust/src/netstack.rs);
//! their ports and addresses are in host order here and converted at the sockaddr boundary.

use super::*;
use super::mem::*;
use super::sig::*;
use super::fs::{lx_fd_get, lx_fd_alloc, lx_file_put, O_NONBLOCK};
use super::sys::{mono_ns, real_ns, tick_at};
use crate::kernel as k;
use crate::netstack::Tcb;
use core::ptr::{self, null_mut};

const PIPE_CAP: u32 = 64 * 1024;
const SOCK_QLEN: u32 = 256 * 1024;
const MAX_MSG_FD: usize = 8;

const O_RDWR_F: c_int = 2;           /* sockets, eventfds, timerfds and epoll fds are read-write, as on Linux */
const O_CLOEXEC_F: c_int = 0o2000000;
const SOCK_NONBLOCK: c_int = 0o4000;
const SOCK_CLOEXEC2: c_int = 0x80000;
const EFD_SEMAPHORE: c_int = 1;

const AF_UNIX: c_int = 1;
const AF_INET: c_int = 2;
const SOCK_STREAM: c_int = 1;
const SOCK_DGRAM: c_int = 2;
const SOCK_SEQPACKET: c_int = 5;
const IPPROTO_TCP: c_int = 6;
const IPPROTO_UDP: c_int = 17;

const POLLIN: u32 = 0x001;
const POLLOUT: u32 = 0x004;
const POLLERR: u32 = 0x008;
const POLLHUP: u32 = 0x010;
const POLLNVAL: u32 = 0x020;

#[repr(C)] struct Obj { refs: c_int }
#[repr(C)] struct Pipe { o: Obj, ring: *mut u8, cap: u32, head: u32, len: u32, readers: c_int, writers: c_int }
#[repr(C)]
struct Msg {
    next: *mut Msg,
    len: u32, off: u32,
    data: *mut u8,
    nfds: c_int,
    fds: [*mut LxFile; MAX_MSG_FD],
    src_ip: u32,                     /* AF_INET datagrams */
    src_port: u16,
}
#[repr(C)]
struct Sock {
    o: Obj,
    ty: c_int,
    bound: bool, listening: bool, connected: bool, peer_gone: bool,
    path: [u8; 108],
    peer: *mut Sock,
    qhead: *mut Msg, qtail: *mut Msg,
    qlen: u32,
    dropped: u64,
    accept_head: *mut Sock, accept_next: *mut Sock,
    reg_next: *mut Sock,
    cred_pid: c_int,                 /* the process this end belongs to, for SO_PEERCRED */
    /* AF_INET: addresses and ports are in host order, like the network stack */
    family: c_int,
    proto: c_int,
    laddr: u32, raddr: u32,
    lport: u16, rport: u16,
    udp_bound: bool,
    reuseaddr: bool, nodelay: bool,
    so_error: c_int,
    tcb: *mut Tcb,                   /* stream socket */
    inet_next: *mut Sock,
}
#[repr(C)] #[derive(Clone, Copy)] struct EpItem { fd: c_int, events: u32, data: u64 }
#[repr(C)] struct Epoll { o: Obj, n: c_int, items: [EpItem; 64] }
#[repr(C)] struct EvFd { o: Obj, count: u64, sem: bool }
#[repr(C)] struct TFd { o: Obj, armed: bool, next: u32, interval: u32, exp: u64, realtime: bool }

static mut BOUND_LIST: *mut Sock = null_mut();
static mut INET_SOCKS: *mut Sock = null_mut();

unsafe fn put<T>(p: *mut LxProc, dst: u64, src: *const T, n: u64) -> bool { uout((*p).vm, dst, src, n) }
unsafe fn get<T>(p: *mut LxProc, dst: *mut T, src: u64, n: u64) -> bool { uin((*p).vm, dst, src, n) }

/* ---------------- wrapping ---------------- */
unsafe fn wrap(ty: c_int, obj: *mut c_void) -> *mut LxFile {
    let f = znew::<LxFile>();
    if f.is_null() { return null_mut(); }
    (*f).ty = ty; (*f).refs = 1; (*f).obj = obj;
    (*(obj as *mut Obj)).refs += 1;
    f
}
unsafe fn install(p: *mut LxProc, f: *mut LxFile, flags: c_int) -> i64 {
    let fd = lx_fd_alloc(p, f, 0);
    if fd < 0 { lx_file_put(f); return fd; }
    if flags & (O_CLOEXEC_F | SOCK_CLOEXEC2) != 0 { (*p).fdflags[fd as usize] = 1; }
    fd
}

/* ---------------- pipes ---------------- */
unsafe fn pipe_new() -> *mut Pipe {
    let pp = znew::<Pipe>();
    if pp.is_null() { return null_mut(); }
    (*pp).ring = kmalloc(PIPE_CAP as usize) as *mut u8;
    if (*pp).ring.is_null() { zfree(pp); return null_mut(); }
    (*pp).cap = PIPE_CAP;
    pp
}
unsafe fn pipe_read(pp: *mut Pipe, d: *mut u8, n: u64, nb: bool) -> i64 {
    let mut got = 0u64;
    if n == 0 { return 0; }
    while got < n {
        while (*pp).len == 0 {
            if (*pp).writers == 0 { return got as i64; }
            if nb { return if got != 0 { got as i64 } else { -EAGAIN }; }
            if interrupted() { return -EINTR; }
            task_sleep(1);
        }
        let mut kk = (n - got).min((*pp).len as u64) as u32;
        let tail = (*pp).cap - (*pp).head;
        if kk > tail { kk = tail; }
        ptr::copy_nonoverlapping((*pp).ring.add((*pp).head as usize), d.add(got as usize), kk as usize);
        (*pp).head = ((*pp).head + kk) % (*pp).cap;
        (*pp).len -= kk;
        got += kk as u64;
    }
    got as i64
}
unsafe fn pipe_write(pp: *mut Pipe, s: *const u8, n: u64, nb: bool) -> i64 {
    let mut done = 0u64;
    while done < n {
        while (*pp).len == (*pp).cap {
            if (*pp).readers == 0 { return if done != 0 { done as i64 } else { -EPIPE }; }
            if nb { return if done != 0 { done as i64 } else { -EAGAIN }; }
            if interrupted() { return -EINTR; }
            task_sleep(1);
        }
        if (*pp).readers == 0 { return if done != 0 { done as i64 } else { -EPIPE }; }
        let widx = ((*pp).head + (*pp).len) % (*pp).cap;
        let mut kk = (n - done).min(((*pp).cap - (*pp).len) as u64) as u32;
        let tail = (*pp).cap - widx;
        if kk > tail { kk = tail; }
        ptr::copy_nonoverlapping(s.add(done as usize), (*pp).ring.add(widx as usize), kk as usize);
        (*pp).len += kk;
        done += kk as u64;
    }
    done as i64
}

/* ---------------- sockets ---------------- */
unsafe fn msg_new(src: *const u8, n: u64) -> *mut Msg {
    let m = znew::<Msg>();
    if m.is_null() { return null_mut(); }
    (*m).data = kmalloc(if n != 0 { n as usize } else { 1 }) as *mut u8;
    if (*m).data.is_null() { zfree(m); return null_mut(); }
    if n != 0 { ptr::copy_nonoverlapping(src, (*m).data, n as usize); }
    (*m).len = n as u32;
    m
}
unsafe fn msg_free(m: *mut Msg) {
    for i in 0..(*m).nfds as usize { if !(*m).fds[i].is_null() { lx_file_put((*m).fds[i]); } }
    if !(*m).data.is_null() { kfree((*m).data as *mut c_void); }
    zfree(m);
}
unsafe fn q_push(s: *mut Sock, m: *mut Msg) {
    (*m).next = null_mut();
    if !(*s).qtail.is_null() { (*(*s).qtail).next = m; } else { (*s).qhead = m; }
    (*s).qtail = m;
    (*s).qlen = (*s).qlen.wrapping_add((*m).len);
}
unsafe fn q_clear(s: *mut Sock) {
    while !(*s).qhead.is_null() { let m = (*s).qhead; (*s).qhead = (*m).next; msg_free(m); }
    (*s).qtail = null_mut(); (*s).qlen = 0;
}
unsafe fn sock_new(ty: c_int) -> *mut Sock {
    let s = znew::<Sock>();
    if s.is_null() { return null_mut(); }
    (*s).ty = ty;
    let t = lx_current();
    (*s).cred_pid = if !t.is_null() && !(*t).proc_.is_null() { (*(*t).proc_).pid } else { 0 };
    s
}
unsafe fn sock_lookup(path: &[u8]) -> *mut Sock {
    let mut s = BOUND_LIST;
    while !s.is_null() { if cstr(&(*s).path) == cstr(path) { return s; } s = (*s).reg_next; }
    null_mut()
}
unsafe fn sock_unregister(s: *mut Sock) {
    let mut pp: *mut *mut Sock = ptr::addr_of_mut!(BOUND_LIST);
    while !(*pp).is_null() { if *pp == s { *pp = (*s).reg_next; break; } pp = ptr::addr_of_mut!((**pp).reg_next); }
}
unsafe fn sock_readable(s: *mut Sock) -> bool {
    if (*s).listening { return !(*s).accept_head.is_null(); }    /* a listening socket: a connection to accept */
    !(*s).qhead.is_null() || (*s).peer_gone || ((*s).peer.is_null() && (*s).connected)
}
unsafe fn sock_writable(s: *mut Sock) -> bool { !(*s).peer.is_null() && !(*s).peer_gone && !(*(*s).peer).peer_gone }
/// A message, or null when the queue is empty. Consumes it unless peek.
unsafe fn sock_take(s: *mut Sock, peek: bool) -> *mut Msg {
    let m = (*s).qhead;
    if m.is_null() { return null_mut(); }
    (*s).qlen = (*s).qlen.wrapping_sub((*m).len - (*m).off);
    if !peek { (*s).qhead = (*m).next; if (*s).qhead.is_null() { (*s).qtail = null_mut(); } }
    m
}
unsafe fn sock_recv(s: *mut Sock, d: *mut u8, n: u64, nb: bool, peek: bool) -> i64 {
    let mut got = 0u64;
    while got < n {
        let m = sock_take(s, peek);
        if m.is_null() {
            if (*s).peer_gone || (*s).peer.is_null() { return got as i64; }
            if nb { return if got != 0 { got as i64 } else { -EAGAIN }; }
            if interrupted() { return -EINTR; }
            task_sleep(1);
            continue;
        }
        let avail = (*m).len - (*m).off;
        let kk = (n - got).min(avail as u64) as u32;
        ptr::copy_nonoverlapping((*m).data.add((*m).off as usize), d.add(got as usize), kk as usize);
        (*m).off += kk;
        got += kk as u64;
        let done = (*m).off >= (*m).len;
        if peek { return kk as i64; }
        if done { msg_free(m); }
        else { (*s).qhead = m; (*s).qlen = (*s).qlen.wrapping_add((*m).len - (*m).off); if (*s).qtail.is_null() { (*s).qtail = m; } }
        if kk == 0 { break; }
    }
    got as i64
}
unsafe fn sock_send(s: *mut Sock, src: *const u8, n: u64, nb: bool, fds: &[*mut LxFile]) -> i64 {
    if (*s).peer.is_null() || (*s).peer_gone { return -EPIPE; }
    while (*(*s).peer).qlen > SOCK_QLEN {
        if nb { return -EAGAIN; }
        if interrupted() { return -EINTR; }
        task_sleep(1);
    }
    let m = msg_new(src, n);
    if m.is_null() { return -ENOMEM; }
    for &f in fds.iter().take(MAX_MSG_FD) { (*m).fds[(*m).nfds as usize] = f; (*m).nfds += 1; }
    q_push((*s).peer, m);
    n as i64
}

/* ---------------- AF_INET sockets ---------------- */
/// Linux sockaddr_in, as it appears in memory: 16 bytes, network byte order.
#[repr(C)] #[derive(Clone, Copy, Default)] struct SockIn { family: u16, port: u16, addr: u32, zero: [u8; 8] }

unsafe fn inet_port_used(port: u16, except: *mut Sock) -> bool {
    let mut o = INET_SOCKS;
    while !o.is_null() { if o != except && (*o).bound && (*o).lport == port { return true; } o = (*o).inet_next; }
    false
}
unsafe fn inet_link(s: *mut Sock) { (*s).inet_next = INET_SOCKS; INET_SOCKS = s; }
unsafe fn inet_unlink(s: *mut Sock) {
    let mut pp: *mut *mut Sock = ptr::addr_of_mut!(INET_SOCKS);
    while !(*pp).is_null() { if *pp == s { *pp = (*s).inet_next; (*s).inet_next = null_mut(); return; } pp = ptr::addr_of_mut!((**pp).inet_next); }
}
unsafe fn inet_new(ty: c_int) -> *mut Sock {
    let s = sock_new(ty);
    if s.is_null() { return null_mut(); }
    (*s).family = AF_INET;
    (*s).proto = if ty == SOCK_STREAM { IPPROTO_TCP } else { IPPROTO_UDP };
    inet_link(s);
    s
}
unsafe fn in_parse(p: *mut LxProc, uaddr: u64, len: c_int, ip: &mut u32, port: &mut u16) -> i64 {
    if len < 8 { return -EINVAL; }
    let mut sa = SockIn::default();
    if !get(p, &mut sa, uaddr, (16).min(len) as u64) { return -EFAULT; }
    if sa.family as c_int != AF_INET { return -EAFNOSUPPORT; }
    *port = u16::from_be(sa.port);
    *ip = u32::from_be(sa.addr);
    0
}
unsafe fn in_put(p: *mut LxProc, uaddr: u64, ualen: u64, ip: u32, port: u16) -> i64 {
    if uaddr == 0 || ualen == 0 { return 0; }
    let mut cap = 0i32;
    if !get(p, &mut cap, ualen, 4) { return -EFAULT; }
    let sa = SockIn { family: AF_INET as u16, port: port.to_be(), addr: ip.to_be(), zero: [0; 8] };
    if !put(p, uaddr, &sa, (cap.max(0) as u64).min(16)) { return -EFAULT; }
    let l = 16i32;
    if put(p, ualen, &l, 4) { 0 } else { -EFAULT }
}
fn inet_map_error(e: c_int) -> c_int { if e == 1 { 111 } else if e == 2 { 104 } else { 110 } }   /* ECONNREFUSED, ECONNRESET, ETIMEDOUT */

/// A connecting socket becomes usable as soon as the stack says so.
unsafe fn inet_sync(s: *mut Sock) {
    if (*s).family != AF_INET || (*s).ty != SOCK_STREAM || (*s).tcb.is_null() || (*s).connected { return; }
    let st = k::tcp_status((*s).tcb);
    if st == 1 { (*s).connected = true; (*s).lport = k::tcp_local_port((*s).tcb); }
    else if st < 0 && (*s).so_error == 0 { (*s).so_error = inet_map_error(k::tcp_error((*s).tcb)); }
}
/// Datagrams arrive on the network task; queue them for recv().
unsafe extern "C" fn inet_udp_input(src: u32, sport: u16, data: *const u8, len: u32, ctx: *mut c_void) {
    let s = ctx as *mut Sock;
    if s.is_null() || (*s).family != AF_INET || (*s).ty != SOCK_DGRAM { return; }
    if (*s).qlen.wrapping_add(len) > SOCK_QLEN { (*s).dropped += 1; return; }
    let m = msg_new(data, len as u64);
    if m.is_null() { (*s).dropped += 1; return; }
    (*m).src_ip = src; (*m).src_port = sport;
    q_push(s, m);
}
unsafe fn inet_auto_bind(s: *mut Sock) -> i64 {
    if (*s).family != AF_INET || (*s).bound { return 0; }
    for _ in 0..128 {
        let port = k::net_ephemeral_port();
        if inet_port_used(port, s) { continue; }
        if (*s).ty == SOCK_DGRAM {
            if k::udp_bind(port, Some(inet_udp_input), s as *mut c_void) == 0 { continue; }
            (*s).udp_bound = true;
        }
        (*s).lport = port; (*s).bound = true;
        return 0;
    }
    -EADDRINUSE
}
unsafe fn inet_udp_send(s: *mut Sock, buf: *const u8, n: u64, mut dip: u32, mut dport: u16, have_dest: bool) -> i64 {
    if !have_dest {
        if !(*s).connected { return -EDESTADDRREQ; }
        dip = (*s).raddr; dport = (*s).rport;
    }
    if n > 1472 { return -EMSGSIZE; }                    /* no IP fragmentation in this stack */
    let r = inet_auto_bind(s);
    if r < 0 { return r; }
    if k::udp_send(dip, (*s).lport, dport, buf, n as u32) == 0 { return -ENOBUFS; }
    n as i64
}
unsafe fn inet_tcp_send(s: *mut Sock, buf: *const u8, n: u64, nb: bool) -> i64 {
    if (*s).tcb.is_null() { return -ENOTCONN; }
    inet_sync(s);
    if k::tcp_status((*s).tcb) == 0 {
        if nb { return -EAGAIN; }
        while k::tcp_status((*s).tcb) == 0 {
            if interrupted() { return -EINTR; }
            task_sleep(1);
        }
        inet_sync(s);
    }
    if k::tcp_status((*s).tcb) < 0 { return if (*s).so_error != 0 { -((*s).so_error as i64) } else { -ECONNRESET }; }
    let mut done = 0u64;
    while done < n {
        let r = k::tcp_send_nb((*s).tcb, buf.add(done as usize), (n - done).min(0x7FFF_FFFF) as u32);
        if r < 0 { return if done != 0 { done as i64 } else if (*s).so_error != 0 { -((*s).so_error as i64) } else { -EPIPE }; }
        if r == 0 {
            if nb { return if done != 0 { done as i64 } else { -EAGAIN }; }
            if interrupted() { return if done != 0 { done as i64 } else { -EINTR }; }
            task_sleep(1);
            continue;
        }
        done += r as u64;
    }
    done as i64
}
unsafe fn inet_tcp_recv(s: *mut Sock, buf: *mut u8, n: u64, peek: bool) -> i64 {
    if (*s).tcb.is_null() { return -ENOTCONN; }
    inet_sync(s);
    if k::tcp_status((*s).tcb) < 0 && k::tcp_recv_ready((*s).tcb) == 0 {
        return if (*s).so_error != 0 { -((*s).so_error as i64) } else { -ECONNRESET };
    }
    let m = n.min(0x7FFF_FFFF) as u32;
    let r = if peek { k::tcp_peek((*s).tcb, buf, m) } else { k::tcp_recv((*s).tcb, buf, m, 0) };
    if r == -2 { return -EAGAIN; }
    if r < 0 {
        let e = k::tcp_error((*s).tcb);
        (*s).so_error = if e != 0 { inet_map_error(e) } else { ECONNRESET as c_int };
        return -((*s).so_error as i64);
    }
    r as i64
}
unsafe fn inet_udp_recv(s: *mut Sock, buf: *mut u8, n: u64, peek: bool, sip: &mut u32, sport: &mut u16, full: &mut u32) -> i64 {
    let m = (*s).qhead;
    if m.is_null() { return -EAGAIN; }
    let len = (*m).len;
    let kk = n.min(len as u64);
    if kk != 0 { ptr::copy_nonoverlapping((*m).data, buf, kk as usize); }
    *sip = (*m).src_ip; *sport = (*m).src_port; *full = len;
    if !peek {
        (*s).qhead = (*m).next;
        if (*s).qhead.is_null() { (*s).qtail = null_mut(); }
        (*s).qlen = (*s).qlen.wrapping_sub(len);
        msg_free(m);
    }
    kk as i64
}
unsafe fn inet_read(s: *mut Sock, buf: *mut u8, n: u64, nb: bool, sip: &mut u32, sport: &mut u16, full: &mut u32, peek: bool) -> i64 {
    if n == 0 { return 0; }
    loop {
        let r = if (*s).ty == SOCK_DGRAM { inet_udp_recv(s, buf, n, peek, sip, sport, full) } else { inet_tcp_recv(s, buf, n, peek) };
        if r != -EAGAIN { return r; }
        if nb { return -EAGAIN; }
        if interrupted() { return -EINTR; }
        task_sleep(1);
    }
}
unsafe fn inet_write(s: *mut Sock, buf: *const u8, n: u64, nb: bool, dip: u32, dport: u16, have_dest: bool) -> i64 {
    if (*s).ty == SOCK_DGRAM { return inet_udp_send(s, buf, n, dip, dport, have_dest); }
    if have_dest { return -EISCONN; }
    inet_tcp_send(s, buf, n, nb)
}

/* ---------------- eventfd and timerfd ---------------- */
unsafe fn tfd_tick(t: *mut TFd) {
    if !(*t).armed { return; }
    let now = timer_ticks();
    if (now.wrapping_sub((*t).next) as i32) < 0 { return; }
    let iv = if (*t).interval != 0 { (*t).interval } else { 1 };
    let laps = 1 + now.wrapping_sub((*t).next) / iv;
    (*t).exp = (*t).exp.wrapping_add(laps as u64);
    if (*t).interval != 0 { (*t).next = (*t).next.wrapping_add(laps.wrapping_mul((*t).interval)); }
    else { (*t).armed = false; }
}

/* ---------------- readiness ---------------- */
pub unsafe fn lx_ipc_ready(f: *mut LxFile, want: u32) -> bool {
    if f.is_null() { return false; }
    match (*f).ty {
        FD_PIPE => {
            let pp = (*f).obj as *mut Pipe;
            if want & POLLIN != 0 { return (*pp).len > 0 || (*pp).writers == 0; }
            if want & POLLOUT != 0 { return (*pp).readers > 0 && (*pp).len < (*pp).cap; }
            false
        }
        FD_SOCKET => {
            let s = (*f).obj as *mut Sock;
            if (*s).family == AF_INET {
                inet_sync(s);
                if want & POLLIN != 0 {
                    if (*s).ty == SOCK_DGRAM { return !(*s).qhead.is_null(); }
                    if (*s).listening { return k::tcp_accept_ready((*s).lport) != 0; }
                    if (*s).tcb.is_null() { return true; }        /* unconnected: read reports ENOTCONN */
                    if !(*s).connected { return k::tcp_status((*s).tcb) != 0; }   /* failed connect is readable */
                    return k::tcp_recv_ready((*s).tcb) != 0;
                }
                if want & POLLOUT != 0 {
                    if (*s).ty == SOCK_DGRAM { return true; }
                    if (*s).tcb.is_null() { return false; }
                    if !(*s).connected { return k::tcp_status((*s).tcb) != 0; }
                    return k::tcp_send_ready((*s).tcb) != 0;
                }
                if want & POLLERR != 0 { return (*s).so_error != 0 || (!(*s).tcb.is_null() && k::tcp_status((*s).tcb) < 0); }
                if want & POLLHUP != 0 { return !(*s).tcb.is_null() && k::tcp_hup((*s).tcb) != 0; }
                return false;
            }
            if want & POLLIN != 0 { return sock_readable(s); }
            if want & POLLOUT != 0 { return sock_writable(s) && (*s).qlen < SOCK_QLEN; }
            if want & POLLHUP != 0 { return (*s).peer_gone; }
            false
        }
        FD_EVENTFD => { let e = (*f).obj as *mut EvFd; if want & POLLIN != 0 { return (*e).count > 0; } if want & POLLOUT != 0 { return (*e).count < !0u64 - 1; } false }
        FD_TIMERFD => { let t = (*f).obj as *mut TFd; tfd_tick(t); want & POLLIN != 0 && (*t).exp > 0 }
        FD_WINDOW => want & POLLIN != 0 && super::win::lxwin_ready((*f).obj),
        FD_DSP => want & POLLOUT != 0 && super::dsp::dsp_writable((*f).obj),
        _ => false,
    }
}

pub unsafe fn lx_ipc_read(f: *mut LxFile, dst: *mut u8, n: u64, nonblock: bool) -> i64 {
    match (*f).ty {
        FD_PIPE => pipe_read((*f).obj as *mut Pipe, dst, n, nonblock),
        FD_SOCKET => {
            let s = (*f).obj as *mut Sock;
            if (*s).family == AF_INET { let (mut a, mut b, mut c) = (0, 0, 0); return inet_read(s, dst, n, nonblock, &mut a, &mut b, &mut c, false); }
            sock_recv(s, dst, n, nonblock, false)
        }
        FD_EVENTFD => {
            let e = (*f).obj as *mut EvFd;
            if n < 8 { return -EINVAL; }
            while (*e).count == 0 {
                if nonblock { return -EAGAIN; }
                if interrupted() { return -EINTR; }
                task_sleep(1);
            }
            let v = if (*e).sem { 1 } else { (*e).count };
            if (*e).sem { (*e).count -= 1; } else { (*e).count = 0; }
            ptr::write_unaligned(dst as *mut u64, v);
            8
        }
        FD_TIMERFD => {
            let t = (*f).obj as *mut TFd;
            if n < 8 { return -EINVAL; }
            loop { tfd_tick(t); if (*t).exp != 0 { break; } if nonblock { return -EAGAIN; } if interrupted() { return -EINTR; } task_sleep(1); }
            let v = (*t).exp; (*t).exp = 0;
            ptr::write_unaligned(dst as *mut u64, v);
            8
        }
        _ => -ENOTTY,
    }
}
pub unsafe fn lx_ipc_write(f: *mut LxFile, src: *const u8, n: u64, nonblock: bool) -> i64 {
    let r = match (*f).ty {
        FD_PIPE => pipe_write((*f).obj as *mut Pipe, src, n, nonblock),
        FD_SOCKET => {
            let s = (*f).obj as *mut Sock;
            if (*s).family == AF_INET { inet_write(s, src, n, nonblock, 0, 0, false) } else { sock_send(s, src, n, nonblock, &[]) }
        }
        FD_EVENTFD => {
            let e = (*f).obj as *mut EvFd;
            if n != 8 { return -EINVAL; }
            let v = ptr::read_unaligned(src as *const u64);
            if v == !0 { return -EINVAL; }
            if (*e).count.wrapping_add(v) < (*e).count { return -EAGAIN; }
            (*e).count += v;
            return 8;
        }
        FD_TIMERFD => return -EINVAL,
        _ => return -ENOTTY,
    };
    if r == -EPIPE {                                     /* a pipe or socket with nobody on the other end */
        let t = lx_current();
        if !t.is_null() { lx_signal_raise((*t).proc_, SIGPIPE); }
    }
    r
}

pub unsafe fn lx_ipc_release(f: *mut LxFile) {
    if f.is_null() || (*f).obj.is_null() { return; }
    let o = (*f).obj as *mut Obj;
    match (*f).ty {
        FD_PIPE => {
            let pp = (*f).obj as *mut Pipe;
            let acc = (*f).oflags & 3;
            if acc == 0 { if (*pp).readers > 0 { (*pp).readers -= 1; } }
            else if acc == 1 { if (*pp).writers > 0 { (*pp).writers -= 1; } }
            else { if (*pp).readers > 0 { (*pp).readers -= 1; } if (*pp).writers > 0 { (*pp).writers -= 1; } }
            (*o).refs -= 1;
            if (*o).refs <= 0 { kfree((*pp).ring as *mut c_void); zfree(pp); }
        }
        FD_SOCKET => {
            let s = (*f).obj as *mut Sock;
            if (*s).family == AF_INET {
                inet_unlink(s);
                if (*s).ty == SOCK_DGRAM {
                    if (*s).udp_bound { k::udp_unbind((*s).lport); (*s).udp_bound = false; }
                } else {
                    if (*s).listening { k::tcp_unlisten((*s).lport); (*s).listening = false; }
                    if !(*s).tcb.is_null() { k::tcp_close((*s).tcb); (*s).tcb = null_mut(); }
                }
                q_clear(s);
                (*o).refs -= 1;
                if (*o).refs <= 0 { zfree(s); }
                return;
            }
            if (*s).bound { sock_unregister(s); (*s).bound = false; }
            if !(*s).peer.is_null() && (*(*s).peer).peer == s { (*(*s).peer).peer = null_mut(); (*(*s).peer).peer_gone = true; }
            q_clear(s);
            (*o).refs -= 1;
            if (*o).refs <= 0 { zfree(s); }
        }
        _ => { (*o).refs -= 1; if (*o).refs <= 0 { zfree(o); } }
    }
}

/* ---------------- syscalls ---------------- */
#[repr(C, packed)] #[derive(Clone, Copy, Default)]
struct MsgHdr { name: u64, namelen: u32, pad: u32, iov: u64, iovlen: u64, control: u64, controllen: u64, flags: i32 }
#[repr(C)] #[derive(Clone, Copy)] struct SockUn { family: u16, path: [u8; 108] }
impl Default for SockUn { fn default() -> Self { SockUn { family: 0, path: [0; 108] } } }
const SOL_SOCKET: i32 = 1;
const SCM_RIGHTS: i32 = 1;
const MSG_CTRUNC: i32 = 8;
const MSG_PEEK: c_int = 2;
const MSG_DONTWAIT: c_int = 0x40;
const MSG_TRUNC: i32 = 0x20;
fn cmsg_align(n: u64) -> u64 { n.wrapping_add(7) & !7 }

unsafe fn sock_fd(p: *mut LxProc, fd: c_int) -> *mut Sock {
    let f = lx_fd_get(p, fd as i64);
    if !f.is_null() && (*f).ty == FD_SOCKET { (*f).obj as *mut Sock } else { null_mut() }
}
unsafe fn self_nb(p: *mut LxProc, fd: c_int) -> bool { (*lx_fd_get(p, fd as i64)).oflags & O_NONBLOCK != 0 }

unsafe fn do_socket(p: *mut LxProc, domain: c_int, ty: c_int, proto: c_int) -> i64 {
    let base = ty & 0xF;
    if domain == AF_UNIX {
        if proto != 0 { return -EPROTONOSUPPORT; }
        if base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_SEQPACKET { return -EPROTOTYPE; }
        let s = sock_new(base);
        if s.is_null() { return -ENOMEM; }
        let f = wrap(FD_SOCKET, s as *mut c_void);
        if f.is_null() { zfree(s); return -ENOMEM; }
        (*f).oflags = O_RDWR_F | if ty & SOCK_NONBLOCK != 0 { O_NONBLOCK } else { 0 };
        return install(p, f, ty);
    }
    if domain == AF_INET {
        if base != SOCK_STREAM && base != SOCK_DGRAM { return -EPROTOTYPE; }
        let want = if base == SOCK_STREAM { IPPROTO_TCP } else { IPPROTO_UDP };
        if proto != 0 && proto != want { return -EPROTONOSUPPORT; }
        let s = inet_new(base);
        if s.is_null() { return -ENOMEM; }
        (*s).proto = if proto != 0 { proto } else { want };
        let f = wrap(FD_SOCKET, s as *mut c_void);
        if f.is_null() { inet_unlink(s); zfree(s); return -ENOMEM; }
        (*f).oflags = O_RDWR_F | if ty & SOCK_NONBLOCK != 0 { O_NONBLOCK } else { 0 };
        return install(p, f, ty);
    }
    -EAFNOSUPPORT
}
unsafe fn do_socketpair(p: *mut LxProc, domain: c_int, ty: c_int, proto: c_int, usv: u64) -> i64 {
    if domain != AF_UNIX { return -EAFNOSUPPORT; }
    if proto != 0 { return -EPROTONOSUPPORT; }
    let base = ty & 0xF;
    if base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_SEQPACKET { return -EPROTOTYPE; }
    let (a, b) = (sock_new(base), sock_new(base));
    if a.is_null() || b.is_null() { zfree(a); zfree(b); return -ENOMEM; }
    (*a).peer = b; (*b).peer = a;
    (*a).connected = true; (*b).connected = true;
    let (fa, fb) = (wrap(FD_SOCKET, a as *mut c_void), wrap(FD_SOCKET, b as *mut c_void));
    if fa.is_null() || fb.is_null() {
        if !fa.is_null() { lx_file_put(fa); } else { zfree(a); }
        if !fb.is_null() { lx_file_put(fb); } else { zfree(b); }
        return -ENOMEM;
    }
    let fl = O_RDWR_F | if ty & SOCK_NONBLOCK != 0 { O_NONBLOCK } else { 0 };
    (*fa).oflags = fl; (*fb).oflags = fl;
    let (fd0, fd1) = (install(p, fa, ty), install(p, fb, ty));
    if fd0 < 0 || fd1 < 0 {
        if fd0 >= 0 { lx_file_put((*p).fds[fd0 as usize]); (*p).fds[fd0 as usize] = null_mut(); }
        if fd1 >= 0 { lx_file_put((*p).fds[fd1 as usize]); (*p).fds[fd1 as usize] = null_mut(); }
        return -EMFILE;
    }
    let sv: [i32; 2] = [fd0 as i32, fd1 as i32];
    if !put(p, usv, &sv, 8) { return -EFAULT; }
    0
}
unsafe fn sun_parse(p: *mut LxProc, uaddr: u64, len: c_int) -> Result<[u8; 108], i64> {
    let mut sa = SockUn::default();
    if len < 2 || len > 110 { return Err(-EINVAL); }
    if !get(p, &mut sa, uaddr, len as u64) { return Err(-EFAULT); }
    if sa.family as c_int != AF_UNIX { return Err(-EAFNOSUPPORT); }
    sa.path[107] = 0;
    Ok(sa.path)
}
unsafe fn do_bind(p: *mut LxProc, fd: c_int, uaddr: u64, len: c_int) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    if (*s).bound { return -EINVAL; }
    if (*s).family == AF_INET {
        let (mut ip, mut port) = (0u32, 0u16);
        let r = in_parse(p, uaddr, len, &mut ip, &mut port);
        if r < 0 { return r; }
        let me = (*ptr::addr_of!(k::netif)).ip;
        if ip != 0 && me != 0 && ip != me && ip != 0x7F00_0001 { return -EADDRNOTAVAIL; }
        if port == 0 {
            let mut i = 0;
            while i < 128 && port == 0 { let cand = k::net_ephemeral_port(); if !inet_port_used(cand, s) { port = cand; } i += 1; }
            if port == 0 { return -EADDRINUSE; }
        } else if inet_port_used(port, s) && !(*s).reuseaddr { return -EADDRINUSE; }
        if (*s).ty == SOCK_DGRAM {
            if k::udp_bind(port, Some(inet_udp_input), s as *mut c_void) == 0 { return -EADDRINUSE; }
            (*s).udp_bound = true;
        }
        (*s).laddr = ip; (*s).lport = port; (*s).bound = true;
        return 0;
    }
    let path = match sun_parse(p, uaddr, len) { Ok(v) => v, Err(e) => return e };
    if path[0] == 0 { return -EADDRINUSE; }                     /* abstract names are not supported */
    if !sock_lookup(&path).is_null() { return -EADDRINUSE; }
    setstr(&mut (*s).path, &path);
    (*s).bound = true;
    (*s).reg_next = BOUND_LIST;
    BOUND_LIST = s;
    0
}
unsafe fn do_listen(p: *mut LxProc, fd: c_int) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    if (*s).family == AF_INET {
        if (*s).ty != SOCK_STREAM { return -EOPNOTSUPP; }
        if (*s).listening { return 0; }
        if !(*s).bound { let r = inet_auto_bind(s); if r < 0 { return r; } }
        if k::tcp_listen((*s).lport) != 0 { return -EADDRINUSE; }
        (*s).listening = true;
        return 0;
    }
    if !(*s).bound { return -EINVAL; }
    (*s).listening = true;
    0
}
unsafe fn do_connect(p: *mut LxProc, fd: c_int, uaddr: u64, len: c_int) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    if (*s).family == AF_INET {
        let (mut ip, mut port) = (0u32, 0u16);
        let r = in_parse(p, uaddr, len, &mut ip, &mut port);
        if r < 0 { return r; }
        if port == 0 { return -ECONNREFUSED; }
        if ip == 0 { return -EADDRNOTAVAIL; }
        if (*s).ty == SOCK_DGRAM { (*s).raddr = ip; (*s).rport = port; (*s).connected = true; return 0; }
        if (*s).connected { return -EISCONN; }
        if !(*s).tcb.is_null() { return -EALREADY; }
        let nb = self_nb(p, fd);
        if k::net_ready() == 0 {
            if !nb && k::net_wait_ready(8000) == 0 { return -ENETUNREACH; }
            if nb { return -ENETUNREACH; }
        }
        if !(*s).bound { let r = inet_auto_bind(s); if r < 0 { return r; } }   /* reserve the local port once */
        let t = k::tcp_open_from(ip, port, (*s).lport);
        if t.is_null() { return -ENOMEM; }
        (*s).tcb = t; (*s).raddr = ip; (*s).rport = port; (*s).so_error = 0;
        (*s).lport = k::tcp_local_port(t);
        if nb { return -EINPROGRESS; }
        let t0 = timer_ticks();
        while k::tcp_status(t) == 0 {
            if interrupted() { k::tcp_close(t); (*s).tcb = null_mut(); return -EINTR; }
            if timer_ticks().wrapping_sub(t0) > 30000 { k::tcp_close(t); (*s).tcb = null_mut(); return -ETIMEDOUT; }
            task_sleep(2);
        }
        if k::tcp_status(t) < 0 {
            let e = inet_map_error(k::tcp_error(t));
            k::tcp_close(t); (*s).tcb = null_mut();
            return -(e as i64);
        }
        (*s).connected = true;
        (*s).lport = k::tcp_local_port(t);
        return 0;
    }
    if (*s).connected { return -EISCONN; }
    let path = match sun_parse(p, uaddr, len) { Ok(v) => v, Err(e) => return e };
    let l = sock_lookup(&path);
    if l.is_null() || !(*l).listening { return -ECONNREFUSED; }
    let srv = sock_new((*s).ty);
    if srv.is_null() { return -ENOMEM; }
    (*srv).connected = true; (*s).connected = true;
    (*srv).peer = s; (*s).peer = srv;
    (*srv).cred_pid = (*l).cred_pid;                  /* the accepted end belongs to the listening process */
    (*srv).accept_next = null_mut();
    if (*l).accept_head.is_null() { (*l).accept_head = srv; }
    else { let mut q = (*l).accept_head; while !(*q).accept_next.is_null() { q = (*q).accept_next; } (*q).accept_next = srv; }
    0
}
unsafe fn put_sun(p: *mut LxProc, uaddr: u64, ualen: u64, src: *mut Sock) {
    let mut sa = SockUn { family: AF_UNIX as u16, path: [0; 108] };
    if !src.is_null() { setstr(&mut sa.path, &(*src).path); }
    let l = 110i32;
    put(p, uaddr, &sa, 110);
    put(p, ualen, &l, 4);
}
unsafe fn do_accept(p: *mut LxProc, fd: c_int, uaddr: u64, ualen: u64, flags: c_int) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    let nb = flags & SOCK_NONBLOCK != 0 || self_nb(p, fd);
    loop {
        if (*s).family == AF_INET {
            if !(*s).listening { return -EINVAL; }
            let c = k::tcp_accept((*s).lport);
            if !c.is_null() {
                let ns = inet_new(SOCK_STREAM);
                if ns.is_null() { k::tcp_close(c); return -ENOMEM; }
                (*ns).tcb = c; (*ns).connected = true;
                (*ns).laddr = (*s).laddr; (*ns).lport = (*s).lport;
                (*ns).raddr = k::tcp_remote_ip(c); (*ns).rport = k::tcp_remote_port(c);
                let f = wrap(FD_SOCKET, ns as *mut c_void);
                if f.is_null() { inet_unlink(ns); zfree(ns); k::tcp_close(c); return -ENOMEM; }
                (*f).oflags = O_RDWR_F | if flags & SOCK_NONBLOCK != 0 { O_NONBLOCK } else { 0 };
                let nfd = install(p, f, flags);
                if nfd < 0 { return nfd; }
                let rr = in_put(p, uaddr, ualen, (*ns).raddr, (*ns).rport);
                if rr < 0 { return rr; }
                return nfd;
            }
        } else if !(*s).accept_head.is_null() {
            let c = (*s).accept_head;
            (*s).accept_head = (*c).accept_next;
            (*c).accept_next = null_mut();
            let f = wrap(FD_SOCKET, c as *mut c_void);
            if f.is_null() { return -ENOMEM; }
            (*f).oflags = O_RDWR_F | if flags & SOCK_NONBLOCK != 0 { O_NONBLOCK } else { 0 };
            let nfd = install(p, f, flags);
            if nfd < 0 { return nfd; }
            if uaddr != 0 && ualen != 0 { put_sun(p, uaddr, ualen, (*c).peer); }
            return nfd;
        }
        if !(*s).listening { return -EINVAL; }
        if nb { return -EAGAIN; }
        if interrupted() { return -EINTR; }
        task_sleep(1);
    }
}
unsafe fn do_sendto(p: *mut LxProc, fd: c_int, buf: u64, mut n: u64, flags: c_int, uaddr: u64, ulen: u64) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    let nb = flags & MSG_DONTWAIT != 0 || self_nb(p, fd);
    if (*s).ty == SOCK_STREAM { if n > SOCK_QLEN as u64 { n = SOCK_QLEN as u64; } }    /* a stream write may be short */
    else if n > SOCK_QLEN as u64 { return -EMSGSIZE; }
    let tmp = kmalloc(if n != 0 { n as usize } else { 1 }) as *mut u8;
    if tmp.is_null() { return -ENOMEM; }
    if n != 0 && !get(p, tmp, buf, n) { kfree(tmp as *mut c_void); return -EFAULT; }
    let r;
    if (*s).family == AF_INET {
        let (mut dip, mut dport) = (0u32, 0u16);
        let have = uaddr != 0 && ulen != 0;
        if have { let e = in_parse(p, uaddr, ulen as c_int, &mut dip, &mut dport); if e < 0 { kfree(tmp as *mut c_void); return e; } }
        r = inet_write(s, tmp, n, nb, dip, dport, have);
    } else { r = sock_send(s, tmp, n, nb, &[]); }
    kfree(tmp as *mut c_void);
    r
}
unsafe fn do_recvfrom(p: *mut LxProc, fd: c_int, buf: u64, n: u64, flags: c_int, uaddr: u64, ualen: u64) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    let nb = flags & MSG_DONTWAIT != 0 || self_nb(p, fd);
    let want = n.min(SOCK_QLEN as u64);
    let tmp = kmalloc(if want != 0 { want as usize } else { 1 }) as *mut u8;
    if tmp.is_null() { return -ENOMEM; }
    if (*s).family == AF_INET {
        let (mut sip, mut full, mut sport) = ((*s).raddr, 0u32, (*s).rport);
        let r = inet_read(s, tmp, want, nb, &mut sip, &mut sport, &mut full, flags & MSG_PEEK != 0);
        if r > 0 && !put(p, buf, tmp, r as u64) { kfree(tmp as *mut c_void); return -EFAULT; }
        kfree(tmp as *mut c_void);
        if r >= 0 && uaddr != 0 && ualen != 0 { let e = in_put(p, uaddr, ualen, sip, sport); if e < 0 { return e; } }
        if flags & MSG_TRUNC != 0 && (*s).ty == SOCK_DGRAM && r >= 0 { return full as i64; }
        return r;
    }
    let r = sock_recv(s, tmp, want, nb, flags & MSG_PEEK != 0);
    if r > 0 && !put(p, buf, tmp, r as u64) { kfree(tmp as *mut c_void); return -EFAULT; }
    kfree(tmp as *mut c_void);
    if r >= 0 && uaddr != 0 && ualen != 0 { put_sun(p, uaddr, ualen, (*s).peer); }
    r
}
unsafe fn do_sendmsg(p: *mut LxProc, fd: c_int, umsg: u64, flags: c_int) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    let mut mh = MsgHdr::default();
    if !get(p, &mut mh, umsg, 52) { return -EFAULT; }
    let (iov, iovlen) = (mh.iov, mh.iovlen);
    let mut total = 0u64;
    let mut i = 0u64;
    while i < iovlen && i < 1024 {
        let mut v = [0u64; 2];
        if !get(p, &mut v, iov.wrapping_add(i * 16), 16) { return -EFAULT; }
        if v[1] > SOCK_QLEN as u64 || total + v[1] > SOCK_QLEN as u64 {
            if (*s).ty == SOCK_STREAM { break; }                 /* a stream write may be short */
            return -EMSGSIZE;
        }
        total += v[1];
        i += 1;
    }
    let buf = kmalloc(if total != 0 { total as usize } else { 1 }) as *mut u8;
    if buf.is_null() { return -ENOMEM; }
    let mut o = 0u64;
    let mut i = 0u64;
    while i < iovlen && i < 1024 && o < total {
        let mut v = [0u64; 2];
        if !get(p, &mut v, iov + i * 16, 16) { kfree(buf as *mut c_void); return -EFAULT; }
        let kk = v[1].min(total - o);
        if kk != 0 && !get(p, buf.add(o as usize), v[0], kk) { kfree(buf as *mut c_void); return -EFAULT; }
        o += kk;
        i += 1;
    }
    let mut fds: [*mut LxFile; MAX_MSG_FD] = [null_mut(); MAX_MSG_FD];
    let mut nfds = 0usize;
    let (control, controllen) = (mh.control, mh.controllen);
    if control != 0 && controllen != 0 {
        let mut ctl = [0u8; 256];
        let cl = controllen.min(256);
        if !get(p, ctl.as_mut_ptr(), control, cl) { kfree(buf as *mut c_void); return -EFAULT; }
        let mut off = 0u64;
        while off + 16 <= cl {
            let c = ctl.as_ptr().add(off as usize);
            let clen = ptr::read_unaligned(c as *const u64);
            let level = ptr::read_unaligned(c.add(8) as *const i32);
            let cty = ptr::read_unaligned(c.add(12) as *const i32);
            if clen < 16 || off + clen > cl { break; }
            if level == SOL_SOCKET && cty == SCM_RIGHTS {
                let cnt = ((clen - 16) / 4) as usize;
                for i in 0..cnt {
                    if nfds >= MAX_MSG_FD { break; }
                    let fdn = ptr::read_unaligned(c.add(16 + i * 4) as *const i32);
                    let f = lx_fd_get(p, fdn as i64);
                    if f.is_null() { break; }
                    (*f).refs += 1;
                    fds[nfds] = f; nfds += 1;
                }
            }
            off += cmsg_align(clen);
        }
    }
    let nb = flags & MSG_DONTWAIT != 0 || self_nb(p, fd);
    if (*s).family == AF_INET {
        for &f in &fds[..nfds] { lx_file_put(f); }
        let (mut dip, mut dport) = (0u32, 0u16);
        let have = mh.name != 0 && mh.namelen != 0;
        if have { let e = in_parse(p, mh.name, mh.namelen as c_int, &mut dip, &mut dport); if e < 0 { kfree(buf as *mut c_void); return e; } }
        let ir = inet_write(s, buf, total, nb, dip, dport, have);
        kfree(buf as *mut c_void);
        return ir;
    }
    let r = sock_send(s, buf, total, nb, &fds[..nfds]);
    if r < 0 { for &f in &fds[..nfds] { lx_file_put(f); } }
    kfree(buf as *mut c_void);
    r
}
unsafe fn do_recvmsg(p: *mut LxProc, fd: c_int, umsg: u64, flags: c_int) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    let mut mh = MsgHdr::default();
    if !get(p, &mut mh, umsg, 52) { return -EFAULT; }
    let nb = flags & MSG_DONTWAIT != 0 || self_nb(p, fd);
    let (iov, iovlen) = (mh.iov, mh.iovlen);
    if (*s).family == AF_INET {
        let mut cap = 0u64;
        let mut i = 0u64;
        while i < iovlen && i < 1024 {
            let mut v = [0u64; 2];
            if !get(p, &mut v, iov.wrapping_add(i * 16), 16) { return -EFAULT; }
            if v[1] > SOCK_QLEN as u64 || cap + v[1] > SOCK_QLEN as u64 { if (*s).ty == SOCK_STREAM { break; } return -EMSGSIZE; }
            cap += v[1];
            i += 1;
        }
        let buf = kmalloc(if cap != 0 { cap as usize } else { 1 }) as *mut u8;
        if buf.is_null() { return -ENOMEM; }
        let (mut sip, mut full, mut sport) = ((*s).raddr, 0u32, (*s).rport);
        let r = inet_read(s, buf, cap, nb, &mut sip, &mut sport, &mut full, flags & MSG_PEEK != 0);
        if r < 0 { kfree(buf as *mut c_void); return r; }
        let mut copied = 0u64;
        let mut i = 0u64;
        while i < iovlen && i < 1024 && copied < r as u64 {
            let mut v = [0u64; 2];
            if !get(p, &mut v, iov + i * 16, 16) { kfree(buf as *mut c_void); return -EFAULT; }
            let kk = v[1].min(r as u64 - copied) as u32;
            if kk != 0 && !put(p, v[0], buf.add(copied as usize), kk as u64) { kfree(buf as *mut c_void); return -EFAULT; }
            copied += kk as u64;
            i += 1;
        }
        kfree(buf as *mut c_void);
        mh.flags = if (*s).ty == SOCK_DGRAM && full as u64 > copied { MSG_TRUNC } else { 0 };
        mh.controllen = 0;
        if mh.name != 0 && mh.namelen != 0 {
            let sa = SockIn { family: AF_INET as u16, port: sport.to_be(), addr: sip.to_be(), zero: [0; 8] };
            let kk = (mh.namelen as u64).min(16);
            if !put(p, mh.name, &sa, kk) { return -EFAULT; }
            mh.namelen = 16;
        }
        if !put(p, umsg, &mh, 52) { return -EFAULT; }
        if flags & MSG_TRUNC != 0 && (*s).ty == SOCK_DGRAM { return full as i64; }
        return r;
    }
    loop {
        if !(*s).qhead.is_null() { break; }
        if (*s).peer_gone || (*s).peer.is_null() { mh.flags = 0; mh.controllen = 0; put(p, umsg, &mh, 52); return 0; }
        if nb { return -EAGAIN; }
        if interrupted() { return -EINTR; }
        task_sleep(1);
    }
    let m = (*s).qhead;
    let peek = flags & MSG_PEEK != 0;
    let avail = (*m).len - (*m).off;
    let mut copied = 0u32;
    let mut i = 0u64;
    while i < iovlen && i < 1024 {
        let mut v = [0u64; 2];
        if !get(p, &mut v, iov.wrapping_add(i * 16), 16) { return -EFAULT; }
        if copied >= avail { break; }
        let kk = v[1].min((avail - copied) as u64) as u32;
        if kk != 0 && !put(p, v[0], (*m).data.add(((*m).off + copied) as usize), kk as u64) { return -EFAULT; }
        copied += kk;
        i += 1;
    }
    mh.flags = if copied < avail { MSG_TRUNC } else { 0 };
    if mh.name != 0 && mh.namelen != 0 {
        let mut sa = SockUn { family: AF_UNIX as u16, path: [0; 108] };
        if !(*s).peer.is_null() { setstr(&mut sa.path, &(*(*s).peer).path); }
        put(p, mh.name, &sa, (mh.namelen as u64).min(110));
    }
    if (*m).nfds != 0 && mh.control != 0 && mh.controllen >= 24 {
        let mut ctl = [0u8; 64];
        let mut passed = 0usize;
        for i in 0..(*m).nfds as usize {
            (*(*m).fds[i]).refs += 1;
            let nfd = lx_fd_alloc(p, (*m).fds[i], 0);
            if nfd < 0 { (*(*m).fds[i]).refs -= 1; break; }
            (*p).fdflags[nfd as usize] = 1;
            ctl[16 + passed * 4..20 + passed * 4].copy_from_slice(&(nfd as i32).to_le_bytes());
            passed += 1;
        }
        let clen = 16 + passed as u64 * 4;
        ctl[0..8].copy_from_slice(&clen.to_le_bytes());
        ctl[8..12].copy_from_slice(&SOL_SOCKET.to_le_bytes());
        ctl[12..16].copy_from_slice(&SCM_RIGHTS.to_le_bytes());
        put(p, mh.control, ctl.as_ptr(), cmsg_align(clen));
        mh.controllen = cmsg_align(clen);
        if passed < (*m).nfds as usize { mh.flags |= MSG_CTRUNC; }
    } else {
        /* no control data: say so, or the program parses whatever its buffer held */
        if (*m).nfds != 0 { mh.flags |= MSG_CTRUNC; }
        mh.controllen = 0;
    }
    if (*m).nfds != 0 && !peek {                         /* descriptors go with the first read of a message only */
        for i in 0..(*m).nfds as usize { if !(*m).fds[i].is_null() { lx_file_put((*m).fds[i]); } }
        (*m).nfds = 0;
    }
    put(p, umsg, &mh, 52);
    if !peek {
        (*m).off += copied;
        (*s).qlen = (*s).qlen.wrapping_sub(copied);
        if (*m).off >= (*m).len { (*s).qhead = (*m).next; if (*s).qhead.is_null() { (*s).qtail = null_mut(); } msg_free(m); }
    }
    copied as i64
}
unsafe fn do_shutdown(p: *mut LxProc, fd: c_int, how: c_int) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    if (*s).family == AF_INET {
        if (*s).ty == SOCK_DGRAM { if how != 0 { (*s).connected = false; } return 0; }   /* stop using the default peer */
        if (*s).tcb.is_null() { return -ENOTCONN; }
        if how == 1 || how == 2 { k::tcp_shutdown_wr((*s).tcb); }   /* SHUT_WR, SHUT_RDWR */
        return 0;                                        /* discarding input is not modelled */
    }
    if how == 1 || how == 2 { (*s).peer_gone = true; }
    if (how == 0 || how == 2) && !(*s).peer.is_null() { (*(*s).peer).peer_gone = true; }
    0
}
unsafe fn do_getname(p: *mut LxProc, fd: c_int, uaddr: u64, ualen: u64, peer: bool) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    if (*s).family == AF_INET {
        if peer && !(*s).connected { return -ENOTCONN; }
        return in_put(p, uaddr, ualen, if peer { (*s).raddr } else { (*s).laddr }, if peer { (*s).rport } else { (*s).lport });
    }
    if uaddr == 0 || ualen == 0 { return 0; }
    let mut sa = SockUn { family: AF_UNIX as u16, path: [0; 108] };
    let src = if peer { (*s).peer } else { s };
    if !src.is_null() { setstr(&mut sa.path, &(*src).path); }
    let mut cap = 0i32;
    get(p, &mut cap, ualen, 4);
    let l = 110i32;
    if !put(p, uaddr, &sa, (cap as u32 as u64).min(110)) { return -EFAULT; }
    if put(p, ualen, &l, 4) { 0 } else { -EFAULT }
}
unsafe fn put_i32(p: *mut LxProc, a: u64, v: i32) -> i64 { if put(p, a, &v, 4) { 0 } else { -EFAULT } }
unsafe fn do_sockopt(p: *mut LxProc, fd: c_int, level: c_int, opt: c_int, uval: u64, ulen: u64, get_opt: bool) -> i64 {
    let s = sock_fd(p, fd);
    if s.is_null() { return -ENOTSOCK; }
    if get_opt {
        if level == SOL_SOCKET {
            return match opt {
                2 | 15 => put_i32(p, uval, (*s).reuseaddr as i32),
                3 => put_i32(p, uval, (*s).ty),
                4 => {                                   /* SO_ERROR clears on read */
                    let v = (*s).so_error; (*s).so_error = 0;
                    if v != 0 && !(*s).tcb.is_null() && k::tcp_status((*s).tcb) < 0 { k::tcp_close((*s).tcb); (*s).tcb = null_mut(); }
                    put_i32(p, uval, v)
                }
                7 | 8 => put_i32(p, uval, SOCK_QLEN as i32),
                38 => put_i32(p, uval, (*s).proto),     /* SO_PROTOCOL */
                39 => put_i32(p, uval, (*s).family),    /* SO_DOMAIN */
                18 | 19 => put_i32(p, uval, 1),
                20 | 21 => { let tv = [0u8; 16]; if put(p, uval, tv.as_ptr(), ulen.min(16)) { 0 } else { -EFAULT } }   /* no timeout */
                9 | 13 => put_i32(p, uval, 0),
                17 => {                                  /* SO_PEERCRED: the peer's pid, uid and gid (Wayland asks) */
                    if (*s).family == AF_INET || (*s).peer.is_null() { return -ENOTCONN; }
                    let cred: [i32; 3] = [(*(*s).peer).cred_pid, 1000, 1000];
                    let mut have = 0i32;
                    if ulen == 0 || !get(p, &mut have, ulen, 4) { return -EFAULT; }   /* getsockopt's length is a pointer */
                    let l: i32 = if have < 12 { have.max(0) } else { 12 };
                    if (l != 0 && !put(p, uval, cred.as_ptr(), l as u64)) || !put(p, ulen, &l, 4) { return -EFAULT; }
                    0
                }
                16 => put_i32(p, uval, 0),               /* SO_PASSCRED */
                _ => -ENOPROTOOPT,
            };
        }
        if level == IPPROTO_TCP {
            if opt == 1 { return put_i32(p, uval, (*s).nodelay as i32); }
            return -ENOPROTOOPT;
        }
        if level == 0 && (opt == 2 || opt == 1 || opt == 10 || opt == 11) { return put_i32(p, uval, if opt == 2 { 64 } else { 0 }); }
        return -ENOPROTOOPT;
    }
    let mut v = 0i32;
    if uval == 0 || ulen < 4 { return -EINVAL; }
    if !get(p, &mut v, uval, 4) { return -EFAULT; }
    if level == SOL_SOCKET {
        return match opt {
            2 | 15 => { (*s).reuseaddr = v != 0; 0 }
            6 | 9 | 10 | 12 | 13 | 20 | 21 | 18 | 19 | 1 | 5 | 7 | 8 | 16 | 32 | 33 => 0,   /* accepted no-ops */
            _ => -ENOPROTOOPT,
        };
    }
    if level == IPPROTO_TCP {
        if opt == 1 { (*s).nodelay = v != 0; return 0; }
        if [4, 5, 6, 9, 18, 30].contains(&opt) { return 0; }
        return -ENOPROTOOPT;
    }
    /* IP options programs set and expect to succeed; the IPv4 stack has no use for them */
    if level == 0 && [1, 2, 8, 10, 11, 12, 13].contains(&opt) { return 0; }
    -ENOPROTOOPT
}

/* ---------------- pipes, eventfd, timerfd, epoll ---------------- */
unsafe fn do_pipe(p: *mut LxProc, ufds: u64, flags: c_int) -> i64 {
    let pp = pipe_new();
    if pp.is_null() { return -ENOMEM; }
    let (rf, wf) = (wrap(FD_PIPE, pp as *mut c_void), wrap(FD_PIPE, pp as *mut c_void));
    if rf.is_null() || wf.is_null() {
        if !rf.is_null() { lx_file_put(rf); }
        if !wf.is_null() { lx_file_put(wf); }
        if rf.is_null() && wf.is_null() { kfree((*pp).ring as *mut c_void); zfree(pp); }
        return -ENOMEM;
    }
    (*rf).oflags = 0;
    (*wf).oflags = 1;
    (*pp).readers = 1;
    (*pp).writers = 1;
    let (fd0, fd1) = (install(p, rf, flags), install(p, wf, flags));
    if fd0 < 0 || fd1 < 0 {
        if fd0 >= 0 { lx_file_put((*p).fds[fd0 as usize]); (*p).fds[fd0 as usize] = null_mut(); }
        if fd1 >= 0 { lx_file_put((*p).fds[fd1 as usize]); (*p).fds[fd1 as usize] = null_mut(); }
        return -EMFILE;
    }
    let fds: [i32; 2] = [fd0 as i32, fd1 as i32];
    if !put(p, ufds, &fds, 8) { return -EFAULT; }
    0
}
unsafe fn do_eventfd(p: *mut LxProc, init: u32, flags: c_int) -> i64 {
    let e = znew::<EvFd>();
    if e.is_null() { return -ENOMEM; }
    (*e).count = init as u64;
    (*e).sem = flags & EFD_SEMAPHORE != 0;
    let f = wrap(FD_EVENTFD, e as *mut c_void);
    if f.is_null() { zfree(e); return -ENOMEM; }
    (*f).oflags = O_RDWR_F | if flags & O_NONBLOCK != 0 { O_NONBLOCK } else { 0 };
    install(p, f, flags)
}
unsafe fn do_timerfd_create(p: *mut LxProc, clockid: c_int, flags: c_int) -> i64 {
    if clockid != 0 && clockid != 1 && clockid != 7 { return -EINVAL; }
    let t = znew::<TFd>();
    if t.is_null() { return -ENOMEM; }
    (*t).realtime = clockid == 0;
    let f = wrap(FD_TIMERFD, t as *mut c_void);
    if f.is_null() { zfree(t); return -ENOMEM; }
    (*f).oflags = O_RDWR_F | if flags & O_NONBLOCK != 0 { O_NONBLOCK } else { 0 };
    install(p, f, flags)
}
unsafe fn tfd_of(p: *mut LxProc, fd: c_int) -> *mut TFd {
    if fd < 0 || fd as usize >= LX_MAXFD || (*p).fds[fd as usize].is_null() || (*(*p).fds[fd as usize]).ty != FD_TIMERFD { null_mut() }
    else { (*(*p).fds[fd as usize]).obj as *mut TFd }
}
unsafe fn do_timerfd_settime(p: *mut LxProc, fd: c_int, flags: c_int, unew: u64, uold: u64) -> i64 {
    let t = tfd_of(p, fd);
    if t.is_null() { return -EINVAL; }
    let mut iv = [0u64; 4];
    if !get(p, &mut iv, unew, 32) { return -EFAULT; }
    if uold != 0 { let zero = [0u64; 4]; if !put(p, uold, &zero, 32) { return -EFAULT; } }
    /* deadlines in whole ticks, rounded up: a timer never fires before the clock that
       clock_gettime reads has reached it (rounding down woke programs up to 1 ms early, and
       libwayland and WebKit then re-armed the same deadline and spun until the next tick) */
    let ns = |s: u64, n: u64| s.saturating_mul(1_000_000_000).saturating_add(n);
    let interval = ns(iv[0], iv[1]);
    let value = ns(iv[2], iv[3]);
    /* setting the timer clears the expirations not yet read, as on Linux: libwayland and WebKit
       re-arm their timerfds without reading them, and a count left over kept them "readable"
       for ever, so their event loops never slept */
    (*t).exp = 0;
    if value == 0 { (*t).armed = false; (*t).interval = 0; return 0; }
    (*t).interval = interval.div_ceil(1_000_000).min(0x7FFF_FFFF) as u32;
    let now = mono_ns();
    let target = if flags & 1 == 0 { now.saturating_add(value) }                       /* relative */
                 else if (*t).realtime { now.saturating_add(value.saturating_sub(real_ns())) }  /* TFD_TIMER_ABSTIME on the real-time clock */
                 else { value };
    (*t).next = tick_at(target.min(now.saturating_add(0x7FFF_FFFF * 1_000_000)));
    (*t).armed = true;
    0
}
unsafe fn do_timerfd_gettime(p: *mut LxProc, fd: c_int, ucur: u64) -> i64 {
    let t = tfd_of(p, fd);
    if t.is_null() { return -EINVAL; }
    let mut cur = [0u64; 4];
    cur[0] = ((*t).interval / 1000) as u64; cur[1] = ((*t).interval % 1000) as u64 * 1_000_000;
    if (*t).armed {
        let now = timer_ticks();
        let left = if (*t).next.wrapping_sub(now) as i32 > 0 { (*t).next.wrapping_sub(now) } else { 0 };
        cur[2] = (left / 1000) as u64; cur[3] = (left % 1000) as u64 * 1_000_000;
    }
    if put(p, ucur, &cur, 32) { 0 } else { -EFAULT }
}
unsafe fn epoll_fd(p: *mut LxProc, fd: c_int) -> *mut Epoll {
    let f = lx_fd_get(p, fd as i64);
    if !f.is_null() && (*f).ty == FD_EPOLL { (*f).obj as *mut Epoll } else { null_mut() }
}
unsafe fn do_epoll_create(p: *mut LxProc, flags: c_int) -> i64 {
    let e = znew::<Epoll>();
    if e.is_null() { return -ENOMEM; }
    let f = wrap(FD_EPOLL, e as *mut c_void);
    if f.is_null() { zfree(e); return -ENOMEM; }
    (*f).oflags = O_RDWR_F;
    install(p, f, flags)
}
unsafe fn do_epoll_ctl(p: *mut LxProc, epfd: c_int, op: c_int, fd: c_int, uev: u64) -> i64 {
    let e = epoll_fd(p, epfd);
    if e.is_null() { return -EBADF; }
    if op != 1 && op != 3 && op != 2 { return -EINVAL; }
    if lx_fd_get(p, fd as i64).is_null() { return -EBADF; }
    let mut ev = [0u8; 12];
    if op != 2 && !get(p, ev.as_mut_ptr(), uev, 12) { return -EFAULT; }
    let events = u32::from_le_bytes([ev[0], ev[1], ev[2], ev[3]]);
    let data = u64::from_le_bytes([ev[4], ev[5], ev[6], ev[7], ev[8], ev[9], ev[10], ev[11]]);
    let n = (*e).n as usize;
    let i = (0..n).find(|&i| (*e).items[i].fd == fd).unwrap_or(n);
    if op == 1 {
        if i < n { return -EEXIST; }
        if n >= 64 { return -ENOSPC; }
        (*e).items[n] = EpItem { fd, events, data }; (*e).n += 1;
        return 0;
    }
    if i >= n { return -ENOENT; }
    if op == 2 { (*e).n -= 1; (*e).items[i] = (*e).items[(*e).n as usize]; return 0; }
    (*e).items[i].events = events; (*e).items[i].data = data;
    0
}
unsafe fn do_epoll_wait(p: *mut LxProc, epfd: c_int, uevents: u64, maxevents: c_int, timeout: c_int) -> i64 {
    let e = epoll_fd(p, epfd);
    if e.is_null() { return -EBADF; }
    if maxevents <= 0 { return -EINVAL; }
    let deadline = timer_ticks().wrapping_add(if timeout > 0 { timeout as u32 } else { 0 });
    loop {
        let mut n = 0i32;
        let mut i = 0usize;
        while i < (*e).n as usize && n < maxevents {
            let it = (*e).items[i];
            i += 1;
            let f = lx_fd_get(p, it.fd as i64);
            let mut got = 0u32;
            if f.is_null() { got = POLLNVAL; }
            else {
                if lx_ipc_ready(f, POLLIN) { got |= POLLIN; }
                if lx_ipc_ready(f, POLLOUT) { got |= POLLOUT; }
                if lx_ipc_ready(f, POLLHUP) { got |= POLLHUP; }
                else if (*f).ty == FD_PIPE && (*((*f).obj as *mut Pipe)).writers == 0 { got |= POLLHUP; }
                else if (*f).ty == FD_SOCKET && (*((*f).obj as *mut Sock)).peer_gone { got |= POLLHUP; }
            }
            if got == 0 { continue; }
            if got & it.events == 0 && got & (POLLERR | POLLHUP | POLLNVAL) == 0 { continue; }
            let mut out = [0u8; 12];
            out[0..4].copy_from_slice(&got.to_le_bytes()); out[4..12].copy_from_slice(&it.data.to_le_bytes());
            if !put(p, uevents.wrapping_add(n as u64 * 12), out.as_ptr(), 12) { return -EFAULT; }
            n += 1;
        }
        if n != 0 { return n as i64; }
        if timeout == 0 { return 0; }
        if timeout > 0 && timer_ticks().wrapping_sub(deadline) as i32 >= 0 { return 0; }
        if interrupted() { return -EINTR; }
        task_sleep(1);
    }
}

/* ---------------- dispatch ---------------- */
pub unsafe fn lx_sys_ipc(t: *mut LxThread, nr: i64, r: *mut Regs, handled: &mut bool) -> i64 {
    let p = (*t).proc_;
    let (a0, a1, a2, a3, a4, a5) = ((*r).edi, (*r).esi, (*r).edx, (*r).r10, (*r).r8, (*r).r9);
    let i = |x: u64| x as c_int;
    *handled = true;
    match nr {
        22 => do_pipe(p, a0, 0),
        293 => do_pipe(p, a0, i(a1)),
        41 => do_socket(p, i(a0), i(a1), i(a2)),
        53 => do_socketpair(p, i(a0), i(a1), i(a2), a3),
        49 => do_bind(p, i(a0), a1, i(a2)),
        50 => do_listen(p, i(a0)),
        42 => do_connect(p, i(a0), a1, i(a2)),
        43 => do_accept(p, i(a0), a1, a2, 0),
        288 => do_accept(p, i(a0), a1, a2, i(a3)),
        44 => do_sendto(p, i(a0), a1, a2, i(a3), a4, a5),
        45 => do_recvfrom(p, i(a0), a1, a2, i(a3), a4, a5),
        46 => do_sendmsg(p, i(a0), a1, i(a2)),
        47 => do_recvmsg(p, i(a0), a1, i(a2)),
        48 => do_shutdown(p, i(a0), i(a1)),
        51 => do_getname(p, i(a0), a1, a2, false),
        52 => do_getname(p, i(a0), a1, a2, true),
        54 => do_sockopt(p, i(a0), i(a1), i(a2), a3, a4, false),
        55 => do_sockopt(p, i(a0), i(a1), i(a2), a3, a4, true),
        290 => do_eventfd(p, a0 as u32, i(a1)),
        284 => do_eventfd(p, a0 as u32, 0),
        283 => do_timerfd_create(p, i(a0), i(a1)),
        286 => do_timerfd_settime(p, i(a0), i(a1), a2, a3),
        287 => do_timerfd_gettime(p, i(a0), a1),
        291 => do_epoll_create(p, i(a0)),
        233 => do_epoll_ctl(p, i(a0), i(a1), i(a2), a3),
        232 | 281 => do_epoll_wait(p, i(a0), a1, i(a2), i(a3)),
        _ => { *handled = false; -ENOSYS }
    }
}
