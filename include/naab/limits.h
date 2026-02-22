#pragma once
// NAAb Language Security Limits
// Week 1, Task 1.2: Input Size Caps
// Prevents DoS attacks via unbounded inputs


#include <stdexcept>
#include <string>
#include <cstddef>

namespace naab {
namespace limits {

// ============================================================================
// Input Size Limits (configurable via environment variables)
// ============================================================================

// Maximum file size that can be read (10MB default)
constexpr size_t MAX_FILE_SIZE = 10 * 1024 * 1024;

// Maximum polyglot block size (1MB default)
constexpr size_t MAX_POLYGLOT_BLOCK_SIZE = 1 * 1024 * 1024;

// Maximum line length (10k chars)
constexpr size_t MAX_LINE_LENGTH = 10000;

// Maximum input string size (100MB default)
constexpr size_t MAX_INPUT_STRING = 100 * 1024 * 1024;

// ============================================================================
// Parse Tree Limits
// ============================================================================

// Maximum parse depth (prevents stack overflow)
constexpr size_t MAX_PARSE_DEPTH = 1000;

// Maximum AST nodes (prevents memory exhaustion)
constexpr size_t MAX_AST_NODES = 1000000;

// Maximum call stack depth (interpreter)
constexpr size_t MAX_CALL_STACK_DEPTH = 10000;

// ============================================================================
// Collection Limits
// ============================================================================

// Maximum array/list size
constexpr size_t MAX_ARRAY_SIZE = 10000000;  // 10 million elements

// Maximum dictionary size
constexpr size_t MAX_DICT_SIZE = 1000000;  // 1 million entries

// Maximum string length
constexpr size_t MAX_STRING_LENGTH = 100 * 1024 * 1024;  // 100MB

// ============================================================================
// Exception Types
// ============================================================================

class InputSizeException : public std::runtime_error {
public:
    explicit InputSizeException(const std::string& msg)
        : std::runtime_error(msg) {}
};

class RecursionLimitException : public std::runtime_error {
public:
    explicit RecursionLimitException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ============================================================================
// Validation Functions
// ============================================================================

// Check file size before reading
inline void checkFileSize(size_t size, const std::string& filename) {
    if (size > MAX_FILE_SIZE) {
        throw InputSizeException(
            "File '" + filename + "' exceeds maximum size: " +
            std::to_string(size) + " > " + std::to_string(MAX_FILE_SIZE) + " bytes"
        );
    }
}

// Check string size
inline void checkStringSize(size_t size, const std::string& context) {
    if (size > MAX_INPUT_STRING) {
        throw InputSizeException(
            context + " exceeds maximum size: " +
            std::to_string(size) + " > " + std::to_string(MAX_INPUT_STRING) + " bytes"
        );
    }
}

// Check polyglot block size
inline void checkPolyglotBlockSize(size_t size, const std::string& language) {
    if (size > MAX_POLYGLOT_BLOCK_SIZE) {
        throw InputSizeException(
            "Polyglot block (" + language + ") exceeds maximum size: " +
            std::to_string(size) + " > " + std::to_string(MAX_POLYGLOT_BLOCK_SIZE) + " bytes"
        );
    }
}

// Check line length
inline void checkLineLength(size_t length, size_t line_number) {
    if (length > MAX_LINE_LENGTH) {
        throw InputSizeException(
            "Line " + std::to_string(line_number) + " exceeds maximum length: " +
            std::to_string(length) + " > " + std::to_string(MAX_LINE_LENGTH) + " characters"
        );
    }
}

// Check array size
inline void checkArraySize(size_t size) {
    if (size > MAX_ARRAY_SIZE) {
        throw InputSizeException(
            "Array size exceeds maximum: " +
            std::to_string(size) + " > " + std::to_string(MAX_ARRAY_SIZE) + " elements"
        );
    }
}

// Check dictionary size
inline void checkDictSize(size_t size) {
    if (size > MAX_DICT_SIZE) {
        throw InputSizeException(
            "Dictionary size exceeds maximum: " +
            std::to_string(size) + " > " + std::to_string(MAX_DICT_SIZE) + " entries"
        );
    }
}

} // namespace limits
} // namespace naab

