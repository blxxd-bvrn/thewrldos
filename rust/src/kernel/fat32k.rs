//! The FAT32 driver's C functions (src/include/disk.h, was `src/kernel/fat32.c`), over
//! `crate::fat32`: the disk is a `blkdev_t` read and written with blk_read/blk_write, and a
//! loaded tree goes into the VFS.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};

use super::{into_c_buffer, put_cstr, CBool, RtcTime, Vnode};
use crate::fat32::{self, Dev, Tree, Vol};

extern "C" {
    fn blk_read(d: *mut c_void, lba: u32, count: u32, buf: *mut c_void) -> CBool;
    fn blk_write(d: *mut c_void, lba: u32, count: u32, buf: *const c_void) -> CBool;
    fn blk_zero(d: *mut c_void, lba: u32, count: u32) -> CBool;
    fn rtc_now(t: *mut RtcTime);
    fn timer_ticks() -> u32;
    fn vfs_lookup_child(dir: *mut Vnode, name: *const c_char) -> *mut Vnode;
    fn vfs_new_node(parent: *mut Vnode, name: *const c_char, dir: CBool) -> *mut Vnode;
    fn kfree(p: *mut c_void);
    fn klog(fmt: *const c_char, ...);
    static mut vfs_generation: u32;
    static vol_load_progress: Option<unsafe extern "C" fn(u64, u64)>;
}

/// A disk through the block layer.
pub struct KDev(*mut c_void);
impl Dev for KDev {
    fn read(&mut self, lba: u32, count: u32, buf: &mut [u8]) -> bool {
        buf.len() >= count as usize * 512 && unsafe { blk_read(self.0, lba, count, buf.as_mut_ptr() as *mut c_void) != 0 }
    }
    fn write(&mut self, lba: u32, count: u32, buf: &[u8]) -> bool {
        buf.len() >= count as usize * 512 && unsafe { blk_write(self.0, lba, count, buf.as_ptr() as *const c_void) != 0 }
    }
    fn zero(&mut self, lba: u32, count: u32) -> bool { unsafe { blk_zero(self.0, lba, count) != 0 } }
    fn now(&self) -> (u16, u16) {
        let mut t = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0, weekday: 0 };
        unsafe { rtc_now(&mut t) };
        fat32::dos_time(t.year as u32, t.month as u32, t.day as u32, t.hour as u32, t.minute as u32, t.second as u32)
    }
}

type FatVol = Vol<KDev>;

unsafe fn vol<'a>(v: *mut FatVol) -> Option<&'a mut FatVol> { v.as_mut() }
unsafe fn cpath<'a>(p: *const c_char) -> &'a [u8] {
    if p.is_null() { return &[]; }
    core::ffi::CStr::from_ptr(p).to_bytes()
}
/// A name as a NUL-terminated C string.
fn cname(n: &[u8]) -> Vec<u8> { let mut v = n.to_vec(); v.push(0); v }

