// NAAb FFI Input Validator Implementation
// Week 4, Task 4.1: FFI Input Validation

#include "naab/ffi_validator.h"
#include <fmt/core.h>
#include <cmath>
#include <algorithm>

namespace naab {
namespace ffi {

// ============================================================================
// Argument Validation
// ============================================================================

void FFIValidator::validateArguments(
    const std::vector<std::shared_ptr<interpreter::Value>>& args,
    const std::string& language
) {
    // Check argument count
    if (args.size() > 1000) {
        throw FFIValidationException(
            fmt::format("Too many FFI arguments for {}: {} > 1000",
                       language, args.size())
        );
    }

    // Validate each argument
    for (size_t i = 0; i < args.size(); i++) {
        std::string context = fmt::format("{}[arg {}]", language, i);
        validateValue(args[i], context);
    }
}

// ============================================================================
// Value Validation
// ============================================================================

void FFIValidator::validateValue(
    const std::shared_ptr<interpreter::Value>& value,
    const std::string& context
) {
    if (!value) {
        throw FFIValidationException(
            fmt::format("{}: null value not allowed", context)
        );
    }

    // Check if type is safe for FFI
    if (!isSafeType(value)) {
        throw FFIValidationException(
            fmt::format("{}: unsafe type for FFI crossing", context)
        );
    }

    // Type-specific validation
    if (value->isString()) {
        validateString(value->asString(), context);
    }
    else if (value->isList() || value->isDict()) {
        validateCollection(value, context);
    }
    else if (value->isInt() || value->isFloat()) {
        validateNumeric(value, context);
    }

    // Check total size
    checkTotalSize(value, context);
}

// ============================================================================
// String Validation
// ============================================================================

void FFIValidator::validateString(
    const std::string& str,
    const std::string& context,
    bool allow_null_bytes
) {
    // Check size limit
    if (str.size() > limits::MAX_STRING_LENGTH) {
        throw FFIValidationException(
            fmt::format("{}: string too long: {} > {} bytes",
                       context, str.size(), limits::MAX_STRING_LENGTH)
        );
    }

    // Check for null bytes (can cause issues in C-style APIs)
    if (!allow_null_bytes) {
        if (str.find('\0') != std::string::npos) {
            throw FFIValidationException(
                fmt::format("{}: string contains null bytes", context)
            );
        }
    }

    // Optional: Validate UTF-8 encoding
    // (Can be added if needed for strict validation)
}

// ============================================================================
// Collection Validation
// ============================================================================

void FFIValidator::validateCollection(
    const std::shared_ptr<interpreter::Value>& value,
    const std::string& context,
    size_t depth
) {
    // Check recursion depth
    if (depth > MAX_FFI_DEPTH) {
        throw FFIValidationException(
            fmt::format("{}: collection nesting too deep: {} > {}",
                       context, depth, MAX_FFI_DEPTH)
        );
    }

    // Validate list
    if (value->isList()) {
        const auto& list = value->asList();

        // Check size limit
        limits::checkArraySize(list.size());

        // Validate each element
        for (size_t i = 0; i < list.size(); i++) {
            std::string elem_context = fmt::format("{}[{}]", context, i);
            validateValue(list[i], elem_context);

            // Recurse for nested collections
            if (list[i]->isList() || list[i]->isDict()) {
                validateCollection(list[i], elem_context, depth + 1);
            }
        }
    }

    // Validate dictionary
    if (value->isDict()) {
        const auto& dict = value->asDict();

        // Check size limit
        limits::checkDictSize(dict.size());

        // Validate each key-value pair
        for (const auto& [key, val] : dict) {
            // Validate key
            validateString(key, fmt::format("{}[key]", context));

            // Validate value
            std::string val_context = fmt::format("{}[\"{}\"]", context, key);
            validateValue(val, val_context);

            // Recurse for nested collections
            if (val->isList() || val->isDict()) {
                validateCollection(val, val_context, depth + 1);
            }
        }
    }
}

// ============================================================================
// Return Value Validation
// ============================================================================

std::shared_ptr<interpreter::Value> FFIValidator::validateReturnValue(
    const std::shared_ptr<interpreter::Value>& value,
    const std::string& language
) {
    if (!value) {
        // Null return is allowed (represents None/null/undefined)
        return value;
    }

    std::string context = fmt::format("{}[return]", language);
    validateValue(value, context);

    return value;
}

// ============================================================================
// Type Checking
// ============================================================================

bool FFIValidator::isSafeType(const std::shared_ptr<interpreter::Value>& value) {
    // All basic types are safe
    if (value->isInt() || value->isFloat() || value->isString() ||
        value->isBool() || value->isNull()) {
        return true;
    }

    // Lists and dictionaries are safe (with depth checking)
    if (value->isList() || value->isDict()) {
        return true;
    }

    // Block values, function values, etc. may not be safely serializable
    // across language boundaries
    return false;
}

void FFIValidator::validateNumeric(
    const std::shared_ptr<interpreter::Value>& value,
    const std::string& context
) {
    if (value->isFloat()) {
        double d = value->asFloat();

        // Check for NaN
        if (std::isnan(d)) {
            throw FFIValidationException(
                fmt::format("{}: NaN not allowed in FFI", context)
            );
        }

        // Check for infinity
        if (std::isinf(d)) {
            throw FFIValidationException(
                fmt::format("{}: Infinity not allowed in FFI", context)
            );
        }
    }

    // Integer overflow is checked by Value class itself
}

// ============================================================================
// Size Checking
// ============================================================================

size_t FFIValidator::calculateTotalSize(
    const std::shared_ptr<interpreter::Value>& value,
    size_t depth
) {
    if (!value) {
        return 0;
    }

    // Prevent infinite recursion
    if (depth > MAX_FFI_DEPTH) {
        return 0;
    }

    size_t total = 0;

    // Base size for the value object
    total += sizeof(interpreter::Value);

    // Type-specific sizes
    if (value->isString()) {
        total += value->asString().size();
    }
    else if (value->isList()) {
        const auto& list = value->asList();
        total += list.size() * sizeof(std::shared_ptr<interpreter::Value>);

        // Recurse for nested values
        for (const auto& elem : list) {
            total += calculateTotalSize(elem, depth + 1);
        }
    }
    else if (value->isDict()) {
        const auto& dict = value->asDict();
        total += dict.size() * sizeof(std::pair<std::string, std::shared_ptr<interpreter::Value>>);

        // Recurse for nested values
        for (const auto& [key, val] : dict) {
            total += key.size();
            total += calculateTotalSize(val, depth + 1);
        }
    }

    return total;
}

void FFIValidator::checkTotalSize(
    const std::shared_ptr<interpreter::Value>& value,
    const std::string& context
) {
    size_t total_size = calculateTotalSize(value);

    if (total_size > MAX_FFI_PAYLOAD_SIZE) {
        throw FFIValidationException(
            fmt::format("{}: total payload too large: {} > {} bytes",
                       context, total_size, MAX_FFI_PAYLOAD_SIZE)
        );
    }
}

} // namespace ffi
} // namespace naab
