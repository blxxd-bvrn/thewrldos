//! The in-memory file system with drive letters (declared in src/include/fs.h, was
//! `src/kernel/vfs.c`). Nodes are C `vnode_t`s from kzalloc, with kmalloc'd data, because
//! C code reads and writes them directly. The path and archive logic is in
//! `crate::vfspath`.
//!
//! Changes from the C version: walking a tree does not recurse; copying a folder into
//! itself, moving a drive's root, and making folders under a file or a missing drive are
//! refused; and a damaged initrd member cannot read past the archive.

use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr::null_mut;

use super::{put_cbytes, CBool, RtcTime, Vnode};
use crate::vfspath::{self as vp, Part};

extern "C" {
    fn kzalloc(n: usize) -> *mut c_void;
    fn kmalloc(n: usize) -> *mut c_void;
    fn krealloc(p: *mut c_void, n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
    fn rtc_now(t: *mut RtcTime);
    fn klog(fmt: *const c_char, ...);
    fn mm_initrd_consumed(upto: *const u8);
}

const VH_CREATE: c_int = 0;
const VH_WRITE: c_int = 1;
const VH_DELETE: c_int = 2;
const VH_RENAME: c_int = 3;
const USER_HOME: &[u8] = b"C:\\Documents and Settings\\User";

static mut DRIVES: [*mut Vnode; 26] = [null_mut(); 26];
static mut LABELS: [[u8; 32]; 26] = [[0; 32]; 26];
static mut KINDS: [c_int; 26] = [0; 26];

/// Bumps on every change (Explorer and the desktop redraw when it moves).
#[no_mangle]
pub static mut vfs_generation: u32 = 0;
/// Called on every change (persist.c saves it to the installed disk).
#[no_mangle]
pub static mut vfs_hook: Option<unsafe extern "C" fn(c_int, *mut Vnode, *const c_char)> = None;

unsafe fn bump() { vfs_generation = vfs_generation.wrapping_add(1); }
unsafe fn hook(op: c_int, n: *mut Vnode, old: *const c_char) { if let Some(f) = vfs_hook { f(op, n, old); } }
/// Removable media (media.c): fills a file's contents the first time it is opened.
#[no_mangle]
pub static mut vfs_lazy_load: Option<unsafe extern "C" fn(*mut Vnode) -> CBool> = None;
/// A file on a removable medium whose contents have not been read yet: read them now.
pub(crate) unsafe fn ensure_loaded(n: *mut Vnode) {
    if !n.is_null() && (*n).dir == 0 && (*n).data.is_null() && (*n).lazy != 0 && (*n).size != 0 {
        if let Some(f) = vfs_lazy_load { f(n); }
    }
}
unsafe fn cstr<'a>(p: *const c_char) -> &'a [u8] { if p.is_null() { &[] } else { CStr::from_ptr(p).to_bytes() } }
fn cvec(s: &[u8]) -> Vec<u8> { let mut v = s.to_vec(); v.push(0); v }
unsafe fn name<'a>(n: *mut Vnode) -> &'a [u8] { cstr((*n).name.as_ptr()) }
unsafe fn set_name(n: *mut Vnode, s: &[u8]) { put_cbytes((*n).name.as_mut_ptr(), 96, s) }
fn letter_index(l: c_int) -> Option<usize> { let u = (l as u8).to_ascii_uppercase(); (l >= 0 && l < 256 && u.is_ascii_uppercase()).then(|| (u - b'A') as usize) }

/// The children of a folder, in order.
unsafe fn kids(d: *mut Vnode) -> Vec<*mut Vnode> {
    let mut v = Vec::new();
    let mut c = if d.is_null() { null_mut() } else { (*d).child };
    while !c.is_null() { v.push(c); c = (*c).next; }
    v
}
/// Every node under `root` (with it), parents before children.
unsafe fn subtree(root: *mut Vnode) -> Vec<*mut Vnode> {
    let mut out = Vec::new();
    let mut stack = Vec::new();
    if !root.is_null() { stack.push(root); }
    while let Some(n) = stack.pop() {
        out.push(n);
        let mut c = (*n).child;
        while !c.is_null() { stack.push(c); c = (*c).next; }
    }
    out
}
unsafe fn append_child(parent: *mut Vnode, n: *mut Vnode) {
    let mut pp = &mut (*parent).child as *mut *mut Vnode;
    while !(*pp).is_null() { pp = &mut (**pp).next; }
    *pp = n;
}
unsafe fn set_drive(n: *mut Vnode, d: c_int) { for c in subtree(n) { (*c).drive = d; } }
unsafe fn free_tree(n: *mut Vnode) { for c in subtree(n) { release_data(c); kfree(c as *mut c_void); } }

// ---------------------------------------------------------------- pinned file data
// Code that keeps pointers into a file's contents (the TrueType faces) pins the buffer instead of
// copying it. The file stays writable: whatever would change, move or free a pinned buffer gives
// the file a new buffer first (vfs_unshare), and the old one stays with whoever pinned it.
const NPIN: usize = 32;
static mut PINS: [(*mut Vnode, *mut u8); NPIN] = [(null_mut(), null_mut()); NPIN];
unsafe fn pin_slot(n: *mut Vnode) -> Option<usize> {
    if n.is_null() || (*n).data.is_null() { return None; }
    (0..NPIN).find(|&i| PINS[i].0 == n && PINS[i].1 == (*n).data)
}
/// The node's contents, guaranteed not to change or be freed (NULL if it cannot be pinned).
#[no_mangle]
pub unsafe extern "C" fn vfs_pin(n: *mut Vnode) -> *const u8 {
    ensure_loaded(n);
    if n.is_null() || (*n).dir != 0 || (*n).data.is_null() { return core::ptr::null(); }
    if pin_slot(n).is_some() { return (*n).data; }
    let Some(i) = (0..NPIN).find(|&i| PINS[i].0.is_null()) else { return core::ptr::null() };
    PINS[i] = (n, (*n).data);
    (*n).data
}
/// Before changing a node's buffer in place: a pinned buffer is replaced by a private copy.
#[no_mangle]
pub unsafe extern "C" fn vfs_unshare(n: *mut Vnode) -> CBool {
    let Some(i) = pin_slot(n) else { return 1 };
    let cap = (*n).cap.max((*n).size).max(1);
    let d = kmalloc(cap as usize) as *mut u8;
    if d.is_null() { return 0; }
    core::ptr::copy_nonoverlapping((*n).data, d, (*n).size as usize);
    (*n).data = d;
    (*n).cap = cap;
    PINS[i] = (null_mut(), null_mut());
    1
}
/// Frees a node's buffer unless it is pinned (then it stays with its pinner).
pub(super) unsafe fn release_data(n: *mut Vnode) {
    match pin_slot(n) { Some(i) => PINS[i] = (null_mut(), null_mut()), None => kfree((*n).data as *mut c_void) }
    (*n).data = null_mut();
}
unsafe fn unlink_node(n: *mut Vnode) {
    if (*n).parent.is_null() { return; }
    let mut pp = &mut (*(*n).parent).child as *mut *mut Vnode;
    while !(*pp).is_null() {
        if *pp == n { *pp = (*n).next; break; }
        pp = &mut (**pp).next;
    }
    (*n).next = null_mut();
}
unsafe fn is_within(n: *mut Vnode, top: *mut Vnode) -> bool {
    let mut p = n;
    while !p.is_null() { if p == top { return true; } p = (*p).parent; }
    false
}
unsafe fn path_of(n: *mut Vnode) -> Vec<u8> {
    if n.is_null() { return Vec::new(); }
    if (*n).parent.is_null() { let mut v = name(n).to_vec(); v.push(b'\\'); return v; }
    let mut parts = Vec::new();
    let mut p = n;
    while !p.is_null() && parts.len() < 64 { parts.push(name(p)); p = (*p).parent; }
    let mut out = Vec::new();
    for (i, s) in parts.iter().enumerate().rev() { out.extend_from_slice(s); if i != 0 { out.push(b'\\'); } }
    out
}

#[no_mangle]
pub unsafe extern "C" fn vfs_ext(name: *const c_char) -> *const c_char {
    let s = cstr(name);
    match s.iter().rposition(|&c| c == b'.') { Some(i) if i > 0 => name.add(i + 1), _ => c"".as_ptr() }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_new_node(parent: *mut Vnode, nm: *const c_char, dir: CBool) -> *mut Vnode {
    let n = kzalloc(core::mem::size_of::<Vnode>()) as *mut Vnode;
    if n.is_null() { return n; }
    set_name(n, cstr(nm));
    (*n).dir = (dir != 0) as CBool;
    rtc_now(&mut (*n).mtime);
    if !parent.is_null() {
        (*n).parent = parent;
        (*n).drive = (*parent).drive;
        append_child(parent, n);
        hook(VH_CREATE, n, core::ptr::null());
    }
    bump();
    n
}

#[no_mangle]
pub unsafe extern "C" fn vfs_init() {
    let c = vfs_new_node(null_mut(), c"C:".as_ptr(), 1);
    (*c).drive = b'C' as c_int;
    DRIVES[2] = c;
    LABELS[2] = [0; 32];
    LABELS[2][..10].copy_from_slice(b"Local Disk");
    KINDS[2] = 1;
    for p in [&b"C:\\WRLD\\System32"[..], b"C:\\Program Files", b"C:\\Documents and Settings\\User\\Desktop",
              b"C:\\Documents and Settings\\User\\My Documents", b"C:\\tmp"] {
        mkdir_p(p);
    }
}
#[no_mangle]
pub unsafe extern "C" fn vfs_root(d: c_int) -> *mut Vnode { letter_index(d).map_or(null_mut(), |i| DRIVES[i]) }
#[no_mangle]
pub unsafe extern "C" fn vfs_drive_letter(mut n: *mut Vnode) -> c_char {
    while !n.is_null() && !(*n).parent.is_null() { n = (*n).parent; }
    if n.is_null() { b'C' as c_char } else { (*n).drive as c_char }
}
#[no_mangle]
pub unsafe extern "C" fn vfs_drive_label(l: c_char) -> *const c_char {
    letter_index(l as u8 as c_int).map_or(c"".as_ptr(), |i| LABELS[i].as_ptr() as *const c_char)
}
#[no_mangle]
pub unsafe extern "C" fn vfs_drive_kind(l: c_char) -> c_int {
    letter_index(l as u8 as c_int).filter(|&i| !DRIVES[i].is_null()).map_or(0, |i| KINDS[i])
}
#[no_mangle]
pub unsafe extern "C" fn vfs_mount(root: *mut Vnode, label: *const c_char, kind: c_int) -> c_char {
    if root.is_null() { return 0; }
    for i in 3..26 {
        if DRIVES[i].is_null() {
            DRIVES[i] = root;
            (*root).parent = null_mut();
            let l = b'A' + i as u8;
            set_name(root, &[l, b':']);
            set_drive(root, l as c_int);
            put_cbytes(LABELS[i].as_mut_ptr() as *mut c_char, 32, cstr(label));
            KINDS[i] = kind;
            bump();
            return l as c_char;
        }
    }
    0
}
#[no_mangle]
pub unsafe extern "C" fn vfs_unmount(l: c_char) -> CBool {
    let u = (l as u8).to_ascii_uppercase();
    if !(b'D'..=b'Z').contains(&u) || DRIVES[(u - b'A') as usize].is_null() { return 0; }
    let i = (u - b'A') as usize;
    free_tree(DRIVES[i]);
    DRIVES[i] = null_mut();
    bump();
    1
}

unsafe fn child_named(dir: *mut Vnode, nm: &[u8]) -> *mut Vnode {
    if dir.is_null() || (*dir).dir == 0 { return null_mut(); }
    let mut c = (*dir).child;
    while !c.is_null() {
        if name(c).eq_ignore_ascii_case(nm) { return c; }
        c = (*c).next;
    }
    null_mut()
}
#[no_mangle]
pub unsafe extern "C" fn vfs_lookup_child(dir: *mut Vnode, nm: *const c_char) -> *mut Vnode { child_named(dir, cstr(nm)) }

#[no_mangle]
pub unsafe extern "C" fn path_normalize(p: *mut c_char) {
    if p.is_null() { return; }
    let mut q = p;
    while *q != 0 { if *q == b'/' as c_char { *q = b'\\' as c_char; } q = q.add(1); }
}
#[no_mangle]
pub unsafe extern "C" fn path_basename(p: *const c_char) -> *const c_char { if p.is_null() { p } else { p.add(vp::basename_at(cstr(p))) } }
#[no_mangle]
pub unsafe extern "C" fn path_dirname(p: *const c_char, out: *mut c_char, cap: c_int) {
    put_cbytes(out, cap, &vp::dirname(cstr(p), cap.max(0) as usize))
}
#[no_mangle]
pub unsafe extern "C" fn path_join(out: *mut c_char, cap: c_int, a: *const c_char, b: *const c_char) {
    put_cbytes(out, cap, &vp::join(cstr(a), cstr(b), cap.max(0) as usize))
}

fn has_drive(p: &[u8]) -> bool { p.len() >= 2 && p[0].is_ascii_alphabetic() && p[1] == b':' }
fn is_sep(c: u8) -> bool { c == b'/' || c == b'\\' }

unsafe fn resolve(path: &[u8], cwd: Option<&[u8]>) -> *mut Vnode {
    let mut buf;
    let mut path = path;
    if path.first() == Some(&b'~') {
        buf = USER_HOME.to_vec();
        vp::lcat(&mut buf, &path[1..], 512);
        path = &buf;
    }
    let mut cur = if has_drive(path) {
        let r = vfs_root(path[0] as c_int); path = &path[2..]; r
    } else if path.first().is_some_and(|&c| is_sep(c)) {
        vfs_root(match cwd { Some(c) if has_drive(c) => c[0], _ => b'C' } as c_int)
    } else {
        match cwd { Some(c) => resolve(c, None), None => vfs_root(b'C' as c_int) }
    };
    if cur.is_null() { return cur; }
    for part in vp::parts(path) {
        match part {
            Part::Here => {}
            Part::Up => if !(*cur).parent.is_null() { cur = (*cur).parent; },
            Part::Name(n) => { cur = child_named(cur, n); if cur.is_null() { return cur; } }
        }
    }
    cur
}
/// Accepts C:\a\b, C:/a, /a/b (on the current drive), a\b (from `cwd`), "..", "." and "~".
#[no_mangle]
pub unsafe extern "C" fn vfs_resolve(path: *const c_char, cwd: *const c_char) -> *mut Vnode {
    if path.is_null() { return null_mut(); }
    let n = resolve(cstr(path), (!cwd.is_null()).then(|| cstr(cwd)));
    ensure_loaded(n);
    n
}
#[no_mangle]
pub unsafe extern "C" fn vfs_path(n: *mut Vnode, buf: *mut c_char, cap: c_int) { put_cbytes(buf, cap, &path_of(n)) }

#[no_mangle]
pub unsafe extern "C" fn vfs_create(path: *const c_char, cwd: *const c_char, dir: CBool) -> *mut Vnode {
    if path.is_null() { return null_mut(); }
    let e = vfs_resolve(path, cwd);
    if !e.is_null() { return e; }
    let p = cstr(path);
    let mut full = if !has_drive(p) && !cwd.is_null() && !p.first().is_some_and(|&c| is_sep(c)) { vp::join(cstr(cwd), p, 512) } else { vp::lcpy(p, 512) };
    vp::normalize(&mut full);
    let parent = vp::dirname(&full, 512);
    let pn = if !parent.is_empty() { resolve(&parent, (!cwd.is_null()).then(|| cstr(cwd))) }
             else if cwd.is_null() { resolve(b"C:\\", None) } else { resolve(cstr(cwd), None) };
    if pn.is_null() || (*pn).dir == 0 || (*pn).readonly != 0 { return null_mut(); }
    let base = &full[vp::basename_at(&full)..];
    if base.is_empty() { return null_mut(); }
    vfs_new_node(pn, cvec(base).as_ptr() as *const c_char, dir)
}

unsafe fn mkdir_p(path: &[u8]) -> *mut Vnode {
    let mut buf = vp::lcpy(path, 512);
    vp::normalize(&mut buf);
    let Some(&d) = buf.first() else { return null_mut() };
    let mut cur = vfs_root(d as c_int);
    for part in buf[buf.len().min(2)..].split(|&c| c == b'\\').filter(|s| !s.is_empty()) {
        if cur.is_null() || (*cur).dir == 0 { return null_mut(); }
        let n = child_named(cur, part);
        cur = if n.is_null() { vfs_new_node(cur, cvec(part).as_ptr() as *const c_char, 1) } else { n };
    }
    cur
}
#[no_mangle]
pub unsafe extern "C" fn vfs_mkdir_p(path: *const c_char) -> *mut Vnode { mkdir_p(cstr(path)) }

#[no_mangle]
pub unsafe extern "C" fn vfs_write_node(n: *mut Vnode, data: *const c_void, len: u32) -> CBool {
    if n.is_null() || (*n).dir != 0 || (*n).readonly != 0 || (data.is_null() && len != 0) { return 0; }
    let src = if len == 0 { &[][..] } else { core::slice::from_raw_parts(data as *const u8, len as usize) };
    if !(*n).data.is_null() && (*n).size == len && core::slice::from_raw_parts((*n).data, len as usize) == src { return 1; }
    if len > (*n).cap || (*n).data.is_null() || pin_slot(n).is_some() {
        let Some(cap) = len.checked_add(64) else { return 0 };
        let d = kmalloc(cap as usize) as *mut u8;
        if d.is_null() { return 0; }
        release_data(n);
        (*n).data = d;
        (*n).cap = cap;
    }
    core::ptr::copy(src.as_ptr(), (*n).data, len as usize);
    (*n).size = len;
    rtc_now(&mut (*n).mtime);
    bump();
    hook(VH_WRITE, n, core::ptr::null());
    1
}
#[no_mangle]
pub unsafe extern "C" fn vfs_append_node(n: *mut Vnode, data: *const c_void, len: u32) -> CBool {
    if n.is_null() || (*n).dir != 0 || (*n).readonly != 0 || (data.is_null() && len != 0) { return 0; }
    let Some(total) = (*n).size.checked_add(len) else { return 0 };
    if vfs_unshare(n) == 0 { return 0; }
    if total > (*n).cap {
        let Some(cap) = total.checked_mul(2).and_then(|c| c.checked_add(64)) else { return 0 };
        let d = krealloc((*n).data as *mut c_void, cap as usize) as *mut u8;
        if d.is_null() { return 0; }
        (*n).data = d;
        (*n).cap = cap;
    }
    if len != 0 { core::ptr::copy(data as *const u8, (*n).data.add((*n).size as usize), len as usize); }
    (*n).size = total;
    rtc_now(&mut (*n).mtime);
    bump();
    hook(VH_WRITE, n, core::ptr::null());
    1
}
#[no_mangle]
pub unsafe extern "C" fn vfs_write_file(path: *const c_char, data: *const c_void, len: u32) -> *mut Vnode {
    let n = vfs_create(path, core::ptr::null(), 0);
    if n.is_null() || (*n).dir != 0 { return null_mut(); }
    if vfs_write_node(n, data, len) != 0 { n } else { null_mut() }
}
#[no_mangle]
pub unsafe extern "C" fn vfs_read_file(path: *const c_char, size: *mut u32) -> *mut u8 {
    let n = vfs_resolve(path, core::ptr::null());
    if n.is_null() || (*n).dir != 0 { return null_mut(); }
    let sz = (*n).size as usize;
    let b = kmalloc(sz + 1) as *mut u8;
    if b.is_null() { return b; }
    if sz != 0 { core::ptr::copy_nonoverlapping((*n).data, b, sz); }
    *b.add(sz) = 0;
    if !size.is_null() { *size = sz as u32; }
    b
}

#[no_mangle]
pub unsafe extern "C" fn vfs_delete(n: *mut Vnode) -> CBool {
    if n.is_null() || (*n).parent.is_null() || (*n).readonly != 0 || (*(*n).parent).readonly != 0 { return 0; }
    hook(VH_DELETE, n, core::ptr::null());
    unlink_node(n);
    free_tree(n);
    bump();
    1
}
#[no_mangle]
pub unsafe extern "C" fn vfs_rename(n: *mut Vnode, nn: *const c_char) -> CBool {
    let new = cstr(nn);
    if n.is_null() || (*n).parent.is_null() || (*n).readonly != 0 || new.is_empty() { return 0; }
    if !child_named((*n).parent, new).is_null() && !name(n).eq_ignore_ascii_case(new) { return 0; }
    let old = cvec(&vp::lcpy(&path_of(n), 512));
    set_name(n, new);
    bump();
    hook(VH_RENAME, n, old.as_ptr() as *const c_char);
    1
}
#[no_mangle]
pub unsafe extern "C" fn vfs_move(n: *mut Vnode, nd: *mut Vnode) -> CBool {
    if n.is_null() || nd.is_null() || (*n).parent.is_null() || (*nd).dir == 0 || (*nd).readonly != 0 || (*n).readonly != 0 { return 0; }
    if is_within(nd, n) { return 0; }
    let old = cvec(&vp::lcpy(&path_of(n), 512));
    unlink_node(n);
    (*n).parent = nd;
    append_child(nd, n);
    set_drive(n, (*nd).drive);
    bump();
    hook(VH_RENAME, n, old.as_ptr() as *const c_char);
    1
}
#[no_mangle]
pub unsafe extern "C" fn vfs_copy(n: *mut Vnode, dd: *mut Vnode, newname: *const c_char) -> *mut Vnode {
    if n.is_null() || dd.is_null() || (*dd).dir == 0 || (*dd).readonly != 0 || ((*n).dir != 0 && is_within(dd, n)) { return null_mut(); }
    // (source, folder to copy it into, name) in the C version's order: each node, then its
    // contents, then its next sibling
    let mut top: *mut Vnode = null_mut();
    let mut stack = Vec::from([(n, dd, newname)]);
    while let Some((s, into, nm)) = stack.pop() {
        let c = vfs_new_node(into, if nm.is_null() { (*s).name.as_ptr() } else { nm }, (*s).dir);
        if top.is_null() { top = c; }
        if c.is_null() { continue; }
        if (*s).dir == 0 {
            ensure_loaded(s);
            if (*s).size != 0 && !(*s).data.is_null() { vfs_write_node(c, (*s).data as *const c_void, (*s).size); }
        } else {
            stack.extend(kids(s).into_iter().rev().map(|k| (k, c, core::ptr::null())));
        }
    }
    top
}
#[no_mangle]
pub unsafe extern "C" fn vfs_count_children(d: *mut Vnode) -> c_int { kids(d).len() as c_int }
#[no_mangle]
pub unsafe extern "C" fn vfs_tree_size(n: *mut Vnode) -> u32 { subtree(n).iter().fold(0u32, |s, &c| s.wrapping_add((*c).size)) }
/// Folders first, then by name.
#[no_mangle]
pub unsafe extern "C" fn vfs_sort_children(d: *mut Vnode) {
    if d.is_null() { return; }
    let k = kids(d);
    let items: Vec<(bool, &[u8])> = k.iter().map(|&c| ((*c).dir != 0, name(c))).collect();
    let order = vp::sort_order(&items);
    let mut pp = &mut (*d).child as *mut *mut Vnode;
    for i in order { let c = k[i]; *pp = c; pp = &mut (*c).next; }
    *pp = null_mut();
}

/// Unpacks the initrd (a ustar archive) under `dest`. As each file is copied out, the part of
/// the archive before it goes back to the heap (mm_initrd_consumed), so a 64 MB machine can
/// unpack it.
#[no_mangle]
pub unsafe extern "C" fn tar_extract(data: *const u8, size: u32, dest: *mut Vnode) {
    let entries = if data.is_null() || size == 0 { Vec::new() } else { vp::tar_entries(core::slice::from_raw_parts(data, size as usize)) };
    let destpath = vp::lcpy(&path_of(dest), 256);
    let mut files: c_int = 0;
    for e in entries {
        let mut full = vp::join(&destpath, &e.name, 512);
        vp::normalize(&mut full);
        if e.dir { mkdir_p(&full); continue; }
        mkdir_p(&vp::dirname(&full, 512));
        vfs_write_file(cvec(&full).as_ptr() as *const c_char, data.add(e.data.start) as *const c_void, e.data.len() as u32);
        files += 1;
        mm_initrd_consumed(data.add(e.data.end));
    }
    klog(c"initrd: extracted %d files".as_ptr(), files);
}
