//! The less common DLLs programs import and probe (was `src/win32/misc_api.c`): ole32 and
//! oleaut32 basics (GUID strings, SAFEARRAYs), advapi32 security and event-log calls,
//! bcrypt's random numbers, crypt32 certificate stores, setupapi, winspool (no printers),
//! wldap32 (no directory), netapi32, normaliz, imm32, wtsapi32, userenv. Where THE WRLD OS
//! has no such service the calls fail the way Windows reports it, so programs carry on
//! instead of stopping on a missing import. The parsing is in `crate::w32util`.

use core::ffi::{c_char, c_void};
use core::mem::offset_of;

use super::w32::{a2w, alloc, free, lookup, set_err, wstr};
use crate::w32util as u;

extern "C" {
    fn rand_next() -> u32;
    fn klog(fmt: *const c_char, ...);
    static user_name: [c_char; 32];
    static computer_name: [c_char; 32];
}

type H = usize;
type W = *const u16;
type A = *const u8;
type P = *mut u32;
type PV = *mut *mut c_void;

const S_OK: i32 = 0;
const S_FALSE: i32 = 1;
const E_FAIL: i32 = 0x8000_4005u32 as i32;
const E_NOTIMPL: i32 = 0x8000_4001u32 as i32;
const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;
const E_OUTOFMEMORY: i32 = 0x8007_000Eu32 as i32;
const DISP_E_BADINDEX: i32 = 0x8002_000Bu32 as i32;
const DISP_E_ARRAYISLOCKED: i32 = 0x8002_000Du32 as i32;
const CO_E_CLASSSTRING: i32 = 0x8004_01F3u32 as i32;
const USER_HOME: &[u8] = b"C:\\Documents and Settings\\User";

unsafe fn cbytes(s: &[c_char; 32]) -> &[u8] { let b = &*(s as *const [c_char; 32] as *const [u8; 32]); &b[..b.iter().position(|&c| c == 0).unwrap_or(32)] }
unsafe fn local_alloc(n: usize) -> *mut u8 {
    let f = lookup(c"kernel32.dll", c"LocalAlloc");
    if f.is_null() { return core::ptr::null_mut(); }
    let f: winfn!(fn(u32, usize) -> *mut u8) = core::mem::transmute(f);
    f(0x40, n)
}
unsafe fn rand_fill(b: *mut u8, n: usize) { for i in 0..n { *b.add(i) = rand_next() as u8; } }
/// Copies a constant string into the caller's buffer of `*n` characters (userenv's way).
unsafe fn put_dir(d: &[u8], b: *mut u8, n: P, wide: bool) -> i32 {
    let l = d.len() as u32;
    if b.is_null() || *n <= l { *n = l + 1; set_err(122); return 0; }
    if wide { a2w(d, b as *mut u16, *n as usize); } else { core::ptr::copy_nonoverlapping(d.as_ptr(), b, d.len()); *b.add(d.len()) = 0; }
    *n = l + 1;
    1
}

// ---------------------------------------------------------------- SAFEARRAY

#[repr(C)]
#[derive(Clone, Copy)]
struct SaBound { n: u32, lb: i32 }
#[repr(C)]
struct Sa { dims: u16, features: u16, elem: u32, locks: u32, data: *mut u8, b: [SaBound; 1] }
const SA_B: usize = offset_of!(Sa, b);
unsafe fn bounds<'a>(a: *mut Sa) -> &'a mut [SaBound] { core::slice::from_raw_parts_mut((a as *mut u8).add(SA_B) as *mut SaBound, (*a).dims as usize) }
unsafe fn sa_offset(a: *mut Sa, idx: *const i32) -> Option<usize> {
    let b: alloc::vec::Vec<(u32, i32)> = bounds(a).iter().map(|b| (b.n, b.lb)).collect();
    let idx = core::slice::from_raw_parts(idx, b.len());
    u::sa_index(&b, idx).map(|i| i.wrapping_mul((*a).elem as usize))
}
unsafe fn sa_total(a: *mut Sa) -> Option<usize> {
    bounds(a).iter().try_fold((*a).elem as usize, |t, b| t.checked_mul(b.n as usize))
}

