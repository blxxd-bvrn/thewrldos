//! The bridge to the C kernel: its heap as Rust's global allocator, `klog` as a log
//! macro, kernel panics for Rust panics, and the C entry points of the Rust modules.

use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_int, c_void};
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::{dmg, fat, inflate, net, netstack};
use crate::netstack::{Netif, Stack, Tcb, St, DnsState, FwConfig, FwEvent, UdpFn};

extern "C" {
    fn kmalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
    fn krealloc(p: *mut c_void, n: usize) -> *mut c_void;
    fn kmalloc_aligned(n: usize, align: usize) -> *mut c_void;
    fn kfree_aligned(p: *mut c_void);
    fn klog(fmt: *const c_char, ...);
    fn panic(fmt: *const c_char, ...) -> !;
    fn vfs_new_node(parent: *mut Vnode, name: *const c_char, dir: CBool) -> *mut Vnode;
    fn vfs_mount(root: *mut Vnode, label: *const c_char, kind: c_int) -> c_char;
    static rust_vnode_layout: [u32; 8];
    static rust_net_layout: [u32; 12];
}

// ---------------------------------------------------------------- heap

/// kmalloc hands out 16-byte aligned blocks (src/kernel/mm.c); larger alignments go through
/// kmalloc_aligned, which keeps the raw pointer just below the block.
const KMALLOC_ALIGN: usize = 16;

struct KernelHeap;

unsafe impl GlobalAlloc for KernelHeap {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let n = l.size().max(1);
        if l.align() <= KMALLOC_ALIGN { kmalloc(n) as *mut u8 } else { kmalloc_aligned(n, l.align()) as *mut u8 }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        if l.align() <= KMALLOC_ALIGN { kfree(p as *mut c_void) } else { kfree_aligned(p as *mut c_void) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        if l.align() <= KMALLOC_ALIGN {
            return krealloc(p as *mut c_void, new.max(1)) as *mut u8;
        }
        let q = self.alloc(Layout::from_size_align_unchecked(new, l.align()));
        if !q.is_null() {
            core::ptr::copy_nonoverlapping(p, q, l.size().min(new));
            self.dealloc(p, l);
        }
        q
    }
}

#[global_allocator]
static HEAP: KernelHeap = KernelHeap;

// ---------------------------------------------------------------- log

/// Formats into a fixed buffer (no allocation, so it works when the heap is short);
/// longer messages are cut.
pub struct LineBuf { buf: [u8; 256], len: usize }

impl LineBuf {
    pub const fn new() -> Self { LineBuf { buf: [0; 256], len: 0 } }
    pub fn as_bytes(&self) -> &[u8] { &self.buf[..self.len] }
}

impl Write for LineBuf {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let room = self.buf.len() - 1 - self.len;                // keep one byte for the NUL
        let n = s.len().min(room);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

pub fn log_fmt(args: fmt::Arguments) {
    let mut b = LineBuf::new();
    let _ = b.write_fmt(args);
    let s = b.as_bytes();
    unsafe { klog(c"%.*s".as_ptr(), s.len() as c_int, s.as_ptr()) };
}

/// `klog!("x = {}", x)`: a line in the kernel log (the serial port), like C's klog().
#[macro_export]
macro_rules! klog {
    ($($arg:tt)*) => { $crate::kernel::log_fmt(format_args!($($arg)*)) };
}

#[panic_handler]
fn on_panic(info: &core::panic::PanicInfo) -> ! {
    let mut b = LineBuf::new();
    let _ = write!(b, "Rust: {}", info);
    let s = b.as_bytes();
    unsafe { panic(c"%.*s".as_ptr(), s.len() as c_int, s.as_ptr()) }
}

// ---------------------------------------------------------------- start-up

/// Called once by kmain after the heap is up: checks that allocation works and says so.
#[no_mangle]
pub extern "C" fn rust_init() {
    let mut v: Vec<u32> = Vec::new();
    for i in 0..1000u32 { v.push(i * 3); }
    let ok = v.iter().copied().sum::<u32>() == 3 * 999 * 1000 / 2;
    if unsafe { rust_net_layout } != net_layout() {
        unsafe { panic(c"Rust: the net_*_t structs in rust/src/kernel.rs do not match src/include/net.h".as_ptr()) }
    }
    if !vnode_layout_ok() {
        unsafe { panic(c"Rust: vnode_t in rust/src/kernel.rs does not match src/include/fs.h".as_ptr()) }
    }
    klog!("rust: wrldrs {} ready (heap {}; modules: inflate, fat, dmg, net, netstack)", env!("CARGO_PKG_VERSION"), if ok { "ok" } else { "FAILED" });
}

// ---------------------------------------------------------------- inflate (src/include/fs.h)

/// Input bytes the last inflate_raw() consumed: src/pkg/unpack.c steps over gzip members
/// with it. Same layout as the C `uint32_t inflate_used`.
#[no_mangle]
pub static inflate_used: AtomicU32 = AtomicU32::new(0);

/// Slices for a C buffer; a null pointer is an empty buffer.
unsafe fn in_slice<'a>(p: *const u8, n: u32) -> &'a [u8] {
    if p.is_null() || n == 0 { &[] } else { core::slice::from_raw_parts(p, n as usize) }
}
unsafe fn out_slice<'a>(p: *mut u8, n: u32) -> &'a mut [u8] {
    if p.is_null() || n == 0 { &mut [] } else { core::slice::from_raw_parts_mut(p, n as usize) }
}

unsafe fn finish(r: Result<inflate::Done, inflate::Error>, outlen: *mut u32) -> c_int {
    match r {
        Ok(d) => {
            if !outlen.is_null() { *outlen = d.written as u32; }
            inflate_used.store(d.consumed as u32, Ordering::Relaxed);
            0
        }
        Err(_) => -1,
    }
}

/// int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t cap, uint32_t *outlen):
/// a raw DEFLATE stream into dst; 0, or -1 on bad data or a full buffer.
#[no_mangle]
pub unsafe extern "C" fn inflate_raw(src: *const u8, srclen: u32, dst: *mut u8, cap: u32, outlen: *mut u32) -> c_int {
    finish(inflate::inflate_raw(in_slice(src, srclen), out_slice(dst, cap)), outlen)
}

/// int inflate_zlib(...): the same for a zlib stream (RFC 1950 header, no checksum check).
#[no_mangle]
pub unsafe extern "C" fn inflate_zlib(src: *const u8, srclen: u32, dst: *mut u8, cap: u32, outlen: *mut u32) -> c_int {
    finish(inflate::inflate_zlib(in_slice(src, srclen), out_slice(dst, cap)), outlen)
}

// ---------------------------------------------------------------- unwinding stubs

// The 32-bit edition links the prebuilt core/alloc of i586-unknown-linux-gnu, which were
// compiled for unwinding and so name these. With panic=abort nothing unwinds (the panic
// handler above never returns), so they are never called.
#[cfg(target_os = "linux")]
#[no_mangle]
pub extern "C" fn _Unwind_Resume() -> ! {
    unsafe { panic(c"Rust: _Unwind_Resume called".as_ptr()) }
}
#[cfg(target_os = "linux")]
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}

