# ISSUES.md Update Instructions

**Purpose:** This document provides verified information to update `../ISSUES.md` based on systematic testing of all 17 issues.

**Date Verified:** 2026-01-22
**Test Coverage:** 17/17 issues (100%)
**Test Files Location:** `/data/data/com.termux/files/home/.naab/language/docs/book/verification/solve_ISSUES/`

---

## Instructions for Updating ISSUES.md

For each issue below:
1. Locate the issue row in ISSUES.md
2. Replace the entire row with the "Updated Entry" provided
3. Keep the table formatting consistent
4. Preserve the table headers

---

## Issues to Mark as RESOLVED (4 issues)

### ISS-014: Range Operator

**Current Entry in ISSUES.md:**
```markdown
| ISS-014 | ALL | Range Operator (`..`) | ❌ Missing | `QUICK_REFERENCE.naab` and `AI_ASSISTANT_GUIDE.md` explicitly state "Range operator (..) not yet implemented." |
```

**Updated Entry:**
```markdown
| ISS-014 | ALL | Range Operator (`..`) | ✅ RESOLVED | Fully implemented 2026-01-22. Syntax `start..end` (exclusive end) works correctly. Tested: basic ranges (0..5), variables (start..end), arithmetic (2*3..2*5), nested loops, break/continue, accumulation. All tests passing. See `test_ISS_014_range_operator.naab`. **Action Required:** Update `AI_ASSISTANT_GUIDE.md` line 1121 and `QUICK_REFERENCE.naab` to show range operator is available. |
```

**Evidence:**
- Test file: `test_ISS_014_range_operator.naab` - ALL TESTS PASSED
- Range operator works: `for i in 0..5` → 0,1,2,3,4
- Sum test: 1..11 → 55 (correct)
- Implemented in: lexer.cpp, parser.cpp, interpreter.cpp, ast.h

---

### ISS-015: Time Module Status Discrepancy

**Current Entry in ISSUES.md:**
```markdown
| ISS-015 | Ch 09 | `time` module status discrepancy | ⚠️ Warning | `AI_ASSISTANT_GUIDE.md` lists "Time Module for Benchmarking" as a "❌ Missing Feature", contradicting `TIME_MODULE_DISCOVERY_2026_01_21.md` and my successful verification (Chapter 9 test passed `time.now()`, `time.sleep()`, etc.). The `time` module *does* work, making the `AI_ASSISTANT_GUIDE.md` statement incorrect. |
```

**Updated Entry:**
```markdown
| ISS-015 | Ch 09 | `time` module status discrepancy | ✅ RESOLVED | Time module is fully functional and production-ready. All 12 functions work correctly: `now()`, `now_millis()`, `sleep()`, `year()`, `month()`, `day()`, `hour()`, `minute()`, `second()`, `weekday()`, `format_timestamp()`, `parse_datetime()`. Verified 2026-01-22. See `test_ISS_015_time_module.naab` and `TIME_MODULE_DISCOVERY_2026_01_21.md`. **Action Required:** Update `AI_ASSISTANT_GUIDE.md` line 1121 to remove "❌ Missing Feature" and mark as "✅ Available". |
```

**Evidence:**
- Test file: `test_ISS_015_time_module.naab` - ALL TESTS PASSED
- `time.now()` → 1769098229.0 ✓
- `time.now_millis()` → working ✓
- `time.sleep(0.1)` → working ✓
- Benchmarking: 1000 iterations in 26ms ✓

---

### ISS-011: File Module (Already Marked Resolved)

**Current Entry in ISSUES.md:**
```markdown
| ISS-011 | Ch 09 | `file` module (`read`, `write`, `append`, `delete`) | ✅ Resolved | Functions `fs.read`, `fs.write`, `fs.append`, `fs.delete` are now working correctly after correcting function names in the book and test. |
```

