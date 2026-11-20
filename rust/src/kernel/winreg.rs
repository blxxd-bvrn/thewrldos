//! advapi32's and shlwapi's registry functions for Win32 programs (was
//! `src/win32/registry.c`), over `crate::registry`. They use the Windows calling
//! convention of each edition (stdcall on x86, the Microsoft x64 one on x86-64) and are
//! listed in `registry_api`, which `src/win32/apitab.c` searches by DLL and name.
//!
//! The wide (W) functions turn names into ANSI as the C version did: characters above 255
//! become '?', key paths are cut at 511 characters and value names at 255. Pointers from the
//! program are trusted as far as Windows trusts them (a buffer is as long as its size says).

use alloc::vec::Vec;
use core::ffi::{c_char, c_void};

use crate::registry::{self as reg, Registry, E_ACCESS, E_NOTFOUND, E_OK};

extern "C" {
    fn vfs_read_file(path: *const c_char, size: *mut u32) -> *mut u8;
    fn vfs_write_file(path: *const c_char, data: *const c_void, len: u32) -> *mut c_void;
    fn kfree(p: *mut c_void);
    fn klog(fmt: *const c_char, ...);
}

const REG_PATH: &core::ffi::CStr = c"C:\\WRLD\\registry.dat";
/// ERROR_BUSY: the registry was entered again while in use (should not happen: kernel code
/// is not preempted).
const E_BUSY: i32 = 170;

struct RegCell { busy: core::cell::Cell<bool>, reg: core::cell::UnsafeCell<Option<Registry>> }
unsafe impl Sync for RegCell {}
static REG: RegCell = RegCell { busy: core::cell::Cell::new(false), reg: core::cell::UnsafeCell::new(None) };

/// Runs `f` on the registry (built and loaded from registry.dat on first use), then saves
/// it if `f` changed it.
fn with_reg(f: impl FnOnce(&mut Registry) -> i32) -> i32 {
    if REG.busy.replace(true) {
        unsafe { klog(c"registry: entered twice\n".as_ptr()) };
        return E_BUSY;
    }
    let slot = unsafe { &mut *REG.reg.get() };
    let r = slot.get_or_insert_with(|| {
        let mut r = Registry::new();
        let mut n = 0u32;
        let p = unsafe { vfs_read_file(REG_PATH.as_ptr(), &mut n) };
        if !p.is_null() {
            r.load(unsafe { core::slice::from_raw_parts(p, n as usize) });
            unsafe { kfree(p as *mut c_void) };
        }
        r.take_dirty();
        r
    });
    let e = f(r);
    if r.take_dirty() {
        let s = r.save();
        unsafe { vfs_write_file(REG_PATH.as_ptr(), s.as_ptr() as *const c_void, s.len() as u32) };
    }
    REG.busy.set(false);
    e
}

// ---------------------------------------------------------------- the program's arguments

/// The longest buffer taken from a size the program gives (keeps slices valid on 32-bit).
const MAX_BUF: usize = 0x7FFF_0000;

unsafe fn astr<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0; while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
/// A wide name to ANSI, at most `cap - 1` characters (the C version's w2a).
unsafe fn wstr(p: *const u16, cap: usize) -> Vec<u8> {
    let mut v = Vec::new();
    if p.is_null() { return v; }
    let mut i = 0;
    while i < cap - 1 { let c = *p.add(i); if c == 0 { break; } v.push(if c < 256 { c as u8 } else { b'?' }); i += 1; }
    v
}
unsafe fn opt<'a, T>(p: *mut T) -> Option<&'a mut T> { if p.is_null() { None } else { Some(&mut *p) } }
/// The caller's data buffer: `size` bytes (0 when there is no size).
unsafe fn buf<'a>(p: *mut u8, size: *const u32) -> Option<&'a mut [u8]> {
    if p.is_null() { return None; }
    let n = if size.is_null() { 0 } else { (*size as usize).min(MAX_BUF) };
    Some(core::slice::from_raw_parts_mut(p, n))
}
/// A name buffer of `*cc` characters, or none when either pointer is missing.
unsafe fn namebuf<'a>(p: *mut u8, cc: *const u32, wide: bool) -> Option<&'a mut [u8]> {
    if p.is_null() || cc.is_null() { return None; }
    Some(core::slice::from_raw_parts_mut(p, (*cc as usize * if wide { 2 } else { 1 }).min(MAX_BUF)))
}
unsafe fn data<'a>(p: *const u8, n: u32) -> &'a [u8] { if p.is_null() { &[] } else { core::slice::from_raw_parts(p, (n as usize).min(MAX_BUF)) } }

