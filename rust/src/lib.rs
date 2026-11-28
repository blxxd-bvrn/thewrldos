//! wrldrs: the parts of THE WRLD OS kernel written in Rust.
//!
//! The kernel is C (see README, "Rust in the kernel"); this library is linked into it and
//! takes over modules one at a time. Each module keeps the C interface its callers already
//! use, so a module moves to Rust without its callers changing.
//!
//! Rules for code here:
//! - No panics on bad input. A panic in the kernel is a kernel panic, so parsers return
//!   errors instead of indexing out of range or overflowing (`cargo test` fuzzes them).
//! - Pure logic (parsers, codecs) lives in modules that build and test on the host; only
//!   `kernel` (behind the `kernel` feature) touches the C kernel.
//! - C structs that Rust reads or writes are mirrored with `#[repr(C)]`, and C exports
//!   their real layout so `rust_init` can check it at boot. The kernel's `bool` is an `int`:
//!   use `kernel::CBool` for it, never Rust's `bool`.
//! - Inline assembly that pushes or pops the flags names the operand size (`pushfd`/`popfd`,
//!   `pushfq`/`popfq`): a bare `pushf` assembles to the 16-bit form and unbalances the stack.
#![cfg_attr(feature = "kernel", no_std)]

extern crate alloc;

pub mod dmg;
pub mod fat;
pub mod media;
pub mod fat32;
pub mod inflate;
pub mod net;
pub mod netstack;
pub mod ntfs;
pub mod vfspath;
pub mod shell;
pub mod imgdec;
pub mod css;
pub mod persist;
pub mod ttf;
pub mod pkgfmt;
pub mod usbdesc;
pub mod dxbc;
pub mod dxil;
pub mod glsl;
pub mod jit;
pub mod jit4;
pub mod hoist;
pub mod lxpath;
pub mod pcm;
pub mod adblock;
pub mod http;
pub mod wpdoc;
pub mod unarc;
pub mod ppmd;
#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub mod html;
#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub mod svg;
#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub mod dom;
pub mod pe;
pub mod registry;
pub mod w32util;

/// The kernel's `bool` (src/include/types.h: `typedef int bool`): 4 bytes, not Rust's bool.
/// Use it for every C bool, in structs and in function arguments.
pub type CBool = core::ffi::c_int;

#[cfg(feature = "kernel")]
pub mod kernel;
