# Chapter 6: Language-Specific Integrations

While the `<<lang` syntax provides a unified interface for polyglot programming, each supported language has its own unique strengths, use cases, and specific behaviors within the NAAb environment. This chapter dives into the details of integrating the core supported languages.

## 6.1 Python: Data Science and Scripting

Python integration is designed to leverage its massive ecosystem for data analysis, machine learning, and general-purpose scripting.

### 6.1.1 Execution Model

NAAb's Python executor seamlessly handles both simple expressions and complex multi-line scripts:

*   **Expressions**: Single-line calculations are evaluated and returned.
*   **Multi-line Code**: You can write full Python scripts with imports, loops, classes, and functions. NAAb automatically captures the value of the **last expression** in the block and returns it.

```naab
// Expression (returns value)
let sum = <<python 10 + 20 >>

// Multi-line (returns value of last expression)
let pi_approx = <<python
import math
# Perform some calculation
radius = 5.0
area = math.pi * (radius ** 2)
area # This value is returned to NAAb
>>
```

### 6.1.2 Data Science Example

One of the best uses for Python blocks is to perform statistical analysis on data collected by NAAb.

```naab
main {
    let data_points = [12, 45, 67, 23, 89, 34, 56]

    <<python[data_points]
    import statistics
    
    mean = statistics.mean(data_points)
    median = statistics.median(data_points)
    stdev = statistics.stdev(data_points)
    
    print(f"Data Analysis:")
    print(f"  Mean:   {mean:.2f}")
    print(f"  Median: {median:.2f}")
    print(f"  StDev:  {stdev:.2f}")
    >>
}
```

## 6.2 JavaScript: Text Processing and JSON

JavaScript (powered by the QuickJS engine) is excellent for text manipulation, JSON handling, and logic that might already exist in web codebases.

### 6.2.1 Multi-line Support

NAAb supports multi-line JavaScript code, including variable declarations (`const`, `let`), control flow, and complex object literals. The value of the last expression is returned.

```naab
let user_info = <<javascript
const name = "Alice";
const role = "admin";
({
    user: name,
    permissions: [role, "editor"],
    active: true
}); // This object is returned
>>
```

### 6.2.2 Scope Isolation

Each `<<javascript>>` block runs in its own isolated scope (wrapped in an Immediately Invoked Function Expression or IIFE). This prevents variable collisions between blocks. You can safely use `const` and `let` without worrying about redeclaration errors in subsequent blocks.

```naab
<<javascript
const x = 10;
console.log("Block 1:", x);
>>

<<javascript
// This 'x' is distinct from the previous block
const x = 20; 
console.log("Block 2:", x);
>>
```

### 6.2.2 Working with JSON

JavaScript's native JSON handling makes it a great choice for parsing or transforming complex data structures passed from NAAb.

```naab
main {
    let raw_data = {"id": 1, "user": "alice", "roles": ["admin", "editor"]}

    <<javascript[raw_data]
    // raw_data is automatically converted to a JS object
    if (raw_data.roles.includes("admin")) {
        console.log(`User ${raw_data.user} has admin access.`);
    }
    
    // Add a timestamp
    raw_data.timestamp = Date.now();
    console.log("Enriched data:", JSON.stringify(raw_data));
    >>
}
```
**Note:** NAAb automatically captures the value of the last expression in the block.

## 6.3 C++: High-Performance Inline Code

For performance-critical sections, you can drop down to C++. NAAb compiles these blocks on the fly (and caches them) into shared libraries.

### 6.3.1 Multi-line Support

You can write multi-line C++ code with variable declarations. The last line is treated as the return value if it is an expression.

```naab
let result = <<cpp
int x = 10;
int y = 20;
x * y // Returns 200
>>
```

### 6.3.2 Automatic Header Injection

NAAb automatically injects common standard library headers into your C++ blocks, so you don't need to manually include them for basic operations. Included headers are: `<iostream>`, `<string>`, `<vector>`, `<map>`, `<cmath>`, and `<algorithm>`.

```naab
<<cpp
// <iostream>, <vector>, <algorithm> are auto-included!

std::vector<int> numbers = {5, 1, 4, 2, 3};
std::sort(numbers.begin(), numbers.end());

std::cout << "Sorted in C++: ";
for (int n : numbers) {
    std::cout << n << " ";
}
std::cout << std::endl;
>>
```

### 6.3.2 Use Cases

