//! /dev/wrld/window (was src/linux/wrldwin.c): a desktop window for a Linux program.
//!
//! The program opens the device, creates a window with an ioctl, mmaps the window's pixels
//! (a shared object, 32-bit 0xAARRGGBB, what Cairo's ARGB32 and pixman use) and draws into
//! them, then presents a frame with another ioctl. Mouse, keyboard, resize and close events
//! are read from the same descriptor, which works with poll and epoll. This is the display
//! path a browser engine's backend (or a small Wayland compositor) uses.
//!
//! Shared with programs through samples/linux-gnome/wrldwin.h. The window manager's
//! window_t is mirrored up to its `data` field; rust_init checks those offsets at boot.

use super::*;
use super::mem::*;
use super::sig::interrupted;
use core::ptr::{self, null_mut};

/// canvas_t
#[repr(C)]
pub struct Canvas { px: *mut u32, w: c_int, h: c_int, stride: c_int, clip: [c_int; 4] }
/// key_event_t
#[repr(C)]
pub struct KeyEvent { key: c_int, ch: c_char, pressed: CBool, mods: c_int }
type WinFn = Option<unsafe extern "C" fn(*mut Window)>;
/// window_t, up to `data` (src/include/wm.h).
#[repr(C)]
pub struct Window {
    id: c_int,
    title: [c_char; 96],
    icon: c_int,
    x: c_int, y: c_int, w: c_int, h: c_int,
    min_w: c_int, min_h: c_int,
    flags: c_int, state: c_int,
    restore: [c_int; 4],
    surf: *mut Canvas,
    pid: c_int,
    dirty: CBool,
    menus: *mut c_void,
    nmenus: c_int,
    paint: WinFn,
    mouse: Option<unsafe extern "C" fn(*mut Window, c_int, c_int, c_int, c_int)>,
    key: Option<unsafe extern "C" fn(*mut Window, *mut KeyEvent)>,
    close: Option<unsafe extern "C" fn(*mut Window) -> CBool>,
    menu_cmd: Option<unsafe extern "C" fn(*mut Window, c_int)>,
    resized: WinFn,
    timer: WinFn,
    destroy: WinFn,
    activate: Option<unsafe extern "C" fn(*mut Window, CBool)>,
    timer_ms: u32, timer_next: u32,
    data: *mut c_void,
}
pub fn wlayout() -> [u32; 5] {
    use core::mem::{offset_of, size_of};
    [offset_of!(Window, surf) as u32, offset_of!(Window, paint) as u32, offset_of!(Window, data) as u32,
     size_of::<Canvas>() as u32, size_of::<KeyEvent>() as u32]
}

#[allow(clashing_extern_declarations)]
extern "C" {
    fn wm_create(title: *const c_char, x: c_int, y: c_int, cw: c_int, ch: c_int, flags: c_int) -> *mut Window;
    fn wm_destroy(w: *mut Window);
    fn wm_alive(w: *mut Window) -> CBool;
    fn wm_center(w: *mut Window);
    fn wm_set_title(w: *mut Window, t: *const c_char);
    fn wm_resize_client(w: *mut Window, cw: c_int, ch: c_int);
    fn wm_begin_move(w: *mut Window);
    fn wm_begin_resize(w: *mut Window, edges: c_int);
    fn wm_minimize(w: *mut Window);
    fn wm_maximize(w: *mut Window, on: CBool);
    fn gfx_clear(c: *mut Canvas, color: u32);
    fn shell_open_later(path: *const c_char);
    fn clip_set_text(s: *const c_char);
    fn clip_text() -> *const c_char;
    fn clip_gen() -> u32;
    static SCREEN_W: c_int;
    static SCREEN_H: c_int;
}