// ---------------------------------------------------------------- the VFS (src/include/fs.h)

pub use crate::CBool;

/// rtc_time_t
#[repr(C)]
pub struct RtcTime { year: c_int, month: c_int, day: c_int, hour: c_int, minute: c_int, second: c_int, weekday: c_int }

/// vnode_t; rust_init() checks this against the C definition.
#[repr(C)]
pub struct Vnode {
    name: [c_char; 96],
    dir: CBool,
    readonly: CBool,
    data: *mut u8,
    size: u32,
    cap: u32,
    parent: *mut Vnode,
    child: *mut Vnode,
    next: *mut Vnode,
    mtime: RtcTime,
    drive: c_int,
    dirty: CBool,
}

fn vnode_layout_ok() -> bool {
    use core::mem::{offset_of, size_of};
    let want = unsafe { rust_vnode_layout };
    want == [size_of::<Vnode>() as u32, offset_of!(Vnode, dir) as u32, offset_of!(Vnode, readonly) as u32, offset_of!(Vnode, data) as u32,
             offset_of!(Vnode, size) as u32, offset_of!(Vnode, parent) as u32, offset_of!(Vnode, mtime) as u32, offset_of!(Vnode, dirty) as u32]
}

/// A NUL-terminated copy of `s` in a stack buffer (names are at most 95 bytes in the VFS).
struct CName([u8; 96]);
impl CName {
    fn new(s: &str) -> Self {
        let mut b = [0u8; 96];
        let n = s.len().min(95);
        b[..n].copy_from_slice(&s.as_bytes()[..n]);
        CName(b)
    }
    fn ptr(&self) -> *const c_char { self.0.as_ptr() as *const c_char }
}

/// Copies `s` into a C buffer of `cap` bytes, cut to fit, NUL-terminated (like snprintf).
unsafe fn put_cstr(buf: *mut c_char, cap: c_int, s: &str) {
    if buf.is_null() || cap <= 0 { return; }
    let n = s.len().min(cap as usize - 1);
    core::ptr::copy_nonoverlapping(s.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
}

/// Hands a buffer to C: a kmalloc'd block (our allocator) that C frees with kfree, or NULL.
fn into_c_buffer(v: Vec<u8>) -> *mut u8 {
    if v.is_empty() { return core::ptr::null_mut(); }
    alloc::boxed::Box::into_raw(v.into_boxed_slice()) as *mut u8
}

/// Builds VFS nodes from a FAT volume: read-only, with the volume's dates.
struct VfsBuilder;
impl fat::Builder for VfsBuilder {
    type Node = *mut Vnode;
    fn dir(&mut self, parent: *mut Vnode, name: &str, t: fat::DosTime) -> *mut Vnode {
        node(parent, name, true, t)
    }
    fn file(&mut self, parent: *mut Vnode, name: &str, t: fat::DosTime, data: Vec<u8>) {
        let n = node(parent, name, false, t);
        if n.is_null() { return; }
        unsafe {
            (*n).size = data.len() as u32;
            (*n).cap = data.len() as u32;
            (*n).data = into_c_buffer(data);
        }
    }
}
fn node(parent: *mut Vnode, name: &str, dir: bool, t: fat::DosTime) -> *mut Vnode {
    let n = unsafe { vfs_new_node(parent, CName::new(name).ptr(), dir as CBool) };
    if !n.is_null() {
        unsafe {
            (*n).readonly = 1;
            (*n).mtime = RtcTime { year: t.year as c_int, month: t.month as c_int, day: t.day as c_int,
                                   hour: t.hour as c_int, minute: t.minute as c_int, second: t.second as c_int, weekday: 0 };
        }
    }
    n
}

/// Mounts the FAT volume in `img` as a new VFS tree: its root, or None with the reason.
fn mount_fat(img: &[u8]) -> Result<(*mut Vnode, fat::Info), &'static str> {
    let vol = fat::Volume::open(img)?;
    let root = unsafe { vfs_new_node(core::ptr::null_mut(), c"X:".as_ptr(), 1) };
    if root.is_null() { return Err("Not enough memory"); }
    let info = vol.load(root, &mut VfsBuilder);
    unsafe { (*root).readonly = 1; }
    klog!("fat: mounted FAT{} volume '{}' ({} clusters){}", info.bits, info.label, info.clusters,
          if info.damaged { "; the image is damaged, some entries were left out" } else { "" });
    Ok((root, info))
}

