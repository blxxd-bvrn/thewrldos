//! Linux file system calls (was src/linux/fs.c). Paths: "/" is C:\Linux, /mnt/<letter>/ is
//! that drive, /lib, /lib64, /bin and /sbin are the merged-/usr links of current
//! distributions, and a few /dev and /proc entries are built in (rust/src/lxpath.rs). Files
//! live in the WRLD VFS (whole files in memory), so reads and mmap() copy from there.

use super::*;
use super::mem::*;
use super::proc_::{resolve_w, basename};
use crate::lxpath::{self as lp, kind as K};
use alloc::vec::Vec;
use core::ptr::{self, null_mut};
use core::fmt::Write as _;

#[allow(clashing_extern_declarations)]
extern "C" {
    fn vfs_create(path: *const c_char, cwd: *const c_char, dir: CBool) -> *mut Vnode;
    fn vfs_mkdir_p(path: *const c_char) -> *mut Vnode;
    fn vfs_write_node(n: *mut Vnode, data: *const c_void, len: u32) -> CBool;
    fn vfs_append_node(n: *mut Vnode, data: *const c_void, len: u32) -> CBool;
    fn vfs_write_file(path: *const c_char, data: *const c_void, len: u32) -> *mut Vnode;
    fn vfs_delete(n: *mut Vnode) -> CBool;
    fn vfs_rename(n: *mut Vnode, nn: *const c_char) -> CBool;
    fn vfs_move(n: *mut Vnode, nd: *mut Vnode) -> CBool;
    pub fn vfs_unshare(n: *mut Vnode) -> CBool;
}

pub const AT_FDCWD: c_int = -100;
const O_ACCMODE: c_int = 3;
const O_CREAT: c_int = 0o100;
const O_EXCL: c_int = 0o200;
const O_TRUNC: c_int = 0o1000;
pub const O_APPEND: c_int = 0o2000;
pub const O_NONBLOCK: c_int = 0o4000;
const O_DIRECTORY: c_int = 0o200000;
const O_CLOEXEC: c_int = 0o2000000;
const O_PATH: c_int = 0o10000000;

/* ---------------- paths ---------------- */
/// The absolute Linux path a call names: relative to `dirfd` (or the working directory).
pub unsafe fn linux_abs(p: *mut LxProc, dirfd: c_int, upath: &[u8]) -> Vec<u8> {
    let cwd: &[u8] = if p.is_null() { b"/" } else { cstr(&(*p).cwd) };
    if upath.first() != Some(&b'/') && dirfd != AT_FDCWD {
        let d = lx_fd_get(p, dirfd as i64);
        let base: Vec<u8> = if d.is_null() { cwd.to_vec() } else { cstr(&(*d).path).to_vec() };
        lp::absolute(&base, upath)
    } else {
        lp::absolute(cwd, upath)
    }
}
/// A Linux path's kind and the WRLD path it maps to.
pub unsafe fn lx_path(p: *mut LxProc, dirfd: c_int, upath: &[u8]) -> (c_int, Vec<u8>) {
    let a = linux_abs(p, dirfd, upath);
    (lp::classify(&a), lp::to_wrld(&a, 512))
}
/// The same, with the absolute Linux path too.
unsafe fn lx_path2(p: *mut LxProc, dirfd: c_int, upath: &[u8]) -> (c_int, Vec<u8>, Vec<u8>) {
    let a = linux_abs(p, dirfd, upath);
    (lp::classify(&a), lp::to_wrld(&a, 512), a)
}

/// What readlink("/proc/self/fd/N") gives, as Linux names it: the path the file was opened
/// as, or "pipe:[ino]", "socket:[ino]", "anon_inode:[eventfd]" and so on.
unsafe fn fd_link_name(f: *mut LxFile) -> Vec<u8> {
    let p = cstr(&(*f).path);
    if !p.is_empty() { return p.to_vec(); }
    let ino = ((if (*f).obj.is_null() { f as usize } else { (*f).obj as usize }) >> 4) & 0xFF_FFFF;
    let mut v = Vec::new();
    let _ = match (*f).ty {
        FD_PIPE => write!(Wv(&mut v), "pipe:[{}]", ino),
        FD_SOCKET => write!(Wv(&mut v), "socket:[{}]", ino),
        FD_EPOLL => write!(Wv(&mut v), "anon_inode:[eventpoll]"),
        FD_EVENTFD => write!(Wv(&mut v), "anon_inode:[eventfd]"),
        FD_TIMERFD => write!(Wv(&mut v), "anon_inode:[timerfd]"),
        FD_CONSOLE => write!(Wv(&mut v), "/dev/tty"),
        FD_NULL => write!(Wv(&mut v), "/dev/null"),
        FD_ZERO => write!(Wv(&mut v), "/dev/zero"),
        FD_RANDOM => write!(Wv(&mut v), "/dev/urandom"),
        FD_DSP => write!(Wv(&mut v), "/dev/dsp"),
        FD_WINDOW => write!(Wv(&mut v), "/dev/wrld/window"),
        _ => write!(Wv(&mut v), "anon_inode:[{}]", (*f).ty),
    };
    v
}
struct Wv<'a>(&'a mut Vec<u8>);
impl core::fmt::Write for Wv<'_> { fn write_str(&mut self, s: &str) -> core::fmt::Result { self.0.extend_from_slice(s.as_bytes()); Ok(()) } }

/// A path argument: Err(-EFAULT / -ENAMETOOLONG).
unsafe fn ustr(src: u64, cap: usize) -> Result<Vec<u8>, i64> {
    let vm = cur_vm();
    let mut v = Vec::new();
    for i in 0..cap as u64 {
        if (src.wrapping_add(i) & (PAGE - 1) == 0 || i == 0) && !uvm_check(vm, src.wrapping_add(i), 1, false) { return Err(-EFAULT); }
        let c = ptr::read_volatile((src + i) as *const u8);
        if c == 0 { return Ok(v); }
        v.push(c);
    }
    Err(-ENAMETOOLONG)
}
macro_rules! tryr { ($e:expr) => { match $e { Ok(v) => v, Err(e) => return e } } }

