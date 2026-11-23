//! shlwapi (path and string helpers), more of shell32 (special folders, item ID lists,
//! ShellExecuteEx, CommandLineToArgvW, SHFileOperation, SHGetFileInfo), version.dll (version
//! resources read from the file itself) and winmm (time, PlaySound, waveOut on the kernel's
//! sound streams). Was `src/win32/shell.c`; the C-variadic wnsprintf functions stay in
//! `src/win32/shell_va.c`. The parsing (paths, URLs, command lines, version resources) is
//! in `crate::w32util`.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};

use super::w32::{a2w, alloc, astr, free, lookup, set_err, wstr};
use super::Vnode;
use crate::w32util::{self as u, is_sep, wlow, MAXP};

#[allow(clashing_extern_declarations)]
extern "C" {
    fn w32_cur() -> *mut c_void;
    fn w32_realloc(p: *mut c_void, ptr: *mut c_void, n: u32) -> *mut c_void;
    fn w32_msize(p: *mut c_void) -> u32;
    fn w32_proc_cwd() -> *const c_char;
    fn w32_proc_exe() -> *const c_char;
    fn w32_mod_path_by_name(n: *const c_char) -> *const c_char;
    fn w32_taskbar_rect(r: *mut i32);
    fn w32_find_resource_in(p: *mut c_void, hmod: usize, ty: usize, name: usize, wide: super::CBool, lang: c_int) -> usize;
    fn w32_resource_data(p: *mut c_void, hrsrc: usize, size: *mut u32) -> *mut u8;
    fn vfs_resolve(path: *const c_char, cwd: *const c_char) -> *mut Vnode;
    fn vfs_create(path: *const c_char, cwd: *const c_char, dir: super::CBool) -> *mut Vnode;
    fn vfs_delete(n: *mut Vnode) -> super::CBool;
    fn vfs_path(n: *mut Vnode, buf: *mut c_char, cap: c_int);
    fn path_join(out: *mut c_char, cap: c_int, a: *const c_char, b: *const c_char);
    fn path_basename(p: *const c_char) -> *const c_char;
    fn timer_ticks() -> u32;
    fn task_sleep(ms: u32);
    fn audio_present() -> super::CBool;
    fn audio_play(s: *const c_void) -> c_int;
    fn audio_play_file(path: *const c_char, err: *mut c_char, cap: c_int) -> super::CBool;
    fn audio_wait(ms: u32);
    fn audio_stop_all();
    fn audio_rate() -> c_int;
    fn audio_stream_open() -> c_int;
    fn audio_stream_write(id: c_int, pcm: *const i16, frames: c_int) -> c_int;
    fn audio_stream_room(id: c_int) -> c_int;
    fn audio_stream_queued(id: c_int) -> c_int;
    fn audio_stream_close(id: c_int);
    fn sound_load(data: *const u8, len: u32, ext: *const c_char, err: *mut c_char, cap: c_int) -> *mut c_void;
    fn speaker_beep(freq: u32, ms: u32);
}

type H = usize;
type W = *const u16;
type A = *const u8;
type P = *mut u32;

const S_OK: i32 = 0;
const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;
const E_NOTIMPL: i32 = 0x8000_4001u32 as i32;
const E_POINTER: i32 = 0x8000_4003u32 as i32;
const USER_HOME: &str = "C:\\Documents and Settings\\User";
const OS_NAME: &str = "THE WRLD OS";

// ---------------------------------------------------------------- helpers

unsafe fn w2a(w: W, cap: usize) -> Vec<u8> { wstr(w, cap - 1).iter().map(|&c| if c < 256 { c as u8 } else { b'?' }).collect() }
fn cz(s: &[u8]) -> Vec<u8> { let mut v = s.to_vec(); v.push(0); v }
fn wz(s: &[u16]) -> Vec<u16> { let mut v = s.to_vec(); v.push(0); v }
unsafe fn cstr<'a>(p: *const c_char) -> &'a [u8] { astr(p as *const u8) }
/// strlcpy into `n` bytes; returns the bytes copied.
unsafe fn put_a(src: &[u8], dst: *mut u8, n: usize) -> usize {
    if dst.is_null() || n == 0 { return 0; }
    let k = src.len().min(n - 1);
    core::ptr::copy_nonoverlapping(src.as_ptr(), dst, k); *dst.add(k) = 0;
    k
}
/// A wide string into a buffer of `n` characters (cut, with its NUL).
unsafe fn put_w(src: &[u16], dst: *mut u16, n: usize) -> usize {
    if dst.is_null() || n == 0 { return 0; }
    let k = src.len().min(n - 1);
    core::ptr::copy(src.as_ptr(), dst, k); *dst.add(k) = 0;
    k
}
unsafe fn ws<'a>(p: W) -> &'a [u16] { wstr(p, usize::MAX) }
fn widen(a: &[u8]) -> Vec<u16> { a.iter().map(|&c| c as u16).collect() }
unsafe fn k32(name: &core::ffi::CStr) -> *mut c_void { lookup(c"kernel32.dll", name) }
unsafe fn file_attrs(p: A) -> u32 {
    let f = k32(c"GetFileAttributesA");
    if f.is_null() { return 0xFFFF_FFFF; }
    let f: winfn!(fn(A) -> u32) = core::mem::transmute(f); f(p)
}
unsafe fn local_alloc(flags: u32, n: usize) -> *mut u8 {
    let f = k32(c"LocalAlloc");
    if f.is_null() { return core::ptr::null_mut(); }
    let f: winfn!(fn(u32, usize) -> *mut u8) = core::mem::transmute(f); f(flags, n)
}
unsafe fn set_event(h: H) -> i32 {
    let f = k32(c"SetEvent");
    if f.is_null() { return 0; }
    let f: winfn!(fn(H) -> i32) = core::mem::transmute(f); f(h)
}
unsafe fn create_thread(entry: *const c_void, arg: *mut c_void) -> H {
    let f = k32(c"CreateThread");
    if f.is_null() { return 0; }
    let f: winfn!(fn(*mut c_void, usize, *const c_void, *mut c_void, u32, *mut u32) -> H) = core::mem::transmute(f);
    f(core::ptr::null_mut(), 0, entry, arg, 0, core::ptr::null_mut())
}
unsafe fn shell_execute_a(h: H, verb: A, file: A, params: A, dir: A, show: i32) -> usize {
    let f = lookup(c"shell32.dll", c"ShellExecuteA");
    if f.is_null() { return 2; }
    let f: winfn!(fn(H, A, A, A, A, i32) -> usize) = core::mem::transmute(f);
    f(h, verb, file, params, dir, show)
}
fn opt_ptr(v: &Option<Vec<u8>>) -> A { v.as_ref().map_or(core::ptr::null(), |v| v.as_ptr()) }
unsafe fn node(path: &[u8]) -> *mut Vnode { vfs_resolve(cz(path).as_ptr() as *const c_char, core::ptr::null()) }
unsafe fn basename_of(p: &[u8]) -> Vec<u8> { let c = cz(p); cstr(path_basename(c.as_ptr() as *const c_char)).to_vec() }
unsafe fn joined(a: &[u8], b: &[u8], cap: usize) -> Vec<u8> {
    let mut out = alloc::vec![0u8; cap];
    path_join(out.as_mut_ptr() as *mut c_char, cap as c_int, cz(a).as_ptr() as *const c_char, cz(b).as_ptr() as *const c_char);
    cstr(out.as_ptr() as *const c_char).to_vec()
}

// ---------------------------------------------------------------- special folders and item ID lists

unsafe fn mkdirs(path: &[u8]) {
    let b = &path[..path.len().min(299)];
    for i in 3..=b.len() {
        if i == b.len() || b[i] == b'\\' {
            let p = cz(&b[..i]);
            if vfs_resolve(p.as_ptr() as *const c_char, core::ptr::null()).is_null() { vfs_create(p.as_ptr() as *const c_char, core::ptr::null(), 1); }
        }
    }
}
unsafe fn folder_path(csidl: i32) -> Result<Vec<u8>, i32> {
    let p = u::csidl_path(csidl, USER_HOME).ok_or(E_INVALIDARG)?.into_bytes();
    if csidl & 0x8000 != 0 { mkdirs(&p); }
    Ok(p[..p.len().min(MAXP - 1)].to_vec())
}
/// An item ID list: one item holding the path, then a zero WORD.
unsafe fn make_pidl(path: &[u8]) -> *mut u8 {
    let l = path.len() + 1;
    let p = alloc(l + 4);
    if p.is_null() { return p; }
    *(p as *mut u16) = (l + 2) as u16;
    core::ptr::copy_nonoverlapping(path.as_ptr(), p.add(2), path.len()); *p.add(2 + path.len()) = 0;
    *(p.add(2 + l) as *mut u16) = 0;
    p
}
unsafe fn pidl_path<'a>(pidl: *const u8) -> &'a [u8] { if !pidl.is_null() && *(pidl as *const u16) > 2 { astr(pidl.add(2)) } else { b"C:\\" } }

// IMalloc on the program heap
#[repr(C)] struct IMalloc { vt: *const [*const c_void; 9] }
unsafe impl Sync for IMalloc {}
struct Vt([*const c_void; 9]);
unsafe impl Sync for Vt {}
static IMALLOC_VT: Vt = Vt([im_qi as *const c_void, im_ref as *const c_void, im_ref as *const c_void, im_alloc as *const c_void, im_realloc as *const c_void,
                            im_free as *const c_void, im_size as *const c_void, im_did as *const c_void, im_min as *const c_void]);
static IMALLOC: IMalloc = IMalloc { vt: &IMALLOC_VT.0 };

// ---------------------------------------------------------------- the folder picker

#[repr(C)]
struct Ofn { size: u32, owner: H, inst: H, filter: A, cust: *mut u8, ncust: u32, findex: u32, file: *mut u8, nfile: u32, ftitle: *mut u8, nftitle: u32,
             initdir: A, title: A, flags: u32, fo: u16, fe: u16, defext: A, cd: isize, hook: *mut c_void, tpl: A, r1: *mut c_void, r2: u32, fx: u32 }
/// The system's file dialog, opened on the folder (its path is the answer).
unsafe fn browse(owner: H, title: &[u8]) -> *mut u8 {
    let f = lookup(c"comdlg32.dll", c"GetSaveFileNameA");
    let mut file = [0u8; MAXP];
    put_a(b"(this folder)", file.as_mut_ptr(), MAXP);
    let t = cz(if title.is_empty() { b"Browse For Folder" } else { title });
    let docs = cz(alloc::format!("{USER_HOME}\\My Documents").as_bytes());
    let mut o: Ofn = core::mem::zeroed();
    o.size = core::mem::size_of::<Ofn>() as u32; o.owner = owner; o.file = file.as_mut_ptr(); o.nfile = MAXP as u32; o.title = t.as_ptr(); o.initdir = docs.as_ptr();
    if f.is_null() { return core::ptr::null_mut(); }
    let f: winfn!(fn(*mut Ofn) -> i32) = core::mem::transmute(f);
    if f(&mut o) == 0 { return core::ptr::null_mut(); }
    let mut dir = astr(file.as_ptr()).to_vec();
    let v = node(&dir);
    if v.is_null() || (*v).dir == 0 {
        if let Some(s) = dir.iter().rposition(|&c| c == b'\\') { if s > 2 { dir.truncate(s); } else { dir.truncate(s + 1); } }
    }
    make_pidl(&dir)
}
#[repr(C)] struct BiA { owner: H, root: *mut c_void, display: *mut u8, title: A, flags: u32, f: *mut c_void, lp: isize, image: i32 }
#[repr(C)] struct BiW { owner: H, root: *mut c_void, display: *mut u16, title: W, flags: u32, f: *mut c_void, lp: isize, image: i32 }

