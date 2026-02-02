# Bounds Validation Audit

**Week 5, Task 5.1: Comprehensive Bounds Validation Audit**
**Status:** In Progress
**Date:** 2026-01-30

---

## Overview

This audit identifies all array/vector accesses in the NAAb codebase and ensures proper bounds checking. Unchecked accesses can lead to:
- Buffer overflows
- Segmentation faults
- Undefined behavior
- Memory corruption
- Security vulnerabilities

## Methodology

1. **Search Phase:** Identify all uses of `operator[]` on containers
2. **Analysis Phase:** Determine if bounds checking is present
3. **Remediation Phase:** Replace unsafe accesses with safe alternatives
4. **Testing Phase:** Add boundary condition tests

---

## Unsafe Patterns

### Pattern 1: Direct Vector Access Without Bounds Check

**Unsafe:**
```cpp
std::vector<Token> tokens_;

Token current() {
    return tokens_[pos_];  // ❌ No bounds check
}
```

**Safe Alternative 1 (Using .at()):**
```cpp
Token current() {
    return tokens_.at(pos_);  // ✅ Throws std::out_of_range
}
```

**Safe Alternative 2 (Manual Check):**
```cpp
Token current() {
    if (pos_ >= tokens_.size()) {
        throw std::out_of_range("Token position out of bounds");
    }
    return tokens_[pos_];  // ✅ Checked
}
```

**Safe Alternative 3 (Using safe_math):**
```cpp
#include "naab/safe_math.h"

Token current() {
    math::checkArrayBounds(pos_, tokens_.size(), "Token access");
    return tokens_[pos_];  // ✅ Checked
}
```

### Pattern 2: Function Parameter Array Access

**Unsafe:**
```cpp
void defineParam(const std::vector<std::string>& params, size_t i) {
    std::string param_name = params[i];  // ❌ No bounds check
}
```

**Safe:**
```cpp
void defineParam(const std::vector<std::string>& params, size_t i) {
    if (i >= params.size()) {
        throw std::out_of_range(
            fmt::format("Parameter index {} out of bounds (size: {})", i, params.size())
        );
    }
    std::string param_name = params[i];  // ✅ Checked
}
```

### Pattern 3: Loop-Based Access

**Unsafe:**
```cpp
for (size_t i = 0; i < items.size(); i++) {
    process(items[i]);  // ❌ Logically safe but not enforced
}
```

**Safe (Best Practice):**
```cpp
// Option 1: Range-based for loop (no indexing)
for (const auto& item : items) {
    process(item);  // ✅ No bounds issues
}

// Option 2: If index is needed, use .at()
for (size_t i = 0; i < items.size(); i++) {
    process(items.at(i));  // ✅ Bounds checked
}
```

### Pattern 4: String Indexing

**Unsafe:**
```cpp
std::string str = "hello";
char first = str[0];  // ❌ No bounds check (crashes on empty string)
```

**Safe:**
```cpp
std::string str = "hello";
if (!str.empty()) {
    char first = str[0];  // ✅ Checked
} else {
    // Handle empty string
}

// Or use .at()
char first = str.at(0);  // ✅ Throws on out of bounds
```

---

## Audit Results by File

### 1. src/parser/parser.cpp

#### Finding 1: Token Access (Lines 98, 106)

**Location:** `Parser::current()`, `Parser::peek()`

**Current Code:**
```cpp
Token Parser::current() {
    return tokens_[pos_];  // Line 98
}

Token Parser::peek(size_t lookahead) {
    size_t peek_pos = pos_ + lookahead;
    return tokens_[peek_pos];  // Line 106
}
```

**Risk:** High - Can crash if `pos_` exceeds `tokens_.size()`