const WIN_CREATE: u64 = 0x57A1;      /* struct { int32 width, height; uint32 flags; char title[100]; } */
const WIN_PRESENT: u64 = 0x57A2;     /* the buffer holds a new frame */
const WIN_RESIZE: u64 = 0x57A3;      /* struct { int32 width, height; }: resize the buffer (then mmap again) */
const WIN_TITLE: u64 = 0x57A4;       /* char title[100] */
const WIN_INFO: u64 = 0x57A5;        /* struct { int32 width, height, buf_width, buf_height; } */
const WIN_SETSIZE: u64 = 0x57A6;     /* struct { int32 width, height; }: resize the window and its buffer */
const WIN_ACTION: u64 = 0x57A7;      /* 1 move, 2 | edges << 8 resize, 3 minimise, 4 maximise, 5 restore */
const WIN_SCREEN: u64 = 0x57A8;      /* struct { int32 screen_w, screen_h, maximized, minimized; }: works before CREATE too */
const WIN_OPEN: u64 = 0x57A9;        /* char path[512] (a Linux path): THE WRLD OS opens it, as a double-click would */
const WIN_CLIP_SET: u64 = 0x57AA;    /* struct { uint64 text; uint32 len; }: the system clipboard's text */
const WIN_CLIP_GET: u64 = 0x57AB;    /* struct { uint64 buf; uint32 cap; uint32 gen; }: copies it, returns its length */
const WIN_DROP_GET: u64 = 0x57AC;    /* struct { uint64 buf; uint32 cap; }: the files of the last drop, as a text/uri-list */
const WS_MINIMIZED: c_int = 1; const WS_MAXIMIZED: c_int = 2;
const WINF_RESIZABLE: u32 = 1;
const WINF_FRAMELESS: u32 = 2;       /* the program draws its own frame; pixels with alpha 0 are see-through */
const WF_RESIZABLE: c_int = 0x001;
const WF_SKINNED: c_int = 0x200;
const ICON_EXE: c_int = 13;
const ICON_BROWSER: c_int = 64;
const ME_MOVE: c_int = 0; const ME_DOWN: c_int = 1; const ME_UP: c_int = 2; const ME_DBLCLICK: c_int = 3; const ME_WHEEL: c_int = 4; const ME_LEAVE: c_int = 5;
const MB_LEFT: c_int = 1; const MB_RIGHT: c_int = 2; const MB_MIDDLE: c_int = 4;

const WEV_MOTION: u32 = 1; const WEV_BUTTON_DOWN: u32 = 2; const WEV_BUTTON_UP: u32 = 3; const WEV_WHEEL: u32 = 4;
const WEV_KEY_DOWN: u32 = 5; const WEV_KEY_UP: u32 = 6; const WEV_RESIZE: u32 = 7; const WEV_CLOSE: u32 = 8; const WEV_LEAVE: u32 = 9; const WEV_ACTIVE: u32 = 10;
const WEV_DROP: u32 = 11;            /* files dropped on the window at x, y: WIN_DROP_GET has them */
#[repr(C)] #[derive(Clone, Copy, Default)] struct Wev { ty: u32, x: i32, y: i32, code: u32, ch: u32, mods: u32 }
const WQ: usize = 256;

#[repr(C)]
struct LxWin {
    w: *mut Window,
    fb: *mut ShmObj,
    bw: c_int, bh: c_int,                            /* buffer size */
    q: [Wev; WQ], head: usize, tail: usize,
    closed: bool,                                    /* the user closed the window */
    frameless: bool,
    frames: u32,
    owner: c_int,                                    /* pid of the process that created the window */
    next: *mut LxWin,
    drop_list: *mut u8, drop_len: usize,             /* the last drop, as a text/uri-list (kmalloc) */
}
static mut ALL_WINS: *mut LxWin = null_mut();
/// The WebKit browser's compositor (src/apps/webkit.c).
#[no_mangle]
pub static mut lxwin_browser_pid: c_int = 0;
/// The desktop window a Linux process created, if it still has one.
#[no_mangle]
pub unsafe extern "C" fn lxwin_window_of(pid: c_int) -> *mut Window {
    let mut l = ALL_WINS;
    while !l.is_null() {
        if (*l).owner == pid && !(*l).w.is_null() && !(*l).closed && wm_alive((*l).w) != 0 { return (*l).w; }
        l = (*l).next;
    }
    null_mut()
}

