# Phase 2.4.5: Null Safety - Implementation Plan

**Status**: READY TO IMPLEMENT
**Priority**: HIGH (Critical for v1.0)
**Estimated Time**: 3-4 days
**Date**: 2026-01-17

---

## Overview

Implement **non-nullable by default** null safety for NAAb, making types safe unless explicitly marked with `?`.

**Key Features**:
1. Non-nullable types by default (`string` cannot be null)
2. Explicit nullable types (`string?` can be null)
3. Runtime null checks and validation
4. Clear error messages

---

## Implementation Strategy

Since NAAb is an **interpreted language** without a separate compile-time type checker, we'll implement null safety as **runtime validation** (similar to Phase 2.4.2 union types).

**Three-Layer Approach**:
1. **Parser**: Ensure `?` nullable marker is properly parsed (already works)
2. **Runtime**: Validate null assignments and accesses in interpreter
3. **Messages**: Provide clear error messages

**Defer to Future**:
- Flow-sensitive analysis (type narrowing after null checks)
- Optional chaining (`?.`)
- Null coalescing (`??`)
- Migration tool

---

## Phase 1: Verify Parser Support (30 minutes)

### Check Current State

**Files to Check**:
- `include/naab/ast.h` - Type struct
- `src/parser/parser.cpp` - parseType() function

**Verify**:
1. `Type` struct has `is_nullable` field
2. Parser recognizes `?` after types
3. Default `is_nullable = false`

**Tests**:
```naab
let x: int       // is_nullable = false
let y: int?      // is_nullable = true
let z: string?   // is_nullable = true
```

---

## Phase 2: Runtime Null Validation (2 days)

### 2.1: Null Assignment Validation (4 hours)

**Location**: `src/interpreter/interpreter.cpp` - `visit(VarDeclStmt&)`

**Implementation**:
```cpp
void Interpreter::visit(ast::VarDeclStmt& node) {
    auto value = eval(*node.getInit());

    // Phase 2.4.5: Null safety validation
    if (node.getType().has_value()) {
        ast::Type declared_type = node.getType().value();

        // Cannot assign null to non-nullable
        if (!declared_type.is_nullable && isNull(value)) {
            throw std::runtime_error(
                "Null safety error: Cannot assign null to non-nullable variable '" +
                node.getName() + "' of type " + formatTypeName(declared_type) +
                "\n  Help: Change to nullable type if null values are expected: " +
                formatTypeName(declared_type) + "?"
            );
        }
    }

    current_env_->define(node.getName(), value);
}
```

**Helper Method**:
```cpp
bool Interpreter::isNull(const std::shared_ptr<Value>& value) {
    // Check if value represents null
    return std::holds_alternative<std::monostate>(value->data) ||
           !value; // nullptr shared_ptr
}
```

**Test Cases**:
```naab
let x: int = 42         // ✅ OK
let y: int? = null      // ✅ OK
let z: int = null       // ❌ ERROR: Cannot assign null to non-nullable
```

### 2.2: Null Access Validation (4 hours)

**Location**: `src/interpreter/interpreter.cpp` - `visit(MemberExpr&)`

**Implementation**:
```cpp
void Interpreter::visit(ast::MemberExpr& node) {
    auto obj = eval(*node.getObject());

    // Phase 2.4.5: Null access check
    if (isNull(obj)) {
        // Get expected type of object
        // (would need type tracking - defer to phase 2.4.4)

        throw std::runtime_error(
            "Null safety error: Accessing member '" + node.getMember() +
            "' on null object" +
            "\n  Help: Add null check before accessing members:" +
            "\n    if (obj != null) { obj." + node.getMember() + " }"
        );
    }

    // ... rest of member access logic
}
```

**Test Cases**:
```naab
let x: string? = null
print(x.length)     // ❌ ERROR: Accessing member on null object
```

### 2.3: Function Parameter Validation (2 hours)

**Location**: `src/interpreter/interpreter.cpp` - `visit(CallExpr&)`

