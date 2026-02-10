# Bug #3: JavaScript Array/Object Type Conversion Fix

## Problem Summary

JavaScript polyglot blocks could not return arrays or objects as structured data. Arrays and objects either:
1. Returned `null` (for empty arrays/objects)
2. Caused the program to hang indefinitely (for arrays/objects with elements)

## Root Causes

### Root Cause #1: Missing Type Conversion Code
The `fromJSValue()` function only handled primitive types (null, boolean, number, string). When JavaScript returned an array or object, it fell through to the "Unsupported type" case and returned null.

**Location:** `src/runtime/js_executor.cpp` lines 373-410

**Problem:**
```cpp
static std::shared_ptr<interpreter::Value> fromJSValue(JSContext* ctx, JSValue val) {
    // ... handles primitives ...

    // Unsupported type
    fmt::print("[WARN] Unsupported JavaScript type, returning null\n");
    return std::make_shared<interpreter::Value>();
}
```

### Root Cause #2: Problematic Code Wrapping Strategy
For multi-line JavaScript code, the executor wrapped it in an IIFE using eval() with template literals:

```javascript
(function() { return eval(`[42]`); })()
```

This complex wrapping caused QuickJS to hang when evaluating arrays with elements. The combination of:
- IIFE (Immediately Invoked Function Expression)
- eval() call
- Template literal escaping

Created an execution path that QuickJS could not handle correctly for certain array expressions.

**Location:** `src/runtime/js_executor.cpp` lines 269-297

## Solution

### Solution #1: Add Array and Object Support

Added comprehensive array and object conversion in both directions:

#### `fromJSValue()` - JavaScript → NAAb

```cpp
// Array
if (JS_IsArray(ctx, val)) {
    JSValue length_val = JS_GetPropertyStr(ctx, val, "length");
    uint32_t length = 0;
    if (JS_IsNumber(length_val)) {
        JS_ToUint32(ctx, &length, length_val);
    }
    JS_FreeValue(ctx, length_val);

    std::vector<std::shared_ptr<interpreter::Value>> naab_array;
    for (uint32_t i = 0; i < length; i++) {
        JSValue elem = JS_GetPropertyUint32(ctx, val, i);
        naab_array.push_back(fromJSValue(ctx, elem));  // Recursive
        JS_FreeValue(ctx, elem);
    }

    return std::make_shared<interpreter::Value>(naab_array);
}

// Object
if (JS_IsObject(val)) {
    std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> naab_dict;

    JSPropertyEnum* props = nullptr;
    uint32_t prop_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, val, ...) == 0) {
        for (uint32_t i = 0; i < prop_count; i++) {
            JSAtom atom = props[i].atom;
            const char* key = JS_AtomToCString(ctx, atom);
            if (key) {
                JSValue prop_val = JS_GetProperty(ctx, val, atom);
                naab_dict[std::string(key)] = fromJSValue(ctx, prop_val);  // Recursive
                JS_FreeValue(ctx, prop_val);
                JS_FreeCString(ctx, key);
            }
        }
        // Cleanup
    }

    return std::make_shared<interpreter::Value>(naab_dict);
}
```

**Key Features:**
- Recursive conversion handles nested structures
- Proper memory management with JS_FreeValue calls
- Array indices and object properties converted correctly

#### `toJSValue()` - NAAb → JavaScript

```cpp
else if (std::holds_alternative<std::vector<...>>(val->data)) {
    // Array
    const auto& vec = std::get<std::vector<...>>(val->data);
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < vec.size(); i++) {
        JSValue elem = toJSValue(ctx, vec[i]);  // Recursive
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), elem);
    }
    return arr;
}
else if (std::holds_alternative<std::unordered_map<...>>(val->data)) {
    // Dictionary/Object
    const auto& map = std::get<std::unordered_map<...>>(val->data);
    JSValue obj = JS_NewObject(ctx);
    for (const auto& [key, value] : map) {
        JSValue prop_val = toJSValue(ctx, value);  // Recursive
        JS_SetPropertyStr(ctx, obj, key.c_str(), prop_val);
    }
    return obj;
}
```

### Solution #2: Simplified Wrapping Strategy

Changed the multi-line code wrapping to avoid eval() for simple expressions:

**Before:**
```javascript
// All multi-line code used eval():
(function() { return eval(`[42]`); })()
```

**After:**
```javascript
// Simple expressions (no semicolons, no declarations):
(function() { return ([42]); })()

// Complex code with statements still uses eval:
(function() { return eval(`let x = 42; x * 2`); })()
```