unsafe fn push(lw: *mut LxWin, ty: u32, x: i32, y: i32, code: u32, ch: u32, mods: u32) {
    let l = &mut *lw;
    let nt = (l.tail + 1) % WQ;
    if nt == l.head {                                    /* full: drop the oldest motion */
        if ty == WEV_MOTION { return; }
        l.head = (l.head + 1) % WQ;
    }
    if ty == WEV_MOTION && l.head != l.tail {            /* coalesce consecutive moves */
        let last = &mut l.q[(l.tail + WQ - 1) % WQ];
        if last.ty == WEV_MOTION { last.x = x; last.y = y; return; }
    }
    l.q[l.tail] = Wev { ty, x, y, code, ch, mods };
    l.tail = nt;
}

/* ---- the desktop side ---- */
const BG: u32 = 0xFF20_2020;
unsafe extern "C" fn win_paint(w: *mut Window) {
    let lw = (*w).data as *mut LxWin;
    let c = (*w).surf;
    if lw.is_null() || (*lw).fb.is_null() || (*lw).bw == 0 { gfx_clear(c, BG); return; }
    let cw = (*c).w.min((*lw).bw);
    let ch = (*c).h.min((*lw).bh);
    let stride = (*lw).bw as u64 * 4;
    for y in 0..ch {
        let dst = (*c).px.add(y as usize * (*c).stride as usize);
        let off = y as u64 * stride;
        let mut x = 0;
        while x < cw {
            let at = off + x as u64 * 4;
            let pg = shm_page((*lw).fb, at / PAGE, false);
            let run = (cw - x).min(((PAGE - (at & (PAGE - 1))) / 4) as c_int);
            if !pg.is_null() {
                let src = pg.add((at & (PAGE - 1)) as usize) as *const u32;
                for i in 0..run as usize {
                    let s = *src.add(i);
                    *dst.add(x as usize + i) = if (*lw).frameless { if s >> 24 != 0 { s | 0xFF00_0000 } else { 0 } } else { s | 0xFF00_0000 };
                }
            } else { for i in 0..run as usize { *dst.add(x as usize + i) = 0xFF00_0000; } }
            x += run;
        }
        for i in cw..(*c).w { *dst.add(i as usize) = BG; }
    }
    for y in ch..(*c).h { for i in 0..(*c).w { *(*c).px.add(y as usize * (*c).stride as usize + i as usize) = BG; } }
}
fn button(btn: c_int) -> u32 { (if btn == MB_LEFT { 1 } else if btn == MB_RIGHT { 3 } else if btn == MB_MIDDLE { 2 } else { btn }) as u32 }
unsafe extern "C" fn win_mouse(w: *mut Window, ty: c_int, x: c_int, y: c_int, btn: c_int) {
    let lw = (*w).data as *mut LxWin;
    if lw.is_null() { return; }
    match ty {
        ME_MOVE => push(lw, WEV_MOTION, x, y, 0, 0, 0),
        ME_DOWN | ME_DBLCLICK => push(lw, WEV_BUTTON_DOWN, x, y, button(btn), 0, 0),
        ME_UP => push(lw, WEV_BUTTON_UP, x, y, button(btn), 0, 0),
        ME_WHEEL => push(lw, WEV_WHEEL, x, y, btn as u32, 0, 0),
        ME_LEAVE => push(lw, WEV_LEAVE, 0, 0, 0, 0, 0),
        _ => {}
    }
}
unsafe extern "C" fn win_key(w: *mut Window, e: *mut KeyEvent) {
    let lw = (*w).data as *mut LxWin;
    if lw.is_null() { return; }
    push(lw, if (*e).pressed != 0 { WEV_KEY_DOWN } else { WEV_KEY_UP }, 0, 0, (*e).key as u32, (*e).ch as u8 as u32, (*e).mods as u32);
}
/* the window became active (1) or not (0), minimized included: wrldcomp slows its frames then */
unsafe extern "C" fn win_activate(w: *mut Window, on: CBool) {
    let lw = (*w).data as *mut LxWin;
    if lw.is_null() { return; }
    push(lw, WEV_ACTIVE, 0, 0, (on != 0) as u32, 0, 0);
}
unsafe extern "C" fn win_resized(w: *mut Window) {
    let lw = (*w).data as *mut LxWin;
    if lw.is_null() { return; }
    push(lw, WEV_RESIZE, (*(*w).surf).w, (*(*w).surf).h, 0, 0, 0);
}
unsafe extern "C" fn win_close(w: *mut Window) -> CBool {
    let lw = (*w).data as *mut LxWin;
    if lw.is_null() || (*lw).closed { return 1; }       /* asked twice: close it anyway */
    (*lw).closed = true;
    push(lw, WEV_CLOSE, 0, 0, 0, 0, 0);
    0                                                    /* the program decides; it usually exits */
}
unsafe extern "C" fn win_destroy(w: *mut Window) { let lw = (*w).data as *mut LxWin; if !lw.is_null() { (*lw).w = null_mut(); } (*w).data = null_mut(); }

