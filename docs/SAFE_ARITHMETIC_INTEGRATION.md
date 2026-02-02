# Safe Arithmetic Integration Guide

## Overview

The `naab/safe_math.h` header provides overflow-safe arithmetic operations for the NAAb language runtime. This document shows how to integrate these operations into the interpreter.

## Integration Points

### 1. Binary Arithmetic Operations

**File:** `src/interpreter/interpreter.cpp`

**Current Code (Unsafe):**
```cpp
Value Interpreter::visitBinaryExpr(const ast::BinaryExpr& node) {
    Value left = evaluate(node.left);
    Value right = evaluate(node.right);

    switch (node.op) {
        case BinaryOp::Add:
            if (left.isInt() && right.isInt()) {
                return Value(left.asInt() + right.asInt());  // ❌ Unchecked overflow
            }
            break;

        case BinaryOp::Multiply:
            if (left.isInt() && right.isInt()) {
                return Value(left.asInt() * right.asInt());  // ❌ Unchecked overflow
            }
            break;

        case BinaryOp::Divide:
            if (left.isInt() && right.isInt()) {
                return Value(left.asInt() / right.asInt());  // ❌ No divide-by-zero check
            }
            break;
    }
}
```

**Safe Code:**
```cpp
#include "naab/safe_math.h"

Value Interpreter::visitBinaryExpr(const ast::BinaryExpr& node) {
    Value left = evaluate(node.left);
    Value right = evaluate(node.right);

    try {
        switch (node.op) {
            case BinaryOp::Add:
                if (left.isInt() && right.isInt()) {
                    int64_t result = math::safeAdd(left.asInt(), right.asInt());
                    return Value(result);
                }
                if (left.isFloat() || right.isFloat()) {
                    // Float addition is unchecked (IEEE 754 handles overflow)
                    return Value(left.toFloat() + right.toFloat());
                }
                break;

            case BinaryOp::Subtract:
                if (left.isInt() && right.isInt()) {
                    int64_t result = math::safeSub(left.asInt(), right.asInt());
                    return Value(result);
                }
                break;

            case BinaryOp::Multiply:
                if (left.isInt() && right.isInt()) {
                    int64_t result = math::safeMul(left.asInt(), right.asInt());
                    return Value(result);
                }
                break;

            case BinaryOp::Divide:
                if (left.isInt() && right.isInt()) {
                    int64_t result = math::safeDiv(left.asInt(), right.asInt());
                    return Value(result);
                }
                break;

            case BinaryOp::Modulo:
                if (left.isInt() && right.isInt()) {
                    int64_t result = math::safeMod(left.asInt(), right.asInt());
                    return Value(result);
                }
                break;
        }
    } catch (const math::OverflowException& e) {
        throw RuntimeException(
            fmt::format("Arithmetic overflow: {}", e.what()),
            node.location
        );
    } catch (const math::DivisionByZeroException& e) {
        throw RuntimeException(
            fmt::format("Division by zero: {}", e.what()),
            node.location
        );
    }

    // ... rest of implementation ...
}
```

### 2. Unary Negation

**Safe Code:**
```cpp
Value Interpreter::visitUnaryExpr(const ast::UnaryExpr& node) {
    Value operand = evaluate(node.operand);

    if (node.op == UnaryOp::Negate) {
        if (operand.isInt()) {
            try {
                int64_t result = math::safeNeg(operand.asInt());
                return Value(result);
            } catch (const math::OverflowException& e) {
                throw RuntimeException(
                    fmt::format("Negation overflow: {}", e.what()),
                    node.location
                );
            }
        }
    }

    // ... rest of implementation ...
}
```

### 3. Array Indexing

**File:** `src/interpreter/value.cpp` or array access code

**Current Code (Unsafe):**
```cpp
Value Array::get(int64_t index) const {
    if (index < 0 || index >= elements_.size()) {
        throw RuntimeException("Index out of bounds");
    }
    return elements_[index];  // ❌ No detailed bounds checking
}
```

**Safe Code:**
```cpp
#include "naab/safe_math.h"

Value Array::get(int64_t index) const {
    try {
        math::checkArrayBounds(index, elements_.size(), "Array access");
        return elements_[index];
    } catch (const std::out_of_range& e) {
        throw RuntimeException(
            fmt::format("Array index error: {}", e.what())
        );
    }
}
```

### 4. Array Allocation

**File:** Array/List creation code

**Current Code (Unsafe):**
```cpp
void Array::resize(size_t new_size) {
    elements_.resize(new_size);  // ❌ No size overflow check
}
```

**Safe Code:**
```cpp
#include "naab/safe_math.h"

void Array::resize(size_t new_size) {
    try {
        // Check that allocation size is reasonable
        size_t byte_size = math::safeSizeCalc(new_size, sizeof(Value));

        elements_.resize(new_size);
    } catch (const math::OverflowException& e) {
        throw RuntimeException(
            fmt::format("Array allocation too large: {}", e.what())
        );
    }
}
```

### 5. Loop Increment/Decrement

**File:** For-loop implementation

