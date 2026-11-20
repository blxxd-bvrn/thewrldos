//! More of kernel32 (was `src/win32/k32more.c`): the process environment, version checks,
//! INI files, atoms, Tool Help snapshots, SearchPath, console input records, file
//! information classes, the wide lstr* functions and the serial-port and debugging calls
//! programs probe for. The parsing (INI files, %VAR% expansion, version conditions, atoms)
//! is in `crate::w32util`; the process's state is reached through src/win32/rsglue.c.

use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};

use super::w32::{a2w, alloc, astr, free, lookup, set_err, wstr};
use super::Vnode;
use crate::w32util as u;

extern "C" {
    fn w32_env_lookup(n: *const c_char) -> *const c_char;
    fn w32_env_store(n: *const c_char, v: *const c_char);
    fn w32_env_at(i: c_int) -> *const c_char;
    fn w32_proc_cwd() -> *const c_char;
    fn w32_proc_exe() -> *const c_char;
    fn w32_proc_pid() -> c_int;
    fn w32_pid_exe(pid: c_int) -> *const c_char;
    fn w32_mod_info(i: c_int, name: *mut *const c_char, path: *mut *const c_char, base: *mut usize, size: *mut u32) -> super::CBool;
    fn w32_console_key() -> c_int;
    fn w32_file_node(h: usize) -> *mut Vnode;
    fn w32_fatal_app_exit(m: *const c_char) -> !;
    fn w32_exit_process(code: c_int) -> !;
    fn w32_msize(p: *mut c_void) -> u32;
    fn w32_realloc(p: *mut c_void, ptr: *mut c_void, n: u32) -> *mut c_void;
    fn w32_cur() -> *mut c_void;
    fn pe_proc_list(pids: *mut c_int, max: c_int) -> c_int;
    fn vfs_resolve(path: *const c_char, cwd: *const c_char) -> *mut Vnode;
    fn vfs_write_file(path: *const c_char, data: *const c_void, len: u32) -> *mut c_void;
    fn vfs_path(n: *mut Vnode, buf: *mut c_char, cap: c_int);
    fn vfs_drive_kind(letter: c_char) -> c_int;
    fn path_join(out: *mut c_char, cap: c_int, a: *const c_char, b: *const c_char);
    fn path_normalize(p: *mut c_char);
    fn path_basename(p: *const c_char) -> *const c_char;
    fn path_dirname(p: *const c_char, out: *mut c_char, cap: c_int);
    fn timer_ticks() -> u32;
    static user_name: [c_char; 32];
    static computer_name: [c_char; 32];
}

type H = usize;
type W = *const u16;
type A = *const u8;
type P = *mut u32;

// ---------------------------------------------------------------- strings

/// A wide string as ANSI, at most `cap - 1` characters (the C files' w2a).
unsafe fn w2a(w: W, cap: usize) -> Vec<u8> { wstr(w, cap - 1).iter().map(|&c| if c < 256 { c as u8 } else { b'?' }).collect() }
/// NUL-terminated copy for C.
fn cz(s: &[u8]) -> Vec<u8> { let mut v = s.to_vec(); v.push(0); v }
unsafe fn cstr<'a>(p: *const c_char) -> &'a [u8] { astr(p as *const u8) }
unsafe fn cname(s: &[c_char; 32]) -> &[u8] { let b = &*(s as *const [c_char; 32] as *const [u8; 32]); &b[..b.iter().position(|&c| c == 0).unwrap_or(32)] }
/// strlcpy into a caller's buffer of `n` bytes; returns the bytes copied.
unsafe fn put_a(src: &[u8], dst: *mut u8, n: usize) -> usize {
    if dst.is_null() || n == 0 { return 0; }
    let k = src.len().min(n - 1);
    core::ptr::copy_nonoverlapping(src.as_ptr(), dst, k); *dst.add(k) = 0;
    k
}
unsafe fn env_get(n: &[u8]) -> Option<Vec<u8>> { let v = w32_env_lookup(cz(n).as_ptr() as *const c_char); if v.is_null() { None } else { Some(cstr(v).to_vec()) } }
unsafe fn env_all() -> Vec<Vec<u8>> {
    let mut v = Vec::new();
    loop { let e = w32_env_at(v.len() as c_int); if e.is_null() { break; } v.push(cstr(e).to_vec()); }
    v
}
unsafe fn call0(dll: &core::ffi::CStr, name: &core::ffi::CStr) -> *mut c_void { lookup(dll, name) }
unsafe fn create_event(manual: i32) -> H {
    let f = call0(c"kernel32.dll", c"CreateEventA");
    if f.is_null() { return 0; }
    let f: winfn!(fn(*mut c_void, i32, i32, *const c_char) -> H) = core::mem::transmute(f);
    f(core::ptr::null_mut(), manual, 0, core::ptr::null())
}

// ---------------------------------------------------------------- C path helpers

/// A path in a 600-byte C buffer: `path_join(a, b)`, normalized.
unsafe fn joined(a: &[u8], b: &[u8]) -> Vec<u8> {
    let mut out = [0u8; 600];
    path_join(out.as_mut_ptr() as *mut c_char, 600, cz(a).as_ptr() as *const c_char, cz(b).as_ptr() as *const c_char);
    path_normalize(out.as_mut_ptr() as *mut c_char);
    cstr(out.as_ptr() as *const c_char).to_vec()
}
unsafe fn normalized(p: &[u8]) -> Vec<u8> {
    let mut b = [0u8; 600];
    let n = p.len().min(599);
    b[..n].copy_from_slice(&p[..n]);
    path_normalize(b.as_mut_ptr() as *mut c_char);
    cstr(b.as_ptr() as *const c_char).to_vec()
}
/// Where path_basename puts the name in `p`.
unsafe fn basename_at(p: &[u8]) -> usize { let c = cz(p); path_basename(c.as_ptr() as *const c_char) as usize - c.as_ptr() as usize }
unsafe fn node(path: &[u8]) -> *mut Vnode { vfs_resolve(cz(path).as_ptr() as *const c_char, core::ptr::null()) }
unsafe fn node_data<'a>(n: *mut Vnode) -> &'a [u8] {
    if n.is_null() || (*n).data.is_null() { &[] } else { core::slice::from_raw_parts((*n).data, (*n).size as usize) }
}

// ---------------------------------------------------------------- INI files

