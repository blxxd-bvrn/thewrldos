//! Removable media in the VFS (the C side, the polling task and AutoPlay, is src/kernel/media.c):
//! a disc or a stick becomes a drive letter whose tree of names and sizes is read when it is
//! mounted, and whose files are read the first time they are opened (vfs_lazy_load), through
//! the file systems in `crate::media`.

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicBool, Ordering};

use crate::media::{self, Dev, Entry, Part, Volume};
use super::{CBool, Vnode, put_cstr, kmalloc, vfs_new_node, vfs_mount};

extern "C" {
    fn blk_read(d: *mut c_void, lba: u32, count: u32, buf: *mut c_void) -> CBool;
    fn blk_bytes(d: *mut c_void) -> u64;
    fn blk_dead(d: *mut c_void) -> CBool;
    fn vfs_unmount(l: c_char) -> CBool;
    fn task_yield();
    fn mm_total() -> u32;
    fn mm_used() -> u32;
    fn klog(fmt: *const c_char, ...);
    static mut vfs_lazy_load: Option<unsafe extern "C" fn(*mut Vnode) -> CBool>;
}

/// The disk, read in 64 KB pieces with the last one kept.
struct KDev { d: *mut c_void, len: u64, chunk: u64, buf: Vec<u8> }
const CHUNK: u64 = 64 * 1024;
impl Dev for KDev {
    fn read_at(&mut self, off: u64, out: &mut [u8]) -> bool {
        let mut done = 0usize;
        while done < out.len() {
            let pos = off + done as u64;
            let c = pos / CHUNK;
            if self.buf.is_empty() || self.chunk != c {
                let start = c * CHUNK;
                if start >= self.len { return false; }
                let n = (self.len - start).min(CHUNK);
                self.buf.resize(n as usize, 0);
                if unsafe { blk_read(self.d, (start / 512) as u32, (n / 512) as u32, self.buf.as_mut_ptr() as *mut c_void) } == 0 { self.buf.clear(); return false; }
                self.chunk = c;
            }
            let at = (pos - c * CHUNK) as usize;
            if at >= self.buf.len() { return false; }
            let k = (self.buf.len() - at).min(out.len() - done);
            out[done..done + k].copy_from_slice(&self.buf[at..at + k]);
            done += k;
        }
        true
    }
    fn len(&self) -> u64 { self.len }
}

struct Mount { id: u32, dev: *mut c_void, off: u64, len: u64, vol: Volume, letter: u8, kdev: KDev }
static mut MOUNTS: Vec<Mount> = Vec::new();
static mut NEXT_ID: u32 = 1;
static LOCK: AtomicBool = AtomicBool::new(false);
struct Guard;
fn lock() -> Guard { while LOCK.swap(true, Ordering::Acquire) { unsafe { task_yield() } } Guard }
impl Drop for Guard { fn drop(&mut self) { LOCK.store(false, Ordering::Release); } }
#[allow(static_mut_refs)]
fn mounts() -> &'static mut Vec<Mount> { unsafe { &mut *core::ptr::addr_of_mut!(MOUNTS) } }

const MAX_NODES: usize = 200_000;
/// What a disc holds, for AutoPlay: music, video and picture files, and the first of each.
#[derive(Default)]
struct Scan { nodes: usize, music: u32, video: u32, pics: u32, first_media: String, first_pic: String }
fn ext_of(name: &str) -> String { name.rsplit_once('.').map(|(_, e)| e.to_ascii_lowercase()).unwrap_or_default() }