**Detection Logic:**
```cpp
bool is_simple_expr = (code.find(';') == std::string::npos &&
                       code.find("let ") == std::string::npos &&
                       code.find("const ") == std::string::npos &&
                       code.find("var ") == std::string::npos &&
                       code.find("function ") == std::string::npos);

if (is_simple_expr) {
    // Direct wrapping without eval
    wrapped = "(function() { return (" + code + "); })()";
} else {
    // Use eval for complex code
    wrapped = "(function() { return eval(`" + escaped + "`); })()";
}
```

## Files Modified

### `src/runtime/js_executor.cpp`

**Lines 269-290:** Changed multi-line code wrapping strategy
- Added simple expression detection
- Use direct IIFE wrapping for simple expressions
- Keep eval() approach for complex code with statements

**Lines 328-370:** Enhanced `toJSValue()` function
- Added array conversion (NAAb → JavaScript)
- Added object/dict conversion (NAAb → JavaScript)
- Recursive conversion for nested structures

**Lines 373-448:** Enhanced `fromJSValue()` function
- Added array conversion (JavaScript → NAAb)
- Added object conversion (JavaScript → NAAb)
- Recursive conversion for nested structures
- Proper QuickJS memory management

## Testing

Created comprehensive test: `tests/bugs/test_js_arrays_fix.naab`

**Test Coverage:**
1. Empty array
2. Single element array
3. Multiple elements array
4. Mixed types array (number, string, float, boolean, null)
5. Nested arrays
6. Empty object
7. Object with properties
8. Nested objects
9. Array of objects
10. Object with array properties

**Expected Results:**
- All 10 tests should pass
- Arrays return as NAAb arrays with correct elements
- Objects return as NAAb dicts with correct properties
- Nested structures work correctly
- No hanging or timeouts

## Impact

### Before Fix:
- JavaScript arrays → null (unusable)
- JavaScript objects → null (unusable)
- Arrays with elements → program hangs
- Workaround required: `JSON.stringify()` + `json.parse()`

### After Fix:
- JavaScript arrays → NAAb arrays ✅
- JavaScript objects → NAAb dicts ✅
- Nested structures work correctly ✅
- No hanging ✅
- Direct, natural syntax works ✅

### Example Usage:

**Before (Workaround):**
```naab
let data = <<javascript
JSON.stringify([1, 2, 3])
>>
let arr = json.parse(data)  // Extra step required
```

**After (Direct):**
```naab
let arr = <<javascript
[1, 2, 3]
>>
// arr is now a proper NAAb array!
print(arr[0])  // Works: 1
```

## Developer Experience

**Time Saved:**
- No workarounds needed (~2-3 minutes per occurrence)
- Intuitive, expected behavior works out of the box
- Reduced cognitive load understanding polyglot boundaries

**Code Quality:**
- Cleaner code without JSON serialization
- Better type safety (structured data, not strings)
- Natural integration between JavaScript and NAAb

## Technical Notes

### Memory Management
All QuickJS values are properly freed using `JS_FreeValue()` to prevent memory leaks.

### Recursion
Both conversion functions (`fromJSValue` and `toJSValue`) are recursive, supporting arbitrarily nested structures:
- Arrays of arrays of arrays
- Objects containing arrays containing objects
- Mixed nesting to any depth

### Type Mapping

| JavaScript Type | NAAb Type | Notes |
|----------------|-----------|-------|
| `null`, `undefined` | null (monostate) | Both map to NAAb null |
| `boolean` | bool | Direct mapping |
| `number` | int or float | Integer if fits in int32 |
| `string` | string | Direct mapping |
| `Array` | array | Recursive conversion |
| `Object` | dict | Recursive conversion |

### Performance
- Recursive conversion has O(n) time complexity where n is the total number of elements
- Memory usage proportional to structure size
- No noticeable overhead for typical use cases

## Regression Testing

**Baseline Tests:** All 28 comprehensive tests still pass ✅
- No regressions in existing functionality
- Polyglot integration still works for other languages
- All existing JavaScript polyglot code unaffected

## Key Learning

**Avoid Over-Complex Wrapping:**
When wrapping user code for evaluation, simpler is better. The combination of IIFE + eval() + template literals created an unnecessarily complex execution path that exposed edge cases in QuickJS. For simple expressions, direct wrapping is more reliable.

**Complete Type Support:**
When implementing polyglot integration, ensure ALL common types are bidirectionally supported. Missing array/object conversion made JavaScript polyglot blocks much less useful than they should have been.