// ---------------------------------------------------------------- ShellExecute and file operations

#[repr(C)]
struct Sei { size: u32, mask: u32, hwnd: H, verb: *const c_void, file: *const c_void, params: *const c_void, dir: *const c_void, show: i32,
             inst: usize, idl: *mut c_void, cls: *const c_void, key: *mut c_void, hotkey: u32, icon: H, proc_: H }
unsafe fn exec(h: H, verb: Option<&[u8]>, file: &[u8], params: Option<&[u8]>, dir: Option<&[u8]>, show: i32, proc_: Option<&mut H>) -> usize {
    let mut proc_ = proc_;
    if let Some(p) = proc_.as_deref_mut() { *p = 0; }
    if file.is_empty() { return 2; }
    let (vz, fz, pz, dz) = (verb.map(cz), cz(file), params.map(cz), dir.map(cz));
    let is_url = file.windows(3).any(|w| w == b"://") || (file.len() >= 7 && file[..7].eq_ignore_ascii_case(b"mailto:"));
    if !is_url {
        let base = basename_of(file);
        let ext = base.iter().rposition(|&c| c == b'.').map(|p| &base[p..]);
        let exe = ext.is_some_and(|e| e.eq_ignore_ascii_case(b".exe") || e.eq_ignore_ascii_case(b".com"));
        if exe && verb.is_none_or(|v| v.eq_ignore_ascii_case(b"open") || v.eq_ignore_ascii_case(b"runas")) {
            let mut cmd = b"\"".to_vec(); cmd.extend_from_slice(file); cmd.push(b'"');
            if let Some(p) = params { if !p.is_empty() { cmd.push(b' '); } cmd.extend_from_slice(p); }
            cmd.truncate(1023); cmd.push(0);
            #[repr(C)] struct Si { cb: u32, rest: [u8; 100] }
            #[repr(C)] struct Pi { p: H, t: H, pid: u32, tid: u32 }
            let mut si: Si = core::mem::zeroed(); si.cb = core::mem::size_of::<Si>() as u32;
            let mut pi: Pi = core::mem::zeroed();
            let f = k32(c"CreateProcessA");
            if f.is_null() { return 2; }
            let f: winfn!(fn(A, *mut u8, *mut c_void, *mut c_void, i32, u32, *mut c_void, A, *mut Si, *mut Pi) -> i32) = core::mem::transmute(f);
            if f(core::ptr::null(), cmd.as_mut_ptr(), core::ptr::null_mut(), core::ptr::null_mut(), 0, 0, core::ptr::null_mut(), opt_ptr(&dz), &mut si, &mut pi) == 0 { return 2; }
            if let Some(p) = proc_ { *p = pi.p; }
            return 42;
        }
    }
    shell_execute_a(h, opt_ptr(&vz), fz.as_ptr(), opt_ptr(&pz), opt_ptr(&dz), show)
}
unsafe fn sei_exec(e: *mut Sei, wide: bool) -> i32 {
    let s = |p: *const c_void, cap: usize| -> Option<Vec<u8>> { if p.is_null() { None } else if wide { Some(w2a(p as W, cap)) } else { Some(astr(p as A).to_vec()) } };
    let (v, f, p, d) = (s((*e).verb, 32), s((*e).file, MAXP * 2), s((*e).params, 1024), s((*e).dir, MAXP));
    let mut pr = 0;
    let r = exec((*e).hwnd, v.as_deref(), f.as_deref().unwrap_or(b""), p.as_deref(), d.as_deref(), (*e).show, Some(&mut pr));
    (*e).inst = r;
    if (*e).mask & 0x40 != 0 { (*e).proc_ = pr; }
    if r <= 32 { set_err(2); return 0; }
    1
}
#[repr(C)] struct Shfos { hwnd: H, func: u32, from: *const c_void, to: *const c_void, flags: u16, aborted: i32, map: *mut c_void, title: *const c_void }
unsafe fn call_bool2(name: &core::ffi::CStr, a: &[u8], b: &[u8]) -> bool {
    let f = k32(name); if f.is_null() { return false; }
    let f: winfn!(fn(A, A) -> i32) = core::mem::transmute(f);
    f(cz(a).as_ptr(), cz(b).as_ptr()) != 0
}
/// Delete, rename, move and copy of a list of paths (first only for move and rename).
unsafe fn file_op(func: u32, from: &[Vec<u8>], to: &[u8]) -> i32 {
    for f in from {
        match func {
            3 => {                                          // FO_DELETE (files, and empty or full folders)
                let v = node(f);
                if v.is_null() { return 2; }
                if (*v).dir != 0 {
                    while !(*v).child.is_null() {
                        let mut c = [0u8; MAXP];
                        vfs_path((*v).child, c.as_mut_ptr() as *mut c_char, MAXP as c_int);
                        let before = (*v).child;
                        file_op(3, &[astr(c.as_ptr()).to_vec()], b"");
                        if (*v).child == before { break; }         // could not delete it: stop rather than loop
                    }
                    vfs_delete(v);
                } else {
                    let d = k32(c"DeleteFileA");
                    if !d.is_null() { let d: winfn!(fn(A) -> i32) = core::mem::transmute(d); d(cz(f).as_ptr()); }
                }
            }
            4 | 1 => {                                      // FO_RENAME, FO_MOVE
                let mut dst = to[..to.len().min(MAXP - 1)].to_vec();
                let d = node(&dst);
                if !d.is_null() && (*d).dir != 0 && func == 1 { dst.push(b'\\'); dst.extend_from_slice(&basename_of(f)); dst.truncate(MAXP - 1); }
                if !call_bool2(c"MoveFileA", f, &dst) { return 2; }
                return 0;
            }
            2 => {                                          // FO_COPY
                let mut dst = to[..to.len().min(MAXP - 1)].to_vec();
                let d = node(&dst);
                if !d.is_null() && (*d).dir != 0 { dst.push(b'\\'); dst.extend_from_slice(&basename_of(f)); dst.truncate(MAXP - 1); }
                let c = k32(c"CopyFileA");
                if c.is_null() { return 2; }
                let c: winfn!(fn(A, A, i32) -> i32) = core::mem::transmute(c);
                if c(cz(f).as_ptr(), cz(&dst).as_ptr(), 0) == 0 { return 2; }
            }
            _ => return 0x78,
        }
    }
    0
}
/// A double-NUL list of paths.
unsafe fn path_list(p: A) -> Vec<Vec<u8>> {
    let mut v = Vec::new();
    if p.is_null() { return v; }
    let mut q = p;
    loop { let s = astr(q); if s.is_empty() { break; } v.push(s.to_vec()); q = q.add(s.len() + 1); }
    v
}
#[repr(C)] struct SfiA { icon: H, iicon: i32, attr: u32, name: [u8; 260], ty: [u8; 80] }
#[repr(C)] struct SfiW { icon: H, iicon: i32, attr: u32, name: [u16; 260], ty: [u16; 80] }

// ---------------------------------------------------------------- version resources

/// The RT_VERSION resource of a PE file: a program's own module by name, the program by its
/// file name, else a path from the current folder.
unsafe fn version_resource(file: &[u8]) -> Option<&'static [u8]> {
    if file.is_empty() { return None; }
    let path = if !file.iter().any(|&c| c == b'\\' || c == b'/' || c == b':') {
        let m = w32_mod_path_by_name(cz(file).as_ptr() as *const c_char);
        if !m.is_null() { cstr(m).to_vec() }
        else if file.eq_ignore_ascii_case(&basename_of(cstr(w32_proc_exe()))) { cstr(w32_proc_exe()).to_vec() }
        else { joined(cstr(w32_proc_cwd()), file, MAXP) }
    } else { file[..file.len().min(MAXP - 1)].to_vec() };
    let v = node(&path);
    if v.is_null() || (*v).dir != 0 || (*v).data.is_null() { return None; }
    let d = core::slice::from_raw_parts((*v).data, (*v).size as usize);
    let (o, l) = u::version_resource(d)?;
    Some(&d[o..o + l])
}
unsafe fn ver_query(blk: *const u8, sub: &[u16]) -> Option<(usize, usize, bool)> {
    if blk.is_null() { return None; }
    let total = *(blk as *const u16) as usize;
    u::ver_find(core::slice::from_raw_parts(blk, total), sub)
}

// ---------------------------------------------------------------- winmm

#[repr(C)]
struct MmTimer { delay: u32, f: *const c_void, user: usize, flags: u32, id: u32, stop: bool }
struct Timers([*mut MmTimer; 16]);
static mut MMTIMERS: Timers = Timers([core::ptr::null_mut(); 16]);
unsafe fn timers() -> &'static mut [*mut MmTimer; 16] { &mut (*core::ptr::addr_of_mut!(MMTIMERS)).0 }

/// PlaySound's work: a sound in memory, a system sound by alias, or a .wav file.
unsafe fn play(name: Option<&[u8]>, mem: *const u8, memlen: u32, fl: u32) -> i32 {
    let mut err = [0u8; 120];
    if name.is_none() && mem.is_null() { audio_stop_all(); return 1; }
    if !mem.is_null() {
        let len = if memlen != 0 { memlen } else if *mem == b'R' { (mem.add(4) as *const u32).read_unaligned().wrapping_add(8) } else { 0 };
        let s = sound_load(mem, len, c"wav".as_ptr(), err.as_mut_ptr() as *mut c_char, 120);
        if s.is_null() { return 0; }
        audio_play(s);
        return 1;                                          // the voice keeps its sound; it stays loaded
    }
    let name = name.unwrap();
    if fl & 0x10000 != 0 || !name.contains(&b'.') {      // system sounds by alias
        for (alias, file) in [(&b"SystemStart"[..], "Windows XP Startup.wav"), (&b"SystemExit"[..], "Windows XP Shutdown.wav")] {
            if name.eq_ignore_ascii_case(alias) {
                let p = cz(alloc::format!("C:\\WRLD\\Media\\{file}").as_bytes());
                return audio_play_file(p.as_ptr() as *const c_char, err.as_mut_ptr() as *mut c_char, 120) as i32;
            }
        }
        speaker_beep(800, 60);
        return 1;
    }
    let p = if name.len() >= 2 && name[0].is_ascii_alphabetic() && name[1] == b':' { name[..name.len().min(MAXP - 1)].to_vec() } else { joined(cstr(w32_proc_cwd()), name, MAXP) };
    if node(&p).is_null() { if fl & 2 != 0 { return 0; } speaker_beep(800, 60); return 0; }
    let ok = audio_play_file(cz(&p).as_ptr() as *const c_char, err.as_mut_ptr() as *mut c_char, 120) != 0;
    if ok && fl & 1 == 0 { audio_wait(60_000); }
    ok as i32
}