#[no_mangle]
pub unsafe extern "C" fn fat32_mount(dev: *mut c_void, lba: u32, sectors: u32) -> *mut FatVol {
    match Vol::mount(KDev(dev), lba, sectors) {
        Ok(v) => {
            klog(c"fat32: mounted \"%s\" serial %08x, %u clusters of %u bytes, %u MB free".as_ptr(), v.label().as_ptr(), v.serial(),
                 v.nclus(), v.cluster_size(), (v.free_bytes() / (1024 * 1024)) as u32);
            Box::into_raw(Box::new(v))
        }
        Err(e) => {
            if e != "no boot sector" && e != "not FAT32" { klog(c"fat32: not mounted: %s".as_ptr(), cname(e.as_bytes()).as_ptr()); }
            core::ptr::null_mut()
        }
    }
}
#[no_mangle]
pub unsafe extern "C" fn fat32_unmount(v: *mut FatVol) { if !v.is_null() { drop(Box::from_raw(v)); } }
#[no_mangle]
pub unsafe extern "C" fn fat32_serial(v: *mut FatVol) -> u32 { vol(v).map(|v| v.serial()).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn fat32_label(v: *mut FatVol) -> *const c_char { vol(v).map(|v| v.label().as_ptr() as *const c_char).unwrap_or(c"".as_ptr()) }
#[no_mangle]
pub unsafe extern "C" fn fat32_free_bytes(v: *mut FatVol) -> u64 { vol(v).map(|v| v.free_bytes()).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn fat32_total_bytes(v: *mut FatVol) -> u64 { vol(v).map(|v| v.total_bytes()).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn fat32_dev(v: *mut FatVol) -> *mut c_void { vol(v).map(|v| v.dev.0).unwrap_or(core::ptr::null_mut()) }
#[no_mangle]
pub unsafe extern "C" fn fat32_exists(v: *mut FatVol, path: *const c_char) -> CBool { vol(v).map(|v| v.exists(cpath(path)) as CBool).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn fat32_mkdir(v: *mut FatVol, path: *const c_char) -> CBool { vol(v).map(|v| v.mkdir(cpath(path)) as CBool).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn fat32_write(v: *mut FatVol, path: *const c_char, data: *const c_void, len: u32, readonly: CBool) -> CBool {
    let Some(v) = vol(v) else { return 0 };
    let d = if data.is_null() || len == 0 { &[][..] } else { core::slice::from_raw_parts(data as *const u8, len as usize) };
    v.write(cpath(path), d, readonly != 0) as CBool
}
#[no_mangle]
pub unsafe extern "C" fn fat32_delete(v: *mut FatVol, path: *const c_char) -> CBool { vol(v).map(|v| v.delete(cpath(path)) as CBool).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn fat32_rename(v: *mut FatVol, from: *const c_char, to: *const c_char) -> CBool {
    vol(v).map(|v| v.rename(cpath(from), cpath(to)) as CBool).unwrap_or(0)
}
/// A file's contents in a kmalloc'd buffer with a NUL after them (the caller kfrees it).
#[no_mangle]
pub unsafe extern "C" fn fat32_read(v: *mut FatVol, path: *const c_char, len: *mut u32) -> *mut u8 {
    let Some(v) = vol(v) else { return core::ptr::null_mut() };
    let Some(mut d) = v.read(cpath(path)) else { return core::ptr::null_mut() };
    let n = d.len() as u32;
    if d.try_reserve_exact(1).is_err() { return core::ptr::null_mut(); }
    d.push(0);
    if !len.is_null() { *len = n; }
    into_c_buffer(d)
}
#[no_mangle]
pub unsafe extern "C" fn fat32_sync(v: *mut FatVol) { if let Some(v) = vol(v) { v.sync(); } }

/// Puts a volume's folders and files into the VFS (reusing nodes that are already there).
struct VfsTree { last_tick: u32 }
impl VfsTree {
    unsafe fn node(parent: *mut Vnode, name: &[u8], dir: bool, mdate: u16, mtime: u16) -> *mut Vnode {
        if parent.is_null() { return parent; }
        let c = cname(name);
        let mut n = vfs_lookup_child(parent, c.as_ptr() as *const c_char);
        if !n.is_null() && ((*n).dir != 0) != dir { n = core::ptr::null_mut(); }
        if n.is_null() { n = vfs_new_node(parent, c.as_ptr() as *const c_char, dir as CBool); }
        if !n.is_null() {
            (*n).mtime = RtcTime { year: 1980 + (mdate >> 9) as c_int, month: ((mdate >> 5) & 15) as c_int, day: (mdate & 31) as c_int,
                                   hour: (mtime >> 11) as c_int, minute: ((mtime >> 5) & 63) as c_int, second: ((mtime & 31) * 2) as c_int, weekday: (*n).mtime.weekday };
        }
        n
    }
}
impl Tree for VfsTree {
    type Node = *mut Vnode;
    fn dir(&mut self, parent: *mut Vnode, name: &[u8], mdate: u16, mtime: u16) -> *mut Vnode { unsafe { Self::node(parent, name, true, mdate, mtime) } }
    fn file(&mut self, parent: *mut Vnode, name: &[u8], mdate: u16, mtime: u16, readonly: bool, data: Option<Vec<u8>>) {
        unsafe {
            let n = Self::node(parent, name, false, mdate, mtime);
            if n.is_null() { return; }
            let mut d = match data {
                Some(d) => d,
                None => { klog(c"fat32: read failed for %s".as_ptr(), cname(name).as_ptr()); Vec::new() }
            };
            if d.try_reserve_exact(1).is_err() { d = Vec::new(); }
            d.push(0);                                   // the C driver kept a byte past the data
            kfree((*n).data as *mut c_void);
            (*n).size = d.len() as u32 - 1;
            (*n).cap = d.len() as u32;
            (*n).data = into_c_buffer(d);
            (*n).readonly = readonly as CBool;
        }
    }
    fn progress(&mut self, done: u64, total: u64) {
        unsafe {
            let Some(f) = vol_load_progress else { return };
            let now = timer_ticks();
            if done >= total || now.wrapping_sub(self.last_tick) >= 50 { self.last_tick = now; f(done, total); }
        }
    }
}

/// Loads the whole volume under `root`: the number of files, or -1.
#[no_mangle]
pub unsafe extern "C" fn fat32_load_tree(v: *mut FatVol, root: *mut Vnode) -> c_int {
    let Some(v) = vol(v) else { return -1 };
    let mut t = VfsTree { last_tick: timer_ticks() };
    let r = match v.load_tree(&mut t, root) { Some(n) => n as c_int, None => return -1 };
    vfs_generation = vfs_generation.wrapping_add(1);
    r
}

#[no_mangle]
pub unsafe extern "C" fn fat32_format(dev: *mut c_void, lba: u32, sectors: u32, label: *const c_char, full: CBool, serial: u32,
                                      cb: Option<unsafe extern "C" fn(c_int, *const c_char)>, err: *mut c_char, errcap: c_int) -> CBool {
    let mut d = KDev(dev);
    let r = fat32::format(&mut d, lba, sectors, cpath(label), full != 0, serial, |pct, what| {
        if let Some(f) = cb { let w = cname(what.as_bytes()); f(pct, w.as_ptr() as *const c_char); }
    });
    match r {
        Ok((nclus, csz)) => { klog(c"fat32: formatted %u MB, %u clusters of %u bytes".as_ptr(), sectors / 2048, nclus, csz); 1 }
        Err(e) => { put_cstr(err, errcap, &e); 0 }
    }
}
