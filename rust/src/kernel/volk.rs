//! Volumes (declared in src/include/disk.h, was `src/kernel/vol.c`): one interface over the
//! FAT32 and NTFS drivers, used by Setup and by persistence (the installed system's C:).

use alloc::boxed::Box;
use core::ffi::{c_char, c_int, c_void};

use super::fat32k::{self as fat, FatVol};
use super::ntfsk::{self as ntfs, NtfsVol};
use super::{CBool, Vnode};

const FS_FAT32: c_int = 1;
const FS_NTFS: c_int = 2;

/// vol_t: a mounted volume of either kind.
pub enum Vol { Fat(*mut FatVol), Ntfs(*mut NtfsVol) }
type Progress = Option<unsafe extern "C" fn(c_int, *const c_char)>;

/// Called while a volume's files are loaded into the VFS (Setup's and the boot screen's bar).
#[no_mangle]
pub static mut vol_load_progress: Option<unsafe extern "C" fn(u64, u64)> = None;

#[no_mangle]
pub unsafe extern "C" fn vol_mount(d: *mut c_void, lba: u32, sectors: u32) -> *mut Vol {
    let f = fat::fat32_mount(d, lba, sectors);
    let v = if !f.is_null() { Vol::Fat(f) } else {
        let n = ntfs::ntfs_mount(d, lba, sectors);
        if n.is_null() { return core::ptr::null_mut(); }
        Vol::Ntfs(n)
    };
    Box::into_raw(Box::new(v))
}
#[no_mangle]
pub unsafe extern "C" fn vol_unmount(v: *mut Vol) {
    if v.is_null() { return; }
    match *Box::from_raw(v) { Vol::Fat(f) => fat::fat32_unmount(f), Vol::Ntfs(n) => ntfs::ntfs_unmount(n) }
}
#[no_mangle]
pub unsafe extern "C" fn vol_format(kind: c_int, d: *mut c_void, lba: u32, sectors: u32, label: *const c_char, full: CBool, serial: u32,
                                    cb: Progress, err: *mut c_char, errcap: c_int) -> CBool {
    if kind == FS_NTFS { ntfs::ntfs_format(d, lba, sectors, label, full, serial, cb, err, errcap) }
    else { fat::fat32_format(d, lba, sectors, label, full, serial, cb, err, errcap) }
}
#[no_mangle]
pub unsafe extern "C" fn vol_kind(v: *mut Vol) -> c_int { match &*v { Vol::Fat(_) => FS_FAT32, Vol::Ntfs(_) => FS_NTFS } }
#[no_mangle]
pub unsafe extern "C" fn vol_fs_name(v: *mut Vol) -> *const c_char { match &*v { Vol::Fat(_) => c"FAT32".as_ptr(), Vol::Ntfs(_) => c"NTFS".as_ptr() } }
#[no_mangle]
pub extern "C" fn vol_part_type(kind: c_int) -> u8 { if kind == FS_NTFS { 0x07 } else { 0x0C } }

macro_rules! dispatch {
    ($(fn $name:ident($($a:ident: $t:ty),*) -> $r:ty => $f:ident, $n:ident;)*) => {$(
        #[no_mangle]
        pub unsafe extern "C" fn $name(v: *mut Vol $(, $a: $t)*) -> $r {
            match &*v { Vol::Fat(x) => fat::$f(*x $(, $a)*), Vol::Ntfs(x) => ntfs::$n(*x $(, $a)*) }
        }
    )*};
}
dispatch! {
    fn vol_serial() -> u32 => fat32_serial, ntfs_serial;
    fn vol_label() -> *const c_char => fat32_label, ntfs_label;
    fn vol_free_bytes() -> u64 => fat32_free_bytes, ntfs_free_bytes;
    fn vol_total_bytes() -> u64 => fat32_total_bytes, ntfs_total_bytes;
    fn vol_exists(p: *const c_char) -> CBool => fat32_exists, ntfs_exists;
    fn vol_mkdir(p: *const c_char) -> CBool => fat32_mkdir, ntfs_mkdir;
    fn vol_write(p: *const c_char, d: *const c_void, n: u32, ro: CBool) -> CBool => fat32_write, ntfs_write;
    fn vol_delete(p: *const c_char) -> CBool => fat32_delete, ntfs_delete;
    fn vol_rename(a: *const c_char, b: *const c_char) -> CBool => fat32_rename, ntfs_rename;
    fn vol_read(p: *const c_char, n: *mut u32) -> *mut u8 => fat32_read, ntfs_read;
    fn vol_load_tree(root: *mut Vnode) -> c_int => fat32_load_tree, ntfs_load_tree;
    fn vol_sync() -> () => fat32_sync, ntfs_sync;
}
