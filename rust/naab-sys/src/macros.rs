// Convenience macros for NAAb block development

use crate::value::Value;
use crate::ffi::NaabRustValue;

/// Macro for easily creating NAAb blocks
///
/// # Example
///
/// ```ignore
/// use naab_sys::prelude::*;
///
/// naab_block!(add_one, |args: Vec<Value>| {
///     let num = args[0].as_int().unwrap_or(0);
///     Value::Int(num + 1)
/// });
/// ```
#[macro_export]
macro_rules! naab_block {
    ($fn_name:ident, $body:expr) => {
        #[no_mangle]
        pub extern "C" fn $fn_name(
            args: *mut *mut $crate::ffi::NaabRustValue,
            arg_count: usize,
        ) -> *mut $crate::ffi::NaabRustValue {
            use $crate::value::Value;

            // Convert C args to Rust Vec<Value>
            let rust_args: Vec<Value> = unsafe {
                if args.is_null() || arg_count == 0 {
                    Vec::new()
                } else {
                    std::slice::from_raw_parts(args, arg_count)
                        .iter()
                        .map(|&ptr| Value::from_ffi_borrowed(ptr))
                        .collect()
                }
            };

            // Call user function
            let result = $body(rust_args);

            // Convert result back to FFI
            result.to_ffi()
        }
    };
}

/// Helper macro for extracting integer argument
#[macro_export]
macro_rules! get_int_arg {
    ($args:expr, $index:expr, $default:expr) => {
        $args
            .get($index)
            .and_then(|v| v.as_int())
            .unwrap_or($default)
    };
}

/// Helper macro for extracting string argument
#[macro_export]
macro_rules! get_string_arg {
    ($args:expr, $index:expr, $default:expr) => {
        $args
            .get($index)
            .and_then(|v| v.as_string())
            .unwrap_or($default)
    };
}

/// Helper macro for extracting double argument
#[macro_export]
macro_rules! get_double_arg {
    ($args:expr, $index:expr, $default:expr) => {
        $args
            .get($index)
            .and_then(|v| v.as_double())
            .unwrap_or($default)
    };
}

/// Helper macro for extracting bool argument
#[macro_export]
macro_rules! get_bool_arg {
    ($args:expr, $index:expr, $default:expr) => {
        $args
            .get($index)
            .and_then(|v| v.as_bool())
            .unwrap_or($default)
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    // Test the helper macros
    #[test]
    fn test_get_int_arg() {
        let args = vec![Value::Int(42), Value::String("test".to_string())];
        assert_eq!(get_int_arg!(args, 0, 0), 42);
        assert_eq!(get_int_arg!(args, 1, 0), 0); // Wrong type, returns default
        assert_eq!(get_int_arg!(args, 2, 99), 99); // Out of bounds, returns default
    }

    #[test]
    fn test_get_string_arg() {
        let args = vec![Value::String("hello".to_string()), Value::Int(42)];
        assert_eq!(get_string_arg!(args, 0, ""), "hello");
        assert_eq!(get_string_arg!(args, 1, "default"), "default");
    }
}