// ---------------------------------------------------------------- shared steps

unsafe fn open_key(r: &mut Registry, h: usize, sub: &[u8], res: *mut usize, create: bool, disp: *mut u32) -> i32 {
    let h = h as u32;
    if !r.is_key(h) { return reg::E_BADKEY; }
    if res.is_null() { return reg::E_PARAM; }
    let (e, hk, d) = r.open(h, sub, create);
    if d != 0 { if let Some(dp) = opt(disp) { *dp = d; } }
    *res = if e == E_OK { hk as usize } else { 0 };
    e
}
unsafe fn query(r: &mut Registry, h: usize, name: &[u8], t: *mut u32, d: *mut u8, sz: *mut u32, wide: bool) -> i32 {
    r.query(h as u32, name, opt(t), buf(d, sz), opt(sz), wide)
}
/// RegQueryValue: the default value of a subkey, as a string.
unsafe fn query_default(r: &mut Registry, h: usize, sub: &[u8], d: *mut u8, sz: *mut i32, wide: bool) -> i32 {
    let mut k = 0usize;
    let e = open_key(r, h, sub, &mut k, false, core::ptr::null_mut());
    if e != E_OK { return e; }
    let mut n: u32 = if sz.is_null() { 0 } else { *sz as u32 };
    let szp: *mut u32 = if d.is_null() { sz as *mut u32 } else { &mut n };
    let mut e = query(r, k, b"", core::ptr::null_mut(), d, szp, wide);
    let unit = if wide { 2 } else { 1 };
    if e == E_NOTFOUND {
        if !d.is_null() && !sz.is_null() && *sz >= unit { *d = 0; if wide { *d.add(1) = 0; } }
        if !sz.is_null() { *sz = unit; }
        e = E_OK;
    } else if !sz.is_null() && !d.is_null() { *sz = n as i32; }
    r.close(k as u32);
    e
}
unsafe fn enum_key(r: &mut Registry, h: usize, i: u32, n: *mut u8, cc: *mut u32, wide: bool) -> i32 {
    r.enum_key(h as u32, i, namebuf(n, cc, wide), opt(cc), wide)
}
unsafe fn enum_value(r: &mut Registry, h: usize, i: u32, n: *mut u8, cc: *mut u32, t: *mut u32, d: *mut u8, sz: *mut u32, wide: bool) -> i32 {
    r.enum_value(h as u32, i, namebuf(n, cc, wide), opt(cc), opt(t), buf(d, sz), opt(sz), wide)
}
unsafe fn info(r: &mut Registry, h: usize, cls: *mut u8, clc: *mut u32, ns: *mut u32, ms: *mut u32, mc: *mut u32, nv: *mut u32, mn: *mut u32, md: *mut u32, sd: *mut u32, ft: *mut u8, wide: bool) -> i32 {
    if let Some(c) = opt(clc) { if !cls.is_null() && *c != 0 { *cls = 0; if wide { *cls.add(1) = 0; } } *c = 0; }
    if let Some(p) = opt(mc) { *p = 0; }
    if let Some(p) = opt(sd) { *p = 0; }
    let i = match r.info(h as u32) { Ok(i) => i, Err(e) => return e };
    for (p, v) in [(ns, i.subkeys), (ms, i.max_subkey), (nv, i.values), (mn, i.max_name), (md, i.max_data)] { if let Some(p) = opt(p) { *p = v; } }
    if !ft.is_null() { core::ptr::copy_nonoverlapping(116_444_736_000_000_000u64.to_le_bytes().as_ptr(), ft, 8); }   // 1 January 1970
    E_OK
}
unsafe fn get_value(r: &mut Registry, h: usize, sub: &[u8], name: &[u8], fl: u32, t: *mut u32, d: *mut u8, sz: *mut u32, wide: bool) -> i32 {
    r.get_value(h as u32, sub, name, fl, opt(t), buf(d, sz), opt(sz), wide)
}
unsafe fn wlen(d: *const u16) -> usize { let mut l = 0; if !d.is_null() { while *d.add(l) != 0 { l += 1; } } l }

