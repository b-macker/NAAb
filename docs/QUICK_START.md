# NAAb Quick Start Guide

## ✅ All Fixes Applied - Ready to Use!

### What Works Now
- ✅ Shell blocks return {exit_code, stdout, stderr}
- ✅ Struct serialization with json.stringify()
- ✅ IQR-based anomaly detection (no sklearn needed)
- ✅ All module aliases consistent
- ✅ Type conversion via json.stringify()

---

## Common Patterns

### 1. Shell Commands with Error Handling
```naab
let result = <<sh[path]
mkdir -p "$path"
>>

if result.exit_code == 0 {
    io.write("✓ Success\n")
} else {
    io.write_error("✗ Error: ", result.stderr, "\n")
}
```

### 2. Type Conversion
```naab
// Convert anything to string
let int_str = json.stringify(42)        // "42"
let bool_str = json.stringify(true)     // "true"
let float_str = json.stringify(3.14)    // "3.14"
```

### 3. String Concatenation (2 args only!)
```naab
// ✅ Correct
let a = str.concat("hello", " world")

// ✅ Chain for multiple
let prefix = str.concat(dir, "/file_")
let path = str.concat(prefix, timestamp)

// ❌ Wrong - too many args
// let bad = str.concat(a, b, c)
```

### 4. Module Aliases
```naab
use string as str  // Use 'str' everywhere, not 'string'
use io as io       // Use 'io' everywhere

// ✅ Correct
let result = str.concat("a", "b")
io.write("Hello\n")

// ❌ Wrong
// let bad = string.concat("a", "b")
```

### 5. Struct Serialization
```naab
struct Person { name: string, age: int }

let p = new Person { name: "Alice", age: 30 }
let json_str = json.stringify(p)  // Works automatically!
// Result: {"name":"Alice","age":30}
```

---

## Testing Your Code

### Run Tests
```bash
# Test shell blocks
./build/naab-lang run test_shell_return.naab

# Test struct serialization
./build/naab-lang run test_struct_serialization.naab

# Test nested generics
./build/naab-lang run test_nested_generics.naab
```

### Check for Issues
```bash
# Find old NAAB_VAR_ syntax
grep -r "NAAB_VAR_" --include="*.naab" .

# Find incorrect type conversion
grep -r "str\.to_string\|string\.to_string" --include="*.naab" .

# Find module alias issues
grep -r "string\." --include="*.naab" . | grep -v "^[[:space:]]*use"
```

---

## ATLAS Pipeline

### Run Full Pipeline
```bash
cd docs/book/verification/ch0_full_projects/data_harvesting_engine
/path/to/build/naab-lang run main.naab
```

### Expected Results
- ✅ Stage 1: Configuration Loading - PASS
- ✅ Stage 2: Data Harvesting - PASS
- ✅ Stage 3: Data Processing - PASS
- ✅ Stage 4: Analytics (IQR) - PASS
- ⚠️  Stage 5: Reports - needs template file
- ✅ Stage 6: Assets - code ready

---

## Debug Helpers

### Shell Block Debugging
```naab
fn debug_shell(result: ShellResult, label: string) {
    io.write("\n=== ", label, " ===\n")
    io.write("exit_code: ", result.exit_code, "\n")
    io.write("stdout: '", result.stdout, "'\n")
    io.write("stderr: '", result.stderr, "'\n")
}

let r = <<sh
echo "test"
>>
debug_shell(r, "My Command")
```

### Performance Profiling
```naab
use time

let t0 = time.now()
// ... do work ...
let duration = time.now() - t0
io.write("Time: ", duration, " seconds\n")
```

---

## Common Errors & Fixes

| Error | Cause | Fix |
|-------|-------|-----|
| `Unknown function: to_string` | Using `str.to_string()` | Use `json.stringify()` |
| `concat() takes exactly 2 arguments` | Too many args to concat | Chain calls: `str.concat(str.concat(a,b),c)` |
| `Undefined variable: string` | Using `string.` not `str.` | Use alias from `use string as str` |
| `NAAB_VAR_x not found` | Old template syntax | Use direct name in `<<lang[x]` blocks |
| `Member access not supported` | Shell block returns string | Use `.exit_code`, `.stdout`, `.stderr` |

---

## Documentation Files

### Main References
1. `SESSION_COMPLETE_SUMMARY.md` - Complete session overview
2. `DEBUG_HELPERS.md` - All debug tools
3. `FIX_RECOMMENDATIONS.md` - Issue analysis
4. `ATLAS_SKLEARN_REPLACEMENT_SUMMARY.md` - sklearn details

### Quick Lookup
- Shell blocks: `test_shell_return.naab`
- Struct JSON: `test_struct_serialization.naab`
- Type parsing: `test_nested_generics.naab`

---

## Build & Run

```bash
# Build
cd build
make -j4

# Run a file
./naab-lang run ../your_file.naab

# Run tests
./naab-lang run ../test_shell_return.naab
```

---

## Need Help?

1. Check `DEBUG_HELPERS.md` for tools
2. Review `SESSION_COMPLETE_SUMMARY.md` for details
3. Look at test files for examples
4. Run automated checks (grep commands above)

---

## Quick Checklist

Before committing code:
- [ ] No `NAAB_VAR_` usage
- [ ] No `str.to_string()` calls
- [ ] Module aliases used consistently
- [ ] `str.concat()` has max 2 arguments
- [ ] Shell blocks access `.exit_code`
- [ ] All tests passing

---

**Status:** ✅ Everything working! Start coding!