/// vnode_t *fat_mount_image(const uint8_t *img, uint32_t size, char *label_out, int label_cap, char *err, int errcap)
#[no_mangle]
pub unsafe extern "C" fn fat_mount_image(img: *const u8, size: u32, label: *mut c_char, lcap: c_int, err: *mut c_char, ecap: c_int) -> *mut Vnode {
    match mount_fat(in_slice(img, size)) {
        Ok((root, info)) => { put_cstr(label, lcap, &info.label); root }
        Err(e) => { put_cstr(err, ecap, e); core::ptr::null_mut() }
    }
}

/// uint8_t *dmg_extract(const uint8_t *file, uint32_t size, uint32_t *out_size, char *info, int icap):
/// the decoded disk (kfree it), or NULL with the reason in info.
#[no_mangle]
pub unsafe extern "C" fn dmg_extract(file: *const u8, size: u32, out_size: *mut u32, info: *mut c_char, icap: c_int) -> *mut u8 {
    match dmg::extract(in_slice(file, size)) {
        Ok(d) => {
            put_cstr(info, icap, &d.info);
            if !out_size.is_null() { *out_size = d.bytes.len() as u32; }
            let p = into_c_buffer(d.bytes);
            if p.is_null() { put_cstr(info, icap, "Empty disk image"); }
            p
        }
        Err(e) => { put_cstr(info, icap, &e); core::ptr::null_mut() }
    }
}

/// char dmg_mount(vnode_t *file, char *msg, int mcap): mounts a disk image file's FAT volume
/// as a new drive; its letter, or 0 with the reason in msg.
#[no_mangle]
pub unsafe extern "C" fn dmg_mount(file: *mut Vnode, msg: *mut c_char, mcap: c_int) -> c_char {
    let mut out = alloc::string::String::new();
    let letter = (|| -> Result<c_char, ()> {
        if file.is_null() { return Err(()); }
        let disk = dmg::extract(in_slice((*file).data, (*file).size)).map_err(|e| out = e)?;
        let (off, len) = dmg::find_partition(&disk.bytes).map_err(|why| {
            out = alloc::format!("{}. {} - this file system is not supported yet (FAT12/16/32 volumes are).", disk.info, why);
        })?;
        let (root, info) = mount_fat(&disk.bytes[off..off + len])
            .map_err(|e| out = alloc::format!("{}. Could not mount volume: {}", disk.info, e))?;
        let l = vfs_mount(root, CName::new(&info.label).ptr(), 2);
        if l == 0 { out = alloc::string::String::from("No free drive letters"); return Err(()); }
        out = alloc::format!("Mounted '{}' as {}: ({})", info.label, l as u8 as char, disk.info);
        klog!("dmg: {}", out);
        Ok(l)
    })();
    put_cstr(msg, mcap, &out);
    letter.unwrap_or(0)
}

// ---------------------------------------------------------------- network parsers (src/include/net.h)

#[repr(C)] pub struct NetArp { op: u32, spa: u32, tpa: u32, sha: [u8; 8] }
#[repr(C)] pub struct NetIpv4 { src: u32, dst: u32, proto: u32, off: u32, len: u32 }
#[repr(C)] pub struct NetUdp { sport: u32, dport: u32, off: u32, len: u32 }
#[repr(C)] pub struct NetTcp { sport: u32, dport: u32, seq: u32, ack: u32, flags: u32, win: u32, mss: u32, hl: u32 }
#[repr(C)] pub struct NetDhcp { kind: u32, yiaddr: u32, mask: u32, gw: u32, dns: u32, server: u32, lease: u32 }
fn net_layout() -> [u32; 12] {
    use core::mem::{offset_of, size_of};
    [size_of::<NetArp>() as u32, size_of::<NetIpv4>() as u32, size_of::<NetUdp>() as u32, size_of::<NetTcp>() as u32,
     size_of::<NetDhcp>() as u32, size_of::<Netif>() as u32, offset_of!(Netif, mac) as u32, offset_of!(Netif, ip) as u32,
     offset_of!(Netif, rx_dropped) as u32, size_of::<FwEvent>() as u32, size_of::<NicOps>() as u32, size_of::<PciDev>() as u32]
}

