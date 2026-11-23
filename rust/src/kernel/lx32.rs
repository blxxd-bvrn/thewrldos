//! The 32-bit edition has no Linux layer (rust/src/kernel/lx is x86-64 only): what C asks of it.

use core::ffi::{c_char, c_int, c_void};
use crate::CBool;

#[no_mangle]
pub unsafe extern "C" fn lx_spawn(_path: *const c_char, _argc: c_int, _argv: *mut *mut c_char, _envp: *mut *mut c_char,
                                  _inp: *mut c_void, _out: *mut c_void, err: *mut c_char, ecap: c_int) -> c_int {
    let msg = b"Linux programs need the 64-bit edition of THE WRLD OS.";
    if !err.is_null() && ecap > 0 {
        let n = msg.len().min(ecap as usize - 1);
        core::ptr::copy_nonoverlapping(msg.as_ptr(), err as *mut u8, n);
        *err.add(n) = 0;
    }
    -1
}
#[no_mangle]
pub unsafe extern "C" fn lx_is_elf(d: *const u8, n: u32) -> CBool {
    (n >= 4 && *d == 0x7F && *d.add(1) == b'E' && *d.add(2) == b'L' && *d.add(3) == b'F') as CBool
}
