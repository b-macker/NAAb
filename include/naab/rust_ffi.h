#ifndef NAAB_RUST_FFI_H
#define NAAB_RUST_FFI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>  // Phase 4.2.4: For uint32_t

#ifdef __cplusplus
extern "C" {
#endif

// Value type enum for Rust interop
typedef enum {
    NAAB_RUST_TYPE_VOID = 0,
    NAAB_RUST_TYPE_INT = 1,
    NAAB_RUST_TYPE_DOUBLE = 2,
    NAAB_RUST_TYPE_BOOL = 3,
    NAAB_RUST_TYPE_STRING = 4
} NaabRustValueType;

// Opaque value handle
typedef struct NaabRustValue NaabRustValue;

// Block function signature
typedef NaabRustValue* (*NaabRustBlockFn)(NaabRustValue** args, size_t arg_count);

// Phase 4.2.4: Error metadata for stack tracing
typedef struct {
    char* message;
    char* file;
    uint32_t line;
} NaabRustError;

// Value creation functions
NaabRustValue* naab_rust_value_create_int(int value);
NaabRustValue* naab_rust_value_create_double(double value);
NaabRustValue* naab_rust_value_create_bool(bool value);
NaabRustValue* naab_rust_value_create_string(const char* value);
NaabRustValue* naab_rust_value_create_void();

// Value access functions
int naab_rust_value_get_int(const NaabRustValue* value);
double naab_rust_value_get_double(const NaabRustValue* value);
bool naab_rust_value_get_bool(const NaabRustValue* value);
const char* naab_rust_value_get_string(const NaabRustValue* value);
NaabRustValueType naab_rust_value_get_type(const NaabRustValue* value);

// Memory management
void naab_rust_value_free(NaabRustValue* value);

// Phase 4.2.4: Error handling
NaabRustError* naab_rust_get_last_error();
void naab_rust_error_free(NaabRustError* error);

#ifdef __cplusplus
}
#endif

#endif // NAAB_RUST_FFI_H
