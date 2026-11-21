//! Linux software packages' C functions (`crate::pkgfmt`; was the reading and checking half of
//! `src/apps/pkg.c`): pkg_load fills the C `pkg_t` the installer and the package windows
//! use, pkg_check fills `pkg_check_t`, and the helpers the installer still calls (finding an
//! entry, paths, `.desktop` entries, update-alternatives lines, gunzip) are here too.
//! Installing, removing and the package database stay in C.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};
use core::ptr::{null, null_mut};
use super::{CBool, into_c_buffer};
use super::vfsk::{path_basename, vfs_resolve};
use crate::pkgfmt::{self, Bytes, Package, PkgView, Sys, Unpack};

/// pkg_entry_t
#[repr(C)]
pub struct PkgEntry { path: [u8; 256], link: [u8; 256], data: *const u8, size: u32, ty: u8 }
/// pkg_t
#[repr(C)]
pub struct PkgT {
    fmt: c_char, file: [u8; 256], name: [u8; 64], version: [u8; 96], arch: [u8; 24],
    summary: [u8; 160], maint: [u8; 128], homepage: [u8; 160], section: [u8; 48], license: [u8; 64],
    desc: *mut u8, depends: *mut u8, provides: *mut u8,
    inst_kb: u32, comp: [u8; 8], scripts: [u8; 96], postinst: *mut u8,
    payload: *mut u8, payload_len: u32,
    ents: *mut PkgEntry, nents: c_int, cap: c_int,
    pidx: *mut c_int, bidx: *mut c_int, icap: c_int,
    nfiles: c_int,
    err: [u8; 200],
}
/// pkg_check_t
#[repr(C)]
pub struct PkgCheck { missing_pkgs: [u8; 512], missing_libs: [u8; 512], missing_vers: [u8; 512], missing_interp: [u8; 256], programs: [u8; 512], foreign_elf: CBool, ok: CBool }

extern "C" {
    static rust_pkg_layout: [u32; 11];
    fn unpack_all(s: *const u8, n: u32, max: u32, outlen: *mut u32, err: *mut c_char, ecap: c_int) -> *mut u8;
    fn unpack_yield();
    fn mm_total() -> u32;
    fn mm_used() -> u32;
    /// src/apps/pkg.c: a Debian dependency met by an installed package or the OS image
    fn pkg_dep_met(name: *const c_char) -> CBool;
    fn kmalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
}
/// rust_init checks the structures above against src/include/pkg.h.
pub fn layout_ok() -> bool {
    use core::mem::{offset_of, size_of};
    let want = unsafe { rust_pkg_layout };
    want == [size_of::<PkgT>() as u32, offset_of!(PkgT, desc) as u32, offset_of!(PkgT, payload) as u32, offset_of!(PkgT, ents) as u32,
             offset_of!(PkgT, nfiles) as u32, offset_of!(PkgT, err) as u32, size_of::<PkgEntry>() as u32, offset_of!(PkgEntry, data) as u32,
             offset_of!(PkgEntry, ty) as u32, size_of::<PkgCheck>() as u32, offset_of!(PkgCheck, foreign_elf) as u32]
}

unsafe fn cs<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0;
    while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
/// strlcpy into a fixed C array
fn put(dst: &mut [u8], s: &[u8]) { let n = s.len().min(dst.len() - 1); dst[..n].copy_from_slice(&s[..n]); dst[n] = 0; }
unsafe fn put_raw(dst: *mut c_char, cap: c_int, s: &[u8]) {
    if dst.is_null() || cap <= 0 { return; }
    let n = s.len().min(cap as usize - 1);
    core::ptr::copy_nonoverlapping(s.as_ptr(), dst as *mut u8, n);
    *dst.add(n) = 0;
}
/// A kmalloc'd C string (NULL when out of memory).
unsafe fn cdup(s: &[u8]) -> *mut u8 {
    let p = kmalloc(s.len() + 1) as *mut u8;
    if !p.is_null() { core::ptr::copy_nonoverlapping(s.as_ptr(), p, s.len()); *p.add(s.len()) = 0; }
    p
}

