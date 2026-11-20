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
#![cfg_attr(feature = "kernel", no_std)]

#[cfg(feature = "kernel")]
extern crate alloc;

pub mod inflate;

#[cfg(feature = "kernel")]
pub mod kernel;