#[repr(C)] struct WaveHdr { data: *mut u8, len: u32, recorded: u32, user: usize, flags: u32, loops: u32, next: *mut c_void, res: usize }
#[repr(C)] #[derive(Clone, Copy)] struct Wfx { tag: u16, ch: u16, rate: u32, bps: u32, align: u16, bits: u16, cb: u16 }
struct WOut { stream: c_int, fmt: Wfx, cb: usize, inst: usize, flags: u32, q: Vec<*mut WaveHdr>, played: u64, paused: bool, open: bool, thread: H }
const WHDR_DONE: u32 = 1;
const WHDR_PREPARED: u32 = 2;
const WHDR_INQUEUE: u32 = 0x10;
unsafe fn wout_notify(w: *mut WOut, msg: u32, p1: usize) {
    let kind = (*w).flags & 0x70000;
    if kind == 0x30000 && (*w).cb != 0 {                  // CALLBACK_FUNCTION
        let f: winfn!(fn(usize, u32, usize, usize, usize) -> ()) = core::mem::transmute((*w).cb);
        f(w as usize, msg, (*w).inst, p1, 0);
    } else if kind == 0x50000 && (*w).cb != 0 {           // CALLBACK_EVENT
        set_event((*w).cb);
    } else if kind == 0x10000 && (*w).cb != 0 {           // CALLBACK_WINDOW: MM_WOM_*
        let f = lookup(c"user32.dll", c"PostMessageA");
        if !f.is_null() { let f: winfn!(fn(H, u32, usize, isize) -> i32) = core::mem::transmute(f); f((*w).cb, msg, w as usize, p1 as isize); }
    }
}
/// Converts a buffer to 16-bit stereo at the output rate and queues it.
unsafe fn wout_push(w: *mut WOut, h: *mut WaveHdr) {
    let rate = audio_rate().max(1) as u64;
    let (ch, bits) = ((*w).fmt.ch as usize, (*w).fmt.bits as usize);
    let frames_in = ((*h).len as usize / (ch * bits / 8).max(1)) as u64;
    let src_rate = if (*w).fmt.rate != 0 { (*w).fmt.rate as u64 } else { rate };
    let frames_out = frames_in * rate / src_rate;
    let mut out: Vec<i16> = Vec::new();
    if out.try_reserve_exact(frames_out as usize * 2).is_err() { return; }
    let inp = (*h).data;
    for i in 0..frames_out {
        let mut s = i * src_rate / rate; if s >= frames_in { s = frames_in.saturating_sub(1); }
        let (l, r) = if bits == 16 {
            let p = (inp as *const i16).add(s as usize * ch); let l = *p as i32; (l, if ch > 1 { *p.add(1) as i32 } else { l })
        } else {
            let p = inp.add(s as usize * ch); let l = (*p as i32 - 128) << 8; (l, if ch > 1 { (*p.add(1) as i32 - 128) << 8 } else { l })
        };
        out.push(l as i16); out.push(r as i16);
    }
    let mut done = 0u64;
    while done < frames_out && (*w).open {
        let room = audio_stream_room((*w).stream);
        if room <= 0 { task_sleep(5); continue; }
        let k = audio_stream_write((*w).stream, out.as_ptr().add(done as usize * 2), (room as u64).min(frames_out - done) as c_int);
        if k <= 0 { task_sleep(5); continue; }
        done += k as u64;
    }
    (*w).played += frames_in;
}
#[repr(C)] struct WocW { mid: u16, pid: u16, ver: u32, name: [u16; 32], formats: u32, ch: u16, res: u16, support: u32 }
#[repr(C)] struct WocA { mid: u16, pid: u16, ver: u32, name: [u8; 32], formats: u32, ch: u16, res: u16, support: u32 }
#[repr(C)] struct MmTime { ty: u32, v: u32, pad: [u32; 2] }

/// MCI's "play <file>" on .wav files; the rest reports no device.
unsafe fn mci(cmd: &[u8]) -> u32 {
    let c = &cmd[..cmd.len().min(MAXP - 1)];
    if c.len() >= 5 && c[..5].eq_ignore_ascii_case(b"play ") {
        let mut f = &c[5..];
        while f.first() == Some(&b' ') { f = &f[1..]; }
        let f = if f.first() == Some(&b'"') { let r = &f[1..]; &r[..r.iter().position(|&x| x == b'"').unwrap_or(r.len())] } else { &f[..f.iter().position(|&x| x == b' ').unwrap_or(f.len())] };
        if let Some(d) = f.iter().rposition(|&x| x == b'.') { if f[d..].eq_ignore_ascii_case(b".wav") { return if play(Some(f), core::ptr::null(), 0, 1) != 0 { 0 } else { 275 }; } }
    }
    if (c.len() >= 4 && c[..4].eq_ignore_ascii_case(b"stop")) || (c.len() >= 5 && c[..5].eq_ignore_ascii_case(b"close")) { audio_stop_all(); return 0; }
    263                                                    // MCIERR_INVALID_DEVICE_NAME
}