/// A buffer from unpack_all (kernel heap), handed on to pkg_t as it is.
struct CBuf(*mut u8, usize);
impl Bytes for CBuf {
    fn bytes(&self) -> &[u8] { if self.0.is_null() { &[] } else { unsafe { core::slice::from_raw_parts(self.0, self.1) } } }
    fn leak(self: Box<Self>) -> Option<*mut u8> { let p = self.0; core::mem::forget(self); Some(p) }
}
impl Drop for CBuf { fn drop(&mut self) { unsafe { kfree(self.0 as *mut c_void) } } }
/// gunzip's output (Rust heap, which is the kernel heap).
struct RVec(Vec<u8>);
impl Bytes for RVec {
    fn bytes(&self) -> &[u8] { &self.0 }
    fn leak(self: Box<Self>) -> Option<*mut u8> { Some(into_c_buffer(self.0)) }
}
struct KUnpack;
impl Unpack for KUnpack {
    fn unpack(&self, d: &[u8], max: u32) -> Result<Box<dyn Bytes>, Vec<u8>> {
        if pkgfmt::unpack_kind(d) == 1 { return pkgfmt::gunzip(d, max).map(|v| Box::new(RVec(v)) as Box<dyn Bytes>).map_err(|e| e.to_vec()); }
        let (mut n, mut e) = (0u32, [0u8; 120]);
        let o = unsafe { unpack_all(d.as_ptr(), d.len() as u32, max, &mut n, e.as_mut_ptr() as *mut c_char, e.len() as c_int) };
        if o.is_null() { return Err(pkgfmt::cstr(&e).to_vec()); }
        Ok(Box::new(CBuf(o, n as usize)))
    }
}
/// Leave room for the installed copies: half the free memory, at most 1.5 GB.
fn unpack_limit() -> u32 {
    let (t, u) = unsafe { (mm_total() as u64, mm_used() as u64) };
    let free_mb = t.saturating_sub(u);
    (free_mb * 1024 * 1024 / 2).min(0x6000_0000) as u32
}

fn hstr(s: &[u8]) -> u32 { s.iter().fold(2166136261u32, |h, &c| (h ^ c as u32).wrapping_mul(16777619)) }
fn base_of(p: &[u8]) -> &[u8] { match p.iter().rposition(|&c| c == b'/') { Some(i) => &p[i + 1..], None => p } }
/// Index the entries by path and by base name (-1 = empty slot), as the C code did.
unsafe fn build_index(p: &mut PkgT) {
    let n = p.nents.max(0) as usize;
    let mut cap = 64usize;
    while cap < n * 2 + 2 { cap *= 2; }
    let (pi, bi) = (kmalloc(cap * 4) as *mut c_int, kmalloc(cap * 4) as *mut c_int);
    if pi.is_null() || bi.is_null() { kfree(pi as *mut c_void); kfree(bi as *mut c_void); return; }
    for i in 0..cap { *pi.add(i) = -1; *bi.add(i) = -1; }
    for i in 0..n {
        let e = &*p.ents.add(i);
        let path = pkgfmt::cstr(&e.path);
        let mut h = hstr(path) as usize & (cap - 1);
        while *pi.add(h) >= 0 { h = (h + 1) & (cap - 1); }
        *pi.add(h) = i as c_int;
        if e.ty == pkgfmt::PE_DIR { continue; }
        h = hstr(base_of(path)) as usize & (cap - 1);
        while *bi.add(h) >= 0 { h = (h + 1) & (cap - 1); }
        *bi.add(h) = i as c_int;
    }
    p.pidx = pi; p.bidx = bi; p.icap = cap as c_int;
}

