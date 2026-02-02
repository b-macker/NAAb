# Issues Investigation Summary

**Date:** 2026-01-31
**Investigator:** Claude (Phase 1 Item 7 work paused to address issues)
**Status:** 3/4 issues resolved, 1 fixed with code changes

---

## Issues Investigated

### ✅ ISS-021: Global `let` after `use` - **BY DESIGN**

**Status:** Not a bug - intentional design decision

**Findings:**
- Parser explicitly rejects global `let` statements with clear error message
- Error: `'let' statements must be inside a 'main {}' block or function.`
- Hint provided: `Top level can only contain: use, import, export, struct, enum, function, main`

**Test:**
```naab
use io

let global_var = 42  // ❌ Rejected by parser

main {
    io.println(global_var)
}
```

**Result:**
```
Error: Parse error at line 3, column 1: 'let' statements must be inside a 'main {}' block or function.
```

**Resolution:**
- This is intentional design to prevent global mutable state
- For module-level constants, use `export let` inside a module file
- For main program, use `let` inside `main {}` block or functions
- **Updated ISSUES.md** to reflect this is a design limitation, not a bug

---

### ✅ ISS-032: Exported `let` variables not accessible - **WORKS CORRECTLY** (File Mismatch Fixed)

**Status:** Feature works - issue was duplicate files with different content

**Findings:**
- Exported `let` variables ARE accessible from importing modules
- The issue was that different `math_utils.naab` files existed in the project
- Root file didn't have `export let PI`, verification directory file did
- Tests loaded different files depending on working directory

**Root Cause:**
```
/home/.naab/language/math_utils.naab
  - Had only functions, NO PI constant
  - Loaded when running from language root ❌

/home/.naab/language/docs/book/verification/ch04_functions/math_utils.naab
  - Had export let PI: float = 3.14159
  - Loaded when running from verification directory ✅
```

**Test (Clean):**
```naab
// test_export_let.naab
export let MY_CONSTANT = 42
export let PI_VALUE = 3.14159

// test_import_let.naab
use test_export_let as mod

main {
    print("Constant:", mod.MY_CONSTANT)
    print("PI:", mod.PI_VALUE)
}
```

**Result:**
```
[INFO] Exported variable: MY_CONSTANT
[INFO] Exported variable: PI_VALUE
Constant: 42
PI: 3.141590
```

**Fix Applied:**
- Updated root `math_utils.naab` to match verification directory version
- Now includes `export let PI: float = 3.14159`
- Tests work from any directory

**Verification:**
```bash
cd /home/.naab/language
./build/naab-lang run docs/book/verification/ch04_functions/test_modules.naab

# Output:
[INFO] Exported variable: PI
PI from module:  3.141590  ✅
```

**Resolution:**
- Feature works perfectly
- File mismatch resolved
- **Updated ISSUES.md** with correct explanation

---

### ✅ ISS-033: `arr.reverse()` unexpected behavior - **NOT A BUG**

**Status:** Works correctly - misunderstood output

**Findings:**
- Array reverse function works perfectly
- Confusion was due to misreading the output

**Test:**
```naab
use array as arr

main {
    let numbers = [42, 17, 8, 91, 23, 56, 3]
    print("Original:", numbers)

    let reversed = arr.reverse(numbers)
    print("Reversed:", reversed)
}
```

**Result:**
```
Original: [42, 17, 8, 91, 23, 56, 3]
Reversed: [3, 56, 23, 91, 8, 17, 42]  ✅ CORRECT!
```

**Verification:**
- Original: `[42, 17, 8, 91, 23, 56, 3]`
- Reversed: `[3, 56, 23, 91, 8, 17, 42]`
- Manual check: `[3, 56, 23, 91, 8, 17, 42]` is indeed the reverse ✅

**Resolution:**
- No bug exists - function works correctly
- **Updated ISSUES.md** to mark as resolved (not a bug)

---

### ✅ ISS-034: Stdlib constants return call markers - **FIXED**

**Status:** Real bug - fixed with code changes

**Findings:**
- Math constants `PI` and `E` returned internal markers instead of values
- Interpreter created call markers for ALL member accesses on stdlib modules
- Constants needed immediate invocation, not deferred call markers

**Before Fix:**
```naab
use math

main {
    print("PI:", math.PI)
}
```

**Output Before:**
```
PI: __stdlib_call__:math:PI  ❌ BUG!
```

**Root Cause:**
```cpp
// interpreter.cpp line ~2883
// OLD CODE: Always created markers for ALL member accesses
std::string func_marker = "__stdlib_call__:" + module_alias + ":" + member_name;
result_ = std::make_shared<Value>(func_marker);
```