**Updated Entry:**
```markdown
| ISS-011 | Ch 09 | `file` module (`read`, `write`, `append`, `delete`) | ✅ Resolved | Functions `fs.read`, `fs.write`, `fs.append`, `fs.delete` verified working 2026-01-22. All file operations tested successfully. See `test_ISS_011_file.naab`. |
```

**Evidence:**
- Test file: `test_ISS_011_file.naab` - ALL TESTS PASSED
- fs.write() → works ✓
- fs.read() → works ✓
- fs.append() → works ✓
- fs.delete() → works ✓

---

### ISS-012: Env Module (Already Marked Resolved)

**Current Entry in ISSUES.md:**
```markdown
| ISS-012 | Ch 09 | `env` module (`get`, `set_var`) | ✅ Resolved | Functions `env.get`, `env.set_var` are now working correctly after correcting function names in the book and test. |
```

**Updated Entry:**
```markdown
| ISS-012 | Ch 09 | `env` module (`get`, `set_var`) | ✅ Resolved | Functions `env.get`, `env.set_var` verified working 2026-01-22. Environment variable operations tested successfully. See `test_ISS_012_env.naab`. |
```

**Evidence:**
- Test file: `test_ISS_012_env.naab` - ALL TESTS PASSED
- env.set_var() → works ✓
- env.get() → works ✓

---

## Issues to Update Status/Description (7 issues)

### ISS-007: Array HOF Naming

**Current Entry in ISSUES.md:**
```markdown
| ISS-007 | Ch 07 | `array` HOFs (`map_fn`, `filter_fn`, `reduce_fn`) | ❌ Naming Mismatch | `array_impl.cpp` implements higher-order functions as `map_fn`, `filter_fn`, `reduce_fn`. Documentation consistently uses `map`, `filter`, `reduce` (without `_fn` suffix). The functions should be called with `_fn` suffix. |
```

**Updated Entry:**
```markdown
| ISS-007 | Ch 07 | `array` HOFs (`map_fn`, `filter_fn`, `reduce_fn`) | ✅ WORKS (docs need update) | Functions work correctly with `_fn` suffix: `arr.map_fn()`, `arr.filter_fn()`, `arr.reduce_fn()`. Verified 2026-01-22. This is NOT a bug - the functions work. **Action Required:** Update all documentation to consistently use `map_fn`, `filter_fn`, `reduce_fn` (WITH `_fn` suffix). See `test_ISS_007_array_hof.naab`. |
```

**Evidence:**
- Test file: `test_ISS_007_array_hof.naab` - ALL TESTS PASSED
- arr.map_fn(numbers, double) → [2,4,6,8,10] ✓
- arr.filter_fn(numbers, is_even) → [2,4] ✓
- arr.reduce_fn(numbers, sum_fn, 0) → 15 ✓

---

### ISS-008: Collections Functional Style

**Current Entry in ISSUES.md:**
```markdown
| ISS-008 | Ch 09 | `collections` module (`Set` constructor, `set_add`, etc.) | ❌ Buggy Internal State | `CollectionsModule` implements `Set` (constructor) and `set_add`, `set_remove`, etc. The issue was not that functions were missing, but that `set_add` returns a NEW set (functional style), requiring reassignment. The test `array_collections.naab` is now updated to reflect this. |
```

**Updated Entry:**
```markdown
| ISS-008 | Ch 09 | `collections` module (`Set` constructor, `set_add`, etc.) | ✅ WORKS (functional style by design) | Collections module works correctly. `set_add()` returns a NEW set (functional/immutable style), requiring reassignment: `set = coll.set_add(set, value)`. This is intentional design, not a bug. Verified 2026-01-22. **Action Required:** Update documentation to clearly show reassignment pattern. See `test_ISS_008_collections.naab`. |
```

**Evidence:**
- Test file: `test_ISS_008_collections.naab` - WORKS AS DESIGNED
- Without reassignment: set remains empty (expected)
- With reassignment: set = coll.set_add(set, 1) → [1] ✓
- Contains check: coll.set_contains(set, 1) → true ✓

---

### ISS-009: Regex Module

