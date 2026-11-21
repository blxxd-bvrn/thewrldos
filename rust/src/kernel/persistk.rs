//! Installed-system persistence (was `src/kernel/persist.c`). When THE WRLD OS boots from
//! its hard disk the system partition (FAT32 or NTFS) is loaded into the C: drive, and every
//! change to C: is written back: folders, deletes and renames at once, file contents by a
//! background write-back task (and on shutdown and restart). Also keeps the user's
//! settings in C:\WRLD\wrld.ini; copying them to and from the desktop's variables is
//! settings_apply / settings_capture in src/kernel/persist.c.
//!
//! The dirty list is shared by the VFS hook (any task) and the write-back task, so it is
//! only touched with interrupts off; a flush takes the whole list as one batch, and a delete
//! during a flush also clears the deleted files from that batch.

use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};
use core::ptr::null_mut;
use super::{Vnode, CBool, put_cbytes};
use super::vfsk::{vfs_drive_letter, vfs_path, vfs_read_file, vfs_root, vfs_write_file, vfs_hook};
use super::volk::{Vol, vol_delete, vol_exists, vol_free_bytes, vol_fs_name, vol_label, vol_load_tree, vol_mkdir, vol_mount, vol_read,
                  vol_rename, vol_serial, vol_sync, vol_total_bytes, vol_unmount, vol_write};
use crate::persist::{self, Settings};

#[repr(C)]
#[derive(Clone, Copy)]
struct Part { ty: u8, boot: u8, lba: u32, sectors: u32 }
extern "C" {
    fn blk_count() -> c_int;
    fn blk_get(i: c_int) -> *mut c_void;
    fn mbr_read(d: *mut c_void, parts: *mut Part) -> c_int;
    fn task_create(name: *const c_char, entry: extern "C" fn(*mut c_void), arg: *mut c_void, stack: u32) -> *mut c_void;
    fn task_sleep(ms: u32);
    fn kfree(p: *mut c_void);
    fn klog(fmt: *const c_char, ...);
    fn snprintf(buf: *mut c_char, n: usize, fmt: *const c_char, ...) -> c_int;
    static mut user_password: [u8; 32];
    /// src/kernel/persist.c: the settings to the desktop's variables and back
    fn settings_apply();
    fn settings_capture();
}

const VH_CREATE: c_int = 0;
const VH_WRITE: c_int = 1;
const VH_DELETE: c_int = 2;
const VH_RENAME: c_int = 3;

/// vol_t *system_volume: NULL when running live from the CD
#[no_mangle]
pub static mut system_volume: *mut Vol = null_mut();
#[no_mangle]
pub static mut system_disk_index: c_int = -1;
#[no_mangle]
pub static mut system_part_index: c_int = -1;
static mut PERSIST_ON: bool = false;
static mut FAT_CHANGED: bool = false;
static mut DIRTY: Vec<*mut Vnode> = Vec::new();
static mut INFLIGHT: Vec<*mut Vnode> = Vec::new();
static mut FLUSHING: bool = false;

/// Interrupts off for the guard's life (the scheduler preempts from the timer interrupt).
struct NoIrq(usize);
impl NoIrq {
    fn new() -> Self {
        let f: usize;
        // spelled with the operand size: a bare "pushf" assembles to the 16-bit pushfw
        #[cfg(target_arch = "x86")]
        unsafe { core::arch::asm!("pushfd", "pop {}", "cli", out(reg) f) };
        #[cfg(target_arch = "x86_64")]
        unsafe { core::arch::asm!("pushfq", "pop {}", "cli", out(reg) f) };
        NoIrq(f)
    }
}
impl Drop for NoIrq {
    fn drop(&mut self) {
        #[cfg(target_arch = "x86")]
        unsafe { core::arch::asm!("push {}", "popfd", in(reg) self.0) };
        #[cfg(target_arch = "x86_64")]
        unsafe { core::arch::asm!("push {}", "popfq", in(reg) self.0) };
    }
}

fn cz(p: &[u8]) -> Vec<u8> { let mut v = Vec::with_capacity(p.len() + 1); v.extend_from_slice(p); v.push(0); v }
unsafe fn cs<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0;
    while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