Use C++ blocks for:
*   Heavy mathematical computations.
*   Custom algorithms where interpreter overhead is unacceptable.
*   Interfacing with C/C++ libraries.

## 6.4 Bash: System Operations

Bash blocks execute commands directly in the system shell. This is ideal for file system manipulation, process management, and glue code.

```naab
main {
    let dirname = "backup_dir"

    <<bash[dirname]
    echo "Creating backup directory: $dirname"
    mkdir -p "$dirname"
    touch "$dirname/timestamp.txt"
    date >> "$dirname/timestamp.txt"
    ls -l "$dirname"
    >>
}
```
Variables passed to Bash are available as standard environment variables (e.g., `$dirname`).

## 6.5 Other Languages

NAAb also supports:
*   **Rust**: For safe, concurrent systems programming.
*   **Go**: For fast, concurrent network services.
*   **Ruby**: For expressive scripting and text processing.
*   **C#**: For .NET integration.

These languages now fully support multi-line code blocks. For compiled languages (Rust, Go, C#), NAAb automatically wraps your code in the necessary `main` function or class structure and prints the result of the last expression.

```naab
let rust_sum = <<rust
let x = 50;
let y = 30;
x + y // Returns 80
>>
```

## 6.6 Data Marshaling and Type Correspondence

When you pass data between NAAb and a foreign language, NAAb performs "marshaling" (serialization) and "unmarshaling" (deserialization). Here is how types generally map:

| NAAb Type | Python | JavaScript | C++ | Bash |
| :--- | :--- | :--- | :--- | :--- |
| `int` | `int` | `Number` | `int`/`long` | String |
| `float` | `float` | `Number` | `double` | String |
| `string` | `str` | `String` | `std::string` | String |
| `bool` | `bool` | `Boolean` | `bool` | String ("true"/"false") |
| `array` | `list` | `Array` | `std::vector` (if homogeneous) | JSON String |
| `dict` | `dict` | `Object` | `std::map` (or JSON) | JSON String |
| `struct` | `dict` | `Object` | JSON String | JSON String |

**Handling Complex Structures:**
For complex nested structures (like a list of dicts), NAAb typically serializes the data to a JSON string when passing it to languages like Bash or C++, requiring you to parse it on the other side if you need deep access. Dynamic languages like Python and JavaScript receive native objects (lists/dicts) directly.

## 6.7 C FFI: Struct Interface for C++ Blocks

For advanced C++ block integration, NAAb provides a C FFI (Foreign Function Interface) that allows C++ blocks to inspect and manipulate NAAb struct values directly. All FFI functions are thread-safe.

### 6.7.1 Type Constants

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
    NAAB_TYPE_STRUCT = 10
} NaabValueType;
```

### 6.7.2 Struct Query and Access

```c
// Get the struct's type name (returns NULL if not a struct)
const char* naab_value_get_struct_type_name(void* value);

// Get field count (-1 if not a struct)
int naab_value_get_struct_field_count(void* value);

// Get field name by index (NULL if out of bounds)
const char* naab_value_get_struct_field_name(void* value, int field_index);

// Get a field value by name (NULL if field doesn't exist)
void* naab_value_get_struct_field(void* value, const char* field_name);

// Set a field value (returns 0 on success, -1 on error)
int naab_value_set_struct_field(void* struct_value, const char* field_name, void* field_value);

// Create a new struct instance (NULL if type not registered)
void* naab_value_create_struct(const char* type_name);
```

### 6.7.3 Example: C++ Block Using Structs

```cpp
#include "naab/cpp_block_interface.h"

// Takes a Point struct and returns a new Point with doubled coordinates
int double_point(void* point_in, void** result, char* error_msg) {
    const char* type_name = naab_value_get_struct_type_name(point_in);
    if (!type_name || strcmp(type_name, "Point") != 0) {
        strncpy(error_msg, "Expected Point struct", 512);
        return -1;
    }

    void* x_val = naab_value_get_struct_field(point_in, "x");
    void* y_val = naab_value_get_struct_field(point_in, "y");

    int x, y;
    naab_value_get_int(x_val, &x);
    naab_value_get_int(y_val, &y);

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

### 6.7.4 Best Practices

1.  **Always check return values** — NULL indicates failure
2.  **Destroy temporary values** — Call `naab_value_destroy()` on created values after use to avoid memory leaks
3.  **Verify struct types** — Check the type name before accessing fields
4.  **Handle missing fields gracefully** — `naab_value_get_struct_field()` returns NULL for non-existent fields