winapi! {
    // ================================================================ shlwapi: paths
    fn PathFindFileNameW(p: W) -> W { p.add(u::find_file_name(ws(p))) }
    fn PathFindFileNameA(p: A) -> A { p.add(u::find_file_name(&widen(astr(p)))) }
    fn PathFindExtensionW(p: W) -> W { p.add(u::find_extension(ws(p))) }
    fn PathFindExtensionA(p: A) -> A { p.add(u::find_extension(&widen(astr(p)))) }
    fn PathFileExistsA(p: A) -> i32 { (!p.is_null() && file_attrs(p) != 0xFFFF_FFFF) as i32 }
    fn PathFileExistsW(p: W) -> i32 { if p.is_null() { return 0; } let a = cz(&w2a(p, 520)); PathFileExistsA(a.as_ptr()) }
    fn PathIsDirectoryA(p: A) -> i32 { let a = file_attrs(p); if a != 0xFFFF_FFFF && a & 0x10 != 0 { 0x10 } else { 0 } }
    fn PathIsDirectoryW(p: W) -> i32 { let a = cz(&w2a(p, 520)); PathIsDirectoryA(a.as_ptr()) }
    fn PathIsRelativeW(p: W) -> i32 { (p.is_null() || u::is_relative(ws(p))) as i32 }
    fn PathIsRelativeA(p: A) -> i32 { (p.is_null() || u::is_relative(&widen(astr(p)))) as i32 }
    fn PathIsRootW(p: W) -> i32 { (!p.is_null() && u::is_root(ws(p))) as i32 }
    fn PathIsRootA(p: A) -> i32 { (!p.is_null() && u::is_root(&widen(&astr(p)[..astr(p).len().min(MAXP - 1)]))) as i32 }
    fn PathIsUNCW(p: W) -> i32 { (!p.is_null() && is_sep(*p) && is_sep(*p.add(1))) as i32 }
    fn PathIsUNCA(p: A) -> i32 { (!p.is_null() && *p == b'\\' && *p.add(1) == b'\\') as i32 }
    fn PathIsURLW(p: W) -> i32 {
        let s = if p.is_null() { &[][..] } else { ws(p) };
        for i in 0..s.len().min(12) { if s[i] == b':' as u16 { return (i > 1 && s.get(i + 1) == Some(&(b'/' as u16))) as i32; } }
        0
    }
    fn PathIsURLA(p: A) -> i32 { if p.is_null() { return 0; } let w = wz(&widen(&astr(p)[..astr(p).len().min(MAXP - 1)])); PathIsURLW(w.as_ptr()) }
    fn PathIsFileSpecW(p: W) -> i32 { (!ws(p).iter().any(|&c| is_sep(c) || c == b':' as u16)) as i32 }
    fn PathGetDriveNumberW(p: W) -> i32 {
        if p.is_null() || *p == 0 || *p.add(1) != b':' as u16 || !((*p & 0x7F) as u8).is_ascii_alphabetic() { return -1; }
        wlow(*p) as i32 - b'a' as i32
    }
    fn PathGetDriveNumberA(p: A) -> i32 { if p.is_null() || *p == 0 || *p.add(1) != b':' || !(*p).is_ascii_alphabetic() { -1 } else { (*p).to_ascii_lowercase() as i32 - b'a' as i32 } }
    fn PathAddBackslashW(p: *mut u16) -> *mut u16 {
        let mut l = ws(p).len();
        if l + 1 >= MAXP { return core::ptr::null_mut(); }
        if l == 0 || !is_sep(*p.add(l - 1)) { *p.add(l) = b'\\' as u16; l += 1; *p.add(l) = 0; }
        p.add(l)
    }
    fn PathAddBackslashA(p: *mut u8) -> *mut u8 {
        let mut l = astr(p).len();
        if l + 1 >= MAXP { return core::ptr::null_mut(); }
        if l == 0 || (*p.add(l - 1) != b'\\' && *p.add(l - 1) != b'/') { *p.add(l) = b'\\'; l += 1; *p.add(l) = 0; }
        p.add(l)
    }
    fn PathRemoveBackslashW(p: *mut u16) -> *mut u16 {
        let mut l = ws(p).len();
        if l > 0 && is_sep(*p.add(l - 1)) && !u::is_root(ws(p)) { l -= 1; *p.add(l) = 0; }
        p.add(l.saturating_sub(1))
    }
    fn PathRemoveBackslashA(p: *mut u8) -> *mut u8 {
        let mut l = astr(p).len();
        if l > 0 && (*p.add(l - 1) == b'\\' || *p.add(l - 1) == b'/') && !(l == 3 && *p.add(1) == b':') { l -= 1; *p.add(l) = 0; }
        p.add(l.saturating_sub(1))
    }
    fn PathRemoveFileSpecW(p: *mut u16) -> i32 { let (v, ch) = u::remove_file_spec(ws(p)); *p.add(v.len()) = 0; ch as i32 }
    fn PathRemoveFileSpecA(p: *mut u8) -> i32 {
        let w = widen(&astr(p)[..astr(p).len().min(MAXP - 1)]);
        let (v, ch) = u::remove_file_spec(&w);
        let a: Vec<u8> = v.iter().map(|&c| if c < 256 { c as u8 } else { b'?' }).collect();
        put_a(&a, p, MAXP); ch as i32
    }
    fn PathRemoveExtensionW(p: *mut u16) -> () { *p.add(u::find_extension(ws(p))) = 0; }
    fn PathRemoveExtensionA(p: *mut u8) -> () { *p.add(u::find_extension(&widen(astr(p)))) = 0; }
    fn PathRenameExtensionW(p: *mut u16, ext: W) -> i32 {
        let e = u::find_extension(ws(p)); let x = ws(ext);
        if e + x.len() >= MAXP { return 0; }
        core::ptr::copy(ext, p.add(e), x.len() + 1); 1
    }
    fn PathRenameExtensionA(p: *mut u8, ext: A) -> i32 {
        let e = u::find_extension(&widen(astr(p))); let x = astr(ext);
        if e + x.len() >= MAXP { return 0; }
        core::ptr::copy(ext, p.add(e), x.len() + 1); 1
    }
    fn PathAddExtensionW(p: *mut u16, ext: W) -> i32 {
        let s = ws(p);
        if u::find_extension(s) < s.len() { return 0; }
        let x: Vec<u16> = if ext.is_null() { b".exe".iter().map(|&c| c as u16).collect() } else { ws(ext).to_vec() };
        let l = s.len();
        for (i, &c) in x.iter().enumerate() { *p.add(l + i) = c; }
        *p.add(l + x.len()) = 0;
        1
    }
    fn PathStripPathW(p: *mut u16) -> () { let s = ws(p); let f = u::find_file_name(s); if f != 0 { core::ptr::copy(p.add(f), p, s.len() - f + 1); } }
    fn PathStripPathA(p: *mut u8) -> () { let s = astr(p); let f = u::find_file_name(&widen(s)); if f != 0 { core::ptr::copy(p.add(f), p, s.len() - f + 1); } }
    fn PathStripToRootW(p: *mut u16) -> i32 {
        if *p != 0 && *p.add(1) == b':' as u16 { if is_sep(*p.add(2)) { *p.add(3) = 0; } else { *p.add(2) = 0; } return 1; }
        if is_sep(*p) { *p.add(1) = 0; return 1; }
        0
    }
    fn PathStripToRootA(p: *mut u8) -> i32 {
        if *p != 0 && *p.add(1) == b':' { if *p.add(2) == b'\\' { *p.add(3) = 0; } else { *p.add(2) = 0; } return 1; }
        if *p == b'\\' { *p.add(1) = 0; return 1; }
        0
    }
    fn PathSkipRootW(p: W) -> W {
        if *p != 0 && *p.add(1) == b':' as u16 && is_sep(*p.add(2)) { return p.add(3); }
        if is_sep(*p) && !is_sep(*p.add(1)) { return p.add(1); }
        core::ptr::null()
    }
    fn PathSkipRootA(p: A) -> A {
        if *p != 0 && *p.add(1) == b':' && *p.add(2) == b'\\' { return p.add(3); }
        if *p == b'\\' && *p.add(1) != b'\\' { return p.add(1); }
        core::ptr::null()
    }
    fn PathFindNextComponentW(p: W) -> W {
        if p.is_null() || *p == 0 { return core::ptr::null(); }
        let mut q = p; while *q != 0 && !is_sep(*q) { q = q.add(1); }
        if *q != 0 { q = q.add(1); }
        q
    }
    fn PathCanonicalizeW(out: *mut u16, inp: W) -> i32 {
        let s = if inp.is_null() { &[][..] } else { ws(inp) };
        let t = u::canonicalize(s);
        put_w(&t, out, t.len() + 1); 1
    }
    fn PathCanonicalizeA(out: *mut u8, inp: A) -> i32 {
        let w = widen(&astr(inp)[..astr(inp).len().min(MAXP - 1)]);
        let t = u::canonicalize(&w);
        let a: Vec<u8> = t.iter().take(MAXP - 1).map(|&c| if c < 256 { c as u8 } else { b'?' }).collect();
        put_a(&a, out, MAXP); 1
    }
    fn PathCombineW(out: *mut u16, dir: W, file: W) -> *mut u16 {
        let d = if dir.is_null() { None } else { Some(ws(dir)) };
        let f = if file.is_null() { None } else { Some(ws(file)) };
        match u::combine(d, f) {
            Some(t) => { put_w(&t, out, t.len() + 1); out }
            None => { if !out.is_null() { *out = 0; } core::ptr::null_mut() }
        }
    }
    fn PathCombineA(out: *mut u8, dir: A, file: A) -> *mut u8 {
        let cut = |s: &[u8]| widen(&s[..s.len().min(MAXP - 1)]);
        let (d, f) = (cut(astr(dir)), cut(astr(file)));
        match u::combine(if dir.is_null() { None } else { Some(&d) }, if file.is_null() { None } else { Some(&f) }) {
            Some(t) => { let a: Vec<u8> = t.iter().take(MAXP - 1).map(|&c| if c < 256 { c as u8 } else { b'?' }).collect(); put_a(&a, out, MAXP); out }
            None => { *out = 0; core::ptr::null_mut() }
        }
    }
    fn PathAppendW(p: *mut u16, more: W) -> i32 {
        let mut m = more; while !m.is_null() && is_sep(*m) { m = m.add(1); }
        match u::combine(Some(ws(p)), if m.is_null() { None } else { Some(ws(m)) }) { Some(t) => { put_w(&t, p, t.len() + 1); 1 } None => 0 }
    }
    fn PathAppendA(p: *mut u8, more: A) -> i32 {
        let mut m = more; while !m.is_null() && (*m == b'\\' || *m == b'/') { m = m.add(1); }
        let r = PathCombineA(p, p, m);
        (!r.is_null()) as i32
    }
    fn PathCommonPrefixW(a: W, b: W, out: *mut u16) -> i32 {
        let n = u::common_prefix(ws(a), ws(b));
        if !out.is_null() { core::ptr::copy(a, out, n); *out.add(n) = 0; }
        n as i32
    }
    fn PathIsPrefixW(pre: W, p: W) -> i32 { (u::common_prefix(ws(pre), ws(p)) == ws(pre).len()) as i32 }
    fn PathIsSameRootW(a: W, b: W) -> i32 { (*a != 0 && *b != 0 && wlow(*a) == wlow(*b) && *a.add(1) == b':' as u16 && *b.add(1) == b':' as u16) as i32 }
    fn PathRelativePathToW(out: *mut u16, from: W, fa: u32, to: W, ta: u32) -> i32 {
        match u::relative_path(ws(from), fa & 0x10 != 0, ws(to)) { Some(r) => { put_w(&r, out, r.len() + 1); 1 } None => 0 }
    }
    fn PathMatchSpecW(file: W, spec: W) -> i32 { u::match_spec(ws(file), ws(spec)) as i32 }
    fn PathMatchSpecA(f: A, s: A) -> i32 {
        let cut = |s: &[u8]| widen(&s[..s.len().min(MAXP - 1)]);
        u::match_spec(&cut(astr(f)), &cut(astr(s))) as i32
    }
    fn PathQuoteSpacesW(p: *mut u16) -> i32 {
        let l = ws(p).len();
        if !ws(p).contains(&(b' ' as u16)) || l + 3 > MAXP { return 0; }
        core::ptr::copy(p, p.add(1), l + 1);
        *p = b'"' as u16; *p.add(l + 1) = b'"' as u16; *p.add(l + 2) = 0;
        1
    }
    fn PathUnquoteSpacesW(p: *mut u16) -> () {
        let l = ws(p).len();
        if l >= 2 && *p == b'"' as u16 && *p.add(l - 1) == b'"' as u16 { core::ptr::copy(p.add(1), p, l - 2); *p.add(l - 2) = 0; }
    }
    fn PathUnquoteSpacesA(p: *mut u8) -> () {
        let l = astr(p).len();
        if l >= 2 && *p == b'"' && *p.add(l - 1) == b'"' { core::ptr::copy(p.add(1), p, l - 2); *p.add(l - 2) = 0; }
    }
    fn PathUnExpandEnvStringsW(p: W, out: *mut u16, n: u32) -> i32 { 0 }
    fn PathCompactPathExW(out: *mut u16, p: W, n: u32, f: u32) -> i32 {
        let (t, ok) = u::compact_path(ws(p), n as usize);
        if n == 0 { return ok as i32; }
        put_w(&t, out, n as usize);
        ok as i32
    }
    fn PathCompactPathExA(out: *mut u8, p: A, n: u32, f: u32) -> i32 {
        let w = widen(&astr(p)[..astr(p).len().min(MAXP - 1)]);
        let (t, ok) = u::compact_path(&w, (n as usize).min(MAXP));
        let a: Vec<u8> = t.iter().map(|&c| if c < 256 { c as u8 } else { b'?' }).collect();
        put_a(&a, out, n as usize);
        ok as i32
    }
    fn SHAutoComplete(h: H, f: u32) -> i32 { S_OK }
    fn IsOS(os: u32) -> i32 { (os == 0 || os == 2 || os == 25) as i32 }

    // ================================================================ shlwapi: strings
    fn StrChrW(s: W, c: u16) -> W { if s.is_null() { return s; } match ws(s).iter().position(|&x| x == c) { Some(i) => s.add(i), None => core::ptr::null() } }
    fn StrChrA(s: A, c: u16) -> A {
        if s.is_null() { return s; }
        let t = astr(s); let c = c as u8;
        if c == 0 { return s.add(t.len()); }
        match t.iter().position(|&x| x == c) { Some(i) => s.add(i), None => core::ptr::null() }
    }
    fn StrChrIW(s: W, c: u16) -> W { if s.is_null() { return s; } match ws(s).iter().position(|&x| wlow(x) == wlow(c)) { Some(i) => s.add(i), None => core::ptr::null() } }
    fn StrChrIA(s: A, c: u16) -> A {
        if s.is_null() { return s; }
        let lc = if c < 128 { (c as u8).to_ascii_lowercase() as u16 } else { c };
        match astr(s).iter().position(|&x| x < 128 && x.to_ascii_lowercase() as u16 == lc) { Some(i) => s.add(i), None => core::ptr::null() }
    }
    fn StrRChrW(s: W, end: W, c: u16) -> W {
        if s.is_null() { return s; }
        let n = if end.is_null() { ws(s).len() } else { (end as usize - s as usize) / 2 };
        let t = core::slice::from_raw_parts(s, n);
        match t.iter().rposition(|&x| x == c) { Some(i) => s.add(i), None => core::ptr::null() }
    }
    fn StrRChrA(s: A, end: A, c: u16) -> A {
        if s.is_null() { return s; }
        let n = if end.is_null() { astr(s).len() } else { end as usize - s as usize };
        let t = core::slice::from_raw_parts(s, n);
        match t.iter().rposition(|&x| x == c as u8) { Some(i) => s.add(i), None => core::ptr::null() }
    }
    fn StrCmpW(a: W, b: W) -> i32 { u::wcmpn(ws(a), ws(b), None, false) }
    fn StrCmpIW(a: W, b: W) -> i32 { u::wcmpn(ws(a), ws(b), None, true) }
    fn StrCmpNW(a: W, b: W, n: i32) -> i32 { u::wcmpn(ws(a), ws(b), if n < 0 { None } else { Some(n as usize) }, false) }
    fn StrCmpNIW(a: W, b: W, n: i32) -> i32 { u::wcmpn(ws(a), ws(b), if n < 0 { None } else { Some(n as usize) }, true) }
    fn StrCmpNA(a: A, b: A, n: i32) -> i32 { u::wcmpn(&widen(astr(a)), &widen(astr(b)), Some(n.max(0) as usize), false) }
    fn StrCmpNIA(a: A, b: A, n: i32) -> i32 { u::wcmpn(&widen(astr(a)), &widen(astr(b)), Some(n.max(0) as usize), true) }
    fn StrCmpIA(a: A, b: A) -> i32 { u::wcmpn(&widen(astr(a)), &widen(astr(b)), None, true) }
    fn StrCmpLogicalW(a: W, b: W) -> i32 { u::cmp_logical(ws(a), ws(b)) }
    fn StrCpyW(d: *mut u16, s: W) -> *mut u16 { let n = ws(s).len(); core::ptr::copy(s, d, n + 1); d }
    fn StrCpyNW(d: *mut u16, s: W, n: i32) -> *mut u16 { if n <= 0 { return d; } put_w(wstr(s, n as usize - 1), d, n as usize); d }
    fn StrCpyNA(d: *mut u8, s: A, n: i32) -> *mut u8 { if n > 0 { put_a(astr(s), d, n as usize); } d }
    fn StrCatW(d: *mut u16, s: W) -> *mut u16 { StrCpyW(d.add(ws(d).len()), s); d }
    fn StrNCatW(d: *mut u16, s: W, n: i32) -> *mut u16 {
        let l = ws(d).len(); let src = wstr(s, n.max(0) as usize);
        core::ptr::copy(src.as_ptr(), d.add(l), src.len()); *d.add(l + src.len()) = 0; d
    }
    fn StrNCatA(d: *mut u8, s: A, n: i32) -> *mut u8 {
        let l = astr(d).len(); let src = astr(s); let k = src.len().min(n.max(0) as usize);
        core::ptr::copy(s, d.add(l), k); *d.add(l + k) = 0; d
    }
    fn StrCatBuffW(d: *mut u16, s: W, n: i32) -> *mut u16 { let l = ws(d).len() as i32; if l < n - 1 { StrCpyNW(d.add(l as usize), s, n - l); } d }
    fn StrCatBuffA(d: *mut u8, s: A, n: i32) -> *mut u8 { let l = astr(d).len() as i32; if l < n - 1 { put_a(astr(s), d.add(l as usize), (n - l) as usize); } d }
    fn StrDupW(s: W) -> *mut u16 {
        let l = ws(s).len();
        let d = local_alloc(0, (l + 1) * 2) as *mut u16;
        if !d.is_null() { core::ptr::copy_nonoverlapping(s, d, l + 1); }
        d
    }
    fn StrDupA(s: A) -> *mut u8 {
        let l = astr(s).len();
        let d = local_alloc(0, l + 1);
        if !d.is_null() { core::ptr::copy_nonoverlapping(s, d, l + 1); }
        d
    }
    fn StrStrW(h: W, n: W) -> W { if h.is_null() || n.is_null() { return core::ptr::null(); } wstrstr(h, n, false) }
    fn StrStrIW(h: W, n: W) -> W { if h.is_null() || n.is_null() { return core::ptr::null(); } wstrstr(h, n, true) }
    fn StrStrA(h: A, n: A) -> A {
        if h.is_null() || n.is_null() { return core::ptr::null(); }
        let (hs, ns) = (astr(h), astr(n));
        if ns.is_empty() { return h; }
        match hs.windows(ns.len()).position(|w| w == ns) { Some(i) => h.add(i), None => core::ptr::null() }
    }
    fn StrStrIA(h: A, n: A) -> A {
        if h.is_null() || n.is_null() { return core::ptr::null(); }
        let (hs, ns) = (astr(h), astr(n));
        if ns.is_empty() { return h; }
        match hs.windows(ns.len()).position(|w| w.eq_ignore_ascii_case(ns)) { Some(i) => h.add(i), None => core::ptr::null() }
    }
    fn StrTrimW(s: *mut u16, trim: W) -> i32 {
        let (t, set) = (ws(s), ws(trim));
        let (mut a, mut b) = (0, t.len());
        while a < b && set.contains(&t[a]) { a += 1; }
        while b > a && set.contains(&t[b - 1]) { b -= 1; }
        if a == 0 && b == t.len() { return 0; }
        core::ptr::copy(s.add(a), s, b - a); *s.add(b - a) = 0; 1
    }
    fn StrTrimA(s: *mut u8, trim: A) -> i32 {
        let (t, set) = (astr(s), astr(trim));
        let (mut a, mut b) = (0, t.len());
        while a < b && set.contains(&t[a]) { a += 1; }
        while b > a && set.contains(&t[b - 1]) { b -= 1; }
        if a == 0 && b == t.len() { return 0; }
        core::ptr::copy(s.add(a), s, b - a); *s.add(b - a) = 0; 1
    }
    fn StrToIntW(s: W) -> i32 {
        let t = ws(s); let (mut i, mut sg) = (0, 1i32);
        if t.first() == Some(&(b'-' as u16)) { sg = -1; i = 1; }
        let mut v = 0i32;
        while i < t.len() && (b'0' as u16..=b'9' as u16).contains(&t[i]) { v = v.wrapping_mul(10).wrapping_add((t[i] - b'0' as u16) as i32); i += 1; }
        v.wrapping_mul(sg)
    }
    fn StrToIntA(s: A) -> i32 { u::kstrtol(astr(s), 10) as i32 }
    fn StrToIntExW(s: W, f: u32, out: *mut i32) -> i32 { let (v, any) = u::str_to_int_ex(ws(s), f & 1 != 0); *out = v; any as i32 }
    fn StrToIntExA(s: A, f: u32, out: *mut i32) -> i32 { let w = widen(&astr(s)[..astr(s).len().min(63)]); let (v, any) = u::str_to_int_ex(&w, f & 1 != 0); *out = v; any as i32 }
    fn StrFormatByteSizeW(n: i64, b: *mut u16, cap: u32) -> *mut u16 { a2w(u::fmt_bytes(n).as_bytes(), b, cap as usize); b }
    fn StrFormatByteSizeA(n: u32, b: *mut u8, cap: u32) -> *mut u8 { put_a(u::fmt_bytes(n as i64).as_bytes(), b, cap as usize); b }
    fn StrFormatByteSize64A(n: i64, b: *mut u8, cap: u32) -> *mut u8 { put_a(u::fmt_bytes(n).as_bytes(), b, cap as usize); b }
    fn StrFormatKBSizeW(n: i64, b: *mut u16, cap: u32) -> *mut u16 { a2w(alloc::format!("{} KB", (n + 1023) / 1024).as_bytes(), b, cap as usize); b }
    fn StrRetToBufW(s: *mut u8, pidl: *const u8, b: *mut u16, n: u32) -> i32 {
        let ty = *(s as *const u32);
        let un = s.add(core::mem::size_of::<usize>());     // the union after uType
        if ty == 0 { let w = *(un as *const *mut u16); StrCpyNW(b, w, n as i32); free(w as *mut u8); }
        else if ty == 2 { a2w(astr(un), b, n as usize); }
        else if ty == 1 && !pidl.is_null() { a2w(astr(pidl.add(*(un as *const u32) as usize)), b, n as usize); }
        S_OK
    }
    fn StrRetToStrW(s: *mut u8, pidl: *const u8, out: *mut *mut u16) -> i32 {
        let mut b = [0u16; MAXP];
        StrRetToBufW(s, pidl, b.as_mut_ptr(), MAXP as u32);
        let l = ws(b.as_ptr()).len();
        let o = alloc((l + 1) * 2) as *mut u16;
        if !o.is_null() { core::ptr::copy_nonoverlapping(b.as_ptr(), o, l + 1); }
        *out = o; S_OK
    }
    fn SHStrDupW(s: W, out: *mut *mut u16) -> i32 {
        let l = ws(s).len(); let o = alloc((l + 1) * 2) as *mut u16;
        if !o.is_null() { core::ptr::copy_nonoverlapping(s, o, l + 1); }
        *out = o; S_OK
    }
    fn SHStrDupA(s: A, out: *mut *mut u16) -> i32 { let t = astr(s); let o = alloc((t.len() + 1) * 2) as *mut u16; a2w(t, o, t.len() + 1); *out = o; S_OK }
    fn UrlEscapeW(inp: W, out: *mut u16, n: P, f: u32) -> i32 {
        let t = u::url_escape(ws(inp), f & 0x0004_0000 != 0);
        let k = t.len() as u32;
        if *n <= k { *n = k + 1; return E_POINTER; }
        put_w(&t, out, t.len() + 1); *n = k; S_OK
    }
    fn UrlUnescapeW(inp: *mut u16, out: *mut u16, n: P, f: u32) -> i32 {
        let t = u::url_unescape(ws(inp));
        if f & 0x0010_0000 != 0 { put_w(&t, inp, t.len() + 1); return S_OK; }   // URL_UNESCAPE_INPLACE
        let k = t.len() as u32;
        if out.is_null() || n.is_null() || *n <= k { if !n.is_null() { *n = k + 1; } return E_POINTER; }
        put_w(&t, out, t.len() + 1); *n = k; S_OK
    }
    fn SHGetObjectCompatFlags(un: *mut c_void, c: *const c_void) -> u32 { 0 }
    fn SHCreateStreamOnFileW(f: W, m: u32, out: *mut *mut c_void) -> i32 { *out = core::ptr::null_mut(); E_NOTIMPL }
    fn SHCreateStreamOnFileEx(f: W, m: u32, a: u32, c: i32, t: *mut c_void, out: *mut *mut c_void) -> i32 { *out = core::ptr::null_mut(); E_NOTIMPL }
    fn SHAnsiToUnicode(a: A, w: *mut u16, n: i32) -> u32 { a2w(astr(a), w, n.max(0) as usize) as u32 + 1 }
    fn SHUnicodeToAnsi(w: W, a: *mut u8, n: i32) -> u32 { if n <= 0 { if !a.is_null() { *a = 0; } return 1; } let s = w2a(w, n as usize); put_a(&s, a, n as usize) as u32 + 1 }
    fn ColorHLSToRGB(h: u16, l: u16, s: u16) -> u32 { u::hls_to_rgb(h, l, s) }
    fn ColorRGBToHLS(rgb: u32, h: *mut u16, l: *mut u16, s: *mut u16) -> () { let (a, b, c) = u::rgb_to_hls(rgb); *h = a; *l = b; *s = c; }

    // ================================================================ shell32
    fn SHGetFolderPathA(h: H, csidl: i32, t: H, f: u32, out: *mut u8) -> i32 { match folder_path(csidl) { Ok(p) => { put_a(&p, out, MAXP); S_OK } Err(e) => e } }
    fn SHGetFolderPathW(h: H, csidl: i32, t: H, f: u32, out: *mut u16) -> i32 { match folder_path(csidl) { Ok(p) => { a2w(&p, out, MAXP); S_OK } Err(e) => { *out = 0; e } } }
    fn SHGetFolderPathAndSubDirW(h: H, csidl: i32, t: H, f: u32, sub: W, out: *mut u16) -> i32 {
        let mut a = match folder_path(csidl) { Ok(p) => p, Err(e) => return e };
        if !sub.is_null() { a.push(b'\\'); a.extend_from_slice(&w2a(sub, MAXP)); a.truncate(MAXP - 1); if csidl & 0x8000 != 0 { mkdirs(&a); } }
        a2w(&a, out, MAXP); S_OK
    }
    fn SHGetSpecialFolderPathA(h: H, out: *mut u8, csidl: i32, create: i32) -> i32 { match folder_path(csidl | if create != 0 { 0x8000 } else { 0 }) { Ok(p) => { put_a(&p, out, MAXP); 1 } Err(_) => 0 } }
    fn SHGetSpecialFolderPathW(h: H, out: *mut u16, csidl: i32, create: i32) -> i32 { match folder_path(csidl | if create != 0 { 0x8000 } else { 0 }) { Ok(p) => { a2w(&p, out, MAXP); 1 } Err(_) => 0 } }
    fn SHGetKnownFolderPath(guid: *const u32, f: u32, t: H, out: *mut *mut u16) -> i32 {
        let c = if guid.is_null() { None } else { u::known_folder_csidl(*guid) };
        match c.map(|c| folder_path(c | if f & 0x8000 != 0 { 0x8000 } else { 0 })) {
            Some(Ok(a)) => { let o = alloc((a.len() + 1) * 2) as *mut u16; a2w(&a, o, a.len() + 1); *out = o; S_OK }
            _ => { *out = core::ptr::null_mut(); 0x8007_0002u32 as i32 }
        }
    }
    fn SHGetSpecialFolderLocation(h: H, csidl: i32, out: *mut *mut u8) -> i32 { match folder_path(csidl) { Ok(a) => { *out = make_pidl(&a); S_OK } Err(_) => { *out = core::ptr::null_mut(); E_INVALIDARG } } }
    fn SHGetFolderLocation(h: H, csidl: i32, t: H, r: u32, out: *mut *mut u8) -> i32 { SHGetSpecialFolderLocation(h, csidl, out) }
    fn SHGetKnownFolderIDList(g: *const u32, f: u32, t: H, out: *mut *mut u8) -> i32 { match u::known_folder_csidl(*g) { Some(c) => SHGetSpecialFolderLocation(0, c, out), None => 0x8007_0002u32 as i32 } }
    fn SHGetPathFromIDListA(pidl: *const u8, out: *mut u8) -> i32 { put_a(pidl_path(pidl), out, MAXP); 1 }
    fn SHGetPathFromIDListW(pidl: *const u8, out: *mut u16) -> i32 { a2w(pidl_path(pidl), out, MAXP); 1 }
    fn SHGetPathFromIDListEx(pidl: *const u8, out: *mut u16, n: u32, f: i32) -> i32 { a2w(pidl_path(pidl), out, n as usize); 1 }
    fn ILCreateFromPathW(p: W) -> *mut u8 { make_pidl(&w2a(p, MAXP)) }
    fn ILCreateFromPathA(p: A) -> *mut u8 { make_pidl(astr(p)) }
    fn ILFree(p: *mut u8) -> () { free(p) }
    fn ILClone(p: *const u8) -> *mut u8 { make_pidl(&pidl_path(p).to_vec()) }
    fn ILGetSize(p: *const u8) -> u32 { if p.is_null() { 0 } else { *(p as *const u16) as u32 + 2 } }
    fn SHParseDisplayName(n: W, bc: *mut c_void, out: *mut *mut u8, a: u32, ao: P) -> i32 { *out = make_pidl(&w2a(n, MAXP)); if !ao.is_null() { *ao = 0; } S_OK }
    fn SHGetDataFromIDListW(sf: *mut c_void, pidl: *const u8, fmt: i32, out: *mut c_void, n: i32) -> i32 { E_INVALIDARG }
    fn im_qi(t: *mut c_void, iid: *const c_void, o: *mut *mut c_void) -> i32 { *o = t; S_OK }
    fn im_ref(t: *mut c_void) -> usize { 1 }
    fn im_alloc(t: *mut c_void, n: usize) -> *mut u8 { alloc(n.max(1)) }
    fn im_realloc(t: *mut c_void, p: *mut c_void, n: usize) -> *mut c_void { w32_realloc(w32_cur(), p, n as u32) }
    fn im_free(t: *mut c_void, p: *mut u8) -> () { free(p) }
    fn im_size(t: *mut c_void, p: *mut c_void) -> usize { w32_msize(p) as usize }
    fn im_did(t: *mut c_void, p: *mut c_void) -> i32 { -1 }
    fn im_min(t: *mut c_void) -> () {}
    fn SHGetMalloc(out: *mut *const IMalloc) -> i32 { *out = &IMALLOC; S_OK }
    fn CoGetMalloc2(c: u32, out: *mut *const IMalloc) -> i32 { *out = &IMALLOC; S_OK }
    fn SHGetDesktopFolder(out: *mut *mut c_void) -> i32 { *out = core::ptr::null_mut(); E_NOTIMPL }
    fn SHBrowseForFolderA(b: *mut BiA) -> *mut u8 {
        let r = browse((*b).owner, astr((*b).title));
        if !r.is_null() && !(*b).display.is_null() { put_a(&basename_of(pidl_path(r)), (*b).display, MAXP); }
        r
    }
    fn SHBrowseForFolderW(b: *mut BiW) -> *mut u8 {
        let r = browse((*b).owner, &w2a((*b).title, 128));
        if !r.is_null() && !(*b).display.is_null() { a2w(&basename_of(pidl_path(r)), (*b).display, MAXP); }
        r
    }
    fn CommandLineToArgvW(cmd: W, argc: *mut i32) -> *mut *mut u16 {
        let args = u::cmdline_to_argv(ws(cmd), cstr(w32_proc_exe()));
        let chars: usize = args.iter().map(|a| a.len() + 1).sum();
        let ptrs = (args.len() + 1) * core::mem::size_of::<usize>();
        let blk = local_alloc(0, ptrs + chars * 2);
        if blk.is_null() { return core::ptr::null_mut(); }
        let argv = blk as *mut *mut u16;
        let mut s = blk.add(ptrs) as *mut u16;
        for (i, a) in args.iter().enumerate() {
            *argv.add(i) = s;
            core::ptr::copy_nonoverlapping(a.as_ptr(), s, a.len()); *s.add(a.len()) = 0;
            s = s.add(a.len() + 1);
        }
        *argv.add(args.len()) = core::ptr::null_mut();
        if !argc.is_null() { *argc = args.len() as i32; }
        argv
    }
    fn ShellExecuteW(h: H, verb: W, file: W, params: W, dir: W, show: i32) -> usize {
        let o = |p: W, cap: usize| if p.is_null() { None } else { Some(w2a(p, cap)) };
        let (v, f, p, d) = (o(verb, 32), w2a(file, MAXP * 2), o(params, 1024), o(dir, MAXP));
        exec(h, v.as_deref(), &f, p.as_deref(), d.as_deref(), show, None)
    }
    fn ShellExecuteExA(e: *mut Sei) -> i32 { sei_exec(e, false) }
    fn ShellExecuteExW(e: *mut Sei) -> i32 { sei_exec(e, true) }
    fn FindExecutableA(f: A, d: A, out: *mut u8) -> usize { *out = 0; 31 }
    fn FindExecutableW(f: W, d: W, out: *mut u16) -> usize { *out = 0; 31 }
    fn SHCreateDirectoryExA(h: H, p: A, sa: *mut c_void) -> i32 {
        let path = astr(p);
        if !node(path).is_null() { return 183; }
        mkdirs(path);
        if node(path).is_null() { 3 } else { 0 }
    }
    fn SHCreateDirectoryExW(h: H, p: W, sa: *mut c_void) -> i32 { let a = cz(&w2a(p, MAXP)); SHCreateDirectoryExA(h, a.as_ptr(), sa) }
    fn SHCreateDirectory(h: H, p: W) -> i32 { SHCreateDirectoryExW(h, p, core::ptr::null_mut()) }
    fn SHFileOperationA(o: *mut Shfos) -> i32 {
        (*o).aborted = 0;
        let to = if (*o).to.is_null() { Vec::new() } else { astr((*o).to as A).to_vec() };
        file_op((*o).func, &path_list((*o).from as A), &to)
    }
    fn SHFileOperationW(o: *mut Shfos) -> i32 {
        let w = (*o).from as W;
        let mut list = Vec::new();
        if !w.is_null() {
            let mut q = w;
            loop { let s = ws(q); if s.is_empty() { break; } list.push(s.iter().map(|&c| if c < 256 { c as u8 } else { b'?' }).collect::<Vec<u8>>()); q = q.add(s.len() + 1); }
        }
        let to = if (*o).to.is_null() { Vec::new() } else { w2a((*o).to as W, MAXP) };
        (*o).aborted = 0;
        file_op((*o).func, &list, &to)
    }
    fn SHGetFileInfoA(path: A, attr: u32, fi: *mut SfiA, n: u32, fl: u32) -> usize {
        let p: Vec<u8> = if fl & 0x8 != 0 { pidl_path(path).to_vec() } else { astr(path).to_vec() };   // SHGFI_PIDL
        let v = if fl & 0x10 != 0 { core::ptr::null_mut() } else { node(&p) };                       // SHGFI_USEFILEATTRIBUTES
        let dir = if v.is_null() { attr & 0x10 != 0 } else { (*v).dir != 0 };
        if v.is_null() && fl & 0x10 == 0 { return 0; }
        if !fi.is_null() {
            core::ptr::write_bytes(fi as *mut u8, 0, core::mem::size_of::<SfiA>());
            put_a(&basename_of(&p), (*fi).name.as_mut_ptr(), 260);
            put_a(&u::file_type_name(&p, dir), (*fi).ty.as_mut_ptr(), 80);
            (*fi).attr = if dir { 0x2000_0000 | 0x4000_0000 } else { 0x4000_0000 };
            (*fi).iicon = if dir { 3 } else { 0 };
        }
        if fl & 0x2000 != 0 {                              // SHGFI_EXETYPE
            let e = p.iter().rposition(|&c| c == b'.').map(|d| &p[d..]);
            return if e.is_some_and(|e| e.eq_ignore_ascii_case(b".exe")) { 0x4550 | (0x0400 << 16) } else { 0 };
        }
        1
    }
    fn SHGetFileInfoW(path: W, attr: u32, fi: *mut SfiW, n: u32, fl: u32) -> usize {
        let a = cz(&if fl & 0x8 != 0 { pidl_path(path as *const u8)[..pidl_path(path as *const u8).len().min(MAXP - 1)].to_vec() } else { w2a(path, MAXP) });
        let mut x: SfiA = core::mem::zeroed();
        let r = SHGetFileInfoA(a.as_ptr(), attr, if fi.is_null() { core::ptr::null_mut() } else { &mut x }, core::mem::size_of::<SfiA>() as u32, fl & !0x8);
        if !fi.is_null() && r != 0 {
            (*fi).icon = x.icon; (*fi).iicon = x.iicon; (*fi).attr = x.attr;
            a2w(astr(x.name.as_ptr()), (*fi).name.as_mut_ptr(), 260); a2w(astr(x.ty.as_ptr()), (*fi).ty.as_mut_ptr(), 80);
        }
        r
    }
    fn ExtractIconA(h: H, f: A, i: u32) -> H { 0 }
    fn ExtractIconW(h: H, f: W, i: u32) -> H { 0 }
    fn ExtractIconExW(f: W, i: i32, l: *mut H, s: *mut H, n: u32) -> u32 { if !l.is_null() { *l = 0; } if !s.is_null() { *s = 0; } 0 }
    fn ExtractIconExA(f: A, i: i32, l: *mut H, s: *mut H, n: u32) -> u32 { if !l.is_null() { *l = 0; } if !s.is_null() { *s = 0; } 0 }
    fn DragAcceptFiles(h: H, a: i32) -> () {}
    fn DragQueryFileA(d: H, i: u32, b: *mut u8, n: u32) -> u32 { 0 }
    fn DragQueryFileW(d: H, i: u32, b: *mut u16, n: u32) -> u32 { 0 }
    fn DragQueryPoint(d: H, pt: *mut c_void) -> i32 { 0 }
    fn DragFinish(d: H) -> () {}
    fn IsUserAnAdmin() -> i32 { 1 }
    fn SHAddToRecentDocs(f: u32, p: *const c_void) -> () {}
    fn SHChangeNotify(e: i32, f: u32, a: *const c_void, b: *const c_void) -> () {}
    fn SHAppBarMessage(msg: u32, d: *mut u8) -> usize {
        if msg == 5 && !d.is_null() {                      // ABM_GETTASKBARPOS: bottom edge
            let ps = core::mem::size_of::<usize>();
            w32_taskbar_rect(d.add(4 + 2 * ps + 4) as *mut i32);
            *(d.add(4 + 2 * ps) as *mut u32) = 3;
            return 1;
        }
        if msg == 4 { 2 } else { 0 }                       // ABM_GETSTATE: always on top
    }
    fn Shell_NotifyIconW(m: u32, d: *mut c_void) -> i32 { 1 }
    fn Shell_NotifyIconA(m: u32, d: *mut c_void) -> i32 { 1 }
    fn ShellAboutW(h: H, app: W, other: W, icon: H) -> i32 {
        let a = w2a(app, 200); let o = w2a(other, 300);
        let (name, title) = match a.iter().position(|&c| c == b'#') { Some(i) => (&a[..i], &a[i + 1..]), None => (&a[..], &a[..]) };
        let mut t = title.to_vec(); t.push(b'\n'); t.extend_from_slice(OS_NAME.as_bytes()); t.extend_from_slice(b"\n\n"); t.extend_from_slice(&o); t.truncate(599);
        let mut cap = b"About ".to_vec(); cap.extend_from_slice(name); cap.truncate(219);
        let f = lookup(c"user32.dll", c"MessageBoxA");
        if f.is_null() { return 1; }
        let f: winfn!(fn(H, A, A, u32) -> i32) = core::mem::transmute(f);
        f(h, cz(&t).as_ptr(), cz(&cap).as_ptr(), 0x40)
    }
    fn ShellAboutA(h: H, app: A, other: A, icon: H) -> i32 {
        let a = wz(&widen(&astr(app)[..astr(app).len().min(199)])); let o = wz(&widen(&astr(other)[..astr(other).len().min(299)]));
        ShellAboutW(h, a.as_ptr(), o.as_ptr(), icon)
    }
    fn SetCurrentProcessExplicitAppUserModelID(id: W) -> i32 { S_OK }
    fn SHGetStockIconInfo(id: i32, f: u32, s: *mut c_void) -> i32 { E_INVALIDARG }
    fn PickIconDlg(h: H, f: *mut u16, n: u32, i: *mut i32) -> i32 { 0 }
    fn SHOpenFolderAndSelectItems(pidl: *const u8, n: u32, a: *const *const c_void, f: u32) -> i32 {
        let p = cz(pidl_path(pidl));
        shell_execute_a(0, b"open\0".as_ptr(), p.as_ptr(), core::ptr::null(), core::ptr::null(), 1);
        S_OK
    }

    // ================================================================ version.dll
    fn GetFileVersionInfoSizeA(f: A, h: P) -> u32 {
        if !h.is_null() { *h = 0; }
        match version_resource(astr(f)) { Some(r) => r.len() as u32 * 2 + 4, None => { set_err(1813); 0 } }
    }
    fn GetFileVersionInfoSizeW(f: W, h: P) -> u32 { let a = cz(&w2a(f, MAXP)); GetFileVersionInfoSizeA(a.as_ptr(), h) }
    fn GetFileVersionInfoSizeExW(fl: u32, f: W, h: P) -> u32 { GetFileVersionInfoSizeW(f, h) }
    fn GetFileVersionInfoA(f: A, h: u32, n: u32, b: *mut u8) -> i32 {
        let Some(r) = version_resource(astr(f)) else { set_err(1813); return 0 };
        core::ptr::write_bytes(b, 0, n as usize);
        core::ptr::copy_nonoverlapping(r.as_ptr(), b, r.len().min(n as usize));
        1
    }
    fn GetFileVersionInfoW(f: W, h: u32, n: u32, b: *mut u8) -> i32 { let a = cz(&w2a(f, MAXP)); GetFileVersionInfoA(a.as_ptr(), h, n, b) }
    fn GetFileVersionInfoExW(fl: u32, f: W, h: u32, n: u32, b: *mut u8) -> i32 { GetFileVersionInfoW(f, h, n, b) }
    fn VerQueryValueW(blk: *const u8, sub: W, out: *mut *const u8, len: P) -> i32 {
        let Some((v, vl, _)) = (if sub.is_null() { None } else { ver_query(blk, ws(sub)) }) else {
            if !out.is_null() { *out = core::ptr::null(); } if !len.is_null() { *len = 0; } return 0
        };
        if !out.is_null() { *out = blk.add(v); } if !len.is_null() { *len = vl as u32; }
        1
    }
    fn VerQueryValueA(blk: *const u8, sub: A, out: *mut *const u8, len: P) -> i32 {
        let w = widen(&astr(sub)[..if sub.is_null() { 0 } else { astr(sub).len().min(255) }]);
        let Some((v, vl, text)) = (if sub.is_null() { None } else { ver_query(blk, &w) }) else {
            if !out.is_null() { *out = core::ptr::null(); } if !len.is_null() { *len = 0; } return 0
        };
        if text {                                          // an ANSI copy, kept in the second half of the caller's block
            let total = *(blk as *const u16) as usize;
            let dst = (blk as *mut u8).add(total + 4 + v / 2);
            let ws_ = blk.add(v) as *const u16;
            for i in 0..vl { let c = core::ptr::read_unaligned(ws_.add(i)); *dst.add(i) = if c < 256 { c as u8 } else { b'?' }; }
            if !out.is_null() { *out = dst; }
        } else if !out.is_null() { *out = blk.add(v); }
        if !len.is_null() { *len = vl as u32; }
        1
    }
    fn VerLanguageNameA(l: u32, b: *mut u8, n: u32) -> u32 { put_a(if l == 0x409 { b"English (United States)" } else { b"Language Neutral" }, b, n as usize) as u32 }
    fn VerLanguageNameW(l: u32, b: *mut u16, n: u32) -> u32 { a2w(if l == 0x409 { b"English (United States)" } else { b"Language Neutral" }, b, n as usize) as u32 }

    // ================================================================ winmm
    fn timeGetTime() -> u32 { timer_ticks() }
    fn timeBeginPeriod(p: u32) -> u32 { 0 }
    fn timeEndPeriod(p: u32) -> u32 { 0 }
    fn timeGetDevCaps(caps: *mut u32, n: u32) -> u32 { if n >= 8 { *caps = 1; *caps.add(1) = 1_000_000; } 0 }
    fn timeGetSystemTime(mmt: *mut u32, n: u32) -> u32 { if n >= 8 { *mmt = 1; *mmt.add(1) = timer_ticks(); } 0 }
    fn mmtimer_thread(arg: *mut MmTimer) -> u32 {
        let t = arg;
        loop {
            task_sleep((*t).delay.max(1));
            if core::ptr::read_volatile(&(*t).stop) { break; }
            if (*t).flags & 0x10 != 0 { set_event((*t).f as H); }                  // TIME_CALLBACK_EVENT_SET
            else { let f: winfn!(fn(u32, u32, usize, usize, usize) -> ()) = core::mem::transmute((*t).f); f((*t).id, 0, (*t).user, 0, 0); }
            if (*t).flags & 1 == 0 || core::ptr::read_volatile(&(*t).stop) { break; }   // TIME_PERIODIC
        }
        0
    }
    fn timeSetEvent(delay: u32, res: u32, f: *const c_void, user: usize, flags: u32) -> u32 {
        let tm = timers();
        let Some(slot) = tm.iter().position(|t| t.is_null()) else { return 0 };
        if f.is_null() { return 0; }
        let t = Box::into_raw(Box::new(MmTimer { delay, f, user, flags, id: slot as u32 + 1, stop: false }));
        tm[slot] = t;
        create_thread(mmtimer_thread as *const c_void, t as *mut c_void);
        slot as u32 + 1
    }
    fn timeKillEvent(id: u32) -> u32 {
        let tm = timers();
        if id == 0 || id > 16 || tm[id as usize - 1].is_null() { return 97; }
        core::ptr::write_volatile(&mut (*tm[id as usize - 1]).stop, true);  // the thread still uses it; it stays allocated
        tm[id as usize - 1] = core::ptr::null_mut();
        0
    }
    fn PlaySoundA(n: A, m: usize, f: u32) -> i32 {
        if f & 0x40 != 0 { audio_stop_all(); return 1; }
        if f & 0x40004 == 0x40004 && !n.is_null() {        // a WAVE resource of the module
            let r = w32_find_resource_in(w32_cur(), m, b"WAVE\0".as_ptr() as usize, n as usize, 0, 0);
            let mut sz = 0u32;
            let d = if r != 0 { w32_resource_data(w32_cur(), r, &mut sz) } else { core::ptr::null_mut() };
            return if d.is_null() { 0 } else { play(None, d, sz, f) };
        }
        if f & 4 != 0 { play(None, n, 0, f) } else { play(if n.is_null() { None } else { Some(astr(n)) }, core::ptr::null(), 0, f) }
    }
    fn PlaySoundW(n: W, m: usize, f: u32) -> i32 {
        if f & 0x40 != 0 { audio_stop_all(); return 1; }
        if f & 4 != 0 || (n as usize) < 0x10000 { return PlaySoundA(n as A, m, f); }
        let a = cz(&w2a(n, MAXP)); PlaySoundA(a.as_ptr(), m, f)
    }
    fn sndPlaySoundA(n: A, f: u32) -> i32 { PlaySoundA(n, 0, (f & 0xF) | if n.is_null() { 0 } else { 0x20000 }) }
    fn sndPlaySoundW(n: W, f: u32) -> i32 { PlaySoundW(n, 0, (f & 0xF) | if n.is_null() { 0 } else { 0x20000 }) }
    fn wout_thread(arg: *mut WOut) -> u32 {
        let w = arg;
        while (*w).open {
            if (*w).paused || (*w).q.is_empty() { task_sleep(5); continue; }
            let h = (&(*w).q)[0];
            wout_push(w, h);
            if !(*w).open { break; }
            (*w).q.remove(0);
            (*h).flags = ((*h).flags & !WHDR_INQUEUE) | WHDR_DONE;
            wout_notify(w, 0x3BD, h as usize);              // WOM_DONE
        }
        0
    }
    fn waveOutGetNumDevs() -> u32 { (audio_present() != 0) as u32 }
    fn waveOutGetDevCapsW(id: usize, c: *mut WocW, n: u32) -> u32 {
        if audio_present() == 0 { return 2; }
        core::ptr::write_bytes(c as *mut u8, 0, (n as usize).min(core::mem::size_of::<WocW>()));
        (*c).mid = 1; (*c).pid = 1; (*c).ver = 0x100; a2w(b"Speakers", (*c).name.as_mut_ptr(), 32); (*c).formats = 0xFFFFF; (*c).ch = 2; (*c).support = 0x2C;
        0
    }
    fn waveOutGetDevCapsA(id: usize, c: *mut WocA, n: u32) -> u32 {
        if audio_present() == 0 { return 2; }
        core::ptr::write_bytes(c as *mut u8, 0, (n as usize).min(core::mem::size_of::<WocA>()));
        (*c).mid = 1; (*c).pid = 1; (*c).ver = 0x100; put_a(b"Speakers", (*c).name.as_mut_ptr(), 32); (*c).formats = 0xFFFFF; (*c).ch = 2; (*c).support = 0x2C;
        0
    }
    fn waveOutOpen(out: *mut usize, id: u32, f: *const Wfx, cb: usize, inst: usize, fl: u32) -> u32 {
        if audio_present() == 0 { return 6; }              // MMSYSERR_NODRIVER
        if f.is_null() { return 32; }
        let fmt = core::ptr::read_unaligned(f);
        if (fmt.tag != 1 && fmt.tag != 0xFFFE) || (fmt.bits != 8 && fmt.bits != 16) || fmt.ch == 0 || fmt.ch > 2 { return 32; }   // WAVERR_BADFORMAT
        if fl & 1 != 0 { return 0; }                       // WAVE_FORMAT_QUERY
        let stream = audio_stream_open();
        if stream < 0 { return 4; }                        // MMSYSERR_ALLOCATED
        let w = Box::into_raw(Box::new(WOut { stream, fmt, cb, inst, flags: fl, q: Vec::with_capacity(64), played: 0, paused: false, open: true, thread: 0 }));
        if !out.is_null() { *out = w as usize; }
        (*w).thread = create_thread(wout_thread as *const c_void, w as *mut c_void);
        wout_notify(w, 0x3BB, 0);                          // WOM_OPEN
        0
    }
    fn waveOutPrepareHeader(h: usize, hd: *mut WaveHdr, n: u32) -> u32 { (*hd).flags |= WHDR_PREPARED; 0 }
    fn waveOutUnprepareHeader(h: usize, hd: *mut WaveHdr, n: u32) -> u32 { if (*hd).flags & WHDR_INQUEUE != 0 { return 33; } (*hd).flags &= !WHDR_PREPARED; 0 }
    fn waveOutWrite(h: usize, hd: *mut WaveHdr, n: u32) -> u32 {
        let w = h as *mut WOut;
        if w.is_null() || !(*w).open { return 5; }
        if (*hd).flags & WHDR_PREPARED == 0 { return 34; }
        while (*w).q.len() >= 64 && (*w).open { task_sleep(5); }
        (*hd).flags = ((*hd).flags & !WHDR_DONE) | WHDR_INQUEUE;
        (*w).q.push(hd);
        0
    }
    fn waveOutReset(h: usize) -> u32 {
        let w = h as *mut WOut;
        if w.is_null() { return 5; }
        while let Some(hd) = (*w).q.pop() { (*hd).flags = ((*hd).flags & !WHDR_INQUEUE) | WHDR_DONE; wout_notify(w, 0x3BD, hd as usize); }
        0
    }
    fn waveOutClose(h: usize) -> u32 {
        let w = h as *mut WOut;
        if w.is_null() { return 5; }
        if !(*w).q.is_empty() { return 33; }               // WAVERR_STILLPLAYING
        (*w).open = false;
        let f = k32(c"WaitForSingleObject");
        if !f.is_null() { let f: winfn!(fn(H, u32) -> u32) = core::mem::transmute(f); f((*w).thread, 500); }
        audio_stream_close((*w).stream);
        wout_notify(w, 0x3BC, 0);                          // WOM_CLOSE (the block stays: the thread may still look at it)
        0
    }
    fn waveOutPause(h: usize) -> u32 { let w = h as *mut WOut; if !w.is_null() { (*w).paused = true; } 0 }
    fn waveOutRestart(h: usize) -> u32 { let w = h as *mut WOut; if !w.is_null() { (*w).paused = false; } 0 }
    fn waveOutGetPosition(h: usize, t: *mut MmTime, n: u32) -> u32 {
        let w = h as *mut WOut;
        if w.is_null() { return 5; }
        let rate = audio_rate().max(1) as u64;
        let queued = audio_stream_queued((*w).stream).max(0) as u64 * (*w).fmt.rate as u64 / rate;
        let heard = (*w).played.saturating_sub(queued);
        match (*t).ty {
            1 => (*t).v = heard as u32,                    // TIME_SAMPLES
            2 => (*t).v = (heard * 1000 / ((*w).fmt.rate.max(1) as u64)) as u32,   // TIME_MS
            _ => { (*t).ty = 4; (*t).v = (heard * (*w).fmt.align as u64) as u32 }  // TIME_BYTES
        }
        0
    }
    fn waveOutGetVolume(h: usize, v: P) -> u32 { *v = 0xFFFF_FFFF; 0 }
    fn waveOutSetVolume(h: usize, v: u32) -> u32 { 0 }
    fn waveOutGetErrorTextA(e: u32, b: *mut u8, n: u32) -> u32 {
        let t = match e { 32 => alloc::string::String::from("The format is not supported."), 6 => "No sound driver is installed.".into(), _ => alloc::format!("Sound error {e}.") };
        put_a(t.as_bytes(), b, n as usize); 0
    }
    fn waveOutGetErrorTextW(e: u32, b: *mut u16, n: u32) -> u32 { let mut a = [0u8; 128]; waveOutGetErrorTextA(e, a.as_mut_ptr(), 128); a2w(astr(a.as_ptr()), b, n as usize); 0 }
    fn waveOutMessage(h: usize, m: u32, a: usize, b: usize) -> u32 { 8 }
    // no recording, MIDI, mixer or joystick devices
    fn waveInGetNumDevs() -> u32 { 0 }
    fn waveInGetDevCapsW(id: usize, c: *mut c_void, n: u32) -> u32 { 2 }
    fn waveInGetDevCapsA(id: usize, c: *mut c_void, n: u32) -> u32 { 2 }
    fn waveInOpen(h: *mut usize, id: u32, f: *const c_void, cb: usize, i: usize, fl: u32) -> u32 { 6 }
    fn waveIn_stub(h: usize) -> u32 { 5 }
    fn waveIn_stub3(h: usize, x: *mut c_void, n: u32) -> u32 { 5 }
    fn midiOutGetNumDevs() -> u32 { 0 }
    fn midiOutOpen(h: *mut usize, id: u32, cb: usize, i: usize, f: u32) -> u32 { 6 }
    fn midiInGetNumDevs() -> u32 { 0 }
    fn mixerGetNumDevs() -> u32 { 0 }
    fn mixerOpen(h: *mut usize, id: u32, cb: usize, i: usize, f: u32) -> u32 { 6 }
    fn auxGetNumDevs() -> u32 { 0 }
    fn joyGetNumDevs() -> u32 { 0 }
    fn joyGetPos(id: u32, p: *mut c_void) -> u32 { 167 }               // JOYERR_UNPLUGGED
    fn joyGetPosEx(id: u32, p: *mut c_void) -> u32 { 167 }
    fn joyGetDevCapsA(id: usize, c: *mut c_void, n: u32) -> u32 { 167 }
    fn joyGetDevCapsW(id: usize, c: *mut c_void, n: u32) -> u32 { 167 }
    fn mciSendStringA(cmd: A, ret: *mut u8, n: u32, cb: H) -> u32 { if !ret.is_null() && n != 0 { *ret = 0; } mci(astr(cmd)) }
    fn mciSendStringW(cmd: W, ret: *mut u16, n: u32, cb: H) -> u32 { let a = w2a(cmd, MAXP); if !ret.is_null() && n != 0 { *ret = 0; } mci(&a) }
    fn mciSendCommandW(id: u32, msg: u32, f: usize, p: usize) -> u32 { 263 }
    fn mciSendCommandA(id: u32, msg: u32, f: usize, p: usize) -> u32 { 263 }
    fn mciGetErrorStringA(e: u32, b: *mut u8, n: u32) -> i32 { put_a(b"The specified device is not open or is not recognized by MCI.", b, n as usize); 1 }
    fn mciGetErrorStringW(e: u32, b: *mut u16, n: u32) -> i32 { a2w(b"The specified device is not open or is not recognized by MCI.", b, n as usize); 1 }
}