**Recommendation:**
```cpp
Token Parser::current() {
    if (pos_ >= tokens_.size()) {
        throw ParserException("Unexpected end of input");
    }
    return tokens_[pos_];
}

Token Parser::peek(size_t lookahead) {
    size_t peek_pos = pos_ + lookahead;
    if (peek_pos >= tokens_.size()) {
        // Return EOF token instead of crashing
        return Token{TokenType::EOF_TOKEN, "", location_};
    }
    return tokens_[peek_pos];
}
```

#### Finding 2: String Character Access (Lines 1461, 1677)

**Location:** Variable name parsing, type checking

**Current Code:**
```cpp
// Line 1461
while (start < var_list.size() && (var_list[start] == ' ' || var_list[start] == '\t')) {
    start++;
}

// Line 1677
if (module_prefix.empty() && type_name.length() == 1 && std::isupper(type_name[0])) {
    // ...
}
```

**Risk:** Low - Bounds checked in condition (line 1461), checked for size (line 1677)

**Recommendation:** Already safe - bounds are checked before access

---

### 2. src/interpreter/interpreter.cpp

#### Finding 1: Function Parameter Access (Lines 464, 480, 485, 488, 491)

**Location:** `Interpreter::visitFunctionCall()` - Parameter binding

**Current Code:**
```cpp
// Line 464
if (!func->defaults[i]) {
    // ...
}

// Line 480
func_env->define(func->params[i], args[i]);

// Line 485
if (func->defaults[i]) {
    // ...
}

// Line 488
func->defaults[i]->accept(*this);

// Line 491
func_env->define(func->params[i], default_val);
```

**Risk:** Medium - Assumes `i` is within bounds of `params`, `defaults`, and `args`

**Analysis:** Need to check surrounding code for loop bounds

**Recommendation:**
```cpp
// Before the loop accessing these arrays:
if (i >= func->params.size()) {
    throw RuntimeException(
        fmt::format("Internal error: parameter index {} out of bounds", i)
    );
}

if (i >= args.size()) {
    throw RuntimeException(
        fmt::format("Not enough arguments: expected {}, got {}",
                   func->params.size(), args.size())
    );
}

// Then proceed with access
func_env->define(func->params.at(i), args.at(i));
```

#### Finding 2: Struct Field Access (Lines 137, 138)

**Location:** Value toString for structs

**Current Code:**
```cpp
// Line 137
result += arg->definition->fields[i].name + ": ";

// Line 138
result += arg->field_values[i]->toString();
```

**Risk:** Medium - Assumes `i` is within bounds

**Recommendation:**
```cpp
// Add bounds check in loop
for (size_t i = 0; i < arg->field_values.size(); i++) {
    // Verify fields array is in sync
    if (i >= arg->definition->fields.size()) {
        throw RuntimeException("Struct field count mismatch");
    }

    result += arg->definition->fields.at(i).name + ": ";
    result += arg->field_values.at(i)->toString();
}
```

#### Finding 3: List Element Access (Line 114)

**Location:** Value toString for lists

**Current Code:**
```cpp
// Line 114
result += arg[i]->toString();
```

**Risk:** Low - Likely within loop with proper bounds

**Recommendation:** Replace with `.at(i)` for safety

---

### 3. src/lexer/lexer.cpp

**Status:** Need to audit

**Priority:** High (lexer is first line of defense)

**Action Items:**
- Search for source string indexing: `source_[pos_]`
- Check lookahead operations
- Verify EOF handling

---

### 4. src/interpreter/evaluator.cpp

**Status:** Need to audit

**Priority:** High (core evaluation logic)

**Action Items:**
- Check array element access
- Check struct field access
- Verify collection indexing

---

### 5. Standard Library Modules

**Files to Audit:**
- `src/stdlib/array.cpp` - Array operations
- `src/stdlib/string.cpp` - String indexing
- `src/stdlib/io.cpp` - Buffer operations

**Priority:** High (user-facing operations)

---

## Safe Access Patterns

### Pattern 1: Use .at() for Single Accesses

```cpp
// ✅ Safe - throws std::out_of_range
auto value = vec.at(index);
```

**Pros:** Simple, standard, automatic bounds check
**Cons:** Exception overhead (negligible in practice)

