# Known Issues

## JavaScript Array/Object Type Conversion (Bug #3)

**Status:** Under Investigation
**Severity:** Medium
**Workaround:** Available

### Description
JavaScript polyglot blocks cannot return arrays or objects as structured data. They are converted to strings instead.

### Symptoms
```naab
let arr = <<javascript
[1, 2, 3]
>>
// arr is treated as string, not array
```

### Root Cause
The `fromJSValue()` function in `js_executor.cpp` lacks array and object conversion support. When implementing this feature, execution hangs when processing arrays with elements (empty arrays work fine).

### Investigation Findings
- Empty JavaScript arrays work correctly
- Arrays with elements cause hanging before `fromJSValue()` is even called
- The hang occurs during or before `JS_Eval()` in the `evaluate()` method
- The hang is NOT in type conversion code (confirmed via debug output)
- Possible causes: QuickJS runtime state, resource limiter, or context issues
- Cross-language-bridge has similar code but is only used for Python

### Workaround
Use `JSON.stringify()` to return complex data as strings:

```naab
let data = <<javascript
JSON.stringify([1, 2, 3])
>>
let parsed = json.parse(data)  // Use NAAb's json module
```

### Next Steps
1. Investigate with proper debugging tools (gdb, lldb)
2. Check if QuickJS needs special initialization for array handling
3. Compare runtime state between working (empty array) and hanging (array with elements) cases
4. Consider alternative QuickJS API calls for array access

### Related Files
- `src/runtime/js_executor.cpp` - JavaScript executor
- `src/runtime/cross_language_bridge.cpp` - Similar (unused) implementation
- `tests/bugs/test_js_types.naab` - Test cases for this issue