/// The node's path on the system volume, NUL-terminated.
unsafe fn rel_path(n: *mut Vnode) -> Vec<u8> {
    let mut full = [0u8; 512];
    vfs_path(n, full.as_mut_ptr() as *mut c_char, full.len() as c_int);
    cz(persist::rel_path(cs(full.as_ptr())))
}
unsafe fn on_sys(n: *mut Vnode) -> bool { PERSIST_ON && !n.is_null() && vfs_drive_letter(n) as u8 == b'C' }
unsafe fn mark_dirty(n: *mut Vnode) {
    let _g = NoIrq::new();
    let list = &mut *core::ptr::addr_of_mut!(DIRTY);
    if (*n).dirty != 0 || list.try_reserve(1).is_err() { return; }
    (*n).dirty = 1;
    list.push(n);
}
/// A folder that arrived on C: (or moved within it and could not be renamed on disk):
/// its folders now, its files at the next flush. Iterative, for deep trees.
unsafe fn mark_tree(top: *mut Vnode) {
    let mut n = top;
    while !n.is_null() {
        if (*n).dir != 0 {
            if vol_mkdir(system_volume, rel_path(n).as_ptr() as *const c_char) != 0 { FAT_CHANGED = true; }
            if !(*n).child.is_null() { n = (*n).child; continue; }
        } else { mark_dirty(n); }
        if n == top { return; }
        while (*n).next.is_null() { n = (*n).parent; if n == top || n.is_null() { return; } }
        n = (*n).next;
    }
}
unsafe fn is_under(mut n: *mut Vnode, top: *mut Vnode) -> bool {
    while !n.is_null() { if n == top { return true; } n = (*n).parent; }
    false
}
/// Forget the dirty files under top (deleted, or moved off C:).
unsafe fn purge(top: *mut Vnode) {
    let _g = NoIrq::new();
    let list = &mut *core::ptr::addr_of_mut!(DIRTY);
    list.retain(|&f| if is_under(f, top) { (*f).dirty = 0; false } else { true });
    for f in (*core::ptr::addr_of_mut!(INFLIGHT)).iter_mut() { if !f.is_null() && is_under(*f, top) { *f = null_mut(); } }
}
unsafe extern "C" fn hook(op: c_int, n: *mut Vnode, old: *const c_char) {
    if !PERSIST_ON || system_volume.is_null() { return; }
    let sv = system_volume;
    match op {
        VH_CREATE => {
            if !on_sys(n) { return; }
            if (*n).dir != 0 { vol_mkdir(sv, rel_path(n).as_ptr() as *const c_char); FAT_CHANGED = true; } else { mark_dirty(n); }
        }
        VH_WRITE => { if on_sys(n) { mark_dirty(n); } }
        VH_DELETE => {
            if !on_sys(n) { return; }
            purge(n);
            vol_delete(sv, rel_path(n).as_ptr() as *const c_char);
            FAT_CHANGED = true;
        }
        VH_RENAME => {
            let old = cs(old as *const u8);
            let was = persist::on_c(old);
            let now = on_sys(n);
            let from = if was { cz(&old[2..]) } else { Vec::new() };
            if was && now {
                if vol_rename(sv, from.as_ptr() as *const c_char, rel_path(n).as_ptr() as *const c_char) == 0 { mark_tree(n); }
            } else if was { vol_delete(sv, from.as_ptr() as *const c_char); purge(n); }
            else if now { mark_tree(n); }
            FAT_CHANGED = true;
        }
        _ => {}
    }
}