/// Stores `v` through `out` when both are there; the C bool result.
unsafe fn give<T>(out: *mut T, v: Option<T>) -> CBool {
    match v {
        Some(v) if !out.is_null() => { out.write(v); 1 }
        _ => 0,
    }
}

/// uint16_t ip_checksum(const void *data, uint32_t len, uint32_t initial)
#[no_mangle]
pub unsafe extern "C" fn ip_checksum(data: *const u8, len: u32, initial: u32) -> u16 {
    net::checksum(in_slice(data, len), initial)
}

/// bool str_to_ip(const char *s, uint32_t *ip)
#[no_mangle]
pub unsafe extern "C" fn str_to_ip(s: *const c_char, ip: *mut u32) -> CBool {
    if s.is_null() { return 0; }
    give(ip, net::parse_ipv4_addr(core::ffi::CStr::from_ptr(s).to_bytes()))
}

#[no_mangle]
pub unsafe extern "C" fn net_parse_arp(p: *const u8, len: u32, out: *mut NetArp) -> CBool {
    let mut sha = [0u8; 8];
    give(out, net::arp(in_slice(p, len)).map(|a| { sha[..6].copy_from_slice(&a.sha); NetArp { op: a.op as u32, spa: a.spa, tpa: a.tpa, sha } }))
}

#[no_mangle]
pub unsafe extern "C" fn net_parse_ipv4(p: *const u8, len: u32, out: *mut NetIpv4) -> CBool {
    give(out, net::ipv4(in_slice(p, len)).map(|i| NetIpv4 { src: i.src, dst: i.dst, proto: i.proto as u32, off: i.off as u32, len: i.len as u32 }))
}

#[no_mangle]
pub unsafe extern "C" fn net_parse_udp(p: *const u8, len: u32, out: *mut NetUdp) -> CBool {
    give(out, net::udp(in_slice(p, len)).map(|u| NetUdp { sport: u.sport as u32, dport: u.dport as u32, off: u.off as u32, len: u.len as u32 }))
}

#[no_mangle]
pub unsafe extern "C" fn net_parse_tcp(p: *const u8, len: u32, out: *mut NetTcp) -> CBool {
    give(out, net::tcp(in_slice(p, len)).map(|t| NetTcp {
        sport: t.sport as u32, dport: t.dport as u32, seq: t.seq, ack: t.ack, flags: t.flags as u32,
        win: t.win as u32, mss: t.mss as u32, hl: t.hl as u32,
    }))
}

#[no_mangle]
pub unsafe extern "C" fn net_parse_dhcp(m: *const u8, len: u32, xid: u32, out: *mut NetDhcp) -> CBool {
    give(out, net::dhcp(in_slice(m, len), xid).map(|d| NetDhcp {
        kind: d.kind as u32, yiaddr: d.yiaddr, mask: d.mask, gw: d.gw, dns: d.dns, server: d.server, lease: d.lease,
    }))
}

/// int net_parse_dns(const uint8_t *m, uint32_t len, uint16_t id, uint32_t *ip, int *rcode)
#[no_mangle]
pub unsafe extern "C" fn net_parse_dns(m: *const u8, len: u32, id: u16, ip: *mut u32, rcode: *mut c_int) -> c_int {
    match net::dns(in_slice(m, len), id) {
        net::Dns::NotOurs => -1,
        net::Dns::Address(a) => { if !ip.is_null() { *ip = a; } 1 }
        net::Dns::NoAddress { rcode: r } => { if !rcode.is_null() { *rcode = r as c_int; } 0 }
    }
}

// ---------------------------------------------------------------- the network stack (src/include/net.h)

extern "C" {
    fn timer_ticks() -> u32;
    fn task_sleep(ms: u32);
    fn rand_next() -> u32;
    fn task_create(name: *const c_char, entry: extern "C" fn(*mut c_void), arg: *mut c_void, stack: u32) -> *mut c_void;
    fn vfs_read_file(path: *const c_char, size: *mut u32) -> *mut u8;
    fn pci_list(out: *mut *mut PciDev) -> c_int;
    static computer_name: [c_char; 32];
    fn virtionet_probe(ops: *mut NicOps) -> CBool;
    fn vmxnet3_probe(ops: *mut NicOps) -> CBool;
    fn e1000_probe(ops: *mut NicOps) -> CBool;
    fn igb_probe(ops: *mut NicOps) -> CBool;
    fn rtl8169_probe(ops: *mut NicOps) -> CBool;
    fn rtl8139_probe(ops: *mut NicOps) -> CBool;
    fn pcnet_probe(ops: *mut NicOps) -> CBool;
}

/// nic_ops_t: what a network card driver's probe fills in.
#[repr(C)]
pub struct NicOps {
    name: *const c_char,
    send: Option<unsafe extern "C" fn(*const c_void, u32) -> CBool>,
    poll: Option<unsafe extern "C" fn()>,
    link_up: Option<unsafe extern "C" fn() -> CBool>,
}

/// pci_dev_t
#[repr(C)]
pub struct PciDev { bus: u8, dev: u8, func: u8, vendor: u16, device: u16, cls: u8, sub: u8, prog: u8 }

