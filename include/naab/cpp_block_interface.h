#ifndef NAAB_CPP_BLOCK_INTERFACE_H
#define NAAB_CPP_BLOCK_INTERFACE_H

// NAAb C++ Block Interface Standard v1.0
// Defines the contract between NAAb interpreter and dynamically loaded C++ blocks

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Block Metadata Functions (Required)
// ============================================================================

/**
 * Returns the block ID (e.g., "BLOCK-JSON")
 */
const char* naab_block_id();

/**
 * Returns the block version (e.g., "1.0.0")
 */
const char* naab_block_version();

/**
 * Returns comma-separated list of exported functions
 * Example: "parse,stringify"
 */
const char* naab_block_functions();

// ============================================================================
// Block Lifecycle Functions (Required)
// ============================================================================

/**
 * Initialize block when loaded
 * Returns 0 on success, non-zero on error
 */
int naab_block_init();

/**
 * Cleanup block when unloaded
 */
void naab_block_cleanup();

// ============================================================================
// Function Calling Convention
// ============================================================================

/**
 * Generic function call interface
 *
 * @param func_name - Name of function to call
 * @param argc - Number of arguments
 * @param argv - Array of argument pointers (void* to Value structs)
 * @param result - Pointer to store result (void* to Value struct)
 * @param error_msg - Buffer to store error message (512 bytes)
 *
 * @return 0 on success, non-zero on error
 */
int naab_block_call(
    const char* func_name,
    int argc,
    void** argv,
    void** result,
    char* error_msg
);

// ============================================================================
// Value Type Interface
// ============================================================================

// Value type tags matching naab::interpreter::ValueData variant indices
typedef enum {
    NAAB_TYPE_NULL = 0,
    NAAB_TYPE_INT = 1,
    NAAB_TYPE_DOUBLE = 2,
    NAAB_TYPE_BOOL = 3,
    NAAB_TYPE_STRING = 4,
    NAAB_TYPE_ARRAY = 5,
    NAAB_TYPE_DICT = 6,
    NAAB_TYPE_BLOCK = 7,
    NAAB_TYPE_FUNCTION = 8,
    NAAB_TYPE_PYOBJECT = 9,
    NAAB_TYPE_STRUCT = 10
} NaabValueType;

/**
 * Get type of a Value
 */
int naab_value_type(void* value);

/**
 * Extract primitive values
 */
int naab_value_get_int(void* value, int* out);
int naab_value_get_double(void* value, double* out);
int naab_value_get_bool(void* value, int* out);
int naab_value_get_string(void* value, const char** out);

/**
 * Create new Values (caller must manage lifetime)
 */
void* naab_value_create_null();
void* naab_value_create_int(int val);
void* naab_value_create_double(double val);
void* naab_value_create_bool(int val);
void* naab_value_create_string(const char* val);

/**
 * Destroy a Value (free memory)
 */
void naab_value_destroy(void* value);

// ============================================================================
// Struct Type Interface (Week 6 - C++ block support)
// ============================================================================

/**
 * Get struct type name
 * Returns NULL if value is not a struct
 */
const char* naab_value_get_struct_type_name(void* value);

/**
 * Get number of fields in struct
 * Returns -1 if value is not a struct
 */
int naab_value_get_struct_field_count(void* value);

/**
 * Get name of field at given index (0-based)
 * Returns NULL if index out of bounds or value is not a struct
 */
const char* naab_value_get_struct_field_name(void* value, int field_index);

/**
 * Get value of named field
 * Returns NULL if field doesn't exist or value is not a struct
 */
void* naab_value_get_struct_field(void* value, const char* field_name);

/**
 * Set value of named field
 * Returns 0 on success, non-zero on error
 */
int naab_value_set_struct_field(void* struct_value, const char* field_name, void* field_value);

/**
 * Create new struct instance
 * Returns NULL if type_name is not a registered struct type
 */
void* naab_value_create_struct(const char* type_name);

#ifdef __cplusplus
}
#endif

#endif // NAAB_CPP_BLOCK_INTERFACE_H