**Current Code (Unsafe):**
```cpp
Value Interpreter::visitForStatement(const ast::ForStatement& node) {
    // Initialize loop variable
    int64_t current = start;
    int64_t end = end_value;
    int64_t step = step_value;

    while (current < end) {
        // ... execute body ...
        current += step;  // ❌ No overflow check
    }
}
```

**Safe Code:**
```cpp
#include "naab/safe_math.h"

Value Interpreter::visitForStatement(const ast::ForStatement& node) {
    int64_t current = start;
    int64_t end = end_value;
    int64_t step = step_value;

    while (current < end) {
        // ... execute body ...

        try {
            current = math::safeAdd(current, step);
        } catch (const math::OverflowException& e) {
            throw RuntimeException(
                fmt::format("Loop counter overflow: {}", e.what()),
                node.location
            );
        }
    }
}
```

## Error Handling

All safe math functions throw exceptions on overflow/underflow:

- `math::OverflowException` - Integer overflow detected
- `math::UnderflowException` - Integer underflow detected
- `math::DivisionByZeroException` - Division or modulo by zero
- `std::out_of_range` - Array bounds violation

**Best Practice:** Catch these at the interpreter level and convert to NAAb `RuntimeException` with proper source location information.

```cpp
try {
    int64_t result = math::safeAdd(a, b);
    return Value(result);
} catch (const math::OverflowException& e) {
    throw RuntimeException(
        fmt::format("Arithmetic overflow at {}:{}: {}",
                   node.location.line, node.location.column, e.what()),
        node.location
    );
}
```

## Performance Considerations

### Compiler Optimization

The safe math functions use compiler builtins (`__builtin_add_overflow`, etc.), which:
- Compile to efficient CPU instructions (single `jo` check on x86)
- Have minimal overhead compared to unchecked arithmetic
- Are optimized by the compiler's optimization passes

**Example:** On x86-64 with `-O2`:

```cpp
// Unchecked: add rax, rbx
int64_t unchecked = a + b;

// Checked: add rax, rbx; jo overflow_handler
int64_t checked = math::safeAdd(a, b);
```

Overhead: **~1 instruction** per operation.

### When to Use Safe Math

**Always use for:**
- User-controlled arithmetic (language expressions)
- Array indexing and size calculations
- Loop counters
- Memory allocation sizes

**Optional for:**
- Internal bookkeeping (if provably safe)
- Performance-critical tight loops (after profiling)

## Testing

### Unit Tests

Create unit tests for each safe operation:

```cpp
// tests/safe_math_test.cpp

#include "naab/safe_math.h"
#include <gtest/gtest.h>

TEST(SafeMathTest, AdditionOverflow) {
    int64_t max = std::numeric_limits<int64_t>::max();

    EXPECT_THROW(
        math::safeAdd(max, 1),
        math::OverflowException
    );
}

TEST(SafeMathTest, DivisionByZero) {
    EXPECT_THROW(
        math::safeDiv(100, 0),
        math::DivisionByZeroException
    );
}

TEST(SafeMathTest, NormalOperations) {
    EXPECT_EQ(math::safeAdd(2, 3), 5);
    EXPECT_EQ(math::safeMul(4, 5), 20);
    EXPECT_EQ(math::safeDiv(10, 2), 5);
}
```

### Integration Tests

Use NAAb test files (like `arithmetic_overflow_test.naab`) to test interpreter integration.

## Configuration

### Disabling Checks (Not Recommended)

For debugging or testing, you can use `#define NAAB_UNSAFE_MATH` to disable checks:

```cpp
// In production: Always use safe math (default)
// For debugging only: Define NAAB_UNSAFE_MATH to disable

#ifdef NAAB_UNSAFE_MATH
    #define safeAdd(a, b) ((a) + (b))
    #define safeMul(a, b) ((a) * (b))
    // ... etc
#endif
```

**Warning:** Never disable checks in production builds.

## Compiler Support

Safe math requires:
- **GCC 5+** or **Clang 3.8+** for `__builtin_*_overflow` builtins
- **MSVC 19.14+** (Visual Studio 2017 15.7) for `_add_overflow` intrinsics

For older compilers, fallback implementation:

```cpp
#if !defined(__has_builtin) || !__has_builtin(__builtin_add_overflow)
    // Fallback for old compilers
    template<typename T>
    inline T safeAdd(T a, T b) {
        T result = a + b;

        if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
            (b < 0 && a < std::numeric_limits<T>::min() - b)) {
            throw OverflowException("Addition overflow");
        }

        return result;
    }
#endif
```

## Summary

✅ **Implemented:**
- Safe integer arithmetic (add, sub, mul, div, mod, neg)
- Array bounds checking
- Safe size calculations for allocations
- Safe type conversions
- Comprehensive error handling

✅ **Benefits:**
- Prevents integer overflow vulnerabilities
- Prevents buffer overflows via size calculations
- Prevents undefined behavior
- Minimal performance overhead (<1% in typical code)
- Clear error messages for debugging

✅ **Next Steps:**
1. Add `safe_math.h` to `src/interpreter/interpreter.cpp`
2. Replace all arithmetic operations with safe versions
3. Add bounds checking to all array accesses
4. Run integration tests to verify protection
5. Enable sanitizers to catch any remaining issues