/// The interface's settings, shared with C (drivers, shell, Security Center).
#[no_mangle]
pub static mut netif: Netif = Netif::new();
/// The inbound firewall's switches (Security Center, saved settings).
#[no_mangle]
pub static mut fw_enabled: CBool = 0;
#[no_mangle]
pub static mut fw_block_tcp: CBool = 1;
#[no_mangle]
pub static mut fw_block_udp: CBool = 1;
#[no_mangle]
pub static mut fw_blocked: u32 = 0;

static mut NIC: NicOps = NicOps { name: core::ptr::null(), send: None, poll: None, link_up: None };

/// The stack, used by one task at a time. Kernel tasks switch only when they block, and no
/// code here blocks while it holds the stack, so `with_stack` is never entered twice; the
/// flag turns a mistake into a clear kernel panic instead of two live &mut to the stack.
struct StackCell { busy: core::cell::Cell<bool>, st: core::cell::UnsafeCell<Stack> }
unsafe impl Sync for StackCell {}
static STACK: StackCell = StackCell { busy: core::cell::Cell::new(false), st: core::cell::UnsafeCell::new(Stack::new()) };

struct KernelLink;
impl netstack::Link for KernelLink {
    fn now(&self) -> u32 { unsafe { timer_ticks() } }
    fn rand(&mut self) -> u32 { unsafe { rand_next() ^ (rdtsc() as u32) } }
    fn send(&mut self, frame: &[u8]) -> bool {
        match unsafe { NIC.send } { Some(f) => unsafe { f(frame.as_ptr() as *const c_void, frame.len() as u32) != 0 }, None => false }
    }
}
fn rdtsc() -> u64 {
    #[cfg(target_arch = "x86_64")] unsafe { core::arch::x86_64::_rdtsc() }
    #[cfg(target_arch = "x86")] unsafe { core::arch::x86::_rdtsc() }
}

fn with_stack<R>(f: impl FnOnce(&mut Stack, &mut netstack::Io) -> R) -> R {
    if STACK.busy.replace(true) { unsafe { panic(c"Rust: the network stack was entered twice".as_ptr()) } }
    let name = unsafe { &*core::ptr::addr_of!(computer_name) };
    let len = name.iter().position(|&c| c == 0).unwrap_or(name.len());
    let host = unsafe { core::slice::from_raw_parts(name.as_ptr() as *const u8, len) };
    let mut link = KernelLink;
    let mut io = netstack::Io { nif: unsafe { &mut *core::ptr::addr_of_mut!(netif) }, link: &mut link, hostname: host };
    let r = f(unsafe { &mut *STACK.st.get() }, &mut io);
    STACK.busy.set(false);
    r
}
fn nif() -> &'static Netif { unsafe { &*core::ptr::addr_of!(netif) } }
fn now() -> u32 { unsafe { timer_ticks() } }
fn sleep(ms: u32) { unsafe { task_sleep(ms) } }

/// A dotted quad for messages.
struct Ip(u32);
impl fmt::Display for Ip {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result { let a = self.0.to_be_bytes(); write!(f, "{}.{}.{}.{}", a[0], a[1], a[2], a[3]) }
}
/// A message into a C buffer.
unsafe fn put_fmt(buf: *mut c_char, cap: c_int, args: fmt::Arguments) {
    let mut b = LineBuf::new();
    let _ = b.write_fmt(args);
    put_cstr(buf, cap, core::str::from_utf8(b.as_bytes()).unwrap_or(""));
}

/// void ip_to_str(uint32_t ip, char *buf), buf >= 16
#[no_mangle]
pub unsafe extern "C" fn ip_to_str(ip: u32, buf: *mut c_char) { put_fmt(buf, 16, format_args!("{}", Ip(ip))); }

// ---- the interface and the network task

#[no_mangle]
pub unsafe extern "C" fn net_rx(frame: *const u8, len: u32) {
    let f = in_slice(frame, len);
    let fw = FwConfig { enabled: fw_enabled != 0, block_tcp: fw_block_tcp != 0, block_udp: fw_block_udp != 0 };
    let r = with_stack(|st, io| st.rx(io, f, fw));
    fw_blocked = fw_blocked.wrapping_add(r.blocked);
    // a C handler is called with the stack free, so it can send in reply
    if let Some(d) = r.delivery {
        (d.f)(d.src, d.sport, f.as_ptr().add(d.off), d.len as u32, d.ctx as *mut c_void);
    }
}

extern "C" fn net_loop(_arg: *mut c_void) {
    let mut was_up = false;
    loop {
        unsafe {
            if let Some(p) = NIC.poll { p(); }
            let up = NIC.link_up.is_some_and(|f| f() != 0);
            if up != was_up {
                was_up = up;
                (*core::ptr::addr_of_mut!(netif)).link = up as CBool;
                klog!("net: link {}", if up { "up" } else { "down" });
                if up { with_stack(|st, io| st.dhcp_renew(io)); } else { (*core::ptr::addr_of_mut!(netif)).configured = 0; }
            }
        }
        let (bound, conns) = with_stack(|st, io| { st.tick(io); (core::mem::take(&mut st.dhcp_bound_event), st.tcp.count()) });
        if bound {
            let n = nif();
            klog!("dhcp: address {}, gateway {}, DNS {}, lease {}s", Ip(n.ip), Ip(n.gw), Ip(n.dns), n.lease_s);
        }
        sleep(if conns > 0 { 1 } else { 3 });
    }
}