// ---------------------------------------------------------------- the functions

macro_rules! winapi {
    ($(fn $name:ident($($a:ident: $t:ty),* $(,)?) -> $r:ty $body:block)*) => {$(
        #[cfg(target_arch = "x86")] #[allow(non_snake_case, unused_variables)]
        unsafe extern "stdcall" fn $name($($a: $t),*) -> $r $body
        #[cfg(target_arch = "x86_64")] #[allow(non_snake_case, unused_variables)]
        unsafe extern "win64" fn $name($($a: $t),*) -> $r $body
    )*};
}

type W = *const u16;
type A = *const u8;
type H = usize;
type P = *mut u32;

winapi! {
    fn RegOpenKeyExA(h: H, sub: A, o: u32, sam: u32, res: *mut H) -> i32 { with_reg(|r| open_key(r, h, astr(sub), res, false, core::ptr::null_mut())) }
    fn RegOpenKeyExW(h: H, sub: W, o: u32, sam: u32, res: *mut H) -> i32 { let a = wstr(sub, 512); with_reg(|r| open_key(r, h, &a, res, false, core::ptr::null_mut())) }
    fn RegOpenKeyA(h: H, sub: A, res: *mut H) -> i32 { with_reg(|r| open_key(r, h, astr(sub), res, false, core::ptr::null_mut())) }
    fn RegOpenKeyW(h: H, sub: W, res: *mut H) -> i32 { let a = wstr(sub, 512); with_reg(|r| open_key(r, h, &a, res, false, core::ptr::null_mut())) }
    fn RegOpenCurrentUser(sam: u32, res: *mut H) -> i32 { if let Some(p) = opt(res) { *p = reg::HKEY_CURRENT_USER as i32 as isize as H; /* sign-extended, as Windows has it */ } E_OK }
    fn RegCreateKeyExA(h: H, sub: A, rs: u32, cls: *mut u8, o: u32, sam: u32, sa: *mut c_void, res: *mut H, disp: P) -> i32 { with_reg(|r| open_key(r, h, astr(sub), res, true, disp)) }
    fn RegCreateKeyExW(h: H, sub: W, rs: u32, cls: *mut u16, o: u32, sam: u32, sa: *mut c_void, res: *mut H, disp: P) -> i32 { let a = wstr(sub, 512); with_reg(|r| open_key(r, h, &a, res, true, disp)) }
    fn RegCreateKeyA(h: H, sub: A, res: *mut H) -> i32 { with_reg(|r| open_key(r, h, astr(sub), res, true, core::ptr::null_mut())) }
    fn RegCreateKeyW(h: H, sub: W, res: *mut H) -> i32 { let a = wstr(sub, 512); with_reg(|r| open_key(r, h, &a, res, true, core::ptr::null_mut())) }
    fn RegCloseKey(h: H) -> i32 { with_reg(|r| { r.close(h as u32); E_OK }) }
    fn RegFlushKey(h: H) -> i32 { E_OK }
    fn RegQueryValueExA(h: H, n: A, rs: P, t: P, d: *mut u8, sz: P) -> i32 { with_reg(|r| query(r, h, astr(n), t, d, sz, false)) }
    fn RegQueryValueExW(h: H, n: W, rs: P, t: P, d: *mut u8, sz: P) -> i32 { let a = wstr(n, 256); with_reg(|r| query(r, h, &a, t, d, sz, true)) }
    fn RegQueryValueA(h: H, sub: A, d: *mut u8, sz: *mut i32) -> i32 { with_reg(|r| query_default(r, h, astr(sub), d, sz, false)) }
    fn RegQueryValueW(h: H, sub: W, d: *mut u16, sz: *mut i32) -> i32 { let a = wstr(sub, 512); with_reg(|r| query_default(r, h, &a, d as *mut u8, sz, true)) }
    fn RegSetValueExA(h: H, n: A, rs: u32, t: u32, d: A, sz: u32) -> i32 { with_reg(|r| r.store(h as u32, astr(n), t, data(d, sz), false)) }
    fn RegSetValueExW(h: H, n: W, rs: u32, t: u32, d: A, sz: u32) -> i32 { let a = wstr(n, 256); with_reg(|r| r.store(h as u32, &a, t, data(d, sz), true)) }
    fn RegSetValueA(h: H, sub: A, t: u32, d: A, sz: u32) -> i32 {
        let s = astr(d); let v = if d.is_null() { &[][..] } else { core::slice::from_raw_parts(d, s.len() + 1) };
        with_reg(|r| r.set_default(h as u32, astr(sub), v, false))
    }
    fn RegSetValueW(h: H, sub: W, t: u32, d: W, sz: u32) -> i32 {
        let a = wstr(sub, 512);
        let v = if d.is_null() { &[][..] } else { core::slice::from_raw_parts(d as *const u8, (wlen(d) + 1) * 2) };
        with_reg(|r| r.set_default(h as u32, &a, v, true))
    }
    fn RegDeleteValueA(h: H, n: A) -> i32 { with_reg(|r| r.delete_value(h as u32, astr(n))) }
    fn RegDeleteValueW(h: H, n: W) -> i32 { let a = wstr(n, 256); with_reg(|r| r.delete_value(h as u32, &a)) }
    fn RegDeleteKeyA(h: H, s: A) -> i32 { with_reg(|r| r.delete_key(h as u32, astr(s), false)) }
    fn RegDeleteKeyW(h: H, s: W) -> i32 { let a = wstr(s, 512); with_reg(|r| r.delete_key(h as u32, &a, false)) }
    fn RegDeleteKeyExA(h: H, s: A, sam: u32, rs: u32) -> i32 { with_reg(|r| r.delete_key(h as u32, astr(s), false)) }
    fn RegDeleteKeyExW(h: H, s: W, sam: u32, rs: u32) -> i32 { let a = wstr(s, 512); with_reg(|r| r.delete_key(h as u32, &a, false)) }
    fn RegDeleteTreeA(h: H, s: A) -> i32 { with_reg(|r| r.delete_key(h as u32, astr(s), true)) }
    fn RegDeleteTreeW(h: H, s: W) -> i32 { let a = wstr(s, 512); with_reg(|r| r.delete_key(h as u32, &a, true)) }
    fn RegEnumKeyExA(h: H, i: u32, n: *mut u8, cc: P, rs: P, cls: *mut u8, clc: P, ft: *mut c_void) -> i32 { if let Some(c) = opt(clc) { *c = 0; } with_reg(|r| enum_key(r, h, i, n, cc, false)) }
    fn RegEnumKeyExW(h: H, i: u32, n: *mut u16, cc: P, rs: P, cls: *mut u16, clc: P, ft: *mut c_void) -> i32 { if let Some(c) = opt(clc) { *c = 0; } with_reg(|r| enum_key(r, h, i, n as *mut u8, cc, true)) }
    fn RegEnumKeyA(h: H, i: u32, n: *mut u8, c: u32) -> i32 { let mut cc = c; with_reg(|r| enum_key(r, h, i, n, &mut cc, false)) }
    fn RegEnumKeyW(h: H, i: u32, n: *mut u16, c: u32) -> i32 { let mut cc = c; with_reg(|r| enum_key(r, h, i, n as *mut u8, &mut cc, true)) }
    fn RegEnumValueA(h: H, i: u32, n: *mut u8, cc: P, rs: P, t: P, d: *mut u8, sz: P) -> i32 { with_reg(|r| enum_value(r, h, i, n, cc, t, d, sz, false)) }
    fn RegEnumValueW(h: H, i: u32, n: *mut u16, cc: P, rs: P, t: P, d: *mut u8, sz: P) -> i32 { with_reg(|r| enum_value(r, h, i, n as *mut u8, cc, t, d, sz, true)) }
    fn RegQueryInfoKeyA(h: H, cls: *mut u8, clc: P, rs: P, ns: P, ms: P, mc: P, nv: P, mn: P, md: P, sd: P, ft: *mut u8) -> i32 { with_reg(|r| info(r, h, cls, clc, ns, ms, mc, nv, mn, md, sd, ft, false)) }
    fn RegQueryInfoKeyW(h: H, cls: *mut u16, clc: P, rs: P, ns: P, ms: P, mc: P, nv: P, mn: P, md: P, sd: P, ft: *mut u8) -> i32 { with_reg(|r| info(r, h, cls as *mut u8, clc, ns, ms, mc, nv, mn, md, sd, ft, true)) }
    fn RegGetValueA(h: H, sub: A, n: A, fl: u32, t: P, d: *mut u8, sz: P) -> i32 { with_reg(|r| get_value(r, h, astr(sub), astr(n), fl, t, d, sz, false)) }
    fn RegGetValueW(h: H, sub: W, n: W, fl: u32, t: P, d: *mut u8, sz: P) -> i32 { let (a, b) = (wstr(sub, 512), wstr(n, 256)); with_reg(|r| get_value(r, h, &a, &b, fl, t, d, sz, true)) }
    fn RegNotifyChangeKeyValue(h: H, sub: i32, f: u32, ev: H, asy: i32) -> i32 { E_OK }
    fn RegConnectRegistryW(m: W, h: H, res: *mut H) -> i32 { if !m.is_null() && *m != 0 { return 53; } if let Some(p) = opt(res) { *p = h; } E_OK }
    fn RegConnectRegistryA(m: A, h: H, res: *mut H) -> i32 { if !m.is_null() && *m != 0 { return 53; } if let Some(p) = opt(res) { *p = h; } E_OK }
    fn RegOverridePredefKey(h: H, n: H) -> i32 { E_OK }
    fn RegDisablePredefinedCache() -> i32 { E_OK }
    fn RegLoadKeyW(h: H, s: W, f: W) -> i32 { E_ACCESS }
    fn RegSaveKeyW(h: H, f: W, sa: *mut c_void) -> i32 { E_ACCESS }
    fn RegSaveKeyA(h: H, f: A, sa: *mut c_void) -> i32 { E_ACCESS }
    // shlwapi's registry helpers
    fn SHGetValueA(h: H, sub: A, n: A, t: P, d: *mut u8, sz: P) -> i32 { with_reg(|r| get_value(r, h, astr(sub), astr(n), 0, t, d, sz, false)) }
    fn SHGetValueW(h: H, sub: W, n: W, t: P, d: *mut u8, sz: P) -> i32 { let (a, b) = (wstr(sub, 512), wstr(n, 256)); with_reg(|r| get_value(r, h, &a, &b, 0, t, d, sz, true)) }
    fn SHSetValueA(h: H, sub: A, n: A, t: u32, d: A, sz: u32) -> i32 {
        with_reg(|r| { let mut k = 0; let e = open_key(r, h, astr(sub), &mut k, true, core::ptr::null_mut()); if e != E_OK { return e; }
                       let e = r.store(k as u32, astr(n), t, data(d, sz), false); r.close(k as u32); e })
    }
    fn SHSetValueW(h: H, sub: W, n: W, t: u32, d: A, sz: u32) -> i32 {
        let (a, b) = (wstr(sub, 512), wstr(n, 256));
        with_reg(|r| { let mut k = 0; let e = open_key(r, h, &a, &mut k, true, core::ptr::null_mut()); if e != E_OK { return e; }
                       let e = r.store(k as u32, &b, t, data(d, sz), true); r.close(k as u32); e })
    }
    fn SHDeleteKeyA(h: H, s: A) -> i32 { with_reg(|r| r.delete_key(h as u32, astr(s), true)) }
    fn SHDeleteKeyW(h: H, s: W) -> i32 { let a = wstr(s, 512); with_reg(|r| r.delete_key(h as u32, &a, true)) }
    fn SHDeleteValueA(h: H, sub: A, n: A) -> i32 {
        with_reg(|r| { let mut k = 0; let e = open_key(r, h, astr(sub), &mut k, false, core::ptr::null_mut()); if e != E_OK { return e; }
                       let e = r.delete_value(k as u32, astr(n)); r.close(k as u32); e })
    }
    fn SHDeleteValueW(h: H, sub: W, n: W) -> i32 {
        let (a, b) = (wstr(sub, 512), wstr(n, 256));
        with_reg(|r| { let mut k = 0; let e = open_key(r, h, &a, &mut k, false, core::ptr::null_mut()); if e != E_OK { return e; }
                       let e = r.delete_value(k as u32, &b); r.close(k as u32); e })
    }
    fn SHQueryValueExA(h: H, n: A, rs: P, t: P, d: *mut u8, sz: P) -> i32 { with_reg(|r| query(r, h, astr(n), t, d, sz, false)) }
    fn SHQueryValueExW(h: H, n: W, rs: P, t: P, d: *mut u8, sz: P) -> i32 { let a = wstr(n, 256); with_reg(|r| query(r, h, &a, t, d, sz, true)) }
    fn SHEnumKeyExW(h: H, i: u32, n: *mut u16, cc: P) -> i32 { with_reg(|r| enum_key(r, h, i, n as *mut u8, cc, true)) }
    fn SHEnumKeyExA(h: H, i: u32, n: *mut u8, cc: P) -> i32 { with_reg(|r| enum_key(r, h, i, n, cc, false)) }
}