winapi! {
    // ------------------------------------------------------------ ole32
    fn CLSIDFromString(s: W, g: *mut u8) -> i32 {
        core::ptr::write_bytes(g, 0, 16);
        match u::guid_parse(wstr(s, 64)) { Ok(v) => { core::ptr::copy_nonoverlapping(v.as_ptr(), g, 16); S_OK } Err(e) => e }
    }
    fn IIDFromString(s: W, g: *mut u8) -> i32 { CLSIDFromString(s, g) }
    fn StringFromCLSID(g: *const u8, out: *mut *mut u16) -> i32 {
        let t = u::guid_text(&*(g as *const [u8; 16]));
        let f = lookup(c"ole32.dll", c"CoTaskMemAlloc");
        let p = if f.is_null() { alloc(80) } else { let f: winfn!(fn(usize) -> *mut u8) = core::mem::transmute(f); f(80) } as *mut u16;
        *out = p;
        a2w(&t, p, 40);
        S_OK
    }
    fn StringFromGUID2(g: *const u8, b: *mut u16, n: i32) -> i32 {
        let t = u::guid_text(&*(g as *const [u8; 16]));
        if n <= 38 { return 0; }
        a2w(&t, b, n as usize);
        39
    }
    fn CoCreateGuid(g: *mut u8) -> i32 {
        rand_fill(g, 16);
        *g.add(7) = (*g.add(7) & 0x0F) | 0x40;           // version 4 (Data3's high nibble)
        *g.add(8) = (*g.add(8) & 0x3F) | 0x80;           // the variant
        S_OK
    }
    fn IsEqualGUID(a: *const u8, b: *const u8) -> i32 { (core::slice::from_raw_parts(a, 16) == core::slice::from_raw_parts(b, 16)) as i32 }
    fn CoDisconnectObject(un: *mut c_void, r: u32) -> i32 { S_OK }
    fn CreateDataAdviseHolder(out: PV) -> i32 { *out = core::ptr::null_mut(); E_NOTIMPL }
    fn CreateStreamOnHGlobal(h: *mut c_void, del: i32, out: PV) -> i32 { *out = core::ptr::null_mut(); E_NOTIMPL }
    fn GetHGlobalFromStream(s: *mut c_void, h: PV) -> i32 { E_INVALIDARG }
    fn DoDragDrop(d: *mut c_void, s: *mut c_void, ok: u32, eff: P) -> i32 { if !eff.is_null() { *eff = 0; } 0x0004_0101 }   // DRAGDROP_S_CANCEL
    fn RegisterDragDrop(h: H, t: *mut c_void) -> i32 { S_OK }
    fn RevokeDragDrop(h: H) -> i32 { S_OK }
    fn OleGetClipboard(out: PV) -> i32 { *out = core::ptr::null_mut(); E_FAIL }
    fn OleSetClipboard(d: *mut c_void) -> i32 { if d.is_null() { S_OK } else { E_FAIL } }
    fn OleFlushClipboard() -> i32 { S_OK }
    fn OleIsCurrentClipboard(d: *mut c_void) -> i32 { S_FALSE }
    fn PropVariantClear(v: *mut u8) -> i32 { core::ptr::write_bytes(v, 0, 16); S_OK }
    fn ReleaseStgMedium(m: *mut c_void) -> () {}
    fn CoRegisterClassObject(c: *const c_void, un: *mut c_void, ctx: u32, f: u32, cookie: P) -> i32 { if !cookie.is_null() { *cookie = 1; } S_OK }
    fn CoRevokeClassObject(c: u32) -> i32 { S_OK }
    fn CoInitializeSecurity(a: *mut c_void, b: i32, c: *mut c_void, d: *mut c_void, e: u32, f: u32, g: *mut c_void, h: u32, i: *mut c_void) -> i32 { S_OK }
    fn CoGetClassObject(c: *const c_void, ctx: u32, s: *mut c_void, iid: *const c_void, out: PV) -> i32 { *out = core::ptr::null_mut(); 0x8004_0154u32 as i32 }
    fn CoLockObjectExternal(un: *mut c_void, l: i32, r: i32) -> i32 { S_OK }
    fn CoSetProxyBlanket(p: *mut c_void, a: u32, b: u32, c: *mut c_void, d: u32, e: u32, f: *mut c_void, g: u32) -> i32 { S_OK }
    fn OleInitialize2(r: *mut c_void) -> i32 { S_OK }
    fn CoFreeUnusedLibraries() -> i32 { S_OK }
    fn ProgIDFromCLSID(c: *const c_void, out: *mut *mut u16) -> i32 { *out = core::ptr::null_mut(); CO_E_CLASSSTRING }
    fn CLSIDFromProgID(p: W, g: *mut u8) -> i32 { core::ptr::write_bytes(g, 0, 16); CO_E_CLASSSTRING }

    // ------------------------------------------------------------ oleaut32: SAFEARRAY
    fn SafeArrayCreate(vt: u16, dims: u32, b: *const SaBound) -> *mut Sa {
        let es = u::vt_size(vt, core::mem::size_of::<usize>());
        if es == 0 || dims == 0 || dims > 16 { return core::ptr::null_mut(); }
        let hs = SA_B + dims as usize * 8;
        let a = alloc(hs) as *mut Sa;
        if a.is_null() { return a; }
        core::ptr::write_bytes(a as *mut u8, 0, hs);
        (*a).dims = dims as u16;
        (*a).elem = es as u32;
        (*a).features = match vt { 8 => 0x100, 12 => 0x800, 9 | 13 => 0x200, _ => 0 };
        let given = core::slice::from_raw_parts(b, dims as usize);
        for (i, g) in given.iter().enumerate() { bounds(a)[dims as usize - 1 - i] = *g; }
        let Some(total) = sa_total(a) else { free(a as *mut u8); return core::ptr::null_mut() };
        let d = alloc(total.max(1));
        if d.is_null() { free(a as *mut u8); return core::ptr::null_mut(); }
        core::ptr::write_bytes(d, 0, total.max(1));
        (*a).data = d;
        a
    }
    fn SafeArrayCreateVector(vt: u16, lb: i32, n: u32) -> *mut Sa { let b = SaBound { n, lb }; SafeArrayCreate(vt, 1, &b) }
    fn SafeArrayDestroy(a: *mut Sa) -> i32 {
        if a.is_null() { return S_OK; }
        if (*a).locks != 0 { return DISP_E_ARRAYISLOCKED; }
        free((*a).data); free(a as *mut u8);
        S_OK
    }
    fn SafeArrayAccessData(a: *mut Sa, d: PV) -> i32 { if a.is_null() { return E_INVALIDARG; } (*a).locks += 1; *d = (*a).data as *mut c_void; S_OK }
    fn SafeArrayUnaccessData(a: *mut Sa) -> i32 { if a.is_null() { return E_INVALIDARG; } if (*a).locks != 0 { (*a).locks -= 1; } S_OK }
    fn SafeArrayLock(a: *mut Sa) -> i32 { if a.is_null() { return E_INVALIDARG; } (*a).locks += 1; S_OK }
    fn SafeArrayUnlock(a: *mut Sa) -> i32 { if a.is_null() { return E_INVALIDARG; } if (*a).locks != 0 { (*a).locks -= 1; } S_OK }
    fn SafeArrayGetDim(a: *mut Sa) -> u32 { if a.is_null() { 0 } else { (*a).dims as u32 } }
    fn SafeArrayGetElemsize(a: *mut Sa) -> u32 { if a.is_null() { 0 } else { (*a).elem } }
    fn SafeArrayGetLBound(a: *mut Sa, d: u32, v: *mut i32) -> i32 {
        if a.is_null() || d == 0 || d > (*a).dims as u32 { return DISP_E_BADINDEX; }
        *v = bounds(a)[((*a).dims as u32 - d) as usize].lb; S_OK
    }
    fn SafeArrayGetUBound(a: *mut Sa, d: u32, v: *mut i32) -> i32 {
        if a.is_null() || d == 0 || d > (*a).dims as u32 { return DISP_E_BADINDEX; }
        let b = bounds(a)[((*a).dims as u32 - d) as usize];
        *v = b.lb.wrapping_add(b.n as i32).wrapping_sub(1); S_OK
    }
    fn SafeArrayPtrOfIndex(a: *mut Sa, idx: *const i32, p: PV) -> i32 {
        if a.is_null() { return E_INVALIDARG; }
        match sa_offset(a, idx) { Some(o) => { *p = (*a).data.add(o) as *mut c_void; S_OK } None => DISP_E_BADINDEX }
    }
    fn SafeArrayGetElement(a: *mut Sa, idx: *const i32, v: *mut u8) -> i32 {
        if a.is_null() { return E_INVALIDARG; }
        match sa_offset(a, idx) { Some(o) => { core::ptr::copy_nonoverlapping((*a).data.add(o), v, (*a).elem as usize); S_OK } None => DISP_E_BADINDEX }
    }
    fn SafeArrayPutElement(a: *mut Sa, idx: *const i32, v: *const u8) -> i32 {
        if a.is_null() { return E_INVALIDARG; }
        let Some(o) = sa_offset(a, idx) else { return DISP_E_BADINDEX };
        let at = (*a).data.add(o);
        if (*a).features & 0x100 != 0 {                     // BSTR: a copy of the string
            let f = lookup(c"oleaut32.dll", c"SysAllocString");
            let s = if f.is_null() { v as *mut u16 } else { let f: winfn!(fn(*const u16) -> *mut u16) = core::mem::transmute(f); f(v as *const u16) };
            *(at as *mut *mut u16) = s;
        } else if (*a).features & 0x200 != 0 {              // interfaces: the pointer itself
            core::ptr::copy_nonoverlapping(&v as *const *const u8 as *const u8, at, (*a).elem as usize);
        } else {
            core::ptr::copy_nonoverlapping(v, at, (*a).elem as usize);
        }
        S_OK
    }
    fn SafeArrayCopy(a: *mut Sa, out: *mut *mut Sa) -> i32 {
        if a.is_null() { *out = core::ptr::null_mut(); return S_OK; }
        let Some(n) = sa_total(a) else { return E_OUTOFMEMORY };
        let hs = SA_B + (*a).dims as usize * 8;
        let c = alloc(hs) as *mut Sa;
        if c.is_null() { return E_OUTOFMEMORY; }
        core::ptr::copy_nonoverlapping(a as *const u8, c as *mut u8, hs);
        (*c).locks = 0;
        let d = alloc(n.max(1));
        if d.is_null() { free(c as *mut u8); return E_OUTOFMEMORY; }
        core::ptr::copy_nonoverlapping((*a).data, d, n);
        (*c).data = d;
        *out = c;
        S_OK
    }
    fn SysReAllocStringLen(b: *mut *mut u16, s: W, n: u32) -> i32 {
        let al = lookup(c"oleaut32.dll", c"SysAllocStringLen");
        if al.is_null() { return 0; }
        let al: winfn!(fn(*const u16, u32) -> *mut u16) = core::mem::transmute(al);
        let nb = al(s, n);
        if nb.is_null() { return 0; }
        let fr = lookup(c"oleaut32.dll", c"SysFreeString");
        if !fr.is_null() && !(*b).is_null() { let fr: winfn!(fn(*mut u16) -> ()) = core::mem::transmute(fr); fr(*b); }
        *b = nb;
        1
    }
    fn SysReAllocString(b: *mut *mut u16, s: W) -> i32 { SysReAllocStringLen(b, s, wstr(s, usize::MAX).len() as u32) }
    fn VariantCopyInd(d: *mut u8, s: *const u8) -> i32 {
        let f = lookup(c"oleaut32.dll", c"VariantCopy");
        if f.is_null() { return E_FAIL; }
        let f: winfn!(fn(*mut u8, *const u8) -> i32) = core::mem::transmute(f);
        f(d, s)
    }
    fn VariantChangeType(d: *mut u8, s: *const u8, f: u16, vt: u16) -> i32 {
        let src = &*(s as *const [u8; 16]);
        if u16::from_le_bytes([src[0], src[1]]) == vt { return VariantCopyInd(d, s); }
        match u::variant_change(src, vt) {
            Ok(v) => { core::ptr::copy_nonoverlapping(v.as_ptr(), d, 16); S_OK }
            Err(e) => {
                // the C version cleared the destination once the source type was accepted
                if !matches!(u16::from_le_bytes([src[0], src[1]]), 2 | 3 | 11 | 17 | 19) { return e; }
                core::ptr::write_bytes(d, 0, 16); *d = vt as u8; *d.add(1) = (vt >> 8) as u8; e
            }
        }
    }
    fn VariantChangeTypeEx(d: *mut u8, s: *const u8, l: u32, f: u16, vt: u16) -> i32 { VariantChangeType(d, s, f, vt) }
    fn GetErrorInfo(r: u32, e: PV) -> i32 { *e = core::ptr::null_mut(); S_FALSE }
    fn SetErrorInfo(r: u32, e: *mut c_void) -> i32 { S_OK }
    fn CreateErrorInfo(e: PV) -> i32 { *e = core::ptr::null_mut(); E_OUTOFMEMORY }
    fn LoadTypeLib(f: W, t: PV) -> i32 { *t = core::ptr::null_mut(); 0x8002_9C4Au32 as i32 }
    fn LoadRegTypeLib(g: *const c_void, a: u16, b: u16, l: u32, t: PV) -> i32 { *t = core::ptr::null_mut(); 0x8002_801Du32 as i32 }
    fn RegisterTypeLib(t: *mut c_void, a: W, b: W) -> i32 { S_OK }
    fn VarBstrCmp(a: W, b: W, l: u32, f: u32) -> i32 {
        if a.is_null() || b.is_null() { return if a == b { 1 } else if !a.is_null() { 2 } else { 0 }; }
        let mut i = 0; while *a.add(i) != 0 && *a.add(i) == *b.add(i) { i += 1; }
        let (x, y) = (*a.add(i), *b.add(i));
        if x == y { 1 } else if x < y { 0 } else { 2 }
    }
    fn SystemTimeToVariantTime(st: *const u16, v: *mut f64) -> i32 { *v = u::ole_date(&*(st as *const [u16; 8])); 1 }

    // ------------------------------------------------------------ advapi32: security and the event log
    fn AccessCheck(sd: *mut c_void, t: H, want: u32, map: *mut c_void, ps: *mut c_void, pslen: P, granted: P, ok: *mut i32) -> i32 {
        if !granted.is_null() { *granted = want; } if !ok.is_null() { *ok = 1; } 1
    }
    fn ImpersonateSelf(level: i32) -> i32 { 1 }
    fn RevertToSelf() -> i32 { 1 }
    fn GetFileSecurityA(f: A, i: u32, sd: *mut u8, n: u32, need: P) -> i32 {
        if !need.is_null() { *need = 20; }
        if n < 20 || sd.is_null() { set_err(122); return 0; }
        core::ptr::write_bytes(sd, 0, 20); *sd = 1; 1
    }
    fn SetFileSecurityA(f: A, i: u32, sd: *mut c_void) -> i32 { 1 }
    fn GetNamedSecurityInfoW(n: W, ty: i32, info: u32, owner: PV, group: PV, dacl: PV, sacl: PV, sd: PV) -> u32 {
        for p in [owner, group, dacl, sacl] { if !p.is_null() { *p = core::ptr::null_mut(); } }
        if !sd.is_null() { let m = local_alloc(20); *sd = m as *mut c_void; if !m.is_null() { *m = 1; } }
        0
    }
    fn SetNamedSecurityInfoW(n: *mut u16, ty: i32, info: u32, o: *mut c_void, g: *mut c_void, d: *mut c_void, s: *mut c_void) -> u32 { 0 }
    fn GetSecurityDescriptorOwner(sd: *mut c_void, owner: PV, def: *mut i32) -> i32 { *owner = core::ptr::null_mut(); if !def.is_null() { *def = 0; } 1 }
    fn GetSecurityDescriptorDacl(sd: *mut c_void, present: *mut i32, dacl: PV, def: *mut i32) -> i32 { *present = 0; *dacl = core::ptr::null_mut(); if !def.is_null() { *def = 0; } 1 }
    fn GetSidIdentifierAuthority(sid: *mut u8) -> *mut u8 { sid.add(2) }
    fn GetSidSubAuthority(sid: *mut u8, i: u32) -> *mut u32 { sid.add(8 + 4 * i as usize) as *mut u32 }
    fn GetSidSubAuthorityCount(sid: *mut u8) -> *mut u8 { sid.add(1) }
    fn ConvertSidToStringSidW(sid: *mut c_void, out: *mut *mut u16) -> i32 { let p = local_alloc(64) as *mut u16; *out = p; a2w(b"S-1-5-21-1000-1000-1000-1000", p, 32); 1 }
    fn ConvertSidToStringSidA(sid: *mut c_void, out: *mut *mut u8) -> i32 {
        let s = b"S-1-5-21-1000-1000-1000-1000\0";
        let p = local_alloc(32); *out = p;
        if !p.is_null() { core::ptr::copy_nonoverlapping(s.as_ptr(), p, s.len()); }
        1
    }
    fn LookupAccountSidW(s: W, sid: *mut c_void, name: *mut u16, nn: P, dom: *mut u16, dn: P, used: *mut i32) -> i32 {
        let (un, cn) = (cbytes(&user_name), cbytes(&computer_name));
        let (l, dl) = (un.len() as u32, cn.len() as u32);
        if name.is_null() || *nn <= l || dom.is_null() || *dn <= dl { *nn = l + 1; *dn = dl + 1; set_err(122); return 0; }
        a2w(un, name, *nn as usize); a2w(cn, dom, *dn as usize);
        *nn = l; *dn = dl;
        if !used.is_null() { *used = 1; }
        1
    }
    fn IsTextUnicode(b: *const u8, n: i32, r: *mut i32) -> i32 {
        let (u, f) = u::text_is_unicode(if b.is_null() || n <= 0 { &[] } else { core::slice::from_raw_parts(b, n as usize) });
        if !r.is_null() { *r = f; }
        u as i32
    }
    fn CryptAcquireContextW(h: *mut H, a: W, b: W, t: u32, f: u32) -> i32 { *h = 1; 1 }
    fn RegisterEventSourceW(s: W, n: W) -> H { 0x4556_4C31 }
    fn RegisterEventSourceA(s: A, n: A) -> H { 0x4556_4C31 }
    fn DeregisterEventSource(h: H) -> i32 { 1 }
    fn ReportEventW(h: H, ty: u16, cat: u16, id: u32, sid: *mut c_void, n: u16, dl: u32, s: *const W, d: *mut c_void) -> i32 { 1 }
    fn ReportEventA(h: H, ty: u16, cat: u16, id: u32, sid: *mut c_void, n: u16, dl: u32, s: *const A, d: *mut c_void) -> i32 {
        if n != 0 && !s.is_null() && !(*s).is_null() { klog(c"w32: event log: %s".as_ptr(), *s); }
        1
    }
    fn EventRegister(g: *const c_void, cb: *mut c_void, ctx: *mut c_void, h: *mut u64) -> u32 { if !h.is_null() { *h = 1; } 0 }
    fn EventUnregister(h: u64) -> u32 { 0 }
    fn EventWrite(h: u64, d: *const c_void, n: u32, data: *mut c_void) -> u32 { 0 }
    fn EventEnabled(h: u64, d: *const c_void) -> i32 { 0 }
    fn OpenSCManagerW(m: W, d: W, a: u32) -> H { set_err(5); 0 }
    fn OpenSCManagerA(m: A, d: A, a: u32) -> H { set_err(5); 0 }
    fn CloseServiceHandle(h: H) -> i32 { 1 }
    fn SystemFunction040(m: *mut c_void, n: u32, f: u32) -> i32 { 0 }
    fn SystemFunction041(m: *mut c_void, n: u32, f: u32) -> i32 { 0 }
    // bcrypt: random numbers, the rest not provided
    fn BCryptGenRandom(alg: H, b: *mut u8, n: u32, f: u32) -> i32 { rand_fill(b, n as usize); 0 }
    fn BCryptOpenAlgorithmProvider(h: *mut H, id: W, im: W, f: u32) -> i32 { *h = 0; 0xC000_0225u32 as i32 }
    fn BCryptCloseAlgorithmProvider(h: H, f: u32) -> i32 { 0 }
    fn ProcessPrng(b: *mut u8, n: usize) -> i32 { rand_fill(b, n); 1 }
    // crypt32: no certificate stores
    fn CertOpenSystemStoreA(p: usize, n: A) -> H { set_err(2); 0 }
    fn CertOpenSystemStoreW(p: usize, n: W) -> H { set_err(2); 0 }
    fn CertOpenStore(p: A, e: u32, h: usize, f: u32, para: *const c_void) -> H { set_err(2); 0 }
    fn CertCloseStore(h: H, f: u32) -> i32 { 1 }
    fn CertEnumCertificatesInStore(h: H, prev: *const c_void) -> *const c_void { set_err(0x8009_2004); core::ptr::null() }
    fn CertFreeCertificateContext(c: *const c_void) -> i32 { 1 }
    fn CertGetEnhancedKeyUsage(c: *const c_void, f: u32, us: *mut c_void, n: P) -> i32 { set_err(0x8009_2004); 0 }
    fn CertGetIntendedKeyUsage(e: u32, info: *mut c_void, us: *mut u8, n: u32) -> i32 { 0 }
    fn CertGetNameStringA(c: *const c_void, t: u32, f: u32, p: *mut c_void, b: *mut u8, n: u32) -> u32 { if !b.is_null() && n != 0 { *b = 0; } 1 }
    fn CertGetNameStringW(c: *const c_void, t: u32, f: u32, p: *mut c_void, b: *mut u16, n: u32) -> u32 { if !b.is_null() && n != 0 { *b = 0; } 1 }
    fn CryptStringToBinaryA(s: A, n: u32, f: u32, b: *mut u8, bn: P, skip: P, ff: P) -> i32 { 0 }
    fn CryptProtectData(i: *mut c_void, d: W, e: *mut c_void, r: *mut c_void, p: *mut c_void, f: u32, o: *mut c_void) -> i32 { 0 }
    fn CryptUnprotectData(i: *mut c_void, d: *mut *mut u16, e: *mut c_void, r: *mut c_void, p: *mut c_void, f: u32, o: *mut c_void) -> i32 { 0 }
    // setupapi and cfgmgr32: no device enumeration for programs
    fn SetupDiGetClassDevsA(g: *const c_void, e: A, h: H, f: u32) -> H { set_err(13); usize::MAX }
    fn SetupDiGetClassDevsW(g: *const c_void, e: W, h: H, f: u32) -> H { set_err(13); usize::MAX }
    fn SetupDiDestroyDeviceInfoList(h: H) -> i32 { 1 }
    fn SetupDiEnumDeviceInfo(h: H, i: u32, d: *mut c_void) -> i32 { set_err(259); 0 }
    fn SetupDiEnumDeviceInterfaces(h: H, d: *mut c_void, g: *const c_void, i: u32, o: *mut c_void) -> i32 { set_err(259); 0 }
    fn SetupDiGetDeviceInterfaceDetailA(h: H, i: *mut c_void, d: *mut c_void, n: u32, need: P, di: *mut c_void) -> i32 { set_err(259); 0 }
    fn SetupDiGetDeviceInterfaceDetailW(h: H, i: *mut c_void, d: *mut c_void, n: u32, need: P, di: *mut c_void) -> i32 { set_err(259); 0 }
    fn SetupDiGetDeviceRegistryPropertyA(h: H, d: *mut c_void, p: u32, t: P, b: *mut u8, n: u32, need: P) -> i32 { set_err(13); 0 }
    fn SetupDiGetDeviceRegistryPropertyW(h: H, d: *mut c_void, p: u32, t: P, b: *mut u8, n: u32, need: P) -> i32 { set_err(13); 0 }
    fn CM_Locate_DevNodeA(n: P, id: A, f: u32) -> u32 { 0xD }        // CR_NO_SUCH_DEVNODE
    fn CM_Locate_DevNodeW(n: P, id: W, f: u32) -> u32 { 0xD }
    fn CM_Get_Parent(p: P, n: u32, f: u32) -> u32 { 0xD }
    fn CM_Get_Device_IDA(n: u32, b: *mut u8, l: u32, f: u32) -> u32 { 0xD }
    fn CM_Get_Device_IDW(n: u32, b: *mut u16, l: u32, f: u32) -> u32 { 0xD }
    // winspool: no printers
    fn OpenPrinterA(n: *mut u8, h: *mut H, d: *mut c_void) -> i32 { *h = 0; set_err(1801); 0 }
    fn OpenPrinterW(n: *mut u16, h: *mut H, d: *mut c_void) -> i32 { *h = 0; set_err(1801); 0 }
    fn ClosePrinter(h: H) -> i32 { 1 }
    fn DocumentPropertiesA(w: H, h: H, n: *mut u8, o: *mut c_void, i: *mut c_void, m: u32) -> i32 { -1 }
    fn DocumentPropertiesW(w: H, h: H, n: *mut u16, o: *mut c_void, i: *mut c_void, m: u32) -> i32 { -1 }
    fn EnumPrintersW(f: u32, n: *mut u16, l: u32, b: *mut u8, c: u32, need: P, ret: P) -> i32 { if !need.is_null() { *need = 0; } if !ret.is_null() { *ret = 0; } 1 }
    fn EnumPrintersA(f: u32, n: *mut u8, l: u32, b: *mut u8, c: u32, need: P, ret: P) -> i32 { if !need.is_null() { *need = 0; } if !ret.is_null() { *ret = 0; } 1 }
    fn GetDefaultPrinterW(b: *mut u16, n: P) -> i32 { set_err(1801); 0 }
    fn GetDefaultPrinterA(b: *mut u8, n: P) -> i32 { set_err(1801); 0 }
    // wldap32: no directory server
    fn ldap_init(h: A, p: u32) -> *mut c_void { set_err(0x51); core::ptr::null_mut() }
    fn ldap_sslinit(h: A, p: u32, s: i32) -> *mut c_void { set_err(0x51); core::ptr::null_mut() }
    fn ldap_err_ul() -> u32 { 0x51 }                                      // LDAP_SERVER_DOWN
    fn ldap_bind_s(l: *mut c_void, dn: A, c: A, m: u32) -> u32 { 0x51 }
    fn ldap_simple_bind_s(l: *mut c_void, dn: A, pw: A) -> u32 { 0x51 }
    fn ldap_unbind_s(l: *mut c_void) -> u32 { 0 }
    fn ldap_search_s(l: *mut c_void, b: A, s: u32, f: A, at: *mut *mut u8, o: u32, res: PV) -> u32 { if !res.is_null() { *res = core::ptr::null_mut(); } 0x51 }
    fn ldap_set_option(l: *mut c_void, o: i32, v: *const c_void) -> u32 { 0 }
    fn ldap_err2string(e: u32) -> *const c_char { c"The LDAP server is unavailable.".as_ptr() }
    fn ldap_null() -> *mut c_void { core::ptr::null_mut() }
    fn ldap_zero() -> u32 { 0 }
    // netapi32: not in a domain
    fn NetApiBufferFree(b: *mut c_void) -> u32 { 0 }
    fn NetGetDCName(s: W, d: W, b: *mut *mut u8) -> u32 { *b = core::ptr::null_mut(); 2453 }   // NERR_DCNotFound
    fn NetUserGetInfo(s: W, un: W, l: u32, b: *mut *mut u8) -> u32 { *b = core::ptr::null_mut(); 2221 }   // NERR_UserNotFound
    fn NetWkstaGetInfo(s: *mut u16, l: u32, b: *mut *mut u8) -> u32 { *b = core::ptr::null_mut(); 2102 }
    // normaliz: host names are kept as they are (ASCII names need no conversion)
    fn IdnToAscii(f: u32, inp: W, n: i32, out: *mut u16, on: i32) -> i32 {
        let n = if n < 0 { wstr(inp, usize::MAX).len() as i32 + 1 } else { n };
        for i in 0..n as usize { if *inp.add(i) > 127 { set_err(1113); return 0; } }
        if on == 0 { return n; }
        if on < n { set_err(122); return 0; }
        core::ptr::copy_nonoverlapping(inp, out, n as usize);
        n
    }
    fn IdnToUnicode(f: u32, inp: W, n: i32, out: *mut u16, on: i32) -> i32 {
        let n = if n < 0 { wstr(inp, usize::MAX).len() as i32 + 1 } else { n };
        if on == 0 { return n; }
        if on < n { set_err(122); return 0; }
        core::ptr::copy_nonoverlapping(inp, out, n as usize);
        n
    }
    // imm32: no input method editor
    fn ImmGetCandidateListW(c: H, i: u32, l: *mut c_void, n: u32) -> u32 { 0 }
    fn ImmGetIMEFileNameA(k: H, b: *mut u8, n: u32) -> u32 { if !b.is_null() && n != 0 { *b = 0; } 0 }
    fn ImmNotifyIME(c: H, a: u32, i: u32, v: u32) -> i32 { 0 }
    fn ImmSetCandidateWindow(c: H, cf: *mut c_void) -> i32 { 1 }
    fn ImmSetCompositionStringW(c: H, i: u32, a: *const c_void, n: u32, b: *const c_void, m: u32) -> i32 { 0 }
    fn ImmIsIME(k: H) -> i32 { 0 }
    fn ImmGetDefaultIMEWnd(h: H) -> H { 0 }
    fn ImmDisableIME(t: u32) -> i32 { 1 }
    // wtsapi32, userenv, secur32, oleacc, iphlpapi
    fn WTSRegisterSessionNotification(h: H, f: u32) -> i32 { 1 }
    fn WTSUnRegisterSessionNotification(h: H) -> i32 { 1 }
    fn WTSQuerySessionInformationW(s: H, id: u32, c: i32, b: *mut *mut u16, n: P) -> i32 { set_err(1); 0 }
    fn WTSFreeMemory(p: *mut c_void) -> () {}
    fn GetProfilesDirectoryW(b: *mut u16, n: P) -> i32 { put_dir(b"C:\\Documents and Settings", b as *mut u8, n, true) }
    fn GetUserProfileDirectoryW(t: H, b: *mut u16, n: P) -> i32 { put_dir(USER_HOME, b as *mut u8, n, true) }
    fn GetUserProfileDirectoryA(t: H, b: *mut u8, n: P) -> i32 { put_dir(USER_HOME, b, n, false) }
    fn InitSecurityInterfaceA() -> *mut c_void { core::ptr::null_mut() }
    fn InitSecurityInterfaceW() -> *mut c_void { core::ptr::null_mut() }
    fn LresultFromObject(iid: *const c_void, w: usize, o: *mut c_void) -> i32 { E_NOTIMPL }
    fn AccessibleObjectFromWindow(h: H, id: u32, iid: *const c_void, o: PV) -> i32 { *o = core::ptr::null_mut(); E_NOTIMPL }
    fn if_nametoindex(n: A) -> u32 { 0 }
    fn GetAdaptersInfo(a: *mut c_void, n: P) -> u32 { if !n.is_null() { *n = 0; } 232 }   // ERROR_NO_DATA
    fn GetAdaptersAddresses(f: u32, fl: u32, r: *mut c_void, a: *mut c_void, n: P) -> u32 { if !n.is_null() { *n = 0; } 232 }
    fn GetBestInterface(a: u32, i: P) -> u32 { *i = 1; 0 }
    fn GetNetworkParams(p: *mut c_void, n: P) -> u32 { if !n.is_null() { *n = 0; } 232 }
    // fibers: a program asking for them gets a refusal it can report
    fn ConvertThreadToFiber(p: *mut c_void) -> *mut c_void { FIBER[0] = p as usize; core::ptr::addr_of_mut!(FIBER) as *mut c_void }
    fn ConvertThreadToFiberEx(p: *mut c_void, f: u32) -> *mut c_void { ConvertThreadToFiber(p) }
    fn ConvertFiberToThread() -> i32 { 1 }
    fn CreateFiber(s: usize, f: *mut c_void, p: *mut c_void) -> *mut c_void { set_err(50); core::ptr::null_mut() }
    fn CreateFiberEx(a: usize, b: usize, fl: u32, f: *mut c_void, p: *mut c_void) -> *mut c_void { set_err(50); core::ptr::null_mut() }
    fn DeleteFiber(f: *mut c_void) -> () {}
    fn SwitchToFiber(f: *mut c_void) -> () {}
}
static mut FIBER: [usize; 4] = [0; 4];

