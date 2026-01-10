//! # naab-sys
//!
//! Rust FFI bindings for NAAb Block Assembly Language.
//!
//! This crate provides safe Rust wrappers around the C FFI interface,
//! allowing you to write NAAb blocks in idiomatic Rust.
//!
//! ## Example
//!
//! ```ignore
//! use naab_sys::prelude::*;
//!
//! naab_block!(add_numbers, |args: Vec<Value>| {
//!     let a = get_int_arg!(args, 0, 0);
//!     let b = get_int_arg!(args, 1, 0);
//!     Value::Int(a + b)
//! });
//! ```
//!
//! ## Building Blocks
//!
//! To build your block as a shared library:
//!
//! ```bash
//! cargo build --release
//! ```
//!
//! Then use it from NAAb:
//!
//! ```naab
//! let result = call("rust://./target/release/libmy_block.so::add_numbers", 5, 10)
//! print(result)  // 15
//! ```

pub mod ffi;
pub mod value;
#[macro_use]
pub mod macros;

/// Re-exports for convenience
pub mod prelude {
    pub use crate::ffi::{NaabRustValue, NaabRustValueType, NaabRustBlockFn};
    pub use crate::value::Value;
    pub use crate::{naab_block, get_int_arg, get_string_arg, get_double_arg, get_bool_arg};
}
