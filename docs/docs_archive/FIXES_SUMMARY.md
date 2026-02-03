# NAAb Core Language Fixes - Summary

**Date:** 2026-01-26
**Session Focus:** Fix struct serialization and nested generic type parsing

## Issues Fixed

### 1. ✅ json.stringify() Struct Serialization Support

**Problem:** Structs were serializing as `"<unsupported>"` instead of JSON objects.

**Root Cause:** The `valueToJson()` function in `json_impl.cpp` didn't handle the `std::shared_ptr<interpreter::StructValue>` variant type.

**Solution:** Added struct serialization support to recursively convert struct fields to JSON objects.

**Files Modified:**
- `src/stdlib/json_impl.cpp` (lines 103-119)

**Implementation:**
```cpp
else if constexpr (std::is_same_v<T, std::shared_ptr<interpreter::StructValue>>) {
    // Struct serialization - convert to JSON object
    json obj = json::object();
    if (arg && arg->definition) {
        const auto& fields = arg->definition->fields;
        const auto& values = arg->field_values;

        for (size_t i = 0; i < fields.size() && i < values.size(); ++i) {
            const std::string& field_name = fields[i].name;
            if (values[i]) {
                obj[field_name] = valueToJson(*values[i]);
            } else {
                obj[field_name] = nullptr;
            }
        }
    }
    return obj;
}
```

**Test Results:**
```naab
struct Person { name: string, age: int, active: bool }
let person = new Person { name: "Alice", age: 30, active: true }
let json_str = json.stringify(person)
// Result: {"active":true,"age":30,"name":"Alice"}
```

**Impact:**
- ✅ Structs can now be serialized to JSON
- ✅ Lists of structs work correctly
- ✅ Nested structs are recursively serialized
- ✅ ATLAS pipeline Stage 3 (data_transformer) now works without manual dict conversion

---

### 2. ✅ Nested Generic Type Parsing (e.g., `list<dict<string, string>>`)

**Problem:** Parser failed with `Expected '>' after dict value type` when parsing nested generics like `list<dict<string, string>>`.

**Root Cause:** The lexer treats `>>` as a single `GT_GT` token (for right-shift operator and inline code blocks). When parsing `dict<string, string>>`, the parser sees `dict<string, string` followed by `GT_GT` instead of two separate `>` tokens.

**Solution:** Implemented token splitting in the parser using a "pending token" mechanism:
1. When expecting `>` but encountering `>>`, split it into two `>` tokens
2. Use the first `>` immediately
3. Store the second `>` as a pending token for the next `current()` call

**Files Modified:**
- `include/naab/parser.h` (added `pending_token_` and `stored_gt_token_` fields, added `expectGTOrSplitGTGT()` declaration)
- `src/parser/parser.cpp`:
  - Added `expectGTOrSplitGTGT()` helper function (lines 133-162)
  - Modified `current()` to return pending token first (lines 72-75)
  - Modified `advance()` to clear pending token (lines 95-98)
  - Updated type parsing to use `expectGTOrSplitGTGT()` instead of `expect(GT, ...)` (lines 1580, 1602, 1646)

**Implementation Details:**
```cpp
// In parser.h:
mutable std::optional<lexer::Token> pending_token_;
mutable lexer::Token stored_gt_token_;

// In parser.cpp:
const lexer::Token& Parser::expectGTOrSplitGTGT(const std::string& msg) {
    if (check(lexer::TokenType::GT)) {
        const auto& token = current();
        advance();
        return token;
    }

    if (check(lexer::TokenType::GT_GT)) {
        const auto& token = current();
        pos_++;  // Move past GT_GT

        // Create two separate > tokens
        stored_gt_token_ = lexer::Token(lexer::TokenType::GT, ">", token.line, token.column);
        pending_token_ = lexer::Token(lexer::TokenType::GT, ">", token.line, token.column + 1);

        return stored_gt_token_;  // Return first >
    }

    throw ParseError(...);
}
```

**Test Results:**
```naab
let data: list<dict<string, string>> = []
let dict1 = {"name": "Alice", "role": "Developer"}
data = array.push(data, dict1)
// ✅ Parses and executes successfully
```

**Impact:**
- ✅ Nested generic types now parse correctly
- ✅ Works for any nesting depth: `list<list<dict<string, int>>>`
- ✅ No changes needed to lexer (maintains `>>` for inline code and shift operations)
- ✅ Parser elegantly handles the ambiguity

---

## Testing

### Test Files Created:
1. `test_struct_serialization.naab` - Validates struct to JSON conversion
2. `test_nested_generics.naab` - Validates nested generic type parsing

### Integration Test:
**ATLAS Data Harvesting Pipeline** - Real-world multi-module project

**Results:**
```
✅ Stage 1: Configuration Loading - PASSED
✅ Stage 2: Data Harvesting (Static Scraping) - PASSED
✅ Stage 3: Data Processing & Validation - PASSED (now uses direct struct serialization!)
❌ Stage 4: Analytics - BLOCKED (requires scikit-learn, not available on Termux)
```

---

## Additional Fixes from Session

### 3. Enhanced Debug Hints (Completed)
All 5 debug hint categories implemented:
- ✅ Postfix `?` operator detection
- ✅ Reserved keyword validation
- ✅ `array.new()` pattern detection
- ✅ Stdlib module detection in undefined variables
- ✅ Python block return validation

### 4. Bug Fixes in ATLAS Pipeline
- ✅ Fixed `array.push()` usage (must assign return value)
- ✅ Fixed Python variable binding (direct names, not `NAAB_VAR_` prefix)
- ✅ Fixed web_scraper item_container override logic
- ✅ Fixed `array.push()` in all custom modules

---

## Build Status

**Compiler:** ✅ Built successfully with all changes
**Warnings:** 1 unused variable (non-critical)
**Tests:** All passing

---

## Documentation Impact

These fixes eliminate two common workarounds that were needed:

**Before:**
```naab
// Workaround 1: Manual dict conversion for structs
let json_items = []
for item in raw_items {
    let item_dict = {
        "name": item.name,
        "description": item.description
    }
    json_items = array.push(json_items, item_dict)
}
let json_str = json.stringify(json_items)

// Workaround 2: Avoid nested generics
let data: list<dict<string, string>>  // ❌ Parse error
let data = []  // Use untyped list instead
```

**After:**
```naab
// Direct struct serialization
let json_str = json.stringify(raw_items)  // ✅ Works!

// Nested generic types
let data: list<dict<string, string>> = []  // ✅ Parses correctly!
```

---

## Next Steps

1. Update language documentation to reflect struct serialization support
2. Add nested generic examples to type system documentation
3. Consider adding `json.parse()` support for deserializing into structs
4. Update ATLAS pipeline to remove remaining workarounds
5. Consider addressing Stage 4 analytics with pure-NAAb implementation (no sklearn)

---

## Files Changed Summary

**Core Language:**
- `src/stdlib/json_impl.cpp` - Added struct serialization
- `include/naab/parser.h` - Added token splitting support
- `src/parser/parser.cpp` - Implemented nested generic parsing

**ATLAS Pipeline:**
- `data_transformer.naab` - Simplified to use direct struct serialization
- `web_scraper.naab` - Fixed array.push usage
- `engine_config.json` - Updated for static scraping

**Test Files:**
- `test_struct_serialization.naab` - New
- `test_nested_generics.naab` - New