/* ---- the program side ---- */
pub unsafe fn lxwin_new() -> *mut c_void { znew::<LxWin>() as *mut c_void }
pub unsafe fn lxwin_release(o: *mut c_void) {
    let lw = o as *mut LxWin;
    if lw.is_null() { return; }
    if !(*lw).w.is_null() && wm_alive((*lw).w) != 0 { (*(*lw).w).data = null_mut(); wm_destroy((*lw).w); }
    let mut pp: *mut *mut LxWin = ptr::addr_of_mut!(ALL_WINS);
    while !(*pp).is_null() { if *pp == lw { *pp = (*lw).next; break; } pp = ptr::addr_of_mut!((**pp).next); }
    shm_unref((*lw).fb);
    kfree((*lw).drop_list as *mut c_void);
    zfree(lw);
}
unsafe fn set_buffer(lw: *mut LxWin, w: c_int, h: c_int) -> i64 {
    if !(1..=4096).contains(&w) || !(1..=4096).contains(&h) { return -EINVAL; }
    if (*lw).fb.is_null() { (*lw).fb = shm_new(Some(b"wrld-window")); if (*lw).fb.is_null() { return -ENOMEM; } }
    let r = shm_resize((*lw).fb, w as u64 * h as u64 * 4);
    if r != 0 { return r; }
    (*lw).bw = w; (*lw).bh = h;
    0
}
pub unsafe fn lxwin_ioctl(o: *mut c_void, cmd: u64, arg: u64, vm: *mut Uvm) -> i64 {
    let lw = o as *mut LxWin;
    match cmd {
        WIN_CREATE => {
            #[repr(C)] #[derive(Clone, Copy)] struct A { w: i32, h: i32, flags: u32, title: [u8; 100] }
            let mut a = A { w: 0, h: 0, flags: 0, title: [0; 100] };
            if !uin(vm, &mut a, arg, 112) { return -EFAULT; }
            a.title[99] = 0;
            if !(*lw).w.is_null() { return -EEXIST; }
            let r = set_buffer(lw, a.w, a.h); if r != 0 { return r; }
            (*lw).frameless = a.flags & WINF_FRAMELESS != 0;
            let title: &[u8] = if a.title[0] != 0 { &a.title } else { b"Linux program\0" };
            let w = wm_create(title.as_ptr() as *const c_char, -1, -1, a.w, a.h,
                              (if a.flags & WINF_RESIZABLE != 0 { WF_RESIZABLE } else { 0 }) | (if (*lw).frameless { WF_SKINNED } else { 0 }));
            if w.is_null() { return -ENOMEM; }
            (*w).data = lw as *mut c_void; (*w).paint = Some(win_paint); (*w).mouse = Some(win_mouse); (*w).key = Some(win_key);
            (*w).resized = Some(win_resized); (*w).close = Some(win_close); (*w).destroy = Some(win_destroy); (*w).activate = Some(win_activate);
            (*w).icon = ICON_EXE; (*w).min_w = 120; (*w).min_h = 80;
            let th = lx_current();
            (*lw).owner = if !th.is_null() && !(*th).proc_.is_null() { (*(*th).proc_).pid } else { 0 };
            if (*lw).owner != 0 && (*lw).owner == lxwin_browser_pid { (*w).icon = ICON_BROWSER; (*w).min_w = 480; (*w).min_h = 320; }
            (*lw).next = ALL_WINS; ALL_WINS = lw;
            (*lw).w = w;
            wm_center(w);
            (*w).dirty = 1;
            0
        }
        WIN_PRESENT => { if !(*lw).w.is_null() { (*(*lw).w).dirty = 1; (*lw).frames += 1; 0 } else { -ENODEV } }
        WIN_RESIZE => {
            let Some(s) = uget::<[i32; 2]>(vm, arg) else { return -EFAULT };
            let r = set_buffer(lw, s[0], s[1]);
            if r == 0 && !(*lw).w.is_null() { (*(*lw).w).dirty = 1; }
            r
        }
        WIN_SETSIZE => {
            let Some(s) = uget::<[i32; 2]>(vm, arg) else { return -EFAULT };
            let r = set_buffer(lw, s[0], s[1]);
            let w = (*lw).w;
            if r == 0 && !w.is_null() && ((*(*w).surf).w != s[0] || (*(*w).surf).h != s[1]) {
                let rs = (*w).resized;
                (*w).resized = None;                     /* the program asked: no resize event back */
                wm_resize_client(w, s[0], s[1]);
                (*w).resized = rs;
                (*w).dirty = 1;
            }
            r
        }
        WIN_ACTION => {
            let w = (*lw).w;
            if w.is_null() { return -ENODEV; }
            match arg & 0xFF {
                1 => wm_begin_move(w),
                2 => wm_begin_resize(w, ((arg >> 8) & 0xF) as c_int),
                3 => wm_minimize(w),
                4 => wm_maximize(w, 1),
                5 => wm_maximize(w, 0),
                _ => return -EINVAL,
            }
            0
        }
        WIN_TITLE => {
            let Some(mut t) = uget::<[u8; 100]>(vm, arg) else { return -EFAULT };
            t[99] = 0;
            if !(*lw).w.is_null() { wm_set_title((*lw).w, t.as_ptr() as *const c_char); }
            if !(*lw).w.is_null() { 0 } else { -ENODEV }
        }
        WIN_OPEN => {
            let Some(mut b) = uget::<[u8; 512]>(vm, arg) else { return -EFAULT };
            b[511] = 0;
            let th = lx_current();
            if th.is_null() { return -EINVAL; }
            let (_, mut wp) = super::fs::lx_path((*th).proc_, -100, cstr(&b));
            if super::proc_::resolve_w(&wp).is_null() { return -ENOENT; }
            wp.push(0);
            shell_open_later(wp.as_ptr() as *const c_char);
            0
        }
        WIN_CLIP_SET => {
            let Some(a) = uget::<[u64; 2]>(vm, arg) else { return -EFAULT };
            let n = (a[1] as u32).min(4 << 20) as usize;
            let mut t: alloc::vec::Vec<u8> = alloc::vec![0u8; n + 1];
            if n > 0 && !uin(vm, t.as_mut_ptr(), a[0], n as u64) { return -EFAULT; }
            t[n] = 0;
            clip_set_text(t.as_ptr() as *const c_char);
            0
        }
        WIN_CLIP_GET => {
            let Some(a) = uget::<[u64; 2]>(vm, arg) else { return -EFAULT };
            let gen = clip_gen();
            if !uput(vm, arg + 12, &gen) { return -EFAULT; }
            let t = clip_text();
            if t.is_null() { return 0; }
            let s = core::ffi::CStr::from_ptr(t).to_bytes();
            let cap = (a[1] as u32) as usize;
            let n = s.len().min(cap);
            if n > 0 && !uout(vm, a[0], s.as_ptr(), n as u64) { return -EFAULT; }
            s.len() as i64
        }
        WIN_DROP_GET => {
            let Some(a) = uget::<[u64; 2]>(vm, arg) else { return -EFAULT };
            let cap = (a[1] as u32) as usize;
            let n = (*lw).drop_len.min(cap);
            if n > 0 && !uout(vm, a[0], (*lw).drop_list, n as u64) { return -EFAULT; }
            (*lw).drop_len as i64
        }
        WIN_SCREEN => {
            /* the desktop's size now (it changes with the display settings) and the window's state,
             * so a compositor can tell its programs the real output size and that they are maximized */
            let w = (*lw).w;
            let st = if w.is_null() { 0 } else { (*w).state };
            let s: [i32; 4] = [SCREEN_W, SCREEN_H, (st == WS_MAXIMIZED) as i32, (st == WS_MINIMIZED) as i32];
            if uput(vm, arg, &s) { 0 } else { -EFAULT }
        }
        WIN_INFO => {
            let w = (*lw).w;
            let s: [i32; 4] = [if w.is_null() { 0 } else { (*(*w).surf).w }, if w.is_null() { 0 } else { (*(*w).surf).h }, (*lw).bw, (*lw).bh];
            if uput(vm, arg, &s) { 0 } else { -EFAULT }
        }
        _ => -ENOTTY,
    }
}
pub unsafe fn lxwin_buffer(o: *mut c_void) -> *mut ShmObj { (*(o as *mut LxWin)).fb }
pub unsafe fn lxwin_ready(o: *mut c_void) -> bool { let lw = o as *mut LxWin; (*lw).head != (*lw).tail || ((*lw).w.is_null() && (*lw).bw != 0) }
pub unsafe fn lxwin_read(o: *mut c_void, dst: *mut u8, n: u64, nonblock: bool) -> i64 {
    let lw = o as *mut LxWin;
    const SZ: u64 = core::mem::size_of::<Wev>() as u64;
    if n < SZ { return -EINVAL; }
    while (*lw).head == (*lw).tail {
        if (*lw).bw != 0 && (*lw).w.is_null() { return 0; }   /* the window is gone: end of file */
        if nonblock { return -EAGAIN; }
        if interrupted() { return -EINTR; }
        task_sleep(1);
    }
    let mut k = 0u64;
    while (*lw).head != (*lw).tail && k + SZ <= n {
        ptr::write_unaligned(dst.add(k as usize) as *mut Wev, (*lw).q[(*lw).head]);
        (*lw).head = ((*lw).head + 1) % WQ;
        k += SZ;
    }
    k as i64
}