**Current Entry in ISSUES.md:**
```markdown
| ISS-009 | Ch 08 | `regex` module (`match`, `search`, etc.) | ❌ Failed | All functions in the `regex` module (`re.match`, `re.search`, `re.find_all`, `re.replace`, `re.full_match`) are reported as "Unknown function" despite `MASTER_STATUS.md` and `PHASE_2_COMPLETE_2026_01_19.md` claiming "Regex Module - ALL WORKING ✅" and names matching `_impl.cpp`. This implies the entire regex module is non-functional. |
```

**Updated Entry:**
```markdown
| ISS-009 | Ch 08 | `regex` module (`match`, `search`, etc.) | ❌ PARTIAL (2 separate issues) | Regex module has TWO distinct problems verified 2026-01-22: <br>**Issue 1:** `re.match()` cannot be called because `match` is a reserved keyword in NAAb. Parser error: "Expected member name, Got: 'match'". **Fix:** Rename function in `regex_impl.cpp` to `matches()` or `regex_match()`. <br>**Issue 2:** Functions that CAN be called return incorrect results: `re.search()` returns false for valid patterns, `re.find_all()` returns empty arrays. Only `re.replace()` works correctly. **Fix:** Debug search/find_all implementations. See `test_ISS_009_regex_v2.naab`. |
```

**Evidence:**
- Test file: `test_ISS_009_regex.naab` - Shows keyword conflict
- Test file: `test_ISS_009_regex_v2.naab` - Shows buggy results
- re.search("Hello World", "World") → false (WRONG, should find match)
- re.find_all("Hello World", "\\w+") → [] (WRONG, should find words)
- re.replace("Hello World", "World", "Universe") → "Universe" ✓ (WORKS)

---

### ISS-016: String Escape Sequences