api_table! { misc_api;
    "ole32": CLSIDFromString, IIDFromString, StringFromCLSID, StringFromCLSID = "StringFromIID", StringFromGUID2, CoCreateGuid, IsEqualGUID,
        CoDisconnectObject, CreateDataAdviseHolder, CreateStreamOnHGlobal, GetHGlobalFromStream, DoDragDrop, RegisterDragDrop, RevokeDragDrop,
        OleGetClipboard, OleSetClipboard, OleFlushClipboard, OleIsCurrentClipboard, PropVariantClear, ReleaseStgMedium, CoRegisterClassObject,
        CoRevokeClassObject, CoInitializeSecurity, CoGetClassObject, CoLockObjectExternal, CoSetProxyBlanket, OleInitialize2 = "OleInitialize",
        CoFreeUnusedLibraries, ProgIDFromCLSID, CLSIDFromProgID;
    "oleaut32": SafeArrayCreate, SafeArrayCreateVector, SafeArrayDestroy, SafeArrayAccessData, SafeArrayUnaccessData, SafeArrayLock,
        SafeArrayUnlock, SafeArrayGetDim, SafeArrayGetElemsize, SafeArrayGetLBound, SafeArrayGetUBound, SafeArrayPtrOfIndex, SafeArrayGetElement,
        SafeArrayPutElement, SafeArrayCopy, SysReAllocStringLen, SysReAllocString, VariantCopyInd, VariantChangeType, VariantChangeTypeEx,
        GetErrorInfo, SetErrorInfo, CreateErrorInfo, LoadTypeLib, LoadRegTypeLib, RegisterTypeLib, VarBstrCmp, SystemTimeToVariantTime,
        SysReAllocString = "Ordinal3", SysReAllocStringLen = "Ordinal5", VariantCopyInd = "Ordinal11", VariantChangeType = "Ordinal12",
        SafeArrayCreate = "Ordinal15", SafeArrayDestroy = "Ordinal16", SafeArrayGetDim = "Ordinal17", SafeArrayGetElemsize = "Ordinal18",
        SafeArrayGetUBound = "Ordinal19", SafeArrayGetLBound = "Ordinal20", SafeArrayLock = "Ordinal21", SafeArrayUnlock = "Ordinal22",
        SafeArrayAccessData = "Ordinal23", SafeArrayUnaccessData = "Ordinal24", SafeArrayGetElement = "Ordinal25",
        SafeArrayPutElement = "Ordinal26", SafeArrayCopy = "Ordinal27", SafeArrayPtrOfIndex = "Ordinal148", GetErrorInfo = "Ordinal200",
        SetErrorInfo = "Ordinal201", VariantChangeTypeEx = "Ordinal147", SafeArrayCreateVector = "Ordinal411", LoadTypeLib = "Ordinal161";
    "advapi32": AccessCheck, ImpersonateSelf, RevertToSelf, GetFileSecurityA, SetFileSecurityA, GetNamedSecurityInfoW, SetNamedSecurityInfoW,
        GetSecurityDescriptorOwner, GetSecurityDescriptorDacl, GetSidIdentifierAuthority, GetSidSubAuthority, GetSidSubAuthorityCount,
        ConvertSidToStringSidW, ConvertSidToStringSidA, LookupAccountSidW, IsTextUnicode, CryptAcquireContextW, RegisterEventSourceW,
        RegisterEventSourceA, DeregisterEventSource, ReportEventW, ReportEventA, EventRegister, EventUnregister, EventWrite, EventEnabled,
        OpenSCManagerW, OpenSCManagerA, CloseServiceHandle, SystemFunction040, SystemFunction041;
    "bcrypt": BCryptGenRandom, BCryptOpenAlgorithmProvider, BCryptCloseAlgorithmProvider;
    "bcryptprimitives": ProcessPrng;
    "crypt32": CertOpenSystemStoreA, CertOpenSystemStoreW, CertOpenStore, CertCloseStore, CertEnumCertificatesInStore, CertFreeCertificateContext,
        CertGetEnhancedKeyUsage, CertGetIntendedKeyUsage, CertGetNameStringA, CertGetNameStringW, CryptStringToBinaryA, CryptProtectData,
        CryptUnprotectData;
    "setupapi": SetupDiGetClassDevsA, SetupDiGetClassDevsW, SetupDiDestroyDeviceInfoList, SetupDiEnumDeviceInfo, SetupDiEnumDeviceInterfaces,
        SetupDiGetDeviceInterfaceDetailA, SetupDiGetDeviceInterfaceDetailW, SetupDiGetDeviceRegistryPropertyA, SetupDiGetDeviceRegistryPropertyW,
        CM_Locate_DevNodeA, CM_Locate_DevNodeW, CM_Get_Parent, CM_Get_Device_IDA, CM_Get_Device_IDW;
    "cfgmgr32": CM_Locate_DevNodeW, CM_Get_Parent, CM_Get_Device_IDW;
    "winspool": OpenPrinterA, OpenPrinterW, ClosePrinter, DocumentPropertiesA, DocumentPropertiesW, EnumPrintersW, EnumPrintersA,
        GetDefaultPrinterW, GetDefaultPrinterA;
    "wldap32": ldap_init, ldap_sslinit, ldap_bind_s, ldap_simple_bind_s, ldap_unbind_s, ldap_search_s, ldap_set_option, ldap_err2string,
        ldap_null = "ldap_first_entry", ldap_null = "ldap_next_entry", ldap_null = "ldap_first_attribute", ldap_null = "ldap_next_attribute",
        ldap_null = "ldap_get_dn", ldap_null = "ldap_get_values_len", ldap_zero = "ldap_value_free_len", ldap_zero = "ldap_memfree",
        ldap_zero = "ldap_msgfree", ldap_zero = "ber_free", ldap_err_ul = "LdapGetLastError";
    "netapi32": NetApiBufferFree, NetGetDCName, NetUserGetInfo, NetWkstaGetInfo;
    "normaliz": IdnToAscii, IdnToUnicode;
    "kernel32": IdnToAscii, IdnToUnicode;
    "imm32": ImmGetCandidateListW, ImmGetIMEFileNameA, ImmNotifyIME, ImmSetCandidateWindow, ImmSetCompositionStringW, ImmIsIME,
        ImmGetDefaultIMEWnd, ImmDisableIME;
    "wtsapi32": WTSRegisterSessionNotification, WTSUnRegisterSessionNotification, WTSQuerySessionInformationW, WTSFreeMemory;
    "userenv": GetProfilesDirectoryW, GetUserProfileDirectoryW, GetUserProfileDirectoryA;
    "secur32": InitSecurityInterfaceA, InitSecurityInterfaceW;
    "oleacc": LresultFromObject, AccessibleObjectFromWindow;
    "iphlpapi": if_nametoindex, GetAdaptersInfo, GetAdaptersAddresses, GetBestInterface, GetNetworkParams;
    "kernel32": ConvertThreadToFiber, ConvertThreadToFiberEx, ConvertFiberToThread, CreateFiber, CreateFiberEx, DeleteFiber, SwitchToFiber;
}
