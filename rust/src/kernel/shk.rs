//! The shell (declared in src/include/apps.h, was `src/shell/sh.c`): runs the syntax trees of
//! `crate::shell` with pipelines, redirection, functions and control flow, starts programs
//! (Win32, Linux, scripts, disk images, GUI applications), and has the built-in commands that
//! work on text and files. The commands that front other parts of the system (network,
//! display, sound, devices, packages, themes...) stay in C, in src/shell/shcmds.c, and are
//! called through sh_c_command.
//!
//! Changes from the C version: output written to a pipe or captured by $(..) is dropped
//! instead of crashing when memory runs out; a redirection target deleted by its own command
//! is not written through a stale pointer; seq stops at Ctrl+C and at an overflow; and the
//! fixes listed in `crate::shell`.

use alloc::rc::Rc;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr::null_mut;

use super::vfsk::*;
use super::{CBool, RtcTime, Vnode};
use crate::shell::{self as sl, Env, Node, RedirKind};
use crate::vfspath as vp;

// ---------------------------------------------------------------- C side

/// console_t (src/include/apps.h)
#[repr(C)]
pub struct Console {
    write: Option<unsafe extern "C" fn(*mut Console, *const c_char, c_int)>,
    readline: Option<unsafe extern "C" fn(*mut Console, *mut c_char, c_int) -> c_int>,
    getkey: Option<unsafe extern "C" fn(*mut Console) -> c_int>,
    clear: Option<unsafe extern "C" fn(*mut Console)>,
    set_title: Option<unsafe extern "C" fn(*mut Console, *const c_char)>,
    set_color: Option<unsafe extern "C" fn(*mut Console, c_int, c_int)>,
    alive: Option<unsafe extern "C" fn(*mut Console) -> CBool>,
    imp: *mut c_void,
}
/// A command's standard input, output and error (io_t in src/shell/shcmds.c).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Io { inp: *mut Console, out: *mut Console, err: *mut Console }

extern "C" {
    fn sh_c_command(name: *const c_char, cwd: *const c_char, argc: c_int, argv: *mut *mut c_char, io: *mut Io) -> c_int;
    fn sh_gui_launch(app: *const c_char, arg: *const c_char);
    fn sh_task_pid() -> c_int;
    fn app_find(id: *const c_char) -> *const c_void;
    fn pe_spawn(path: *const c_char, args: *const c_char, con: *mut Console, err: *mut c_char, errcap: c_int) -> c_int;
    fn pe_set_stdin(pid: c_int, inp: *mut Console);
    fn pe_is_gui(n: *mut Vnode) -> CBool;
    fn lx_spawn(path: *const c_char, argc: c_int, argv: *mut *mut c_char, envp: *mut *mut c_char, inp: *mut Console, out: *mut Console, err: *mut c_char, ecap: c_int) -> c_int;
    fn macho_probe(d: *const u8, n: u32, desc: *mut c_char, cap: c_int) -> CBool;
    fn task_find(pid: c_int) -> *mut c_void;
    fn task_kill(pid: c_int) -> CBool;
    fn task_exit_code(pid: c_int) -> c_int;
    fn task_sleep(ms: u32);
    fn task_yield();
    fn timer_ticks() -> u32;
    fn rand_next() -> u32;
    fn rtc_now(t: *mut RtcTime);
    fn mm_total() -> u32;
    fn mm_used() -> u32;
    fn terminal_cwd_ptr(c: *mut Console, cwd: *mut *mut c_char) -> *const c_char;
    fn console_interrupted(c: *mut Console) -> CBool;
    fn console_clear_interrupt(c: *mut Console);
    fn terminal_open(path: *const c_char, cwd: *const c_char);
    static user_name: [c_char; 32];
}

const USER_HOME: &[u8] = b"C:\\Documents and Settings\\User";
const OS_NAME: &[u8] = b"THE WRLD OS";
#[cfg(target_arch = "x86_64")]
const PATH: &[u8] = b".;C:\\WRLD\\System32;C:\\WRLD;C:\\Linux\\usr\\local\\bin;C:\\Linux\\usr\\bin;C:\\Linux\\usr\\games;C:\\Samples;C:\\Program Files";
#[cfg(not(target_arch = "x86_64"))]
const PATH: &[u8] = b".;C:\\WRLD\\System32;C:\\WRLD;C:\\Samples;C:\\Program Files";

fn cvec(s: &[u8]) -> Vec<u8> { let mut v = sl::cstr(s).to_vec(); v.push(0); v }
unsafe fn cbytes<'a>(p: *const c_char) -> &'a [u8] { if p.is_null() { &[] } else { CStr::from_ptr(p).to_bytes() } }
fn cat(parts: &[&[u8]]) -> Vec<u8> { let mut v = Vec::new(); for p in parts { v.extend_from_slice(p); } v }
fn num(n: impl core::fmt::Display) -> Vec<u8> { alloc::format!("{}", n).into_bytes() }
fn username() -> Vec<u8> { unsafe { sl::cstr(&*(core::ptr::addr_of!(user_name) as *const [u8; 32])).to_vec() } }

unsafe fn cwrite(c: *mut Console, s: &[u8]) {
    if c.is_null() || s.is_empty() { return; }
    if let Some(f) = (*c).write { f(c, s.as_ptr() as *const c_char, s.len().min(i32::MAX as usize) as c_int); }
}
unsafe fn creadline(c: *mut Console, cap: usize) -> Option<Vec<u8>> {
    let f = if c.is_null() { None } else { (*c).readline }?;
    let mut buf = alloc::vec![0u8; cap];
    let n = f(c, buf.as_mut_ptr() as *mut c_char, cap as c_int);
    if n < 0 { return None; }
    Some(sl::cstr(&buf).to_vec())
}
unsafe fn calive(c: *mut Console) -> bool { if c.is_null() { return false; } (*c).alive.map_or(true, |f| f(c) != 0) }
unsafe fn out(io: &Io, s: &[u8]) { cwrite(io.out, s) }
unsafe fn errm(io: &Io, s: &[u8]) { cwrite(io.err, b"\x1b[91m"); cwrite(io.err, s); cwrite(io.err, b"\x1b[0m"); }

// ---------------------------------------------------------------- buffer consoles (pipes, $(..), redirection)

#[repr(C)]
struct BufCon { c: Console, buf: Vec<u8>, rpos: usize, parent: *mut Console }
unsafe extern "C" fn bc_write(c: *mut Console, s: *const c_char, n: c_int) {
    let b = &mut *(c as *mut BufCon);
    if n <= 0 || s.is_null() { return; }
    let d = core::slice::from_raw_parts(s as *const u8, n as usize);
    if b.buf.try_reserve(d.len()).is_ok() { b.buf.extend_from_slice(d); }      // out of memory: dropped
}
unsafe extern "C" fn bc_readline(c: *mut Console, o: *mut c_char, cap: c_int) -> c_int {
    let b = &mut *(c as *mut BufCon);
    if b.rpos >= b.buf.len() || cap <= 0 { return -1; }
    let mut n = 0usize;
    while b.rpos < b.buf.len() && b.buf[b.rpos] != b'\n' {
        if n < cap as usize - 1 { *o.add(n) = b.buf[b.rpos] as c_char; n += 1; }
        b.rpos += 1;
    }
    if b.rpos < b.buf.len() { b.rpos += 1; }
    *o.add(n) = 0;
    n as c_int
}
unsafe extern "C" fn bc_getkey(c: *mut Console) -> c_int {
    let b = &mut *(c as *mut BufCon);
    if b.rpos < b.buf.len() { b.rpos += 1; b.buf[b.rpos - 1] as c_int } else { -1 }
}
unsafe extern "C" fn bc_noop(_c: *mut Console) {}
unsafe extern "C" fn bc_title(_c: *mut Console, _t: *const c_char) {}
unsafe extern "C" fn bc_color(_c: *mut Console, _f: c_int, _b: c_int) {}
unsafe extern "C" fn bc_alive(c: *mut Console) -> CBool { let b = &*(c as *mut BufCon); if b.parent.is_null() { 1 } else { calive(b.parent) as CBool } }
impl BufCon {
    fn new(parent: *mut Console) -> alloc::boxed::Box<BufCon> {
        let mut b = alloc::boxed::Box::new(BufCon {
            c: Console { write: Some(bc_write), readline: Some(bc_readline), getkey: Some(bc_getkey), clear: Some(bc_noop),
                         set_title: Some(bc_title), set_color: Some(bc_color), alive: Some(bc_alive), imp: null_mut() },
            buf: Vec::new(), rpos: 0, parent,
        });
        b.c.imp = &mut *b as *mut BufCon as *mut c_void;
        b
    }
    fn con(&mut self) -> *mut Console { &mut self.c }
}

// ---------------------------------------------------------------- the shell's state

pub struct Sh {
    con: *mut Console,
    cwd: Vec<u8>,
    /// oldest first; listed newest first, as the C list was
    vars: Vec<(Vec<u8>, Vec<u8>)>,
    /// names given to `export`: these variables go to the Linux programs the shell starts
    exports: Vec<Vec<u8>>,
    funcs: Vec<(Vec<u8>, Option<Rc<Node>>)>,
    status: i32,
    argv: Vec<Vec<u8>>,
    brk: i32,
    cont: i32,
    ret: bool,
    exiting: bool,
    exit_code: i32,
    depth: i32,
    script_name: Option<Vec<u8>>,
}

unsafe fn node_data<'a>(n: *mut Vnode) -> &'a [u8] {
    if n.is_null() || (*n).data.is_null() { &[] } else { core::slice::from_raw_parts((*n).data, (*n).size as usize) }
}
unsafe fn node_name<'a>(n: *mut Vnode) -> &'a [u8] { cbytes((*n).name.as_ptr()) }
unsafe fn resolve(path: &[u8], cwd: Option<&[u8]>) -> *mut Vnode {
    let p = cvec(path);
    match cwd { Some(c) => { let c = cvec(c); vfs_resolve(p.as_ptr() as *const c_char, c.as_ptr() as *const c_char) } None => vfs_resolve(p.as_ptr() as *const c_char, core::ptr::null()) }
}
/// vfs_path into the C version's 256-byte buffer
unsafe fn path_of(n: *mut Vnode, cap: usize) -> Vec<u8> {
    let mut b = alloc::vec![0u8; cap];
    vfs_path(n, b.as_mut_ptr() as *mut c_char, cap as c_int);
    sl::cstr(&b).to_vec()
}
fn ext(name: &[u8]) -> &[u8] { match name.iter().rposition(|&c| c == b'.') { Some(i) if i > 0 => &name[i + 1..], _ => b"" } }
fn ends_ci(s: &[u8], suf: &[u8]) -> bool { s.len() >= suf.len() && s[s.len() - suf.len()..].eq_ignore_ascii_case(suf) }
fn has_drive(p: &[u8]) -> bool { p.len() >= 2 && p[0].is_ascii_alphabetic() && p[1] == b':' }
fn is_devnull(t: &[u8]) -> bool { t == b"/dev/null" || t.eq_ignore_ascii_case(b"nul") }

