// High-level Rust API for NAAb values
// Provides idiomatic Rust interface over C FFI

use crate::ffi::{self, NaabRustValue, NaabRustValueType};
use std::fmt;

/// High-level Rust value enum
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Int(i32),
    Double(f64),
    Bool(bool),
    String(String),
    Void,
}

impl Value {
    /// Convert from C FFI pointer to Rust enum
    /// Takes ownership of the FFI value and frees it
    pub fn from_ffi(ffi_val: *mut NaabRustValue) -> Self {
        if ffi_val.is_null() {
            return Value::Void;
        }

        let value_type = ffi::get_type(ffi_val);
        let result = match value_type {
            NaabRustValueType::Int => Value::Int(ffi::get_int(ffi_val)),
            NaabRustValueType::Double => Value::Double(ffi::get_double(ffi_val)),
            NaabRustValueType::Bool => Value::Bool(ffi::get_bool(ffi_val)),
            NaabRustValueType::String => Value::String(ffi::get_string(ffi_val)),
            NaabRustValueType::Void => Value::Void,
        };

        // Free the FFI value after extracting data
        ffi::free_value(ffi_val);

        result
    }

    /// Convert from Rust enum to C FFI pointer
    /// Caller is responsible for freeing the returned pointer
    pub fn to_ffi(&self) -> *mut NaabRustValue {
        match self {
            Value::Int(v) => ffi::create_int(*v),
            Value::Double(v) => ffi::create_double(*v),
            Value::Bool(v) => ffi::create_bool(*v),
            Value::String(v) => ffi::create_string(v),
            Value::Void => ffi::create_void(),
        }
    }

    /// Create from C FFI without taking ownership (borrow)
    pub fn from_ffi_borrowed(ffi_val: *const NaabRustValue) -> Self {
        if ffi_val.is_null() {
            return Value::Void;
        }

        let value_type = ffi::get_type(ffi_val);
        match value_type {
            NaabRustValueType::Int => Value::Int(ffi::get_int(ffi_val)),
            NaabRustValueType::Double => Value::Double(ffi::get_double(ffi_val)),
            NaabRustValueType::Bool => Value::Bool(ffi::get_bool(ffi_val)),
            NaabRustValueType::String => Value::String(ffi::get_string(ffi_val)),
            NaabRustValueType::Void => Value::Void,
        }
    }

    /// Check if value is void
    pub fn is_void(&self) -> bool {
        matches!(self, Value::Void)
    }

    /// Try to extract int value
    pub fn as_int(&self) -> Option<i32> {
        match self {
            Value::Int(v) => Some(*v),
            _ => None,
        }
    }

    /// Try to extract double value
    pub fn as_double(&self) -> Option<f64> {
        match self {
            Value::Double(v) => Some(*v),
            _ => None,
        }
    }

    /// Try to extract bool value
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(v) => Some(*v),
            _ => None,
        }
    }

    /// Try to extract string value
    pub fn as_string(&self) -> Option<&str> {
        match self {
            Value::String(v) => Some(v.as_str()),
            _ => None,
        }
    }
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Int(v) => write!(f, "{}", v),
            Value::Double(v) => write!(f, "{}", v),
            Value::Bool(v) => write!(f, "{}", v),
            Value::String(v) => write!(f, "{}", v),
            Value::Void => write!(f, "void"),
        }
    }
}

// Conversions from Rust types
impl From<i32> for Value {
    fn from(v: i32) -> Self {
        Value::Int(v)
    }
}

impl From<f64> for Value {
    fn from(v: f64) -> Self {
        Value::Double(v)
    }
}

impl From<bool> for Value {
    fn from(v: bool) -> Self {
        Value::Bool(v)
    }
}

impl From<String> for Value {
    fn from(v: String) -> Self {
        Value::String(v)
    }
}

impl From<&str> for Value {
    fn from(v: &str) -> Self {
        Value::String(v.to_string())
    }
}

impl From<()> for Value {
    fn from(_: ()) -> Self {
        Value::Void
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_int_roundtrip() {
        let original = Value::Int(42);
        let ffi_ptr = original.to_ffi();
        let recovered = Value::from_ffi(ffi_ptr);
        assert_eq!(original, recovered);
    }

    #[test]
    fn test_string_roundtrip() {
        let original = Value::String("Hello, Rust!".to_string());
        let ffi_ptr = original.to_ffi();
        let recovered = Value::from_ffi(ffi_ptr);
        assert_eq!(original, recovered);
    }

    #[test]
    fn test_bool_roundtrip() {
        let original = Value::Bool(true);
        let ffi_ptr = original.to_ffi();
        let recovered = Value::from_ffi(ffi_ptr);
        assert_eq!(original, recovered);
    }

    #[test]
    fn test_void_roundtrip() {
        let original = Value::Void;
        let ffi_ptr = original.to_ffi();
        let recovered = Value::from_ffi(ffi_ptr);
        assert_eq!(original, recovered);
    }

    #[test]
    fn test_from_rust_types() {
        assert_eq!(Value::from(42), Value::Int(42));
        assert_eq!(Value::from(3.14), Value::Double(3.14));
        assert_eq!(Value::from(true), Value::Bool(true));
        assert_eq!(Value::from("test"), Value::String("test".to_string()));
    }

    #[test]
    fn test_as_methods() {
        let val = Value::Int(123);
        assert_eq!(val.as_int(), Some(123));
        assert_eq!(val.as_double(), None);

        let val = Value::String("test".to_string());
        assert_eq!(val.as_string(), Some("test"));
        assert_eq!(val.as_int(), None);
    }
}