unsafe fn build(vol: &mut Volume, d: &mut dyn Dev, parent: *mut Vnode, dir: &Entry, path: &mut String, depth: u32, scan: &mut Scan) {
    if depth > 32 || scan.nodes >= MAX_NODES { return; }
    let Some(list) = vol.list(d, dir) else { return };
    for e in list {
        if scan.nodes >= MAX_NODES { break; }
        let mut name = e.name.clone().into_bytes(); name.push(0);
        let n = vfs_new_node(parent, name.as_ptr() as *const c_char, e.dir as CBool);
        if n.is_null() { break; }
        scan.nodes += 1;
        (*n).readonly = 1;
        (*n).mtime.year = e.time.year as _; (*n).mtime.month = e.time.month as _; (*n).mtime.day = e.time.day as _;
        (*n).mtime.hour = e.time.hour as _; (*n).mtime.minute = e.time.minute as _; (*n).mtime.second = e.time.second as _;
        let plen = path.len();
        path.push('\\'); path.push_str(&e.name);
        if e.dir { build(vol, d, n, &e, path, depth + 1, scan); }
        else {
            (*n).size = e.size.min(u32::MAX as u64) as u32;
            (*n).lazy_loc = e.loc;
            match ext_of(&e.name).as_str() {
                "mp3" | "ogg" | "flac" | "wav" | "wma" | "aiff" | "aif" | "au" | "mp2" | "m4a" => { scan.music += 1; if scan.first_media.is_empty() { scan.first_media = path.clone(); } }
                "mpg" | "mpeg" | "vob" | "m2ts" | "mts" | "avi" | "mp4" | "mkv" | "webm" | "wmv" => { scan.video += 1; if scan.first_media.is_empty() { scan.first_media = path.clone(); } }
                "jpg" | "jpeg" | "png" | "gif" | "bmp" => { scan.pics += 1; if scan.first_pic.is_empty() { scan.first_pic = path.clone(); } }
                _ => {}
            }
        }
        path.truncate(plen);
    }
}
/// Tags every file node under `n` with the mount it belongs to (the id is known once mounted).
unsafe fn tag(n: *mut Vnode, id: u32) {
    let mut c = (*n).child;
    while !c.is_null() { if (*c).dir != 0 { tag(c, id); } else { (*c).lazy = id; } c = (*c).next; }
}

/// int media_mount(void *disk, int kind, char *out, int cap): mounts every volume on the disk
/// (kind 3 CD/DVD, 4 removable disk); returns how many. out gets one line per volume:
/// "D|label|fs|music|video|pictures|first media file|first picture".
#[no_mangle]
pub unsafe extern "C" fn media_mount(disk: *mut c_void, kind: c_int, out: *mut c_char, cap: c_int) -> c_int {
    let _g = lock();
    let total = blk_bytes(disk);
    if total == 0 { return 0; }
    let mut kd = KDev { d: disk, len: total, chunk: 0, buf: Vec::new() };
    /* a disc's own file system first: an ISO written to a stick ("isohybrid") also carries a
     * partition table, with the EFI boot image as a small FAT partition */
    let whole_disc = {
        let mut p = Part { dev: &mut kd, off: 0, len: total };
        matches!(Volume::probe(&mut p).map(|v| v.kind), Some(media::Kind::Iso9660) | Some(media::Kind::Udf))
    };
    let parts = if whole_disc { alloc::vec![(0u64, total)] } else { media::partitions(&mut kd) };
    let mut report = String::new(); let mut count = 0;
    for (off, len) in parts {
        let mut p = Part { dev: &mut kd, off, len };
        let Some(mut vol) = Volume::probe(&mut p) else { continue };
        let root = vfs_new_node(core::ptr::null_mut(), c"X:".as_ptr(), 1);
        if root.is_null() { break; }
        (*root).readonly = 1;
        let mut scan = Scan::default();
        let mut path = String::new();
        let re = vol.root();
        build(&mut vol, &mut p, root, &re, &mut path, 0, &mut scan);
        let label = if vol.label.is_empty() { String::from(if kind == 3 { "CD Drive" } else { "Removable Disk" }) } else { vol.label.clone() };
        let mut lb = label.clone().into_bytes(); lb.truncate(31); lb.push(0);
        let letter = vfs_mount(root, lb.as_ptr() as *const c_char, kind) as u8;
        if letter == 0 { break; }
        let id = NEXT_ID; NEXT_ID += 1;
        tag(root, id);
        let fs = vol.fs_name();
        let full = |p: &str| if p.is_empty() { String::new() } else { alloc::format!("{}:{}", letter as char, p) };
        let _ = core::fmt::write(&mut report, format_args!("{}|{}|{}|{}|{}|{}|{}|{}\n", letter as char, label.replace('|', "_"), fs,
            scan.music, scan.video, scan.pics, full(&scan.first_media), full(&scan.first_pic)));
        klog(c"media: %s volume \"%s\" mounted as %c: (%d entries)".as_ptr(), cz(fs).as_ptr(), lb.as_ptr(), letter as c_int, scan.nodes as c_int);
        mounts().push(Mount { id, dev: disk, off, len, vol, letter, kdev: KDev { d: disk, len: total, chunk: 0, buf: Vec::new() } });
        count += 1;
    }
    if count > 0 && (*core::ptr::addr_of!(vfs_lazy_load)).is_none() { *core::ptr::addr_of_mut!(vfs_lazy_load) = Some(lazy_load); }
    put_cstr(out, cap, &report);
    count
}
fn cz(s: &str) -> Vec<u8> { let mut v = Vec::from(s.as_bytes()); v.push(0); v }

