//! What the Win32 API modules share: the Windows calling convention of each edition
//! (`winapi!`, `winfn!`), the tables `src/win32/apitab.c` searches (`api_table!`) and the
//! process's last error, heap and exports.

use core::ffi::{c_char, c_void};

/// Functions in the calling convention Win32 programs use: stdcall on x86, the Microsoft
/// x64 convention on x86-64.
macro_rules! winapi {
    ($(fn $name:ident($($a:ident: $t:ty),* $(,)?) -> $r:ty $body:block)*) => {$(
        #[cfg(target_arch = "x86")] #[allow(non_snake_case, unused_variables)]
        unsafe extern "stdcall" fn $name($($a: $t),*) -> $r $body
        #[cfg(target_arch = "x86_64")] #[allow(non_snake_case, unused_variables)]
        unsafe extern "win64" fn $name($($a: $t),*) -> $r $body
    )*};
}

/// The type of a Win32 function pointer: `winfn!(fn(u32, usize) -> *mut c_void)`.
#[cfg(target_arch = "x86")]
macro_rules! winfn { (fn($($t:ty),*) -> $r:ty) => { unsafe extern "stdcall" fn($($t),*) -> $r }; }
#[cfg(target_arch = "x86_64")]
macro_rules! winfn { (fn($($t:ty),*) -> $r:ty) => { unsafe extern "win64" fn($($t),*) -> $r }; }

/// One entry: a DLL group ("kernel32", "shlwapi", ...), a name and the function
/// (src/win32/w32.h's w32_api_t).
#[repr(C)]
pub struct W32Api { pub group: *const c_char, pub name: *const c_char, pub f: *const c_void }
unsafe impl Sync for W32Api {}

/// `api_table!(name; "dll": Func, Other = "ExportedName", ...; "dll2": ...;)` exports a
/// w32_api_t table called `name`, ending with an empty entry.
macro_rules! api_table {
    ($table:ident; $($g:literal: $($n:ident $(= $alias:literal)?),* $(,)?;)*) => {
        #[no_mangle]
        #[allow(non_upper_case_globals)]
        pub static $table: [$crate::kernel::w32::W32Api; <[()]>::len(&[$($(api_table!(@unit $n)),*),*]) + 1] = [
            $($($crate::kernel::w32::W32Api {
                group: concat!($g, "\0").as_ptr() as *const core::ffi::c_char,
                name: api_table!(@name $n $($alias)?),
                f: $n as *const core::ffi::c_void,
            },)*)*
            $crate::kernel::w32::W32Api { group: core::ptr::null(), name: core::ptr::null(), f: core::ptr::null() },
        ];
    };
    (@unit $n:ident) => { () };
    (@name $n:ident) => { concat!(stringify!($n), "\0").as_ptr() as *const core::ffi::c_char };
    (@name $n:ident $alias:literal) => { concat!($alias, "\0").as_ptr() as *const core::ffi::c_char };
}

extern "C" {
    fn w32_cur() -> *mut c_void;
    fn w32_alloc(p: *mut c_void, n: u32) -> *mut c_void;
    fn w32_free(p: *mut c_void, ptr: *mut c_void);
    fn w32_lookup(dll: *const c_char, name: *const c_char, out: *mut *mut c_void) -> super::CBool;
    fn w32_set_last_error(e: u32);
}

/// SetLastError for the calling program.
pub fn set_err(e: u32) { unsafe { w32_set_last_error(e) } }
/// Memory from the calling program's heap (what its HeapAlloc and malloc use), or NULL.
pub fn alloc(n: usize) -> *mut u8 {
    if n > u32::MAX as usize { return core::ptr::null_mut(); }
    unsafe { let p = w32_cur(); if p.is_null() { core::ptr::null_mut() } else { w32_alloc(p, n as u32) as *mut u8 } }
}
pub fn free(ptr: *mut u8) { unsafe { let p = w32_cur(); if !p.is_null() && !ptr.is_null() { w32_free(p, ptr as *mut c_void); } } }
/// Another Win32 export ("kernel32.dll", "LocalAlloc"), or NULL; `dll` and `name` end with NUL.
pub fn lookup(dll: &core::ffi::CStr, name: &core::ffi::CStr) -> *mut c_void {
    let mut f = core::ptr::null_mut();
    unsafe { w32_lookup(dll.as_ptr(), name.as_ptr(), &mut f) };
    f
}

/// A NUL-terminated wide string's characters (empty for NULL), at most `max`.
pub unsafe fn wstr<'a>(p: *const u16, max: usize) -> &'a [u16] {
    if p.is_null() { return &[]; }
    let mut n = 0; while n < max && *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
/// A NUL-terminated byte string (empty for NULL).
pub unsafe fn astr<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0; while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
/// Bytes widened into a wide buffer of `cap` characters with a NUL (the C files' a2w);
/// returns the characters written.
pub unsafe fn a2w(a: &[u8], w: *mut u16, cap: usize) -> usize {
    if w.is_null() || cap == 0 { return 0; }
    let n = a.len().min(cap - 1);
    for (i, &c) in a[..n].iter().enumerate() { *w.add(i) = c as u16; }
    *w.add(n) = 0;
    n
}