**Implementation**:
```cpp
// In function call handling
for (size_t i = 0; i < args.size(); i++) {
    const ast::Type& param_type = func->param_types[i];

    // Phase 2.4.5: Null safety for parameters
    if (!param_type.is_nullable && isNull(args[i])) {
        throw std::runtime_error(
            "Null safety error: Cannot pass null to non-nullable parameter '" +
            func->params[i] + "' of function '" + func->name + "'" +
            "\n  Expected: " + formatTypeName(param_type) +
            "\n  Got: null" +
            "\n  Help: Change parameter to nullable: " + formatTypeName(param_type) + "?"
        );
    }
}
```

**Test Cases**:
```naab
fn greet(name: string) {
    print("Hello, ", name)
}

greet("Alice")  // ✅ OK
greet(null)     // ❌ ERROR: Cannot pass null to non-nullable parameter
```

### 2.4: Function Return Validation (2 hours)

**Location**: `src/interpreter/interpreter.cpp` - `visit(ReturnStmt&)`

**Implementation**:
```cpp
void Interpreter::visit(ast::ReturnStmt& node) {
    if (node.getExpr()) {
        result_ = eval(*node.getExpr());
    } else {
        result_ = std::make_shared<Value>();  // void return
    }

    // Phase 2.4.5: Null safety for returns
    if (current_function_) {
        const ast::Type& return_type = current_function_->return_type;

        if (!return_type.is_nullable && isNull(result_)) {
            throw std::runtime_error(
                "Null safety error: Cannot return null from function '" +
                current_function_->name + "' with non-nullable return type " +
                formatTypeName(return_type) +
                "\n  Help: Change return type to nullable: " +
                formatTypeName(return_type) + "?"
            );
        }
    }

    returning_ = true;
}
```

**Test Cases**:
```naab
fn getValue() -> int {
    return null  // ❌ ERROR: Cannot return null from non-nullable function
}

fn getNullable() -> int? {
    return null  // ✅ OK
}
```

### 2.5: Struct Field Validation (1 hour)

**Location**: `src/interpreter/interpreter.cpp` - `visit(StructLiteralExpr&)`

**Already implemented in Phase 2.4.2** - just need to ensure it handles nullable types.

**Verify**:
```naab
struct Config {
    port: int      // Non-nullable
    host: string?  // Nullable
}

Config { port: 8080, host: "localhost" }  // ✅ OK
Config { port: null, host: "localhost" }  // ❌ ERROR
Config { port: 8080, host: null }         // ✅ OK
```

---

## Phase 3: Helper Methods (1 day)

### 3.1: isNull() Helper (30 minutes)

**Location**: `src/interpreter/interpreter.cpp`

**Add to header** (`include/naab/interpreter.h`):
```cpp
// Phase 2.4.5: Null safety helpers
bool isNull(const std::shared_ptr<Value>& value);
```

**Implementation**:
```cpp
bool Interpreter::isNull(const std::shared_ptr<Value>& value) {
    if (!value) return true;  // nullptr shared_ptr

    // Check for null literal value
    // NAAb represents null as... (need to check Value implementation)
    // Placeholder logic:
    if (std::holds_alternative<std::monostate>(value->data)) {
        return true;
    }

    return false;
}
```

### 3.2: Update formatTypeName() for Nullable (1 hour)

**Location**: Already exists from Phase 2.4.2

**Update**:
```cpp
std::string Interpreter::formatTypeName(const ast::Type& type) {
    std::string base_name;

    // ... existing type formatting ...

    // Phase 2.4.5: Add nullable marker
    if (type.is_nullable) {
        base_name += "?";
    }

    return base_name;
}
```

**Output**:
- `int` → "int"
- `int?` → "int?"
- `string | int` → "string | int"
- `(string | int)?` → "(string | int)?"

---

## Phase 4: Testing (1 day)

### 4.1: Create Test Files (2 hours)

**File**: `examples/test_phase2_4_5_null_safety.naab`

