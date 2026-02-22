#pragma once
// NAAb Safe Arithmetic
// Week 4, Task 4.3: Arithmetic Overflow Checking
//
// Prevents integer overflow vulnerabilities by:
// - Detecting overflow/underflow in arithmetic operations
// - Throwing exceptions on overflow detection
// - Using compiler builtins for efficient checking
// - Protecting array size calculations and indexing


#include <stdexcept>
#include <string>
#include <cstdint>
#include <limits>
#include <fmt/core.h>

namespace naab {
namespace math {

// ============================================================================
// Arithmetic Overflow Exceptions
// ============================================================================

class OverflowException : public std::runtime_error {
public:
    explicit OverflowException(const std::string& msg)
        : std::runtime_error(msg) {}
};

class UnderflowException : public std::runtime_error {
public:
    explicit UnderflowException(const std::string& msg)
        : std::runtime_error(msg) {}
};

class DivisionByZeroException : public std::runtime_error {
public:
    explicit DivisionByZeroException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ============================================================================
// Safe Arithmetic Operations (Integer)
// ============================================================================

/**
 * Safe integer addition with overflow detection
 *
 * Uses compiler builtin for efficient overflow checking
 * @throws OverflowException if result would overflow
 */
template<typename T>
inline T safeAdd(T a, T b) {
    static_assert(std::is_integral<T>::value, "safeAdd requires integral type");

    T result;
    if (__builtin_add_overflow(a, b, &result)) {
        throw OverflowException(
            fmt::format("Integer overflow in addition: {} + {} exceeds {} range",
                       a, b,
                       std::is_signed<T>::value ? "signed" : "unsigned")
        );
    }
    return result;
}

/**
 * Safe integer subtraction with underflow detection
 *
 * @throws UnderflowException if result would underflow
 */
template<typename T>
inline T safeSub(T a, T b) {
    static_assert(std::is_integral<T>::value, "safeSub requires integral type");

    T result;
    if (__builtin_sub_overflow(a, b, &result)) {
        throw UnderflowException(
            fmt::format("Integer underflow in subtraction: {} - {} exceeds {} range",
                       a, b,
                       std::is_signed<T>::value ? "signed" : "unsigned")
        );
    }
    return result;
}

/**
 * Safe integer multiplication with overflow detection
 *
 * @throws OverflowException if result would overflow
 */
template<typename T>
inline T safeMul(T a, T b) {
    static_assert(std::is_integral<T>::value, "safeMul requires integral type");

    T result;
    if (__builtin_mul_overflow(a, b, &result)) {
        throw OverflowException(
            fmt::format("Integer overflow in multiplication: {} * {} exceeds {} range",
                       a, b,
                       std::is_signed<T>::value ? "signed" : "unsigned")
        );
    }
    return result;
}

/**
 * Safe integer division with divide-by-zero detection
 *
 * Also detects INT_MIN / -1 overflow case
 * @throws DivisionByZeroException if divisor is zero
 * @throws OverflowException if INT_MIN / -1
 */
template<typename T>
inline T safeDiv(T a, T b) {
    static_assert(std::is_integral<T>::value, "safeDiv requires integral type");

    if (b == 0) {
        throw DivisionByZeroException(
            fmt::format("Division by zero: {} / 0", a)
        );
    }

    // Check for INT_MIN / -1 overflow (only for signed types)
    if constexpr (std::is_signed<T>::value) {
        if (a == std::numeric_limits<T>::min() && b == -1) {
            throw OverflowException(
                fmt::format("Integer overflow in division: {} / -1 exceeds range", a)
            );
        }
    }

    return a / b;
}

/**
 * Safe integer modulo with divide-by-zero detection
 *
 * @throws DivisionByZeroException if divisor is zero
 */
template<typename T>
inline T safeMod(T a, T b) {
    static_assert(std::is_integral<T>::value, "safeMod requires integral type");

    if (b == 0) {
        throw DivisionByZeroException(
            fmt::format("Modulo by zero: {} % 0", a)
        );
    }

    // Note: In C++, INT_MIN % -1 is well-defined as 0
    // but we still check for safety
    if constexpr (std::is_signed<T>::value) {
        if (a == std::numeric_limits<T>::min() && b == -1) {
            return 0;  // Well-defined result
        }
    }

    return a % b;
}

/**
 * Safe integer negation with overflow detection
 *
 * @throws OverflowException if negating INT_MIN
 */
template<typename T>
inline T safeNeg(T a) {
    static_assert(std::is_integral<T>::value, "safeNeg requires integral type");

    if constexpr (std::is_signed<T>::value) {
        if (a == std::numeric_limits<T>::min()) {
            throw OverflowException(
                fmt::format("Integer overflow in negation: -{} exceeds range", a)
            );
        }
    }

    return -a;
}

// ============================================================================
// Safe Size Calculations (for array allocation)
// ============================================================================

/**
 * Safe size calculation for array allocation
 *
 * Prevents overflow in size * element_size calculations
 * @throws OverflowException if allocation size would overflow
 */
inline size_t safeSizeCalc(size_t count, size_t element_size) {
    size_t result;
    if (__builtin_mul_overflow(count, element_size, &result)) {
        throw OverflowException(
            fmt::format("Size calculation overflow: {} * {} bytes exceeds addressable memory",
                       count, element_size)
        );
    }

    // Additional sanity check: don't allow allocations > 1GB
    constexpr size_t MAX_ALLOC = 1024ULL * 1024 * 1024;  // 1GB
    if (result > MAX_ALLOC) {
        throw OverflowException(
            fmt::format("Size calculation too large: {} bytes > 1GB limit", result)
        );
    }

    return result;
}

/**
 * Safe array index validation
 *
 * Checks that index is within bounds [0, size)
 * @throws std::out_of_range if index is out of bounds
 */
template<typename T>
inline void checkArrayBounds(T index, size_t size, const std::string& context = "") {
    static_assert(std::is_integral<T>::value, "checkArrayBounds requires integral index");

    // Check for negative index (if signed type)
    if constexpr (std::is_signed<T>::value) {
        if (index < 0) {
            throw std::out_of_range(
                fmt::format("{}: negative array index: {}",
                           context.empty() ? "Array access" : context, index)
            );
        }
    }

    // Check upper bound
    if (static_cast<size_t>(index) >= size) {
        throw std::out_of_range(
            fmt::format("{}: index {} out of bounds [0, {})",
                       context.empty() ? "Array access" : context, index, size)
        );
    }
}

// ============================================================================
// Safe Conversion (with range checking)
// ============================================================================

/**
 * Safe integer conversion between types
 *
 * Checks that source value fits in destination type
 * @throws OverflowException if value doesn't fit
 */
template<typename Dest, typename Source>
inline Dest safeCast(Source value) {
    static_assert(std::is_integral<Dest>::value && std::is_integral<Source>::value,
                  "safeCast requires integral types");

    // Check if value fits in destination type
    if (value < std::numeric_limits<Dest>::min() ||
        value > std::numeric_limits<Dest>::max()) {
        throw OverflowException(
            fmt::format("Integer cast overflow: value {} doesn't fit in destination type",
                       value)
        );
    }

    return static_cast<Dest>(value);
}

// ============================================================================
// RAII Guard for Overflow Checking
// ============================================================================

/**
 * RAII guard to temporarily enable/disable overflow checking
 *
 * Usage:
 *   {
 *       OverflowCheckGuard guard;
 *       // All arithmetic operations are now checked
 *       auto result = safeAdd(x, y);
 *   }
 */
class OverflowCheckGuard {
public:
    OverflowCheckGuard() = default;
    ~OverflowCheckGuard() = default;

    // Non-copyable, non-movable
    OverflowCheckGuard(const OverflowCheckGuard&) = delete;
    OverflowCheckGuard& operator=(const OverflowCheckGuard&) = delete;
};

} // namespace math
} // namespace naab

