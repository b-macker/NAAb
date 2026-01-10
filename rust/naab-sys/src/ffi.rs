// Rust FFI bindings for NAAb C interface
// Mirrors naab/rust_ffi.h

use std::os::raw::{c_char, c_int, c_double};
use std::ffi::{CStr, CString};

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum NaabRustValueType {
    Void = 0,
    Int = 1,
    Double = 2,
    Bool = 3,
    String = 4,
}

/// Opaque handle to NAAb value (matches C struct)
#[repr(C)]
pub struct NaabRustValue {
    _private: [u8; 0],
}

/// Block function signature for extern "C" functions
pub type NaabRustBlockFn = unsafe extern "C" fn(
    args: *mut *mut NaabRustValue,
    arg_count: usize,
) -> *mut NaabRustValue;

// ============================================================================
// External C Functions (from naab_runtime)
// ============================================================================

extern "C" {
    // Value creation
    pub fn naab_rust_value_create_int(value: c_int) -> *mut NaabRustValue;
    pub fn naab_rust_value_create_double(value: c_double) -> *mut NaabRustValue;
    pub fn naab_rust_value_create_bool(value: bool) -> *mut NaabRustValue;
    pub fn naab_rust_value_create_string(value: *const c_char) -> *mut NaabRustValue;
    pub fn naab_rust_value_create_void() -> *mut NaabRustValue;

    // Value access
    pub fn naab_rust_value_get_int(value: *const NaabRustValue) -> c_int;
    pub fn naab_rust_value_get_double(value: *const NaabRustValue) -> c_double;
    pub fn naab_rust_value_get_bool(value: *const NaabRustValue) -> bool;
    pub fn naab_rust_value_get_string(value: *const NaabRustValue) -> *const c_char;
    pub fn naab_rust_value_get_type(value: *const NaabRustValue) -> NaabRustValueType;

    // Memory management
    pub fn naab_rust_value_free(value: *mut NaabRustValue);
}

// ============================================================================
// Safe Rust Wrappers (Tasks 3.2.5-3.2.10)
// ============================================================================

/// Safe wrapper for creating int value
pub fn create_int(value: i32) -> *mut NaabRustValue {
    unsafe { naab_rust_value_create_int(value as c_int) }
}

/// Safe wrapper for creating double value
pub fn create_double(value: f64) -> *mut NaabRustValue {
    unsafe { naab_rust_value_create_double(value as c_double) }
}

/// Safe wrapper for creating bool value
pub fn create_bool(value: bool) -> *mut NaabRustValue {
    unsafe { naab_rust_value_create_bool(value) }
}

/// Safe wrapper for creating string value
pub fn create_string(value: &str) -> *mut NaabRustValue {
    let c_string = CString::new(value).expect("String contains null byte");
    unsafe { naab_rust_value_create_string(c_string.as_ptr()) }
}

/// Safe wrapper for creating void value
pub fn create_void() -> *mut NaabRustValue {
    unsafe { naab_rust_value_create_void() }
}

/// Safe wrapper for getting int value
pub fn get_int(value: *const NaabRustValue) -> i32 {
    unsafe { naab_rust_value_get_int(value) as i32 }
}

/// Safe wrapper for getting double value
pub fn get_double(value: *const NaabRustValue) -> f64 {
    unsafe { naab_rust_value_get_double(value) as f64 }
}

/// Safe wrapper for getting bool value
pub fn get_bool(value: *const NaabRustValue) -> bool {
    unsafe { naab_rust_value_get_bool(value) }
}

/// Safe wrapper for getting string value
pub fn get_string(value: *const NaabRustValue) -> String {
    unsafe {
        let c_str = naab_rust_value_get_string(value);
        if c_str.is_null() {
            String::new()
        } else {
            CStr::from_ptr(c_str)
                .to_string_lossy()
                .into_owned()
        }
    }
}

/// Safe wrapper for getting value type
pub fn get_type(value: *const NaabRustValue) -> NaabRustValueType {
    unsafe { naab_rust_value_get_type(value) }
}

/// Safe wrapper for freeing value
pub fn free_value(value: *mut NaabRustValue) {
    if !value.is_null() {
        unsafe { naab_rust_value_free(value) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_create_and_free() {
        let val = create_int(42);
        assert!(!val.is_null());
        free_value(val);
    }

    #[test]
    fn test_int_roundtrip() {
        let val = create_int(123);
        assert_eq!(get_int(val), 123);
        free_value(val);
    }

    #[test]
    fn test_string_roundtrip() {
        let val = create_string("Hello, Rust!");
        assert_eq!(get_string(val), "Hello, Rust!");
        free_value(val);
    }
}