```naab
// Phase 2.4.5: Null Safety Tests

// Test 1: Non-nullable assignment
fn testNonNullable() {
    print("\n[Test 1] Non-nullable types")

    let x: int = 42
    print("x = ", x)

    // This should error:
    // let y: int = null  // ERROR
}

// Test 2: Nullable assignment
fn testNullable() {
    print("\n[Test 2] Nullable types")

    let x: int? = null
    print("x is null: ", x == null)

    x = 42
    print("x = ", x)
}

// Test 3: Function parameters
fn greet(name: string) {
    print("Hello, ", name)
}

fn greetMaybe(name: string?) {
    if (name != null) {
        print("Hello, ", name)
    } else {
        print("Hello, stranger")
    }
}

fn testParameters() {
    print("\n[Test 3] Function parameters")

    greet("Alice")
    // greet(null)  // Should error

    greetMaybe("Bob")
    greetMaybe(null)  // OK
}

// Test 4: Return types
fn getValue() -> int {
    return 42
    // return null  // Should error
}

fn getNullable() -> int? {
    return null  // OK
}

fn testReturns() {
    print("\n[Test 4] Return types")

    let val1 = getValue()
    print("getValue: ", val1)

    let val2 = getNullable()
    print("getNullable: null")
}

main {
    print("=== Phase 2.4.5: Null Safety Tests ===")

    testNonNullable()
    testNullable()
    testParameters()
    testReturns()

    print("\n=== All tests passed ===")
}
```

**File**: `examples/test_null_error.naab`

```naab
// Test error detection

fn test() {
    let x: int = null  // Should error
}

main {
    test()
}
```

### 4.2: Test Error Cases (2 hours)

Test each error condition:
1. Null to non-nullable variable
2. Null to non-nullable parameter
3. Null from non-nullable function
4. Null access on member

### 4.3: Integration Tests (2 hours)

Test interaction with:
- Generics: `Box<int?>` vs `Box<int>?`
- Union types: `(int | string)?`
- Struct fields: nullable and non-nullable

---

## Phase 5: Documentation (0.5 days)

### 5.1: Create Completion Document

**File**: `PHASE_2_4_5_COMPLETE.md`

Include:
- Implementation summary
- Test results
- Examples
- Known limitations

### 5.2: Update Master Status

**File**: `MASTER_STATUS.md`

Mark Phase 2.4.5 as complete.

---

## Implementation Order

### Day 1: Foundation
1. ✅ Verify parser support for nullable types
2. ⏳ Implement isNull() helper
3. ⏳ Update formatTypeName() for nullable
4. ⏳ Implement null assignment validation

### Day 2: Validation
5. ⏳ Implement null access validation
6. ⏳ Implement parameter validation
7. ⏳ Implement return validation
8. ⏳ Verify struct field validation

### Day 3: Testing
9. ⏳ Create test files
10. ⏳ Test success cases
11. ⏳ Test error cases
12. ⏳ Integration tests

### Day 4: Documentation
13. ⏳ Write completion document
14. ⏳ Update master status
15. ⏳ Create examples

---

## Success Criteria

**Must Have**:
- ✅ Cannot assign null to non-nullable types
- ✅ Can assign null to nullable types
- ✅ Clear error messages with helpful suggestions
- ✅ Works with functions, structs, variables

**Nice to Have** (Future):
- ⏳ Type narrowing after null checks
- ⏳ Optional chaining (`?.`)
- ⏳ Null coalescing (`??`)

---

## Risks & Mitigation

**Risk 1**: Parser doesn't support `?` properly
- **Mitigation**: Check and fix parser first
- **Likelihood**: Low (should already work)

**Risk 2**: Null representation in Value unclear
- **Mitigation**: Examine Value struct implementation
- **Likelihood**: Medium

**Risk 3**: Integration with union types complex
- **Mitigation**: Test thoroughly, leverage Phase 2.4.2 code
- **Likelihood**: Low

---

## Next Steps After Completion

With Phase 2.4.5 complete, the **core type system** will be done:
- ✅ Phase 2.4.1: Generics
- ✅ Phase 2.4.2: Union Types
- ✅ Phase 2.4.3: Enums
- ⏳ Phase 2.4.4: Type Inference (optional)
- ⏳ Phase 2.4.5: Null Safety

**Then proceed to**:
- Phase 3: Runtime optimizations
- Phase 4: Tooling (LSP, formatter)
- Phase 5: Standard library

---

**Created**: 2026-01-17
**Ready to Implement**: Yes
**Estimated Completion**: 3-4 days
