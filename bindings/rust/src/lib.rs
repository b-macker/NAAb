//! Rust bindings for the NAAb governance engine.
//!
//! Provides safe wrappers around the libnaab-governance C API for static
//! analysis of code to detect security violations and policy breaches.
//!
//! # Example
//! ```no_run
//! use naab_governance::GovernanceEngine;
//!
//! let engine = GovernanceEngine::new().unwrap();
//! engine.load_config_string(r#"{"version":"3.0","mode":"enforce",...}"#).unwrap();
//! let result = engine.scan("python", "x = 42", "test.py", 1).unwrap();
//! println!("Blocked: {}", result.blocked);
//! ```

mod ffi;

use std::ffi::{CStr, CString};
use std::fmt;

/// Error type for governance operations.
#[derive(Debug)]
pub struct Error {
    message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "naab-governance: {}", self.message)
    }
}

impl std::error::Error for Error {}

/// Result of a governance scan (parsed from JSON).
#[derive(Debug)]
pub struct ScanResult {
    pub blocked: bool,
    pub raw_json: String,
}

/// A NAAb governance engine instance.
///
/// Each engine is independent and thread-safe. Create one per thread
/// or per request for isolation.
pub struct GovernanceEngine {
    handle: ffi::NaabGovEngine,
}

// Each engine is independent — safe to send between threads.
unsafe impl Send for GovernanceEngine {}

impl GovernanceEngine {
    /// Create a new governance engine.
    pub fn new() -> Result<Self, Error> {
        let handle = unsafe { ffi::naab_gov_create() };
        if handle.is_null() {
            return Err(Error {
                message: "failed to create engine".into(),
            });
        }
        Ok(Self { handle })
    }

    /// Load governance rules from a JSON string.
    pub fn load_config_string(&self, json_config: &str) -> Result<(), Error> {
        let cs = CString::new(json_config).map_err(|_| Error {
            message: "config contains null byte".into(),
        })?;
        let rc = unsafe { ffi::naab_gov_load_config_string(self.handle, cs.as_ptr()) };
        if rc != 0 {
            return Err(Error {
                message: self.last_error().unwrap_or("unknown error".into()),
            });
        }
        Ok(())
    }

    /// Load governance rules from a govern.json file.
    pub fn load_config(&self, path: &str) -> Result<(), Error> {
        let cs = CString::new(path).map_err(|_| Error {
            message: "path contains null byte".into(),
        })?;
        let rc = unsafe { ffi::naab_gov_load_config(self.handle, cs.as_ptr()) };
        if rc != 0 {
            return Err(Error {
                message: self.last_error().unwrap_or("unknown error".into()),
            });
        }
        Ok(())
    }

    /// Returns true if governance rules have been loaded.
    pub fn is_active(&self) -> bool {
        unsafe { ffi::naab_gov_is_active(self.handle) == 1 }
    }

    /// Run all governance checks on the given code.
    pub fn scan(
        &self,
        language: &str,
        code: &str,
        source_file: &str,
        start_line: i32,
    ) -> Result<ScanResult, Error> {
        let c_lang = CString::new(language).unwrap();
        let c_code = CString::new(code).unwrap();
        let c_file = CString::new(source_file).unwrap();

        let raw = unsafe {
            ffi::naab_gov_scan(
                self.handle,
                c_lang.as_ptr(),
                c_code.as_ptr(),
                c_file.as_ptr(),
                start_line,
            )
        };
        if raw.is_null() {
            return Err(Error {
                message: self.last_error().unwrap_or("scan returned null".into()),
            });
        }

        let json_str = unsafe { CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned();
        unsafe { ffi::naab_gov_free_string(raw) };

        let blocked = unsafe { ffi::naab_gov_was_blocked(self.handle) == 1 };

        Ok(ScanResult {
            blocked,
            raw_json: json_str,
        })
    }

    /// Returns true if the last scan had a HARD governance block.
    pub fn was_blocked(&self) -> bool {
        unsafe { ffi::naab_gov_was_blocked(self.handle) == 1 }
    }

    /// Clear check results for the next scan.
    pub fn reset(&self) {
        unsafe { ffi::naab_gov_reset(self.handle) }
    }

    /// Number of check results from the last scan.
    pub fn result_count(&self) -> i32 {
        unsafe { ffi::naab_gov_result_count(self.handle) }
    }

    /// Last error message, if any.
    pub fn last_error(&self) -> Option<String> {
        let p = unsafe { ffi::naab_gov_last_error(self.handle) };
        if p.is_null() {
            return None;
        }
        Some(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
    }
}

impl Drop for GovernanceEngine {
    fn drop(&mut self) {
        unsafe { ffi::naab_gov_destroy(self.handle) }
    }
}

/// Returns the library version string.
pub fn version() -> String {
    unsafe { CStr::from_ptr(ffi::naab_gov_version_string()) }
        .to_string_lossy()
        .into_owned()
}