/// bool pkg_load(const char *wrld_path, pkg_t *p): false with p->err saying why.
#[no_mangle]
pub unsafe extern "C" fn pkg_load(wrld_path: *const c_char, p: *mut PkgT) -> CBool {
    core::ptr::write_bytes(p, 0, 1);
    let p = &mut *p;
    let path = cs(wrld_path as *const u8);
    put(&mut p.file, path);
    let n = vfs_resolve(wrld_path, null());
    if n.is_null() || (*n).dir != 0 {
        let mut m = b"The file ".to_vec(); m.extend_from_slice(path); m.extend_from_slice(b" was not found.");
        put(&mut p.err, &m); return 0;
    }
    let d: &[u8] = if (*n).data.is_null() { &[] } else { core::slice::from_raw_parts((*n).data, (*n).size as usize) };
    let is_deb = d.len() >= 8 && &d[..8] == b"!<arch>\n";
    let is_rpm = d.len() >= 4 && d[..4] == [0xED, 0xAB, 0xEE, 0xDB];
    if !is_deb && !is_rpm {
        let mut m = cs(path_basename(wrld_path) as *const u8).to_vec(); m.extend_from_slice(b" is not a .deb or .rpm package.");
        put(&mut p.err, &m); return 0;
    }
    p.fmt = if is_deb { b'd' } else { b'r' } as c_char;
    let mut pkg = match Package::load(d, &KUnpack, unpack_limit()) { Ok(x) => x, Err(e) => { put(&mut p.err, &e); return 0; } };
    put(&mut p.name, &pkg.name); put(&mut p.version, &pkg.version); put(&mut p.arch, &pkg.arch);
    put(&mut p.summary, &pkg.summary); put(&mut p.maint, &pkg.maint); put(&mut p.homepage, &pkg.homepage);
    put(&mut p.section, &pkg.section); put(&mut p.license, &pkg.license);
    put(&mut p.comp, &pkg.comp); put(&mut p.scripts, &pkg.scripts);
    p.inst_kb = pkg.inst_kb;
    p.desc = pkg.desc.as_deref().map_or(null_mut(), |s| cdup(s));
    p.depends = pkg.depends.as_deref().map_or(null_mut(), |s| cdup(s));
    p.provides = pkg.provides.as_deref().map_or(null_mut(), |s| cdup(s));
    p.postinst = pkg.postinst.as_deref().map_or(null_mut(), |s| cdup(s));
    p.nfiles = pkg.nfiles;
    let len = pkg.payload().len();
    let base = match pkg.payload.take() {
        Some(b) => match b.leak() { Some(ptr) => ptr, None => null_mut() },
        None => null_mut(),
    };
    p.payload = base; p.payload_len = len as u32;
    let ne = pkg.ents.len();
    if ne > 0 {
        let ents = kmalloc(ne * core::mem::size_of::<PkgEntry>()) as *mut PkgEntry;
        if ents.is_null() { put(&mut p.err, b"There is not enough memory to read the package."); return 0; }
        for (i, e) in pkg.ents.iter().enumerate() {
            let mut pe = PkgEntry { path: [0; 256], link: [0; 256], data: null(), size: e.size, ty: e.ty };
            put(&mut pe.path, &e.path); put(&mut pe.link, &e.link);
            if e.ty == pkgfmt::PE_FILE && !base.is_null() { pe.data = base.add(e.data); }
            ents.add(i).write(pe);
        }
        p.ents = ents; p.nents = ne as c_int; p.cap = ne as c_int;
    }
    build_index(p);
    1
}

