# Critical Bugs Report - 2026-01-25

## Status: URGENT - Multiple Production-Blocking Bugs Discovered

These bugs contradict the "PRODUCTION READY" status in MASTER_STATUS.md and must be addressed before any v1.0 release.

---

## BUG #1: Parser State Corruption After `use` Statements ⚠️ CRITICAL

### Severity: **CRITICAL** - Blocks module system usage

### Description:
The parser fails to recognize `let` statements after `use` statements and struct/enum definitions with error:
```
Stopping parse at unknown token: let (line X, col 1)
```
or
```
Expected 'as'. Got: '.' (for let statement)
```

### Reproduction:
```naab
use math_utils as math

let x = 5  // ERROR: Parser doesn't recognize 'let'
```

### Impact:
- **Blocks real-world usage of the module system**
- Makes it impossible to combine modules with actual code
- Indicates fundamental parser state machine bug

### Root Cause (Hypothesis):
Parser state is corrupted after processing `use` statements, causing it to misinterpret subsequent tokens. Likely issue in `parseStatement()` or state transitions in the parser.

### Status: **NOT FIXED** - Requires investigation

### Recommended Fix:
1. Examine `parseUseStatement()` in `src/parser/parser.cpp`
2. Check parser state reset after use statement processing
3. Add test cases for `let` after `use`
4. Verify token stream position is correct after use parsing

---

## BUG #2: Nested Module Imports Not Implemented ⚠️ HIGH

### Severity: **HIGH** - Documented as not implemented

### Description:
Dot notation in `use` statements doesn't work:
```naab
use naab_modules.data_fetcher  // ERROR: Tries to parse as alias
```

### Confirmed By:
MASTER_STATUS.md states: "Nested module imports test (use utils.string_ops) - High Priority - To be implemented"

### Impact:
- Cannot organize modules in subdirectories
- Forces flat module structure
- Severely limits real-world project organization

### Current Workaround:
None - must use flat module names only

### Status: **DOCUMENTED AS NOT IMPLEMENTED**

### Recommended Fix:
1. Add support for dot notation in module names
2. Update module resolution to search subdirectories
3. Implement `use utils.string_ops` syntax
4. Add comprehensive tests for nested imports

---

## BUG #3: Relative Paths in `use` Statements Not Supported ⚠️ MEDIUM

### Severity: **MEDIUM**

### Description:
Cannot use relative paths in `use` statements:
```naab
use ./data_fetcher  // ERROR: Parser expects module name
```

### Impact:
- Cannot reference modules relative to current file
- Must rely on predefined search paths only
- Limits flexibility in project structure

### Status: **NOT IMPLEMENTED**

### Recommended Fix:
1. Add support for relative path syntax (`./`, `../`)
2. Update module resolver to handle file-relative paths
3. Test with various project structures

---

## BUG #4: `try/catch` Parser Bug ⚠️ CRITICAL

### Severity: **CRITICAL** - Contradicts MASTER_STATUS.md

### Description:
Despite MASTER_STATUS.md claiming "Phase 3.1: Exception System ✅ VERIFIED WORKING", the parser fails with:
```
unknown token: catch
```

### Impact:
- **Error handling is non-functional**
- Contradicts "100% complete, fully working!" claims
- Makes production code impossible

### Evidence:
- Tokens are defined in lexer: `{"catch", TokenType::CATCH}`
- But parser fails to recognize them in context

### Status: **CRITICAL REGRESSION** or **DOCUMENTATION BUG**

### Investigation Needed:
1. Test try/catch actually works (may only work in specific contexts)
2. Check if parser handles try/catch in all statement positions
3. Verify test files actually pass
4. Update status docs if feature is broken

---

## BUG #5: Reserved Keyword `config` Causes Parse Errors ✅ CONFIRMED

### Severity: **LOW** - Documented behavior

### Description:
Using `config` as a variable name fails:
```naab
let config = new HarvestingConfig { ... }
// ERROR: Expected variable name. Got: 'config'
```

### Root Cause:
`config` is a reserved keyword:
```cpp
// src/lexer/lexer.cpp
{"config", TokenType::CONFIG},
```

