//! Raw FFI bindings to libnaab-governance C API.

use std::os::raw::{c_char, c_int};

/// Opaque governance engine handle.
pub type NaabGovEngine = *mut std::ffi::c_void;

extern "C" {
    pub fn naab_gov_create() -> NaabGovEngine;
    pub fn naab_gov_destroy(engine: NaabGovEngine);
    pub fn naab_gov_load_config(engine: NaabGovEngine, path: *const c_char) -> c_int;
    pub fn naab_gov_discover_config(engine: NaabGovEngine, dir: *const c_char) -> c_int;
    pub fn naab_gov_load_config_string(engine: NaabGovEngine, json: *const c_char) -> c_int;
    pub fn naab_gov_is_active(engine: NaabGovEngine) -> c_int;
    pub fn naab_gov_scan(
        engine: NaabGovEngine,
        language: *const c_char,
        code: *const c_char,
        source_file: *const c_char,
        start_line: c_int,
    ) -> *mut c_char;
    pub fn naab_gov_check(
        engine: NaabGovEngine,
        check_name: *const c_char,
        language: *const c_char,
        code: *const c_char,
        start_line: c_int,
    ) -> *mut c_char;
    pub fn naab_gov_was_blocked(engine: NaabGovEngine) -> c_int;
    pub fn naab_gov_json_report(engine: NaabGovEngine) -> *mut c_char;
    pub fn naab_gov_sarif_report(engine: NaabGovEngine) -> *mut c_char;
    pub fn naab_gov_summary(engine: NaabGovEngine) -> *mut c_char;
    pub fn naab_gov_result_count(engine: NaabGovEngine) -> c_int;
    pub fn naab_gov_reset(engine: NaabGovEngine);
    pub fn naab_gov_last_error(engine: NaabGovEngine) -> *const c_char;
    pub fn naab_gov_free_string(s: *mut c_char);
    pub fn naab_gov_version_string() -> *const c_char;
}