impl Sh {
    unsafe fn new(con: *mut Console, cwd: Option<&[u8]>) -> Sh {
        let mut s = Sh { con, cwd: USER_HOME.to_vec(), vars: Vec::new(), exports: Vec::new(), funcs: Vec::new(), status: 0, argv: Vec::new(),
                         brk: 0, cont: 0, ret: false, exiting: false, exit_code: 0, depth: 0, script_name: None };
        if let Some(c) = cwd {
            let d = resolve(c, None);
            if !d.is_null() && (*d).dir != 0 { s.cwd = path_of(d, 256); }
        }
        s.var_set(b"HOME", USER_HOME);
        s.var_set(b"USER", &username());
        s.var_set(b"SHELL", b"C:\\WRLD\\System32\\sh.exe");
        s.var_set(b"OS", OS_NAME);
        s.var_set(b"PATH", PATH);
        s.var_set(b"PS1", b"");
        let cwd = s.cwd.clone();
        s.var_set(b"PWD", &cwd);
        s
    }
    fn var_get(&self, n: &[u8]) -> Option<Vec<u8>> { self.vars.iter().find(|v| v.0 == n).map(|v| v.1.clone()) }
    fn var_set(&mut self, n: &[u8], val: &[u8]) {
        let val = sl::cstr(val);
        match self.vars.iter_mut().find(|v| v.0 == n) { Some(v) => v.1 = val.to_vec(), None => self.vars.push((n.to_vec(), val.to_vec())) }
    }
    fn var_unset(&mut self, n: &[u8]) { if let Some(i) = self.vars.iter().position(|v| v.0 == n) { self.vars.remove(i); } }
    /// a child shell for a script or sh -c, with this shell's variables
    unsafe fn child(&self) -> Sh {
        let mut c = Sh::new(self.con, Some(&self.cwd));
        for (k, v) in self.vars.iter().rev() { c.var_set(k, v); }
        c
    }
    unsafe fn interrupted(&self) -> bool { console_interrupted(self.con) != 0 }
    fn sync_cwd(&mut self) { let c = self.cwd.clone(); self.var_set(b"PWD", &c); }
    fn find_func(&self, n: &[u8]) -> Option<Option<Rc<Node>>> { self.funcs.iter().find(|f| f.0 == n).map(|f| f.1.clone()) }
}

struct ShEnv<'a> { s: &'a mut Sh }
impl Env for ShEnv<'_> {
    fn var(&self, n: &[u8]) -> Option<Vec<u8>> { self.s.var_get(n) }
    fn set_var(&mut self, n: &[u8], v: &[u8]) { self.s.var_set(n, v) }
    fn special(&mut self, n: &[u8]) -> Option<Vec<u8>> {
        let s = &*self.s;
        let argc = s.argv.len();
        Some(match n {
            b"?" => num(s.status),
            b"#" => num(argc.saturating_sub(1)),
            b"$" => num(unsafe { sh_task_pid() }),
            b"@" | b"*" => { let mut v = Vec::new(); for i in 1..argc { if i > 1 { v.push(b' '); } v.extend_from_slice(&s.argv[i]); } v }
            b"RANDOM" => num(unsafe { rand_next() } % 32768),
            b"SECONDS" => num(unsafe { timer_ticks() } / 1000),
            b"PWD" => s.cwd.clone(),
            _ if !n.is_empty() && n[0].is_ascii_digit() => {
                let k = sl::atoi(n);
                if k == 0 { s.script_name.clone().unwrap_or_else(|| b"sh".to_vec()) }
                else if k > 0 && (k as usize) < argc { s.argv[k as usize].clone() } else { Vec::new() }
            }
            _ => return None,
        })
    }
    fn cmd_subst(&mut self, cmd: &[u8]) -> Vec<u8> { unsafe { cmd_subst(self.s, cmd) } }
    fn dir_names(&mut self, dir: &[u8]) -> Option<Vec<Vec<u8>>> {
        unsafe {
            let d = resolve(if dir.is_empty() { b"." } else { dir }, Some(&self.s.cwd));
            if d.is_null() || (*d).dir == 0 { return None; }
            vfs_sort_children(d);
            let mut v = Vec::new();
            let mut c = (*d).child;
            while !c.is_null() { v.push(node_name(c).to_vec()); c = (*c).next; }
            Some(v)
        }
    }
    fn home(&self) -> Vec<u8> { USER_HOME.to_vec() }
}
fn expand_words(s: &mut Sh, words: &[Vec<u8>]) -> Vec<Vec<u8>> {
    let mut v = Vec::new();
    let mut e = ShEnv { s };
    for w in words { sl::expand_word(&mut e, w, &mut v, true); }
    v
}
fn expand_single(s: &mut Sh, w: &[u8]) -> Vec<u8> { sl::expand_single(&mut ShEnv { s }, w) }
fn arith(s: &mut Sh, e: &[u8]) -> sl::Long { sl::arith(&mut ShEnv { s }, e) }

unsafe fn cmd_subst(s: &mut Sh, cmd: &[u8]) -> Vec<u8> {
    let mut b = BufCon::new(s.con);
    let io = Io { inp: s.con, out: b.con(), err: b.con() };
    let save = s.cwd.clone();
    run_string(s, cmd, &io);
    s.cwd = save;
    let mut r = sl::cstr(&b.buf).to_vec();
    while matches!(r.last(), Some(b'\n') | Some(b'\r')) { r.pop(); }
    r
}

// ---------------------------------------------------------------- running

unsafe fn run_string(s: &mut Sh, src: &[u8], io: &Io) -> i32 {
    match sl::parse(src) {
        Err(sl::ParseError::Incomplete) => { errm(io, b"sh: syntax error: unexpected end of file\n"); 2 }
        Err(sl::ParseError::Syntax(m)) => { errm(io, &cat(&[b"sh: ", &m, b"\n"])); 2 }
        Ok(n) => run_node(s, n.as_ref(), io),
    }
}

unsafe fn run_node(s: &mut Sh, n: Option<&Node>, io: &Io) -> i32 {
    let Some(n) = n else { return 0 };
    if s.exiting || s.interrupted() { return s.status; }
    s.depth += 1;
    if s.depth > 200 { errm(io, b"sh: maximum recursion depth exceeded\n"); s.depth -= 1; return 1; }
    let rc = match n {
        Node::List(items) => {
            let mut rc = 0;
            for c in items {
                rc = run_node(s, Some(c), io);
                s.status = rc;
                if s.exiting || s.brk != 0 || s.cont != 0 || s.ret || s.interrupted() { break; }
                task_yield();
            }
            rc
        }
        Node::Simple { words, redirs } => run_simple(s, words, redirs, io),
        Node::Not(a) => (run_node(s, a.as_deref(), io) == 0) as i32,
        Node::And(a, b) => { let rc = run_node(s, Some(a), io); s.status = rc; if rc == 0 { run_node(s, Some(b), io) } else { rc } }
        Node::Or(a, b) => { let rc = run_node(s, Some(a), io); s.status = rc; if rc != 0 { run_node(s, Some(b), io) } else { rc } }
        Node::Pipe(v) => run_pipe(s, v, io),
        Node::If { cond, then, els } => {
            if run_node(s, cond.as_deref(), io) == 0 { run_node(s, then.as_deref(), io) }
            else if els.is_some() { run_node(s, els.as_deref(), io) } else { 0 }
        }
        Node::Loop { until, cond, body } => run_loop(s, *until, cond.as_deref(), body.as_deref(), io),
        Node::For { name, words, body } => run_for(s, name, words, body.as_deref(), io),
        Node::Case { word, arms } => run_case(s, word, arms, io),
        Node::Func { name, body } => {
            match s.funcs.iter_mut().find(|f| f.0 == *name) { Some(f) => f.1 = body.clone(), None => s.funcs.push((name.clone(), body.clone())) }
            0
        }
        Node::Group(a) => run_node(s, a.as_deref(), io),
        Node::Subshell(a) => { let save = s.cwd.clone(); let rc = run_node(s, a.as_deref(), io); s.cwd = save; rc }
    };
    s.depth -= 1;
    s.status = rc;
    rc
}
#[inline(never)]
unsafe fn run_pipe(s: &mut Sh, v: &[Node], io: &Io) -> i32 {
    let mut rc = 0;
    let mut prev: Option<alloc::boxed::Box<BufCon>> = None;
    for (i, c) in v.iter().enumerate() {
        let mut pio = *io;
        if let Some(p) = prev.as_mut() { pio.inp = p.con(); }
        let mut cur = if i + 1 < v.len() { Some(BufCon::new(s.con)) } else { None };
        if let Some(c) = cur.as_mut() { pio.out = c.con(); }
        rc = run_node(s, Some(c), &pio);
        prev = cur;
    }
    rc
}
#[inline(never)]
unsafe fn run_loop(s: &mut Sh, until: bool, cond: Option<&Node>, body: Option<&Node>, io: &Io) -> i32 {
    let mut rc = 0;
    let mut guard = 0u32;
    loop {
        let c = run_node(s, cond, io);
        if s.exiting || s.interrupted() { break; }
        if if until { c == 0 } else { c != 0 } { break; }
        rc = run_node(s, body, io);
        if s.brk != 0 { s.brk -= 1; break; }
        if s.cont != 0 { s.cont = 0; }
        if s.ret || s.exiting { break; }
        guard += 1;
        if guard % 64 == 0 { task_yield(); }
    }
    rc
}
#[inline(never)]
unsafe fn run_for(s: &mut Sh, name: &[u8], words: &[Vec<u8>], body: Option<&Node>, io: &Io) -> i32 {
    let mut rc = 0;
    for it in expand_words(s, words) {
        s.var_set(name, &it);
        rc = run_node(s, body, io);
        if s.brk != 0 { s.brk -= 1; break; }
        if s.cont != 0 { s.cont = 0; }
        if s.ret || s.exiting || s.interrupted() { break; }
    }
    rc
}
#[inline(never)]
unsafe fn run_case(s: &mut Sh, word: &[u8], arms: &[(Vec<Vec<u8>>, sl::Sub)], io: &Io) -> i32 {
    let w = expand_single(s, word);
    for (pats, body) in arms {
        let mut m = false;
        for p in pats { if m { break; } let p = expand_single(s, p); m = sl::glob_match(&p, &w); }
        if m { return run_node(s, body.as_deref(), io); }
    }
    0
}

