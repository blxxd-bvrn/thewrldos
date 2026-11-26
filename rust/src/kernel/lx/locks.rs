//! File locks, fsync and shared file mappings for the Linux layer: what SQLite (and anything
//! else that shares a file between processes) relies on.
//!  - POSIX record locks (fcntl F_GETLK/F_SETLK/F_SETLKW), owned by a process: they go when
//!    the process closes any descriptor of the file, or exits
//!  - open file description locks (F_OFD_*) and flock(), owned by the open file: they go with
//!    its last descriptor
//!  - fsync/fdatasync write a file on C: through to the system disk
//!  - MAP_SHARED of a regular file: every process mapping the file shares one set of pages,
//!    written back to the file on msync, munmap and exit (SQLite's WAL index, the -shm file)
use super::*;
use super::mem::*;
use alloc::vec::Vec;
use core::ptr::{self, null_mut};

const F_RDLCK: i16 = 0;
const F_WRLCK: i16 = 1;
const F_UNLCK: i16 = 2;

#[derive(Clone, Copy, PartialEq)]
enum Kind { Posix, Flock }          /* OFD locks share the POSIX namespace, as on Linux */

#[derive(Clone, Copy)]
struct Lock {
    node: *mut Vnode,
    kind: Kind,
    owner: usize,                   /* pid for classic POSIX locks, the LxFile for OFD and flock */
    pid: i32,                       /* what F_GETLK reports: the pid, or -1 for OFD locks */
    start: u64, end: u64,           /* inclusive; u64::MAX = to the end of the file and beyond */
    wr: bool,
}
static mut LOCKS: Vec<Lock> = Vec::new();
unsafe fn locks() -> &'static mut Vec<Lock> { &mut *ptr::addr_of_mut!(LOCKS) }

/// struct flock as x86-64 Linux lays it out.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Flock { l_type: i16, l_whence: i16, _pad: u32, l_start: i64, l_len: i64, l_pid: i32, _pad2: u32 }

unsafe fn conflict(node: *mut Vnode, kind: Kind, owner: usize, s: u64, e: u64, wr: bool) -> Option<Lock> {
    locks().iter().find(|l| l.node == node && l.kind == kind && l.owner != owner && l.start <= e && s <= l.end && (wr || l.wr)).copied()
}
/// The owner's locks of `kind` on [s, e] go (cut back where they overlap it).
unsafe fn clear(node: *mut Vnode, kind: Kind, owner: usize, s: u64, e: u64) {
    let ls = locks();
    let mut add: Vec<Lock> = Vec::new();
    ls.retain(|l| {
        if l.node != node || l.kind != kind || l.owner != owner || l.end < s || l.start > e { return true; }
        if l.start < s { add.push(Lock { end: s - 1, ..*l }); }
        if l.end > e { add.push(Lock { start: e + 1, ..*l }); }
        false
    });
    ls.extend(add);
}
/// Take a lock, waiting for it when `wait` is set.
unsafe fn set(node: *mut Vnode, kind: Kind, owner: usize, pid: i32, s: u64, e: u64, ty: i16, wait: bool) -> i64 {
    if ty == F_UNLCK { clear(node, kind, owner, s, e); return 0; }
    let wr = ty == F_WRLCK;
    loop {
        if conflict(node, kind, owner, s, e, wr).is_none() { break; }
        if !wait { return -EAGAIN; }
        if super::sig::interrupted() { return -EINTR; }
        task_sleep(1);
    }
    clear(node, kind, owner, s, e);
    locks().push(Lock { node, kind, owner, pid, start: s, end: e, wr });
    0
}

