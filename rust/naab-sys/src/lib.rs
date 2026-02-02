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

use std::cell::RefCell;
use std::ffi::{CString, CStr};
use std::os::raw::c_char;

// Thread-local error storage for FFI error handling
thread_local! {
    static LAST_ERROR: RefCell<Option<String>> = RefCell::new(None);
}

/// Set the last error message for this thread
pub fn set_last_error(msg: String) {
    LAST_ERROR.with(|e| {
        *e.borrow_mut() = Some(msg);
    });
}

/// Get the last error message for this thread
#[no_mangle]
pub extern "C" fn naab_rust_get_last_error() -> *const c_char {
    LAST_ERROR.with(|e| {
        if let Some(ref err) = *e.borrow() {
            match CString::new(err.as_str()) {
                Ok(c_str) => c_str.into_raw(),
                Err(_) => std::ptr::null()
            }
        } else {
            std::ptr::null()
        }
    })
}

/// Clear the last error for this thread
#[no_mangle]
pub extern "C" fn naab_rust_clear_error() {
    LAST_ERROR.with(|e| {
        *e.borrow_mut() = None;
    });
}

/// Free error string returned by naab_rust_get_last_error
#[no_mangle]
pub unsafe extern "C" fn naab_rust_free_error(s: *mut c_char) {
    if !s.is_null() {
        let _ = CString::from_raw(s);
    }
}

/// Re-exports for convenience
pub mod prelude {
    pub use crate::ffi::{NaabRustValue, NaabRustValueType, NaabRustBlockFn};
    pub use crate::value::Value;
    pub use crate::{naab_block, get_int_arg, get_string_arg, get_double_arg, get_bool_arg};
    pub use crate::set_last_error;
}

naab_block!(add_one, |args: Vec<Value>| {
    let num = get_int_arg!(args, 0, 0);
    Value::Int(num + 1)
});