#[inline(never)]
unsafe fn run_simple(s: &mut Sh, words: &[Vec<u8>], redirs: &[sl::Redir], io: &Io) -> i32 {
    let mut wi = 0;
    while wi < words.len() {
        let w = &words[wi];
        let Some(eq) = w.iter().position(|&c| c == b'=') else { break };
        if eq == 0 || !w[..eq].iter().all(|&c| c.is_ascii_alphanumeric() || c == b'_') || w[0].is_ascii_digit() { break; }
        wi += 1;
    }
    let mut argv = expand_words(s, &words[wi..]);
    for w in &words[..wi] {
        let eq = w.iter().position(|&c| c == b'=').unwrap();
        let v = expand_single(s, &w[eq + 1..]);
        s.var_set(&w[..eq], &v);
    }
    if argv.is_empty() { return 0; }
    // redirections: output is collected and written when the command ends
    let mut rio = *io;
    let mut bufs: Vec<alloc::boxed::Box<BufCon>> = Vec::new();
    let mut out_to: Option<(usize, Option<Vec<u8>>, bool)> = None;
    let mut err_to: Option<(usize, Option<Vec<u8>>)> = None;
    for r in redirs {
        if r.kind == RedirKind::ErrToOut { rio.err = rio.out; continue; }
        let t = expand_single(s, &r.target);
        let mut b = BufCon::new(s.con);
        let con = b.con();
        if r.kind == RedirKind::In {
            if !is_devnull(&t) {
                let f = resolve(&t, Some(&s.cwd));
                if f.is_null() || (*f).dir != 0 { errm(io, &cat(&[&t, b": No such file\n"])); return 1; }
                bc_write(con, node_data(f).as_ptr() as *const c_char, (*f).size as c_int);
            }
            rio.inp = con;
        } else {
            let target = if is_devnull(&t) { None } else {
                let tc = cvec(&t); let cw = cvec(&s.cwd);
                let f = vfs_create(tc.as_ptr() as *const c_char, cw.as_ptr() as *const c_char, 0);
                if f.is_null() || (*f).dir != 0 || (*f).readonly != 0 { errm(io, &cat(&[&t, b": cannot write\n"])); return 1; }
                Some(path_of(f, 512))
            };
            if r.kind == RedirKind::Err { err_to = Some((bufs.len(), target)); rio.err = con; }
            else { out_to = Some((bufs.len(), target, r.kind == RedirKind::Append)); rio.out = con; }
        }
        bufs.push(b);
    }
    let rc = if let Some(body) = s.find_func(&argv[0]) {
        let old = core::mem::replace(&mut s.argv, argv);
        let rc = run_node(s, body.as_deref(), &rio);
        s.ret = false;
        s.argv = old;
        rc
    } else { run_command(s, &mut argv, &rio) };
    // the target is found again by its path: the command may have deleted or replaced it
    if let Some((i, Some(p), append)) = out_to {
        let f = resolve(&p, None);
        let d = &bufs[i].buf;
        if !f.is_null() && (*f).dir == 0 {
            if append { vfs_append_node(f, d.as_ptr() as *const c_void, d.len() as u32); } else { vfs_write_node(f, d.as_ptr() as *const c_void, d.len() as u32); }
        }
    }
    if let Some((i, Some(p))) = err_to {
        let f = resolve(&p, None);
        let d = &bufs[i].buf;
        if !f.is_null() && (*f).dir == 0 { vfs_write_node(f, d.as_ptr() as *const c_void, d.len() as u32); }
    }
    rc
}

/// A built-in (Rust or C) or a program.
unsafe fn run_command(s: &mut Sh, argv: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    match find_builtin(&argv[0]) {
        Some(Bf::R(f)) => f(s, argv, io),
        Some(Bf::C) => call_c(s, argv, io),
        None => run_external(s, argv, io),
    }
}
unsafe fn call_c(s: &Sh, argv: &[Vec<u8>], io: &Io) -> i32 {
    let mut bufs: Vec<Vec<u8>> = argv.iter().map(|a| cvec(a)).collect();
    let mut ptrs: Vec<*mut c_char> = bufs.iter_mut().map(|b| b.as_mut_ptr() as *mut c_char).collect();
    ptrs.push(null_mut());
    let cwd = cvec(&s.cwd);
    let mut io2 = *io;
    sh_c_command(ptrs[0], cwd.as_ptr() as *const c_char, argv.len() as c_int, ptrs.as_mut_ptr(), &mut io2)
}
unsafe fn gui_launch(app: &[u8], arg: Option<&[u8]>) {
    let a = cvec(app);
    match arg { Some(x) => { let x = cvec(x); sh_gui_launch(a.as_ptr() as *const c_char, x.as_ptr() as *const c_char) } None => sh_gui_launch(a.as_ptr() as *const c_char, core::ptr::null()) }
}
unsafe fn is_app(name: &[u8]) -> bool { let n = cvec(name); !app_find(n.as_ptr() as *const c_char).is_null() }

/// A program by name: on PATH (with .exe, .sh or .app added), or a path.
unsafe fn resolve_external(s: &Sh, name: &[u8]) -> Option<Vec<u8>> {
    let mut n: *mut Vnode = null_mut();
    if name.contains(&b'/') || name.contains(&b'\\') || name.get(1) == Some(&b':') { n = resolve(name, Some(&s.cwd)); }
    else {
        let path = s.var_get(b"PATH").unwrap_or_else(|| b".".to_vec());
        let path = &path[..path.len().min(511)];
        'dirs: for d in path.split(|&c| c == b';') {
            for e in [&b""[..], b".exe", b".sh", b".app"] {
                let p = cat(&[d, b"\\", name, e]);
                let c = resolve(&p, Some(&s.cwd));
                if !c.is_null() && ((*c).dir == 0 || ends_ci(node_name(c), b".app")) { n = c; break 'dirs; }
            }
        }
    }
    if n.is_null() { None } else { Some(path_of(n, 256)) }
}
unsafe fn wait_pid(s: &Sh, pid: c_int) -> i32 {
    while !task_find(pid).is_null() {
        if s.interrupted() { task_kill(pid); console_clear_interrupt(s.con); return 130; }
        task_sleep(15);
    }
    task_exit_code(pid)
}
unsafe fn lx_run(s: &Sh, path: &[u8], argv: &[Vec<u8>], io: &Io) -> i32 {
    let mut bufs: Vec<Vec<u8>> = argv.iter().map(|a| cvec(a)).collect();
    let mut ptrs: Vec<*mut c_char> = bufs.iter_mut().map(|b| b.as_mut_ptr() as *mut c_char).collect();
    ptrs.push(null_mut());
    let p = cvec(path);
    let mut err = [0u8; 256];
    /* the environment: Linux's usual defaults, then what was exported in this shell */
    let mut env: Vec<Vec<u8>> = [&b"PATH=/usr/local/bin:/usr/bin:/bin"[..], b"HOME=/home/user", b"USER=user", b"LOGNAME=user",
                                 b"SHELL=/bin/sh", b"TERM=dumb", b"LANG=C.UTF-8", b"PWD=/"].iter().map(|e| e.to_vec()).collect();
    for name in &s.exports {
        let Some(v) = s.vars.iter().rev().find(|(k, _)| k == name).map(|(_, v)| v.clone()) else { continue };
        env.retain(|e| !(e.len() > name.len() && e.starts_with(name) && e[name.len()] == b'='));
        env.push(cat(&[name, b"=", &v]));
    }
    let mut ebufs: Vec<Vec<u8>> = env.iter().map(|e| cvec(e)).collect();
    let mut eptrs: Vec<*mut c_char> = ebufs.iter_mut().map(|b| b.as_mut_ptr() as *mut c_char).collect();
    eptrs.push(null_mut());
    let pid = lx_spawn(p.as_ptr() as *const c_char, argv.len() as c_int, ptrs.as_mut_ptr(), eptrs.as_mut_ptr(), io.inp, io.out, err.as_mut_ptr() as *mut c_char, 256);
    if pid < 0 { errm(io, &cat(&[&argv[0], b": ", sl::cstr(&err), b"\n"])); return 126; }
    wait_pid(s, pid)
}
unsafe fn run_external(s: &mut Sh, argv: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let a0 = argv[0].clone();
    let Some(path) = resolve_external(s, &a0) else {
        if is_app(&a0) || a0.eq_ignore_ascii_case(b"notepad") || a0.eq_ignore_ascii_case(b"mspaint") {
            let mut full = Vec::new();
            if argv.len() > 1 {
                let n = resolve(&argv[1], Some(&s.cwd));
                if !n.is_null() { full = path_of(n, 512); }
                else if !a0.eq_ignore_ascii_case(b"control") { full = if has_drive(&argv[1]) { vp::lcpy(&argv[1], 512) } else { vp::join(&s.cwd, &argv[1], 512) }; }
                else { full = vp::lcpy(&argv[1], 512); }
            }
            if a0.eq_ignore_ascii_case(b"explorer") && argv.len() == 1 { full = s.cwd.clone(); }
            gui_launch(&a0, if full.is_empty() { None } else { Some(&full) });
            return 0;
        }
        errm(io, &cat(&[&a0, b": command not found\n"]));
        return 127;
    };
    let n = resolve(&path, None);
    if n.is_null() { errm(io, &cat(&[&a0, b": command not found\n"])); return 127; }
    let name = node_name(n).to_vec();
    let e = ext(&name).to_vec();
    let data = node_data(n);
    if (*n).dir != 0 {
        if ends_ci(&name, b".app") { gui_launch(b"::open", Some(&path)); return 0; }
        errm(io, &cat(&[&a0, b": is a directory\n"]));
        return 126;
    }
    if e.eq_ignore_ascii_case(b"exe") || e.eq_ignore_ascii_case(b"com") || (data.len() > 2 && data[0] == b'M' && data[1] == b'Z') {
        let mut args = Vec::new();
        for (i, a) in argv.iter().enumerate().skip(1) {
            if i > 1 { vp::lcat(&mut args, b" ", 512); }
            let q = a.contains(&b' ');
            if q { vp::lcat(&mut args, b"\"", 512); }
            vp::lcat(&mut args, a, 512);
            if q { vp::lcat(&mut args, b"\"", 512); }
        }
        let (p, a) = (cvec(&path), cvec(&args));
        let mut err = [0u8; 256];
        let pid = pe_spawn(p.as_ptr() as *const c_char, a.as_ptr() as *const c_char, io.out, err.as_mut_ptr() as *mut c_char, 256);
        if pid < 0 { errm(io, &cat(&[&a0, b": ", sl::cstr(&err), b"\n"])); return 126; }
        pe_set_stdin(pid, io.inp);
        if pe_is_gui(n) != 0 { out(io, &cat(&[b"Started ", &name, b" (pid ", &num(pid), b")\n"])); return 0; }
        return wait_pid(s, pid);
    }
    #[cfg(target_arch = "x86_64")]
    if data.len() > 3 && data[0] == b'#' && data[1] == b'!' && path.len() >= 9 && path[..9].eq_ignore_ascii_case(b"C:\\Linux\\") {
        return lx_run(s, &path, argv, io);
    }
    if e.eq_ignore_ascii_case(b"sh") || e.eq_ignore_ascii_case(b"bash") || (data.len() > 2 && data[0] == b'#' && data[1] == b'!') {
        return run_file_io(s, &path, argv, io);
    }
    if e.eq_ignore_ascii_case(b"dmg") { return call_c(s, &[b"mount".to_vec(), path], io); }
    let mut desc = [0u8; 128];
    if macho_probe(data.as_ptr(), data.len() as u32, desc.as_mut_ptr() as *mut c_char, 128) != 0 {
        errm(io, &cat(&[&a0, b": cannot execute: ", sl::cstr(&desc), b" (native macOS binaries are not supported)\n"]));
        return 126;
    }
    if data.len() >= 4 && &data[..4] == b"\x7fELF" { return lx_run(s, &path, argv, io); }
    gui_launch(b"::open", Some(&path));
    0
}