/// fcntl lock commands: F_GETLK 5, F_SETLK 6, F_SETLKW 7 and the OFD ones 36, 37, 38.
pub unsafe fn fcntl_lock(p: *mut LxProc, f: *mut LxFile, cmd: u64, arg: u64) -> i64 {
    let vm = (*p).vm;
    let Some(mut fl) = uget::<Flock>(vm, arg) else { return -EFAULT };
    if (*f).ty != FD_FILE || (*f).node.is_null() { return -EINVAL; }
    let node = (*f).node;
    let ofd = cmd >= 36;
    if ofd && fl.l_pid != 0 { return -EINVAL; }
    let base: i64 = match fl.l_whence { 0 => 0, 1 => (*f).pos as i64, 2 => (*node).size() as i64, _ => return -EINVAL };
    let at = base.saturating_add(fl.l_start);
    let (s, e): (i64, u64) =
        if fl.l_len > 0 { (at, (at.saturating_add(fl.l_len) - 1) as u64) }
        else if fl.l_len < 0 { (at.saturating_add(fl.l_len), (at - 1) as u64) }
        else { (at, u64::MAX) };
    if s < 0 { return -EINVAL; }
    let s = s as u64;
    let (owner, pid) = if ofd { (f as usize, -1) } else { ((*p).pid as usize, (*p).pid) };
    match cmd {
        5 | 36 => {
            if fl.l_type != F_RDLCK && fl.l_type != F_WRLCK { return -EINVAL; }
            match conflict(node, Kind::Posix, owner, s, e, fl.l_type == F_WRLCK) {
                Some(l) => {
                    fl.l_type = if l.wr { F_WRLCK } else { F_RDLCK }; fl.l_whence = 0;
                    fl.l_start = l.start as i64; fl.l_len = if l.end == u64::MAX { 0 } else { (l.end - l.start + 1) as i64 };
                    fl.l_pid = l.pid;
                }
                None => fl.l_type = F_UNLCK,
            }
            if uput(vm, arg, &fl) { 0 } else { -EFAULT }
        }
        _ => {
            if fl.l_type < 0 || fl.l_type > 2 { return -EINVAL; }
            if fl.l_type == F_RDLCK && (*f).oflags & 3 == 1 { return -EBADF; }   /* read lock on a write-only file */
            if fl.l_type == F_WRLCK && (*f).oflags & 3 == 0 { return -EBADF; }   /* write lock on a read-only file */
            set(node, Kind::Posix, owner, pid, s, e, fl.l_type, cmd == 7 || cmd == 38)
        }
    }
}

/// flock(fd, op): LOCK_SH 1, LOCK_EX 2, LOCK_UN 8, LOCK_NB 4.
pub unsafe fn flock(f: *mut LxFile, op: u64) -> i64 {
    if (*f).ty != FD_FILE || (*f).node.is_null() { return 0; }                  /* nothing to share */
    let ty = match op & !4 { 1 => F_RDLCK, 2 => F_WRLCK, 8 => F_UNLCK, _ => return -EINVAL };
    set((*f).node, Kind::Flock, f as usize, -1, 0, u64::MAX, ty, op & 4 == 0)
}

/// A process closes a descriptor of `f`: its POSIX locks on the file go.
pub unsafe fn on_close(p: *mut LxProc, f: *mut LxFile) {
    if p.is_null() || f.is_null() || (*f).ty != FD_FILE || (*f).node.is_null() { return; }
    let (node, pid) = ((*f).node, (*p).pid as usize);
    locks().retain(|l| !(l.node == node && l.kind == Kind::Posix && l.owner == pid));
}
/// The last descriptor of an open file goes: its OFD and flock locks go.
pub unsafe fn on_file_free(f: *mut LxFile) {
    if locks().is_empty() { return; }
    let o = f as usize;
    locks().retain(|l| l.owner != o || (l.kind == Kind::Posix && l.pid != -1));
}
/// A process exits.
pub unsafe fn on_exit(pid: i32) {
    let o = pid as usize;
    locks().retain(|l| !(l.kind == Kind::Posix && l.pid != -1 && l.owner == o));
}

/// fsync / fdatasync: a file on C: of an installed system goes to the disk now.
pub unsafe fn fsync(f: *mut LxFile) -> i64 {
    if (*f).ty == FD_FILE && !(*f).node.is_null() {
        let n = (*f).node;
        filemap_sync(n);
        if (*n).dirty != 0 && crate::kernel::vfsk::vfs_drive_letter(n) as u8 == b'C' { crate::kernel::persistk::persist_flush(); }
    }
    0
}