/// A file name without a folder is in C:\WRLD, as Windows keeps them in the Windows folder.
unsafe fn ini_path(file: Option<&[u8]>) -> Vec<u8> {
    let f = match file { Some(f) if !f.is_empty() => f, _ => b"win.ini" };
    let mut p = if !f.iter().any(|&c| c == b'\\' || c == b'/' || c == b':') {
        let mut p = b"C:\\WRLD\\".to_vec(); p.extend_from_slice(f); p.truncate(299); p
    } else if f.len() >= 2 && f[0].is_ascii_alphabetic() && f[1] == b':' {
        f[..f.len().min(299)].to_vec()
    } else {
        let mut out = [0u8; 300];
        path_join(out.as_mut_ptr() as *mut c_char, 300, w32_proc_cwd(), cz(f).as_ptr() as *const c_char);
        cstr(out.as_ptr() as *const c_char).to_vec()
    };
    for c in &mut p { if *c == b'/' { *c = b'\\'; } }
    normalized(&p)
}
unsafe fn ini_node(path: &[u8], create: bool) -> *mut Vnode {
    let mut n = node(path);
    if n.is_null() && create { n = vfs_write_file(cz(path).as_ptr() as *const c_char, b"".as_ptr() as *const c_void, 0) as *mut Vnode; }
    if n.is_null() || (*n).dir != 0 { core::ptr::null_mut() } else { n }
}
unsafe fn ini_get(sec: Option<&[u8]>, key: Option<&[u8]>, def: Option<&[u8]>, buf: *mut u8, size: u32, file: Option<&[u8]>) -> u32 {
    if buf.is_null() || size == 0 { return 0; }
    let n = ini_node(&ini_path(file), false);
    let out = core::slice::from_raw_parts_mut(buf, size as usize);
    u::ini_get(node_data(n), sec, key, def, out) as u32
}
unsafe fn ini_put(sec: Option<&[u8]>, key: Option<&[u8]>, val: Option<&[u8]>, file: Option<&[u8]>) -> i32 {
    let path = ini_path(file);
    let n = ini_node(&path, true);
    if n.is_null() { set_err(3); return 0; }
    let Some(sec) = sec else { return 1 };                // flush the cache: nothing cached
    let t = u::ini_put(node_data(n), sec, key, val);
    vfs_write_file(cz(&path).as_ptr() as *const c_char, t.as_ptr() as *const c_void, t.len() as u32);
    1
}
unsafe fn section_get(sec: Option<&[u8]>, buf: *mut u8, size: u32, file: Option<&[u8]>) -> u32 {
    let n = ini_node(&ini_path(file), false);
    if buf.is_null() || size < 2 { return 0; }
    u::ini_section(node_data(n), sec, core::slice::from_raw_parts_mut(buf, size as usize)) as u32
}
unsafe fn section_put(sec: Option<&[u8]>, data: &[u8], file: Option<&[u8]>) -> i32 {
    ini_put(sec, None, None, file);
    for p in data.split(|&c| c == 0).take_while(|p| !p.is_empty()) {
        let Some(eq) = p.iter().position(|&c| c == b'=') else { continue };
        ini_put(sec, Some(&p[..eq.min(255)]), Some(&p[eq + 1..]), file);
    }
    1
}
/// A wide argument as ANSI (None for NULL), cut like the C version.
unsafe fn wopt(w: W, cap: usize) -> Option<Vec<u8>> { if w.is_null() { None } else { Some(w2a(w, cap)) } }
unsafe fn aopt<'a>(a: A) -> Option<&'a [u8]> { if a.is_null() { None } else { Some(astr(a)) } }
/// Widens what an ANSI INI call wrote: through the count, then (for lists) to the double NUL.
unsafe fn widen_out(o: &[u8], b: *mut u16, n: u32, r: u32, list: bool) {
    for i in 0..n.min(r + 1) as usize { *b.add(i) = o[i] as u16; }
    if list { for i in 0..n as usize { *b.add(i) = o[i] as u16; if o[i] == 0 && i > 0 && o[i - 1] == 0 { break; } } }
}
const WIN_INI: &[u16] = &[b'w' as u16, b'i' as u16, b'n' as u16, b'.' as u16, b'i' as u16, b'n' as u16, b'i' as u16, 0];

// ---------------------------------------------------------------- atoms and other state

struct Global<T>(core::cell::UnsafeCell<T>);
unsafe impl<T> Sync for Global<T> {}
impl<T> Global<T> { const fn new(v: T) -> Self { Global(core::cell::UnsafeCell::new(v)) } fn get(&self) -> &mut T { unsafe { &mut *self.0.get() } } }
static ATOMS: Global<u::Atoms> = Global::new(u::Atoms::new());
static PENDING_KEY: Global<i32> = Global::new(-1);
static ERROR_MODE: Global<u32> = Global::new(0);
static DUMMY: Global<i32> = Global::new(0);

unsafe fn get_atom_name(a: u16, b: *mut u8, n: i32) -> u32 {
    let Some(name) = ATOMS.get().name(a) else { set_err(6); return 0 };
    if n <= 0 { return 0; }
    put_a(&name, b, n as usize) as u32
}

// ---------------------------------------------------------------- Tool Help

#[repr(C)] struct Pe32A { size: u32, usage: u32, pid: u32, heap: usize, module: u32, threads: u32, ppid: u32, pri: i32, flags: u32, exe: [u8; 260] }
#[repr(C)] struct Pe32W { size: u32, usage: u32, pid: u32, heap: usize, module: u32, threads: u32, ppid: u32, pri: i32, flags: u32, exe: [u16; 260] }
#[repr(C)] struct Me32A { size: u32, mid: u32, pid: u32, gusage: u32, pusage: u32, base: usize, bsize: u32, hmod: usize, name: [u8; 256], path: [u8; 260] }
#[repr(C)] struct Me32W { size: u32, mid: u32, pid: u32, gusage: u32, pusage: u32, base: usize, bsize: u32, hmod: usize, name: [u16; 256], path: [u16; 260] }
#[repr(C)] struct Te32 { size: u32, usage: u32, tid: u32, owner: u32, base: i32, delta: i32, flags: u32 }
const SNAP_MAGIC: u32 = 0x534E_4150;
#[repr(C)] struct Snap { magic: u32, npid: i32, pids: [i32; 64], pos: i32, mpos: i32, tpos: i32 }
unsafe fn snap<'a>(h: H) -> Option<&'a mut Snap> {
    let s = h as *mut Snap;
    if s.is_null() || h <= 0x10000 || (w32_msize(s as *mut c_void) as usize) < core::mem::size_of::<Snap>() || (*s).magic != SNAP_MAGIC { None } else { Some(&mut *s) }
}
unsafe fn proc_entry(s: &mut Snap) -> Option<(u32, Vec<u8>)> {
    let i = s.pos; s.pos += 1;
    if i >= s.npid { return None; }
    let pid = s.pids[i as usize];
    let p = w32_pid_exe(pid);
    let exe = if p.is_null() { b"?".to_vec() } else { cstr(path_basename(p)).to_vec() };
    Some((pid as u32, exe))
}
unsafe fn mod_entry(s: &mut Snap) -> Option<(&'static [u8], &'static [u8], usize, u32)> {
    let (mut n, mut p, mut b, mut z) = (core::ptr::null(), core::ptr::null(), 0usize, 0u32);
    if w32_mod_info(s.mpos, &mut n, &mut p, &mut b, &mut z) == 0 { return None; }
    s.mpos += 1;
    Some((cstr(n), cstr(p), b, z))
}
/// Clears a Tool Help entry (all of it, as the C version did) keeping its size field.
unsafe fn clear<T>(e: *mut T) { let sz = *(e as *mut u32); core::ptr::write_bytes(e as *mut u8, 0, core::mem::size_of::<T>()); *(e as *mut u32) = sz; }

// ---------------------------------------------------------------- SearchPath

unsafe fn search(path: Option<&[u8]>, file: &[u8], ext: Option<&[u8]>) -> Option<Vec<u8>> {
    let mut name = file[..file.len().min(299)].to_vec();
    if let Some(e) = ext { if !name[basename_at(&name)..].contains(&b'.') { name.extend_from_slice(e); name.truncate(299); } }
    if (name.len() >= 2 && name[0].is_ascii_alphabetic() && name[1] == b':') || name.first().is_some_and(|&c| c == b'\\' || c == b'/') {
        let cand = if name[0] == b'\\' || name[0] == b'/' { let mut c = b"C:".to_vec(); c.extend_from_slice(&name); c } else { name };
        let cand = normalized(&cand);
        if !node(&cand).is_null() { return Some(cand); }
        set_err(2); return None;
    }
    let dirs = match path {
        Some(p) => p[..p.len().min(2047)].to_vec(),
        None => {
            let mut exedir = [0u8; 300];
            path_dirname(w32_proc_exe(), exedir.as_mut_ptr() as *mut c_char, 300);
            let mut d = cstr(exedir.as_ptr() as *const c_char).to_vec();
            d.extend_from_slice(b";C:\\WRLD\\System32;C:\\WRLD;");
            d.extend_from_slice(cstr(w32_proc_cwd()));
            d.push(b';');
            if let Some(pe) = env_get(b"PATH") { d.extend_from_slice(&pe); }
            d.truncate(2047);
            d
        }
    };
    for d in dirs.split(|&c| c == b';').filter(|d| !d.is_empty()) {
        let cand = joined(d, &name);
        let v = node(&cand);
        if !v.is_null() && (*v).dir == 0 { return Some(cand); }
    }
    set_err(2);
    None
}

// ---------------------------------------------------------------- console input records

#[repr(C, packed)]
struct InRec { ty: u16, pad: u16, down: i32, repeat: u16, vk: u16, scan: u16, ch: u16, ctrl: u32 }