/// A script in a child shell: argv[0] is replaced by the script's full path.
unsafe fn run_file_io(parent: &Sh, path: &[u8], argv: &[Vec<u8>], io: &Io) -> i32 {
    let n = resolve(path, Some(&parent.cwd));
    if n.is_null() || (*n).dir != 0 { errm(io, &cat(&[b"sh: ", path, b": No such file\n"])); return 127; }
    let src = sl::cstr(node_data(n)).to_vec();
    let mut c = parent.child();
    let full = path_of(n, 256);
    c.script_name = Some(full.clone());
    c.argv = core::iter::once(full).chain(argv.iter().skip(1).cloned()).collect();
    let mut rc = run_string(&mut c, &src, io);
    if c.exiting { rc = c.exit_code; }
    rc
}

// ---------------------------------------------------------------- built-in commands

type RFn = unsafe fn(&mut Sh, &mut Vec<Vec<u8>>, &Io) -> i32;
#[derive(Clone, Copy)]
enum Bf { R(RFn), C }
struct Bi { name: &'static [u8], f: Bf, help: Option<&'static str> }
const fn r(name: &'static [u8], f: RFn, help: Option<&'static str>) -> Bi { Bi { name, f: Bf::R(f), help } }
const fn c(name: &'static [u8], help: Option<&'static str>) -> Bi { Bi { name, f: Bf::C, help } }

/// In the order `help` lists them (as the C table had them).
static BUILTINS_A: &[Bi] = &[
    r(b"sh", b_sh, Some("run a script (sh FILE / sh -c CMD)")), r(b"bash", b_sh, None),
    r(b"echo", b_echo, Some("print arguments")), r(b"printf", b_printf, Some("formatted print")), r(b"cd", b_cd, Some("change directory")),
    r(b"pwd", b_pwd, Some("print working directory")), r(b"ls", b_ls, Some("list files (-l -a)")), r(b"dir", b_dir, Some("list files (DOS style)")),
    r(b"cat", b_cat, Some("print files")), r(b"type", b_type, Some("describe a command / print file")), r(b"mkdir", b_mkdir, Some("make directories (-p)")), r(b"md", b_mkdir, None),
    r(b"rmdir", b_rm, None), r(b"rd", b_rm, None), r(b"rm", b_rm, Some("remove files (-r -f)")), r(b"del", b_rm, None), r(b"touch", b_touch, Some("create empty file")),
    r(b"cp", b_cp, Some("copy files")), r(b"copy", b_cp, None), r(b"mv", b_mv, Some("move/rename")), r(b"move", b_mv, None), r(b"ren", b_mv, None),
    r(b"head", b_headtail, Some("first lines")), r(b"tail", b_headtail, Some("last lines")), r(b"wc", b_wc, Some("count lines/words")),
    r(b"grep", b_grep, Some("search text (-i -n -v -c)")), r(b"sort", b_sort, Some("sort lines (-r -n -u)")), r(b"uniq", b_uniq, Some("drop repeated lines")),
    r(b"rev", b_rev, Some("reverse lines")), r(b"tr", b_tr, Some("translate characters")),
    r(b"clear", b_clear, Some("clear the screen")), r(b"cls", b_clear, None), r(b"exit", b_exit, Some("exit the shell")),
    r(b"true", b_true, None), r(b"false", b_false, None), r(b":", b_true, None), r(b"sleep", b_sleep, Some("wait N seconds")),
    r(b"date", b_date, Some("show date (+FORMAT)")), c(b"uname", Some("system name (-a)")), c(b"ver", Some("OS version")),
    c(b"whoami", None), c(b"hostname", None), c(b"ipconfig", Some("network configuration (/renew, /dns A.B.C.D)")), c(b"ifconfig", None),
    c(b"ping", Some("send ICMP echo requests")), c(b"nslookup", Some("look up a host name")),
    c(b"wget", Some("download a URL (http/https)")), r(b"unzip", b_unzip, Some("extract a .zip or .rar archive (unzip FILE [-d DIR] [-l])")), r(b"unrar", b_unzip, Some("extract a .rar or .zip archive (unrar x|e|l FILE [DIR])")), c(b"sha256sum", Some("SHA-256 of files")), c(b"js", Some("run JavaScript (js FILE or js -e CODE)")), c(b"display", Some("show or change the screen resolution")),
    c(b"play", Some("play a sound file (WAV, FLAC, MP3...)")), c(b"volume", Some("show or set the volume (0-100, mute)")), c(b"sndinfo", Some("sound card diagnostics")), c(b"curl", Some("print a URL")), c(b"ps", Some("list processes")), c(b"tasklist", None),
    c(b"kill", Some("terminate a process")), c(b"taskkill", None), c(b"free", Some("memory usage")), c(b"sync", Some("write changes to the system disk now")), c(b"mem", None),
    c(b"uptime", None), c(b"dmesg", Some("kernel log")), r(b"export", b_export, Some("set variables")), r(b"set", b_env, None),
    r(b"unset", b_unset, None), r(b"env", b_env, Some("list variables")), r(b"read", b_read, Some("read a line into variables")),
    r(b"test", b_test, Some("evaluate conditions")), r(b"[", b_test, None), r(b"[[", b_test, None), r(b"seq", b_seq, Some("number sequences")),
    r(b"expr", b_expr, Some("arithmetic")), r(b"let", b_let, None), r(b"basename", b_basename, None), r(b"dirname", b_dirname, None),
    r(b"help", b_help, Some("this help")), r(b"history", b_history, None), r(b"source", b_source, Some("run script in this shell")), r(b".", b_source, None),
    c(b"beep", Some("PC speaker beep")), r(b"color", b_color, Some("console colours (hex)")), r(b"title", b_title, Some("window title")),
    r(b"tree", b_tree, Some("directory tree")), r(b"find", b_find, Some("find files (-name PAT)")), r(b"hexdump", b_hexdump, Some("hex dump a file")), r(b"xxd", b_hexdump, None),
    c(b"mount", Some("mount a .dmg / list drives")), c(b"hdiutil", Some("disk image utility")), c(b"open", Some("open with default app")), c(b"start", None),
];
#[cfg(target_arch = "x86_64")]
static BUILTINS_X64: &[Bi] = &[
    c(b"lxtrace", Some("log Linux system calls (on/off)")), c(b"lxthreads", Some("what each Linux thread is waiting for (saved to C:\\WRLD\\Temp\\lxthreads.txt)")),
    c(b"dpkg", Some("install/remove .deb packages (-i -r -l -L -I)")), c(b"rpm", Some("install/remove .rpm packages (-i -e -qa)")),
    c(b"apt", Some("apt install ./FILE.deb, remove, list")), c(b"apt-get", None),
];
#[cfg(not(target_arch = "x86_64"))]
static BUILTINS_X64: &[Bi] = &[];
static BUILTINS_B: &[Bi] = &[
    c(b"guiprof", Some("log window painting times (on/off)")),
    c(b"theme", Some("change the visual style")), r(b"pause", b_pause, None), c(b"shutdown", Some("power off (-r reboot)")),
    c(b"passwd", Some("change the account password (-c clears)")), c(b"logoff", Some("log off and show the welcome screen")),
    c(b"lspci", Some("list PCI devices")), c(b"devices", Some("devices and their drivers (-p problems, -i IDs)")), c(b"lsdev", None),
    c(b"driverquery", Some("list the built-in drivers")), c(b"lsusb", Some("list USB devices")), c(b"lsblk", Some("list disks and partitions")), r(b"return", b_return, None), r(b"break", b_break, None), r(b"continue", b_continue, None),
    r(b"shift", b_shift, None), c(b"cpuinfo", None), r(b"xargs", b_xargs, Some("build commands from input")),
];
fn builtins() -> impl Iterator<Item = &'static Bi> { BUILTINS_A.iter().chain(BUILTINS_X64.iter()).chain(BUILTINS_B.iter()) }
fn find_builtin(name: &[u8]) -> Option<Bf> { builtins().find(|b| b.name == name).map(|b| b.f) }

