//! The NTFS driver's C functions (called through `volk.rs`, was `src/kernel/ntfs.c`),
//! over `crate::ntfs`: the disk is a `blkdev_t` read and written with blk_read/blk_write,
//! and a loaded tree goes into the VFS.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};

use super::{into_c_buffer, put_cstr, CBool, RtcTime, Vnode};
use crate::ntfs::{self, Dev, Tree, Vol};

extern "C" {
    fn blk_read(d: *mut c_void, lba: u32, count: u32, buf: *mut c_void) -> CBool;
    fn blk_write(d: *mut c_void, lba: u32, count: u32, buf: *const c_void) -> CBool;
    fn rtc_read(t: *mut RtcTime);
    fn rand_next() -> u32;
    fn timer_ticks() -> u32;
    fn vfs_lookup_child(dir: *mut Vnode, name: *const c_char) -> *mut Vnode;
    fn vfs_new_node(parent: *mut Vnode, name: *const c_char, dir: CBool) -> *mut Vnode;
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
    fn now(&self) -> u64 {
        let mut t = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0, weekday: 0 };
        unsafe { rtc_read(&mut t) };
        ntfs::ntfs_time(t.year, t.month, t.day, t.hour, t.minute, t.second)
    }
    fn rand(&mut self) -> u32 { unsafe { rand_next() } }
    fn log(&mut self, msg: &str) { unsafe { klog(c"%s".as_ptr(), cname(msg.as_bytes()).as_ptr()) } }
}

pub(super) type NtfsVol = Vol<KDev>;

unsafe fn vol<'a>(v: *mut NtfsVol) -> Option<&'a mut NtfsVol> { v.as_mut() }
unsafe fn cpath<'a>(p: *const c_char) -> &'a [u8] { if p.is_null() { &[] } else { core::ffi::CStr::from_ptr(p).to_bytes() } }
fn cname(n: &[u8]) -> Vec<u8> { let mut v = n.to_vec(); v.push(0); v }