unsafe fn wstrstr(h: W, n: W, ci: bool) -> W {
    let (hs, ns) = (ws(h), ws(n));
    if ns.is_empty() { return h; }
    for i in 0..hs.len() { if u::wcmpn(&hs[i..], ns, Some(ns.len()), ci) == 0 { return h.add(i); } }
    core::ptr::null()
}

api_table! { shell_api;
    "shlwapi": PathFindFileNameW, PathFindFileNameA, PathFindExtensionW, PathFindExtensionA, PathFileExistsA, PathFileExistsW, PathIsDirectoryA,
        PathIsDirectoryW, PathIsRelativeW, PathIsRelativeA, PathIsRootW, PathIsRootA, PathIsUNCW, PathIsUNCA, PathIsURLW, PathIsURLA, PathIsFileSpecW,
        PathGetDriveNumberW, PathGetDriveNumberA, PathAddBackslashW, PathAddBackslashA, PathRemoveBackslashW, PathRemoveBackslashA, PathRemoveFileSpecW,
        PathRemoveFileSpecA, PathRemoveExtensionW, PathRemoveExtensionA, PathRenameExtensionW, PathRenameExtensionA, PathAddExtensionW, PathStripPathW,
        PathStripPathA, PathStripToRootW, PathStripToRootA, PathSkipRootW, PathSkipRootA, PathFindNextComponentW, PathCanonicalizeW, PathCanonicalizeA,
        PathCombineW, PathCombineA, PathAppendW, PathAppendA, PathCommonPrefixW, PathIsPrefixW, PathIsSameRootW, PathRelativePathToW, PathMatchSpecW,
        PathMatchSpecA, PathQuoteSpacesW, PathUnquoteSpacesW, PathUnquoteSpacesA, PathUnExpandEnvStringsW, PathCompactPathExW, PathCompactPathExA,
        SHAutoComplete, IsOS, IsOS = "Ordinal437",
        StrChrW, StrChrA, StrChrIW, StrChrIA, StrRChrW, StrRChrA, StrCmpW, StrCmpIW, StrCmpNW, StrCmpNIW, StrCmpNA, StrCmpNIA, StrCmpIA, StrCmpLogicalW,
        StrCpyW, StrCpyNW, StrCpyNA, StrCatW, StrNCatW, StrNCatA, StrCatBuffW, StrCatBuffA, StrDupW, StrDupA, StrStrW, StrStrIW, StrStrA, StrStrIA,
        StrTrimW, StrTrimA, StrToIntW, StrToIntA, StrToIntExW, StrToIntExA, StrFormatByteSizeW, StrFormatByteSizeA, StrFormatByteSize64A, StrFormatKBSizeW,
        StrRetToBufW, StrRetToStrW, SHStrDupW, SHStrDupA, UrlEscapeW, UrlUnescapeW, SHGetObjectCompatFlags, SHCreateStreamOnFileW, SHCreateStreamOnFileEx,
        SHAnsiToUnicode, SHUnicodeToAnsi, ColorHLSToRGB, ColorRGBToHLS;
    // shell32 (some of these lived in shlwapi too, and Windows 7 forwards them)
    "shell32": SHGetFolderPathA, SHGetFolderPathW, SHGetFolderPathAndSubDirW, SHGetSpecialFolderPathA, SHGetSpecialFolderPathW, SHGetKnownFolderPath,
        SHGetSpecialFolderLocation, SHGetFolderLocation, SHGetKnownFolderIDList, SHGetPathFromIDListA, SHGetPathFromIDListW, SHGetPathFromIDListEx,
        ILCreateFromPathW, ILCreateFromPathA, ILFree, ILClone, ILGetSize, ILFree = "Ordinal155", ILClone = "Ordinal18", ILGetSize = "Ordinal152",
        ILCreateFromPathW = "Ordinal157", ILFree = "SHFree", ILFree = "Ordinal195", SHParseDisplayName, SHGetDataFromIDListW, SHGetMalloc,
        SHGetDesktopFolder, SHBrowseForFolderA, SHBrowseForFolderW, SHBrowseForFolderA = "SHBrowseForFolder", CommandLineToArgvW, ShellExecuteW,
        ShellExecuteExA, ShellExecuteExW, FindExecutableA, FindExecutableW, SHCreateDirectoryExA, SHCreateDirectoryExW, SHCreateDirectory,
        SHFileOperationA, SHFileOperationW, SHGetFileInfoA, SHGetFileInfoW, ExtractIconA, ExtractIconW, ExtractIconExW, ExtractIconExA, DragAcceptFiles,
        DragQueryFileA, DragQueryFileW, DragQueryPoint, DragFinish, IsUserAnAdmin, IsUserAnAdmin = "Ordinal680", SHAddToRecentDocs, SHChangeNotify,
        SHAppBarMessage, Shell_NotifyIconW, Shell_NotifyIconA, ShellAboutW, ShellAboutA, SetCurrentProcessExplicitAppUserModelID, SHGetStockIconInfo,
        PickIconDlg, SHOpenFolderAndSelectItems, DragQueryFileA = "DragQueryFile", SHGetFileInfoA = "SHGetFileInfo", ShellExecuteExA = "ShellExecuteEx";
    "ole32": CoGetMalloc2 = "CoGetMalloc";
    "shlwapi": SHGetFolderPathW, SHGetFolderPathA;
    "version": GetFileVersionInfoSizeA, GetFileVersionInfoSizeW, GetFileVersionInfoSizeExW, GetFileVersionInfoA, GetFileVersionInfoW, GetFileVersionInfoExW,
        VerQueryValueA, VerQueryValueW, VerLanguageNameA, VerLanguageNameW;
    "kernel32": VerLanguageNameA, VerLanguageNameW;
    "winmm": timeGetTime, timeBeginPeriod, timeEndPeriod, timeGetDevCaps, timeGetSystemTime, timeSetEvent, timeKillEvent, PlaySoundA, PlaySoundW,
        PlaySoundA = "PlaySound", sndPlaySoundA, sndPlaySoundW, waveOutGetNumDevs, waveOutGetDevCapsW, waveOutGetDevCapsA, waveOutOpen,
        waveOutPrepareHeader, waveOutUnprepareHeader, waveOutWrite, waveOutReset, waveOutClose, waveOutPause, waveOutRestart, waveOutGetPosition,
        waveOutGetVolume, waveOutSetVolume, waveOutGetErrorTextA, waveOutGetErrorTextW, waveOutMessage, waveInGetNumDevs, waveInGetDevCapsW,
        waveInGetDevCapsA, waveInOpen, waveIn_stub = "waveInClose", waveIn_stub = "waveInStart", waveIn_stub = "waveInStop", waveIn_stub = "waveInReset",
        waveIn_stub3 = "waveInAddBuffer", waveIn_stub3 = "waveInPrepareHeader", waveIn_stub3 = "waveInUnprepareHeader", midiOutGetNumDevs, midiOutOpen,
        midiInGetNumDevs, mixerGetNumDevs, mixerOpen, auxGetNumDevs, joyGetNumDevs, joyGetPos, joyGetPosEx, joyGetDevCapsA, joyGetDevCapsW,
        mciSendStringA, mciSendStringW, mciSendCommandW, mciSendCommandA, mciGetErrorStringA, mciGetErrorStringW;
}
