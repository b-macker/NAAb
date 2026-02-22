#pragma once

#include <string>

namespace naab {
namespace error {

/**
 * ErrorCategory: Classification of errors for better reporting (Phase 4.1.3)
 *
 * Each category has associated error codes and specific message templates
 */
enum class ErrorCategory {
    TypeError,      // E001-E099: Type mismatches, conversions
    RuntimeError,   // E100-E199: Division by zero, null access, bounds
    ImportError,    // E200-E299: Module not found, circular imports
    SyntaxError,    // E300-E399: Parse errors, malformed code
    NameError,      // E400-E499: Undefined variables, functions
    ValueError      // E500-E599: Invalid values, out of range
};

/**
 * Get error code prefix for a category
 */
inline std::string getErrorCodePrefix(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::TypeError: return "E0";
        case ErrorCategory::RuntimeError: return "E1";
        case ErrorCategory::ImportError: return "E2";
        case ErrorCategory::SyntaxError: return "E3";
        case ErrorCategory::NameError: return "E4";
        case ErrorCategory::ValueError: return "E5";
    }
    return "E?";
}

/**
 * Get category name as string
 */
inline std::string getCategoryName(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::TypeError: return "TypeError";
        case ErrorCategory::RuntimeError: return "RuntimeError";
        case ErrorCategory::ImportError: return "ImportError";
        case ErrorCategory::SyntaxError: return "SyntaxError";
        case ErrorCategory::NameError: return "NameError";
        case ErrorCategory::ValueError: return "ValueError";
    }
    return "UnknownError";
}

// ============================================================================
// Specific Error Code Constants (Phase 4.1.3)
// ============================================================================

// TypeError codes (E001-E099)
constexpr int E001_TYPE_MISMATCH = 1;
constexpr int E002_CANNOT_CONVERT = 2;
constexpr int E003_OPERATOR_TYPE_ERROR = 3;

// RuntimeError codes (E100-E199)
constexpr int E100_DIVISION_BY_ZERO = 100;
constexpr int E101_NULL_POINTER = 101;
constexpr int E102_INDEX_OUT_OF_BOUNDS = 102;

// ImportError codes (E200-E299)
constexpr int E200_MODULE_NOT_FOUND = 200;
constexpr int E201_CIRCULAR_IMPORT = 201;
constexpr int E202_INVALID_IMPORT_PATH = 202;

// SyntaxError codes (E300-E399)
constexpr int E300_UNEXPECTED_TOKEN = 300;
constexpr int E301_MISSING_DELIMITER = 301;
constexpr int E302_INVALID_SYNTAX = 302;

// NameError codes (E400-E499)
constexpr int E400_UNDEFINED_VARIABLE = 400;
constexpr int E401_UNDEFINED_FUNCTION = 401;
constexpr int E402_UNDEFINED_MODULE = 402;

// ValueError codes (E500-E599)
constexpr int E500_INVALID_VALUE = 500;
constexpr int E501_OUT_OF_RANGE = 501;
constexpr int E502_INVALID_ARGUMENT = 502;

// ============================================================================
// Error Message Templates (Phase 4.1.3 - Tasks 4.1.16-4.1.18)
// ============================================================================

/**
 * Format a type error message
 */
inline std::string formatTypeError(const std::string& expected, const std::string& actual) {
    return "Expected type '" + expected + "' but got '" + actual + "'";
}

inline std::string formatConversionError(const std::string& from, const std::string& to) {
    return "Cannot convert type '" + from + "' to '" + to + "'";
}

inline std::string formatOperatorTypeError(const std::string& op,
                                           const std::string& left_type,
                                           const std::string& right_type) {
    return "Type mismatch in operator '" + op + "': cannot operate on '" +
           left_type + "' and '" + right_type + "'";
}

/**
 * Format a runtime error message
 */
inline std::string formatDivisionByZero() {
    return "Division by zero";
}

inline std::string formatNullPointerAccess(const std::string& context = "") {
    if (context.empty()) {
        return "Null pointer access";
    }
    return "Null pointer access in " + context;
}

inline std::string formatIndexOutOfBounds(size_t index, size_t size) {
    return "Index " + std::to_string(index) + " out of bounds for container of size " +
           std::to_string(size);
}

/**
 * Format an import error message
 */
inline std::string formatModuleNotFound(const std::string& module_name) {
    return "Module '" + module_name + "' not found";
}

inline std::string formatCircularImport(const std::string& module_name) {
    return "Circular import detected: '" + module_name + "'";
}

inline std::string formatInvalidImportPath(const std::string& path) {
    return "Invalid import path: '" + path + "'";
}

} // namespace error
} // namespace naab

