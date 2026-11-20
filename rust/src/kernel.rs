//! The bridge to the C kernel: its heap as Rust's global allocator, `klog` as a log
//! macro, kernel panics for Rust panics, and the C entry points of the Rust modules.

use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_int, c_void};
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::{dmg, fat, inflate};

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
    if !vnode_layout_ok() {
        unsafe { panic(c"Rust: vnode_t in rust/src/kernel.rs does not match src/include/fs.h".as_ptr()) }
    }
    klog!("rust: wrldrs {} ready (heap {}; modules: inflate, fat, dmg)", env!("CARGO_PKG_VERSION"), if ok { "ok" } else { "FAILED" });
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

/// The kernel's `bool` (src/include/types.h: `typedef int bool`): 4 bytes, not Rust's bool.
/// Use it for every C bool, in structs and in function arguments.
pub type CBool = c_int;

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