/* ---------------- descriptors ---------------- */
pub unsafe fn lx_fd_alloc(p: *mut LxProc, f: *mut LxFile, min: i64) -> i64 {
    let lim = (*p).rlim[7][0].min(LX_MAXFD as u64) as i64;            /* RLIMIT_NOFILE */
    let mut i = min.max(0);
    while i < lim { if (*p).fds[i as usize].is_null() { (*p).fds[i as usize] = f; (*p).fdflags[i as usize] = 0; return i; } i += 1; }
    -super::EMFILE
}
/// The descriptor's file; `fd` is cut to an int, as the C code's parameter was.
pub unsafe fn lx_fd_get(p: *mut LxProc, fd: i64) -> *mut LxFile {
    let fd = fd as i32;
    if p.is_null() || fd < 0 || fd as usize >= LX_MAXFD { null_mut() } else { (*p).fds[fd as usize] }
}
pub unsafe fn lx_file_put(f: *mut LxFile) {
    if f.is_null() { return; }
    (*f).refs -= 1;
    if (*f).refs > 0 { return; }
    match (*f).ty {
        FD_SHM => { shm_unref((*f).obj as *mut ShmObj); (*f).obj = null_mut(); }
        FD_WINDOW => { super::win::lxwin_release((*f).obj); (*f).obj = null_mut(); }
        FD_DSP => { super::dsp::dsp_release((*f).obj); (*f).obj = null_mut(); }
        _ => super::ipc::lx_ipc_release(f),
    }
    kfree((*f).pdata as *mut c_void); zfree(f);
}
unsafe fn file_new(ty: c_int) -> *mut LxFile { let f = znew::<LxFile>(); (*f).ty = ty; (*f).refs = 1; f }