/// The window manager: files (THE WRLD OS paths, one per line) were dropped on `w` at x, y.
/// For a Linux program's window they become a text/uri-list of their Linux paths and an event;
/// false for any other window.
#[no_mangle]
pub unsafe extern "C" fn lxwin_drop(w: *mut Window, x: c_int, y: c_int, paths: *const c_char) -> CBool {
    if w.is_null() || paths.is_null() { return 0; }
    let mut lw = ALL_WINS;                               /* one of ours? */
    while !lw.is_null() && (*lw).w != w { lw = (*lw).next; }
    if lw.is_null() { return 0; }
    let mut out: alloc::vec::Vec<u8> = alloc::vec::Vec::new();
    for line in core::ffi::CStr::from_ptr(paths).to_bytes().split(|&c| c == b'\n') {
        if line.is_empty() { continue; }
        let lp = crate::lxpath::wrld_to_linux(line, 1024);
        out.extend_from_slice(b"file://");
        for &c in crate::lxpath::cstr(&lp) {
            if c.is_ascii_alphanumeric() || b"/-_.~".contains(&c) { out.push(c); }
            else { let h = b"0123456789ABCDEF"; out.push(b'%'); out.push(h[(c >> 4) as usize]); out.push(h[(c & 15) as usize]); }
        }
        out.extend_from_slice(b"\r\n");
    }
    if out.is_empty() { return 0; }
    kfree((*lw).drop_list as *mut c_void);
    let p = kmalloc(out.len()) as *mut u8;
    if p.is_null() { return 0; }
    ptr::copy_nonoverlapping(out.as_ptr(), p, out.len());
    (*lw).drop_list = p; (*lw).drop_len = out.len();
    push(lw, WEV_DROP, x, y, 0, 0, 0);
    1
}