### Pattern 2: Use safe_math::checkArrayBounds

```cpp
#include "naab/safe_math.h"

// ✅ Safe - throws std::out_of_range with context
math::checkArrayBounds(index, vec.size(), "Vector access");
auto value = vec[index];
```

**Pros:** Custom error messages, consistent with other checks
**Cons:** Two lines instead of one

### Pattern 3: Range-Based For Loops

```cpp
// ✅ Safe - no indexing
for (const auto& item : vec) {
    process(item);
}
```

**Pros:** No bounds issues, cleaner code, better performance
**Cons:** Can't get index easily

### Pattern 4: Iterator-Based Access

```cpp
// ✅ Safe - iterators handle bounds
for (auto it = vec.begin(); it != vec.end(); ++it) {
    process(*it);
}
```

**Pros:** Standard, safe, flexible
**Cons:** More verbose than range-based for

### Pattern 5: Manual Bounds Check

```cpp
// ✅ Safe - explicit check
if (index < vec.size()) {
    auto value = vec[index];
} else {
    // Handle out of bounds
}
```

**Pros:** Custom handling possible
**Cons:** Easy to forget, verbose

---

## Testing Strategy

### 1. Boundary Value Tests

Test all boundary conditions for array accesses:

```cpp
// Test file: tests/security/bounds_test.cpp

TEST(BoundsTest, EmptyVector) {
    std::vector<int> vec;

    // Should throw
    EXPECT_THROW(vec.at(0), std::out_of_range);
}

TEST(BoundsTest, SingleElement) {
    std::vector<int> vec = {42};

    // Valid access
    EXPECT_EQ(vec.at(0), 42);

    // Out of bounds
    EXPECT_THROW(vec.at(1), std::out_of_range);
    EXPECT_THROW(vec.at(-1), std::out_of_range);  // If signed index
}

TEST(BoundsTest, LargeIndex) {
    std::vector<int> vec = {1, 2, 3};

    // Way out of bounds
    EXPECT_THROW(vec.at(1000000), std::out_of_range);
}
```

### 2. Integration Tests

Test boundary conditions in NAAb code:

```naab
// tests/security/array_bounds_test.naab

fn testArrayBounds() {
    let arr = [1, 2, 3, 4, 5]

    // Valid access
    assert(arr[0] == 1, "First element")
    assert(arr[4] == 5, "Last element")

    // Out of bounds - should throw
    try {
        let x = arr[5]  // One past end
        assert(false, "Should have thrown")
    } catch e {
        print("✓ Caught out of bounds: " + e)
    }

    // Negative index - should throw
    try {
        let x = arr[-1]
        assert(false, "Should have thrown")
    } catch e {
        print("✓ Caught negative index: " + e)
    }
}
```

### 3. Fuzz Testing

Add bounds checking to fuzzing targets:

```cpp
// fuzz/fuzz_array_access.cpp

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < sizeof(size_t)) return 0;

    // Use fuzzer input as array index
    size_t index = *reinterpret_cast<const size_t*>(data);

    std::vector<int> vec = {1, 2, 3, 4, 5};

    try {
        // This should safely handle any index
        auto val = vec.at(index);
    } catch (const std::out_of_range&) {
        // Expected for out of bounds - good!
    }

    return 0;
}
```

---

## Remediation Priority

### 🔴 Critical (Fix Immediately)
1. **Parser token access** - Can crash on malformed input
2. **Lexer source access** - Can crash on edge cases
3. **Interpreter parameter binding** - Can corrupt memory

### 🟠 High (Fix This Week)
4. **Struct field access** - Can cause memory corruption
5. **Array module operations** - User-facing, security critical
6. **String module operations** - Common attack vector

### 🟡 Medium (Fix Next Sprint)
7. **Error formatting** - Can leak information
8. **Debug output** - Development-time issues
9. **Internal bookkeeping** - Lower risk

