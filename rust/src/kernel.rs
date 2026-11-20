//! The bridge to the C kernel: its heap as Rust's global allocator, `klog` as a log
//! macro, kernel panics for Rust panics, and the C entry points of the Rust modules.

use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_int, c_void};
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::inflate;

extern "C" {
    fn kmalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
    fn krealloc(p: *mut c_void, n: usize) -> *mut c_void;
    fn kmalloc_aligned(n: usize, align: usize) -> *mut c_void;
    fn kfree_aligned(p: *mut c_void);
    fn klog(fmt: *const c_char, ...);
    fn panic(fmt: *const c_char, ...) -> !;
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
    klog!("rust: wrldrs {} ready (heap {}; modules: inflate)", env!("CARGO_PKG_VERSION"), if ok { "ok" } else { "FAILED" });
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