/// The text of the files named from argv[first], or standard input when there are none (and
/// it is not the terminal); up to the first NUL, as the C version's strings were.
unsafe fn gather_input(s: &Sh, argv: &[Vec<u8>], first: usize, io: &Io) -> (Vec<u8>, bool) {
    let mut d = Vec::new();
    let mut ok = true;
    if first >= argv.len() {
        if io.inp != s.con {
            while let Some(l) = creadline(io.inp, 1024) {
                if d.try_reserve(l.len() + 1).is_err() { break; }
                d.extend_from_slice(&l); d.push(b'\n');
                if !calive(io.inp) { break; }
            }
        }
    } else {
        for a in &argv[first..] {
            let n = resolve(a, Some(&s.cwd));
            if n.is_null() || (*n).dir != 0 { errm(io, &cat(&[&argv[0], b": ", a, b": No such file\n"])); ok = false; continue; }
            let x = node_data(n);
            if d.try_reserve(x.len()).is_ok() { d.extend_from_slice(x); }
        }
    }
    let l = sl::cstr(&d).len();
    d.truncate(l);
    (d, ok)
}

unsafe fn b_sh(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    if a.len() > 2 && a[1] == b"-c" { let mut c = s.child(); return run_string(&mut c, &a[2], io); }
    if a.len() > 1 { let p = a[1].clone(); return run_file_io(s, &p, &a[1..], io); }
    out(io, b"(nested shells are not needed: you are already in sh)\n");
    0
}
unsafe fn b_echo(_s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { out(io, &sl::echo(a)); 0 }
unsafe fn b_printf(_s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { if a.len() < 2 { return 1; } out(io, &sl::printf(a)); 0 }
unsafe fn b_cd(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let mut t = if a.len() > 1 { a[1].clone() } else { USER_HOME.to_vec() };
    if t == b"-" { t = s.var_get(b"OLDPWD").unwrap_or_else(|| s.cwd.clone()); }
    let n = resolve(&t, Some(&s.cwd));
    if n.is_null() || (*n).dir == 0 { errm(io, &cat(&[b"cd: ", &t, b": No such directory\n"])); return 1; }
    let old = s.cwd.clone();
    s.var_set(b"OLDPWD", &old);
    s.cwd = path_of(n, 256);
    set_terminal_cwd(s);
    s.sync_cwd();
    0
}
unsafe fn set_terminal_cwd(s: &Sh) {
    if s.con.is_null() { return; }
    let mut p: *mut c_char = null_mut();
    terminal_cwd_ptr(s.con, &mut p);
    if !p.is_null() {
        let n = s.cwd.len().min(255);
        core::ptr::copy_nonoverlapping(s.cwd.as_ptr(), p as *mut u8, n);
        *p.add(n) = 0;
    }
}
unsafe fn b_pwd(s: &mut Sh, _a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { out(io, &cat(&[&s.cwd, b"\n"])); 0 }
unsafe fn fmt_mtime(t: &RtcTime) -> Vec<u8> { alloc::format!("{:04}-{:02}-{:02} {:02}:{:02}", t.year, t.month, t.day, t.hour, t.minute).into_bytes() }
unsafe fn list_of(d: *mut Vnode) -> Vec<*mut Vnode> {
    let mut v = Vec::new();
    let mut c = (*d).child;
    while !c.is_null() { v.push(c); c = (*c).next; }
    v
}
unsafe fn b_ls(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let (mut lng, mut all) = (false, false);
    let mut first = a.len();
    for i in 1..a.len() {
        if a[i].first() == Some(&b'-') && a[i].len() > 1 { if a[i].contains(&b'l') { lng = true; } if a[i].contains(&b'a') { all = true; } }
        else { first = i; break; }
    }
    let mut rc = 0;
    let paths: Vec<Vec<u8>> = if first == a.len() { alloc::vec![b".".to_vec()] } else { a[first..].to_vec() };
    for path in &paths {
        let d = resolve(path, Some(&s.cwd));
        if d.is_null() { errm(io, &cat(&[b"ls: cannot access '", path, b"': No such file or directory\n"])); rc = 1; continue; }
        if a.len() - first > 1 && (*d).dir != 0 { out(io, &cat(&[path, b":\n"])); }
        let list = if (*d).dir != 0 { vfs_sort_children(d); list_of(d) } else { alloc::vec![d] };
        let mut col = 0usize;
        for c in list {
            let name = node_name(c);
            if !all && name.first() == Some(&b'.') { continue; }
            let e = ext(name);
            let color: &[u8] = if (*c).dir != 0 { b"\x1b[94m" } else if e.eq_ignore_ascii_case(b"exe") || e.eq_ignore_ascii_case(b"sh") { b"\x1b[92m" } else if e.eq_ignore_ascii_case(b"dmg") { b"\x1b[95m" } else { b"" };
            let slash: &[u8] = if (*c).dir != 0 { b"/" } else { b"" };
            if lng {
                let sz = if (*c).dir != 0 { vfs_count_children(c) as u32 } else { (*c).size };
                out(io, &cat(&[if (*c).dir != 0 { b"d" } else { b"-" }, if (*c).readonly != 0 { b"r-x" } else { b"rwx" }, alloc::format!(" {:8} ", sz).as_bytes(),
                               &fmt_mtime(&(*c).mtime), b" ", color, name, slash, b"\x1b[0m\n"]));
            } else {
                let nm = cat(&[name, slash]);
                if io.out != s.con { out(io, &cat(&[&nm, b"\n"])); continue; }
                let w = if nm.len() < 24 { 26 } else { (nm.len() + 2 + 25) / 26 * 26 };
                if col + w > 78 && col != 0 { out(io, b"\n"); col = 0; }
                let mut line = cat(&[color, &nm, b"\x1b[0m"]);
                line.resize(line.len() + (w - nm.len()), b' ');
                out(io, &line);
                col += w;
            }
        }
        if col != 0 { out(io, b"\n"); }
    }
    rc
}
unsafe fn b_dir(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let path: &[u8] = if a.len() > 1 { &a[1] } else { b"." };
    let d = resolve(path, Some(&s.cwd));
    if d.is_null() || (*d).dir == 0 { errm(io, b"File Not Found\n"); return 1; }
    let full = path_of(d, 256);
    let l = vfs_drive_letter(d) as u8;
    let label = cbytes(vfs_drive_label(l as c_char));
    out(io, &cat(&[b" Volume in drive ", &[l], b" is ", label, b"\n\n Directory of ", &full, b"\n\n"]));
    vfs_sort_children(d);
    let (mut nf, mut nd, mut total) = (0, 0, 0u32);
    for c in list_of(d) {
        let t = &(*c).mtime;
        let tb = alloc::format!("{:02}/{:02}/{:04}  {:02}:{:02}", t.month, t.day, t.year, t.hour, t.minute);
        if (*c).dir != 0 { out(io, &cat(&[tb.as_bytes(), b"    <DIR>          ", node_name(c), b"\n"])); nd += 1; }
        else { out(io, &cat(&[tb.as_bytes(), alloc::format!("    {:14} ", (*c).size).as_bytes(), node_name(c), b"\n"])); nf += 1; total = total.wrapping_add((*c).size); }
    }
    out(io, alloc::format!("{:16} File(s) {:14} bytes\n{:16} Dir(s)  {:14} bytes free\n", nf, total, nd, mm_total().wrapping_sub(mm_used())).as_bytes());
    0
}
unsafe fn b_cat(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let numbered = a.len() > 1 && a[1] == b"-n";
    let (d, ok) = gather_input(s, a, if numbered { 2 } else { 1 }, io);
    if numbered { for (i, l) in sl::lines(&d).enumerate() { out(io, &cat(&[alloc::format!("{:6}  ", i + 1).as_bytes(), l, b"\n"])); } }
    else { out(io, &d); }
    if ok { 0 } else { 1 }
}
unsafe fn b_type(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    if a[0] == b"type" && a.len() > 1 && !resolve(&a[1], Some(&s.cwd)).is_null() && find_builtin(&a[1]).is_none() {
        let mut v = alloc::vec![b"cat".to_vec(), a[1].clone()];
        return b_cat(s, &mut v, io);
    }
    for x in a.iter().skip(1) {
        if s.find_func(x).is_some() { out(io, &cat(&[x, b" is a function\n"])); }
        else if find_builtin(x).is_some() { out(io, &cat(&[x, b" is a shell builtin\n"])); }
        else if is_app(x) { out(io, &cat(&[x, b" is a graphical application\n"])); }
        else if let Some(p) = resolve_external(s, x) { out(io, &cat(&[x, b" is ", &p, b"\n"])); }
        else { out(io, &cat(&[x, b": not found\n"])); return 1; }
    }
    0
}
unsafe fn b_mkdir(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let (mut p, mut rc) = (false, 0);
    for x in a.iter().skip(1) {
        if x == b"-p" { p = true; continue; }
        if !resolve(x, Some(&s.cwd)).is_null() { if !p { errm(io, &cat(&[b"mkdir: '", x, b"' already exists\n"])); rc = 1; } continue; }
        if p {
            let mut full = if has_drive(x) { vp::lcpy(x, 512) } else { vp::join(&s.cwd, x, 512) };
            vp::normalize(&mut full);
            let f = cvec(&full);
            vfs_mkdir_p(f.as_ptr() as *const c_char);
        } else {
            let (xc, cw) = (cvec(x), cvec(&s.cwd));
            if vfs_create(xc.as_ptr() as *const c_char, cw.as_ptr() as *const c_char, 1).is_null() { errm(io, &cat(&[b"mkdir: cannot create '", x, b"'\n"])); rc = 1; }
        }
    }
    rc
}
unsafe fn b_rm(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let (mut r, mut f, mut rc) = (false, false, 0);
    for x in a.iter().skip(1) {
        if x.first() == Some(&b'-') && x.len() > 1 { if x.contains(&b'r') || x.contains(&b'R') { r = true; } if x.contains(&b'f') { f = true; } continue; }
        let n = resolve(x, Some(&s.cwd));
        if n.is_null() { if !f { errm(io, &cat(&[b"rm: '", x, b"': No such file\n"])); rc = 1; } continue; }
        if (*n).dir != 0 && !r && !(*n).child.is_null() { errm(io, &cat(&[b"rm: '", x, b"' is a directory (use -r)\n"])); rc = 1; continue; }
        if vfs_delete(n) == 0 { errm(io, &cat(&[b"rm: cannot remove '", x, b"': read-only\n"])); rc = 1; }
    }
    rc
}
unsafe fn b_touch(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    for x in a.iter().skip(1) {
        let n = resolve(x, Some(&s.cwd));
        if !n.is_null() { rtc_now(&mut (*n).mtime); continue; }
        let (xc, cw) = (cvec(x), cvec(&s.cwd));
        if vfs_create(xc.as_ptr() as *const c_char, cw.as_ptr() as *const c_char, 0).is_null() { errm(io, &cat(&[b"touch: cannot create '", x, b"'\n"])); return 1; }
    }
    0
}
unsafe fn cp_mv(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io, mv: bool) -> i32 {
    let argc = a.len();
    let mut first = 1;
    while first < argc && a[first].first() == Some(&b'-') { first += 1; }
    if argc - first < 2 { errm(io, &cat(&[b"usage: ", &a[0], b" SOURCE... DEST\n"])); return 1; }
    let last = a[argc - 1].clone();
    let dst = resolve(&last, Some(&s.cwd));
    let mut rc = 0;
    for i in first..argc - 1 {
        let src = resolve(&a[i], Some(&s.cwd));
        if src.is_null() { errm(io, &cat(&[&a[0], b": '", &a[i], b"': No such file\n"])); rc = 1; continue; }
        if !dst.is_null() && (*dst).dir != 0 {
            let ex = vfs_lookup_child(dst, (*src).name.as_ptr());
            if !ex.is_null() && ex != src { vfs_delete(ex); }
            if mv { if vfs_move(src, dst) == 0 { errm(io, b"mv: failed\n"); rc = 1; } }
            else if vfs_copy(src, dst, core::ptr::null()).is_null() { errm(io, b"cp: failed\n"); rc = 1; }
        } else {
            if argc - first > 2 { errm(io, &cat(&[&a[0], b": target is not a directory\n"])); return 1; }
            let parent_dir = vp::dirname(&last, 512);
            let base = cvec(&last[vp::basename_at(&last)..]);
            if (*src).dir != 0 && !mv {
                let pd = resolve(if parent_dir.is_empty() { b"." } else { &parent_dir }, Some(&s.cwd));
                if !pd.is_null() { vfs_copy(src, pd, base.as_ptr() as *const c_char); }
                continue;
            }
            if mv {
                let pd = resolve(if parent_dir.is_empty() { b"." } else { &parent_dir }, Some(&s.cwd));
                if !dst.is_null() { vfs_delete(dst); }
                if !pd.is_null() && pd != (*src).parent { vfs_move(src, pd); }
                vfs_rename(src, base.as_ptr() as *const c_char);
            } else {
                let (lc, cw) = (cvec(&last), cvec(&s.cwd));
                let n = vfs_create(lc.as_ptr() as *const c_char, cw.as_ptr() as *const c_char, 0);
                let d = node_data(src);
                if n.is_null() || vfs_write_node(n, if d.is_empty() { b"".as_ptr() } else { d.as_ptr() } as *const c_void, (*src).size) == 0 {
                    errm(io, &cat(&[b"cp: cannot write '", &last, b"'\n"])); rc = 1;
                }
            }
        }
    }
    rc
}
unsafe fn b_cp(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { cp_mv(s, a, io, false) }
unsafe fn b_mv(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { cp_mv(s, a, io, true) }
unsafe fn b_headtail(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let tail = a[0] == b"tail";
    let (mut n, mut first) = (10, 1);
    if a.len() > 2 && a[1] == b"-n" { n = sl::atoi(&a[2]); first = 3; }
    else if a.len() > 1 && a[1].first() == Some(&b'-') && a[1].get(1).is_some_and(|c| c.is_ascii_digit()) { n = sl::atoi(&a[1][1..]); first = 2; }
    let (d, _) = gather_input(s, a, first, io);
    out(io, &sl::head_tail(&d, n, tail));
    0
}
unsafe fn b_wc(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let (mut first, mut mode) = (1, 0u8);
    if a.len() > 1 && a[1].first() == Some(&b'-') { mode = a[1].get(1).copied().unwrap_or(0); first = 2; }
    let (d, _) = gather_input(s, a, first, io);
    let (l, w, c) = sl::wc(&d);
    let o = match mode { b'l' => alloc::format!("{}\n", l), b'w' => alloc::format!("{}\n", w), b'c' => alloc::format!("{}\n", c), _ => alloc::format!("{:7} {:7} {:7}\n", l, w, c) };
    out(io, o.as_bytes());
    0
}
unsafe fn b_grep(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let mut o = sl::GrepOpts { ci: false, num: false, inv: false, count: false, quiet: false, after: 0 };
    let mut i = 1;
    while i < a.len() && a[i].first() == Some(&b'-') && a[i].len() > 1 {
        if a[i][1] == b'A' {
            o.after = if a[i].len() > 2 { sl::atoi(&a[i][2..]) } else if i + 1 < a.len() { i += 1; sl::atoi(&a[i]) } else { 0 };
            i += 1; continue;
        }
        for &f in &a[i][1..] { match f { b'i' => o.ci = true, b'n' => o.num = true, b'v' => o.inv = true, b'c' => o.count = true, b'q' => o.quiet = true, _ => {} } }
        i += 1;
    }
    if i >= a.len() { errm(io, b"usage: grep [-invc] PATTERN [FILE...]\n"); return 2; }
    let pat = a[i].clone();
    let (d, _) = gather_input(s, a, i + 1, io);
    let (text, matches) = sl::grep(&d, &pat, &o);
    out(io, &text);
    if matches > 0 { 0 } else { 1 }
}
unsafe fn b_sort(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let (mut rev, mut numeric, mut uniq, mut first) = (false, false, false, 1);
    while first < a.len() && a[first].first() == Some(&b'-') { let x = &a[first]; rev |= x.contains(&b'r'); numeric |= x.contains(&b'n'); uniq |= x.contains(&b'u'); first += 1; }
    let (d, _) = gather_input(s, a, first, io);
    out(io, &sl::sort(&d, rev, numeric, uniq));
    0
}
unsafe fn b_uniq(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { let (d, _) = gather_input(s, a, 1, io); out(io, &sl::uniq(&d)); 0 }
unsafe fn b_rev(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { let (d, _) = gather_input(s, a, 1, io); out(io, &sl::rev(&d)); 0 }
unsafe fn b_tr(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    if a.len() >= 2 && a[1] == b"-d" {
        if a.len() < 3 { return 1; }
        let (d, _) = gather_input(s, a, 3, io);
        out(io, &sl::tr(&d, &a[2], None));
        return 0;
    }
    if a.len() < 3 { errm(io, b"usage: tr SET1 SET2 | tr -d SET\n"); return 1; }
    let (d, _) = gather_input(s, a, 3, io);
    out(io, &sl::tr(&d, &a[1], Some(&a[2])));
    0
}
unsafe fn b_clear(_s: &mut Sh, _a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { if !io.out.is_null() { if let Some(f) = (*io.out).clear { f(io.out); } } 0 }
unsafe fn b_exit(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 { s.exiting = true; s.exit_code = if a.len() > 1 { sl::atoi(&a[1]) } else { s.status }; s.exit_code }
unsafe fn b_true(_s: &mut Sh, _a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 { 0 }
unsafe fn b_false(_s: &mut Sh, _a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 { 1 }
unsafe fn b_sleep(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 {
    if a.len() < 2 { return 1; }
    let p = &a[1];
    let mut i = 0;
    let mut ms: u32 = 0;
    while i < p.len() && p[i].is_ascii_digit() { ms = ms.wrapping_mul(10).wrapping_add((p[i] - b'0') as u32); i += 1; }
    ms = ms.wrapping_mul(1000);
    if i < p.len() && p[i] == b'.' {
        i += 1;
        let mut scale = 100;
        while i < p.len() && p[i].is_ascii_digit() && scale != 0 { ms = ms.wrapping_add((p[i] - b'0') as u32 * scale); i += 1; scale /= 10; }
    }
    let end = timer_ticks().wrapping_add(ms);
    while (timer_ticks().wrapping_sub(end) as i32) < 0 {
        if s.interrupted() { return 130; }
        task_sleep(50u32.min(end.wrapping_sub(timer_ticks())));
    }
    0
}
unsafe fn now() -> sl::Date {
    let mut t = RtcTime { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0, weekday: 0 };
    rtc_now(&mut t);
    sl::Date { year: t.year, month: t.month, day: t.day, hour: t.hour, minute: t.minute, second: t.second, weekday: t.weekday }
}
unsafe fn b_date(_s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let t = now();
    if a.len() > 1 && a[1].first() == Some(&b'+') { out(io, &cat(&[&sl::date_format(&a[1][1..], &t), b"\n"])); return 0; }
    out(io, &sl::date_default(&t));
    0
}
unsafe fn b_export(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 {
    for x in a.iter().skip(1) {
        let name = match x.iter().position(|&c| c == b'=') { Some(eq) => { s.var_set(&x[..eq], &x[eq + 1..]); &x[..eq] } None => &x[..] };
        if !name.is_empty() && !s.exports.iter().any(|e| e == name) { s.exports.push(name.to_vec()); }
    }
    0
}
unsafe fn b_unset(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 { for x in a.iter().skip(1) { s.var_unset(x); } 0 }
unsafe fn b_env(s: &mut Sh, _a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    for (k, v) in s.vars.iter().rev() { out(io, &cat(&[k, b"=", v, b"\n"])); }
    0
}
unsafe fn b_read(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let mut i = 1;
    let mut prompt = None;
    while i < a.len() && a[i].first() == Some(&b'-') { if a[i] == b"-p" && i + 1 < a.len() { prompt = Some(a[i + 1].clone()); i += 2; } else { i += 1; } }
    if let Some(p) = prompt { out(io, &p); }
    let Some(line) = creadline(io.inp, 512) else { return 1 };
    if i >= a.len() { s.var_set(b"REPLY", &line); return 0; }
    let mut p = 0;
    while i < a.len() {
        while p < line.len() && (line[p] == b' ' || line[p] == b'\t') { p += 1; }
        if i == a.len() - 1 { let n = a[i].clone(); s.var_set(&n, &line[p..]); break; }
        let st = p;
        while p < line.len() && line[p] != b' ' && line[p] != b'\t' { p += 1; }
        let n = a[i].clone();
        s.var_set(&n, &line[st..p]);
        i += 1;
    }
    0
}
unsafe fn b_test(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let mut n = a.len() - 1;
    if a[0] == b"[" || a[0] == b"[[" {
        if n < 1 || (a[n] != b"]" && a[n] != b"]]") { errm(io, b"[: missing ']'\n"); return 2; }
        n -= 1;
    }
    let args: Vec<&[u8]> = a[1..1 + n].iter().map(|x| x.as_slice()).collect();
    let cwd = s.cwd.clone();
    let mut stat = |p: &[u8]| -> Option<sl::FileInfo> {
        let f = resolve(p, Some(&cwd));
        if f.is_null() { None } else { Some(sl::FileInfo { dir: (*f).dir != 0, size: (*f).size, readonly: (*f).readonly != 0 }) }
    };
    if sl::test_eval(&args, &mut stat) { 0 } else { 1 }
}
unsafe fn b_seq(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let (mut x, mut y, mut st): (sl::Long, sl::Long, sl::Long) = (1, 1, 1);
    match a.len() {
        2 => y = sl::atoi(&a[1]) as sl::Long,
        3 => { x = sl::atoi(&a[1]) as sl::Long; y = sl::atoi(&a[2]) as sl::Long; }
        n if n >= 4 => { x = sl::atoi(&a[1]) as sl::Long; st = sl::atoi(&a[2]) as sl::Long; y = sl::atoi(&a[3]) as sl::Long; }
        _ => {}
    }
    if st == 0 { return 1; }
    if !(if st > 0 { x <= y } else { x >= y }) { return 0; }
    let mut i = x;
    let mut k = 0u32;
    loop {
        out(io, &cat(&[&sl::ltoa(i), b"\n"]));
        k += 1;
        if k % 256 == 0 && s.interrupted() { return 130; }
        match sl::seq_next(i, st, y) { Some(n) => i = n, None => break }
    }
    0
}
unsafe fn b_expr(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let mut buf = Vec::new();
    for x in a.iter().skip(1) { vp::lcat(&mut buf, x, 512); vp::lcat(&mut buf, b" ", 512); }
    let v = arith(s, &buf);
    out(io, &cat(&[&sl::ltoa(v), b"\n"]));
    (v == 0) as i32
}
unsafe fn b_let(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 {
    let mut v = 0;
    for x in a.clone().iter().skip(1) { v = arith(s, x); }
    (v == 0) as i32
}
unsafe fn b_basename(_s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    if a.len() < 2 { return 1; }
    let mut b = vp::lcpy(&a[1][vp::basename_at(&a[1])..], 256);
    if a.len() > 2 && ends_ci(&b, &a[2]) && a[2].len() < b.len() { let l = b.len() - a[2].len(); b.truncate(l); }
    out(io, &cat(&[&b, b"\n"]));
    0
}
unsafe fn b_dirname(_s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    if a.len() < 2 { return 1; }
    let d = vp::dirname(&a[1], 256);
    out(io, &cat(&[if d.is_empty() { b"." } else { &d }, b"\n"]));
    0
}
unsafe fn b_help(_s: &mut Sh, _a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    out(io, b"\x1b[93mWRLD shell - built-in commands:\x1b[0m\n");
    let mut col = 0;
    for b in builtins() {
        let Some(h) = b.help else { continue };
        let name = core::str::from_utf8(b.name).unwrap_or("");
        out(io, alloc::format!("  \x1b[96m{:<9}\x1b[0m {:<28}", name, h).as_bytes());
        col += 1;
        if col == 2 { out(io, b"\n"); col = 0; }
    }
    if col != 0 { out(io, b"\n"); }
    out(io, b"\n\x1b[93mApplications:\x1b[0m notepad calc paint explorer control taskmgr snake winver\n");
    out(io, b"\x1b[93mPrograms:\x1b[0m run FILE.exe (Win32 PE), FILE.sh (shell script), open FILE.dmg (disk image)\n");
    out(io, b"\x1b[93mSyntax:\x1b[0m VAR=x  $VAR ${VAR:-d}  $(cmd)  $((1+2))  a | b  > >> < 2>&1  && ||\n");
    out(io, b"        if/elif/else/fi  for x in ...; do ... done  while/until  case ... esac  f() { ... }\n");
    0
}
unsafe fn b_history(_s: &mut Sh, _a: &mut Vec<Vec<u8>>, io: &Io) -> i32 { out(io, b"Use the Up/Down arrow keys to browse command history.\n"); 0 }
unsafe fn b_source(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    if a.len() < 2 { return 1; }
    let n = resolve(&a[1], Some(&s.cwd));
    if n.is_null() || (*n).dir != 0 { errm(io, &cat(&[b"source: ", &a[1], b": not found\n"])); return 1; }
    let src = sl::cstr(node_data(n)).to_vec();
    run_string(s, &src, io)
}
unsafe fn b_color(_s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let Some(f) = (if io.out.is_null() { None } else { (*io.out).set_color }) else { return 0 };
    if a.len() < 2 { f(io.out, 7, 0); return 0; }
    let v = sl::strtol(&a[1], 16) as c_int;
    f(io.out, v & 15, (v >> 4) & 15);
    0
}
unsafe fn b_title(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 {
    let mut b = Vec::new();
    for (i, x) in a.iter().enumerate().skip(1) { if i > 1 { vp::lcat(&mut b, b" ", 128); } vp::lcat(&mut b, x, 128); }
    if !s.con.is_null() { if let Some(f) = (*s.con).set_title { let t = cvec(&b); f(s.con, t.as_ptr() as *const c_char); } }
    0
}
unsafe fn tree_rec(io: &Io, d: *mut Vnode, prefix: &mut Vec<u8>, depth: u32) {
    if depth > 12 { return; }
    vfs_sort_children(d);
    let kids = list_of(d);
    for (i, &c) in kids.iter().enumerate() {
        let last = i + 1 == kids.len();
        out(io, &cat(&[prefix, if last { b"\\---" } else { b"+---" }, node_name(c), b"\n"]));
        if (*c).dir != 0 {
            let l = prefix.len();
            prefix.extend_from_slice(if last { b"    " } else { b"|   " });
            tree_rec(io, c, prefix, depth + 1);
            prefix.truncate(l);
        }
    }
}
unsafe fn b_tree(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let d = resolve(if a.len() > 1 { &a[1] } else { b"." }, Some(&s.cwd));
    if d.is_null() || (*d).dir == 0 { errm(io, b"tree: invalid path\n"); return 1; }
    out(io, &cat(&[&path_of(d, 256), b"\n"]));
    let mut prefix = Vec::new();
    tree_rec(io, d, &mut prefix, 0);
    0
}
unsafe fn b_find(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let mut start = b".".to_vec();
    let mut pat: Option<Vec<u8>> = None;
    let mut i = 1;
    while i < a.len() { if a[i] == b"-name" && i + 1 < a.len() { i += 1; pat = Some(a[i].clone()); } else { start = a[i].clone(); } i += 1; }
    let d = resolve(&start, Some(&s.cwd));
    if d.is_null() { errm(io, &cat(&[b"find: '", &start, b"': No such file or directory\n"])); return 1; }
    // depth first, in folder order, without recursion: (folder's remaining children, its path)
    let mut stack: Vec<(Vec<*mut Vnode>, usize, Vec<u8>)> = alloc::vec![(list_of(d), 0, start)];
    while let Some(top) = stack.last_mut() {
        if top.1 >= top.0.len() { stack.pop(); continue; }
        let c = top.0[top.1];
        top.1 += 1;
        let p = cat(&[&top.2, b"\\", node_name(c)]);
        if pat.as_ref().map_or(true, |pt| sl::glob_match(pt, node_name(c))) { out(io, &cat(&[&p, b"\n"])); }
        if (*c).dir != 0 { stack.push((list_of(c), 0, p)); }
    }
    0
}
unsafe fn b_hexdump(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    if a.len() < 2 { return 1; }
    let n = resolve(&a[a.len() - 1], Some(&s.cwd));
    if n.is_null() || (*n).dir != 0 { errm(io, b"hexdump: cannot open\n"); return 1; }
    out(io, &sl::hexdump(node_data(n)));
    0
}
unsafe fn b_return(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 { s.ret = true; if a.len() > 1 { sl::atoi(&a[1]) } else { s.status } }
unsafe fn b_break(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 { s.brk = if a.len() > 1 { sl::atoi(&a[1]) } else { 1 }; 0 }
unsafe fn b_continue(s: &mut Sh, _a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 { s.cont = 1; 0 }
unsafe fn b_shift(s: &mut Sh, a: &mut Vec<Vec<u8>>, _io: &Io) -> i32 {
    let mut n = if a.len() > 1 { sl::atoi(&a[1]) } else { 1 };
    while n > 0 && s.argv.len() > 1 { s.argv.remove(1); n -= 1; }
    0
}
unsafe fn b_pause(s: &mut Sh, _a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    out(io, b"Press any key to continue . . . ");
    if !s.con.is_null() { if let Some(f) = (*s.con).getkey { f(s.con); } }
    out(io, b"\n");
    0
}
unsafe fn b_xargs(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let (d, _) = gather_input(s, &a[..1], 1, io);
    let mut v: Vec<Vec<u8>> = a[1..].to_vec();
    if v.is_empty() { v.push(b"echo".to_vec()); }
    v.extend(d.split(|c| matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c)).filter(|w| !w.is_empty()).map(|w| w.to_vec()));
    match find_builtin(&v[0]) { Some(Bf::R(f)) => f(s, &mut v, io), Some(Bf::C) => call_c(s, &v, io), None => run_external(s, &mut v, io) }
}
/// unzip ARCHIVE [-d DIR] [-l], unrar x|e|l ARCHIVE [DIR]: ZIP and RAR archives (crate::unarc)
unsafe fn b_unzip(s: &mut Sh, a: &mut Vec<Vec<u8>>, io: &Io) -> i32 {
    let cmd = a[0].clone();
    let rar = cmd.eq_ignore_ascii_case(b"unrar");
    let (mut zip, mut dest, mut list, mut flat) = (None, None, false, false);
    let mut i = 1;
    while i < a.len() {
        match a[i].as_slice() {
            b"-d" if i + 1 < a.len() => { i += 1; dest = Some(a[i].clone()); }
            b"-l" | b"l" | b"lb" | b"v" if !(zip.is_none() && !rar && a[i] != b"-l") => list = true,
            b"x" if rar && zip.is_none() => {}
            b"e" if rar && zip.is_none() => flat = true,
            b"-o" | b"-q" | b"-y" | b"-o+" => {}
            x if zip.is_none() => zip = Some(x.to_vec()),
            x => dest = Some(x.to_vec()),
        }
        i += 1;
    }
    let usage: &[u8] = if rar { b"usage: unrar x|e|l ARCHIVE [DIR]\n" } else { b"usage: unzip ARCHIVE [-d DIR] [-l]\n" };
    let Some(zip) = zip else { errm(io, usage); return 1 };
    let z = resolve(&zip, Some(&s.cwd));
    if z.is_null() || (*z).dir != 0 { errm(io, &cat(&[&cmd, b": cannot open ", &zip, b"\n"])); return 1; }
    let d = node_data(z).to_vec();
    if list {
        let (mut files, mut bytes) = (0u32, 0u64);
        let r = crate::unarc::list(&d, &mut |m| {
            if !m.dir { out(io, &cat(&[alloc::format!("{:12}  ", m.size).as_bytes(), m.name, b"\n"])); files += 1; bytes += m.size; }
            true
        });
        if let Err(e) = r { errm(io, &cat(&[&cmd, b": ", &zip, b": ", e.text().as_bytes(), b"\n"])); return 1; }
        out(io, alloc::format!("{} file(s), {} KB\n", files, bytes / 1024).as_bytes());
        return 0;
    }
    let mut root = match &dest { Some(x) if has_drive(x) => vp::lcpy(x, 512), Some(x) => vp::join(&s.cwd, x, 512), None => vp::lcpy(&s.cwd, 512) };
    vp::normalize(&mut root);
    let mut st = ArcStatus::zeroed();
    let n = extract_into(&d, &root, flat, &mut st, Some(io));
    if n < 0 { errm(io, &cat(&[&cmd, b": ", &zip, b": ", sl::cstr(&st.error), b"\n"])); return 1; }
    out(io, alloc::format!("extracted {} file(s){}\n", st.files, if st.failed > 0 { alloc::format!(", {} failed", st.failed) } else { alloc::string::String::new() }).as_bytes());
    if st.failed > 0 { 1 } else { 0 }
}

/// How an extraction is going, shared with Explorer's Extract dialog (src/apps/extract.c).
#[repr(C)]
pub struct ArcStatus {
    pub percent: c_int,
    pub files: c_int,
    pub failed: c_int,
    pub done: CBool,
    pub cancel: CBool,
    pub current: [u8; 160],
    /// the archive's own problem (when the result is -1), or the first member that failed
    pub error: [u8; 256],
}
impl ArcStatus { fn zeroed() -> ArcStatus { ArcStatus { percent: 0, files: 0, failed: 0, done: 0, cancel: 0, current: [0; 160], error: [0; 256] } } }
fn set_cstr(dst: &mut [u8], s: &[u8]) { let n = s.len().min(dst.len() - 1); dst[..n].copy_from_slice(&s[..n]); dst[n] = 0; }

/// Progress inside the member being decoded: the archive span it covers, and its size
static mut ARC_ST: *mut ArcStatus = null_mut();
static mut ARC_SPAN: (usize, usize, usize, u64) = (0, 0, 1, 1);
fn arc_tick(done: u64) {
    unsafe {
        if !ARC_ST.is_null() {
            let (a, b, total, size) = ARC_SPAN;
            let at = a + ((b.saturating_sub(a)) as u64 * done.min(size) / size.max(1)) as usize;
            (*ARC_ST).percent = (at as u64 * 100 / total.max(1) as u64) as c_int;
        }
        task_yield();
    }
}

/// Extracts archive bytes `d` into the folder `root` (made where missing); with `flat`, every
/// file goes straight into `root`. Returns the files written, or -1 when the archive itself
/// could not be read (the reason is in st.error).
unsafe fn extract_into(d: &[u8], root: &[u8], flat: bool, st: &mut ArcStatus, io: Option<&Io>) -> i32 {
    crate::unarc::set_tick(arc_tick);
    if !mkdirs(root) { set_cstr(&mut st.error, &cat(&[b"cannot create the folder ", root])); return -1; }
    ARC_ST = st as *mut ArcStatus;
    let total = d.len().max(1);
    let mut last_at = 0usize;
    // the span of the next member is known only once it is handed over; until then the
    // tick estimates within [last member end, archive end]
    ARC_SPAN = (0, total, total, u64::MAX);
    let r = crate::unarc::extract(d, &mut |m| {
        let name = if flat { match m.name.iter().rposition(|&c| c == b'\\') { Some(k) => &m.name[k + 1..], None => m.name } } else { m.name };
        set_cstr(&mut st.current, name);
        st.percent = (m.at as u64 * 100 / total as u64) as c_int;
        ARC_SPAN = (m.at, total, total, u64::MAX);
        last_at = m.at;
        let full = vp::join(root, name, 512);
        let fail = |st: &mut ArcStatus, why: &[u8]| {
            st.failed += 1;
            if st.error[0] == 0 { set_cstr(&mut st.error, &cat(&[name, b": ", why])); }
            if let Some(io) = io { errm(io, &cat(&[name, b": ", why, b"\n"])); }
        };
        if m.dir { if !flat && !mkdirs(&full) { fail(st, b"cannot create the folder"); } }
        else {
            match &m.data {
                Err(e) => fail(st, e.text().as_bytes()),
                Ok(v) => {
                    let dir = vp::dirname(&full, 512);
                    if !mkdirs(&dir) { fail(st, b"cannot create its folder"); }
                    else if v.len() > u32::MAX as usize || vfs_write_file(cvec(&full).as_ptr() as *const c_char, v.as_ptr() as *const c_void, v.len() as u32).is_null() { fail(st, b"cannot be written (is the disk full?)"); }
                    else { st.files += 1; }
                }
            }
        }
        task_yield();
        st.cancel == 0
    });
    ARC_ST = null_mut();
    let _ = last_at;
    match r {
        Ok(_) => { st.percent = 100; st.files }
        Err(e) => { set_cstr(&mut st.error, e.text().as_bytes()); -1 }
    }
}

/// For Explorer: extracts the archive at `arc` into `dest`, updating `st` as it goes (run it
/// on its own task; it yields often). Sets st.done at the end.
#[no_mangle]
pub unsafe extern "C" fn archive_extract(arc: *const c_char, dest: *const c_char, st: *mut ArcStatus) -> c_int {
    if st.is_null() { return -1; }
    let st = &mut *st;
    let (a, dst) = (cbytes(arc).to_vec(), cbytes(dest).to_vec());
    let z = resolve(&a, None);
    let n = if z.is_null() || (*z).dir != 0 { set_cstr(&mut st.error, b"the archive could not be opened"); -1 }
        else {
            let mut d = Vec::new();
            let src = node_data(z);
            if d.try_reserve_exact(src.len()).is_err() { set_cstr(&mut st.error, b"not enough memory to open the archive"); -1 }
            else { d.extend_from_slice(src); let mut root = vp::lcpy(&dst, 512); vp::normalize(&mut root); extract_into(&d, &root, false, st, None) }
        };
    st.done = 1;
    n
}

/// For Explorer: 1 when `path` is a ZIP or RAR archive this OS can open.
#[no_mangle]
pub unsafe extern "C" fn archive_detect(path: *const c_char) -> CBool {
    let z = resolve(cbytes(path), None);
    if z.is_null() || (*z).dir != 0 { return 0; }
    crate::unarc::kind(node_data(z)).is_some() as CBool
}
/// Every folder of an absolute path ("C:\a\b"), made where missing.
unsafe fn mkdirs(path: &[u8]) -> bool {
    let b = vp::lcpy(path, 512);
    let mut k = 3.min(b.len());
    loop {
        if k >= b.len() || b[k] == b'\\' || b[k] == b'/' {
            let pre = &b[..k];
            if !pre.is_empty() {
                let v = resolve(pre, None);
                if v.is_null() { let p = cvec(pre); if vfs_create(p.as_ptr() as *const c_char, core::ptr::null(), 1).is_null() { return false; } }
                else if (*v).dir == 0 { return false; }
            }
            if k >= b.len() { break; }
        }
        k += 1;
    }
    true
}

// ---------------------------------------------------------------- the C interface

unsafe fn c_args(argc: c_int, argv: *mut *mut c_char) -> Vec<Vec<u8>> {
    if argv.is_null() { return Vec::new(); }
    (0..argc.max(0) as usize).map(|i| cbytes(*argv.add(i)).to_vec()).collect()
}
unsafe fn opt_cwd<'a>(cwd: *const c_char) -> Option<&'a [u8]> { if cwd.is_null() { None } else { Some(cbytes(cwd)) } }

#[no_mangle]
pub unsafe extern "C" fn sh_run_file(con: *mut Console, path: *const c_char, argc: c_int, argv: *mut *mut c_char, cwd: *const c_char) -> c_int {
    let p = Sh::new(con, opt_cwd(cwd));
    let io = Io { inp: con, out: con, err: con };
    run_file_io(&p, cbytes(path), &c_args(argc, argv), &io)
}
#[no_mangle]
pub unsafe extern "C" fn sh_run_string(con: *mut Console, script: *const c_char, cwd: *const c_char) -> c_int {
    let mut s = Sh::new(con, opt_cwd(cwd));
    let io = Io { inp: con, out: con, err: con };
    run_string(&mut s, cbytes(script), &io)
}
#[no_mangle]
pub unsafe extern "C" fn sh_interactive(con: *mut Console, cwd: *const c_char) {
    let mut s = Sh::new(con, opt_cwd(cwd));
    let io = Io { inp: con, out: con, err: con };
    set_terminal_cwd(&s);
    let rc = resolve(&cat(&[USER_HOME, b"\\.profile"]), None);
    if !rc.is_null() && (*rc).dir == 0 { let src = sl::cstr(node_data(rc)).to_vec(); run_string(&mut s, &src, &io); }
    let mut acc: Vec<u8> = Vec::new();
    while !s.exiting {
        if acc.is_empty() { cwrite(con, &cat(&[b"\x1b[92m", &username(), b"\x1b[0m@wrld \x1b[93m", &s.cwd, b"\x1b[0m$ "])); }
        else { cwrite(con, b"> "); }
        let Some(line) = creadline(con, 512) else { break };
        console_clear_interrupt(con);
        let had = !acc.is_empty();
        if had { acc.push(b'\n'); }
        acc.extend_from_slice(&line);
        if acc.is_empty() { continue; }
        match sl::parse(&acc) {
            Err(sl::ParseError::Incomplete) => continue,
            Err(sl::ParseError::Syntax(m)) => errm(&io, &cat(&[b"sh: ", &m, b"\n"])),
            Ok(tree) => { s.status = run_node(&mut s, tree.as_ref(), &io); }
        }
        console_clear_interrupt(con);
        acc.clear();
        set_terminal_cwd(&s);
    }
}
#[no_mangle]
pub unsafe extern "C" fn sh_spawn_script(path: *const c_char, cwd: *const c_char) { terminal_open(path, cwd); }