### 🟢 Low (Fix Eventually)
10. **Already-checked accesses** - Document as safe
11. **Bounded loops** - Logically safe but could use .at()
12. **Test code** - Less critical

---

## Automated Detection

### Static Analysis

Use clang-tidy to detect unchecked accesses:

```yaml
# .clang-tidy

Checks: >
  cppcoreguidelines-pro-bounds-constant-array-index,
  cppcoreguidelines-pro-bounds-array-to-pointer-decay

CheckOptions:
  - key: cppcoreguidelines-pro-bounds-constant-array-index.GslHeader
    value: "naab/safe_math.h"
```

Run with:
```bash
clang-tidy src/**/*.cpp -checks=cppcoreguidelines-pro-bounds-*
```

### Runtime Detection

Enable sanitizers to catch bounds violations:

```bash
# AddressSanitizer detects buffer overflows
cmake -B build-asan -DENABLE_ASAN=ON
cmake --build build-asan

# Run all tests
cd build-asan && ctest --output-on-failure
```

---

## Progress Tracking

### Audit Progress

- [ ] **Parser** (parser.cpp, ast_nodes.cpp)
  - [ ] Token access
  - [ ] String parsing
  - [ ] Type checking

- [ ] **Lexer** (lexer.cpp)
  - [ ] Source string access
  - [ ] Lookahead operations
  - [ ] Character classification

- [ ] **Interpreter** (interpreter.cpp, evaluator.cpp)
  - [ ] Function parameter binding
  - [ ] Struct field access
  - [ ] Array indexing

- [ ] **Standard Library** (stdlib/*.cpp)
  - [ ] Array module
  - [ ] String module
  - [ ] I/O operations

- [ ] **Polyglot** (polyglot/*.cpp)
  - [ ] FFI marshaling
  - [ ] Value conversion
  - [ ] Error handling

### Fix Progress

- [ ] All critical findings fixed
- [ ] All high priority findings fixed
- [ ] Tests added for boundary conditions
- [ ] Documentation updated
- [ ] Code review completed

---

## Best Practices

### 1. Always Use Bounds-Checked Access

**Default:** Use `.at()` for all container access
**Exception:** Only use `operator[]` after explicit bounds check

### 2. Prefer Range-Based Loops

**Avoid:**
```cpp
for (size_t i = 0; i < vec.size(); i++) {
    process(vec[i]);
}
```

**Prefer:**
```cpp
for (const auto& item : vec) {
    process(item);
}
```

### 3. Validate Sizes Before Access

```cpp
if (args.size() < required_count) {
    throw RuntimeException("Not enough arguments");
}

// Now safe to access args[0..required_count-1]
```

### 4. Use safe_math for Index Calculations

```cpp
// Instead of:
size_t index = base + offset;  // Can overflow

// Use:
size_t index = math::safeAdd(base, offset);
```

### 5. Document Preconditions

```cpp
/**
 * Get element at index
 * @pre index < elements.size()
 */
Value& get(size_t index) {
    assert(index < elements.size());  // Debug check
    return elements_[index];
}
```

---

## Next Steps

1. **Complete file-by-file audit** (in progress)
2. **Fix all critical findings** (by end of day 1)
3. **Fix all high priority findings** (by end of day 2)
4. **Add boundary tests** (throughout)
5. **Run static analysis** (end of day 2)
6. **Run fuzzing with sanitizers** (continuous)
7. **Document all changes** (continuous)

---

## Summary

Bounds validation audit identifies and remediates unsafe array/vector accesses. By systematically replacing unchecked accesses with safe alternatives, we eliminate buffer overflow vulnerabilities and undefined behavior.

**Target:** Zero unchecked container accesses in production code

**Status:** Audit in progress, critical fixes to be applied

---

**Next:** [Error Message Scrubbing](ERROR_SCRUBBING.md) (Task 5.2)
**Week 5:** [Testing & Hardening Summary](WEEK5_SUMMARY.md) (when complete)
