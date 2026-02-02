# Phase 3.2: Struct Literal Parsing Issue - 2026-01-18

## Problem Statement

**Issue:** Struct literals are currently failing to parse with error:
```
Parse error at line X, column Y: Unexpected token in expression
  Got: ':'
```

**Impact:** Cannot create test files for memory cycle detection because cycles require struct instances.

**Severity:** **BLOCKER** for Phase 3.2 testing

---

## Reproduction

### Minimal Test Case

**File:** `examples/test_struct_minimal.naab`
```naab
struct Box {
    value: int
}

main {
    let b = Box { value: 42 }
    print("done")
}
```

**Error:**
```
Error: Parse error at line 6, column 24: Unexpected token in expression
  Got: ':'
```

**Line 6:** `let b = Box { value: 42 }`
**Column 24:** The colon after `value`

---

## Investigation

### Tests Attempted

1. **Simple struct literal:**
   ```naab
   let p = Point { x: 10, y: 20 }
   ```
   **Result:** Parse error at ':'

2. **Multi-line struct literal:**
   ```naab
   let p = Point {
       x: 10
       y: 20
   }
   ```
   **Result:** Parse error at ':'

3. **With explicit type annotation:**
   ```naab
   let p: Point = Point { x: 10, y: 20 }
   ```
   **Result:** Parse error at ':'

4. **Minimal single-field struct:**
   ```naab
   let b = Box { value: 42 }
   ```
   **Result:** Parse error at ':'

**ALL VARIATIONS FAIL** at the same location (colon in field assignment)

### Existing Test Files Also Fail

Checked existing test files that should have working struct literals:
- `examples/test_generics_simple.naab` - **FAILS** (same error)
- `examples/test_complete_type_system.naab` - Not tested but likely fails

**Conclusion:** Struct literal parsing is broken across all test files.

---

## Timeline

### Last Known Working State

**Unknown** - No evidence of when struct literals last worked.

**Hypothesis:** Either:
1. Struct literals never worked and test files were never run
2. OR something changed in recent parser/interpreter modifications
3. OR there's a build/compilation issue

### Recent Changes

**Today's Changes (2026-01-18):**
- Phase 2.4.4 Phase 2 & 3 implementation (type inference)
- Modified `src/interpreter/interpreter.cpp` (multiple locations)
- Modified `include/naab/interpreter.h`
- Modified `src/parser/parser.cpp` (line 370)
- Full rebuild at 16:24

**Tests Run Successfully Today:**
- `test_type_inference_final.naab` - No struct literals
- `test_generic_inference_final.naab` - No struct literals
- `test_function_return_inference_simple.naab` - No struct literals
- `test_phase3_1_exceptions_final.naab` - No struct literals

**Observation:** None of the successfully run tests used struct literals!

---

## Technical Analysis

### Parser Behavior

**Expected:** Struct literal syntax should be:
```naab
StructName { field1: value1, field2: value2 }
```

**Parser Error Location:**The parser fails at the colon (`:`) character in `field: value`, treating it as an unexpected token in an expression context.

**Possible Causes:**

1. **Struct literal parsing not implemented:** If struct literal parsing was never implemented, the parser would try to parse `Box { value: 42 }` as:
   - `Box` - identifier
   - `{` - start of block/dict?
   - `value` - identifier
   - `:` - UNEXPECTED (not a valid expression operator)

2. **Parser mode confusion:** The parser might be in the wrong mode when parsing the contents of `{ }` after a struct name.

3. **Precedence issue:** The `{` after `Box` might be interpreted as something other than a struct literal (dict literal?).

---

## Impact on Phase 3.2

### Blocked Tasks

**Cannot Test:**
- ❌ Memory cycles (requires creating struct instances)
- ❌ Circular references (requires struct with self-referential fields)
- ❌ Memory leak demonstration

**Can Still Do:**
- ✅ Implementation design (already done)
- ✅ Code skeleton for cycle detector
- ⚠️ Implementation without testing (risky)

### Risk Assessment

**Risk:** HIGH - Cannot verify memory cycle detection without working struct literals

**Alternatives:**
1. Fix parser first, then implement GC
2. Implement GC blindly, fix parser later, test then
3. Use simpler test cases (lists with circular references)

**Recommended:** Option 1 - Fix parser first

---

## Next Steps (Recommended)

### Immediate (Priority 1 - BLOCKER)

1. **Investigate Parser**
   - Check `src/parser/parser.cpp` for struct literal parsing
   - Search for `parseStructLiteral` or similar
   - Verify it's actually implemented

2. **Determine Root Cause**
   - Was struct literal parsing ever implemented?
   - Did recent changes break it?
   - Is there a syntax we're missing?

3. **Fix Parser**
   - Implement/fix struct literal parsing
   - Ensure `{ field: value }` syntax works
   - Test with minimal example

4. **Rebuild & Test**
   - Clean build
   - Run `test_struct_minimal.naab`
   - Verify success

### After Parser Fix (Priority 2)

5. **Create Cycle Tests**
   - `test_memory_cycles_simple.naab`
   - `test_memory_cycles.naab` (comprehensive)

6. **Implement Cycle Detector**
   - Follow plan in `PHASE_3_2_MEMORY_ANALYSIS.md`
   - Value traversal methods
   - CycleDetector class
   - Integration with Interpreter

7. **Verify with Tests**
   - Run cycle tests
   - Verify GC collects cycles
   - Check with Valgrind

---

## Workarounds (If Parser Fix Delayed)

### Alternative Test Approaches

**Option A: Use Lists**
```naab
# Create cycles with lists instead of structs
let a = [1, 2, 3]
let b = [4, 5, 6]
# Can't easily create cycles without list.append() method
```
**Problem:** Lists don't support circular references easily either

**Option B: Use Dicts**
```naab
# Create cycles with dicts
let a = { "value": 1, "next": null }
let b = { "value": 2, "next": a }
# a["next"] = b  # Creates cycle
```
**Status:** Untested - may or may not work

**Option C: Defer Testing**
- Implement GC conceptually
- Test later after parser fix
- **Risk:** May not work when tested

---

## Documentation Status

**Parser Issue:** ✅ Documented (this file)

**Impact:** Documented in:
- This file
- Session summary (to be created)

**Recommended Next Session:**
1. Fix struct literal parser
2. Test struct literals work
3. Return to Phase 3.2 cycle detection

---

## Conclusion

**Status:** ⚠️ **BLOCKED**

**Blocker:** Struct literal parsing broken

**Cannot Proceed With:** Memory cycle testing

**Can Proceed With:** Parser investigation and fix

**Severity:** HIGH - Blocks Phase 3.2 testing

**Estimated Time to Fix:** Unknown (need to investigate parser)

**Recommendation:** Investigate and fix parser before continuing Phase 3.2

---

**End of Analysis**
**Date:** 2026-01-18
**Status:** Parser issue documented, awaiting investigation