**Fix Applied:**
```cpp
// ISS-034 FIX: Check if this is a constant (zero-argument function)
static const std::unordered_set<std::string> math_constants = {"PI", "E"};
if (module_alias == "math" && math_constants.count(member_name) > 0) {
    // Invoke the constant immediately with no arguments
    std::vector<std::shared_ptr<Value>> no_args;
    result_ = module->call(member_name, no_args);
    return;
}

// Create marker for regular functions
std::string func_marker = "__stdlib_call__:" + module_alias + ":" + member_name;
result_ = std::make_shared<Value>(func_marker);
```

**After Fix:**
```naab
use math

main {
    let pi = math.PI
    print("PI value:", pi)

    let e = math.E
    print("E value:", e)
}
```

**Output After:**
```
PI value: 3.141593  ✅ FIXED!
E value: 2.718282   ✅ FIXED!
```

**Files Modified:**
1. `src/interpreter/interpreter.cpp`:
   - Added `#include <unordered_set>` (line ~19)
   - Added constant detection logic (line ~2883)
   - Immediate invocation for constants before marker creation

**Testing:**
```bash
cd ~
cat > test_math_const_fixed.naab << 'EOF'
use math

main {
    let pi = math.PI
    print("PI value: ", pi)

    let e = math.E
    print("E value: ", e)

    let sqrt_val = math.sqrt(16.0)
    print("sqrt(16): ", sqrt_val)
}
EOF

./build/naab-lang run test_math_const_fixed.naab
# Output:
# PI value:  3.141593
# E value:  2.718282
# sqrt(16):  4.000000
```

**Resolution:**
- **Code fix applied and tested**
- Constants now return actual values
- Functions still work correctly (sqrt example)
- **Updated ISSUES.md** with fix details

---

## Summary

| Issue | Status | Action Taken |
|-------|--------|-------------|
| ISS-021 | ✅ By Design | Updated documentation to clarify design decision |
| ISS-032 | ✅ Works Correctly | Verified feature works, updated status |
| ISS-033 | ✅ Not a Bug | Verified correct behavior, clarified confusion |
| ISS-034 | ✅ **FIXED** | **Code changes applied, tested, documented** |

---

## Impact

### Code Changes
- **1 file modified:** `src/interpreter/interpreter.cpp`
- **Lines added:** ~15 lines
- **Build status:** ✅ Successful
- **Tests:** ✅ All manual tests pass

### Documentation Updates
- **1 file updated:** `docs/book/verification/ISSUES.md`
- **4 issues updated** with current status and findings

### User Impact
- **ISS-034 fix:** Users can now access `math.PI` and `math.E` constants
- **Clarifications:** Users understand ISS-021 is intentional design
- **Confidence:** ISS-032 and ISS-033 confirmed working

---

## Next Steps

**Recommended Actions:**

1. **Continue Phase 1 Item 7:** Resume Regex Timeout Preparation work
   - SafeRegex implementation is ready
   - Unit tests are written
   - Documentation is complete
   - Need to complete integration testing

2. **Consider Future Enhancements:**
   - Extend constant detection to other stdlib modules
   - Add more math constants (TAU, SQRT2, LN2, etc.)
   - Document the const vs function pattern for stdlib modules

3. **Testing:**
   - Run full unit test suite to verify fix doesn't break anything
   - Consider adding automated test for ISS-034 fix

---

## Files Modified

### Source Code
```
src/interpreter/interpreter.cpp
  - Line ~19: Added #include <unordered_set>
  - Line ~2883: Added constant detection and immediate invocation
```

### Documentation
```
docs/book/verification/ISSUES.md
  - ISS-021: Updated status to "By Design"
  - ISS-032: Updated status to "Resolved" (works correctly)
  - ISS-033: Updated status to "Resolved" (not a bug)
  - ISS-034: Updated status to "Resolved" with fix details
```

---

**Investigation Complete**
**1 Bug Fixed, 3 Issues Clarified**
**Ready to Resume Phase 1 Work**

---

### ✅ ISS-035: Module system lacks relative import support - **BUG FIXED**

**Status:** Real bug - fixed with code changes

**Findings:**
- Relative imports use **dot notation** (like Python): `use utils.helper`
- Module resolution was broken - always searched from CWD, not source file's directory
- The feature existed but was not working correctly

**Root Cause:**
- `Interpreter::setSourceCode(source, filename)` was NOT setting `current_file_`
- Module resolution fell back to `std::filesystem::current_path()` (CWD)
- This made imports work only when running from specific directories