/// The C pkg_t as the check sees it, through its own index.
struct CPkg<'a>(&'a PkgT);
impl CPkg<'_> {
    unsafe fn ent(&self, i: usize) -> &PkgEntry { &*self.0.ents.add(i) }
    fn probe(&self, tab: *mut c_int, key: &[u8], f: &dyn Fn(&[u8]) -> &[u8]) -> Option<usize> {
        let cap = self.0.icap as usize;
        if cap == 0 || tab.is_null() { return None; }
        unsafe {
            let mut h = hstr(key) as usize & (cap - 1);
            while *tab.add(h) >= 0 {
                let i = *tab.add(h) as usize;
                if f(pkgfmt::cstr(&self.ent(i).path)) == key { return Some(i); }
                h = (h + 1) & (cap - 1);
            }
        }
        None
    }
}
impl PkgView for CPkg<'_> {
    fn fmt(&self) -> u8 { self.0.fmt as u8 }
    fn depends(&self) -> Option<&[u8]> { if self.0.depends.is_null() { None } else { Some(unsafe { cs(self.0.depends) }) } }
    fn len(&self) -> usize { self.0.nents.max(0) as usize }
    fn path(&self, i: usize) -> &[u8] { unsafe { pkgfmt::cstr(&(*self.0.ents.add(i)).path) } }
    fn ty(&self, i: usize) -> u8 { unsafe { self.ent(i).ty } }
    fn data(&self, i: usize) -> &[u8] { unsafe { let e = self.ent(i); if e.data.is_null() { &[] } else { core::slice::from_raw_parts(e.data, e.size as usize) } } }
    fn find(&self, path: &[u8]) -> Option<usize> { self.probe(self.0.pidx, path, &|p| p) }
    fn has_basename(&self, b: &[u8]) -> bool { self.probe(self.0.bidx, b, &|p| base_of(p)).is_some() }
}
/// What is installed: the VFS under C:\Linux, and the package database.
struct KSys;
impl Sys for KSys {
    fn lookup(&self, lp: &[u8]) -> Option<Option<&[u8]>> {
        let mut w = pkgfmt::lx_to_wrld(lp, 600); w.push(0);
        unsafe {
            let n = vfs_resolve(w.as_ptr() as *const c_char, null());
            if n.is_null() { return None; }
            if (*n).dir != 0 { return Some(None); }
            Some(Some(if (*n).data.is_null() { &[] } else { core::slice::from_raw_parts((*n).data, (*n).size as usize) }))
        }
    }
    fn dep_met(&self, name: &[u8]) -> bool { let mut v = name.to_vec(); v.push(0); unsafe { pkg_dep_met(v.as_ptr() as *const c_char) != 0 } }
    fn yield_now(&self) { unsafe { unpack_yield() } }
}
/// void pkg_check(const pkg_t *p, pkg_check_t *c)
#[no_mangle]
pub unsafe extern "C" fn pkg_check(p: *const PkgT, c: *mut PkgCheck) {
    core::ptr::write_bytes(c, 0, 1);
    let r = pkgfmt::check(&CPkg(&*p), &KSys);
    let c = &mut *c;
    put(&mut c.missing_pkgs, &r.missing_pkgs); put(&mut c.missing_libs, &r.missing_libs); put(&mut c.missing_vers, &r.missing_vers);
    put(&mut c.missing_interp, &r.missing_interp); put(&mut c.programs, &r.programs);
    c.foreign_elf = r.foreign_elf as CBool; c.ok = r.ok as CBool;
}
/// const pkg_entry_t *pkg_find_entry(const pkg_t *p, const char *path)
#[no_mangle]
pub unsafe extern "C" fn pkg_find_entry(p: *const PkgT, path: *const c_char) -> *const PkgEntry {
    let v = CPkg(&*p);
    let c = pkgfmt::canon(&pkgfmt::lcpy(cs(path as *const u8), 300), 300);
    match v.find(&c) { Some(i) => (*p).ents.add(i), None => null() }
}
/// void pkg_canon(char *p, int cap): /bin, /lib ... as /usr/bin, /usr/lib ...
#[no_mangle]
pub unsafe extern "C" fn pkg_canon(p: *mut c_char, cap: c_int) { let v = pkgfmt::canon(cs(p as *const u8), cap.max(0) as usize); put_raw(p, cap, &v); }
/// void pkg_join_link(const char *linkpath, const char *target, char *out, int cap)
#[no_mangle]
pub unsafe extern "C" fn pkg_join_link(linkpath: *const c_char, target: *const c_char, out: *mut c_char, cap: c_int) {
    let v = pkgfmt::join_link(cs(linkpath as *const u8), cs(target as *const u8), cap.max(0) as usize);
    put_raw(out, cap, &v);
}
/// void pkg_lx_to_wrld(const char *lpath, char *out, int cap)
#[no_mangle]
pub unsafe extern "C" fn pkg_lx_to_wrld(lp: *const c_char, out: *mut c_char, cap: c_int) {
    let v = pkgfmt::lx_to_wrld(cs(lp as *const u8), cap.max(0) as usize);
    put_raw(out, cap, &v);
}
/// bool pkg_desktop_entry(data, size, name, ncap, cmd, ccap, &terminal): a .desktop file's
/// visible application entry.
#[no_mangle]
pub unsafe extern "C" fn pkg_desktop_entry(d: *const u8, n: u32, name: *mut c_char, ncap: c_int, cmd: *mut c_char, ccap: c_int, term: *mut CBool) -> CBool {
    let data = if d.is_null() { &[][..] } else { core::slice::from_raw_parts(d, n as usize) };
    match pkgfmt::desktop_entry(data) {
        Some((nm, c, t)) => { put_raw(name, ncap, &nm); put_raw(cmd, ccap, &c); if !term.is_null() { *term = t as CBool; } 1 }
        None => 0,
    }
}
/// void pkg_alternatives(script, fn(ctx, link, path), ctx): each update-alternatives pair.
#[no_mangle]
pub unsafe extern "C" fn pkg_alternatives(script: *const c_char, f: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char)>, ctx: *mut c_void) {
    let Some(f) = f else { return };
    for (l, t) in pkgfmt::alternatives(cs(script as *const u8)) {
        let (mut l, mut t) = (l, t);
        l.push(0); t.push(0);
        f(ctx, l.as_ptr() as *const c_char, t.as_ptr() as *const c_char);
    }
}
/// uint8_t *unpack_gunzip(src, n, max_out, &outlen, err, ecap): for src/pkg/unpack.c.
#[no_mangle]
pub unsafe extern "C" fn unpack_gunzip(s: *const u8, n: u32, max_out: u32, outlen: *mut u32, err: *mut c_char, ecap: c_int) -> *mut u8 {
    let src = if s.is_null() { &[][..] } else { core::slice::from_raw_parts(s, n as usize) };
    match pkgfmt::gunzip(src, max_out) {
        Ok(v) => {
            *outlen = v.len() as u32;
            if v.is_empty() { return kmalloc(1) as *mut u8; }
            into_c_buffer(v)
        }
        Err(m) => { put_raw(err, ecap, m); null_mut() }
    }
}