// ---------------------------------------------------------------- the table apitab.c searches

/// One entry: a DLL group ("advapi32", "kernel32", "shlwapi"), a name and the function
/// (src/win32/w32.h's w32_api_t).
#[repr(C)]
pub struct W32Api { group: *const c_char, name: *const c_char, f: *const c_void }
unsafe impl Sync for W32Api {}

macro_rules! table {
    ($($g:literal: $($n:ident $(= $alias:literal)?),*;)*) => {
        /// The registry functions, ending with an empty entry.
        #[no_mangle]
        #[allow(non_upper_case_globals)]
        pub static registry_api: [W32Api; table!(@count $($($n)*)*) + 1] = [
            $($(W32Api {
                group: concat!($g, "\0").as_ptr() as *const c_char,
                name: table!(@name $n $($alias)?),
                f: $n as *const c_void,
            },)*)*
            W32Api { group: core::ptr::null(), name: core::ptr::null(), f: core::ptr::null() },
        ];
    };
    (@name $n:ident) => { concat!(stringify!($n), "\0").as_ptr() as *const c_char };
    (@name $n:ident $alias:literal) => { concat!($alias, "\0").as_ptr() as *const c_char };
    (@count) => { 0 };
    (@count $h:ident $($t:ident)*) => { 1 + table!(@count $($t)*) };
}

