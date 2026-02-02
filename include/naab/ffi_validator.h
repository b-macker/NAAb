// NAAb FFI Input Validator
// Week 4, Task 4.1: FFI Input Validation
//
// Validates all inputs and outputs at FFI boundaries to prevent:
// - Type confusion
// - Buffer overflows
// - Invalid data crossing language boundaries
// - Memory safety violations

#ifndef NAAB_FFI_VALIDATOR_H
#define NAAB_FFI_VALIDATOR_H

#include "naab/interpreter.h"
#include "naab/limits.h"
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

namespace naab {
namespace ffi {

// ============================================================================
// FFI Validation Exceptions
// ============================================================================

class FFIValidationException : public std::runtime_error {
public:
    explicit FFIValidationException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ============================================================================
// FFI Validator
// ============================================================================

class FFIValidator {
public:
    // ========================================================================
    // Input Validation (NAAb → Foreign Language)
    // ========================================================================

    /**
     * Validate arguments before passing to polyglot executor
     *
     * Checks:
     * - Argument count within limits
     * - String sizes within bounds
     * - Collection sizes within bounds
     * - No null/uninitialized values (unless allowed)
     * - Type consistency
     */
    static void validateArguments(
        const std::vector<std::shared_ptr<interpreter::Value>>& args,
        const std::string& language
    );

    /**
     * Validate a single value before FFI crossing
     */
    static void validateValue(
        const std::shared_ptr<interpreter::Value>& value,
        const std::string& context
    );

    /**
     * Validate string content before FFI crossing
     *
     * Checks:
     * - Size limits
     * - No null bytes (for C-style strings)
     * - Valid UTF-8 encoding (optional)
     */
    static void validateString(
        const std::string& str,
        const std::string& context,
        bool allow_null_bytes = false
    );

    /**
     * Validate collection (list/dict) before FFI crossing
     *
     * Checks:
     * - Size limits
     * - Recursion depth (for nested collections)
     * - All elements are valid
     */
    static void validateCollection(
        const std::shared_ptr<interpreter::Value>& value,
        const std::string& context,
        size_t depth = 0
    );

    // ========================================================================
    // Output Validation (Foreign Language → NAAb)
    // ========================================================================

    /**
     * Validate return value from polyglot executor
     *
     * Checks:
     * - Value is not corrupted
     * - Size limits respected
     * - Valid type
     * - No malicious payloads
     */
    static std::shared_ptr<interpreter::Value> validateReturnValue(
        const std::shared_ptr<interpreter::Value>& value,
        const std::string& language
    );

    // ========================================================================
    // Type Checking
    // ========================================================================

    /**
     * Check if value type is safe for FFI crossing
     */
    static bool isSafeType(const std::shared_ptr<interpreter::Value>& value);

    /**
     * Validate numeric value for overflow/underflow
     */
    static void validateNumeric(
        const std::shared_ptr<interpreter::Value>& value,
        const std::string& context
    );

    // ========================================================================
    // Size Checking
    // ========================================================================

    /**
     * Check total size of value (including nested structures)
     * Prevents memory exhaustion attacks
     */
    static size_t calculateTotalSize(
        const std::shared_ptr<interpreter::Value>& value,
        size_t depth = 0
    );

    /**
     * Check if total size is within FFI limits
     */
    static void checkTotalSize(
        const std::shared_ptr<interpreter::Value>& value,
        const std::string& context
    );

private:
    // Maximum collection depth for FFI
    static constexpr size_t MAX_FFI_DEPTH = 100;

    // Maximum total payload size (10MB)
    static constexpr size_t MAX_FFI_PAYLOAD_SIZE = 10 * 1024 * 1024;
};

// ============================================================================
// RAII Guard for FFI Validation
// ============================================================================

class FFIValidationGuard {
public:
    FFIValidationGuard(
        const std::vector<std::shared_ptr<interpreter::Value>>& args,
        const std::string& language
    ) : language_(language) {
        FFIValidator::validateArguments(args, language);
    }

    std::shared_ptr<interpreter::Value> validateReturn(
        const std::shared_ptr<interpreter::Value>& value
    ) {
        return FFIValidator::validateReturnValue(value, language_);
    }

private:
    std::string language_;
};

} // namespace ffi
} // namespace naab

#endif // NAAB_FFI_VALIDATOR_H