#[repr(C)] struct OsviA { size: u32, major: u32, minor: u32, build: u32, platform: u32, csd: [u8; 128], spmaj: u16, spmin: u16, suite: u16, ty: u8, res: u8 }
#[repr(C)] struct OsviW { size: u32, major: u32, minor: u32, build: u32, platform: u32, csd: [u16; 128], spmaj: u16, spmin: u16, suite: u16, ty: u8, res: u8 }
#[repr(C)] struct CpInfoExW { max: u32, def: [u8; 2], lead: [u8; 12], udef: u16, cp: u32, name: [u16; 260] }
#[repr(C)] struct Sps { ac: u8, flag: u8, pct: u8, res: u8, life: u32, full: u32 }

fn is_self(h: H) -> bool { h as u32 == 0xFFFF_FFFF }

winapi! {
    // ------------------------------------------------------------ environment
    fn GetEnvironmentVariableA(n: A, b: *mut u8, sz: u32) -> u32 {
        let Some(v) = (if n.is_null() { None } else { env_get(astr(n)) }) else { set_err(203); return 0 };
        let l = v.len() as u32;
        if b.is_null() || sz <= l { return l + 1; }
        put_a(&v, b, sz as usize); l
    }
    fn GetEnvironmentVariableW(n: W, b: *mut u16, sz: u32) -> u32 {
        let Some(v) = env_get(&w2a(n, 256)) else { set_err(203); return 0 };
        let l = v.len() as u32;
        if b.is_null() || sz <= l { return l + 1; }
        a2w(&v, b, sz as usize); l
    }
    fn SetEnvironmentVariableA(n: A, v: A) -> i32 {
        if n.is_null() || *n == 0 || astr(n).contains(&b'=') { set_err(87); return 0; }
        let vz = if v.is_null() { None } else { Some(cz(astr(v))) };
        w32_env_store(n as *const c_char, vz.as_ref().map_or(core::ptr::null(), |v| v.as_ptr() as *const c_char));
        1
    }
    fn SetEnvironmentVariableW(n: W, v: W) -> i32 {
        let a = cz(&w2a(n, 256));
        let b = if v.is_null() { None } else { Some(cz(&w2a(v, 2048))) };
        SetEnvironmentVariableA(a.as_ptr(), b.as_ref().map_or(core::ptr::null(), |b| b.as_ptr()))
    }
    fn GetEnvironmentStringsA() -> *mut u8 {
        let mut blk = Vec::new();
        for e in env_all() { blk.extend_from_slice(&e); blk.push(0); }
        blk.extend_from_slice(&[0, 0]);
        let b = alloc(blk.len());
        if !b.is_null() { core::ptr::copy_nonoverlapping(blk.as_ptr(), b, blk.len()); }
        b
    }
    fn GetEnvironmentStringsW() -> *mut u16 {
        let mut blk: Vec<u16> = Vec::new();
        for e in env_all() { blk.extend(e.iter().map(|&c| c as u16)); blk.push(0); }
        blk.extend_from_slice(&[0, 0]);
        let b = alloc(blk.len() * 2) as *mut u16;
        if !b.is_null() { core::ptr::copy_nonoverlapping(blk.as_ptr(), b, blk.len()); }
        b
    }
    fn FreeEnvironmentStringsA(e: *mut u8) -> i32 { free(e); 1 }
    fn FreeEnvironmentStringsW(e: *mut u16) -> i32 { free(e as *mut u8); 1 }
    fn SetEnvironmentStringsW(e: *mut u16) -> i32 { 1 }
    fn ExpandEnvironmentStringsA(s: A, d: *mut u8, n: u32) -> u32 {
        let out = u::expand_env(astr(s), |name| env_get(name));
        let k = out.len() as u32;
        if !d.is_null() && n > k { core::ptr::copy_nonoverlapping(out.as_ptr(), d, out.len()); *d.add(out.len()) = 0; }
        k + 1
    }
    fn ExpandEnvironmentStringsW(s: W, d: *mut u16, n: u32) -> u32 {
        let out = u::expand_env(&w2a(s, 4096), |name| env_get(name));
        let r = out.len() as u32 + 1;
        if !d.is_null() && n >= r { a2w(&out, d, n as usize); }
        r
    }

    // ------------------------------------------------------------ version: Windows 7 SP1
    fn GetVersion() -> u32 { 0x1DB1_0106 }                         // 6.1 build 7601
    fn GetVersionExA(v: *mut OsviA) -> i32 {
        let sz = (*v).size as usize;
        if sz < 148 { set_err(122); return 0; }
        core::ptr::write_bytes((v as *mut u8).add(4), 0, sz.min(core::mem::size_of::<OsviA>()) - 4);
        (*v).major = 6; (*v).minor = 1; (*v).build = 7601; (*v).platform = 2;
        core::ptr::copy_nonoverlapping(b"Service Pack 1".as_ptr(), core::ptr::addr_of_mut!((*v).csd) as *mut u8, 14);
        if sz >= core::mem::size_of::<OsviA>() { (*v).spmaj = 1; (*v).suite = 0x100; (*v).ty = 1; }
        1
    }
    fn GetVersionExW(v: *mut OsviW) -> i32 {
        let sz = (*v).size as usize;
        if sz < 276 { set_err(122); return 0; }
        core::ptr::write_bytes((v as *mut u8).add(4), 0, sz.min(core::mem::size_of::<OsviW>()) - 4);
        (*v).major = 6; (*v).minor = 1; (*v).build = 7601; (*v).platform = 2;
        a2w(b"Service Pack 1", (*v).csd.as_mut_ptr(), 128);
        if sz >= core::mem::size_of::<OsviW>() { (*v).spmaj = 1; (*v).suite = 0x100; (*v).ty = 1; }
        1
    }
    fn RtlGetVersion(v: *mut OsviW) -> i32 { GetVersionExW(v); 0 }
    fn VerSetConditionMask(mask: u64, ty: u32, cond: u8) -> u64 { u::ver_cond_mask(mask, ty, cond) }
    fn VerifyVersionInfoW(v: *const OsviW, ty: u32, mask: u64) -> i32 {
        let v = &*v;
        let ok = u::verify_version(v.major, v.minor, v.build, v.platform, v.spmaj, v.spmin, ty, mask);
        if !ok { set_err(1150); }                          // ERROR_OLD_WIN_VERSION
        ok as i32
    }
    fn VerifyVersionInfoA(v: *const OsviA, ty: u32, mask: u64) -> i32 {
        let v = &*v;
        let ok = u::verify_version(v.major, v.minor, v.build, v.platform, v.spmaj, v.spmin, ty, mask);
        if !ok { set_err(1150); }
        ok as i32
    }
    fn GetProductInfo(a: u32, b: u32, c: u32, d: u32, t: P) -> i32 { *t = 1; 1 }   // PRODUCT_ULTIMATE

    // ------------------------------------------------------------ INI files
    fn GetPrivateProfileStringA(s: A, k: A, d: A, b: *mut u8, n: u32, f: A) -> u32 { ini_get(aopt(s), aopt(k), aopt(d), b, n, aopt(f)) }
    fn GetPrivateProfileStringW(s: W, k: W, d: W, b: *mut u16, n: u32, f: W) -> u32 {
        let (as_, ak, ad, af) = (wopt(s, 256), wopt(k, 256), wopt(d, 1024), wopt(f, 300));
        if b.is_null() || n == 0 { return 0; }
        let mut o = alloc::vec![0u8; n as usize];
        let r = ini_get(as_.as_deref(), ak.as_deref(), ad.as_deref(), o.as_mut_ptr(), n, af.as_deref());
        widen_out(&o, b, n, r, s.is_null() || k.is_null());
        r
    }
    fn GetPrivateProfileIntA(s: A, k: A, d: i32, f: A) -> u32 {
        let mut b = [0u8; 64];
        if ini_get(aopt(s), aopt(k), Some(b""), b.as_mut_ptr(), 64, aopt(f)) == 0 { return d as u32; }
        u::kstrtol(cstr(b.as_ptr() as *const c_char), 0) as u32
    }
    fn GetPrivateProfileIntW(s: W, k: W, d: i32, f: W) -> u32 {
        let (as_, ak) = (cz(&w2a(s, 256)), cz(&w2a(k, 256)));
        let af = wopt(f, 300).map(|f| cz(&f));
        GetPrivateProfileIntA(as_.as_ptr(), ak.as_ptr(), d, af.as_ref().map_or(core::ptr::null(), |f| f.as_ptr()))
    }
    fn WritePrivateProfileStringA(s: A, k: A, v: A, f: A) -> i32 { ini_put(aopt(s), aopt(k), aopt(v), aopt(f)) }
    fn WritePrivateProfileStringW(s: W, k: W, v: W, f: W) -> i32 {
        let (as_, ak, av, af) = (wopt(s, 256), wopt(k, 256), wopt(v, 2048), wopt(f, 300));
        ini_put(as_.as_deref(), ak.as_deref(), av.as_deref(), af.as_deref())
    }
    fn GetPrivateProfileSectionA(s: A, b: *mut u8, n: u32, f: A) -> u32 { section_get(aopt(s), b, n, aopt(f)) }
    fn GetPrivateProfileSectionW(s: W, b: *mut u16, n: u32, f: W) -> u32 {
        let (as_, af) = (w2a(s, 256), wopt(f, 300));
        let mut o = alloc::vec![0u8; n as usize + 1];
        let r = section_get(Some(&as_), o.as_mut_ptr(), n, af.as_deref());
        if !b.is_null() { widen_out(&o, b, n, r, false); }
        r
    }
    fn WritePrivateProfileSectionA(s: A, d: A, f: A) -> i32 {
        let mut data = Vec::new();
        if !d.is_null() {                                   // "k=v\0k=v\0\0", read string by string
            let mut p = d;
            loop { let str_ = astr(p); if str_.is_empty() { break; } data.extend_from_slice(str_); data.push(0); p = p.add(str_.len() + 1); }
            data.push(0);
        }
        section_put(aopt(s), &data, aopt(f))
    }
    fn WritePrivateProfileSectionW(s: W, d: W, f: W) -> i32 {
        let (as_, af) = (w2a(s, 256), wopt(f, 300));
        let mut a = Vec::new();
        if !d.is_null() {
            let mut total = 0; while *d.add(total) != 0 || *d.add(total + 1) != 0 { total += 1; }
            for i in 0..total + 2 { let c = *d.add(i); a.push(if c < 256 { c as u8 } else { b'?' }); }
        }
        section_put(Some(&as_), &a, af.as_deref())
    }
    fn GetPrivateProfileSectionNamesA(b: *mut u8, n: u32, f: A) -> u32 { ini_get(None, None, None, b, n, aopt(f)) }
    fn GetPrivateProfileSectionNamesW(b: *mut u16, n: u32, f: W) -> u32 { GetPrivateProfileStringW(core::ptr::null(), core::ptr::null(), core::ptr::null(), b, n, f) }
    fn GetProfileStringA(s: A, k: A, d: A, b: *mut u8, n: u32) -> u32 { ini_get(aopt(s), aopt(k), aopt(d), b, n, Some(b"win.ini")) }
    fn GetProfileStringW(s: W, k: W, d: W, b: *mut u16, n: u32) -> u32 { GetPrivateProfileStringW(s, k, d, b, n, WIN_INI.as_ptr()) }
    fn GetProfileIntA(s: A, k: A, d: i32) -> u32 { GetPrivateProfileIntA(s, k, d, b"win.ini\0".as_ptr()) }
    fn GetProfileIntW(s: W, k: W, d: i32) -> u32 { GetPrivateProfileIntW(s, k, d, WIN_INI.as_ptr()) }
    fn WriteProfileStringA(s: A, k: A, v: A) -> i32 { ini_put(aopt(s), aopt(k), aopt(v), Some(b"win.ini")) }
    fn WriteProfileStringW(s: W, k: W, v: W) -> i32 { WritePrivateProfileStringW(s, k, v, WIN_INI.as_ptr()) }
    fn GetPrivateProfileStructA(s: A, k: A, d: *mut u8, n: u32, f: A) -> i32 {
        let mut b = [0u8; 1024];
        let l = ini_get(aopt(s), aopt(k), Some(b""), b.as_mut_ptr(), 1024, aopt(f));
        if l as u64 != n as u64 * 2 + 2 { return 0; }
        for i in 0..n as usize { *d.add(i) = u::kstrtoul(&b[2 * i..2 * i + 2], 16) as u8; }
        1
    }
    fn WritePrivateProfileStructA(s: A, k: A, d: *const u8, n: u32, f: A) -> i32 {
        const HX: &[u8; 16] = b"0123456789ABCDEF";
        let mut b = Vec::with_capacity(n as usize * 2 + 2);
        let mut sum = 0u8;
        for i in 0..n as usize { let c = *d.add(i); b.push(HX[(c >> 4) as usize]); b.push(HX[(c & 15) as usize]); sum = sum.wrapping_add(c); }
        b.push(HX[(sum >> 4) as usize]); b.push(HX[(sum & 15) as usize]);
        ini_put(aopt(s), aopt(k), Some(&b), aopt(f))
    }

    // ------------------------------------------------------------ atoms
    fn GlobalAddAtomA(n: A) -> u16 { if (n as usize) < 0x10000 { n as usize as u16 } else { ATOMS.get().add(astr(n)) } }
    fn GlobalAddAtomW(n: W) -> u16 { if (n as usize) < 0x10000 { n as usize as u16 } else { ATOMS.get().add(&w2a(n, 256)) } }
    fn GlobalFindAtomA(n: A) -> u16 { if (n as usize) < 0x10000 { n as usize as u16 } else { ATOMS.get().find(astr(n)) } }
    fn GlobalFindAtomW(n: W) -> u16 { if (n as usize) < 0x10000 { n as usize as u16 } else { ATOMS.get().find(&w2a(n, 256)) } }
    fn GlobalDeleteAtom(a: u16) -> u16 { ATOMS.get().delete(a); 0 }
    fn GlobalGetAtomNameA(a: u16, b: *mut u8, n: i32) -> u32 { get_atom_name(a, b, n) }
    fn GlobalGetAtomNameW(a: u16, b: *mut u16, n: i32) -> u32 {
        let mut t = [0u8; 256];
        let l = get_atom_name(a, t.as_mut_ptr(), 256);
        if l == 0 { return 0; }
        a2w(&t[..l as usize], b, n.max(0) as usize) as u32
    }

    // ------------------------------------------------------------ memory blocks
    fn GlobalSize(p: *mut c_void) -> usize { w32_msize(p) as usize }
    fn GlobalReAlloc(p: *mut c_void, n: usize, f: u32) -> *mut c_void {
        if f & 0x80 != 0 { return p; }                     // GMEM_MODIFY
        let old = w32_msize(p) as usize;
        let q = w32_realloc(w32_cur(), p, if n == 0 { 1 } else { n as u32 });
        if !q.is_null() && f & 0x40 != 0 && n > old { core::ptr::write_bytes((q as *mut u8).add(old), 0, n - old); }
        q
    }
    fn GlobalFlags(p: *mut c_void) -> u32 { 0 }
    fn GlobalHandle(p: *const c_void) -> *mut c_void { p as *mut c_void }
    fn LocalHandle(p: *const c_void) -> *mut c_void { p as *mut c_void }
    fn LocalFlags(p: *mut c_void) -> u32 { 0 }
    fn LocalLock(p: *mut c_void) -> *mut c_void { p }
    fn LocalUnlock(p: *mut c_void) -> i32 { 1 }
    fn GlobalCompact(n: u32) -> usize { 64 * 1024 * 1024 }
    fn GlobalFix(p: *mut c_void) -> () {}
    fn GlobalUnfix(p: *mut c_void) -> () {}

    // ------------------------------------------------------------ Tool Help snapshots
    fn CreateToolhelp32Snapshot(flags: u32, pid: u32) -> H {
        let s = alloc(core::mem::size_of::<Snap>()) as *mut Snap;
        if s.is_null() { return 0; }
        core::ptr::write_bytes(s as *mut u8, 0, core::mem::size_of::<Snap>());
        (*s).magic = SNAP_MAGIC;
        (*s).npid = pe_proc_list((*s).pids.as_mut_ptr(), 64);
        s as H
    }
    fn Process32First(h: H, e: *mut Pe32A) -> i32 { let Some(s) = snap(h) else { set_err(6); return 0 }; s.pos = 0; Process32Next(h, e) }
    fn Process32Next(h: H, e: *mut Pe32A) -> i32 {
        let Some(s) = snap(h) else { set_err(6); return 0 };
        let Some((pid, exe)) = proc_entry(s) else { set_err(18); return 0 };
        clear(e); (*e).pid = pid; (*e).threads = 1; (*e).pri = 8; put_a(&exe, (*e).exe.as_mut_ptr(), 260);
        1
    }
    fn Process32FirstW(h: H, e: *mut Pe32W) -> i32 { let Some(s) = snap(h) else { set_err(6); return 0 }; s.pos = 0; Process32NextW(h, e) }
    fn Process32NextW(h: H, e: *mut Pe32W) -> i32 {
        let Some(s) = snap(h) else { set_err(6); return 0 };
        let Some((pid, exe)) = proc_entry(s) else { set_err(18); return 0 };
        clear(e); (*e).pid = pid; (*e).threads = 1; (*e).pri = 8; a2w(&exe, (*e).exe.as_mut_ptr(), 260);
        1
    }
    fn Module32First(h: H, e: *mut Me32A) -> i32 { let Some(s) = snap(h) else { set_err(6); return 0 }; s.mpos = 0; Module32Next(h, e) }
    fn Module32Next(h: H, e: *mut Me32A) -> i32 {
        let Some(s) = snap(h) else { set_err(6); return 0 };
        let Some((name, path, base, size)) = mod_entry(s) else { set_err(18); return 0 };
        clear(e); (*e).mid = 1; (*e).pid = w32_proc_pid() as u32; (*e).gusage = 1; (*e).pusage = 1;
        (*e).base = base; (*e).bsize = size; (*e).hmod = base;
        put_a(name, (*e).name.as_mut_ptr(), 256); put_a(path, (*e).path.as_mut_ptr(), 260);
        1
    }
    fn Module32FirstW(h: H, e: *mut Me32W) -> i32 { let Some(s) = snap(h) else { set_err(6); return 0 }; s.mpos = 0; Module32NextW(h, e) }
    fn Module32NextW(h: H, e: *mut Me32W) -> i32 {
        let Some(s) = snap(h) else { set_err(6); return 0 };
        let Some((name, path, base, size)) = mod_entry(s) else { set_err(18); return 0 };
        clear(e); (*e).mid = 1; (*e).pid = w32_proc_pid() as u32; (*e).gusage = 1; (*e).pusage = 1;
        (*e).base = base; (*e).bsize = size; (*e).hmod = base;
        a2w(name, (*e).name.as_mut_ptr(), 256); a2w(path, (*e).path.as_mut_ptr(), 260);
        1
    }
    fn Thread32First(h: H, e: *mut Te32) -> i32 { let Some(s) = snap(h) else { set_err(6); return 0 }; s.tpos = 0; Thread32Next(h, e) }
    fn Thread32Next(h: H, e: *mut Te32) -> i32 {
        let Some(s) = snap(h) else { set_err(6); return 0 };
        if s.tpos >= s.npid { set_err(18); return 0; }
        let pid = s.pids[s.tpos as usize] as u32; s.tpos += 1;
        clear(e); (*e).tid = pid; (*e).owner = pid; (*e).base = 8;
        1
    }

    // ------------------------------------------------------------ SearchPath
    fn SearchPathA(path: A, file: A, ext: A, n: u32, out: *mut u8, part: *mut *mut u8) -> u32 {
        let Some(c) = search(aopt(path), astr(file), aopt(ext)) else { return 0 };
        let l = c.len() as u32;
        if out.is_null() || n <= l { return l + 1; }
        put_a(&c, out, n as usize);
        if !part.is_null() { *part = out.add(basename_at(&c)); }
        l
    }
    fn SearchPathW(path: W, file: W, ext: W, n: u32, out: *mut u16, part: *mut *mut u16) -> u32 {
        let (ap, ae) = (wopt(path, 1024), wopt(ext, 32));
        let Some(c) = search(ap.as_deref(), &w2a(file, 300), ae.as_deref()) else { return 0 };
        let r = c.len() as u32;
        if out.is_null() || n <= r { return r + 1; }
        a2w(&c, out, n as usize);
        if !part.is_null() { *part = out.add(basename_at(&c)); }
        r
    }

    // ------------------------------------------------------------ files and volumes
    fn GetFileInformationByHandleEx(h: H, cls: i32, buf: *mut u8, n: u32) -> i32 {
        let v = w32_file_node(h);
        if v.is_null() { set_err(6); return 0; }
        if cls == 0 && n >= 40 {                           // FILE_BASIC_INFO: 4 times, attributes
            core::ptr::write_bytes(buf, 0, 40); *(buf.add(32) as *mut u32) = if (*v).dir != 0 { 0x10 } else { 0x80 }; return 1;
        }
        if cls == 1 && n >= 24 {                           // FILE_STANDARD_INFO
            core::ptr::write_bytes(buf, 0, 24);
            *(buf as *mut i64) = (*v).size as i64; *(buf.add(8) as *mut i64) = (*v).size as i64;
            *(buf.add(16) as *mut u32) = 1; *buf.add(21) = ((*v).dir != 0) as u8;
            return 1;
        }
        if cls == 2 && n >= 4 {                            // FILE_NAME_INFO: the path without the drive
            let mut p = [0u8; 300];
            vfs_path(v, p.as_mut_ptr() as *mut c_char, 300);
            let path = cstr(p.as_ptr() as *const c_char);
            let s = if path.len() >= 2 && path[0].is_ascii_alphabetic() && path[1] == b':' { &path[2..] } else { path };
            *(buf as *mut u32) = s.len() as u32 * 2;
            if (n as usize) < 4 + s.len() * 2 { set_err(234); return 0; }
            let w = buf.add(4) as *mut u16;
            for (i, &c) in s.iter().enumerate() { *w.add(i) = c as u16; }
            return 1;
        }
        set_err(87); 0
    }
    fn GetFinalPathNameByHandleW(h: H, b: *mut u16, n: u32, fl: u32) -> u32 {
        let v = w32_file_node(h);
        if v.is_null() { set_err(6); return 0; }
        let mut p = [0u8; 300]; vfs_path(v, p.as_mut_ptr() as *mut c_char, 300);
        let mut full = b"\\\\?\\".to_vec(); full.extend_from_slice(cstr(p.as_ptr() as *const c_char)); full.truncate(309);
        let l = full.len() as u32;
        if b.is_null() || n <= l { return l + 1; }
        a2w(&full, b, n as usize); l
    }
    fn GetFinalPathNameByHandleA(h: H, b: *mut u8, n: u32, fl: u32) -> u32 {
        let v = w32_file_node(h);
        if v.is_null() { set_err(6); return 0; }
        let mut p = [0u8; 300]; vfs_path(v, p.as_mut_ptr() as *mut c_char, 300);
        let mut full = b"\\\\?\\".to_vec(); full.extend_from_slice(cstr(p.as_ptr() as *const c_char)); full.truncate(309);
        let l = full.len() as u32;
        if b.is_null() || n <= l { return l + 1; }
        put_a(&full, b, n as usize); l
    }
    fn GetVolumeInformationA(root: A, name: *mut u8, nn: u32, serial: P, maxlen: P, flags: P, fs: *mut u8, fsn: u32) -> i32 {
        let d = if !root.is_null() && (*root).is_ascii_alphabetic() { (*root).to_ascii_uppercase() } else { b'C' };
        let kind = vfs_drive_kind(d as c_char);
        if kind == 0 { set_err(3); return 0; }
        if !name.is_null() && nn != 0 { put_a(if d == b'C' { b"WRLD" } else { b"" }, name, nn as usize); }
        if !serial.is_null() { *serial = 0x5752_4C44 + d as u32; }
        if !maxlen.is_null() { *maxlen = 255; }
        if !flags.is_null() { *flags = 0x3 | 0x4; }        // case-preserved and Unicode names
        if !fs.is_null() && fsn != 0 { put_a(if kind == 2 { b"CDFS" } else { b"FAT32" }, fs, fsn as usize); }
        1
    }
    fn GetVolumePathNameW(f: W, out: *mut u16, n: u32) -> i32 {
        if n < 4 { set_err(206); return 0; }
        *out = if !f.is_null() && *f != 0 && *f.add(1) == b':' as u16 { *f } else { b'C' as u16 };
        *out.add(1) = b':' as u16; *out.add(2) = b'\\' as u16; *out.add(3) = 0;
        1
    }
    fn GetVolumePathNameA(f: A, out: *mut u8, n: u32) -> i32 {
        if n < 4 { return 0; }
        let c = if !f.is_null() && *f != 0 && *f.add(1) == b':' { *f } else { b'C' };
        put_a(&[c, b':', b'\\'], out, n as usize); 1
    }
    fn GetVolumeNameForVolumeMountPointW(m: W, out: *mut u16, n: u32) -> i32 { set_err(1); 0 }
    fn GetShortPathNameA(l: A, s: *mut u8, n: u32) -> u32 { let p = astr(l); let k = p.len() as u32; if s.is_null() || n <= k { return k + 1; } put_a(p, s, n as usize); k }
    fn CreateSymbolicLinkW(a: W, b: W, f: u32) -> i32 { set_err(1314); 0 }
    fn CreateSymbolicLinkA(a: A, b: A, f: u32) -> i32 { set_err(1314); 0 }
    fn FindFirstChangeNotificationW(p: W, sub: i32, f: u32) -> H { create_event(1) }
    fn FindFirstChangeNotificationA(p: A, sub: i32, f: u32) -> H { create_event(1) }
    fn FindNextChangeNotification(h: H) -> i32 { 1 }
    fn FindCloseChangeNotification(h: H) -> i32 {
        let f = call0(c"kernel32.dll", c"CloseHandle");
        if f.is_null() { return 0; }
        let f: winfn!(fn(H) -> i32) = core::mem::transmute(f); f(h)
    }
    fn ReadDirectoryChangesW(h: H, b: *mut c_void, n: u32, s: i32, f: u32, r: P, ov: *mut c_void, cr: *mut c_void) -> i32 { set_err(50); 0 }

    // ------------------------------------------------------------ console input records
    fn ReadConsoleInputW(h: H, r: *mut InRec, n: u32, got: P) -> i32 {
        if n == 0 { if !got.is_null() { *got = 0; } return 1; }
        let pk = PENDING_KEY.get();
        let c = if *pk >= 0 { *pk } else { w32_console_key() };
        *pk = -1;
        let (vk, ch) = u::console_vk(c);
        core::ptr::write_unaligned(r, InRec { ty: 1, pad: 0, down: 1, repeat: 1, vk, scan: 0, ch, ctrl: 0 });
        if !got.is_null() { *got = 1; }
        1
    }
    fn ReadConsoleInputA(h: H, r: *mut InRec, n: u32, got: P) -> i32 { ReadConsoleInputW(h, r, n, got) }
    fn PeekConsoleInputW(h: H, r: *mut InRec, n: u32, got: P) -> i32 { if !got.is_null() { *got = 0; } 1 }
    fn PeekConsoleInputA(h: H, r: *mut InRec, n: u32, got: P) -> i32 { if !got.is_null() { *got = 0; } 1 }
    fn WriteConsoleInputW(h: H, r: *const InRec, n: u32, w: P) -> i32 {
        if n != 0 { let rec = core::ptr::read_unaligned(r); if rec.ty == 1 && rec.down != 0 { *PENDING_KEY.get() = rec.ch as i32; } }
        if !w.is_null() { *w = n; }
        1
    }
    fn AttachConsole(pid: u32) -> i32 { set_err(6); 0 }
    fn GenerateConsoleCtrlEvent(e: u32, g: u32) -> i32 { 1 }

    // ------------------------------------------------------------ serial ports (none)
    fn BuildCommDCBW(s: W, dcb: *mut c_void) -> i32 { 1 }
    fn BuildCommDCBA(s: A, dcb: *mut c_void) -> i32 { 1 }
    fn ClearCommError(h: H, e: P, st: *mut u8) -> i32 { if !e.is_null() { *e = 0; } if !st.is_null() { core::ptr::write_bytes(st, 0, 12); } 1 }
    fn EscapeCommFunction(h: H, f: u32) -> i32 { 1 }
    fn GetCommModemStatus(h: H, s: P) -> i32 { if !s.is_null() { *s = 0; } 1 }
    fn PurgeComm(h: H, f: u32) -> i32 { 1 }
    fn SetupComm(h: H, i: u32, o: u32) -> i32 { 1 }
    fn GetCommTimeouts(h: H, t: *mut u8) -> i32 { core::ptr::write_bytes(t, 0, 20); 1 }
    fn SetCommMask(h: H, m: u32) -> i32 { 1 }
    fn WaitCommEvent(h: H, m: P, ov: *mut c_void) -> i32 { set_err(1); 0 }

    // ------------------------------------------------------------ odds and ends
    fn AddVectoredExceptionHandler(first: u32, f: *mut c_void) -> *mut c_void { DUMMY.get() as *mut i32 as *mut c_void }
    fn RemoveVectoredExceptionHandler(h: *mut c_void) -> u32 { 1 }
    fn AddVectoredContinueHandler(first: u32, f: *mut c_void) -> *mut c_void { DUMMY.get() as *mut i32 as *mut c_void }
    fn RemoveVectoredContinueHandler(h: *mut c_void) -> u32 { 1 }
    fn RtlAddFunctionTable(t: *mut c_void, n: u32, base: usize) -> i32 { 1 }
    fn RtlDeleteFunctionTable(t: *mut c_void) -> i32 { 1 }
    fn ContinueDebugEvent(p: u32, t: u32, s: u32) -> i32 { 0 }
    fn WaitForDebugEvent(e: *mut c_void, ms: u32) -> i32 { 0 }
    fn DebugActiveProcess(p: u32) -> i32 { set_err(5); 0 }
    fn CheckRemoteDebuggerPresent(h: H, d: *mut i32) -> i32 { *d = 0; 1 }
    fn ConvertDefaultLocale(l: u32) -> u32 { if l != 0 { l } else { 0x409 } }
    fn EnumCalendarInfoW(f: *mut c_void, l: u32, c: u32, t: u32) -> i32 { 0 }
    fn EnumResourceLanguagesA(m: usize, t: A, n: A, f: *mut c_void, p: isize) -> i32 { set_err(1814); 0 }
    fn EnumResourceNamesW(m: usize, t: W, f: *mut c_void, p: isize) -> i32 { set_err(1813); 0 }
    fn EnumResourceNamesA(m: usize, t: A, f: *mut c_void, p: isize) -> i32 { set_err(1813); 0 }
    fn EnumResourceTypesW(m: usize, f: *mut c_void, p: isize) -> i32 { 0 }
    fn GetActiveProcessorCount(g: u16) -> u32 { 1 }
    fn GetMaximumProcessorCount(g: u16) -> u32 { 1 }
    fn GetActiveProcessorGroupCount() -> u16 { 1 }
    fn GetLogicalProcessorInformation(b: *mut u8, n: P) -> i32 {
        if !n.is_null() {
            if *n < 24 { *n = 24; set_err(122); return 0; }
            core::ptr::write_bytes(b, 0, 24); *(b as *mut usize) = 1; *n = 24;
        }
        1
    }
    fn GetCPInfoExW(cp: u32, f: u32, c: *mut CpInfoExW) -> i32 {
        core::ptr::write_bytes(c as *mut u8, 0, core::mem::size_of::<CpInfoExW>());
        (*c).max = if cp == 65001 { 4 } else { 1 };
        (*c).def[0] = b'?'; (*c).udef = b'?' as u16;
        (*c).cp = if cp == 0 || cp == 1 { 1252 } else { cp };
        let what = match (*c).cp { 65001 => "UTF-8", 437 => "OEM - United States", _ => "ANSI - Latin I" };
        let n = alloc::format!("{} ({})", (*c).cp, what);
        a2w(n.as_bytes(), (*c).name.as_mut_ptr(), 260);
        1
    }
    fn GetStringTypeExA(lc: u32, t: u32, s: A, n: i32, o: *mut u16) -> i32 {
        let n = if n < 0 { astr(s).len() + 1 } else { n as usize };
        for i in 0..n { *o.add(i) = if t == 1 { u::ctype1(*s.add(i)) } else { 0 }; }
        1
    }
    fn GetStringTypeA(lc: u32, t: u32, s: A, n: i32, o: *mut u16) -> i32 { GetStringTypeExA(lc, t, s, n, o) }
    fn GetSystemDefaultUILanguage() -> u16 { 0x409 }
    fn GetUserDefaultUILanguage() -> u16 { 0x409 }
    fn SetThreadLocale(l: u32) -> i32 { 1 }
    fn SetThreadUILanguage(l: u16) -> u16 { 0x409 }
    fn GetThreadUILanguage() -> u16 { 0x409 }
    fn GetSystemPowerStatus(s: *mut Sps) -> i32 { *s = Sps { ac: 1, flag: 128, pct: 255, res: 0, life: 0xFFFF_FFFF, full: 0xFFFF_FFFF }; 1 }
    fn GetSystemTimeAdjustment(adj: P, inc: P, dis: *mut i32) -> i32 { *adj = 156_250; *inc = 156_250; *dis = 1; 1 }
    fn GetThreadContext(h: H, c: *mut c_void) -> i32 { set_err(5); 0 }
    fn SetThreadContext(h: H, c: *const c_void) -> i32 { set_err(5); 0 }
    fn SetThreadExecutionState(f: u32) -> u32 { 0x8000_0000 }
    fn SetHandleCount(n: u32) -> u32 { n }
    fn SetNamedPipeHandleState(h: H, m: P, c: P, t: P) -> i32 { 1 }
    fn GetNamedPipeInfo(h: H, f: P, o: P, i: P, m: P) -> i32 { set_err(1); 0 }
    fn InitializeProcThreadAttributeList(l: *mut u8, n: u32, f: u32, sz: *mut usize) -> i32 {
        if l.is_null() { *sz = 64 + n as usize * 24; set_err(122); return 0; }
        core::ptr::write_bytes(l, 0, *sz); 1
    }
    fn UpdateProcThreadAttribute(l: *mut c_void, f: u32, a: usize, v: *mut c_void, n: usize, prev: *mut c_void, rs: *mut usize) -> i32 { 1 }
    fn DeleteProcThreadAttributeList(l: *mut c_void) -> () {}
    fn ReadProcessMemory(h: H, a: *const u8, b: *mut u8, n: usize, got: *mut usize) -> i32 {
        if !is_self(h) { set_err(5); return 0; }
        core::ptr::copy(a, b, n); if !got.is_null() { *got = n; } 1
    }
    fn WriteProcessMemory(h: H, a: *mut u8, b: *const u8, n: usize, done: *mut usize) -> i32 {
        if !is_self(h) { set_err(5); return 0; }
        core::ptr::copy(b, a, n); if !done.is_null() { *done = n; } 1
    }
    fn VirtualProtectEx(h: H, a: *mut c_void, n: usize, np: u32, op: P) -> i32 { if !is_self(h) { set_err(5); return 0; } if !op.is_null() { *op = 0x40; } 1 }
    fn VirtualQueryEx(h: H, a: *const c_void, m: *mut c_void, n: usize) -> usize {
        if !is_self(h) { set_err(5); return 0; }
        let f = call0(c"kernel32.dll", c"VirtualQuery"); if f.is_null() { return 0; }
        let f: winfn!(fn(*const c_void, *mut c_void, usize) -> usize) = core::mem::transmute(f); f(a, m, n)
    }
    fn VirtualAllocEx(h: H, a: *mut c_void, n: usize, t: u32, p: u32) -> *mut c_void {
        if !is_self(h) { set_err(5); return core::ptr::null_mut(); }
        let f = call0(c"kernel32.dll", c"VirtualAlloc"); if f.is_null() { return core::ptr::null_mut(); }
        let f: winfn!(fn(*mut c_void, usize, u32, u32) -> *mut c_void) = core::mem::transmute(f); f(a, n, t, p)
    }
    fn VirtualFreeEx(h: H, a: *mut c_void, n: usize, t: u32) -> i32 {
        if !is_self(h) { set_err(5); return 0; }
        let f = call0(c"kernel32.dll", c"VirtualFree"); if f.is_null() { return 0; }
        let f: winfn!(fn(*mut c_void, usize, u32) -> i32) = core::mem::transmute(f); f(a, n, t)
    }
    fn VirtualLock(a: *mut c_void, n: usize) -> i32 { 1 }
    fn VirtualUnlock(a: *mut c_void, n: usize) -> i32 { 1 }
    fn SetProcessWorkingSetSize(h: H, a: usize, b: usize) -> i32 { 1 }
    fn GetProcessWorkingSetSize(h: H, a: *mut usize, b: *mut usize) -> i32 { *a = 204_800; *b = 1_413_120; 1 }
    fn GetProcessHandleCount(h: H, n: P) -> i32 { *n = 16; 1 }
    fn GetProcessVersion(pid: u32) -> u32 { 0x0006_0001 }
    fn GetProcessIoCounters(h: H, c: *mut u8) -> i32 { core::ptr::write_bytes(c, 0, 48); 1 }
    fn QueryProcessCycleTime(h: H, c: *mut u64) -> i32 { *c = timer_ticks() as u64 * 2_000_000; 1 }
    fn QueryThreadCycleTime(h: H, c: *mut u64) -> i32 { *c = timer_ticks() as u64 * 2_000_000; 1 }
    fn QueryUnbiasedInterruptTime(t: *mut u64) -> i32 { *t = timer_ticks() as u64 * 10_000; 1 }
    fn GetNumaHighestNodeNumber(n: P) -> i32 { *n = 0; 1 }
    fn Wow64DisableWow64FsRedirection(o: *mut *mut c_void) -> i32 { if !o.is_null() { *o = core::ptr::null_mut(); } 1 }
    fn Wow64RevertWow64FsRedirection(o: *mut c_void) -> i32 { 1 }
    fn SetErrorMode(m: u32) -> u32 { core::mem::replace(ERROR_MODE.get(), m) }
    fn GetErrorMode() -> u32 { 0 }
    fn SetThreadErrorMode(m: u32, o: P) -> i32 { if !o.is_null() { *o = 0; } 1 }
    fn GetThreadErrorMode() -> u32 { 0 }
    fn RaiseFailFastException(r: *mut c_void, c: *mut c_void, f: u32) -> () { w32_exit_process(0xC000_0602u32 as c_int) }
    fn FatalAppExitA(a: u32, m: A) -> () { w32_fatal_app_exit(m as *const c_char) }
    fn FatalAppExitW(a: u32, m: W) -> () { let b = cz(&w2a(m, 300)); w32_fatal_app_exit(b.as_ptr() as *const c_char) }
    fn lstrlenW(s: W) -> i32 { wstr(s, usize::MAX).len() as i32 }
    fn lstrcpyW(d: *mut u16, s: W) -> *mut u16 { if s.is_null() { *d = 0; return d; } let n = wstr(s, usize::MAX).len(); core::ptr::copy(s, d, n + 1); d }
    fn lstrcpynW(d: *mut u16, s: W, n: i32) -> *mut u16 {
        if n <= 0 { return d; }
        let k = if s.is_null() { 0 } else { wstr(s, n as usize - 1).len() };
        core::ptr::copy(s, d, k); *d.add(k) = 0; d
    }
    fn lstrcatW(d: *mut u16, s: W) -> *mut u16 { lstrcpyW(d.add(wstr(d, usize::MAX).len()), s); d }
    fn lstrcmpW(a: W, b: W) -> i32 { u::wcmp(wstr(a, usize::MAX), wstr(b, usize::MAX), false) }
    fn lstrcmpiW(a: W, b: W) -> i32 { u::wcmp(wstr(a, usize::MAX), wstr(b, usize::MAX), true) }
    fn IsBadReadPtr(p: *const c_void, n: usize) -> i32 { p.is_null() as i32 }
    fn IsBadWritePtr(p: *mut c_void, n: usize) -> i32 { p.is_null() as i32 }
    fn IsBadCodePtr(p: *mut c_void) -> i32 { p.is_null() as i32 }
    fn IsBadStringPtrA(p: A, n: usize) -> i32 { p.is_null() as i32 }
    fn IsBadStringPtrW(p: W, n: usize) -> i32 { p.is_null() as i32 }
    fn WTSGetActiveConsoleSessionId() -> u32 { 1 }
    fn ProcessIdToSessionId(pid: u32, s: P) -> i32 { *s = 1; 1 }
    fn CreateWaitableTimerW(sa: *mut c_void, m: i32, n: W) -> H { create_event(m) }
    fn CreateWaitableTimerA(sa: *mut c_void, m: i32, n: A) -> H { create_event(m) }
    fn SetWaitableTimer(h: H, due: *const i64, per: i32, f: *mut c_void, arg: *mut c_void, r: i32) -> i32 {
        let f = call0(c"kernel32.dll", c"SetEvent"); if f.is_null() { return 0; }
        let f: winfn!(fn(H) -> i32) = core::mem::transmute(f); f(h)
    }
    fn CancelWaitableTimer(h: H) -> i32 { 1 }
    fn GetComputerNameExA(t: i32, b: *mut u8, n: P) -> i32 {
        let c = cname(&computer_name); let l = c.len() as u32;
        if *n <= l { *n = l + 1; set_err(234); return 0; }
        put_a(c, b, l as usize + 1); *n = l; 1
    }
    fn SetComputerNameA(n: A) -> i32 { 1 }
    fn GetLongPathNameA2(s: A, l: *mut u8, n: u32) -> u32 { let p = astr(s); let k = p.len() as u32; if l.is_null() || n <= k { return k + 1; } put_a(p, l, n as usize); k }
    fn GetSystemWow64DirectoryA(b: *mut u8, n: u32) -> u32 { set_err(120); 0 }
    fn GetSystemWow64DirectoryW(b: *mut u16, n: u32) -> u32 { set_err(120); 0 }
    fn GetUserNameExW(f: i32, b: *mut u16, n: P) -> i32 {
        let u_ = cname(&user_name); let l = u_.len() as u32;
        if *n <= l { *n = l + 1; set_err(234); return 0; }
        a2w(u_, b, *n as usize); *n = l; 1
    }
    fn GetUserNameExA(f: i32, b: *mut u8, n: P) -> i32 {
        let u_ = cname(&user_name); let l = u_.len() as u32;
        if *n <= l { *n = l + 1; set_err(234); return 0; }
        put_a(u_, b, l as usize + 1); *n = l; 1
    }
}