/// Write everything dirty now (the write-back task, shutdown and restart, `sync`).
#[no_mangle]
pub unsafe extern "C" fn persist_flush() {
    if system_volume.is_null() { return; }
    settings_save();
    loop {                                           // one flush at a time: wait for the other to finish
        { let _g = NoIrq::new(); if !FLUSHING { FLUSHING = true; break; } }
        task_sleep(10);
    }
    let n = {
        let _g = NoIrq::new();
        let batch = core::mem::take(&mut *core::ptr::addr_of_mut!(DIRTY));
        let n = batch.len();
        *core::ptr::addr_of_mut!(INFLIGHT) = batch;
        n
    };
    for i in 0..n {
        let f = {
            let _g = NoIrq::new();
            let fl = &*core::ptr::addr_of!(INFLIGHT);
            let f = fl[i];
            if !f.is_null() { (*f).dirty = 0; }
            f
        };
        if f.is_null() || (*f).dir != 0 || vfs_drive_letter(f) as u8 != b'C' { continue; }
        let p = rel_path(f);
        if vol_write(system_volume, p.as_ptr() as *const c_char, (*f).data as *const c_void, (*f).size, (*f).readonly) == 0 {
            klog(c"persist: could not write %s".as_ptr(), p.as_ptr());
        }
    }
    {
        let _g = NoIrq::new();
        let fl = &mut *core::ptr::addr_of_mut!(INFLIGHT);
        fl.clear();
        if n != 0 || FAT_CHANGED { FAT_CHANGED = false; }
        else { FLUSHING = false; return; }
    }
    vol_sync(system_volume);
    if n != 0 { klog(c"persist: wrote %d file(s) to the system disk".as_ptr(), n as c_int); }
    FLUSHING = false;
}
#[no_mangle]
pub unsafe extern "C" fn persist_pending() -> c_int { let _g = NoIrq::new(); (*core::ptr::addr_of!(DIRTY)).len() as c_int }
extern "C" fn flush_task(_: *mut c_void) {
    loop {
        unsafe {
            task_sleep(2000);
            if persist_pending() != 0 || FAT_CHANGED { persist_flush(); } else { settings_save(); }
        }
    }
}
/// Enable write-back and start the background flush task.
#[no_mangle]
pub unsafe extern "C" fn persist_start() {
    vfs_hook = Some(hook);
    PERSIST_ON = true;
    if !system_volume.is_null() { task_create(c"persistd".as_ptr(), flush_task, null_mut(), 16384); }
}

/// Systems installed by older versions: C:\Windows became C:\WRLD (with its classic style),
/// and the browser's desktop shortcut was renamed twice.
unsafe fn upgrade(v: *mut Vol) {
    let p = |s: &[u8]| cz(s);
    if vol_exists(v, p(b"Windows").as_ptr() as _) != 0 && vol_exists(v, p(b"WRLD").as_ptr() as _) == 0 {
        let ok = vol_rename(v, p(b"Windows").as_ptr() as _, p(b"WRLD").as_ptr() as _) != 0;
        let oldst = p(b"WRLD\\Resources\\Themes\\Windows Classic.wrldstyle");
        let mut sl = 0u32;
        let st = if ok { vol_read(v, oldst.as_ptr() as _, &mut sl) } else { null_mut() };
        if !st.is_null() {                           // rename the style and its Name= line
            let nb = persist::renamed_style(core::slice::from_raw_parts(st, sl as usize));
            if vol_write(v, p(b"WRLD\\Resources\\Themes\\WRLD Classic.wrldstyle").as_ptr() as _, nb.as_ptr() as _, nb.len() as u32, 0) != 0 {
                vol_delete(v, oldst.as_ptr() as _);
            }
            kfree(st as *mut c_void);
        }
        vol_sync(v);
        klog(c"persist: renamed C:\\Windows to C:\\WRLD (%s)".as_ptr(), if ok { c"ok".as_ptr() } else { c"failed".as_ptr() });
    }
    let nl = p(b"Documents and Settings\\User\\Desktop\\The WRLD Browser.lnk");
    for old in [&b"Documents and Settings\\User\\Desktop\\The WRLD Xplorer.lnk"[..], b"Documents and Settings\\User\\Desktop\\XplorerLite.lnk"] {
        let o = p(old);
        let mut ln = 0u32;
        let lk = if vol_exists(v, o.as_ptr() as _) != 0 { vol_read(v, o.as_ptr() as _, &mut ln) } else { null_mut() };
        if lk.is_null() { continue; }
        if vol_exists(v, nl.as_ptr() as _) != 0 || vol_write(v, nl.as_ptr() as _, lk as _, ln, 0) != 0 { vol_delete(v, o.as_ptr() as _); }
        kfree(lk as *mut c_void);
        vol_sync(v);
        klog(c"persist: desktop shortcut renamed to The WRLD Browser".as_ptr());
    }
}
/// Find the system partition (by volume serial, or the one with boot\wrld.elf) and load it into C:.
#[no_mangle]
pub unsafe extern "C" fn persist_boot_from_disk(serial: u32, err: *mut c_char, errcap: c_int) -> CBool {
    for d in 0..blk_count() {
        let dev = blk_get(d);
        let mut parts = [Part { ty: 0, boot: 0, lba: 0, sectors: 0 }; 4];
        if mbr_read(dev, parts.as_mut_ptr()) == 0 { continue; }
        for (i, pt) in parts.iter().enumerate() {
            if !matches!(pt.ty, 0x0B | 0x0C | 0xEF | 0x07) { continue; }
            let v = vol_mount(dev, pt.lba, pt.sectors);
            if v.is_null() { continue; }
            if (serial != 0 && vol_serial(v) == serial) || (serial == 0 && vol_exists(v, c"boot\\wrld.elf".as_ptr()) != 0) {
                system_volume = v; system_disk_index = d; system_part_index = i as c_int;
                upgrade(v);
                let files = vol_load_tree(v, vfs_root(b'C' as c_int));
                klog(c"persist: system disk %d partition %d, %d files loaded".as_ptr(), d, i as c_int + 1, files);
                return (files >= 0) as CBool;
            }
            vol_unmount(v);
        }
    }
    snprintf(err, errcap.max(0) as usize, c"THE WRLD OS could not find its system partition (volume serial %08X).\nCheck that the hard disk is connected, or boot from the CD and run Setup again.".as_ptr(), serial);
    0
}
/// "Disk 0, Partition 1: FAT32 "WRLD", 900 MB free of 1000 MB", or the live session.
#[no_mangle]
pub unsafe extern "C" fn persist_status(b: *mut c_char, cap: c_int) {
    let sv = system_volume;
    if sv.is_null() { put_cbytes(b, cap, b"Live session from the CD (in memory; changes are not saved)"); return; }
    snprintf(b, cap.max(0) as usize, c"Disk %d, Partition %d: %s \"%s\", %u MB free of %u MB".as_ptr(), system_disk_index, system_part_index + 1,
             vol_fs_name(sv), vol_label(sv), (vol_free_bytes(sv) >> 20) as u32, (vol_total_bytes(sv) >> 20) as u32);
}

