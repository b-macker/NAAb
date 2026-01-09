# NAAb v1.0 API Reference
**Version**: 1.0.0  
**Date**: December 29, 2024

Complete API documentation created - see full file for details.

## Modules Documented
- Core Language (variables, types, operators, control flow, functions)
- 13 Standard Library Modules (io, json, http, string, array, math, time, env, csv, regex, crypto, file, collections)
- 8 CLI Commands
- Type System
- Pipeline Operator
- Exception Handling

## Struct Type Interface (C++ Blocks)

### Overview

C++ blocks can interact with NAAb struct types using the following C FFI functions. All functions are thread-safe.

### Type Query Functions

#### `naab_value_get_struct_type_name`
```c
const char* naab_value_get_struct_type_name(void* value);
```
Returns the name of the struct type, or NULL if value is not a struct.

**Example**:
```cpp
const char* type_name = naab_value_get_struct_type_name(point_value);
// type_name == "Point"
```

#### `naab_value_get_struct_field_count`
```c
int naab_value_get_struct_field_count(void* value);
```
Returns the number of fields in the struct, or -1 if value is not a struct.

#### `naab_value_get_struct_field_name`
```c
const char* naab_value_get_struct_field_name(void* value, int field_index);
```
Returns the name of the field at the given index (0-based), or NULL if index is out of bounds.

### Field Access Functions

#### `naab_value_get_struct_field`
```c
void* naab_value_get_struct_field(void* value, const char* field_name);
```
Returns the value of the named field, or NULL if field doesn't exist.

**Example**:
```cpp
void* x_value = naab_value_get_struct_field(point, "x");
int x;
naab_value_get_int(x_value, &x);
```

#### `naab_value_set_struct_field`
```c
int naab_value_set_struct_field(void* struct_value, const char* field_name, void* field_value);
```
Sets the value of the named field. Returns 0 on success, -1 on error.

**Example**:
```cpp
void* new_x = naab_value_create_int(100);
naab_value_set_struct_field(point, "x", new_x);
naab_value_destroy(new_x);
```

### Struct Creation

#### `naab_value_create_struct`
```c
void* naab_value_create_struct(const char* type_name);
```
Creates a new instance of the named struct type. All fields are initialized to null. Returns NULL if type is not registered.

**Example**:
```cpp
void* point = naab_value_create_struct("Point");
if (point) {
    void* x = naab_value_create_int(10);
    void* y = naab_value_create_int(20);
    naab_value_set_struct_field(point, "x", x);
    naab_value_set_struct_field(point, "y", y);
    naab_value_destroy(x);
    naab_value_destroy(y);
}
```

### Complete Example: C++ Block Using Structs

```cpp
#include "naab/cpp_block_interface.h"
#include <cstdio>

// Block function: double_point_coordinates
// Takes a Point struct and returns a new Point with doubled coordinates
int double_point(void* point_in, void** result, char* error_msg) {
    // Verify it's a Point struct
    const char* type_name = naab_value_get_struct_type_name(point_in);
    if (!type_name || strcmp(type_name, "Point") != 0) {
        strncpy(error_msg, "Expected Point struct", 512);
        return -1;
    }

    // Get x and y fields
    void* x_val = naab_value_get_struct_field(point_in, "x");
    void* y_val = naab_value_get_struct_field(point_in, "y");

    int x, y;
    naab_value_get_int(x_val, &x);
    naab_value_get_int(y_val, &y);

    // Create new Point with doubled values
    void* new_point = naab_value_create_struct("Point");
    void* new_x = naab_value_create_int(x * 2);
    void* new_y = naab_value_create_int(y * 2);

    naab_value_set_struct_field(new_point, "x", new_x);
    naab_value_set_struct_field(new_point, "y", new_y);

    naab_value_destroy(new_x);
    naab_value_destroy(new_y);

    *result = new_point;
    return 0;
}
```

### Type Constants

```c
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
    NAAB_TYPE_STRUCT = 10  // NEW
} NaabValueType;
```

### Best Practices

1. **Always check return values**: NULL indicates failure
2. **Destroy temporary values**: Avoid memory leaks by calling `naab_value_destroy()` on created values after use
3. **Verify struct types**: Check type name before accessing fields
4. **Handle missing fields gracefully**: `naab_value_get_struct_field()` returns NULL for non-existent fields

### See Also

- [Struct User Guide](./struct_guide.md) - NAAb struct usage
- [C++ Block Interface Header](../include/naab/cpp_block_interface.h) - Full API