/* ---------------- shared file mappings ---------------- */
struct FileMap { node: *mut Vnode, obj: *mut ShmObj }
static mut MAPS: Vec<FileMap> = Vec::new();
unsafe fn maps() -> &'static mut Vec<FileMap> { &mut *ptr::addr_of_mut!(MAPS) }

/// The shared pages of a file, covering at least [0, need): made from the file's contents the
/// first time, grown (from the file) when a mapping reaches further. The table holds one reference.
pub unsafe fn filemap_get(node: *mut Vnode, need: u64) -> *mut ShmObj {
    let o = match maps().iter().find(|m| m.node == node) {
        Some(m) => m.obj,
        None => {
            let o = shm_new(None);
            if o.is_null() { return null_mut(); }
            maps().push(FileMap { node, obj: o });
            o
        }
    };
    let fsize = (*node).size() as u64;
    let want = palign(need.max(fsize));
    if want > (*o).size {
        let old = (*o).size;
        if shm_resize(o, want) != 0 { return null_mut(); }
        /* the new pages start as the file's bytes */
        let data = (*node).data();
        let mut idx = old / PAGE;
        while idx * PAGE < want {
            let off = idx * PAGE;
            if off < fsize && !data.is_null() {
                let pg = shm_page(o, idx, true);
                if pg.is_null() { return null_mut(); }
                ptr::copy_nonoverlapping(data.add(off as usize), pg, PAGE.min(fsize - off) as usize);
            }
            idx += 1;
        }
    }
    o
}
/// The shared pages back into the file (its size stays as it is).
pub unsafe fn filemap_sync(node: *mut Vnode) {
    let Some(m) = maps().iter().find(|m| m.node == node) else { return };
    let o = m.obj;
    let size = (*node).size() as u64;
    if size == 0 { return; }
    let buf = kmalloc(size as usize) as *mut u8;
    if buf.is_null() { return; }
    let data = (*node).data();
    if !data.is_null() { ptr::copy_nonoverlapping(data, buf, size as usize); }
    let mut off = 0u64;
    while off < size.min((*o).size) {
        let pg = shm_page(o, off / PAGE, false);
        if !pg.is_null() { ptr::copy_nonoverlapping(pg, buf.add(off as usize), PAGE.min(size - off) as usize); }
        off += PAGE;
    }
    crate::kernel::vfsk::vfs_write_node(node, buf as *const core::ffi::c_void, size as u32);
    kfree(buf as *mut core::ffi::c_void);
}
/// A mapping of a file's shared pages goes: written back, and the pages are let go once the
/// table holds the last reference.
pub unsafe fn filemap_unmapped(node: *mut Vnode, o: *mut ShmObj) {
    let Some(i) = maps().iter().position(|m| m.node == node && m.obj == o) else { return };
    filemap_sync(node);
    if (*o).refs <= 1 { maps().swap_remove(i); shm_unref(o); }
}
/// read() and write() of a file that is mapped shared see and change the mapped pages too:
/// before either, the pages go into the file ...
pub unsafe fn filemap_before_io(node: *mut Vnode) { if !maps().is_empty() { filemap_sync(node); } }
/// ... and what write() wrote goes into the pages.
pub unsafe fn filemap_wrote(node: *mut Vnode, pos: u64, src: *const u8, n: u64) {
    if maps().is_empty() { return; }
    let Some(m) = maps().iter().find(|m| m.node == node) else { return };
    let o = m.obj;
    let mut done = 0u64;
    while done < n && pos + done < (*o).size {
        let at = pos + done;
        let pg = shm_page(o, at / PAGE, true);
        if pg.is_null() { return; }
        let k = (PAGE - (at & (PAGE - 1))).min(n - done);
        ptr::copy(src.add(done as usize), pg.add((at & (PAGE - 1)) as usize), k as usize);
        done += k;
    }
}
/// msync: the shared file pages in [a, a+len) back into their files.
pub unsafe fn msync(vm: *mut Uvm, a: u64, len: u64) {
    let end = a.saturating_add(len);
    let mut v = (*vm).vmas;
    while !v.is_null() {
        if (*v).start < end && (*v).end > a && !(*v).fnode.is_null() && !(*v).shm.is_null() { filemap_sync((*v).fnode); }
        v = (*v).next;
    }
}