**Current Entry in ISSUES.md:**
```markdown
| ISS-016 | ALL | Parser Bug: `\n` in String Literal | ❌ Failed | The NAAb parser fails with `Error: Unexpected character '\'` when `\n` is present in a string literal, including within polyglot blocks. This makes it impossible to define multi-line strings or newlines within string output. This issue prevents proper formatting of complex strings. |
```

**Updated Entry:**
```markdown
| ISS-016 | ALL | String Escape Sequences Not Interpreted | ❌ Failed (different from original report) | The parser does NOT fail - code with `\n` parses successfully. However, escape sequences are NOT interpreted at runtime. String `"Line 1\nLine 2"` outputs as literal `Line 1\nLine 2` (with visible backslash-n) instead of two lines. Verified 2026-01-22. **Root Cause:** `readString()` in `lexer.cpp` (~line 185) accepts escape sequences but doesn't convert them. **Fix:** Add escape sequence interpretation: `\n` → newline, `\t` → tab, `\"` → quote, `\\` → backslash. See `test_ISS_016_string_escapes.naab`. |
```

**Evidence:**
- Test file: `test_ISS_016_string_escapes.naab` - Runs without parse error
- Output: `Line 1\nLine 2` (literal, not interpreted)
- Expected: Two separate lines

---

### ISS-005: JS Polyglot Return Value

**Current Entry in ISSUES.md:**
```markdown
| ISS-005 | Ch 05 | JS Polyglot Return Value | ❌ Failed | Capturing return from `<<javascript>>` block results in `null` when an expected value (string, array) is returned. This happens even when assigning to a nullable type (string?, array?). This indicates a bug in JS executor's return value handling/conversion. |
```

**Updated Entry:**
```markdown
| ISS-005 | Ch 05 | JS Polyglot Return Value | ❌ CONFIRMED (P0 - Critical) | JavaScript blocks always return `null` instead of actual return values. Verified 2026-01-22. Test: `let greeting = <<javascript "Hello from JavaScript" >>` results in type inference error because value is null. **Root Cause:** JS executor not capturing/converting return values properly. **Impact:** Blocks major polyglot feature. **Priority:** P0 - Fix immediately. See `test_ISS_005_js_return.naab`. |
```

**Evidence:**
- Test file: `test_ISS_005_js_return.naab` - CONFIRMED
- Error: "Type inference error: Cannot infer type for variable 'greeting' from 'null'"
- Expected: Should capture string "Hello from JavaScript"

---

### ISS-006: C++ Polyglot Return Value

**Current Entry in ISSUES.md:**
```markdown
| ISS-006 | Ch 05 | C++ Polyglot Return Value | ❌ Failed | C++ return value block fails compilation due to missing `std` headers (same root cause as ISS-004). This then leads to a null safety error when trying to assign the resulting `null` to a non-nullable `float`. |
```

**Updated Entry:**
```markdown
| ISS-006 | Ch 05 | C++ Polyglot Return Value | ❌ CONFIRMED (P0 - Critical) | C++ blocks always return `null` instead of actual return values. Verified 2026-01-22. Test: `let result = <<cpp return 3.14159; >>` results in type inference error because value is null. Even simple return statements without std headers return null. **Root Cause:** C++ executor not capturing/converting return values properly (separate from ISS-004 std headers issue). **Impact:** Blocks major polyglot feature. **Priority:** P0 - Fix immediately. See `test_ISS_006_cpp_return.naab`. |
```

**Evidence:**
- Test file: `test_ISS_006_cpp_return.naab` - CONFIRMED
- Error: "Type inference error: Cannot infer type for variable 'result' from 'null'"
- Expected: Should capture float 3.14159

---

### ISS-013: Block Registry CLI

**Current Entry in ISSUES.md:**
```markdown
| ISS-013 | Ch 10 | Block Registry CLI (`info`, `search`) | ❌ Partial | `naab-lang blocks info` command is missing/unimplemented in `src/cli/main.cpp`. `naab-lang blocks search` returns 0 results for broad terms, suggesting search index issues despite `list` showing 24k+ blocks. This contradicts documentation (`BLOCK_SYSTEM_QUICKSTART.md`, `TUTORIAL_BLOCK_ASSEMBLY.naab`). |
```

**Updated Entry:**
```markdown
| ISS-013 | Ch 10 | Block Registry CLI (`info`, `search`) | ❌ CONFIRMED | Verified 2026-01-22: <br>**1.** `naab-lang blocks list` ✓ Works (shows 24,487 blocks) <br>**2.** `naab-lang blocks info` ❌ "Unknown blocks subcommand: info" - Command not implemented <br>**3.** `naab-lang blocks search "sort"` ❌ Returns "No blocks found" for all queries (tested: sort, array, vector) - Search index appears broken or not built. **Root Cause:** `info` command missing from CLI, search functionality broken. **Fix:** Implement `info` command and debug search index. See `test_ISS_013_blocks_cli.md`. |
```

**Evidence:**
- Test file: `test_ISS_013_blocks_cli.md` - Manual CLI testing
- blocks list → 24,487 blocks ✓
- blocks info BLOCK-CPP-00001 → "Unknown subcommand"
- blocks search "sort" → "No blocks found"

---

## Issues That Remain Broken (Confirmed) (6 issues)

### ISS-001: Generics Instantiation

**Current Entry in ISSUES.md:**
```markdown
| ISS-001 | Ch 02 | Generics Instantiation | ❌ Failed | `new Box<int>` causes parse error: `Expected '{' for struct literal. Got: '<'`. Generics parsing is implemented but instantiation syntax is not supported in `parsePrimary`. |
```

**Updated Entry:**
```markdown
| ISS-001 | Ch 02 | Generics Instantiation | ❌ CONFIRMED | Generic struct instantiation syntax `new Box<int> { ... }` fails with parse error: "Expected '{' for struct literal. Got: '<'". Verified 2026-01-22. **Root Cause:** Parser's `parsePrimary()` doesn't handle type parameters in `new` expressions. **Workaround:** Use type inference without explicit type parameters (if supported). **Priority:** P2 (advanced feature). See `test_ISS_001_generics.naab`. |
```

---

### ISS-002: Function Type Annotation

**Current Entry in ISSUES.md:**
```markdown
| ISS-002 | Ch 04 | Function Type Annotation | ❌ Failed | `function` keyword as a type is not recognized. `USER_GUIDE.md` mentions `function<T...>` but the parser does not implement this in `parseBaseType`. Use `any` or omit type for now. |
```

**Updated Entry:**
```markdown
| ISS-002 | Ch 04 | Function Type Annotation | ❌ CONFIRMED | Function type annotation `let callback: function = ...` fails with parse error: "Expected type name. Got: 'function'". Verified 2026-01-22. **Root Cause:** `function` not recognized as a type keyword in parser's `parseBaseType()`. **Workaround:** Use `any` type for function parameters/variables. **Priority:** P3 (workaround available). See `test_ISS_002_function_type.naab`. |
```

---

### ISS-003: Pipeline Operator

**Current Entry in ISSUES.md:**
```markdown
| ISS-003 | Ch 04 | Pipeline Operator | ❌ Buggy | `|>` is implemented but has two critical issues: <br>1. **Parser:** Newline sensitivity causes parse errors if operator is on a new line. <br>2. **Interpreter:** Execution fails to consume the `returning_` flag after executing the piped function. This causes the *enclosing* function (e.g., `main`) to terminate immediately, skipping subsequent code. |
```

**Updated Entry:**
```markdown
| ISS-003 | Ch 04 | Pipeline Operator | ❌ CONFIRMED (newline issue) | Pipeline operator has confirmed newline sensitivity bug. Verified 2026-01-22. Code `let x = 20\n    |> fn1\n    |> fn2` fails with parse error: "Unexpected token in expression. Got: '|>'". Pipeline works on same line. **Root Cause:** Parser doesn't allow `|>` at start of new line. **Note:** Could not test `returning_` flag issue due to parse error blocking execution. **Priority:** P1 (breaks advertised feature). **Fix:** Update parser to handle newlines before `|>`. See `test_ISS_003_pipeline.naab`. |
```

---

### ISS-004: C++ Polyglot std Headers

**Current Entry in ISSUES.md:**
```markdown
| ISS-004 | Ch 05 | C++ Polyglot with `std` headers | ❌ Failed | All `<<cpp>>` blocks (with or without variable binding) fail compilation when `std` features (like `std::cout`) are used, due to missing header injection. The fix documented in `SESSION_2026_01_20_BUG_FIXES.md` (automatic header injection) is not working correctly. Users are still required to manually include headers. This contradicts documentation (`BLOCK_SYSTEM_QUICKSTART.md`, `QUICK_REFERENCE.naab`, `AI_ASSISTANT_GUIDE.md`) that claims "works without manual headers". |
```

**Updated Entry:**
```markdown
| ISS-004 | Ch 05 | C++ Polyglot with `std` headers | ❌ CONFIRMED | C++ blocks using `std::cout`, `std::vector`, etc. fail compilation with "no type named 'cout' in namespace 'std'". Verified 2026-01-22. First block using `std::cout` works (cached), but second block with `std::vector` fails. **Root Cause:** Automatic header injection not working - headers not being added to generated C++ code. **Impact:** Documentation claims "works without manual headers" but users must manually add `#include` directives. **Priority:** P1 (breaks polyglot usability). **Fix:** Implement automatic injection of `#include <iostream>`, `#include <vector>`, etc. in C++ executor. See `test_ISS_004_cpp_std.naab`. |
```

---

### ISS-010: IO Module Console Functions

**Current Entry in ISSUES.md:**
```markdown
| ISS-010 | Ch 09 | `io` module (`write`, `read_line`, etc.) | ❌ Missing/Redundant | Console I/O functions (`io.write`, `io.read_line`, `io.write_error`) are not implemented in `IOModule`. The `io` module currently only exposes file-related functions (`read_file`, `write_file`, `exists`, `list_dir`), which are redundant with `file` module. Documentation (`AI_ASSISTANT_GUIDE.md`) shows `io.read` and `io.write` for console/file. |
```

**Updated Entry:**
```markdown
| ISS-010 | Ch 09 | `io` module (`write`, `read_line`, etc.) | ❌ CONFIRMED | Console I/O functions are missing from `io` module. Verified 2026-01-22. Test of `io.write()` fails with "Unknown io function: write". Module only has file operations (redundant with `file` module), no console I/O. **Missing Functions:** `io.write()`, `io.read_line()`, `io.write_error()`. **Workaround:** Use `print()` for console output. **Priority:** P1 (missing documented feature). **Fix:** Add console I/O functions to IOModule implementation. See `test_ISS_010_io_console.naab`. |
```

---

### ISS-017: Parser Error Reporting

**Current Entry in ISSUES.md:**
```markdown
| ISS-017 | ALL | Parser Bug: Misleading Error Report | ❌ Critical | The NAAb parser reports `Error: Unexpected character '\'` at line 37, column 43, even when that exact line/column contains no backslash, or the relevant code block has been completely removed. This indicates a fundamental bug in the parser's error reporting mechanism, making effective debugging impossible. This is a critical blocker for writing any non-trivial NAAb code. |
```

**Updated Entry:**
```markdown
| ISS-017 | ALL | Parser Error Reporting | ⚠️ NEEDS MORE INVESTIGATION | Initial test (2026-01-22) did NOT reproduce the issue as described. Simple syntax error was reported at approximately correct location (line 15 actual vs line 13 where error occurred). The original issue claims errors reported at non-existent lines (line 37 in a shorter file). **Status:** Cannot confirm or deny with current test. **Recommendation:** Collect more examples of misleading error reports to identify pattern. **Priority:** P0 if reproducible (blocks debugging). See `test_ISS_017_parser_error.naab`. |
```

---

## Summary of Changes

### Status Updates
- **Mark ✅ RESOLVED:** ISS-014, ISS-015
- **Update to ✅ WORKS (docs issue):** ISS-007, ISS-008
- **Update description:** ISS-009, ISS-016, ISS-005, ISS-006, ISS-013
- **Add verification date:** ISS-011, ISS-012
- **Confirm broken:** ISS-001, ISS-002, ISS-003, ISS-004, ISS-010
- **Needs investigation:** ISS-017

### Priority Classification
Issues should be prioritized as:
- **P0 (Critical):** ISS-005, ISS-006, ISS-016, (ISS-017 if confirmed)
- **P1 (High):** ISS-003, ISS-004, ISS-009, ISS-010
- **P2 (Medium):** ISS-001, ISS-013
- **P3 (Low):** ISS-002

### Documentation Updates Needed
1. **AI_ASSISTANT_GUIDE.md line 1121:**
   - Change: `❌ Range operator? NOT AVAILABLE` → `✅ Range operator? AVAILABLE`
   - Change: `❌ Time Module for Benchmarking` → `✅ Time Module AVAILABLE`

2. **QUICK_REFERENCE.naab:**
   - Add range operator examples: `for i in 0..10 { ... }`

3. **All documentation mentioning array HOFs:**
   - Use `map_fn`, `filter_fn`, `reduce_fn` (not map, filter, reduce)

4. **Collections documentation:**
   - Show reassignment pattern: `set = coll.set_add(set, value)`

---

## Verification Evidence

All test files are located in:
```
/data/data/com.termux/files/home/.naab/language/docs/book/verification/solve_ISSUES/
```

Test files created:
- 17 executable .naab test files (one per issue)
- 1 CLI test documentation file
- 4 summary/analysis documents

Each issue can be re-verified by running its corresponding test file.

---

**Document Created:** 2026-01-22
**Purpose:** Provide verified updates for ISSUES.md
**Test Coverage:** 17/17 issues (100%)
**Ready for:** Another LLM to review and update ISSUES.md
