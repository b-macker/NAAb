// NAAb Rust FFI Bridge Implementation
// C-compatible interface for Rust block interoperability

#include "naab/rust_ffi.h"
#include "naab/naab_val.h"
#include <cstring>
#include <string>

using namespace naab::interpreter;

// Internal value representation
struct NaabRustValue {
    NaabRustValueType type;
    union {
        int int_val;
        double double_val;
        bool bool_val;
        char* string_val;  // Owned string (must be freed)
    } data;
};

// ============================================================================
// Value Creation Functions (Tasks 3.1.5-3.1.10)
// ============================================================================

NaabRustValue* naab_rust_value_create_int(int value) {
    auto* v = new NaabRustValue();
    v->type = NAAB_RUST_TYPE_INT;
    v->data.int_val = value;
    return v;
}

NaabRustValue* naab_rust_value_create_double(double value) {
    auto* v = new NaabRustValue();
    v->type = NAAB_RUST_TYPE_DOUBLE;
    v->data.double_val = value;
    return v;
}

NaabRustValue* naab_rust_value_create_bool(bool value) {
    auto* v = new NaabRustValue();
    v->type = NAAB_RUST_TYPE_BOOL;
    v->data.bool_val = value;
    return v;
}

NaabRustValue* naab_rust_value_create_string(const char* value) {
    auto* v = new NaabRustValue();
    v->type = NAAB_RUST_TYPE_STRING;
    v->data.string_val = strdup(value);  // Allocate owned copy
    return v;
}

NaabRustValue* naab_rust_value_create_void() {
    auto* v = new NaabRustValue();
    v->type = NAAB_RUST_TYPE_VOID;
    return v;
}

// ============================================================================
// Value Access Functions (Tasks 3.1.11-3.1.16)
// ============================================================================

int naab_rust_value_get_int(const NaabRustValue* value) {
    if (!value || value->type != NAAB_RUST_TYPE_INT) {
        return 0;
    }
    return value->data.int_val;
}

double naab_rust_value_get_double(const NaabRustValue* value) {
    if (!value || value->type != NAAB_RUST_TYPE_DOUBLE) {
        return 0.0;
    }
    return value->data.double_val;
}

bool naab_rust_value_get_bool(const NaabRustValue* value) {
    if (!value || value->type != NAAB_RUST_TYPE_BOOL) {
        return false;
    }
    return value->data.bool_val;
}

const char* naab_rust_value_get_string(const NaabRustValue* value) {
    if (!value || value->type != NAAB_RUST_TYPE_STRING) {
        return "";
    }
    return value->data.string_val;
}

NaabRustValueType naab_rust_value_get_type(const NaabRustValue* value) {
    if (!value) {
        return NAAB_RUST_TYPE_VOID;
    }
    return value->type;
}

// ============================================================================
// Memory Management (Task 3.1.17)
// ============================================================================

void naab_rust_value_free(NaabRustValue* value) {
    if (!value) return;

    // Free owned string if present
    if (value->type == NAAB_RUST_TYPE_STRING && value->data.string_val) {
        free(value->data.string_val);
    }

    delete value;
}

// ============================================================================
// Conversion Helpers (for RustExecutor)
// ============================================================================

namespace naab {
namespace runtime {

// Convert C FFI value to NaabVal
interpreter::NaabVal ffiToValue(NaabRustValue* ffi_val) {
    if (!ffi_val) {
        return interpreter::NaabVal::makeNull();
    }

    auto type = naab_rust_value_get_type(ffi_val);
    switch (type) {
        case NAAB_RUST_TYPE_INT:
            return interpreter::NaabVal::makeInt(naab_rust_value_get_int(ffi_val));
        case NAAB_RUST_TYPE_DOUBLE:
            return interpreter::NaabVal::makeDouble(naab_rust_value_get_double(ffi_val));
        case NAAB_RUST_TYPE_BOOL:
            return interpreter::NaabVal::makeBool(naab_rust_value_get_bool(ffi_val));
        case NAAB_RUST_TYPE_STRING:
            return interpreter::NaabVal::makeString(std::string(naab_rust_value_get_string(ffi_val)));
        default:
            return interpreter::NaabVal::makeNull();
    }
}

// Convert NaabVal to C FFI value
NaabRustValue* valueToFfi(const interpreter::NaabVal& val) {
    if (val.isNull()) {
        return naab_rust_value_create_void();
    }

    if (val.isInt()) {
        return naab_rust_value_create_int(val.asInt());
    }
    if (val.isDouble()) {
        return naab_rust_value_create_double(val.asDouble());
    }
    if (val.isBool()) {
        return naab_rust_value_create_bool(val.asBool());
    }
    if (val.isString()) {
        return naab_rust_value_create_string(val.asString().c_str());
    }
    return naab_rust_value_create_void();
}

} // namespace runtime
} // namespace naab

// ============================================================================
// Error Handling (Phase 4.2.4) - Stub implementations
// ============================================================================
// These will be replaced with actual Rust implementations when integrated

extern "C" {

// Stub: Return null error (no error occurred)
NaabRustError* naab_rust_get_last_error() {
    return nullptr;
}

// Stub: Free error structure
void naab_rust_error_free(NaabRustError* error) {
    if (error) {
        if (error->message) free(error->message);
        if (error->file) free(error->file);
        free(error);
    }
}

} // extern "C"