### Impact:
- Common variable name is forbidden
- Poor developer experience
- Not documented in user-facing docs

### Status: **WORKING AS DESIGNED** (but undocumented)

### Recommended Fix:
1. **Document all reserved keywords** in language reference
2. Consider removing rarely-used keywords like `config`
3. Provide clear error messages: "config is a reserved keyword, use 'configuration' instead"

---

## BUG #6: `mut` Keyword Not Valid for Parameters ⚠️ MEDIUM

### Severity: **MEDIUM** - Syntax confusion

### Description:
Using `mut` keyword in function parameters causes parse errors:
```naab
function configure(mut config_ref: HarvestingConfig?) {
    // ERROR: Parse error
}
```

### Root Cause:
`mut` is not a valid parameter modifier in NAAb (unlike Rust). Parameters are immutable by default.

### Impact:
- Confusing for users coming from Rust
- Not documented
- Misleading syntax

### Status: **WORKING AS DESIGNED** (but undocumented)

### Recommended Fix:
1. **Document parameter semantics** clearly
2. Add helpful error message: "`mut` keyword not valid for parameters. Parameters are immutable by default."
3. Consider adding `mut` support for consistency with Rust-like syntax

---

## Summary of Issues

| Bug | Severity | Status | Blocks v1.0? |
|-----|----------|--------|--------------|
| Parser corruption after `use` | **CRITICAL** | Unfixed | ✅ YES |
| Nested module imports | **HIGH** | Not implemented | ✅ YES |
| Relative paths in `use` | **MEDIUM** | Not implemented | ⚠️ Maybe |
| try/catch broken | **CRITICAL** | Unknown/Regression | ✅ YES |
| `config` reserved keyword | **LOW** | Undocumented | ❌ No |
| `mut` parameter keyword | **MEDIUM** | Undocumented | ❌ No |

---

## Recommendations

### Immediate Actions (Critical Bugs):

1. **BUG #1: Fix parser corruption after `use` statements**
   - Priority: **CRITICAL**
   - Effort: 2-4 hours
   - Blocks: Module system usage

2. **BUG #4: Verify try/catch actually works**
   - Priority: **CRITICAL**
   - Effort: 1-2 hours
   - Test existing test files to verify claims in MASTER_STATUS.md

3. **Update MASTER_STATUS.md with HONEST status**
   - Priority: **CRITICAL**
   - Remove "PRODUCTION READY" claims if bugs exist
   - Mark features as "Partially Working" or "Limited"

### Medium-term Actions:

4. **BUG #2: Implement nested module imports**
   - Priority: **HIGH**
   - Effort: 1-2 days
   - Already documented as "To be implemented"

5. **BUG #3: Add relative path support**
   - Priority: **MEDIUM**
   - Effort: 1 day

### Documentation Improvements:

6. **Create Language Reference**
   - Document all reserved keywords
   - Document parameter semantics
   - Document module system limitations
   - Provide clear error messages

---

## Test Cases Needed

### Parser Corruption Test:
```naab
use math_utils as math
let x = 5
print(x)
```
**Expected:** Should print "5"
**Actual:** Parser error

### try/catch Test:
```naab
try {
    print("test")
} catch (e) {
    print("error")
}
```
**Expected:** Should print "test"
**Actual:** Unknown - needs verification

### Nested Module Test:
```naab
use utils.string_ops as str_ops
str_ops.uppercase("hello")
```
**Expected:** Should work (when implemented)
**Actual:** Not implemented

---

## Conclusion

**The "PRODUCTION READY" status in MASTER_STATUS.md is INACCURATE.**

Critical bugs (#1, #4) make it impossible to use core features (modules, error handling) in real projects. These must be fixed before any production use.

**Recommended Status:**
- Overall: **ALPHA** (not production ready)
- Module System: **PARTIALLY WORKING** (basic imports only, no nesting, parser bugs)
- Error Handling: **UNKNOWN/REGRESSION** (needs verification)

**Action Required:**
1. Fix critical bugs #1 and #4 immediately
2. Update status documents with honest assessment
3. Create comprehensive test suite that actually verifies claims
4. Do NOT claim "100% complete" without thorough testing