/* ---------------- /proc contents ---------------- */
/// A generated file: each piece is formatted into 512 bytes, as the C code's were.
struct PBuf { b: Vec<u8> }
impl PBuf {
    fn add(&mut self, args: core::fmt::Arguments) {
        let mut tmp = [0u8; 512];
        bprintf(&mut tmp, args);
        self.b.extend_from_slice(cstr(&tmp));
    }
    /// The bytes in a kmalloc'd buffer with a NUL after them, and their length.
    unsafe fn finish(self, out: &mut u32) -> *mut u8 {
        let p = kmalloc(self.b.len() + 1) as *mut u8;
        if p.is_null() { return p; }
        ptr::copy_nonoverlapping(self.b.as_ptr(), p, self.b.len());
        *p.add(self.b.len()) = 0;
        *out = self.b.len() as u32;
        p
    }
}
fn proc_comm(p: &LxProc) -> Vec<u8> {
    let n = cstr(&p.exe);
    let s = n.iter().rposition(|&c| c == b'/' || c == b'\\').map_or(0, |i| i + 1);
    let mut v = if s < n.len() { n[s..].to_vec() } else { b"?".to_vec() };
    v.truncate(63);
    v
}
unsafe fn proc_text(p: *mut LxProc, kind: c_int, out: &mut u32) -> *mut u8 {
    let mut b = PBuf { b: Vec::new() };
    let comm = proc_comm(&*p);
    let vm = (*p).vm;
    let vsize = || { let mut s = 0u64; let mut v = (*vm).vmas; while !v.is_null() { s = s.wrapping_add((*v).end - (*v).start); v = (*v).next; } s };
    match kind {
        K::PROC_MAPS => {
            let mut v = (*vm).vmas;
            while !v.is_null() {
                let name = if (*v).flags & VMF_STACK != 0 { "[stack]" } else if (*v).flags & VMF_HEAP != 0 { "[heap]" } else { "" };
                b.add(format_args!("{:016x}-{:016x} {}{}{}{} {:08x} {:02x}:{:02x} {:<8} {}\n", (*v).start, (*v).end,
                      if (*v).prot & VM_READ != 0 { 'r' } else { '-' }, if (*v).prot & VM_WRITE != 0 { 'w' } else { '-' },
                      if (*v).prot & VM_EXEC != 0 { 'x' } else { '-' }, if (*v).flags & VMF_SHARED != 0 { 's' } else { 'p' },
                      (*v).foff, 8, 1, if (*v).fnode.is_null() { 0u32 } else { (((*v).fnode as usize >> 4) & 0xFF_FFFF) as u32 }, name));
                v = (*v).next;
            }
        }
        K::PROC_STAT => {
            let rss = (*vm).pages as u64 * PAGE;
            let self_ = task_current();
            let ct = lx_current();
            let ut = if !self_.is_null() && !ct.is_null() && (*ct).proc_ == p { (*self_).cpu_ms / 10 } else { 0 };
            b.add(format_args!("{} ({}) R {} {} {} 0 -1 4194304 0 0 0 0 {} 0 0 0 20 0 {} 0 0 {} {} 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                  (*p).pid, B(&comm), (*p).ppid, (*p).pid, (*p).pid, ut, if (*p).threads != 0 { (*p).threads } else { 1 }, vsize(), rss));
        }
        K::PROC_STATUS => {
            let vs = vsize();
            b.add(format_args!("Name:\t{}\nUmask:\t{:04o}\nState:\tR (running)\nTgid:\t{}\nPid:\t{}\nPPid:\t{}\nThreads:\t{}\n",
                  B(&comm), (*p).umask, (*p).pid, (*p).pid, (*p).ppid, if (*p).threads != 0 { (*p).threads } else { 1 }));
            b.add(format_args!("SigQ:\t0/0\nSigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\nSigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n"));
            b.add(format_args!("CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t0000000000000000\nCapBnd:\t0000000000000000\nCapAmb:\t0000000000000000\nNoNewPrivs:\t{}\nSeccomp:\t0\n",
                  (*p).no_new_privs as i32));
            let rsskb = (*vm).pages as u64 * PAGE / 1024;
            b.add(format_args!("VmPeak:\t{:8} kB\nVmSize:\t{:8} kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\nVmHWM:\t{:8} kB\nVmRSS:\t{:8} kB\n",
                  vs / 1024, vs / 1024, rsskb, rsskb));
            b.add(format_args!("RssAnon:\t       0 kB\nRssFile:\t       0 kB\nRssShmem:\t       0 kB\nVmData:\t       0 kB\nVmStk:\t       0 kB\nVmExe:\t       0 kB\nVmLib:\t       0 kB\nVmPTE:\t       0 kB\nVmSwap:\t       0 kB\nHugetlbPages:\t       0 kB\n"));
        }
        K::PROC_CMDLINE => { b.b.extend_from_slice(cstr(&(*p).exe)); b.b.push(0); }
        K::PROC_MEMINFO => {
            let totalkb = 256u64 << 10;
            let usedkb = pmm_used_pages() as u64 * PAGE / 1024;
            let freekb = totalkb.saturating_sub(usedkb);
            b.add(format_args!("MemTotal:       {:8} kB\nMemFree:        {:8} kB\nMemAvailable:   {:8} kB\nBuffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\nActive:                0 kB\nInactive:              0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\nDirty:                 0 kB\nWriteback:             0 kB\nAnonPages:             0 kB\nMapped:                0 kB\nShmem:                 0 kB\nSlab:                  0 kB\nSReclaimable:          0 kB\nSUnreclaim:            0 kB\nPageTables:            0 kB\n",
                  totalkb, freekb, freekb));
        }
        K::PROC_CPUINFO => {
            let (mut vendor, mut brand) = ([0u8; 16], [0u8; 64]);
            cpu_get_info(vendor.as_mut_ptr() as *mut c_char, brand.as_mut_ptr() as *mut c_char);
            let (eax, _, _, _) = cpuid1();
            let vd: &[u8] = if vendor[0] != 0 { &vendor } else { b"GenuineIntel" };
            let bd: &[u8] = if brand[0] != 0 { &brand } else { b"Unknown" };
            b.add(format_args!("processor\t: 0\nvendor_id\t: {}\ncpu family\t: {}\nmodel\t\t: {}\nmodel name\t: {}\nstepping\t: {}\n",
                  B(vd), (eax >> 8) & 0xF, (eax >> 4) & 0xF, B(bd), eax & 0xF));
            b.add(format_args!("physical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\nflags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss syscall nx lm\n"));
        }
        K::PROC_UPTIME => {
            let ms = timer_ticks();
            let idle = ptr::read_volatile(ptr::addr_of!(idle_ticks));
            b.add(format_args!("{}.{:02} {}.{:02}\n", ms / 1000, (ms % 1000) / 10, idle / 100, idle % 100));
        }
        K::PROC_VERSION => b.add(format_args!("Linux version 6.0.0-wrld (wrld@wrld) (gcc version 13.2.0) #1 SMP THE WRLD OS\n")),
        K::SYS_CPU_ONLINE | K::SYS_CPU_POSSIBLE | K::SYS_CPU_PRESENT => b.add(format_args!("0\n")),
        K::SYS_OSRELEASE => b.add(format_args!("6.0.0-wrld\n")),
        K::SYS_OSTYPE => b.add(format_args!("Linux\n")),
        K::SYS_VERSION => b.add(format_args!("#1 SMP THE WRLD OS\n")),
        _ => {}
    }
    b.finish(out)
}
unsafe fn proc_listing(p: *mut LxProc, kind: c_int, out: &mut u32) -> *mut u8 {
    let mut b = PBuf { b: Vec::new() };
    match kind {
        K::PROC_DIR => b.add(format_args!("self 4\n{} 4\nmeminfo 8\ncpuinfo 8\nuptime 8\nversion 8\n", (*p).pid)),
        K::PROC_SELF_DIR => b.add(format_args!("fd 4\nmaps 8\nstat 8\nstatus 8\ncmdline 8\nexe 10\n")),
        K::SYS => b.add(format_args!("devices 4\nkernel 4\n")),
        K::SYS_DEVICES => b.add(format_args!("system 4\n")),
        K::SYS_DEV_SYSTEM => b.add(format_args!("cpu 4\n")),
        K::SYS_CPU => b.add(format_args!("online 8\npossible 8\npresent 8\n")),
        K::SYS_KERNEL => b.add(format_args!("osrelease 8\nostype 8\nversion 8\n")),
        _ => for i in 0..LX_MAXFD { if !(*p).fds[i].is_null() { b.add(format_args!("{} 10\n", i)); } },
    }
    b.finish(out)
}

/* ---------------- stat ---------------- */
#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
struct LStat {
    dev: u64, ino: u64, nlink: u64, mode: u32, uid: u32, gid: u32, pad0: u32, rdev: u64, size: i64, blksize: i64, blocks: i64,
    atime: i64, atime_ns: i64, mtime: i64, mtime_ns: i64, ctime: i64, ctime_ns: i64, reserved: [i64; 3],
}
const _: () = assert!(core::mem::size_of::<LStat>() == 144);
/// Days-from-civil seconds since the epoch.
pub fn epoch_of(t: [c_int; 6]) -> i64 {
    let [mut y, m, d, h, mi, s] = t;
    y -= (m <= 2) as c_int;
    let era: i64 = if y >= 0 { y as i64 } else { y as i64 - 399 } / 400;
    let yoe = (y as i64 - era * 400) as u32;
    let mm: i32 = m + if m > 2 { -3 } else { 9 };
    let doy = ((153i64 * mm as i64 + 2) / 5) as u32 + d as u32 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    (era * 146097 + doe as i64 - 719468) * 86400 + h as i64 * 3600 + mi as i64 * 60 + s as i64
}
unsafe fn node_time(n: *mut Vnode) -> i64 {
    let t = (*n).mtime6();
    if t[0] == 0 { return 1700000000; }
    epoch_of(t)
}
unsafe fn shm_stat(o: *mut ShmObj) -> LStat {
    let size = (*o).size as i64;
    LStat { dev: 0x802, nlink: 1, uid: 1000, gid: 1000, blksize: 4096, ino: ((o as usize >> 4) & 0xFFFF_FFFF) as u64,
            mode: 0o100600, size, blocks: (size + 511) / 512, ..Default::default() }
}
unsafe fn fill_stat(n: *mut Vnode, kind: c_int) -> LStat {
    let mut st = LStat { dev: 0x801, nlink: 1, uid: 1000, gid: 1000, blksize: 4096, ..Default::default() };
    if kind == K::PROC_DIR || kind == K::PROC_SELF_DIR || kind == K::PROC_FD_DIR || (K::SYS..=K::SYS_KERNEL).contains(&kind) {
        st.mode = 0o40555; st.nlink = 2; st.ino = 200 + kind as u64; return st;
    }
    if kind == K::PROC_SELF_FD { st.mode = 0o120777; st.ino = 200 + kind as u64; return st; }
    if (K::PROC_MAPS..=K::PROC_VERSION).contains(&kind) || (K::SYS_CPU_ONLINE..=K::SYS_VERSION).contains(&kind) {
        st.mode = 0o100444; st.ino = 200 + kind as u64; return st;
    }
    if kind != K::FILE && kind != K::PROC_SELF_EXE { st.mode = 0o20666; st.rdev = 0x103; st.ino = 100 + kind as u64; return st; }
    st.ino = ((n as usize >> 4) & 0xFFFF_FFFF) as u64;
    st.mode = if (*n).is_dir() { 0o40755 } else { 0o100755 };
    st.size = if (*n).is_dir() { 4096 } else { (*n).size() as i64 };
    st.blocks = (st.size + 511) / 512;
    let t = node_time(n);
    st.atime = t; st.mtime = t; st.ctime = t;
    st
}

/// glibc's name resolver insists on reading a few configuration files before it will use
/// our sockets. Create them in the RAM file system the first time they are looked up;
/// resolv.conf follows whatever DHCP handed us.
unsafe fn etc_seed(abs: &[u8], wrld: &[u8]) -> *mut Vnode {
    const FIXED: [(&[u8], &[u8]); 4] = [
        (b"/etc/host.conf", b"order hosts,bind\nmulti on\n"),
        (b"/etc/nsswitch.conf", b"hosts: files dns\n"),
        (b"/etc/hosts", b"# The only names this machine knows without DNS.\n127.0.0.1\tlocalhost\n::1\t\tlocalhost\n"),
        (b"/etc/services", b"ftp 21/tcp\nssh 22/tcp\nsmtp 25/tcp\ndomain 53/udp\nhttp 80/tcp\nhttps 443/tcp\nntp 123/udp\n"),
    ];
    if !abs.starts_with(b"/etc") || (abs.len() > 4 && abs[4] != b'/') { return null_mut(); }
    if resolve_w(b"C:\\Linux").is_null() { return null_mut(); }      /* no Linux root installed */
    vfs_mkdir_p(c"C:\\Linux\\etc".as_ptr());
    let zw = zstr(wrld);
    for (path, text) in FIXED {
        if abs == path && resolve_w(wrld).is_null() { vfs_write_file(zw.as_ptr() as *const c_char, text.as_ptr() as *const c_void, text.len() as u32); }
    }
    if abs == b"/etc/resolv.conf" {
        let mut text = [0u8; 64];
        let dns = (*ptr::addr_of!(crate::kernel::netif)).dns;
        if dns != 0 {
            let mut a = [0u8; 16];
            crate::kernel::ip_to_str(dns, a.as_mut_ptr() as *mut c_char);
            bprintf(&mut text, format_args!("nameserver {}\n", B(&a)));
        } else { setstr(&mut text, b"# no DNS server configured\n"); }
        let t = cstr(&text);
        vfs_write_file(zw.as_ptr() as *const c_char, t.as_ptr() as *const c_void, t.len() as u32);
    }
    resolve_w(wrld)
}

const LX_FILE_GROW: u64 = 64 << 20;     /* the most a regular file grows in one write or truncate */
/// Resolve a Linux path to a node (null: not found), its kind and absolute Linux path.
unsafe fn resolve(p: *mut LxProc, dirfd: c_int, upath: &[u8]) -> (*mut Vnode, c_int, Vec<u8>) {
    let (k, w, labs) = lx_path2(p, dirfd, upath);
    if k == K::PROC_SELF_EXE { let we = lp::to_wrld(&(*p).exe, 512); return (resolve_w(&we), k, labs); }
    if k != K::FILE { return (null_mut(), k, labs); }
    let mut n = resolve_w(&w);
    if n.is_null() { n = etc_seed(&labs, &w); }
    (n, k, labs)
}

unsafe fn do_open(p: *mut LxProc, dirfd: c_int, upath: u64, flags: c_int, _mode: c_int) -> i64 {
    let path = tryr!(ustr(upath, 512));
    if path.is_empty() { return -ENOENT; }
    let (mut n, kind, labs) = resolve(p, dirfd, &path);
    let f: *mut LxFile;
    if kind == K::DEV_NULL { f = file_new(FD_NULL); }
    else if kind == K::DEV_ZERO { f = file_new(FD_ZERO); }
    else if kind == K::DEV_RANDOM { f = file_new(FD_RANDOM); }
    else if kind == K::DEV_TTY { f = file_new(FD_CONSOLE); (*f).con = (*p).con; }
    else if kind == K::DEV_SHM {                              /* shm_open: a named shared object */
        let name = &labs[9..];
        let mut o = shm_lookup(name);
        if !o.is_null() && flags & O_CREAT != 0 && flags & O_EXCL != 0 { shm_unref(o); return -EEXIST; }
        if o.is_null() {
            if flags & O_CREAT == 0 { return -ENOENT; }
            o = shm_create_named(name);
            if o.is_null() { return -ENOMEM; }
        }
        if flags & O_TRUNC != 0 && flags & O_ACCMODE != 0 { shm_resize(o, 0); }
        f = file_new(FD_SHM); (*f).obj = o as *mut c_void;
    } else if kind == K::DEV_DSP {
        let mut err = 0i64;
        let o = super::dsp::dsp_open(&mut err);
        if o.is_null() { return err; }
        f = file_new(FD_DSP); (*f).obj = o;
    } else if kind == K::DEV_WRLDWIN {
        f = file_new(FD_WINDOW); (*f).obj = super::win::lxwin_new();
        if (*f).obj.is_null() { zfree(f); return -ENOMEM; }
    } else if kind == K::DEV_SHM_DIR {
        if flags & O_ACCMODE != 0 && flags & O_PATH == 0 { return -EISDIR; }
        f = file_new(FD_DIR); (*f).pdata = kzalloc(1) as *mut u8; (*f).plen = 0;
    } else if kind == K::PROC_SELF_FD {                       /* opening an fd link is a dup */
        let fd = atoi(&labs[14..]);
        let src = lx_fd_get(p, fd as i64);
        if src.is_null() { return -ENOENT; }
        (*src).refs += 1;
        let nfd = lx_fd_alloc(p, src, 0);
        if nfd < 0 { lx_file_put(src); return nfd; }
        if flags & O_CLOEXEC != 0 { (*p).fdflags[nfd as usize] = 1; }
        return nfd;
    } else if kind == K::PROC_DIR || kind == K::PROC_SELF_DIR || kind == K::PROC_FD_DIR || (K::SYS..=K::SYS_KERNEL).contains(&kind) {
        if flags & O_ACCMODE != 0 && flags & O_PATH == 0 { return -EISDIR; }
        f = file_new(FD_DIR);
        (*f).pdata = proc_listing(p, kind, &mut (*f).plen);
        if (*f).pdata.is_null() { zfree(f); return -ENOMEM; }
    } else if (K::PROC_MAPS..=K::PROC_VERSION).contains(&kind) || (K::SYS_CPU_ONLINE..=K::SYS_VERSION).contains(&kind) {
        if flags & O_DIRECTORY != 0 { return -ENOTDIR; }
        if flags & O_ACCMODE != 0 && flags & O_PATH == 0 { return -EACCES; }
        f = file_new(FD_FILE);
        (*f).pdata = proc_text(p, kind, &mut (*f).plen);
        if (*f).pdata.is_null() { zfree(f); return -ENOMEM; }
    } else {
        if n.is_null() {
            if flags & O_CREAT == 0 { return -ENOENT; }
            let w = lp::to_wrld(&labs, 512);
            let dir = crate::vfspath::dirname(&w, 512);
            let dn = resolve_w(&dir);
            if dn.is_null() || !(*dn).is_dir() { return -ENOENT; }
            let zw = zstr(&w);
            n = vfs_create(zw.as_ptr() as *const c_char, ptr::null(), 0);
            if n.is_null() { return -EACCES; }
        } else if flags & O_CREAT != 0 && flags & O_EXCL != 0 { return -EEXIST; }
        if flags & O_DIRECTORY != 0 && !(*n).is_dir() { return -ENOTDIR; }
        if (*n).is_dir() && flags & O_ACCMODE != 0 && flags & O_PATH == 0 { return -EISDIR; }
        f = file_new(if (*n).is_dir() { FD_DIR } else { FD_FILE });
        (*f).node = n;
        if flags & O_TRUNC != 0 && !(*n).is_dir() && flags & O_ACCMODE != 0 { vfs_write_node(n, c"".as_ptr() as *const c_void, 0); }
    }
    (*f).oflags = flags;
    setstr(&mut (*f).path, &labs);
    let fd = lx_fd_alloc(p, f, 0);
    if fd < 0 { zfree(f); return fd; }
    if flags & O_CLOEXEC != 0 { (*p).fdflags[fd as usize] = 1; }
    fd
}
/// The C library's atoi: optional spaces and sign, then digits.
pub fn atoi(s: &[u8]) -> c_int {
    let s = cstr(s);
    let mut i = 0;
    while i < s.len() && (s[i] == b' ' || (b'\t'..=b'\r').contains(&s[i])) { i += 1; }
    let mut neg = false;
    if i < s.len() && (s[i] == b'-' || s[i] == b'+') { neg = s[i] == b'-'; i += 1; }
    let mut v: c_int = 0;
    while i < s.len() && s[i].is_ascii_digit() { v = v.wrapping_mul(10).wrapping_add((s[i] - b'0') as c_int); i += 1; }
    if neg { v.wrapping_neg() } else { v }
}

static mut RAND_S: u64 = 0x9E37_79B9_7F4A_7C15;
unsafe fn file_read(p: *mut LxProc, f: *mut LxFile, buf: u64, n: u64, at: i64) -> i64 {
    if !uvm_check((*p).vm, buf, n, true) { return -EFAULT; }
    let dst = buf as *mut u8;
    let nb = (*f).oflags & O_NONBLOCK != 0;
    match (*f).ty {
        FD_NULL => 0,
        FD_ZERO => { ptr::write_bytes(dst, 0, n as usize); n as i64 }
        FD_RANDOM => {
            for i in 0..n as usize { RAND_S ^= rdtsc(); RAND_S ^= RAND_S << 13; RAND_S ^= RAND_S >> 7; RAND_S ^= RAND_S << 17; *dst.add(i) = RAND_S as u8; }
            n as i64
        }
        FD_PIPE | FD_SOCKET | FD_EVENTFD | FD_TIMERFD => super::ipc::lx_ipc_read(f, dst, n, nb),
        FD_CONSOLE => {
            let con = (*f).con;
            if con.is_null() { return 0; }
            if (*f).line_pos >= (*f).line_len {
                let l = match (*con).readline { Some(rl) => rl(con, (*f).line.as_mut_ptr() as *mut c_char, 511), None => -1 };
                if l < 0 { return 0; }                                   /* end of input */
                (*f).line[l as usize] = b'\n'; (*f).line_len = l + 1; (*f).line_pos = 0;
            }
            let k = n.min(((*f).line_len - (*f).line_pos) as u64);
            ptr::copy_nonoverlapping((*f).line.as_ptr().add((*f).line_pos as usize), dst, k as usize);
            (*f).line_pos += k as c_int;
            k as i64
        }
        FD_DIR => -EISDIR,
        FD_WINDOW => super::win::lxwin_read((*f).obj, dst, n, nb),
        FD_DSP => -EINVAL,                                          /* no recording */
        FD_SHM => {
            let pos = if at >= 0 { at as u64 } else { (*f).pos };
            let k = shm_rw((*f).obj as *mut ShmObj, dst, n, pos, false);
            if k > 0 && at < 0 { (*f).pos = pos + k as u64; }
            k
        }
        FD_FILE => {
            let (data, size): (*const u8, u64) = if !(*f).node.is_null() { ((*(*f).node).data(), (*(*f).node).size() as u64) } else { ((*f).pdata, (*f).plen as u64) };
            let pos = if at >= 0 { at as u64 } else { (*f).pos };
            if data.is_null() || pos >= size { return 0; }
            let k = n.min(size - pos);
            ptr::copy(data.add(pos as usize), dst, k as usize);
            if at < 0 { (*f).pos = pos + k; }
            k as i64
        }
        _ => -EBADF,
    }
}
unsafe fn file_write(p: *mut LxProc, f: *mut LxFile, buf: u64, n: u64, at: i64) -> i64 {
    if !uvm_check((*p).vm, buf, n, false) { return -EFAULT; }
    let src = buf as *const u8;
    let nb = (*f).oflags & O_NONBLOCK != 0;
    match (*f).ty {
        FD_NULL | FD_ZERO | FD_RANDOM => n as i64,
        FD_PIPE | FD_SOCKET | FD_EVENTFD | FD_TIMERFD => super::ipc::lx_ipc_write(f, src, n, nb),
        FD_CONSOLE => {
            let con = (*f).con;
            if !con.is_null() && n != 0 { if let Some(w) = (*con).write { w(con, src as *const c_char, n as c_int); } }
            n as i64
        }
        FD_DSP => super::dsp::dsp_write((*f).obj, src, n, nb),
        FD_DIR => -EISDIR,
        FD_SHM => {
            if (*f).oflags & O_ACCMODE == 0 { return -EBADF; }
            let o = (*f).obj as *mut ShmObj;
            let pos = if at >= 0 { at as u64 } else if (*f).oflags & O_APPEND != 0 { (*o).size } else { (*f).pos };
            let k = shm_rw(o, src as *mut u8, n, pos, true);
            if k > 0 && at < 0 { (*f).pos = pos + k as u64; }
            k
        }
        FD_FILE => {
            if (*f).node.is_null() { return -EACCES; }               /* generated /proc files are read-only */
            if (*f).oflags & O_ACCMODE == 0 { return -EBADF; }
            let v = (*f).node;
            let vsize = (*v).size() as u64;
            let pos = if at >= 0 { at as u64 } else if (*f).oflags & O_APPEND != 0 { vsize } else { (*f).pos };
            if pos.wrapping_add(n) > 0x7FFF_FFFF || pos.wrapping_add(n) > (*p).rlim[1][0] { return -EFBIG; }   /* RLIMIT_FSIZE */
            /* files live in memory, whole: a write far past the end would allocate and zero the
             * gap in one go and stall the desktop, so a file grows at most LX_FILE_GROW at a time */
            if pos > vsize && pos - vsize > LX_FILE_GROW { return -EFBIG; }
            let ns = vsize.max(pos + n);
            if pos == vsize && at < 0 {
                if vfs_append_node(v, src as *const c_void, n as u32) == 0 { return -ENOSPC; }
            } else {
                let b = kmalloc(if ns != 0 { ns as usize } else { 1 }) as *mut u8;
                if b.is_null() { return -ENOMEM; }
                ptr::copy((*v).data(), b, vsize as usize);
                if pos > vsize { ptr::write_bytes(b.add(vsize as usize), 0, (pos - vsize) as usize); }
                ptr::copy(src, b.add(pos as usize), n as usize);
                let ok = vfs_write_node(v, b as *const c_void, ns as u32);
                kfree(b as *mut c_void);
                if ok == 0 { return -ENOSPC; }
            }
            if at < 0 { (*f).pos = pos + n; }
            n as i64
        }
        _ => -EBADF,
    }
}

unsafe fn getdents(p: *mut LxProc, f: *mut LxFile, buf: u64, cap: u64) -> i64 {
    if (*f).ty != FD_DIR { return -ENOTDIR; }
    if !uvm_check((*p).vm, buf, cap, true) { return -EFAULT; }
    let out = buf as *mut u8;
    let mut o = 0u64;
    loop {
        let name: Vec<u8>;
        let ty: u8;
        let ino: u64;
        if (*f).pos == 0 { name = b".".to_vec(); ty = 4; ino = 1; }
        else if (*f).pos == 1 { name = b"..".to_vec(); ty = 4; ino = 2; }
        else if !(*f).node.is_null() {
            let mut c = (*(*f).node).child();
            let mut i = 2;
            while !c.is_null() && i < (*f).pos { c = (*c).next(); i += 1; }
            if c.is_null() { break; }
            name = cstr((*c).name_bytes()).to_vec(); ty = if (*c).is_dir() { 4 } else { 8 }; ino = ((c as usize >> 4) & 0xFFFF_FFFF) as u64;
        } else if !(*f).pdata.is_null() && (*f).pos >= 2 {
            let data = cbytes((*f).pdata);
            let mut s = Some(0usize);
            let mut i = 2;
            while let Some(at) = s { if i >= (*f).pos { break; }
                let mut q = at;
                while q < data.len() && data[q] != b'\n' { q += 1; }
                s = if q < data.len() { Some(q + 1) } else { None };
                i += 1;
            }
            let Some(at) = s else { break };
            if at >= data.len() { break; }
            let mut e = at;
            while e < data.len() && data[e] != b'\n' { e += 1; }
            let l = (e - at).min(63);
            let mut nm = data[at..at + l].to_vec();
            let mut t = 8;
            if let Some(sp) = nm.iter().position(|&c| c == b' ') { t = atoi(&nm[sp + 1..]); nm.truncate(sp); }
            name = nm; ty = t as u8; ino = 0x200u64.wrapping_add((*f).pos);
        } else { break; }
        let nl = name.len() as u64;
        let rl = (19 + nl + 1 + 7) & !7;
        if o + rl > cap { if o == 0 { return -EINVAL; } break; }
        let rec = out.add(o as usize);
        ptr::write_bytes(rec, 0, rl as usize);
        ptr::write_unaligned(rec as *mut u64, ino);
        ptr::write_unaligned(rec.add(8) as *mut i64, (*f).pos as i64 + 1);
        ptr::write_unaligned(rec.add(16) as *mut u16, rl as u16);
        *rec.add(18) = ty;
        ptr::copy_nonoverlapping(name.as_ptr(), rec.add(19), nl as usize);
        o += rl; (*f).pos += 1;
    }
    o as i64
}

/// File-related system calls; `handled` is false for numbers that are not ours.
pub unsafe fn lx_sys_file(t: *mut LxThread, nr: i64, r: *mut Regs, handled: &mut bool) -> i64 {
    let p = (*t).proc_;
    let vm = (*p).vm;
    let (a0, a1, a2, a3) = ((*r).edi, (*r).esi, (*r).edx, (*r).r10);
    *handled = true;
    let fdg = |fd: u64| lx_fd_get(p, fd as i64);
    match nr {
        0 => { let f = fdg(a0); if f.is_null() { -EBADF } else { file_read(p, f, a1, a2, -1) } }
        1 => { let f = fdg(a0); if f.is_null() { -EBADF } else { file_write(p, f, a1, a2, -1) } }
        17 => { let f = fdg(a0); if f.is_null() { -EBADF } else { file_read(p, f, a1, a2, a3 as i64) } }
        18 => { let f = fdg(a0); if f.is_null() { -EBADF } else { file_write(p, f, a1, a2, a3 as i64) } }
        19 | 20 => {                                                            /* readv, writev */
            let f = fdg(a0); if f.is_null() { return -EBADF; }
            let mut total = 0i64;
            let mut i = 0u64;
            while i < a2 && i < 1024 {
                let Some(v) = uget::<[u64; 2]>(vm, a1.wrapping_add(i * 16)) else { return -EFAULT };
                i += 1;
                if v[1] == 0 { continue; }
                let k = if nr == 19 { file_read(p, f, v[0], v[1], -1) } else { file_write(p, f, v[0], v[1], -1) };
                if k < 0 { return if total != 0 { total } else { k }; }
                total += k;
                if (k as u64) < v[1] { break; }
            }
            total
        }
        2 => do_open(p, AT_FDCWD, a0, a1 as c_int, a2 as c_int),
        257 => do_open(p, a0 as c_int, a1, a2 as c_int, a3 as c_int),
        85 => do_open(p, AT_FDCWD, a0, 0o1101, a1 as c_int),
        3 => {
            let f = fdg(a0); if f.is_null() { return -EBADF; }
            (*p).fds[a0 as usize] = null_mut(); lx_file_put(f); 0
        }
        319 => {                                                                /* memfd_create */
            let name = tryr!(ustr(a0, 80));
            if a1 & !0x7 != 0 { return -EINVAL; }
            let o = shm_new(Some(&name));
            if o.is_null() { return -ENOMEM; }
            if a1 & 2 == 0 { (*o).seals = SHM_SEAL_SEAL; }           /* without MFD_ALLOW_SEALING no seals can be added */
            let f = file_new(FD_SHM);
            (*f).obj = o as *mut c_void; (*f).oflags = 2;
            bprintf(&mut (*f).path, format_args!("/memfd:{} (deleted)", B(&name)));
            let fd = lx_fd_alloc(p, f, 0);
            if fd < 0 { lx_file_put(f); return fd; }
            if a1 & 1 != 0 { (*p).fdflags[fd as usize] = 1; }
            fd
        }
        285 => {                                                                /* fallocate */
            let f = fdg(a0); if f.is_null() { return -EBADF; }
            if a1 & !1 != 0 { return -EOPNOTSUPP; }
            if (*f).ty == FD_SHM { let o = (*f).obj as *mut ShmObj; let want = a2.wrapping_add(a3); return if a1 & 1 != 0 || want <= (*o).size { 0 } else { shm_resize(o, want) }; }
            if (*f).ty != FD_FILE || (*f).node.is_null() { return -ENODEV; }
            0
        }
        436 => {                                                                /* close_range */
            if a0 > a1 { return -EINVAL; }
            if a2 & !6 != 0 { return -EINVAL; }
            let last = a1.min(LX_MAXFD as u64 - 1);
            let mut fd = a0;
            while fd <= last && fd < LX_MAXFD as u64 {
                let i = fd as usize; fd += 1;
                if (*p).fds[i].is_null() { continue; }
                if a2 & 4 != 0 { (*p).fdflags[i] = 1; continue; }
                let f = (*p).fds[i]; (*p).fds[i] = null_mut(); lx_file_put(f);
            }
            0
        }
        8 => {                                                                  /* lseek */
            let f = fdg(a0); if f.is_null() { return -EBADF; }
            if (*f).ty == FD_CONSOLE || ((*f).ty >= FD_PIPE && (*f).ty != FD_SHM) { return -ESPIPE; }
            let end: i64 = if (*f).ty == FD_SHM { (*((*f).obj as *mut ShmObj)).size as i64 } else if !(*f).node.is_null() { (*(*f).node).size() as i64 } else { 0 };
            let base: i64 = match a2 { 0 => 0, 1 => (*f).pos as i64, 2 => end, _ => -1 };
            if base < 0 { return -EINVAL; }
            let np = base.wrapping_add(a1 as i64);
            if np < 0 { return -EINVAL; }
            (*f).pos = np as u64; np
        }
        4 | 6 | 5 | 262 => {                                                   /* stat, lstat, fstat, newfstatat */
            if nr == 5 || (nr == 262 && a3 & 0x1000 != 0) {
                let mut empty = true;
                if nr == 262 {
                    /* AT_EMPTY_PATH only matters for an empty path; any other path is looked up
                       as usual (the C code read at most one byte, so it failed with ENAMETOOLONG) */
                    empty = tryr!(ustr(a1, 512)).is_empty();
                }
                if nr == 5 || empty {
                    let f = fdg(a0); if f.is_null() { return -EBADF; }
                    let st = if (*f).ty == FD_SHM { shm_stat((*f).obj as *mut ShmObj) }
                             else { fill_stat((*f).node, if !(*f).node.is_null() { K::FILE } else if (*f).ty == FD_CONSOLE { K::DEV_TTY } else { K::DEV_NULL }) };
                    return if uput(vm, if nr == 5 { a1 } else { a2 }, &st) { 0 } else { -EFAULT };
                }
            }
            let path = tryr!(ustr(if nr == 262 { a1 } else { a0 }, 512));
            if path.is_empty() { return -ENOENT; }
            let (n, kind, labs) = resolve(p, if nr == 262 { a0 as c_int } else { AT_FDCWD }, &path);
            if n.is_null() && (kind == K::FILE || kind == K::PROC_SELF_EXE) { return -ENOENT; }
            let st = if kind == K::DEV_SHM {
                let o = shm_lookup(&labs[9..]); if o.is_null() { return -ENOENT; }
                let s = shm_stat(o); shm_unref(o); s
            } else if kind == K::DEV_SHM_DIR { LStat { mode: 0o41777, nlink: 2, ino: 300, dev: 0x801, ..Default::default() } }
            else { fill_stat(n, kind) };
            if uput(vm, if nr == 262 { a2 } else { a1 }, &st) { 0 } else { -EFAULT }
        }
        332 => {                                                                /* statx */
            let path = tryr!(ustr(a1, 512));
            let (n, kind);
            if path.is_empty() && a2 & 0x1000 != 0 {
                let f = fdg(a0); if f.is_null() { return -EBADF; }
                n = (*f).node; kind = if n.is_null() { K::DEV_TTY } else { K::FILE };
            } else {
                let (nn, k, _) = resolve(p, a0 as c_int, &path);
                if nn.is_null() && k == K::FILE { return -ENOENT; }
                n = nn; kind = k;
            }
            let st = fill_stat(n, kind);
            let mut sx = [0u8; 256];
            sx[0..4].copy_from_slice(&0x7FFu32.to_le_bytes());
            sx[4..8].copy_from_slice(&4096u32.to_le_bytes());
            sx[16..20].copy_from_slice(&1u32.to_le_bytes()); sx[20..24].copy_from_slice(&1000u32.to_le_bytes()); sx[24..28].copy_from_slice(&1000u32.to_le_bytes());
            sx[28..30].copy_from_slice(&(st.mode as u16).to_le_bytes());
            sx[32..40].copy_from_slice(&{ st.ino }.to_le_bytes()); sx[40..48].copy_from_slice(&{ st.size }.to_le_bytes()); sx[48..56].copy_from_slice(&{ st.blocks }.to_le_bytes());
            for k in 0..4 { sx[64 + k * 16..72 + k * 16].copy_from_slice(&{ st.mtime }.to_le_bytes()); }
            if uout(vm, (*r).r8, sx.as_ptr(), 256) { 0 } else { -EFAULT }
        }
        21 | 269 | 439 => {                                                    /* access, faccessat, faccessat2 */
            let path = tryr!(ustr(if nr == 21 { a0 } else { a1 }, 512));
            let (n, kind, _) = resolve(p, if nr == 21 { AT_FDCWD } else { a0 as c_int }, &path);
            if !n.is_null() || (kind != K::FILE && kind != K::PROC_SELF_EXE) { 0 } else { -ENOENT }
        }
        217 => { let f = fdg(a0); if f.is_null() { -EBADF } else { getdents(p, f, a1, a2) } }
        89 | 267 => {                                                          /* readlink, readlinkat */
            let path = tryr!(ustr(if nr == 89 { a0 } else { a1 }, 512));
            let (buf, cap) = if nr == 89 { (a1, a2) } else { (a2, a3) };
            /* /proc/self/fd/N names what descriptor N is open on (the C code compared the
             * WRLD path here, so this always failed with EINVAL) */
            let (kind, _, labs) = lx_path2(p, if nr == 89 { AT_FDCWD } else { a0 as c_int }, &path);
            if kind != K::PROC_SELF_EXE {
                if labs.starts_with(b"/proc/self/fd/") && labs.len() > 14 && labs[14..].iter().all(u8::is_ascii_digit) {
                    let f = lx_fd_get(p, atoi(&labs[14..]) as i64); if f.is_null() { return -ENOENT; }
                    let s = fd_link_name(f); let l = (s.len() as u64).min(cap);
                    return if uout(vm, buf, s.as_ptr(), l) { l as i64 } else { -EFAULT };
                }
                return -EINVAL;                                                 /* not a symbolic link */
            }
            let s = cstr(&(*p).exe); let l = (s.len() as u64).min(cap);
            if uout(vm, buf, s.as_ptr(), l) { l as i64 } else { -EFAULT }
        }
        79 => {                                                                 /* getcwd */
            let s = cstr(&(*p).cwd); let l = s.len() as u64 + 1;
            if l > a1 { return -ERANGE; }
            if uout(vm, a0, (*p).cwd.as_ptr(), l) { l as i64 } else { -EFAULT }
        }
        80 | 81 => {                                                            /* chdir, fchdir */
            let labs: Vec<u8>;
            if nr == 81 {
                let f = fdg(a0); if f.is_null() || (*f).ty != FD_DIR { return -ENOTDIR; }
                labs = cstr(&(*f).path).to_vec();
            } else {
                let path = tryr!(ustr(a0, 512));
                let (n, _, l) = resolve(p, AT_FDCWD, &path);
                if n.is_null() { return -ENOENT; }
                if !(*n).is_dir() { return -ENOTDIR; }
                labs = l;
            }
            setstr(&mut (*p).cwd, &labs);
            0
        }
        83 | 258 => {                                                          /* mkdir, mkdirat */
            let path = tryr!(ustr(if nr == 83 { a0 } else { a1 }, 512));
            let (_, w, _) = lx_path2(p, if nr == 83 { AT_FDCWD } else { a0 as c_int }, &path);
            if !resolve_w(&w).is_null() { return -EEXIST; }
            let zw = zstr(&w);
            if vfs_create(zw.as_ptr() as *const c_char, ptr::null(), 1).is_null() { -ENOENT } else { 0 }
        }
        87 | 84 | 263 => {                                                     /* unlink, rmdir, unlinkat */
            let path = tryr!(ustr(if nr == 263 { a1 } else { a0 }, 512));
            let (n, kind, labs) = resolve(p, if nr == 263 { a0 as c_int } else { AT_FDCWD }, &path);
            if kind == K::DEV_SHM { return shm_unlink_name(&labs[9..]); }      /* shm_unlink */
            if n.is_null() { return -ENOENT; }
            let want_dir = nr == 84 || (nr == 263 && a2 & 0x200 != 0);
            if want_dir && !(*n).is_dir() { return -ENOTDIR; }
            if !want_dir && (*n).is_dir() { return -EISDIR; }
            if (*n).is_dir() && !(*n).child().is_null() { return -ENOTEMPTY; }
            if vfs_delete(n) != 0 { 0 } else { -EACCES }
        }
        82 | 264 | 316 => {                                                    /* rename, renameat, renameat2 */
            let from = tryr!(ustr(if nr == 82 { a0 } else { a1 }, 512));
            let to = tryr!(ustr(if nr == 82 { a1 } else { a3 }, 512));
            let (n, _, _) = resolve(p, if nr == 82 { AT_FDCWD } else { a0 as c_int }, &from);
            if n.is_null() { return -ENOENT; }
            let (_, wt, _) = lx_path2(p, if nr == 82 { AT_FDCWD } else { a2 as c_int }, &to);
            let old = resolve_w(&wt);
            if !old.is_null() && old != n { if (*old).is_dir() && !(*old).child().is_null() { return -ENOTEMPTY; } vfs_delete(old); }
            let dir = crate::vfspath::dirname(&wt, 512);
            let dn = resolve_w(&dir);
            if dn.is_null() || !(*dn).is_dir() { return -ENOENT; }
            if (*n).parent() != dn && vfs_move(n, dn) == 0 { return -EXDEV; }
            let zb = zstr(basename(&wt));
            if vfs_rename(n, zb.as_ptr() as *const c_char) != 0 { 0 } else { -EACCES }
        }
        86 | 265 => {                                                          /* link, linkat */
            /* The VFS has no shared inodes, so a hard link is a copy made at link time: enough
             * for lock files and write-then-link saves, which only need the new name to appear
             * atomically and to fail if it exists. */
            let from = tryr!(ustr(if nr == 86 { a0 } else { a1 }, 512));
            let to = tryr!(ustr(if nr == 86 { a1 } else { a3 }, 512));
            let (n, _, _) = resolve(p, if nr == 86 { AT_FDCWD } else { a0 as c_int }, &from);
            if n.is_null() { return -ENOENT; }
            if (*n).is_dir() { return -EPERM; }
            let (_, wt, _) = lx_path2(p, if nr == 86 { AT_FDCWD } else { a2 as c_int }, &to);
            if !resolve_w(&wt).is_null() { return -EEXIST; }
            let zw = zstr(&wt);
            if vfs_write_file(zw.as_ptr() as *const c_char, (*n).data() as *const c_void, (*n).size()).is_null() { -ENOENT } else { 0 }
        }
        76 | 77 => {                                                           /* truncate, ftruncate */
            let n: *mut Vnode;
            if nr == 77 {
                let f = fdg(a0); if f.is_null() { return -EBADF; }
                if (*f).ty == FD_SHM { return if (*f).oflags & O_ACCMODE != 0 { shm_resize((*f).obj as *mut ShmObj, a1) } else { -EINVAL }; }
                if (*f).node.is_null() { return -EBADF; }
                n = (*f).node;
            } else {
                let path = tryr!(ustr(a0, 512));
                let (nn, _, _) = resolve(p, AT_FDCWD, &path);
                if nn.is_null() { return -ENOENT; }
                n = nn;
            }
            if (*n).is_dir() { return -EISDIR; }
            let ns = a1;
            let size = (*n).size() as u64;
            if ns > 0x7FFF_FFFF { return -EFBIG; }
            if ns > size && ns - size > LX_FILE_GROW { return -EFBIG; }
            let b = kmalloc(if ns != 0 { ns as usize } else { 1 }) as *mut u8;
            if b.is_null() { return -ENOMEM; }
            ptr::copy((*n).data(), b, ns.min(size) as usize);
            if ns > size { ptr::write_bytes(b.add(size as usize), 0, (ns - size) as usize); }
            let ok = vfs_write_node(n, b as *const c_void, ns as u32); kfree(b as *mut c_void);
            if ok != 0 { 0 } else { -ENOSPC }
        }
        74 | 75 => if fdg(a0).is_null() { -EBADF } else { 0 },                  /* fsync, fdatasync */
        137 | 138 => {                                                         /* statfs, fstatfs */
            let mut sf = [0u64; 15];
            sf[0] = 0xEF53; sf[1] = 4096; sf[2] = 1 << 20; sf[3] = 1 << 19; sf[4] = 1 << 19; sf[5] = 1 << 20; sf[6] = 1 << 19; sf[8] = 255; sf[9] = 4096;
            if uout(vm, a1, sf.as_ptr(), 120) { 0 } else { -EFAULT }
        }
        90 | 91 | 92 | 93 | 94 | 268 | 260 | 280 | 235 | 132 => 0,              /* chmod, fchmod, chown... utimes */
        _ => { *handled = false; -ENOSYS }
    }
}