/// void net_init(void): finds a network card, then starts the network task and DHCP.
#[no_mangle]
pub unsafe extern "C" fn net_init() {
    *core::ptr::addr_of_mut!(netif) = Netif::new();
    // first match wins: paravirtual cards first, then the emulated/real ones
    let probes: [unsafe extern "C" fn(*mut NicOps) -> CBool; 7] = [virtionet_probe, vmxnet3_probe, e1000_probe, igb_probe, rtl8169_probe, rtl8139_probe, pcnet_probe];
    let mut found = false;
    for p in probes {
        *core::ptr::addr_of_mut!(NIC) = NicOps { name: core::ptr::null(), send: None, poll: None, link_up: None };
        if p(core::ptr::addr_of_mut!(NIC)) != 0 { found = true; break; }
    }
    let n = &mut *core::ptr::addr_of_mut!(netif);
    if !found {
        let mut devs: *mut PciDev = core::ptr::null_mut();
        let count = pci_list(&mut devs);
        for i in 0..count.max(0) as usize {
            let d = &*devs.add(i);
            if d.cls == 0x02 { klog!("net: no driver for network adapter {:04x}:{:04x}", d.vendor, d.device); }
        }
        klog!("net: no supported network adapter found");
        put_cstr(n.driver.as_mut_ptr() as *mut c_char, 48, "No supported adapter");
        return;
    }
    n.present = 1;
    with_stack(|st, _| st.dhcp_start());
    task_create(c"netd".as_ptr(), net_loop, core::ptr::null_mut(), 32768);
}

#[no_mangle]
pub extern "C" fn net_ready() -> CBool { nif().ready() as CBool }

fn wait_ready(ms: u32) -> bool {
    let t0 = now();
    while !nif().ready() {
        if nif().present == 0 || now().wrapping_sub(t0) > ms { return false; }
        sleep(20);
    }
    true
}
#[no_mangle]
pub extern "C" fn net_wait_ready(ms: u32) -> CBool { wait_ready(ms) as CBool }

#[no_mangle]
pub extern "C" fn dhcp_renew() { with_stack(|st, io| st.dhcp_renew(io)); }

static mut STATUS: [u8; 96] = [0; 96];
/// const char *net_status_text(void): one line for the UI.
#[no_mangle]
pub unsafe extern "C" fn net_status_text() -> *const c_char {
    let n = nif();
    if n.present == 0 { return c"No network adapter".as_ptr(); }
    if n.link == 0 { return c"Network cable unplugged".as_ptr(); }
    if n.configured == 0 { return c"Acquiring network address...".as_ptr(); }
    let buf = core::ptr::addr_of_mut!(STATUS) as *mut c_char;
    put_fmt(buf, 96, format_args!("Connected - {}", Ip(n.ip)));
    buf
}

// ---- IP, ICMP, UDP, firewall

#[no_mangle]
pub unsafe extern "C" fn ip_send(dst: u32, proto: u8, payload: *const u8, len: u32) -> CBool {
    let p = in_slice(payload, len);
    with_stack(|st, io| st.core.ip_send(io, dst, proto, p)) as CBool
}

/// int icmp_ping(uint32_t dst, uint16_t seq, uint32_t timeout_ms): round-trip ms, or -1
#[no_mangle]
pub extern "C" fn icmp_ping(dst: u32, seq: u16, timeout: u32) -> c_int {
    let t0 = now();
    if !with_stack(|st, io| st.ping_send(io, dst, seq)) { return -1; }
    while now().wrapping_sub(t0) < timeout {
        if with_stack(|st, _| st.ping_answered(seq)) { return now().wrapping_sub(t0) as c_int; }
        sleep(1);
    }
    -1
}

#[no_mangle]
pub extern "C" fn net_ephemeral_port() -> u16 { with_stack(|st, io| st.ephemeral_port(io)) }
#[no_mangle]
pub extern "C" fn udp_bind(port: u16, h: Option<UdpFn>, ctx: *mut c_void) -> CBool {
    match h { Some(f) => with_stack(|st, _| st.udp_bind(port, f, ctx as usize)) as CBool, None => 0 }
}
#[no_mangle]
pub extern "C" fn udp_unbind(port: u16) { with_stack(|st, _| st.udp_unbind(port)) }
#[no_mangle]
pub unsafe extern "C" fn udp_send(dst: u32, sport: u16, dport: u16, data: *const u8, len: u32) -> CBool {
    let d = in_slice(data, len);
    with_stack(|st, io| st.udp_send(io, dst, sport, dport, d)) as CBool
}