**Test 1: Simple Subdirectory**
```naab
// File: utils/helper.naab
export fn greet(name: string) -> string {
    return "Hello, " + name
}

// File: main.naab
use utils.helper  // ✅ Dot notation works!

main {
    print(helper.greet("World"))
}
```

**Output:**
```
Hello, World  ✅
```

**Test 2: Deep Nesting**
```naab
// File: lib/math/calculator.naab
export fn add(a: int, b: int) -> int { return a + b }

// File: main.naab
use lib.math.calculator

main {
    print("5 + 3 =", calculator.add(5, 3))
}
```

**Output:**
```
5 + 3 = 8  ✅
```

**Test 3: With Alias**
```naab
use lib.math.calculator as calc

main {
    print("Result:", calc.add(10, 20))
}
```

**Output:**
```
Result: 30  ✅
```

**How It Works:**
```cpp
// src/runtime/module_system.cpp lines 32-49
std::string ModuleRegistry::modulePathToFilePath(const std::string& module_path) const {
    std::string file_path = module_path;

    // Replace dots with directory separators
    for (char& c : file_path) {
        if (c == '.') {
            c = std::filesystem::path::preferred_separator;
        }
    }

    // Add .naab extension
    file_path += ".naab";
    return file_path;
}
```

**Conversion Examples:**
| Module Path | File Path |
|-------------|-----------|
| `utils.helper` | `utils/helper.naab` |
| `lib.math.calculator` | `lib/math/calculator.naab` |
| `app.services.db` | `app/services/db.naab` |

**Documentation Created:**
- `docs/RELATIVE_IMPORTS.md` (comprehensive guide)
- Added ISS-035 to ISSUES.md as resolved

**Fix Applied:**
```cpp
// src/interpreter/interpreter.cpp line ~424
void Interpreter::setSourceCode(const std::string& source, const std::string& filename) {
    source_code_ = source;
    current_file_ = std::filesystem::absolute(filename).string();  // ISS-035 FIX
    error_reporter_.setSource(source, filename);
}
```

**Before Fix:**
- `current_file_` was empty for main program
- Module resolution used: `current_file_.empty() ? std::filesystem::current_path() : ...`
- Always fell back to CWD

**After Fix:**
- `current_file_` set to absolute path of source file
- Module resolution uses: `std::filesystem::path(current_file_).parent_path()`
- Correctly resolves relative to source file's directory

**Testing:**
```bash
cd /data/data/com.termux/files/home/myproject
./build/naab-lang run main3.naab  # ✅ Works!

# Resolved to: /data/data/com.termux/files/home/myproject/utils/helper.naab
```

**Resolution:**
- **Code fix applied and tested**
- Feature now works correctly
- **Updated ISSUES.md** with fix details
- **Updated RELATIVE_IMPORTS.md** with correct behavior

---

## Updated Summary

| Issue | Status | Action Taken |
|-------|--------|-------------|
| ISS-021 | ✅ By Design | Clarified design decision |
| ISS-032 | ✅ Fixed | Fixed file mismatch |
| ISS-033 | ✅ Not a Bug | Verified correct behavior |
| ISS-034 | ✅ **FIXED** | **Code changes applied** |
| ISS-035 | ✅ **FIXED** | **Code fix applied - module resolution corrected** |

---

## Final Code Changes Summary

### Files Modified (Code)
1. `src/interpreter/interpreter.cpp`
   - Line ~19: Added `#include <unordered_set>` (ISS-034)
   - Line ~424: **ISS-035 FIX** - Set `current_file_` in `setSourceCode()`
   - Line ~2883: Added constant detection for math.PI and math.E (ISS-034)

2. `/home/.naab/language/math_utils.naab`
   - Updated to include `export let PI: float = 3.14159` (ISS-032)

### Files Created (Documentation)
1. `docs/ISSUES_INVESTIGATION_2026-01-31.md` - Investigation report
2. `docs/RELATIVE_IMPORTS.md` - Comprehensive relative import guide

### Files Updated (Documentation)
1. `docs/book/verification/ISSUES.md`
   - ISS-021: Updated to "By Design"
   - ISS-032: Updated with file mismatch explanation
   - ISS-033: Updated to "Not a Bug"
   - ISS-034: Updated with fix details
   - ISS-035: **ADDED** with syntax clarification

---

**All Known Issues:** ✅ **RESOLVED**
**Total Issues Fixed:** 2 (ISS-034, ISS-035)
**Total Issues Clarified:** 3 (ISS-021, ISS-033)
**Total Issues Fixed (File Sync):** 1 (ISS-032)
**Documentation Created:** 2 comprehensive guides

