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
#![cfg_attr(feature = "kernel", no_std)]

extern crate alloc;

pub mod dmg;
pub mod fat;
pub mod inflate;
pub mod net;
pub mod netstack;
pub mod pe;
pub mod registry;

/// The kernel's `bool` (src/include/types.h: `typedef int bool`): 4 bytes, not Rust's bool.
/// Use it for every C bool, in structs and in function arguments.
pub type CBool = core::ffi::c_int;

#[cfg(feature = "kernel")]
pub mod kernel;
