# Bug Fix Testing Plan - 2026-01-25

## Overview

This document outlines the testing plan for verifying all bug fixes and enhancements made on 2026-01-25.

**Status:** Code changes complete, pending build and test execution

---

## Build Instructions

### Step 1: Compile Changes

```bash
cd /data/data/com.termux/files/home/.naab/language/build
make -j4
```

**Expected Result:** Clean build with no errors

**Files Modified:**
- `src/parser/parser.cpp` - Error messages + alias parsing
- `include/naab/ast.h` - ModuleUseStmt alias field
- `src/interpreter/interpreter.cpp` - Alias resolution logic

---

## Test Suite

### Test 1: Improved Error Message for Top-Level `let`

**Purpose:** Verify helpful error message guides users to correct syntax

**Test File:** `test_error_message_improved.naab`

```bash
./naab-lang run ../test_error_message_improved.naab
```

**Expected Output:**
```
Parse error at line 5, column 1: 'let' statements must be inside a 'main {}' block or function.
  Hint: Top level can only contain: use, import, export, struct, enum, function, main
```

**Success Criteria:**
- ✅ Error message is clear and helpful
- ✅ Error message explains the solution (wrap in main {})
- ✅ Error message shows correct line/column

---

### Test 2: Correct Usage Pattern (let inside main)

**Purpose:** Verify correct syntax works as expected

**Test File:** `test_use_with_main.naab`

```bash
./naab-lang run ../test_use_with_main.naab
```

**Expected Output:**
```
x =  10
y =  20
sum =  30
```

**Success Criteria:**
- ✅ Program executes successfully
- ✅ Module import works (math.add function)
- ✅ Variables defined inside main block
- ✅ All operations complete correctly

---

### Test 3: Alias Support - Basic Functionality

**Purpose:** Verify `use module as alias` syntax works

**Test File:** `test_alias_support.naab`

```bash
./naab-lang run ../test_alias_support.naab
```

**Expected Output:**
```
=== Testing Module Alias Support ===
Test 1: math.add(5, 10) = 15
Test 2: math.multiply(3, 7) = 21
Test 3: math.subtract(20, 8) = 12
=== All alias tests passed ===
```

**Success Criteria:**
- ✅ Alias parsing works (use math_utils as math)
- ✅ Alias resolves correctly (math.add works)
- ✅ Multiple operations work with same alias
- ✅ All calculations are correct

---

### Test 4: Nested Module with Short Alias

**Purpose:** Verify short aliases work correctly

**Test File:** `test_nested_module_alias.naab`

```bash
./naab-lang run ../test_nested_module_alias.naab
```

**Expected Output:**
```
=== Testing Nested Module with Alias ===
m.add(100, 200) = 300
=== Nested module alias test complete ===
```

**Success Criteria:**
- ✅ Short alias "m" works
- ✅ Module function called successfully
- ✅ Result is correct

---

### Test 5: try/catch Verification (Already Verified)

**Purpose:** Re-verify try/catch works after parser changes

**Test File:** `test_trycatch_verify.naab`

```bash
./naab-lang run ../test_trycatch_verify.naab
```

**Expected Output:**
```
=== Testing try/catch ===
Test 1: In try block
x =  5
Test 2: Before throw
Test 2: Caught:  Test error
=== try/catch tests complete ===
```

**Success Criteria:**
- ✅ try block executes normally
- ✅ catch block executes on throw
- ✅ Exception value passed to catch
- ✅ No "unknown token: catch" error

---

## Advanced Tests (Future)

### Test 6: Nested Module File Resolution

**Purpose:** Test file system resolution for nested paths

**Prerequisite:** Create directory structure:
```
naab_modules/
  data/
    processor.naab
```

**Test Code:**
```naab
use data.processor as dp

main {
    dp.process("test data")
}
```

**Status:** ⚠️ NOT YET IMPLEMENTED - Parser works, file resolution pending

---

## Regression Tests

### Existing Test Files to Re-run

Verify no functionality was broken by the changes:

```bash
# Run all existing beta tests
./naab-lang run ../beta_test_*.naab

# Verify module system still works
./naab-lang run ../test_module_use.naab

# Verify polyglot still works
./naab-lang run ../test_phase2_3_return_values.naab
```

**Success Criteria:**
- ✅ All previously passing tests still pass
- ✅ No new errors introduced
- ✅ No performance degradation

---

## Documentation Verification

### Check Documentation Updates

**RESERVED_KEYWORDS.md:**
- ✅ File exists
- ✅ Lists all reserved keywords including `config`
- ✅ Provides examples and workarounds
- ✅ Clear formatting

**CRITICAL_BUGS_REPORT_2026_01_25.md:**
- ✅ Updated with investigation results
- ✅ All bugs categorized correctly (fixed/not-a-bug/verified)
- ✅ Summary table updated
- ✅ Conclusion reflects accurate status

---

## Code Quality Checks

### Static Analysis

```bash
# Check for warnings
make 2>&1 | grep -i warning

# Check for unused includes
# (Optional - low priority)
```

**Expected Result:** No critical warnings

---

## Performance Verification

### Parser Performance

```bash
# Time parsing of large file with many use statements
time ./naab-lang parse ../large_test_file.naab
```

**Success Criteria:**
- ✅ No significant slowdown vs previous version
- ✅ Parser handles 100+ use statements efficiently

---

## Checklist Before Commit

- [ ] All test files execute successfully
- [ ] No build errors or warnings
- [ ] Regression tests pass
- [ ] Documentation is updated
- [ ] CRITICAL_BUGS_REPORT reflects accurate status
- [ ] Code changes are clean and well-commented
- [ ] Test files are organized and named clearly

---

## Test Execution Summary Template

```
=== NAAb Bug Fix Testing - 2026-01-25 ===

Build Status: [ PASS / FAIL ]
  - Compilation: [ PASS / FAIL ]
  - Warnings: [ count ]

Test Results:
  - Test 1 (Error Message): [ PASS / FAIL ]
  - Test 2 (Correct Usage): [ PASS / FAIL ]
  - Test 3 (Alias Support): [ PASS / FAIL ]
  - Test 4 (Short Alias): [ PASS / FAIL ]
  - Test 5 (try/catch): [ PASS / FAIL ]

Regression Tests: [ PASS / FAIL ]
  - Beta tests: [ X / Y passed ]
  - Module tests: [ PASS / FAIL ]
  - Polyglot tests: [ PASS / FAIL ]

Overall Status: [ ALL PASS / ISSUES FOUND ]

Issues Found:
  - [ List any issues ]

Next Steps:
  - [ List next actions ]
```

---

## Known Limitations

1. **Nested Module File Resolution** - Parser works, but file system resolution for paths like `naab_modules/data/processor.naab` needs implementation
2. **Build Environment** - Termux /tmp directory issues may require workarounds
3. **Performance Testing** - No benchmarks available yet for parser changes

---

## Success Metrics

**All tests passing indicates:**
- ✅ Parser handles aliases correctly
- ✅ Error messages are helpful
- ✅ No regressions in existing functionality
- ✅ try/catch continues to work
- ✅ Code is ready for commit

**Ready for v1.0 when:**
- ✅ All bug fixes tested and working
- ✅ Documentation complete
- ✅ No critical issues remaining
- ✅ Nested file resolution implemented (optional for v1.0)
