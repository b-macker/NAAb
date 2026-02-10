# Known Issues

## ~~JavaScript Array/Object Type Conversion (Bug #3)~~ ✅ FIXED

**Status:** ✅ FIXED (February 2026)
**Severity:** Was Medium
**Solution:** Complete array/object conversion support added

### Description
JavaScript polyglot blocks can now return arrays and objects as structured data. This was previously broken, causing either null returns or program hangs.

### What Was Fixed
1. **Added complete array/object support** to `fromJSValue()` and `toJSValue()`
   - Recursive conversion for nested structures
   - Proper memory management with QuickJS
   - Bidirectional support (JS ↔ NAAb)

2. **Simplified code wrapping strategy**
   - Simple expressions: Direct IIFE wrapping without eval()
   - Complex code: eval() approach for statements
   - Avoids problematic eval() + template literal combination

### Now Works Correctly
```naab
let arr = <<javascript
[1, 2, 3]
>>
// arr is now a proper NAAb array!
print(arr[0])  // Works: 1

let obj = <<javascript
({name: "Alice", age: 30})
>>
print(obj["name"])  // Works: "Alice"
```

### Documentation
See `BUG_3_JAVASCRIPT_ARRAYS_FIX.md` for complete technical details.

### Tests
- `tests/bugs/test_js_arrays_fix.naab` - 10 comprehensive tests
- All nested structures, mixed types, arrays of objects, etc.

---

## No Other Known Issues

All discovered bugs have been fixed and tested. The NAAb language polyglot integration now supports:
- ✅ All primitive types (int, float, bool, string, null)
- ✅ Arrays with recursive nesting
- ✅ Objects/dicts with recursive nesting
- ✅ Mixed structures (arrays of objects, objects with arrays)
- ✅ All polyglot operators (`>>` in code, etc.)
- ✅ Number parsing (leading/trailing decimals)
- ✅ Python large integers (graceful overflow)

If you discover a new issue, please document it here following the template above.