#[no_mangle]
pub unsafe extern "C" fn ntfs_mount(dev: *mut c_void, lba: u32, sectors: u32) -> *mut NtfsVol {
    match Vol::mount(KDev(dev), lba, sectors) { Ok(v) => Box::into_raw(Box::new(v)), Err(_) => core::ptr::null_mut() }
}
#[no_mangle]
pub unsafe extern "C" fn ntfs_unmount(v: *mut NtfsVol) { if !v.is_null() { drop(Box::from_raw(v)); } }
#[no_mangle]
pub unsafe extern "C" fn ntfs_serial(v: *mut NtfsVol) -> u32 { vol(v).map(|v| v.serial()).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn ntfs_label(v: *mut NtfsVol) -> *const c_char { vol(v).map(|v| v.label().as_ptr() as *const c_char).unwrap_or(c"".as_ptr()) }
#[no_mangle]
pub unsafe extern "C" fn ntfs_free_bytes(v: *mut NtfsVol) -> u64 { vol(v).map(|v| v.free_bytes()).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn ntfs_total_bytes(v: *mut NtfsVol) -> u64 { vol(v).map(|v| v.total_bytes()).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn ntfs_exists(v: *mut NtfsVol, path: *const c_char) -> CBool { vol(v).map(|v| v.exists(cpath(path)) as CBool).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn ntfs_mkdir(v: *mut NtfsVol, path: *const c_char) -> CBool { vol(v).map(|v| v.mkdir(cpath(path)) as CBool).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn ntfs_write(v: *mut NtfsVol, path: *const c_char, data: *const c_void, len: u32, readonly: CBool) -> CBool {
    let Some(v) = vol(v) else { return 0 };
    let d = if data.is_null() || len == 0 { &[][..] } else { core::slice::from_raw_parts(data as *const u8, len as usize) };
    v.write(cpath(path), d, readonly != 0) as CBool
}
#[no_mangle]
pub unsafe extern "C" fn ntfs_delete(v: *mut NtfsVol, path: *const c_char) -> CBool { vol(v).map(|v| v.delete(cpath(path)) as CBool).unwrap_or(0) }
#[no_mangle]
pub unsafe extern "C" fn ntfs_rename(v: *mut NtfsVol, from: *const c_char, to: *const c_char) -> CBool {
    vol(v).map(|v| v.rename(cpath(from), cpath(to)) as CBool).unwrap_or(0)
}
/// A file's contents in a kmalloc'd buffer with a byte after them (the caller kfrees it).
#[no_mangle]
pub unsafe extern "C" fn ntfs_read(v: *mut NtfsVol, path: *const c_char, len: *mut u32) -> *mut u8 {
    let Some(v) = vol(v) else { return core::ptr::null_mut() };
    let Some(mut d) = v.read(cpath(path)) else { return core::ptr::null_mut() };
    let n = d.len() as u32;
    if d.try_reserve_exact(1).is_err() { return core::ptr::null_mut(); }
    d.push(0);
    if !len.is_null() { *len = n; }
    into_c_buffer(d)
}
#[no_mangle]
pub unsafe extern "C" fn ntfs_sync(v: *mut NtfsVol) { if let Some(v) = vol(v) { v.sync(); } }

/// Puts a volume's folders and files into the VFS (reusing nodes that are already there).
struct VfsTree { last_tick: u32 }
unsafe fn vnode(parent: *mut Vnode, name: &[u8], dir: bool) -> *mut Vnode {
    if parent.is_null() { return parent; }
    let c = cname(name);
    let mut n = vfs_lookup_child(parent, c.as_ptr() as *const c_char);
    if !n.is_null() && ((*n).dir != 0) != dir { n = core::ptr::null_mut(); }
    if n.is_null() { n = vfs_new_node(parent, c.as_ptr() as *const c_char, dir as CBool); }
    n
}
impl Tree for VfsTree {
    type Node = *mut Vnode;
    fn dir(&mut self, parent: *mut Vnode, name: &[u8]) -> *mut Vnode { unsafe { vnode(parent, name, true) } }
    fn file(&mut self, parent: *mut Vnode, name: &[u8], readonly: bool, data: Option<Vec<u8>>) {
        unsafe {
            let n = vnode(parent, name, false);
            if n.is_null() { return; }
            let mut d = data.unwrap_or_default();
            if d.try_reserve_exact(1).is_err() { d = Vec::new(); }
            d.push(0);
            super::vfsk::release_data(n);
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

#[no_mangle]
pub unsafe extern "C" fn ntfs_load_tree(v: *mut NtfsVol, root: *mut Vnode) -> c_int {
    let Some(v) = vol(v) else { return -1 };
    let mut t = VfsTree { last_tick: timer_ticks() };
    let n = v.load_tree(&mut t, root) as c_int;
    vfs_generation = vfs_generation.wrapping_add(1);
    n
}

#[no_mangle]
pub unsafe extern "C" fn ntfs_format(dev: *mut c_void, lba: u32, sectors: u32, label: *const c_char, full: CBool, serial: u32,
                                     cb: Option<unsafe extern "C" fn(c_int, *const c_char)>, err: *mut c_char, errcap: c_int) -> CBool {
    let (_, r) = ntfs::format(KDev(dev), lba, sectors, cpath(label), full != 0, serial, |pct, what| {
        if let Some(f) = cb { let w = cname(what.as_bytes()); f(pct, w.as_ptr() as *const c_char); }
    });
    match r {
        Ok((nclus, nrec)) => { klog(c"ntfs: formatted %u MB, %u clusters of 4 KB, %u MFT records".as_ptr(), (nclus * 4096 >> 20) as u32, nclus as u32, nrec); 1 }
        Err(e) => { put_cstr(err, errcap, &e); 0 }
    }
}