table! {
    "advapi32": RegOpenKeyExA, RegOpenKeyExW, RegOpenKeyA, RegOpenKeyW, RegOpenCurrentUser, RegCreateKeyExA, RegCreateKeyExW,
        RegCreateKeyA, RegCreateKeyW, RegCloseKey, RegFlushKey, RegQueryValueExA, RegQueryValueExW, RegQueryValueA, RegQueryValueW,
        RegSetValueExA, RegSetValueExW, RegSetValueA, RegSetValueW, RegDeleteValueA, RegDeleteValueW, RegDeleteKeyA, RegDeleteKeyW,
        RegDeleteKeyExA, RegDeleteKeyExW, RegDeleteTreeA, RegDeleteTreeW, RegEnumKeyExA, RegEnumKeyExW, RegEnumKeyA, RegEnumKeyW,
        RegEnumValueA, RegEnumValueW, RegQueryInfoKeyA, RegQueryInfoKeyW, RegGetValueA, RegGetValueW, RegNotifyChangeKeyValue,
        RegConnectRegistryW, RegConnectRegistryA, RegOverridePredefKey, RegDisablePredefinedCache, RegLoadKeyW, RegSaveKeyW, RegSaveKeyA;
    // Windows 7 moved these into kernel32 too (through the api-ms-win-core-registry set)
    "kernel32": RegOpenKeyExA, RegOpenKeyExW, RegCreateKeyExA, RegCreateKeyExW, RegCloseKey, RegQueryValueExA, RegQueryValueExW,
        RegSetValueExA, RegSetValueExW, RegDeleteValueW, RegEnumKeyExW, RegEnumValueW, RegQueryInfoKeyW, RegGetValueW, RegDeleteTreeW;
    "shlwapi": SHGetValueA, SHGetValueW, SHSetValueA, SHSetValueW, SHDeleteKeyA, SHDeleteKeyW, SHDeleteValueA, SHDeleteValueW,
        SHQueryValueExA, SHQueryValueExW, SHEnumKeyExW, SHEnumKeyExA, RegGetValueW = "SHRegGetValueW", RegGetValueA = "SHRegGetValueA";
}