/// int media_unmount(void *disk, char *letters, int cap): takes the disk's drive letters away
/// (it was ejected or unplugged); returns how many, letters gets them.
#[no_mangle]
pub unsafe extern "C" fn media_unmount(disk: *mut c_void, letters: *mut c_char, cap: c_int) -> c_int {
    let _g = lock();
    let mut gone = String::new();
    mounts().retain(|m| {
        if m.dev != disk { return true; }
        vfs_unmount(m.letter as c_char);
        gone.push(m.letter as char);
        false
    });
    put_cstr(letters, cap, &gone);
    gone.len() as c_int
}
#[no_mangle]
pub unsafe extern "C" fn media_mounted(disk: *mut c_void) -> CBool { let _g = lock(); mounts().iter().any(|m| m.dev == disk) as CBool }
/// The device behind a drive letter (for Eject), or NULL.
#[no_mangle]
pub unsafe extern "C" fn media_disk_of(letter: c_char) -> *mut c_void {
    let _g = lock();
    let l = (letter as u8).to_ascii_uppercase();
    mounts().iter().find(|m| m.letter == l).map_or(core::ptr::null_mut(), |m| m.dev)
}
/// const char *media_fs(char letter): "CDFS", "UDF", "FAT32"... for a removable drive, or "".
#[no_mangle]
pub unsafe extern "C" fn media_fs(letter: c_char) -> *const c_char {
    let _g = lock();
    let l = (letter as u8).to_ascii_uppercase();
    match mounts().iter().find(|m| m.letter == l).map(|m| m.vol.fs_name()) {
        Some("CDFS") => c"CDFS".as_ptr(), Some("UDF") => c"UDF".as_ptr(), Some("FAT32") => c"FAT32".as_ptr(),
        Some("FAT16") => c"FAT16".as_ptr(), Some("FAT12") => c"FAT12".as_ptr(), Some("exFAT") => c"exFAT".as_ptr(), _ => c"".as_ptr(),
    }
}
/// Devices that went away (unplugged): their letters go too. Returns how many letters went.
#[no_mangle]
pub unsafe extern "C" fn media_drop_dead(letters: *mut c_char, cap: c_int) -> c_int {
    let _g = lock();
    let mut gone = String::new();
    mounts().retain(|m| { if blk_dead(m.dev) == 0 { return true; } vfs_unmount(m.letter as c_char); gone.push(m.letter as char); false });
    put_cstr(letters, cap, &gone);
    gone.len() as c_int
}

/// The VFS asks for a file's contents the first time it is opened.
unsafe extern "C" fn lazy_load(n: *mut Vnode) -> CBool {
    if n.is_null() || (*n).lazy == 0 || !(*n).data.is_null() { return 0; }
    let _g = lock();
    let Some(m) = mounts().iter_mut().find(|m| m.id == (*n).lazy) else { return 0 };
    if blk_dead(m.dev) != 0 { return 0; }
    /* no more than a quarter of the free memory for one file */
    let free = (mm_total() as u64).saturating_sub(mm_used() as u64);
    let max = (free / 4).min(1 << 30);
    let e = Entry { name: String::new(), dir: false, size: (*n).size as u64, loc: (*n).lazy_loc, time: Default::default() };
    if e.size > max {
        klog(c"media: a %u MB file is too big to open with the memory free".as_ptr(), ((*n).size >> 20) as u32);
        return 0;
    }
    let mut p = Part { dev: &mut m.kdev, off: m.off, len: m.len };
    let Some(data) = m.vol.read(&mut p, &e, max) else { klog(c"media: reading a file failed".as_ptr()); return 0 };
    let buf = kmalloc(data.len().max(1)) as *mut u8;
    if buf.is_null() { return 0; }
    core::ptr::copy_nonoverlapping(data.as_ptr(), buf, data.len());
    (*n).data = buf; (*n).cap = data.len().max(1) as u32; (*n).size = data.len() as u32;
    1
}