#[no_mangle]
pub extern "C" fn fw_port_allowed(p: u16) -> CBool { with_stack(|st, _| st.fw_port_allowed(p)) as CBool }
#[no_mangle]
pub extern "C" fn fw_allow_port(p: u16) -> CBool { with_stack(|st, _| st.fw_allow_port(p)) as CBool }
#[no_mangle]
pub extern "C" fn fw_clear_ports() { with_stack(|st, _| st.fw_clear_ports()) }
#[no_mangle]
pub extern "C" fn fw_port_count() -> c_int { with_stack(|st, _| st.fw_ports().len() as c_int) }
#[no_mangle]
pub extern "C" fn fw_port_at(i: c_int) -> u16 { with_stack(|st, _| st.fw_ports().get(i as usize).copied().unwrap_or(0)) }
#[no_mangle]
pub extern "C" fn fw_remove_port(p: u16) -> CBool { with_stack(|st, _| st.fw_remove_port(p)) as CBool }
#[no_mangle]
pub extern "C" fn fw_log_count() -> c_int { with_stack(|st, _| st.fw_log_count() as c_int) }
/// const fw_event_t *fw_log_at(int i): 0 = the most recent blocked attempt (in the stack, which lives forever)
#[no_mangle]
pub extern "C" fn fw_log_at(i: c_int) -> *const FwEvent {
    if i < 0 { return core::ptr::null(); }
    with_stack(|st, _| st.fw_log_at(i as usize).map_or(core::ptr::null(), |e| e as *const FwEvent))
}

// ---- DNS

/// bool dns_resolve(const char *name, uint32_t *ip, char *err, int errcap)
#[no_mangle]
pub unsafe extern "C" fn dns_resolve(name: *const c_char, ip: *mut u32, err: *mut c_char, errcap: c_int) -> CBool {
    if name.is_null() || ip.is_null() { return 0; }
    let Ok(name) = core::ffi::CStr::from_ptr(name).to_str() else {
        put_cstr(err, errcap, "That is not a valid server name."); return 0;
    };
    if let Some(a) = net::parse_ipv4_addr(name.as_bytes()) { *ip = a; return 1; }
    if name.eq_ignore_ascii_case("localhost") { *ip = 0x7F00_0001; return 1; }
    // the hosts file
    let mut size = 0u32;
    let text = vfs_read_file(c"C:\\WRLD\\hosts".as_ptr(), &mut size);
    if !text.is_null() {
        let hit = netstack::hosts_lookup(core::slice::from_raw_parts(text, size as usize), name);
        kfree(text as *mut c_void);
        if let Some(a) = hit { *ip = a; return 1; }
    }
    if let Some(a) = with_stack(|st, _| st.dns_cache_get(name, now())) { *ip = a; return 1; }
    if !wait_ready(8000) { put_cstr(err, errcap, "The network is not connected."); return 0; }
    if nif().dns == 0 { put_cstr(err, errcap, "No DNS server is configured."); return 0; }
    let Some(slot) = with_stack(|st, io| st.dns_begin(io, name)) else {
        put_fmt(err, errcap, format_args!("The server name \"{}\" could not be looked up.", name)); return 0;
    };
    let mut state = DnsState::Waiting;
    for attempt in 0..3 {
        if attempt > 0 { with_stack(|st, io| st.dns_send(io, slot, name)); }
        let t0 = now();
        while now().wrapping_sub(t0) < 1500 {
            state = with_stack(|st, _| st.dns_state(slot));
            if state != DnsState::Waiting { break; }
            sleep(5);
        }
        if state != DnsState::Waiting { break; }
    }
    let rcode = with_stack(|st, _| { let r = st.dns_rcode(slot); st.dns_end(slot); r });
    match state {
        DnsState::Address(a) => {
            with_stack(|st, io| st.dns_cache_put(io, name, a));
            *ip = a;
            1
        }
        _ if rcode == 3 => { put_fmt(err, errcap, format_args!("The server name \"{}\" could not be found.", name)); 0 }
        _ => { put_fmt(err, errcap, format_args!("The DNS server did not answer for \"{}\".", name)); 0 }
    }
}

// ---- TCP: handles are the connections' addresses; the stack checks them before use

fn tcb<R>(h: *const Tcb, f: impl FnOnce(&mut Tcb) -> R) -> Option<R> { with_stack(|st, _| st.tcp.get(h).map(f)) }

#[no_mangle]
pub extern "C" fn tcp_count() -> c_int { with_stack(|st, _| st.tcp.count() as c_int) }

#[no_mangle]
pub extern "C" fn tcp_open_from(ip: u32, port: u16, lport: u16) -> *mut Tcb {
    if !nif().ready() { return core::ptr::null_mut(); }
    with_stack(|st, io| {
        let lp = if lport != 0 { lport } else { st.ephemeral_port(io) };
        st.tcp.open(&mut st.core, io, lp, ip, port)
    }).unwrap_or(core::ptr::null_mut())
}
#[no_mangle]
pub extern "C" fn tcp_open(ip: u32, port: u16) -> *mut Tcb { tcp_open_from(ip, port, 0) }

/// tcb_t *tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms, char *err, int errcap)
#[no_mangle]
pub unsafe extern "C" fn tcp_connect(ip: u32, port: u16, timeout: u32, err: *mut c_char, errcap: c_int) -> *mut Tcb {
    if !wait_ready(8000) { put_cstr(err, errcap, "The network is not connected."); return core::ptr::null_mut(); }
    let Some(h) = with_stack(|st, io| { let lp = st.ephemeral_port(io); st.tcp.open(&mut st.core, io, lp, ip, port) }) else {
        put_cstr(err, errcap, "Not enough memory for a connection."); return core::ptr::null_mut();
    };
    let t0 = now();
    while tcb(h, |t| t.state) == Some(St::SynSent) && now().wrapping_sub(t0) < timeout { sleep(2); }
    let (state, e) = tcb(h, |t| (t.state, t.err)).unwrap_or((St::Closed, 3));
    if state != St::Established {
        if e == 1 { put_fmt(err, errcap, format_args!("The connection to {} port {} was refused.", Ip(ip), port)); }
        else { put_fmt(err, errcap, format_args!("The connection to {} port {} timed out.", Ip(ip), port)); }
        with_stack(|st, io| st.tcp.abort(&mut st.core, io, h));
        return core::ptr::null_mut();
    }
    h
}