api_table! { k32more_api;
    "kernel32": GetEnvironmentVariableA, GetEnvironmentVariableW, SetEnvironmentVariableA, SetEnvironmentVariableW,
        GetEnvironmentStringsA = "GetEnvironmentStrings", GetEnvironmentStringsA, GetEnvironmentStringsW, FreeEnvironmentStringsA,
        FreeEnvironmentStringsW, SetEnvironmentStringsW, ExpandEnvironmentStringsA, ExpandEnvironmentStringsW, GetVersion, GetVersionExA,
        GetVersionExW, RtlGetVersion, VerSetConditionMask, VerifyVersionInfoW, VerifyVersionInfoA, GetProductInfo,
        GetPrivateProfileStringA, GetPrivateProfileStringW, GetPrivateProfileIntA, GetPrivateProfileIntW, WritePrivateProfileStringA,
        WritePrivateProfileStringW, GetPrivateProfileSectionA, GetPrivateProfileSectionW, WritePrivateProfileSectionA,
        WritePrivateProfileSectionW, GetPrivateProfileSectionNamesA, GetPrivateProfileSectionNamesW, GetProfileStringA, GetProfileStringW,
        GetProfileIntA, GetProfileIntW, WriteProfileStringA, WriteProfileStringW, GetPrivateProfileStructA, WritePrivateProfileStructA,
        GlobalAddAtomA, GlobalAddAtomW, GlobalFindAtomA, GlobalFindAtomW, GlobalDeleteAtom, GlobalGetAtomNameA, GlobalGetAtomNameW,
        GlobalAddAtomA = "AddAtomA", GlobalAddAtomW = "AddAtomW", GlobalFindAtomA = "FindAtomA", GlobalFindAtomW = "FindAtomW",
        GlobalDeleteAtom = "DeleteAtom", GlobalGetAtomNameA = "GetAtomNameA", GlobalGetAtomNameW = "GetAtomNameW",
        GlobalSize, GlobalReAlloc, GlobalFlags, GlobalHandle, LocalHandle, LocalFlags, LocalLock, LocalUnlock, GlobalCompact, GlobalFix,
        GlobalUnfix, GlobalSize = "LocalSize", GlobalReAlloc = "LocalReAlloc",
        CreateToolhelp32Snapshot, Process32First, Process32Next, Process32FirstW, Process32NextW, Module32First, Module32Next,
        Module32FirstW, Module32NextW, Thread32First, Thread32Next,
        SearchPathA, SearchPathW, GetFileInformationByHandleEx, GetFinalPathNameByHandleW, GetFinalPathNameByHandleA, GetVolumeInformationA,
        GetVolumePathNameW, GetVolumePathNameA, GetVolumeNameForVolumeMountPointW, GetShortPathNameA, CreateSymbolicLinkW, CreateSymbolicLinkA,
        FindFirstChangeNotificationW, FindFirstChangeNotificationA, FindNextChangeNotification, FindCloseChangeNotification, ReadDirectoryChangesW,
        ReadConsoleInputW, ReadConsoleInputA, PeekConsoleInputW, PeekConsoleInputA, WriteConsoleInputW, AttachConsole, GenerateConsoleCtrlEvent,
        BuildCommDCBW, BuildCommDCBA, ClearCommError, EscapeCommFunction, GetCommModemStatus, PurgeComm, SetupComm, GetCommTimeouts,
        SetCommMask, WaitCommEvent,
        AddVectoredExceptionHandler, RemoveVectoredExceptionHandler, AddVectoredContinueHandler, RemoveVectoredContinueHandler, RtlAddFunctionTable,
        RtlDeleteFunctionTable, ContinueDebugEvent, WaitForDebugEvent, DebugActiveProcess, CheckRemoteDebuggerPresent, ConvertDefaultLocale,
        EnumCalendarInfoW, EnumResourceLanguagesA, EnumResourceNamesW, EnumResourceNamesA, EnumResourceTypesW, GetActiveProcessorCount,
        GetMaximumProcessorCount, GetActiveProcessorGroupCount, GetLogicalProcessorInformation, GetCPInfoExW, GetStringTypeExA, GetStringTypeA,
        GetSystemDefaultUILanguage, GetUserDefaultUILanguage, SetThreadLocale, SetThreadUILanguage, GetThreadUILanguage, GetSystemPowerStatus,
        GetSystemTimeAdjustment, GetThreadContext, SetThreadContext, SetThreadExecutionState, SetHandleCount, SetNamedPipeHandleState,
        GetNamedPipeInfo, InitializeProcThreadAttributeList, UpdateProcThreadAttribute, DeleteProcThreadAttributeList, ReadProcessMemory,
        WriteProcessMemory, VirtualProtectEx, VirtualQueryEx, VirtualAllocEx, VirtualFreeEx, VirtualLock, VirtualUnlock,
        SetProcessWorkingSetSize, GetProcessWorkingSetSize, GetProcessHandleCount, GetProcessVersion, GetProcessIoCounters, QueryProcessCycleTime,
        QueryThreadCycleTime, QueryUnbiasedInterruptTime, GetNumaHighestNodeNumber, Wow64DisableWow64FsRedirection, Wow64RevertWow64FsRedirection,
        SetErrorMode, GetErrorMode, SetThreadErrorMode, GetThreadErrorMode, RaiseFailFastException, FatalAppExitA, FatalAppExitW,
        lstrlenW, lstrcpyW, lstrcpynW, lstrcatW, lstrcmpW, lstrcmpiW, IsBadReadPtr, IsBadWritePtr, IsBadCodePtr, IsBadStringPtrA,
        IsBadStringPtrW, WTSGetActiveConsoleSessionId, ProcessIdToSessionId, CreateWaitableTimerW, CreateWaitableTimerA, SetWaitableTimer,
        CancelWaitableTimer, GetComputerNameExA, SetComputerNameA, GetLongPathNameA2 = "GetLongPathNameA", GetSystemWow64DirectoryA,
        GetSystemWow64DirectoryW;
    "advapi32": GetUserNameExW, GetUserNameExA;
    "secur32": GetUserNameExW, GetUserNameExA;
}