// ---------------------------------------------------------------- settings (C:\WRLD\wrld.ini)

static mut SETTINGS: Settings = Settings::new();
static mut LAST_SAVED: Vec<u8> = Vec::new();
const SETTINGS_FILE: &core::ffi::CStr = c"C:\\WRLD\\wrld.ini";
unsafe fn store() -> &'static mut Settings { &mut *core::ptr::addr_of_mut!(SETTINGS) }

#[no_mangle]
pub unsafe extern "C" fn settings_get(key: *const c_char) -> *const c_char {
    let s = store();
    match s.find(cs(key as *const u8)) { Some(i) => s.kv[i].1.as_ptr() as *const c_char, None => core::ptr::null() }
}
#[no_mangle]
pub unsafe extern "C" fn settings_set(key: *const c_char, val: *const c_char) { store().set(cs(key as *const u8), cs(val as *const u8)); }
/// Read wrld.ini and apply it.
#[no_mangle]
pub unsafe extern "C" fn settings_load() {
    let mut len = 0u32;
    let t = vfs_read_file(SETTINGS_FILE.as_ptr(), &mut len);
    if t.is_null() { return; }
    store().load_ini(core::slice::from_raw_parts(t, len as usize));
    kfree(t as *mut c_void);
    settings_apply();
    klog(c"settings: loaded %d values".as_ptr(), store().n as c_int);
}
/// Collect the settings and write wrld.ini if anything changed.
#[no_mangle]
pub unsafe extern "C" fn settings_save() {
    settings_capture();
    let text = store().to_ini();
    let last = &mut *core::ptr::addr_of_mut!(LAST_SAVED);
    if text == *last { return; }
    if !vfs_write_file(SETTINGS_FILE.as_ptr(), text.as_ptr() as *const c_void, text.len() as u32).is_null() { *last = text; }
}
#[no_mangle]
pub unsafe extern "C" fn password_set(plain: *const c_char) {
    let pw = &mut *core::ptr::addr_of_mut!(user_password);
    let p = cs(plain as *const u8);
    if p.is_empty() { pw[0] = 0; return; }
    pw[..16].copy_from_slice(&persist::password_digest(p));
    pw[16] = 0;
}
#[no_mangle]
pub unsafe extern "C" fn password_matches(plain: *const c_char) -> CBool {
    let pw = &*core::ptr::addr_of!(user_password);
    if pw[0] == 0 { return 1; }
    (cs(pw.as_ptr()) == &persist::password_digest(cs(plain as *const u8))[..]) as CBool
}