/// int tcp_send(tcb_t *t, const void *data, uint32_t len): waits for room; bytes queued, -1 on error
#[no_mangle]
pub unsafe extern "C" fn tcp_send(h: *mut Tcb, data: *const u8, len: u32) -> c_int {
    let d = in_slice(data, len);
    let mut done = 0usize;
    while done < d.len() {
        let n = with_stack(|st, io| st.tcp.send(&mut st.core, io, h, &d[done..]));
        if n < 0 { return if done > 0 { done as c_int } else { -1 }; }
        if n == 0 { sleep(2); continue; }
        done += n as usize;
    }
    done as c_int
}
#[no_mangle]
pub unsafe extern "C" fn tcp_send_nb(h: *mut Tcb, data: *const u8, len: u32) -> c_int {
    let d = in_slice(data, len);
    with_stack(|st, io| st.tcp.send(&mut st.core, io, h, d))
}

/// int tcp_recv(tcb_t *t, void *buf, uint32_t max, uint32_t timeout_ms): >0 bytes, 0 closed by the peer, -1 error, -2 timeout
#[no_mangle]
pub unsafe extern "C" fn tcp_recv(h: *mut Tcb, buf: *mut u8, max: u32, timeout: u32) -> c_int {
    let out = out_slice(buf, max);
    let t0 = now();
    loop {
        let r = with_stack(|st, io| -> Option<c_int> {
            let t = st.tcp.get(h)?;
            if t.rlen() > 0 { return Some(st.tcp.recv(&mut st.core, io, h, out) as c_int); }
            if t.fin_rcvd { return Some(0); }
            if t.state == St::Closed { return Some(if t.err != 0 { -1 } else { 0 }); }
            Some(i32::MIN)
        });
        match r {
            None => return -1,
            Some(i32::MIN) => {}
            Some(v) => return v,
        }
        if now().wrapping_sub(t0) > timeout { return -2; }
        sleep(1);
    }
}
#[no_mangle]
pub unsafe extern "C" fn tcp_peek(h: *mut Tcb, buf: *mut u8, max: u32) -> c_int {
    let out = out_slice(buf, max);
    tcb(h, |t| {
        if t.rlen() == 0 { return if t.fin_rcvd || t.state == St::Closed { 0 } else { -2 }; }
        t.read(out, false) as c_int
    }).unwrap_or(-1)
}
#[no_mangle]
pub extern "C" fn tcp_shutdown_wr(h: *mut Tcb) { with_stack(|st, io| st.tcp.shutdown_wr(&mut st.core, io, h)) }
#[no_mangle]
pub extern "C" fn tcp_close(h: *mut Tcb) { with_stack(|st, io| st.tcp.close(&mut st.core, io, h)) }
#[no_mangle]
pub extern "C" fn tcp_status(h: *mut Tcb) -> c_int { tcb(h, |t| t.status()).unwrap_or(-1) }
#[no_mangle]
pub extern "C" fn tcp_error(h: *mut Tcb) -> c_int { tcb(h, |t| t.err).unwrap_or(0) }
#[no_mangle]
pub extern "C" fn tcp_recv_ready(h: *mut Tcb) -> CBool { tcb(h, |t| t.recv_ready()).unwrap_or(false) as CBool }
#[no_mangle]
pub extern "C" fn tcp_send_ready(h: *mut Tcb) -> CBool { tcb(h, |t| t.send_ready()).unwrap_or(false) as CBool }
#[no_mangle]
pub extern "C" fn tcp_hup(h: *mut Tcb) -> CBool { tcb(h, |t| t.hup()).unwrap_or(false) as CBool }
#[no_mangle]
pub extern "C" fn tcp_local_port(h: *mut Tcb) -> u16 { tcb(h, |t| t.lport).unwrap_or(0) }
#[no_mangle]
pub extern "C" fn tcp_remote_port(h: *mut Tcb) -> u16 { tcb(h, |t| t.rport).unwrap_or(0) }
#[no_mangle]
pub extern "C" fn tcp_remote_ip(h: *mut Tcb) -> u32 { tcb(h, |t| t.rip).unwrap_or(0) }
#[no_mangle]
pub extern "C" fn tcp_listen(port: u16) -> c_int { with_stack(|st, _| st.tcp.listen(port)) }
#[no_mangle]
pub extern "C" fn tcp_unlisten(port: u16) { with_stack(|st, _| st.tcp.unlisten(now(), port)) }
#[no_mangle]
pub extern "C" fn tcp_accept_ready(port: u16) -> CBool { with_stack(|st, _| st.tcp.accept_ready(port)) as CBool }
#[no_mangle]
pub extern "C" fn tcp_accept(port: u16) -> *mut Tcb { with_stack(|st, _| st.tcp.accept(now(), port)).unwrap_or(core::ptr::null_mut()) }
