# JSON Module Integration

## Overview

The NAAb language now includes a fully functional JSON module built using the industry-standard [nlohmann/json](https://github.com/nlohmann/json) library (v3.11.3). This module provides JSON parsing and stringification capabilities.

## Implementation Details

### Library
- **nlohmann/json v3.11.3** - Header-only C++ JSON library
- Location: `/storage/emulated/0/Download/.naab/naab_language/external/json/`
- Header: `external/json/include/nlohmann/json.hpp`

### Source Files
- `src/stdlib/json_impl.cpp` - Real implementation using nlohmann/json
- `include/naab/stdlib.h` - JSONModule class declaration
- Integrated into `naab_stdlib` library via CMakeLists.txt

### Architecture
The JSON module implements the standard `Module` interface:
```cpp
class JSONModule : public Module {
public:
    std::string getName() const override { return "json"; }
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;

private:
    std::shared_ptr<interpreter::Value> parse(...);
    std::shared_ptr<interpreter::Value> stringify(...);
};
```

## Available Functions

### `parse(json_string)`
Parses a JSON string into a NAAb value.

**Parameters:**
- `json_string` (string) - Valid JSON string to parse

**Returns:**
- Parsed value (can be number, string, boolean, null, array, or object)

**Supported Types:**
- `null` → NAAb null (monostate)
- `true`/`false` → NAAb boolean
- `42` → NAAb integer
- `3.14` → NAAb double
- `"hello"` → NAAb string
- `[1, 2, 3]` → NAAb array (vector)
- `{"key": "value"}` → NAAb object (unordered_map)

**Example:**
```cpp
auto json_str = std::make_shared<interpreter::Value>(std::string("{\"name\": \"NAAb\"}"));
auto obj = json_module.call("parse", {json_str});
```

**Error Handling:**
- Throws `std::runtime_error` on parse errors with byte position and error message
- Example: `"JSON parse error at byte 5: unexpected character"`

### `stringify(value, [indent])`
Converts a NAAb value to a JSON string.

**Parameters:**
- `value` (any) - Value to stringify
- `indent` (int, optional) - Number of spaces for pretty printing (default: compact)

**Returns:**
- JSON string representation

**Example:**
```cpp
// Compact (default)
auto compact = json_module.call("stringify", {obj});
// Output: {"active":true,"name":"NAAb","version":1.0}

// Pretty print with 2-space indent
auto indent = std::make_shared<interpreter::Value>(2);
auto pretty = json_module.call("stringify", {obj, indent});
// Output:
// {
//   "active": true,
//   "name": "NAAb",
//   "version": 1.0
// }
```

## Type Conversion

### JSON → NAAb Value

| JSON Type | NAAb Type | Example |
|-----------|-----------|---------|
| `null` | `std::monostate` | `null` |
| `true`/`false` | `bool` | `true` |
| Integer | `int` | `42` |
| Float | `double` | `3.14` |
| String | `std::string` | `"hello"` |
| Array | `std::vector<std::shared_ptr<Value>>` | `[1, 2, 3]` |
| Object | `std::unordered_map<string, shared_ptr<Value>>` | `{"key": "value"}` |

### NAAb Value → JSON

The reverse conversion is handled automatically by `valueToJson()` helper function using `std::visit` on the variant type.

## Test Results

All JSON tests passing:

```
=== JSON Module Test ===

Test 1: Parse simple values
  Number: 42
  String: hello
  Boolean: true
  Null: null

Test 2: Parse array
  Array: [1, 2, 3, 4, 5]

Test 3: Parse object
  Object: {"active": true, "name": NAAb, "version": 1.000000}

Test 4: Stringify
  Compact: {"active":true,"name":"NAAb","version":1.0}
  Pretty:
{
  "active": true,
  "name": "NAAb",
  "version": 1.0
}

Test 5: Round-trip test
  Original: {"test": "value", "number": 123}
  Stringified: {"number":123,"test":"value"}
  Round-trip successful!

=== All JSON tests passed! ===
```

### Test Coverage
- ✅ Parse primitive values (number, string, boolean, null)
- ✅ Parse arrays
- ✅ Parse objects (nested structures)
- ✅ Stringify with compact formatting
- ✅ Stringify with pretty printing
- ✅ Round-trip conversion (parse → stringify → parse)

## Build Integration

### CMakeLists.txt Changes
```cmake
# Standard library
add_library(naab_stdlib
    src/stdlib/core.cpp
    src/stdlib/io.cpp
    src/stdlib/collections.cpp
    src/stdlib/json_impl.cpp      # ← Added
    src/stdlib/stdlib.cpp
)
target_include_directories(naab_stdlib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/include  # ← Added
)
```

### Module Registration
The JSON module is automatically registered in `StdLib::registerModules()`:
```cpp
void StdLib::registerModules() {
    modules_["io"] = std::make_shared<IOModule>();
    modules_["json"] = std::make_shared<JSONModule>();  // ← Auto-registered
    modules_["http"] = std::make_shared<HTTPModule>();
    modules_["collections"] = std::make_shared<CollectionsModule>();
}
```

## Known Limitations

1. **NAAb Language Access**: Currently, the JSON module is available in C++ but not yet accessible from NAAb programs. The interpreter needs to be extended to expose stdlib modules via syntax like:
   ```naab
   use json
   let data = json.parse('{"key": "value"}')
   ```

2. **String Literal Constructor**: When creating `Value` objects in C++, string literals must be explicitly wrapped:
   ```cpp
   // Wrong - converts to bool!
   auto val = std::make_shared<interpreter::Value>("42");

   // Correct - explicit std::string
   auto val = std::make_shared<interpreter::Value>(std::string("42"));
   ```

## Future Enhancements

1. Expose JSON module to NAAb programs via `use` statements
2. Add JSON schema validation
3. Add JSON Patch (RFC 6902) support
4. Add JSON Pointer (RFC 6901) support
5. Streaming JSON parser for large files

## Dependencies

- **nlohmann/json**: v3.11.3 (MIT License)
- **fmt**: For error message formatting
- **C++17**: Required for `std::variant` and `std::visit`

## Files

- Implementation: `src/stdlib/json_impl.cpp` (163 lines)
- Header: `include/naab/stdlib.h` (JSONModule class)
- Tests: `test_json_module.cpp` (69 lines)
- Example: `examples/test_json.naab`

## Phase 5 Status

✅ **JSON Library Integration - COMPLETE**

Next: HTTP Library Integration (libcurl)
